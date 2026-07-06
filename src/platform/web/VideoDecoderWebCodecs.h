#pragma once

// WebCodecs-backed VideoDecoder for the browser build (DESIGN.md §3.1:
// video decoding is a platform concern; this is the WebCodecs half, the
// native runtime's ffmpeg being the other). The mp4 is demuxed up front
// (core/Mp4Demux), samples feed a decode-ahead VideoDecoder, and frameAt
// picks by timestamp from the decoded queue — so frame selection depends
// on the requested time, matching the contract's determinism intent. The
// one browser concession: frameAt cannot block, so during pipeline warm-up
// (open, seek, loop wrap) it returns the previous frame until decode
// catches up, typically a frame or two of latency.

#include <memory>
#include <string>

#include "core/Video.h"

namespace drift::web {

// Opens an mp4 (h264/h265/av1) from the bundle's MEMFS. absPath must
// already be resolved and confined by the caller. Returns nullptr with
// error set when the file cannot be read or demuxed; a codec the browser
// cannot decode surfaces asynchronously as one black frame plus a console
// error (the load itself cannot await the probe). device receives decoded
// frames via copyExternalImageToTexture (GPU color conversion, no CPU
// pixel round-trip); the canvas capture path remains as the fallback.
std::unique_ptr<core::VideoDecoder> createVideoDecoder(
    const std::string& absPath, bool loop, const wgpu::Device& device,
    std::string& error);

} // namespace drift::web
