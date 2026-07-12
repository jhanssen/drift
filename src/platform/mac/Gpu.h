#pragma once

// macOS platform layer: Dawn native device creation on the Metal backend.
// Presentation goes through a CAMetalLayer-backed wgpu::Surface (CocoaApp),
// so unlike the Linux layer there is no self-allocated render-target
// machinery here; zero-copy video import (IOSurface) is a device feature
// the core requests per frame.

#include <dawn/native/DawnNative.h>
#include <dawn/webgpu_cpp.h>

namespace drift::platform {

class Gpu {
public:
    // needPresent kept for interface parity with the Linux layer: surface
    // presentation needs no additional device features on Metal.
    bool init(bool needPresent);

    const wgpu::Device& device() const { return mDevice; }
    const wgpu::Adapter& adapter() const { return mAdapter; }
    // For wgpu::Surface creation (CocoaApp presents through one).
    wgpu::Instance instance() const { return wgpu::Instance(mInstance.Get()); }

    // True once any uncaptured device error or unexpected device loss has
    // occurred. Headless/test runs turn this into a failing exit code.
    bool hasError() const { return mHasError; }

    // Pump Dawn callbacks until the queue is idle.
    void waitForQueue();
    void processEvents();

private:
    dawn::native::Instance mInstance;
    wgpu::Adapter mAdapter;
    wgpu::Device mDevice;
    bool mHasError = false;
};

} // namespace drift::platform
