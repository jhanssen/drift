#pragma once

// Minimal ISO-BMFF (mp4) demuxer for the browser video decoder: WebCodecs
// decodes but does not demux, so the web platform needs sample-level access
// to feed it. Deliberately a bounded reader in the WgslInterface spirit —
// non-fragmented files, the first video track, avc1/avc3, hvc1/hev1, and
// av01 sample entries — not a general mp4 library. The native runtime uses
// ffmpeg and never calls this; it lives in core because it is pure,
// portable, and unit-testable there.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace drift::core {

struct Mp4Sample {
    uint64_t offset = 0; // byte offset into the file
    uint32_t size = 0;
    double pts = 0.0;    // presentation time, seconds; earliest sample = 0
    bool keyframe = false;
};

struct Mp4Track {
    // RFC 6381 string for VideoDecoderConfig.codec ("avc1.64000b",
    // "hvc1.1.6.L30.b0", "av01.0.00M.08").
    std::string codec;
    // VideoDecoderConfig.description: the avcC/hvcC payload. Empty for AV1
    // (its config is carried in-band).
    std::vector<uint8_t> description;
    uint32_t width = 0, height = 0;
    double duration = 0.0;          // seconds
    std::vector<Mp4Sample> samples; // decode order == WebCodecs feed order
};

// Parses a complete mp4 held in memory. Returns false with `error` set for
// anything outside the supported envelope (fragmented movie, no video
// track, unsupported codec, truncated tables).
bool parseMp4(const uint8_t* data, size_t size, Mp4Track& track,
              std::string& error);

} // namespace drift::core
