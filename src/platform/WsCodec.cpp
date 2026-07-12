#include "WsCodec.h"

#include <cstring>

namespace drift::platform::ws {

namespace {

// Compact SHA-1 (FIPS 180-1). WebSocket's use is a handshake checksum, not
// security, so SHA-1 remains what the RFC requires.
void sha1(const uint8_t* data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476,
                      0xc3d2e1f0 };
    const uint64_t bitLen = (uint64_t)len * 8;

    // Message + 0x80 pad + zeros + 64-bit length, in 64-byte blocks.
    std::string padded((const char*)data, len);
    padded.push_back((char)0x80);
    while (padded.size() % 64 != 56) {
        padded.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        padded.push_back((char)((bitLen >> (i * 8)) & 0xff));
    }

    for (size_t block = 0; block < padded.size(); block += 64) {
        const uint8_t* p = (const uint8_t*)padded.data() + block;
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
                   ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
        }
        for (int i = 16; i < 80; ++i) {
            const uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (x << 1) | (x >> 31);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5a827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdc;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6;
            }
            const uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d;
            d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h[i];
    }
}

std::string base64(const uint8_t* data, size_t len)
{
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t chunk = (uint32_t)data[i] << 16;
        if (i + 1 < len) {
            chunk |= (uint32_t)data[i + 1] << 8;
        }
        if (i + 2 < len) {
            chunk |= data[i + 2];
        }
        out.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
        out.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
        out.push_back(i + 1 < len ? kAlphabet[(chunk >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < len ? kAlphabet[chunk & 0x3f] : '=');
    }
    return out;
}

// A control message is a small JSON text; anything approaching this is a
// misbehaving or malicious peer.
constexpr uint64_t kMaxPayload = 1 << 20;

} // namespace

std::string acceptKey(std::string_view clientKey)
{
    std::string input(clientKey);
    input += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t digest[20];
    sha1((const uint8_t*)input.data(), input.size(), digest);
    return base64(digest, sizeof(digest));
}

ptrdiff_t decodeFrame(std::string_view buf, Frame& out)
{
    if (buf.size() < 2) {
        return 0;
    }
    const uint8_t b0 = (uint8_t)buf[0];
    const uint8_t b1 = (uint8_t)buf[1];
    if (b0 & 0x70) {
        return -1; // RSV bits: no extensions negotiated
    }
    if (!(b1 & 0x80)) {
        return -1; // client frames must be masked (§5.1)
    }
    out.fin = (b0 & 0x80) != 0;
    out.opcode = (Opcode)(b0 & 0x0f);

    uint64_t len = b1 & 0x7f;
    size_t pos = 2;
    if (len == 126) {
        if (buf.size() < pos + 2) {
            return 0;
        }
        len = ((uint64_t)(uint8_t)buf[pos] << 8) | (uint8_t)buf[pos + 1];
        pos += 2;
    } else if (len == 127) {
        if (buf.size() < pos + 8) {
            return 0;
        }
        len = 0;
        for (int i = 0; i < 8; ++i) {
            len = (len << 8) | (uint8_t)buf[pos + i];
        }
        pos += 8;
    }
    if (len > kMaxPayload) {
        return -1;
    }
    if (buf.size() < pos + 4 + len) {
        return 0;
    }
    uint8_t mask[4];
    std::memcpy(mask, buf.data() + pos, 4);
    pos += 4;

    out.payload.resize(len);
    for (uint64_t i = 0; i < len; ++i) {
        out.payload[i] = (char)((uint8_t)buf[pos + i] ^ mask[i % 4]);
    }
    return (ptrdiff_t)(pos + len);
}

std::string encodeFrame(Opcode opcode, std::string_view payload)
{
    std::string out;
    out.push_back((char)(0x80 | (uint8_t)opcode)); // FIN, no fragmentation
    if (payload.size() < 126) {
        out.push_back((char)payload.size());
    } else if (payload.size() < 65536) {
        out.push_back(126);
        out.push_back((char)(payload.size() >> 8));
        out.push_back((char)(payload.size() & 0xff));
    } else {
        out.push_back(127);
        for (int i = 7; i >= 0; --i) {
            out.push_back((char)((payload.size() >> (i * 8)) & 0xff));
        }
    }
    out += payload;
    return out;
}

} // namespace drift::platform::ws
