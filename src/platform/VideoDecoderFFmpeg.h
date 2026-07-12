#pragma once

// ffmpeg-backed VideoDecoder (software decode + swscale to RGBA, one decode
// thread per instance). The VAAPI/zero-copy dmabuf path replaces this
// implementation behind the same interface later.

#include <memory>
#include <string>

#include "core/Video.h"

namespace drift::platform {

// Opens an absolute (already confined) path. Returns nullptr with error set
// on failure.
std::unique_ptr<drift::core::VideoDecoder> createFFmpegVideoDecoder(
    const std::string& path, bool loop, std::string& error);

} // namespace drift::platform
