#pragma once

// Video decoding interface (SCENE_FORMAT.md §9.2). Decoding is a platform
// concern (DESIGN.md §3.1: native decoder vs. WebCodecs), so core sees only
// this interface; the platform supplies a factory to Scene::load.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace drift::core {

struct VideoFrame {
    std::vector<uint8_t> rgba; // tightly packed, width*4 stride, sRGB, opaque
    uint32_t width = 0, height = 0;
    int64_t index = -1; // increases with every newly decoded frame, loop-aware
};

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;

    // Returns the latest frame with timestamp <= seconds, blocking until the
    // decode thread has produced it (this is what makes headless rendering
    // deterministic: frame content depends on the requested time, never on
    // decode timing). After end of stream (non-looping) the last frame is
    // returned forever. Returns nullptr on decode error. The pointer stays
    // valid until the next frameAt call.
    virtual const VideoFrame* frameAt(double seconds) = 0;
};

// Opens path (project-relative; the platform resolves and confines it).
// loop: wrap to the start at end of stream. Returns nullptr with error set
// on failure.
using VideoDecoderFactory = std::function<std::unique_ptr<VideoDecoder>(
    const std::string& path, bool loop, std::string& error)>;

} // namespace drift::core
