#pragma once

// Video decoding interface (SCENE_FORMAT.md §9.2). Decoding is a platform
// concern (DESIGN.md §3.1: native decoder vs. WebCodecs), so core sees only
// this interface; the platform supplies a factory to Scene::load.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <webgpu/webgpu_cpp.h>

namespace drift::core {

// One dmabuf plane of a zero-copy frame (Y or UV for NV12), described with
// plain handles so core stays free of platform video headers.
struct VideoPlane {
    int fd = -1;         // owned by the decoder; import must duplicate
    uint32_t offset = 0;
    uint32_t stride = 0;
    uint32_t drmFormat = 0; // fourcc
    uint64_t modifier = 0;
    uint32_t width = 0, height = 0;
};

struct VideoFrame {
    // CPU path: tightly packed RGBA, width*4 stride, sRGB, opaque.
    std::vector<uint8_t> rgba;
    // GPU path (rgba and planes empty): the decoder already produced the
    // frame in this texture (sRGB RGBA, opaque, TextureBinding usage).
    // The contents stay valid until the next frameAt call; the consumer
    // samples it directly instead of uploading.
    wgpu::Texture texture;
    // Zero-copy path (rgba empty): dmabuf planes (Y then UV) of a decoder
    // surface. surfaceId is stable across the decoder's surface pool, so
    // consumers can cache their imports; the fds stay valid until the next
    // frameAt call.
    std::vector<VideoPlane> planes;
    uint64_t surfaceId = 0;
    bool bt709 = false;     // YUV matrix (else BT.601)
    bool fullRange = false; // YUV range (else limited/video)
    // Final frame of a non-looping stream (§17.4): the consumer fires its
    // 'finished' event when it observes this.
    bool last = false;
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

    // The consumer failed to import this decoder's zero-copy planes (e.g.
    // the GPU can't take the dmabuf): produce CPU frames from now on.
    virtual void disableZeroCopy() {}

    // Seek back to the start (video 'restart' input, §9.2): subsequent
    // frameAt calls will use small timestamps again, and frame indices keep
    // increasing. Also rearms a finished non-looping stream.
    virtual void restart() {}
};

// Opens path (project-relative; the platform resolves and confines it).
// loop: wrap to the start at end of stream. Returns nullptr with error set
// on failure.
using VideoDecoderFactory = std::function<std::unique_ptr<VideoDecoder>(
    const std::string& path, bool loop, std::string& error)>;

} // namespace drift::core
