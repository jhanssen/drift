#include "ControlServer.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <glaze/glaze.hpp>

#include "platform/ParamJson.h"
#include "WsCodec.h"

namespace drift::platform {

namespace {

// Covers the largest legitimate message ("load" carries a whole scene.json
// document); a peer exceeding this is broken or hostile.
constexpr size_t kMaxBuffered = 1024 * 1024;

bool parseValueJson(const glz::generic& j, core::Value& out)
{
    if (j.is_number()) {
        out.type = core::ValueType::Scalar;
        out.v[0] = j.get_number();
        return true;
    }
    if (j.is_array()) {
        const auto& arr = j.get_array();
        if (arr.size() < 2 || arr.size() > 4) {
            return false;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            if (!arr[i].is_number()) {
                return false;
            }
            out.v[i] = arr[i].get_number();
        }
        out.type = arr.size() == 2 ? core::ValueType::Vec2
                 : arr.size() == 3 ? core::ValueType::Vec3
                                   : core::ValueType::Vec4;
        return true;
    }
    return false;
}

// Any web page can attempt ws://127.0.0.1:<port>, so browser connections
// are only accepted from localhost origins. Non-browser clients send no
// Origin header and pass.
bool originAllowed(std::string_view origin)
{
    const size_t scheme = origin.find("://");
    if (scheme == std::string_view::npos) {
        return false; // includes "null" (file:// pages)
    }
    std::string_view host = origin.substr(scheme + 3);
    if (!host.empty() && host[0] == '[') {
        const size_t end = host.find(']');
        if (end == std::string_view::npos) {
            return false;
        }
        host = host.substr(0, end + 1);
    } else {
        host = host.substr(0, host.find_first_of(":/"));
    }
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

std::string lowered(std::string s)
{
    for (char& c : s) {
        c = (char)tolower((unsigned char)c);
    }
    return s;
}

bool containsToken(const std::string& headerValue, const char* token)
{
    // Comma-separated token list, case-insensitive (e.g. Connection:
    // keep-alive, Upgrade).
    const std::string hay = lowered(headerValue);
    size_t pos = 0;
    while (pos < hay.size()) {
        size_t end = hay.find(',', pos);
        if (end == std::string::npos) {
            end = hay.size();
        }
        std::string item = hay.substr(pos, end - pos);
        item.erase(0, item.find_first_not_of(" \t"));
        item.erase(item.find_last_not_of(" \t") + 1);
        if (item == token) {
            return true;
        }
        pos = end + 1;
    }
    return false;
}

} // namespace

ControlServer::~ControlServer()
{
    for (const auto& [fd, client] : mClients) {
        close(fd);
    }
    if (mListen >= 0) {
        close(mListen);
    }
    if (mEpoll >= 0) {
        close(mEpoll);
    }
}

bool ControlServer::start(uint16_t port, Callbacks callbacks, std::string& error)
{
    mCallbacks = std::move(callbacks);

    mListen = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (mListen < 0) {
        error = std::string("socket: ") + strerror(errno);
        return false;
    }
    const int one = 1;
    setsockopt(mListen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // never a public socket
    if (bind(mListen, (const sockaddr*)&addr, sizeof(addr)) != 0) {
        error = std::string("bind 127.0.0.1:") + std::to_string(port) + ": " +
                strerror(errno);
        return false;
    }
    if (listen(mListen, 8) != 0) {
        error = std::string("listen: ") + strerror(errno);
        return false;
    }

    mEpoll = epoll_create1(EPOLL_CLOEXEC);
    if (mEpoll < 0) {
        error = std::string("epoll_create1: ") + strerror(errno);
        return false;
    }
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = mListen;
    epoll_ctl(mEpoll, EPOLL_CTL_ADD, mListen, &ev);
    return true;
}

void ControlServer::drive()
{
    epoll_event events[16];
    for (;;) {
        const int n = epoll_wait(mEpoll, events, 16, 0);
        if (n <= 0) {
            return;
        }
        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            if (fd == mListen) {
                acceptClients();
                continue;
            }
            if (!mClients.count(fd)) {
                continue; // closed earlier in this batch
            }
            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                closeClient(fd);
                continue;
            }
            if (events[i].events & EPOLLIN) {
                readClient(fd);
            }
            if (mClients.count(fd) && (events[i].events & EPOLLOUT)) {
                flush(fd, mClients[fd]);
            }
        }
    }
}

std::string ControlServer::transportJson() const
{
    const double seconds = mCallbacks.time ? mCallbacks.time() : 0.0;
    const bool paused = mCallbacks.paused && mCallbacks.paused();
    return "{\"seconds\":" + numberJson(seconds) +
           ",\"paused\":" + (paused ? "true" : "false") + "}";
}

// Collect fds first: a failed send closes the client and mutates the map.
void ControlServer::broadcast(const std::string& message, int exceptFd)
{
    const std::string frame = ws::encodeFrame(ws::Opcode::Text, message);
    std::vector<int> targets;
    for (const auto& [fd, client] : mClients) {
        if (fd != exceptFd && client.upgraded && !client.closing) {
            targets.push_back(fd);
        }
    }
    for (int target : targets) {
        if (auto it = mClients.find(target); it != mClients.end()) {
            send(target, it->second, frame);
        }
    }
}

void ControlServer::acceptClients()
{
    for (;;) {
        const int fd = accept4(mListen, nullptr, nullptr,
                               SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            return;
        }
        mClients.emplace(fd, Client{});
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        epoll_ctl(mEpoll, EPOLL_CTL_ADD, fd, &ev);
    }
}

void ControlServer::readClient(int fd)
{
    Client& client = mClients[fd];
    char buf[4096];
    for (;;) {
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            client.in.append(buf, (size_t)n);
            if (client.in.size() > kMaxBuffered) {
                closeClient(fd);
                return;
            }
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        closeClient(fd); // peer closed or hard error
        return;
    }

    if (!client.upgraded) {
        if (client.in.find("\r\n\r\n") == std::string::npos) {
            return; // headers incomplete
        }
        if (!handshake(fd, client)) {
            client.closing = true;
            flush(fd, client);
            return;
        }
        client.upgraded = true;
    }
    if (!handleFrames(fd, client)) {
        closeClient(fd);
    }
}

bool ControlServer::handshake(int fd, Client& client)
{
    const size_t headerEnd = client.in.find("\r\n\r\n");
    const std::string head = client.in.substr(0, headerEnd);
    client.in.erase(0, headerEnd + 4);

    std::map<std::string, std::string> headers;
    size_t lineStart = head.find("\r\n");
    const std::string requestLine = head.substr(0, lineStart);
    while (lineStart != std::string::npos && lineStart < head.size()) {
        lineStart += 2;
        size_t lineEnd = head.find("\r\n", lineStart);
        const std::string line = head.substr(
            lineStart, lineEnd == std::string::npos ? std::string::npos
                                                    : lineEnd - lineStart);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string value = line.substr(colon + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            headers[lowered(line.substr(0, colon))] = value;
        }
        lineStart = lineEnd;
    }

    auto fail = [&](const char* status) {
        send(fd, client, std::string("HTTP/1.1 ") + status +
                             "\r\nConnection: close\r\nContent-Length: 0"
                             "\r\n\r\n");
        return false;
    };

    if (requestLine.compare(0, 4, "GET ") != 0) {
        return fail("405 Method Not Allowed");
    }
    if (auto it = headers.find("origin");
        it != headers.end() && !originAllowed(it->second)) {
        return fail("403 Forbidden");
    }
    const auto upgrade = headers.find("upgrade");
    const auto connection = headers.find("connection");
    const auto key = headers.find("sec-websocket-key");
    if (upgrade == headers.end() || lowered(upgrade->second) != "websocket" ||
        connection == headers.end() ||
        !containsToken(connection->second, "upgrade") ||
        key == headers.end() || key->second.empty()) {
        return fail("400 Bad Request");
    }
    if (auto it = headers.find("sec-websocket-version");
        it == headers.end() || it->second != "13") {
        return fail("426 Upgrade Required");
    }

    send(fd, client,
         "HTTP/1.1 101 Switching Protocols\r\n"
         "Upgrade: websocket\r\n"
         "Connection: Upgrade\r\n"
         "Sec-WebSocket-Accept: " + ws::acceptKey(key->second) + "\r\n\r\n");
    return true;
}

bool ControlServer::handleFrames(int fd, Client& client)
{
    for (;;) {
        ws::Frame frame;
        const ptrdiff_t consumed = ws::decodeFrame(client.in, frame);
        if (consumed == 0) {
            return true;
        }
        if (consumed < 0) {
            return false;
        }
        client.in.erase(0, (size_t)consumed);

        switch (frame.opcode) {
        case ws::Opcode::Text:
        case ws::Opcode::Continuation:
            if (frame.opcode == ws::Opcode::Text) {
                client.message = std::move(frame.payload);
            } else {
                client.message += frame.payload;
                if (client.message.size() > kMaxBuffered) {
                    return false;
                }
            }
            if (frame.fin) {
                const std::string text = std::move(client.message);
                client.message.clear();
                handleRequest(fd, client, text);
                if (!mClients.count(fd)) {
                    return true; // closed while responding
                }
            }
            break;
        case ws::Opcode::Binary:
            return false; // text-only protocol
        case ws::Opcode::Ping:
            send(fd, client,
                 ws::encodeFrame(ws::Opcode::Pong, frame.payload));
            if (!mClients.count(fd)) {
                return true; // write error closed it under us
            }
            break;
        case ws::Opcode::Pong:
            break;
        case ws::Opcode::Close:
            send(fd, client, ws::encodeFrame(ws::Opcode::Close, {}));
            client.closing = true;
            flush(fd, client);
            return mClients.count(fd) != 0;
        }
    }
}

void ControlServer::handleRequest(int fd, Client& client, const std::string& text)
{
    std::string idJson; // echoed verbatim when present and numeric
    auto respond = [&](const std::string& body) {
        std::string msg = "{";
        if (!idJson.empty()) {
            msg += "\"id\":" + idJson + ",";
        }
        msg += body + "}";
        send(fd, client, ws::encodeFrame(ws::Opcode::Text, msg));
    };

    glz::generic json{};
    if (glz::read_json(json, text) || !json.is_object()) {
        respond("\"error\":\"malformed JSON request\"");
        return;
    }
    const auto& obj = json.get_object();
    if (auto it = obj.find("id"); it != obj.end() && it->second.is_number()) {
        idJson = numberJson(it->second.get_number());
    }
    const auto methodIt = obj.find("method");
    if (methodIt == obj.end() || !methodIt->second.is_string()) {
        respond("\"error\":\"missing 'method'\"");
        return;
    }
    const std::string& method = methodIt->second.get_string();

    if (method == "describe") {
        const SceneInfo info = mCallbacks.describe();
        respond("\"result\":" +
                describeJson(info.loaded, info.name, info.animated,
                             info.parameters, info.sequences));
        return;
    }

    if (method == "set") {
        const auto paramsIt = obj.find("params");
        if (paramsIt == obj.end() || !paramsIt->second.is_object()) {
            respond("\"error\":\"'set' needs a params object\"");
            return;
        }
        const auto& params = paramsIt->second.get_object();
        const auto nameIt = params.find("name");
        const auto valueIt = params.find("value");
        core::Value value;
        if (nameIt == params.end() || !nameIt->second.is_string() ||
            valueIt == params.end() ||
            !parseValueJson(valueIt->second, value)) {
            respond("\"error\":\"'set' needs a string name and a "
                    "number/vector value\"");
            return;
        }
        const std::string& name = nameIt->second.get_string();
        std::string error;
        core::Value applied = value;
        if (!mCallbacks.setParameter(name, value, error, applied)) {
            respond("\"error\":\"" + jsonEscape(error) + "\"");
            return;
        }
        respond("\"result\":" + std::string("{\"value\":") +
                valueJson(applied) + "}");

        // Everyone else learns about the change (last-write-wins), with the
        // value as applied — clamping may have altered the request.
        broadcast("{\"event\":\"parameter\",\"name\":\"" + jsonEscape(name) +
                      "\",\"value\":" + valueJson(applied) + "}",
                  fd);
        return;
    }

    if (method == "time") {
        respond("\"result\":" + transportJson());
        return;
    }

    if (method == "pause") {
        const auto paramsIt = obj.find("params");
        if (paramsIt == obj.end() || !paramsIt->second.is_object()) {
            respond("\"error\":\"'pause' needs a params object\"");
            return;
        }
        const auto& params = paramsIt->second.get_object();
        const auto pausedIt = params.find("paused");
        if (pausedIt == params.end() || !pausedIt->second.is_boolean()) {
            respond("\"error\":\"'pause' needs a boolean 'paused'\"");
            return;
        }
        mCallbacks.setPaused(pausedIt->second.get_boolean());
        respond("\"result\":" + transportJson());
        broadcast("{\"event\":\"transport\"," + transportJson().substr(1), fd);
        return;
    }

    if (method == "seek") {
        const auto paramsIt = obj.find("params");
        double seconds = -1.0;
        if (paramsIt != obj.end() && paramsIt->second.is_object()) {
            const auto& params = paramsIt->second.get_object();
            if (auto it = params.find("time");
                it != params.end() && it->second.is_number()) {
                seconds = it->second.get_number();
            }
        }
        if (seconds < 0.0) {
            respond("\"error\":\"'seek' needs a non-negative 'time'\"");
            return;
        }
        std::string error;
        if (!mCallbacks.seek(seconds, error)) {
            respond("\"error\":\"" + jsonEscape(error) + "\"");
            return;
        }
        respond("\"result\":" + transportJson());
        broadcast("{\"event\":\"transport\"," + transportJson().substr(1), fd);
        return;
    }

    if (method == "reload") {
        std::string error;
        if (!mCallbacks.reload(error)) {
            respond("\"error\":\"" + jsonEscape(error) + "\"");
            return;
        }
        respond("\"result\":{}");
        broadcast("{\"event\":\"reload\"}", fd);
        return;
    }

    if (method == "fire") {
        std::string node, port;
        if (auto paramsIt = obj.find("params");
            paramsIt != obj.end() && paramsIt->second.is_object()) {
            const auto& params = paramsIt->second.get_object();
            if (auto it = params.find("node");
                it != params.end() && it->second.is_string()) {
                node = it->second.get_string();
            }
            if (auto it = params.find("port");
                it != params.end() && it->second.is_string()) {
                port = it->second.get_string();
            }
        }
        if (node.empty() || port.empty()) {
            respond("\"error\":\"'fire' needs string 'node' and 'port'\"");
            return;
        }
        std::string error;
        if (!mCallbacks.fire(node, port, error)) {
            respond("\"error\":\"" + jsonEscape(error) + "\"");
            return;
        }
        respond("\"result\":{}");
        return;
    }

    if (method == "source") {
        respond("\"result\":{\"scene\":\"" +
                jsonEscape(mCallbacks.source ? mCallbacks.source()
                                             : std::string()) +
                "\"}");
        return;
    }

    if (method == "load") {
        const std::string* sceneJson = nullptr;
        if (auto paramsIt = obj.find("params");
            paramsIt != obj.end() && paramsIt->second.is_object()) {
            const auto& params = paramsIt->second.get_object();
            if (auto it = params.find("scene");
                it != params.end() && it->second.is_string()) {
                sceneJson = &it->second.get_string();
            }
        }
        if (!sceneJson) {
            respond("\"error\":\"'load' needs a string 'scene' document\"");
            return;
        }
        std::string error;
        if (!mCallbacks.load(*sceneJson, error)) {
            respond("\"error\":\"" + jsonEscape(error) + "\"");
            return;
        }
        respond("\"result\":{}");
        broadcast("{\"event\":\"reload\"}", fd);
        return;
    }

    if (method == "write-asset" || method == "read-asset") {
        std::string path;
        const std::string* contents = nullptr;
        if (auto paramsIt = obj.find("params");
            paramsIt != obj.end() && paramsIt->second.is_object()) {
            const auto& params = paramsIt->second.get_object();
            if (auto it = params.find("path");
                it != params.end() && it->second.is_string()) {
                path = it->second.get_string();
            }
            if (auto it = params.find("contents");
                it != params.end() && it->second.is_string()) {
                contents = &it->second.get_string();
            }
        }
        if (path.empty() || (method == "write-asset" && !contents)) {
            respond("\"error\":\"'" + method +
                    "' needs a string 'path'" +
                    (method == "write-asset" ? " and 'contents'" : "") +
                    "\"");
            return;
        }
        std::string error;
        if (method == "write-asset") {
            if (!mCallbacks.writeAsset ||
                !mCallbacks.writeAsset(path, *contents, error)) {
                respond("\"error\":\"" +
                        jsonEscape(error.empty() ? "unsupported" : error) +
                        "\"");
                return;
            }
            respond("\"result\":{}");
            return;
        }
        std::string out;
        if (!mCallbacks.readAsset ||
            !mCallbacks.readAsset(path, out, error)) {
            respond("\"error\":\"" +
                    jsonEscape(error.empty() ? "unsupported" : error) +
                    "\"");
            return;
        }
        respond("\"result\":{\"contents\":\"" + jsonEscape(out) + "\"}");
        return;
    }

    respond("\"error\":\"unknown method '" + jsonEscape(method) + "'\"");
}

void ControlServer::send(int fd, Client& client, std::string bytes)
{
    client.out += bytes;
    flush(fd, client);
}

void ControlServer::flush(int fd, Client& client)
{
    while (!client.out.empty()) {
        const ssize_t n =
            ::send(fd, client.out.data(), client.out.size(), MSG_NOSIGNAL);
        if (n > 0) {
            client.out.erase(0, (size_t)n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            updateEpoll(fd, client);
            return;
        }
        closeClient(fd);
        return;
    }
    if (client.closing) {
        closeClient(fd);
        return;
    }
    updateEpoll(fd, client);
}

void ControlServer::updateEpoll(int fd, const Client& client)
{
    epoll_event ev{};
    ev.events = EPOLLIN | (client.out.empty() ? 0 : EPOLLOUT);
    ev.data.fd = fd;
    epoll_ctl(mEpoll, EPOLL_CTL_MOD, fd, &ev);
}

void ControlServer::closeClient(int fd)
{
    epoll_ctl(mEpoll, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    mClients.erase(fd);
}

} // namespace drift::platform
