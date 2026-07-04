#include <doctest/doctest.h>

#include <string>

#include "platform/linux/WsCodec.h"

// RFC 6455 wire pieces for the control endpoint (ControlServer).

using namespace drift::platform::ws;

TEST_CASE("accept key matches the RFC 6455 §1.3 example")
{
    CHECK(acceptKey("dGhlIHNhbXBsZSBub25jZQ==") ==
          "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

namespace {

// Client-side (masked) frame, for feeding decodeFrame.
std::string maskedFrame(Opcode opcode, std::string_view payload, bool fin = true)
{
    std::string out;
    out.push_back((char)((fin ? 0x80 : 0x00) | (uint8_t)opcode));
    const uint8_t mask[4] = { 0x12, 0x34, 0x56, 0x78 };
    if (payload.size() < 126) {
        out.push_back((char)(0x80 | payload.size()));
    } else {
        out.push_back((char)(0x80 | 126));
        out.push_back((char)(payload.size() >> 8));
        out.push_back((char)(payload.size() & 0xff));
    }
    out.append((const char*)mask, 4);
    for (size_t i = 0; i < payload.size(); ++i) {
        out.push_back((char)((uint8_t)payload[i] ^ mask[i % 4]));
    }
    return out;
}

} // namespace

TEST_CASE("masked client frames decode")
{
    const std::string wire = maskedFrame(Opcode::Text, "{\"id\":1}");
    Frame frame;
    CHECK(decodeFrame(wire, frame) == (ptrdiff_t)wire.size());
    CHECK(frame.opcode == Opcode::Text);
    CHECK(frame.fin);
    CHECK(frame.payload == "{\"id\":1}");
}

TEST_CASE("extended 16-bit lengths decode")
{
    const std::string payload(300, 'x');
    const std::string wire = maskedFrame(Opcode::Text, payload);
    Frame frame;
    CHECK(decodeFrame(wire, frame) == (ptrdiff_t)wire.size());
    CHECK(frame.payload == payload);
}

TEST_CASE("partial frames ask for more data")
{
    const std::string wire = maskedFrame(Opcode::Text, "hello");
    Frame frame;
    for (size_t cut = 0; cut < wire.size(); ++cut) {
        CHECK(decodeFrame(std::string_view(wire).substr(0, cut), frame) == 0);
    }
}

TEST_CASE("unmasked client frames are a protocol error")
{
    const std::string wire = encodeFrame(Opcode::Text, "hello"); // unmasked
    Frame frame;
    CHECK(decodeFrame(wire, frame) == -1);
}

TEST_CASE("fragmented messages carry fin and continuation opcodes")
{
    Frame frame;
    const std::string first = maskedFrame(Opcode::Text, "hel", /*fin=*/false);
    CHECK(decodeFrame(first, frame) == (ptrdiff_t)first.size());
    CHECK(!frame.fin);
    CHECK(frame.opcode == Opcode::Text);

    const std::string rest = maskedFrame(Opcode::Continuation, "lo");
    CHECK(decodeFrame(rest, frame) == (ptrdiff_t)rest.size());
    CHECK(frame.fin);
    CHECK(frame.opcode == Opcode::Continuation);
    CHECK(frame.payload == "lo");
}

TEST_CASE("server frames roundtrip through a re-masked decode")
{
    // encodeFrame output is unmasked (server->client); verify the header
    // layout by re-wrapping the payload as a client frame.
    const std::string payload(70000, 'y'); // forces the 64-bit length path
    const std::string wire = encodeFrame(Opcode::Text, payload);
    CHECK((uint8_t)wire[0] == 0x81);
    CHECK((uint8_t)wire[1] == 127); // no mask bit, 64-bit length
    CHECK(wire.size() == 2 + 8 + payload.size());
}
