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
    //
    // Adapter selection honors DRIFT_ADAPTER: a case-insensitive substring
    // matched against each adapter's device name (e.g. "llvmpipe" for the
    // deterministic software rasterizer the golden tests use).
    bool init(bool needPresent);

    const wgpu::Device& device() const { return mDevice; }

    // True once any uncaptured device error or unexpected device loss has
    // occurred. Headless/test runs turn this into a failing exit code.
    bool hasError() const { return mHasError; }

    // DRM format modifiers Dawn can import for the given WebGPU format.
    std::vector<uint64_t> importableModifiers(wgpu::TextureFormat format);

    bool createTarget(uint32_t width, uint32_t height, uint32_t fourcc,
                      const std::vector<uint64_t>& modifiers, DmabufTarget& out);
    void destroyTarget(DmabufTarget& target);

    void beginFrame(DmabufTarget& target);
    // EndAccess; when synchronize is set (the buffer will be committed),
    // attaches the GPU-completion fences to the dmabuf as implicit-sync
    // fences (sync-fd export + DMA_BUF_IOCTL_IMPORT_SYNC_FILE), falling
    // back to a CPU wait if the kernel or driver can't.
    void endFrame(DmabufTarget& target, bool synchronize);

    // Pump Dawn callbacks until the queue is idle.
    void waitForQueue();
    void processEvents();

private:
    dawn::native::Instance mInstance;
    wgpu::Adapter mAdapter;
    wgpu::Device mDevice;
    int mDrmFd = -1;
    gbm_device* mGbm = nullptr;
    bool mHasError = false;
    bool mSyncFdBroken = false; // sync-fd path failed once; stay on CPU sync
};

} // namespace drift::platform
