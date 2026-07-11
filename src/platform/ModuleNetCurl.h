#pragma once

// §4.4 native network transport: HTTP (whole-response) and WebSockets
// over one lazily-started curl-multi thread — the video-decoder-thread
// precedent. Policy, mailboxes, queues, tokens, and wakes live in the
// core ModuleNet; this backend contributes the wire:
//
//   - HTTP buffers the complete body off-thread and aborts mid-flight
//     as the byte count passes kModuleNetMaxResponse (write-callback
//     error, never download-then-discard).
//   - Redirects are followed manually so every hop re-checks the
//     allowlist (ModuleNet::originAllowed) and re-runs the address
//     check below; 301/302/303 rewrite to GET, 307/308 keep the body.
//   - Private/link-local/loopback ranges are blocked with the check
//     applied post-DNS-resolution (CURLOPT_PREREQFUNCTION — the
//     DNS-rebinding defense); loopback is exempted only when the URL
//     host itself is localhost/127.0.0.1/[::1] (the dev carve-out the
//     origin grammar already gates).
//   - WebSockets use curl's client (8.11+ floor): the handshake runs
//     through the multi as a CONNECT_ONLY=2 transfer, after which the
//     socket joins the thread's poll set and complete messages are
//     assembled from curl_ws_recv frames. Sends are non-blocking
//     enqueues flushed by the thread (rate-capped by the core token
//     bucket); a runtime built against a curl without WebSocket
//     support serves the offline face for ws handles.
//
// cancel() honors the ModuleNetBackend contract: after it returns, no
// further deliveries for that handle — delivery and cancellation
// serialize on one registry mutex, distinct from the command-queue
// mutex so the issue path (which runs under ModuleNet's lock) can
// never deadlock against a delivery taking that same lock.

#include <memory>

#include "core/Module.h"

namespace drift::platform {

// One per process (main.cpp creates it once; every scene's ModuleNets
// share it via shared_ptr). The thread starts on the first request and
// joins when the last owner releases the backend.
std::shared_ptr<drift::core::ModuleNetBackend> createCurlNetBackend();

// Exposed for tests: true when the address (dotted IPv4 or IPv6 text,
// as curl reports the connected peer) is private, link-local, loopback,
// or otherwise non-routable. allowLoopback exempts 127/8 and ::1 (URL
// host was explicitly local).
bool isBlockedAddress(const char* ip, bool allowLoopback);

} // namespace drift::platform
