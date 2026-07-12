#pragma once

// Minimal RFC 6455 WebSocket wire pieces for the control endpoint: the
// upgrade accept key and frame encode/decode. Pure functions, no I/O —
// unit-tested in tests/unit/ws_codec_test.cpp.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace drift::platform::ws {

// base64(SHA1(clientKey + RFC 6455 GUID)) for Sec-WebSocket-Accept.
std::string acceptKey(std::string_view clientKey);

enum class Opcode : uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xa,
};

struct Frame {
    Opcode opcode = Opcode::Text;
    bool fin = true;
    std::string payload;
};

// Decodes one frame from the head of buf. Returns the bytes consumed,
// 0 when more data is needed, or -1 on protocol error (reserved bits,
// unmasked client frame, oversized payload).
ptrdiff_t decodeFrame(std::string_view buf, Frame& out);

// Encodes a server->client frame (single, unmasked; RFC 6455 §5.1).
std::string encodeFrame(Opcode opcode, std::string_view payload);

} // namespace drift::platform::ws
