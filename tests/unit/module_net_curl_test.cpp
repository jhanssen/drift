#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <wasm.h>
#include <wasmtime.h>

#include "core/Module.h"
#include "core/Nodes.h"
#include "platform/ModuleNetCurl.h"
#include "platform/ModuleWasmtime.h"
#include "platform/linux/WsCodec.h"

// The §4.4 native network backend, end to end: the real curl-multi
// thread against in-process loopback servers — whole-response HTTP
// (GET/POST/status), manual redirects with per-hop allowlist checks,
// the mid-flight response cap, cancellation, and the WebSocket path
// (open wake, text/binary echo, server close) via a WsCodec echo
// server. 127.0.0.1 is a valid plain-http/ws origin (the dev
// carve-out), which also exercises the post-DNS loopback exemption.

using namespace drift::core;

namespace {

bool waitFor(const std::function<bool()>& pred, int ms = 8000)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

void sendAll(int fd, const void* data, size_t len)
{
    const char* p = (const char*)data;
    while (len) {
        const ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            return; // peer gone (e.g. the client aborted at the cap)
        }
        p += n;
        len -= (size_t)n;
    }
}

void sendAll(int fd, const std::string& s)
{
    sendAll(fd, s.data(), s.size());
}

// Reads until buf contains delim or the peer closes; returns false on
// close-before-delim.
bool readUntil(int fd, const std::string& delim, std::string& buf)
{
    char tmp[4096];
    while (buf.find(delim) == std::string::npos) {
        const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            return false;
        }
        buf.append(tmp, (size_t)n);
    }
    return true;
}

struct Listener {
    int fd = -1;
    uint16_t port = 0;
    std::thread acceptor;
    std::vector<std::thread> workers;

    bool start(std::function<void(int)> handler)
    {
        fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return false;
        }
        const int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0 ||
            listen(fd, 8) != 0) {
            return false;
        }
        socklen_t len = sizeof(addr);
        getsockname(fd, (sockaddr*)&addr, &len);
        port = ntohs(addr.sin_port);
        acceptor = std::thread([this, handler = std::move(handler)] {
            for (;;) {
                const int c = accept(fd, nullptr, nullptr);
                if (c < 0) {
                    return;
                }
                workers.emplace_back([handler, c] {
                    handler(c);
                    close(c);
                });
            }
        });
        return true;
    }

    ~Listener()
    {
        if (fd >= 0) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
        if (acceptor.joinable()) {
            acceptor.join();
        }
        for (std::thread& w : workers) {
            w.join();
        }
    }
};

// One-connection-per-request HTTP server with the routes the tests use.
void httpHandler(int c)
{
    std::string req;
    if (!readUntil(c, "\r\n\r\n", req)) {
        return;
    }
    const size_t lineEnd = req.find("\r\n");
    const std::string line = req.substr(0, lineEnd);
    const size_t sp1 = line.find(' ');
    const size_t sp2 = line.find(' ', sp1 + 1);
    const std::string path = line.substr(sp1 + 1, sp2 - sp1 - 1);

    size_t bodyLen = 0;
    {
        // curl sends "Content-Length"; keep the scan simple.
        const size_t cl = req.find("Content-Length: ");
        if (cl != std::string::npos) {
            bodyLen = strtoul(req.c_str() + cl + 16, nullptr, 10);
        }
    }
    std::string body = req.substr(req.find("\r\n\r\n") + 4);
    while (body.size() < bodyLen) {
        char tmp[4096];
        const ssize_t n = ::recv(c, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            return;
        }
        body.append(tmp, (size_t)n);
    }

    const auto respond = [c](const char* status, const std::string& payload) {
        std::string out = "HTTP/1.1 ";
        out += status;
        out += "\r\nContent-Length: " + std::to_string(payload.size()) +
               "\r\nConnection: close\r\n\r\n";
        out += payload;
        sendAll(c, out);
    };
    const auto redirect = [c](const char* status, const std::string& to) {
        sendAll(c, "HTTP/1.1 " + std::string(status) + "\r\nLocation: " + to +
                       "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    };

    if (path == "/data") {
        respond("200 OK", "hello from drift");
    } else if (path == "/echo") {
        respond("200 OK", body);
    } else if (path == "/reqheaders") {
        // Reflects the request headers the §4.4 parity rule cares about:
        // a module POST must arrive header-bare on both targets.
        const auto header = [&req](const std::string& name) {
            const size_t at = req.find("\r\n" + name + ": ");
            if (at == std::string::npos) {
                return std::string("-");
            }
            const size_t begin = at + name.size() + 4;
            return req.substr(begin, req.find("\r\n", begin) - begin);
        };
        respond("200 OK",
                "ct=" + header("Content-Type") + " exp=" + header("Expect"));
    } else if (path == "/missing") {
        respond("404 Not Found", "nope");
    } else if (path == "/redir") {
        redirect("302 Found", "/data"); // relative: resolution is curl's
    } else if (path == "/hop") {
        // A different port is a different origin — never in the
        // allowlist, so the hop must die without being dialed.
        redirect("302 Found", "http://127.0.0.1:1/secret");
    } else if (path == "/loop") {
        redirect("302 Found", "/loop");
    } else if (path == "/big") {
        // No Content-Length: the honest-header early reject cannot fire,
        // so the mid-flight write-callback cap must.
        sendAll(c, std::string("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"));
        const std::string chunk(65536, 'x');
        for (int i = 0; i < 9 * 16; ++i) { // 9 MiB > the 8 MiB cap
            sendAll(c, chunk);
        }
    } else if (path == "/slow") {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        respond("200 OK", "late");
    } else {
        respond("404 Not Found", "");
    }
}

// WebSocket echo server on the control endpoint's WsCodec: upgrade,
// echo text/binary frames, close on "bye" or a client CLOSE.
void wsHandler(int c)
{
    namespace ws = drift::platform::ws;
    std::string req;
    if (!readUntil(c, "\r\n\r\n", req)) {
        return;
    }
    const size_t keyAt = req.find("Sec-WebSocket-Key: ");
    if (keyAt == std::string::npos) {
        return;
    }
    const size_t keyEnd = req.find("\r\n", keyAt);
    const std::string key = req.substr(keyAt + 19, keyEnd - keyAt - 19);
    sendAll(c, "HTTP/1.1 101 Switching Protocols\r\n"
               "Upgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Accept: " +
                   ws::acceptKey(key) + "\r\n\r\n");

    std::string buf = req.substr(req.find("\r\n\r\n") + 4);
    for (;;) {
        ws::Frame frame;
        const ptrdiff_t used = ws::decodeFrame(buf, frame);
        if (used < 0) {
            return;
        }
        if (used == 0) {
            char tmp[4096];
            const ssize_t n = ::recv(c, tmp, sizeof(tmp), 0);
            if (n <= 0) {
                return;
            }
            buf.append(tmp, (size_t)n);
            continue;
        }
        buf.erase(0, (size_t)used);
        switch (frame.opcode) {
        case ws::Opcode::Text:
        case ws::Opcode::Binary:
            if (frame.payload == "bye") {
                sendAll(c, ws::encodeFrame(ws::Opcode::Close, {}));
                return;
            }
            sendAll(c, ws::encodeFrame(frame.opcode, frame.payload));
            break;
        case ws::Opcode::Ping:
            sendAll(c, ws::encodeFrame(ws::Opcode::Pong, frame.payload));
            break;
        case ws::Opcode::Close:
            sendAll(c, ws::encodeFrame(ws::Opcode::Close, frame.payload));
            return;
        default:
            break;
        }
    }
}

int32_t httpGet(ModuleNet& net, const std::string& url)
{
    return net.httpRequest((const uint8_t*)url.data(), (uint32_t)url.size(),
                           nullptr, 0, 0);
}

bool settled(ModuleNet& net, int32_t h)
{
    return net.stat(h, 0) != kModuleNetStateConnecting;
}

} // namespace

TEST_CASE("curl backend: address classifier (§4.4 post-DNS blocking)")
{
    using drift::platform::isBlockedAddress;
    // Loopback: only the explicit local carve-out may pass.
    CHECK(isBlockedAddress("127.0.0.1", false));
    CHECK(!isBlockedAddress("127.0.0.1", true));
    CHECK(isBlockedAddress("::1", false));
    CHECK(!isBlockedAddress("::1", true));
    // Private/link-local/CGNAT/multicast: blocked regardless.
    for (const char* ip :
         { "10.1.2.3", "192.168.0.10", "172.16.0.1", "172.31.255.255",
           "169.254.1.1", "100.64.0.1", "0.0.0.0", "224.0.0.1",
           "255.255.255.255", "fe80::1", "fc00::1", "fd12::34", "::",
           "ff02::1", "::ffff:10.0.0.1", "::ffff:192.168.1.1",
           // NAT64 (64:ff9b::/96) embedding private space, the RFC 8215
           // local-use prefix, and v4-compatible embeddings.
           "64:ff9b::192.168.1.1", "64:ff9b::10.0.0.1", "64:ff9b:1::1",
           "::10.0.0.1" }) {
        CAPTURE(ip);
        CHECK(isBlockedAddress(ip, true));
    }
    // Public space stays dialable — including NAT64-embedded public v4.
    for (const char* ip : { "93.184.216.34", "8.8.8.8", "172.32.0.1",
                            "100.128.0.1", "2606:4700::1111",
                            "64:ff9b::8.8.8.8" }) {
        CAPTURE(ip);
        CHECK(!isBlockedAddress(ip, false));
    }
    CHECK(isBlockedAddress("not-an-ip", true));
    CHECK(isBlockedAddress("", true));
}

TEST_CASE("curl backend: HTTP whole-response — GET, POST, status (§4.4)")
{
    Listener srv;
    REQUIRE(srv.start(httpHandler));
    const std::string origin = "http://127.0.0.1:" + std::to_string(srv.port);
    ModuleNet net({ origin }, { origin },
                  drift::platform::createCurlNetBackend());
    net.beginUpdate(0.0);

    const int32_t h = httpGet(net, origin + "/data");
    REQUIRE(h > 0);
    CHECK(!net.wakePending());
    REQUIRE(waitFor([&] { return settled(net, h); }));
    CHECK(net.wakePending()); // the completion wake
    CHECK(net.stat(h, 0) == kModuleNetStateReady);
    CHECK(net.stat(h, 1) == 200);
    char buf[64] = {};
    const int32_t n = net.poll(h, (uint8_t*)buf, sizeof(buf));
    CHECK(std::string(buf, (size_t)n) == "hello from drift");
    // Re-readable until close (the mailbox contract).
    CHECK(net.poll(h, (uint8_t*)buf, sizeof(buf)) == n);
    net.close(h);

    // POST: blen > 0, binary-safe both ways.
    const std::string url = origin + "/echo";
    const std::string body = std::string("bin\0\x01ary", 8);
    const int32_t p =
        net.httpRequest((const uint8_t*)url.data(), (uint32_t)url.size(),
                        (const uint8_t*)body.data(), (uint32_t)body.size(), 0);
    REQUIRE(p > 0);
    REQUIRE(waitFor([&] { return settled(net, p); }));
    CHECK(net.stat(p, 0) == kModuleNetStateReady);
    char ebuf[64] = {};
    const int32_t en = net.poll(p, (uint8_t*)ebuf, sizeof(ebuf));
    CHECK(std::string(ebuf, (size_t)en) == body);
    net.close(p);

    // Browser header parity: fetch with a byte body sends no
    // Content-Type, so the native backend must suppress curl's
    // form-urlencoded default — and its Expect: 100-continue, which
    // curl only adds past ~1 KiB (hence the big body).
    const std::string hurl = origin + "/reqheaders";
    const std::string big(2048, 'b');
    const int32_t hp =
        net.httpRequest((const uint8_t*)hurl.data(), (uint32_t)hurl.size(),
                        (const uint8_t*)big.data(), (uint32_t)big.size(), 0);
    REQUIRE(hp > 0);
    REQUIRE(waitFor([&] { return settled(net, hp); }));
    CHECK(net.stat(hp, 0) == kModuleNetStateReady);
    char hbuf[64] = {};
    const int32_t hn = net.poll(hp, (uint8_t*)hbuf, sizeof(hbuf));
    CHECK(std::string(hbuf, (size_t)hn) == "ct=- exp=-");
    net.close(hp);

    // Non-2xx is still a delivered response, like fetch.
    const int32_t m = httpGet(net, origin + "/missing");
    REQUIRE(waitFor([&] { return settled(net, m); }));
    CHECK(net.stat(m, 0) == kModuleNetStateReady);
    CHECK(net.stat(m, 1) == 404);
    net.close(m);
}

TEST_CASE("curl backend: manual redirects re-check the allowlist (§4.4)")
{
    Listener srv;
    REQUIRE(srv.start(httpHandler));
    const std::string origin = "http://127.0.0.1:" + std::to_string(srv.port);
    ModuleNet net({ origin }, { origin },
                  drift::platform::createCurlNetBackend());
    net.beginUpdate(0.0);

    // Same-origin hop (relative Location): followed to the payload.
    const int32_t ok = httpGet(net, origin + "/redir");
    REQUIRE(waitFor([&] { return settled(net, ok); }));
    CHECK(net.stat(ok, 0) == kModuleNetStateReady);
    CHECK(net.stat(ok, 1) == 200);
    char buf[64] = {};
    const int32_t n = net.poll(ok, (uint8_t*)buf, sizeof(buf));
    CHECK(std::string(buf, (size_t)n) == "hello from drift");
    net.close(ok);

    // Cross-origin hop: the target is never dialed — the offline face.
    const int32_t bad = httpGet(net, origin + "/hop");
    REQUIRE(waitFor([&] { return settled(net, bad); }));
    CHECK(net.stat(bad, 0) == kModuleNetStateFailed);
    CHECK(net.poll(bad, nullptr, 0) == kModuleNetFailed);
    net.close(bad);

    // A redirect loop exhausts the hop budget instead of spinning.
    const int32_t loop = httpGet(net, origin + "/loop");
    REQUIRE(waitFor([&] { return settled(net, loop); }));
    CHECK(net.stat(loop, 0) == kModuleNetStateFailed);
    net.close(loop);
}

TEST_CASE("curl backend: response cap aborts mid-flight (§4.4)")
{
    Listener srv;
    REQUIRE(srv.start(httpHandler));
    const std::string origin = "http://127.0.0.1:" + std::to_string(srv.port);
    ModuleNet net({ origin }, { origin },
                  drift::platform::createCurlNetBackend());
    net.beginUpdate(0.0);

    const int32_t h = httpGet(net, origin + "/big");
    REQUIRE(waitFor([&] { return settled(net, h); }, 20000));
    CHECK(net.stat(h, 0) == kModuleNetStateFailed);
    CHECK(net.stat(h, 1) == 200); // headers arrived; the body was refused
    CHECK(net.poll(h, nullptr, 0) == kModuleNetFailed);
    net.close(h);
}

TEST_CASE("curl backend: cancellation mid-flight, then business as usual")
{
    Listener srv;
    REQUIRE(srv.start(httpHandler));
    const std::string origin = "http://127.0.0.1:" + std::to_string(srv.port);
    ModuleNet net({ origin }, { origin },
                  drift::platform::createCurlNetBackend());
    net.beginUpdate(0.0);

    const int32_t h = httpGet(net, origin + "/slow");
    REQUIRE(h > 0);
    net.close(h); // cancel() contract: no delivery may land after this
    CHECK(net.poll(h, nullptr, 0) == kModuleNetInvalid);

    const int32_t next = httpGet(net, origin + "/data");
    REQUIRE(waitFor([&] { return settled(net, next); }));
    CHECK(net.stat(next, 0) == kModuleNetStateReady);
    net.close(next);
}

TEST_CASE("curl backend: WebSocket — open wake, echo, server close (§4.4)")
{
    Listener srv;
    REQUIRE(srv.start(wsHandler));
    const std::string origin = "ws://127.0.0.1:" + std::to_string(srv.port);
    ModuleNet net({ origin }, { origin },
                  drift::platform::createCurlNetBackend());
    net.beginUpdate(0.0);

    const int32_t h =
        net.wsOpen((const uint8_t*)origin.data(), (uint32_t)origin.size());
    REQUIRE(h > 0);
    REQUIRE(waitFor([&] { return net.stat(h, 0) != kModuleNetStateConnecting; }));
    REQUIRE(net.stat(h, 0) == kModuleNetStateReady); // the open wake

    // Text echo.
    CHECK(net.send(h, (const uint8_t*)"ping", 4, 1) == 0);
    REQUIRE(waitFor([&] { return net.poll(h, nullptr, 0) > 0; }));
    char buf[64] = {};
    int32_t n = net.poll(h, (uint8_t*)buf, sizeof(buf));
    CHECK(std::string(buf, (size_t)n) == "ping");

    // Binary echo (with a NUL) through the same byte-message queue.
    const std::string blob("\x00\x01\x02", 3);
    CHECK(net.send(h, (const uint8_t*)blob.data(), (uint32_t)blob.size(),
                   0) == 0);
    REQUIRE(waitFor([&] { return net.poll(h, nullptr, 0) > 0; }));
    n = net.poll(h, (uint8_t*)buf, sizeof(buf));
    CHECK(std::string(buf, (size_t)n) == blob);

    // Server-initiated close is a lifecycle wake, then sends fail Closed.
    CHECK(net.send(h, (const uint8_t*)"bye", 3, 1) == 0);
    REQUIRE(waitFor([&] { return net.stat(h, 0) == kModuleNetStateClosed; }));
    CHECK(net.send(h, (const uint8_t*)"x", 1, 1) == kModuleNetClosed);
    net.close(h);
}

TEST_CASE("curl backend: the whole native chain — wasmtime imports, curl "
          "thread, external wake (§4.4)")
{
    // The native counterpart of the browser backend's live check: a WAT
    // module issues drift_http_request on its first update, the real
    // curl thread fetches from the loopback server, the completion
    // raises the node's external wake, and the woken update reads the
    // body size and status through drift_net_poll/stat.
    Listener srv;
    REQUIRE(srv.start(httpHandler));
    const std::string origin = "http://127.0.0.1:" + std::to_string(srv.port);
    const std::string url = origin + "/data";

    char wat[2048];
    snprintf(wat, sizeof(wat), R"((module
        (import "env" "drift_http_request"
            (func $req (param i32 i32 i32 i32 i32) (result i32)))
        (import "env" "drift_net_poll"
            (func $poll (param i32 i32 i32) (result i32)))
        (import "env" "drift_net_stat"
            (func $stat (param i32 i32) (result i32)))
        (memory (export "memory") 1)
        (data (i32.const 512) "%s")
        (func (export "drift_abi") (result i32) i32.const 1)
        (func (export "drift_init") (param i32) (result i32) i32.const 1024)
        (func (export "drift_update")
            (if (i32.eqz (i32.load (i32.const 900)))
                (then
                    (i32.store (i32.const 900)
                        (call $req (i32.const 512) (i32.const %zu)
                                   (i32.const 0) (i32.const 0) (i32.const 0)))
                    (f32.store (i32.const 1056) (f32.const -1))
                    (f32.store (i32.const 1060) (f32.const -1)))
                (else
                    (f32.store (i32.const 1056)
                        (f32.convert_i32_s
                            (call $poll (i32.load (i32.const 900))
                                        (i32.const 2048) (i32.const 4096))))
                    (f32.store (i32.const 1060)
                        (f32.convert_i32_s
                            (call $stat (i32.load (i32.const 900))
                                        (i32.const 1)))))))))",
             url.c_str(), url.size());
    wasm_byte_vec_t bytes;
    REQUIRE(wasmtime_wat2wasm(wat, strlen(wat), &bytes) == nullptr);
    const std::string wasm(bytes.data, bytes.size);
    wasm_byte_vec_delete(&bytes);

    ModuleInterface iface;
    std::string err;
    const std::string ifaceJson = R"({
        "abi": 1,
        "permissions": { "network": { "origins": [")" +
                                  origin + R"("] } },
        "outputs": { "size": { "type": "scalar" },
                     "status": { "type": "scalar" } } })";
    REQUIRE_MESSAGE(ModuleInterface::parse(ifaceJson, iface, err), err);
    iface.computeLayout();

    auto net = std::make_unique<ModuleNet>(
        iface.permissions.networkOrigins, iface.permissions.networkOrigins,
        drift::platform::createCurlNetBackend());
    ModuleNet* np = net.get();
    auto loader = drift::platform::wasmtimeModuleLoader();
    auto instance = loader(wasm, iface.ioSize, nullptr, np, err);
    REQUIRE_MESSAGE(instance != nullptr, err);
    ModuleNode node(std::move(instance), iface, iface.permissions, nullptr,
                    std::move(net));

    FrameContext ctx{};
    ctx.seconds = 0.0;
    node.evaluate(ctx); // issues the request
    node.firstEvaluate = false;
    CHECK(node.outputs[0].value.v[0] == -1.0);

    // Between issue and completion the graph quiesces; the delivery
    // raises exactly this node's wake.
    REQUIRE(waitFor([&] { return node.wakePending(1.0); }));
    ctx.seconds = 1.0;
    node.evaluate(ctx);
    CHECK(node.outputs[0].value.v[0] == 16.0); // "hello from drift"
    CHECK(node.outputs[1].value.v[0] == 200.0);
    CHECK(!node.wakePending(2.0)); // consumed
}

TEST_CASE("curl backend: teardown with a live socket does not hang")
{
    Listener srv;
    REQUIRE(srv.start(wsHandler));
    const std::string origin = "ws://127.0.0.1:" + std::to_string(srv.port);
    {
        ModuleNet net({ origin }, { origin },
                      drift::platform::createCurlNetBackend());
        net.beginUpdate(0.0);
        const int32_t h = net.wsOpen((const uint8_t*)origin.data(),
                                     (uint32_t)origin.size());
        REQUIRE(h > 0);
        REQUIRE(waitFor(
            [&] { return net.stat(h, 0) == kModuleNetStateReady; }));
        // ~ModuleNet cancels the open handle; the backend thread joins
        // when the shared_ptr releases. Reaching the next line is the
        // assertion.
    }
    CHECK(true);
}
