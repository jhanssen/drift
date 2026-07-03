#pragma once

// Linux platform layer: Dawn native device creation and GBM/dmabuf render
// targets. Everything Dawn-native- or OS-specific stays on this side of the
// core/platform boundary.

#include <cstdint>
#include <vector>

#include <dawn/native/DawnNative.h>
#include <dawn/webgpu_cpp.h>

struct gbm_device;
struct gbm_bo;

namespace drift::platform {

// A GBM-allocated dmabuf imported into Dawn as a render target. The same
// fd/planes are what linux-dmabuf uses to create the wl_buffer, and later
// what zero-copy video decode produces.
struct DmabufTarget {
    gbm_bo* bo = nullptr;
    int fd = -1;
    uint32_t width = 0, height = 0;
    uint32_t fourcc = 0;
    uint64_t modifier = 0;
    struct Plane { uint32_t offset; uint32_t stride; };
    std::vector<Plane> planes;
    wgpu::SharedTextureMemory memory;
    wgpu::Texture texture;
    bool everInitialized = false;
};

class Gpu {
public:
    ~Gpu();

    // needPresent: require the GBM/dmabuf import path (presenting to a
    // compositor). Headless rendering works without it.
    bool init(bool needPresent);

    const wgpu::Device& device() const { return mDevice; }

    // DRM format modifiers Dawn can import for the given WebGPU format.
    std::vector<uint64_t> importableModifiers(wgpu::TextureFormat format);

    bool createTarget(uint32_t width, uint32_t height, uint32_t fourcc,
                      const std::vector<uint64_t>& modifiers, DmabufTarget& out);
    void destroyTarget(DmabufTarget& target);

    void beginFrame(DmabufTarget& target);
    // EndAccess + CPU-wait for queue completion before the buffer is handed
    // to the compositor. TODO: replace the CPU wait with sync-fd export
    // (implicit sync / linux-drm-syncobj).
    void endFrame(DmabufTarget& target);

    // Pump Dawn callbacks until the queue is idle.
    void waitForQueue();
    void processEvents();

private:
    dawn::native::Instance mInstance;
    wgpu::Adapter mAdapter;
    wgpu::Device mDevice;
    int mDrmFd = -1;
    gbm_device* mGbm = nullptr;
};

} // namespace drift::platform
