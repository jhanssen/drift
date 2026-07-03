#pragma once

// Platform-agnostic core. Rule for everything under src/core/: only the
// portable WebGPU API — no dawn/native, no Wayland, no GBM, no OS calls.
// This code must recompile unchanged under Emscripten for the browser runtime.

#include <webgpu/webgpu_cpp.h>

namespace drift::core {

// Draws the placeholder scene (an animated gradient) into a caller-provided
// render target. Knows nothing about where the target came from or how it is
// presented. Grows into the scene-graph executor.
class Renderer {
public:
    bool init(const wgpu::Device& device, wgpu::TextureFormat targetFormat);
    void render(const wgpu::TextureView& target, float timeSeconds);

private:
    wgpu::Device mDevice;
    wgpu::RenderPipeline mPipeline;
    wgpu::Buffer mUniforms;
    wgpu::BindGroup mBindGroup;
};

} // namespace drift::core
