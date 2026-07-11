#include "ModuleNetCurl.h"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>

namespace drift::platform {

namespace {

using drift::core::kModuleNetMaxMessage;
using drift::core::kModuleNetMaxResponse;
using drift::core::kModuleNetStateClosed;
using drift::core::kModuleNetStateFailed;
using drift::core::kModuleNetStateReady;
using drift::core::ModuleNet;

constexpr int kMaxRedirects = 5;
constexpr long kConnectTimeoutS = 20;
// HTTP transfers may not dawdle: a stall detector plus a hard ceiling
// bound how long one of the module's 16 handles can sit on the wire.
// Neither applies to an open WebSocket (CURLOPT_TIMEOUT on a
// connect-only handle would also time out later curl_ws_recv calls).
constexpr long kHttpStallSeconds = 30;
constexpr long kHttpHardTimeoutS = 300;
// Outbound WS bound: a full token-bucket burst of maximum messages must
// fit, or a legal send burst against a slow peer would silently drop
// messages the module was told succeeded; past a burst the bucket's
// refill rate is far below any plausible drain rate.
constexpr uint64_t kWsOutboxBytes =
    (uint64_t)drift::core::kModuleNetTokensBurst * kModuleNetMaxMessage;

bool blockedV4(uint32_t host, bool allowLoopback)
{
    const uint32_t octet0 = host >> 24;
    if (octet0 == 127) {
        return !allowLoopback;
    }
    if (octet0 == 0 || octet0 == 10) { // 0.0.0.0/8, 10/8
        return true;
    }
    if ((host & 0xffc00000u) == 0x64400000u) { // 100.64/10 (CGNAT)
        return true;
    }
    if ((host & 0xffff0000u) == 0xa9fe0000u) { // 169.254/16 link-local
        return true;
    }
    if ((host & 0xfff00000u) == 0xac100000u) { // 172.16/12
        return true;
    }
    if ((host & 0xffff0000u) == 0xc0a80000u) { // 192.168/16
        return true;
    }
    if ((host & 0xffffff00u) == 0xc0000000u) { // 192.0.0.0/24
        return true;
    }
    return octet0 >= 224; // multicast, reserved, broadcast
}

} // namespace

bool isBlockedAddress(const char* ip, bool allowLoopback)
{
    if (!ip || !*ip) {
        return true;
    }
    in_addr a4{};
    if (inet_pton(AF_INET, ip, &a4) == 1) {
        return blockedV4(ntohl(a4.s_addr), allowLoopback);
    }
    in6_addr a6{};
    if (inet_pton(AF_INET6, ip, &a6) == 1) {
        const auto embeddedV4 = [&a6, allowLoopback] {
            uint32_t v4 = 0;
            std::memcpy(&v4, a6.s6_addr + 12, 4);
            return blockedV4(ntohl(v4), allowLoopback);
        };
        if (IN6_IS_ADDR_V4MAPPED(&a6) || IN6_IS_ADDR_V4COMPAT(&a6)) {
            return embeddedV4();
        }
        // NAT64 well-known prefix (64:ff9b::/96): the gateway routes to
        // the embedded IPv4, so classify that.
        static const uint8_t nat64[12] = { 0,    0x64, 0xff, 0x9b, 0, 0,
                                           0,    0,    0,    0,    0, 0 };
        if (std::memcmp(a6.s6_addr, nat64, 12) == 0) {
            return embeddedV4();
        }
        // RFC 8215 local-use NAT64 (64:ff9b:1::/48): mapping unknowable.
        if (a6.s6_addr[0] == 0 && a6.s6_addr[1] == 0x64 &&
            a6.s6_addr[2] == 0xff && a6.s6_addr[3] == 0x9b &&
            a6.s6_addr[4] == 0 && a6.s6_addr[5] == 1) {
            return true;
        }
        if (IN6_IS_ADDR_LOOPBACK(&a6)) {
            return !allowLoopback;
        }
        if (IN6_IS_ADDR_UNSPECIFIED(&a6) || IN6_IS_ADDR_LINKLOCAL(&a6) ||
            IN6_IS_ADDR_MULTICAST(&a6)) {
            return true;
        }
        if ((a6.s6_addr[0] & 0xfe) == 0xfc) { // fc00::/7 unique-local
            return true;
        }
        return false;
    }
    return true; // unparseable: block
}

namespace {

// True when the URL's host is the explicit local dev carve-out the
// origin grammar allows plain http/ws for — only then may the post-DNS
// check accept a loopback peer.
bool urlHostIsLocal(const std::string& url)
{
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return false;
    }
    const size_t begin = scheme + 3;
    const size_t end = url.find_first_of("/?#", begin);
    std::string auth = url.substr(
        begin, end == std::string::npos ? std::string::npos : end - begin);
    if (!auth.empty() && auth[0] == '[') {
        const size_t bracket = auth.find(']');
        return bracket != std::string::npos &&
               auth.substr(0, bracket + 1) == "[::1]";
    }
    const size_t colon = auth.find(':');
    const std::string host =
        colon == std::string::npos ? auth : auth.substr(0, colon);
    return host == "localhost" || host == "127.0.0.1";
}

bool schemeSupported(const std::string& url)
{
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return false;
    }
    const std::string proto = url.substr(0, scheme);
    static const std::set<std::string> supported = [] {
        std::set<std::string> out;
        const curl_version_info_data* v = curl_version_info(CURLVERSION_NOW);
        for (const char* const* p = v->protocols; p && *p; ++p) {
            out.insert(*p);
        }
        return out;
    }();
    return supported.contains(proto);
}

class CurlNetBackend : public drift::core::ModuleNetBackend {
public:
    ~CurlNetBackend() override
    {
        {
            std::lock_guard<std::mutex> lock(mCmdMutex);
            if (!mMulti) {
                return; // never started
            }
            mCmds.push_back(Cmd{ Cmd::Quit, {}, {}, {}, false });
        }
        curl_multi_wakeup(mMulti);
        mThread.join();
        curl_multi_cleanup(mMulti);
    }

    void httpRequest(ModuleNet* net, int32_t handle, const std::string& url,
                     std::string body) override
    {
        enqueue(Cmd{ Cmd::Http, { net, handle }, url, std::move(body), false });
    }

    void wsOpen(ModuleNet* net, int32_t handle,
                const std::string& url) override
    {
        enqueue(Cmd{ Cmd::WsOpen, { net, handle }, url, {}, false });
    }

    void wsSend(ModuleNet* net, int32_t handle, std::string data,
                bool text) override
    {
        enqueue(Cmd{ Cmd::WsSend, { net, handle }, {}, std::move(data), text });
    }

    // Contract: no deliveries for this handle after cancel() returns.
    // Deliveries hold mLiveMutex across the check-and-call, so removing
    // the key here (blocking on any delivery in flight) is the whole
    // guarantee; the queued command only tears the transfer down. The
    // tombstone covers a cancel that outruns the thread: the original
    // command must not register the key back to life.
    void cancel(ModuleNet* net, int32_t handle) override
    {
        {
            // Never started ⇒ no command was ever accepted for any key:
            // nothing to tear down, and cancelling a policy-refused
            // handle must not be what spins the network thread up.
            std::lock_guard<std::mutex> lock(mCmdMutex);
            if (!mMulti) {
                return;
            }
        }
        const Key key{ net, handle };
        {
            std::lock_guard<std::mutex> lock(mLiveMutex);
            if (!mLive.erase(key)) {
                mCancelled.insert(key);
            }
        }
        enqueue(Cmd{ Cmd::Cancel, key, {}, {}, false });
    }

private:
    using Key = std::pair<ModuleNet*, int32_t>;

    struct Cmd {
        enum Type { Http, WsOpen, WsSend, Cancel, Quit } type;
        Key key;
        std::string url;
        std::string data;
        bool text;
    };

    struct Transfer {
        Key key;
        bool ws = false;
        CURL* easy = nullptr;
        bool inMulti = false;
        std::string url;
        bool allowLoopback = false;
        // http
        std::string body; // request body; empty = GET
        curl_slist* headers = nullptr;
        std::string response;
        int redirects = 0;
        bool overflow = false;
        // ws
        bool connected = false;
        curl_socket_t sock = CURL_SOCKET_BAD;
        std::string inbound; // frame-fragment assembly
        bool skipMessage = false; // oversized: swallow to the boundary
        struct Outgoing {
            std::string data;
            size_t off = 0;
            bool text = false;
        };
        std::deque<Outgoing> outbox;
        uint64_t outboxBytes = 0;
        bool wantWrite = false; // a send returned CURLE_AGAIN
    };

    void enqueue(Cmd cmd)
    {
        {
            std::lock_guard<std::mutex> lock(mCmdMutex);
            if (!mMulti) {
                // Lazy start: the wall pays for the network thread only
                // once a module actually dials.
                curl_global_init(CURL_GLOBAL_DEFAULT);
                mMulti = curl_multi_init();
                if (!mMulti) {
                    return; // OOM: the handle just never completes —
                            // the offline face, and no thread to join
                }
                mThread = std::thread([this] { threadMain(); });
            }
            mCmds.push_back(std::move(cmd));
        }
        curl_multi_wakeup(mMulti);
    }

    // ---- delivery surface (curl thread; serialized against cancel) ----

    void deliverHttp(const Key& key, int32_t state, int32_t status,
                     std::string body)
    {
        std::lock_guard<std::mutex> lock(mLiveMutex);
        if (mLive.erase(key)) { // terminal either way
            key.first->deliverHttp(key.second, state, status,
                                   std::move(body));
        }
    }

    void deliverWsState(const Key& key, int32_t state)
    {
        std::lock_guard<std::mutex> lock(mLiveMutex);
        if (state == kModuleNetStateReady ? mLive.contains(key)
                                          : mLive.erase(key) != 0) {
            key.first->deliverWsState(key.second, state);
        }
    }

    void deliverWsMessage(const Key& key, std::string message)
    {
        std::lock_guard<std::mutex> lock(mLiveMutex);
        if (mLive.contains(key)) {
            key.first->deliverWsMessage(key.second, std::move(message));
        }
    }

    // Redirect hops re-ask the issuing ModuleNet (§4.4: every hop
    // re-checks the allowlist). Guarded by liveness: a cancelled
    // handle's net may already be gone.
    bool redirectAllowed(const Key& key, const char* url)
    {
        std::lock_guard<std::mutex> lock(mLiveMutex);
        return mLive.contains(key) &&
               key.first->originAllowed(url, /*ws=*/false);
    }

    // ---- curl thread ----

    static size_t writeCb(char* data, size_t size, size_t nmemb, void* userp)
    {
        auto* t = (Transfer*)userp;
        const size_t n = size * nmemb;
        if (t->response.size() + n > kModuleNetMaxResponse) {
            // Mid-flight cap: abort the transfer, never
            // download-then-discard (§4.4).
            t->overflow = true;
            return 0;
        }
        t->response.append(data, n);
        return n;
    }

    static int prereqCb(void* clientp, char* primaryIp, char* /*localIp*/,
                        int /*primaryPort*/, int /*localPort*/)
    {
        auto* t = (Transfer*)clientp;
        return isBlockedAddress(primaryIp, t->allowLoopback)
                   ? CURL_PREREQFUNC_ABORT
                   : CURL_PREREQFUNC_OK;
    }

    CURL* makeEasy(Transfer* t, const char* protocols)
    {
        CURL* e = curl_easy_init();
        if (!e) {
            return nullptr;
        }
        curl_easy_setopt(e, CURLOPT_URL, t->url.c_str());
        curl_easy_setopt(e, CURLOPT_PRIVATE, t);
        curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(e, CURLOPT_PROTOCOLS_STR, protocols);
        curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(e, CURLOPT_PREREQFUNCTION, prereqCb);
        curl_easy_setopt(e, CURLOPT_PREREQDATA, t);
        curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutS);
        curl_easy_setopt(e, CURLOPT_USERAGENT, "drift");
        // No proxies: honoring http_proxy/https_proxy would make the
        // post-DNS address check vet the proxy's peer instead of the
        // target (the proxy resolves the name, re-opening the
        // DNS-rebinding hole the prereq hook closes).
        curl_easy_setopt(e, CURLOPT_PROXY, "");
        return e;
    }

    void startHttp(Transfer* t)
    {
        t->allowLoopback = urlHostIsLocal(t->url);
        CURL* e = makeEasy(t, "http,https");
        if (!e) {
            deliverHttp(t->key, kModuleNetStateFailed, 0, {});
            destroyTransfer(t);
            return;
        }
        curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, writeCb);
        curl_easy_setopt(e, CURLOPT_WRITEDATA, t);
        curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, kHttpStallSeconds);
        curl_easy_setopt(e, CURLOPT_TIMEOUT, kHttpHardTimeoutS);
        // Early reject on an honest Content-Length; the write callback
        // backstops liars.
        curl_easy_setopt(e, CURLOPT_MAXFILESIZE_LARGE,
                         (curl_off_t)kModuleNetMaxResponse);
        if (!t->body.empty()) {
            curl_easy_setopt(e, CURLOPT_POSTFIELDS, t->body.data());
            curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE_LARGE,
                             (curl_off_t)t->body.size());
            // Header parity with the browser: fetch sends a bare binary
            // body — no Content-Type, no Expect — so suppress curl's
            // defaults (form-urlencoded, 100-continue) rather than show
            // a server two different requests for the same module POST.
            if (!t->headers) {
                t->headers = curl_slist_append(nullptr, "Content-Type:");
                t->headers = curl_slist_append(t->headers, "Expect:");
            }
            curl_easy_setopt(e, CURLOPT_HTTPHEADER, t->headers);
        }
        t->easy = e;
        t->inMulti = true;
        curl_multi_add_handle(mMulti, e);
    }

    void startWs(Transfer* t)
    {
        if (!schemeSupported(t->url)) {
            // libcurl built without WebSocket support: the offline face,
            // with one hint on stderr (the §4.4 floor is curl 8.11).
            static bool warned = false;
            if (!warned) {
                warned = true;
                fprintf(stderr, "drift: libcurl lacks WebSocket support; "
                                "module ws connections read as offline\n");
            }
            deliverWsState(t->key, kModuleNetStateFailed);
            destroyTransfer(t);
            return;
        }
        t->allowLoopback = urlHostIsLocal(t->url);
        CURL* e = makeEasy(t, "ws,wss");
        if (!e) {
            deliverWsState(t->key, kModuleNetStateFailed);
            destroyTransfer(t);
            return;
        }
        // The handshake runs as a transfer; afterwards the socket is
        // ours to poll and curl_ws_recv/send operate on the easy.
        curl_easy_setopt(e, CURLOPT_CONNECT_ONLY, 2L);
        // CONNECTTIMEOUT covers only TCP/TLS; this bounds the upgrade
        // round-trip too (a server that connects but never answers must
        // not pin a handle forever). Disarmed once open — left in
        // place it would time out curl_ws_recv on the long-lived
        // connection.
        curl_easy_setopt(e, CURLOPT_TIMEOUT, kConnectTimeoutS + 10);
        t->easy = e;
        t->inMulti = true;
        curl_multi_add_handle(mMulti, e);
    }

    void destroyTransfer(Transfer* t)
    {
        if (t->easy) {
            if (t->inMulti) {
                curl_multi_remove_handle(mMulti, t->easy);
            }
            curl_easy_cleanup(t->easy);
        }
        if (t->headers) {
            curl_slist_free_all(t->headers);
        }
        mTransfers.erase(t->key);
    }

    void onDone(CURL* easy, CURLcode result)
    {
        Transfer* t = nullptr;
        curl_easy_getinfo(easy, CURLINFO_PRIVATE, (char**)&t);

        if (t->ws) {
            // The easy stays attached to the multi: removing it detaches
            // the connection and curl_ws_recv/send refuse the handle.
            // The multi no longer polls a completed transfer's socket,
            // so the thread polls it via curl_multi_poll's extra fds.
            curl_socket_t sock = CURL_SOCKET_BAD;
            if (result == CURLE_OK) {
                curl_easy_getinfo(easy, CURLINFO_ACTIVESOCKET, &sock);
            }
            if (sock == CURL_SOCKET_BAD) {
                deliverWsState(t->key, kModuleNetStateFailed);
                destroyTransfer(t);
                return;
            }
            curl_easy_setopt(easy, CURLOPT_TIMEOUT, 0L); // handshake only
            t->sock = sock;
            t->connected = true;
            // The open wake: where a module sends its subscribe/hello.
            deliverWsState(t->key, kModuleNetStateReady);
            return;
        }
        curl_multi_remove_handle(mMulti, easy);
        t->inMulti = false;

        long code = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);
        if (result == CURLE_OK &&
            (code == 301 || code == 302 || code == 303 || code == 307 ||
             code == 308)) {
            char* loc = nullptr;
            curl_easy_getinfo(easy, CURLINFO_REDIRECT_URL, &loc);
            if (loc && t->redirects < kMaxRedirects &&
                redirectAllowed(t->key, loc)) {
                ++t->redirects;
                t->url = loc;
                if (code != 307 && code != 308) {
                    t->body.clear(); // rewrite to GET, as browsers do
                }
                t->response.clear();
                curl_easy_cleanup(easy);
                t->easy = nullptr;
                startHttp(t);
                return;
            }
            // Off-allowlist hop, hop budget spent, or no Location: the
            // offline face — a module never follows where it may not go.
            deliverHttp(t->key, kModuleNetStateFailed, 0, {});
            destroyTransfer(t);
            return;
        }
        if (result == CURLE_OK) {
            deliverHttp(t->key, kModuleNetStateReady, (int32_t)code,
                        std::move(t->response));
        } else {
            // Overflow keeps the status (the headers arrived; the body
            // was refused) — the browser backend reports the same. curl
            // enforces MAXFILESIZE against received bytes too, so its
            // verdict usually lands before the write callback's.
            const bool overflow =
                t->overflow || result == CURLE_FILESIZE_EXCEEDED;
            deliverHttp(t->key, kModuleNetStateFailed,
                        overflow ? (int32_t)code : 0, {});
        }
        destroyTransfer(t);
    }

    // Returns false when the transfer was torn down.
    bool flushSends(Transfer* t)
    {
        while (!t->outbox.empty()) {
            Transfer::Outgoing& m = t->outbox.front();
            size_t sent = 0;
            const CURLcode rc = curl_ws_send(
                t->easy, m.data.data() + m.off, m.data.size() - m.off, &sent,
                0, m.text ? CURLWS_TEXT : CURLWS_BINARY);
            m.off += sent;
            t->outboxBytes -= sent;
            // Pop on full consumption regardless of rc — a later retry
            // must never pass zero bytes with data flags (curl would
            // frame a phantom empty message); curl flushes any buffered
            // tail at the head of its next send.
            if (m.off >= m.data.size()) {
                t->outbox.pop_front();
            }
            if (rc == CURLE_AGAIN) {
                // Partially accepted; curl continues the same frame when
                // called again with the remainder.
                t->wantWrite = true;
                return true;
            }
            if (rc != CURLE_OK) {
                deliverWsState(t->key, kModuleNetStateFailed);
                destroyTransfer(t);
                return false;
            }
        }
        t->wantWrite = false;
        return true;
    }

    // Returns false when the transfer was torn down.
    bool recvMessages(Transfer* t)
    {
        char buf[65536];
        for (;;) {
            size_t n = 0;
            const curl_ws_frame* meta = nullptr;
            const CURLcode rc =
                curl_ws_recv(t->easy, buf, sizeof(buf), &n, &meta);
            if (rc == CURLE_AGAIN) {
                return true;
            }
            if (rc != CURLE_OK || !meta) {
                // Abrupt end (no CLOSE frame) reports as an error, like
                // the browser's onerror-beats-onclose rule. A CURLE_OK
                // with no frame metadata has no defined meaning; treat
                // it the same rather than spin on it.
                deliverWsState(t->key, kModuleNetStateFailed);
                destroyTransfer(t);
                return false;
            }
            const int flags = meta->flags;
            if (flags & CURLWS_CLOSE) {
                deliverWsState(t->key, kModuleNetStateClosed);
                destroyTransfer(t);
                return false;
            }
            if (flags & (CURLWS_PING | CURLWS_PONG)) {
                continue; // curl answers pings itself
            }
            if (!t->skipMessage) {
                t->inbound.append(buf, n);
                if (t->inbound.size() > kModuleNetMaxMessage) {
                    // Oversized: swallow the rest of the message; the
                    // core would refuse it anyway (bounded queues).
                    t->skipMessage = true;
                    t->inbound.clear();
                }
            }
            const bool complete =
                meta->bytesleft == 0 && !(flags & CURLWS_CONT);
            if (complete) {
                if (t->skipMessage) {
                    t->skipMessage = false;
                } else {
                    deliverWsMessage(t->key, std::move(t->inbound));
                    t->inbound.clear();
                }
            }
        }
    }

    void runCommand(Cmd& cmd, bool& quit)
    {
        switch (cmd.type) {
        case Cmd::Http:
        case Cmd::WsOpen: {
            {
                std::lock_guard<std::mutex> lock(mLiveMutex);
                if (mCancelled.contains(cmd.key)) {
                    // cancel() outran us: the handle was closed before
                    // the transfer ever started. The Cancel command
                    // still in the queue clears the tombstone.
                    return;
                }
                mLive.insert(cmd.key);
            }
            auto transfer = std::make_unique<Transfer>();
            Transfer* t = transfer.get();
            t->key = cmd.key;
            t->url = std::move(cmd.url);
            mTransfers[cmd.key] = std::move(transfer);
            if (cmd.type == Cmd::WsOpen) {
                t->ws = true;
                startWs(t);
            } else {
                t->body = std::move(cmd.data);
                startHttp(t);
            }
            break;
        }
        case Cmd::WsSend: {
            auto it = mTransfers.find(cmd.key);
            if (it == mTransfers.end() || !it->second->ws) {
                break; // raced a teardown; the send is simply lost
            }
            Transfer* t = it->second.get();
            if (t->outboxBytes + cmd.data.size() > kWsOutboxBytes) {
                break; // bounded outbox: drop (core rate-caps sends)
            }
            t->outboxBytes += cmd.data.size();
            t->outbox.push_back(
                Transfer::Outgoing{ std::move(cmd.data), 0, cmd.text });
            if (t->connected) {
                flushSends(t);
            }
            break;
        }
        case Cmd::Cancel: {
            {
                std::lock_guard<std::mutex> lock(mLiveMutex);
                mCancelled.erase(cmd.key);
            }
            auto it = mTransfers.find(cmd.key);
            if (it == mTransfers.end()) {
                break;
            }
            Transfer* t = it->second.get();
            // Polite best-effort close frame — but only with no partial
            // frame outstanding, or the CLOSE header would splice into
            // its unfinished payload.
            if (t->ws && t->connected && t->outbox.empty() &&
                !t->wantWrite) {
                size_t sent = 0;
                curl_ws_send(t->easy, "", 0, &sent, 0, CURLWS_CLOSE);
            }
            destroyTransfer(t);
            break;
        }
        case Cmd::Quit:
            quit = true;
            break;
        }
    }

    void threadMain()
    {
        std::vector<Cmd> cmds;
        std::vector<Key> connected;
        std::vector<curl_waitfd> extra;
        bool quit = false;
        while (!quit) {
            {
                std::lock_guard<std::mutex> lock(mCmdMutex);
                cmds.swap(mCmds);
            }
            for (Cmd& cmd : cmds) {
                runCommand(cmd, quit);
            }
            cmds.clear();
            if (quit) {
                break;
            }

            int running = 0;
            curl_multi_perform(mMulti, &running);
            int queued = 0;
            while (CURLMsg* msg = curl_multi_info_read(mMulti, &queued)) {
                if (msg->msg == CURLMSG_DONE) {
                    onDone(msg->easy_handle, msg->data.result);
                }
            }

            // Service connected sockets. recv/flush may tear a transfer
            // down, so walk a key snapshot.
            connected.clear();
            for (const auto& [key, t] : mTransfers) {
                if (t->ws && t->connected) {
                    connected.push_back(key);
                }
            }
            for (const Key& key : connected) {
                auto it = mTransfers.find(key);
                if (it == mTransfers.end()) {
                    continue;
                }
                Transfer* t = it->second.get();
                if (!flushSends(t)) {
                    continue;
                }
                recvMessages(t);
            }

            // Sleep on the multi's transfers plus the live WS sockets;
            // commands interrupt via curl_multi_wakeup.
            extra.clear();
            bool active = false; // any transfer curl still drives
            for (const auto& [key, t] : mTransfers) {
                if (t->inMulti && !t->connected) {
                    active = true; // HTTP or a WS handshake in flight
                }
                if (t->ws && t->connected) {
                    curl_waitfd w{};
                    w.fd = t->sock;
                    w.events = CURL_WAIT_POLLIN;
                    if (!t->outbox.empty() || t->wantWrite) {
                        w.events |= CURL_WAIT_POLLOUT;
                    }
                    extra.push_back(w);
                }
            }
            int numfds = 0;
            // In-flight transfers need regular perform ticks for curl's
            // own timeouts; established WebSockets are fd-driven through
            // extra above, so a thread holding only those sleeps like an
            // empty one (the idle wall must stay idle).
            const int timeoutMs = active ? 1000 : 600000;
            curl_multi_poll(mMulti, extra.data(), (unsigned)extra.size(),
                            timeoutMs, &numfds);
        }
        // Quit: only reachable once every owner released the backend,
        // which required cancelling every handle — but be thorough.
        while (!mTransfers.empty()) {
            destroyTransfer(mTransfers.begin()->second.get());
        }
    }

    std::mutex mCmdMutex;
    std::vector<Cmd> mCmds;
    CURLM* mMulti = nullptr;
    std::thread mThread;

    // Liveness registry: deliveries hold mLiveMutex across the
    // check-and-deliver; cancel() removes the key under the same lock.
    // Never taken while holding mCmdMutex or a ModuleNet lock.
    std::mutex mLiveMutex;
    std::set<Key> mLive;
    std::set<Key> mCancelled; // cancelled before the command ran

    // curl-thread-only state.
    std::map<Key, std::unique_ptr<Transfer>> mTransfers;
};

} // namespace

std::shared_ptr<drift::core::ModuleNetBackend> createCurlNetBackend()
{
    return std::make_shared<CurlNetBackend>();
}

} // namespace drift::platform
