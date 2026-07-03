#include "Renderer.h"

namespace drift::core {

namespace {

const char* kShader = R"(
struct Uniforms { data: vec4f }  // x = time (seconds)
@group(0) @binding(0) var<uniform> u: Uniforms;

struct VOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
}

@vertex
fn vs(@builtin(vertex_index) i: u32) -> VOut {
    // fullscreen triangle
    var p = array<vec2f, 3>(vec2f(-1.0, -3.0), vec2f(-1.0, 1.0), vec2f(3.0, 1.0));
    var out: VOut;
    out.pos = vec4f(p[i], 0.0, 1.0);
    out.uv = p[i] * 0.5 + vec2f(0.5);
    return out;
}

@fragment
fn fs(in: VOut) -> @location(0) vec4f {
    let t = u.data.x;
    let a = 0.5 + 0.5 * sin(t * 0.30 + in.uv.x * 3.0);
    let b = 0.5 + 0.5 * sin(t * 0.23 + in.uv.y * 4.0 + 1.7);
    let c = 0.5 + 0.5 * sin(t * 0.17 + (in.uv.x + in.uv.y) * 2.0 + 3.1);
    let col = vec3f(0.05, 0.08, 0.15) + vec3f(a, b, c) * vec3f(0.20, 0.25, 0.45);
    return vec4f(col, 1.0);
}
)";

} // namespace

bool Renderer::init(const wgpu::Device& device, wgpu::TextureFormat targetFormat)
{
    mDevice = device;

    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = kShader;
    wgpu::ShaderModuleDescriptor smDesc{};
    smDesc.nextInChain = &wgsl;
    wgpu::ShaderModule module = mDevice.CreateShaderModule(&smDesc);
    if (!module) {
        return false;
    }

    wgpu::ColorTargetState color{};
    color.format = targetFormat;

    wgpu::FragmentState fragment{};
    fragment.module = module;
    fragment.entryPoint = "fs";
    fragment.targetCount = 1;
    fragment.targets = &color;

    wgpu::RenderPipelineDescriptor rpDesc{};
    rpDesc.vertex.module = module;
    rpDesc.vertex.entryPoint = "vs";
    rpDesc.fragment = &fragment;
    mPipeline = mDevice.CreateRenderPipeline(&rpDesc);
    if (!mPipeline) {
        return false;
    }

    wgpu::BufferDescriptor ubDesc{};
    ubDesc.size = 16;
    ubDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    mUniforms = mDevice.CreateBuffer(&ubDesc);

    wgpu::BindGroupEntry entry{};
    entry.binding = 0;
    entry.buffer = mUniforms;
    entry.size = 16;
    wgpu::BindGroupDescriptor bgDesc{};
    bgDesc.layout = mPipeline.GetBindGroupLayout(0);
    bgDesc.entryCount = 1;
    bgDesc.entries = &entry;
    mBindGroup = mDevice.CreateBindGroup(&bgDesc);

    return true;
}

void Renderer::render(const wgpu::TextureView& target, float timeSeconds)
{
    const float uniforms[4] = { timeSeconds, 0.f, 0.f, 0.f };
    mDevice.GetQueue().WriteBuffer(mUniforms, 0, uniforms, sizeof(uniforms));

    wgpu::RenderPassColorAttachment attachment{};
    attachment.view = target;
    attachment.loadOp = wgpu::LoadOp::Clear;
    attachment.storeOp = wgpu::StoreOp::Store;
    attachment.clearValue = { 0.0, 0.0, 0.0, 1.0 };

    wgpu::RenderPassDescriptor rpDesc{};
    rpDesc.colorAttachmentCount = 1;
    rpDesc.colorAttachments = &attachment;

    wgpu::CommandEncoder encoder = mDevice.CreateCommandEncoder();
    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rpDesc);
    pass.SetPipeline(mPipeline);
    pass.SetBindGroup(0, mBindGroup);
    pass.Draw(3);
    pass.End();

    wgpu::CommandBuffer commands = encoder.Finish();
    mDevice.GetQueue().Submit(1, &commands);
}

} // namespace drift::core
