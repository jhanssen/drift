#include "Nodes.h"

#include <cmath>
#include <cstring>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include <stb_image.h>

namespace drift::core {

namespace {

constexpr float kTau = 6.28318530717958647692f;

// Fullscreen-triangle vertex stage prepended to every internal pipeline and
// to shader node fragment sources (SCENE_FORMAT.md §9.10: uv [0,0] top-left).
const char* kVertexPrelude = R"(
struct DriftVsOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
}
@vertex
fn drift_vs(@builtin(vertex_index) i: u32) -> DriftVsOut {
    var p = array<vec2f, 3>(vec2f(-1.0, 3.0), vec2f(-1.0, -1.0), vec2f(3.0, -1.0));
    var out: DriftVsOut;
    out.pos = vec4f(p[i], 0.0, 1.0);
    out.uv = vec2f(p[i].x * 0.5 + 0.5, 1.0 - (p[i].y * 0.5 + 0.5));
    return out;
}
)";

const char* kBlitShader = R"(
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var src_sampler: sampler;

fn srgbEncode(c: f32) -> f32 {
    if (c <= 0.0031308) {
        return c * 12.92;
    }
    return 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

@fragment
fn drift_blit_fs(@location(0) uv: vec2f) -> @location(0) vec4f {
    let c = textureSample(src, src_sampler, uv);
    return vec4f(srgbEncode(c.r), srgbEncode(c.g), srgbEncode(c.b), 1.0);
}
)";

wgpu::ShaderModule makeModule(const wgpu::Device& device, const std::string& source)
{
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = source.c_str();
    wgpu::ShaderModuleDescriptor desc{};
    desc.nextInChain = &wgsl;
    return device.CreateShaderModule(&desc);
}

wgpu::RenderPipeline makeFullscreenPipeline(const wgpu::Device& device,
                                            const wgpu::ShaderModule& module,
                                            const char* fsEntry,
                                            wgpu::TextureFormat targetFormat)
{
    wgpu::ColorTargetState color{};
    color.format = targetFormat;

    wgpu::FragmentState fragment{};
    fragment.module = module;
    fragment.entryPoint = fsEntry;
    fragment.targetCount = 1;
    fragment.targets = &color;

    wgpu::RenderPipelineDescriptor desc{};
    desc.vertex.module = module;
    desc.vertex.entryPoint = "drift_vs";
    desc.fragment = &fragment;
    return device.CreateRenderPipeline(&desc);
}

wgpu::Sampler makeLinearClampSampler(const wgpu::Device& device)
{
    wgpu::SamplerDescriptor desc{};
    desc.magFilter = wgpu::FilterMode::Linear;
    desc.minFilter = wgpu::FilterMode::Linear;
    desc.addressModeU = wgpu::AddressMode::ClampToEdge;
    desc.addressModeV = wgpu::AddressMode::ClampToEdge;
    return device.CreateSampler(&desc);
}

void writeOutput(Node::Output& out, const Value& value)
{
    if (!out.value.sameValueAs(value)) {
        out.value = value;
        out.dirty = true;
    }
}

uint16_t floatToHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, sizeof(x));
    const uint32_t sign = (x >> 16) & 0x8000;
    const int32_t exp = (int32_t)((x >> 23) & 0xff) - 127 + 15;
    uint32_t mant = x & 0x7fffff;
    if (exp <= 0) {
        if (exp < -10) {
            return (uint16_t)sign; // flush to zero
        }
        mant = (mant | 0x800000) >> (1 - exp);
        return (uint16_t)(sign | (mant >> 13));
    }
    if (exp >= 31) {
        return (uint16_t)(sign | 0x7c00); // clamp to inf
    }
    return (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
}

float srgbDecode(float c)
{
    return c <= 0.04045f ? c / 12.92f
                         : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

} // namespace

// ---- TimeNode ----

TimeNode::TimeNode()
{
    outputs.resize(2); // 0=seconds (default), 1=delta
    outputs[0].value.type = ValueType::Scalar;
    outputs[1].value.type = ValueType::Scalar;
}

void TimeNode::evaluate(FrameContext& ctx)
{
    Value seconds{};
    seconds.v[0] = ctx.seconds;
    Value delta{};
    delta.v[0] = firstEvaluate ? 0.0f : ctx.seconds - mLast;
    mLast = ctx.seconds;
    writeOutput(outputs[0], seconds);
    writeOutput(outputs[1], delta);
}

// ---- MouseNode ----

MouseNode::MouseNode()
{
    outputs.resize(2); // 0=position (default), 1=active
    outputs[0].value.type = ValueType::Vec2;
    outputs[0].value.v = { 0.5f, 0.5f, 0.0f, 0.0f };
    outputs[1].value.type = ValueType::Scalar;
}

void MouseNode::evaluate(FrameContext& ctx)
{
    // position holds its last value while inactive (§9.8); scenes use
    // 'active' to fade rather than snap.
    if (ctx.mouseActive) {
        Value pos{};
        pos.type = ValueType::Vec2;
        pos.v = { ctx.mouseX, ctx.mouseY, 0.0f, 0.0f };
        writeOutput(outputs[0], pos);
    }
    Value active{};
    active.v[0] = ctx.mouseActive ? 1.0f : 0.0f;
    writeOutput(outputs[1], active);
}

// ---- WaveNode ----

WaveNode::WaveNode(Shape shape)
    : mShape(shape)
{
    outputs.resize(1);
    outputs[0].value.type = ValueType::Scalar;
}

void WaveNode::evaluate(FrameContext&)
{
    const float input = inputValue(0).v[0];
    const float frequency = inputValue(1).v[0];
    const float phase = inputValue(2).v[0];
    const float x = input * frequency + phase;
    const float fract = x - std::floor(x);

    float result = 0.0f;
    switch (mShape) {
    case Shape::Sine: result = std::sin(x * kTau); break;
    case Shape::Triangle: result = 4.0f * std::abs(fract - 0.5f) - 1.0f; break;
    case Shape::Saw: result = 2.0f * fract - 1.0f; break;
    case Shape::Square: result = fract < 0.5f ? 1.0f : -1.0f; break;
    }

    Value out{};
    out.v[0] = result;
    writeOutput(outputs[0], out);
}

// ---- RemapNode ----

RemapNode::RemapNode(bool clamp)
    : mClamp(clamp)
{
    outputs.resize(1);
}

void RemapNode::evaluate(FrameContext&)
{
    const Value value = inputValue(0);
    const Value inMin = inputValue(1);
    const Value inMax = inputValue(2);
    const Value outMin = inputValue(3);
    const Value outMax = inputValue(4);

    Value out{};
    out.type = value.type;
    const int n = componentCount(value.type);
    for (int i = 0; i < n; ++i) {
        const float range = inMax.v[i] - inMin.v[i];
        float t = range != 0.0f ? (value.v[i] - inMin.v[i]) / range : 0.0f;
        float r = outMin.v[i] + t * (outMax.v[i] - outMin.v[i]);
        if (mClamp) {
            const float lo = std::min(outMin.v[i], outMax.v[i]);
            const float hi = std::max(outMin.v[i], outMax.v[i]);
            r = std::min(std::max(r, lo), hi);
        }
        out.v[i] = r;
    }
    writeOutput(outputs[0], out);
}

// ---- CombineNode ----

CombineNode::CombineNode(ValueType resultType)
{
    outputs.resize(1);
    outputs[0].value.type = resultType;
}

void CombineNode::evaluate(FrameContext&)
{
    Value out{};
    out.type = outputs[0].value.type;
    const int n = componentCount(out.type);
    for (int i = 0; i < n; ++i) {
        out.v[i] = inputValue(i).v[0];
    }
    writeOutput(outputs[0], out);
}

// ---- SplitNode ----

SplitNode::SplitNode(int arity)
{
    outputs.resize(arity);
    for (auto& out : outputs) {
        out.value.type = ValueType::Scalar;
    }
}

void SplitNode::evaluate(FrameContext&)
{
    const Value value = inputValue(0);
    for (size_t i = 0; i < outputs.size(); ++i) {
        Value out{};
        out.v[0] = value.v[i];
        writeOutput(outputs[i], out);
    }
}

// ---- ImageNode ----

ImageNode::ImageNode(std::vector<uint16_t> pixels, uint32_t width, uint32_t height)
    : mPixels(std::move(pixels))
    , mWidth(width)
    , mHeight(height)
{
    outputs.resize(1);
    outputs[0].value.type = ValueType::Texture;
}

ImageNode* ImageNode::decode(const std::string& bytes, std::string& error)
{
    int width = 0, height = 0, comp = 0;
    stbi_uc* rgba = stbi_load_from_memory((const stbi_uc*)bytes.data(),
                                          (int)bytes.size(), &width, &height,
                                          &comp, 4);
    if (!rgba) {
        error = stbi_failure_reason();
        return nullptr;
    }

    // sRGB -> linear, premultiply, pack to rgba16float (SCENE_FORMAT.md §12).
    float srgbTable[256];
    for (int i = 0; i < 256; ++i) {
        srgbTable[i] = srgbDecode((float)i / 255.0f);
    }
    std::vector<uint16_t> pixels((size_t)width * height * 4);
    for (size_t i = 0; i < (size_t)width * height; ++i) {
        const float a = (float)rgba[i * 4 + 3] / 255.0f;
        pixels[i * 4 + 0] = floatToHalf(srgbTable[rgba[i * 4 + 0]] * a);
        pixels[i * 4 + 1] = floatToHalf(srgbTable[rgba[i * 4 + 1]] * a);
        pixels[i * 4 + 2] = floatToHalf(srgbTable[rgba[i * 4 + 2]] * a);
        pixels[i * 4 + 3] = floatToHalf(a);
    }
    stbi_image_free(rgba);
    return new ImageNode(std::move(pixels), (uint32_t)width, (uint32_t)height);
}

void ImageNode::evaluate(FrameContext& ctx)
{
    if (!mTexture) {
        wgpu::TextureDescriptor desc{};
        desc.format = wgpu::TextureFormat::RGBA16Float;
        desc.size = { mWidth, mHeight, 1 };
        desc.usage = wgpu::TextureUsage::TextureBinding |
                     wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::CopySrc;
        mTexture = ctx.device.CreateTexture(&desc);

        wgpu::TexelCopyTextureInfo dst{};
        dst.texture = mTexture;
        wgpu::TexelCopyBufferLayout layout{};
        layout.bytesPerRow = mWidth * 8;
        layout.rowsPerImage = mHeight;
        wgpu::Extent3D extent = { mWidth, mHeight, 1 };
        ctx.device.GetQueue().WriteTexture(&dst, mPixels.data(),
                                           mPixels.size() * sizeof(uint16_t),
                                           &layout, &extent);
        mPixels = {};
    }

    Value out{};
    out.type = ValueType::Texture;
    out.texture = mTexture;
    out.texWidth = mWidth;
    out.texHeight = mHeight;
    writeOutput(outputs[0], out); // dirty on first evaluate only
}

// ---- VideoNode ----

VideoNode::VideoNode(std::unique_ptr<VideoDecoder> decoder)
    : mDecoder(std::move(decoder))
{
    outputs.resize(1);
    outputs[0].value.type = ValueType::Texture;
}

void VideoNode::evaluate(FrameContext& ctx)
{
    const VideoFrame* frame = mDecoder->frameAt(ctx.seconds);
    if (!frame) {
        return; // decode error; hold the previous frame
    }

    if (!mTexture || frame->width != mWidth || frame->height != mHeight) {
        wgpu::TextureDescriptor desc{};
        // sRGB view: sampling converts to linear in hardware, so the CPU
        // side stays a cheap byte copy per frame. Opaque, so premultiplied
        // trivially (§12).
        desc.format = wgpu::TextureFormat::RGBA8UnormSrgb;
        desc.size = { frame->width, frame->height, 1 };
        desc.usage = wgpu::TextureUsage::TextureBinding |
                     wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::CopySrc;
        mTexture = ctx.device.CreateTexture(&desc);
        mWidth = frame->width;
        mHeight = frame->height;
        mLastIndex = -1;
    }

    if (frame->index != mLastIndex) {
        wgpu::TexelCopyTextureInfo dst{};
        dst.texture = mTexture;
        wgpu::TexelCopyBufferLayout layout{};
        layout.bytesPerRow = mWidth * 4;
        layout.rowsPerImage = mHeight;
        wgpu::Extent3D extent = { mWidth, mHeight, 1 };
        ctx.device.GetQueue().WriteTexture(&dst, frame->rgba.data(),
                                           frame->rgba.size(), &layout, &extent);
        mLastIndex = frame->index;

        Value out{};
        out.type = ValueType::Texture;
        out.texture = mTexture;
        out.texWidth = mWidth;
        out.texHeight = mHeight;
        outputs[0].value = out;
        outputs[0].dirty = true; // same texture object, new contents
    }
}

// ---- TransformNode ----

namespace {

// Quad corners in source-uv space, positioned by the vertex stage per
// SCENE_FORMAT.md §9.4: extent in output-height units so scale is
// resolution-independent, rotation about the anchor in physical (aspect-
// corrected) space, y-down clockwise.
const char* kTransformShader = R"(
struct TransformParams {
    position: vec2f,
    scale: vec2f,
    anchor: vec2f,
    rotation: f32,
    opacity: f32,
    srcAspect: f32,
    outAspect: f32,
}
@group(0) @binding(0) var<uniform> params: TransformParams;
@group(0) @binding(1) var src: texture_2d<f32>;
@group(0) @binding(2) var src_sampler: sampler;

struct TransformVsOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
}

@vertex
fn transform_vs(@builtin(vertex_index) i: u32) -> TransformVsOut {
    var corners = array<vec2f, 4>(
        vec2f(0.0, 0.0), vec2f(1.0, 0.0), vec2f(0.0, 1.0), vec2f(1.0, 1.0));
    let c = corners[i];
    let extent = vec2f(params.scale.x * params.srcAspect, params.scale.y);
    let local = (c - params.anchor) * extent;
    let s = sin(params.rotation);
    let co = cos(params.rotation);
    let rotated = vec2f(local.x * co - local.y * s, local.x * s + local.y * co);
    let uvPos = params.position + vec2f(rotated.x / params.outAspect, rotated.y);
    var out: TransformVsOut;
    out.pos = vec4f(uvPos.x * 2.0 - 1.0, 1.0 - uvPos.y * 2.0, 0.0, 1.0);
    out.uv = c;
    return out;
}

@fragment
fn transform_fs(@location(0) uv: vec2f) -> @location(0) vec4f {
    // Premultiplied: opacity scales all channels.
    return textureSample(src, src_sampler, uv) * params.opacity;
}
)";

struct TransformUniforms {
    float position[2];
    float scale[2];
    float anchor[2];
    float rotation;
    float opacity;
    float srcAspect;
    float outAspect;
    float pad[2]; // uniform buffer sizes rounded to 16
};
static_assert(sizeof(TransformUniforms) == 48);

} // namespace

TransformNode::TransformNode()
{
    outputs.resize(1);
    outputs[0].value.type = ValueType::Texture;
}

bool TransformNode::ensurePipeline(FrameContext& ctx)
{
    if (mPipeline) {
        return true;
    }
    wgpu::ShaderModule module = makeModule(ctx.device, kTransformShader);
    if (!module) {
        return false;
    }

    wgpu::ColorTargetState color{};
    color.format = wgpu::TextureFormat::RGBA16Float;

    wgpu::FragmentState fragment{};
    fragment.module = module;
    fragment.entryPoint = "transform_fs";
    fragment.targetCount = 1;
    fragment.targets = &color;

    wgpu::RenderPipelineDescriptor desc{};
    desc.vertex.module = module;
    desc.vertex.entryPoint = "transform_vs";
    desc.primitive.topology = wgpu::PrimitiveTopology::TriangleStrip;
    desc.fragment = &fragment;
    mPipeline = ctx.device.CreateRenderPipeline(&desc);
    if (!mPipeline) {
        return false;
    }

    wgpu::BufferDescriptor bd{};
    bd.size = sizeof(TransformUniforms);
    bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    mUniforms = ctx.device.CreateBuffer(&bd);
    mSampler = makeLinearClampSampler(ctx.device);
    return true;
}

void TransformNode::evaluate(FrameContext& ctx)
{
    const Value source = inputValue(0);
    if (!source.texture || !ensurePipeline(ctx)) {
        return;
    }

    if (!mColor || mWidth != ctx.targetWidth || mHeight != ctx.targetHeight) {
        wgpu::TextureDescriptor desc{};
        desc.format = wgpu::TextureFormat::RGBA16Float;
        desc.size = { ctx.targetWidth, ctx.targetHeight, 1 };
        desc.usage = wgpu::TextureUsage::RenderAttachment |
                     wgpu::TextureUsage::TextureBinding |
                     wgpu::TextureUsage::CopySrc;
        mColor = ctx.device.CreateTexture(&desc);
        mWidth = ctx.targetWidth;
        mHeight = ctx.targetHeight;
    }

    constexpr float kDegToRad = kTau / 360.0f;
    TransformUniforms u{};
    u.position[0] = inputValue(1).v[0];
    u.position[1] = inputValue(1).v[1];
    u.scale[0] = inputValue(3).v[0];
    u.scale[1] = inputValue(3).v[1];
    u.anchor[0] = inputValue(4).v[0];
    u.anchor[1] = inputValue(4).v[1];
    u.rotation = inputValue(2).v[0] * kDegToRad;
    u.opacity = inputValue(5).v[0];
    u.srcAspect = source.texHeight ? (float)source.texWidth / source.texHeight : 1.0f;
    u.outAspect = mHeight ? (float)mWidth / mHeight : 1.0f;
    ctx.device.GetQueue().WriteBuffer(mUniforms, 0, &u, sizeof(u));

    wgpu::BindGroupEntry entries[3] = {};
    entries[0].binding = 0;
    entries[0].buffer = mUniforms;
    entries[0].size = sizeof(TransformUniforms);
    entries[1].binding = 1;
    entries[1].textureView = source.texture.CreateView();
    entries[2].binding = 2;
    entries[2].sampler = mSampler;
    wgpu::BindGroupDescriptor bgDesc{};
    bgDesc.layout = mPipeline.GetBindGroupLayout(0);
    bgDesc.entryCount = 3;
    bgDesc.entries = entries;
    wgpu::BindGroup group = ctx.device.CreateBindGroup(&bgDesc);

    wgpu::RenderPassColorAttachment attachment{};
    attachment.view = mColor.CreateView();
    attachment.loadOp = wgpu::LoadOp::Clear;
    attachment.storeOp = wgpu::StoreOp::Store;
    attachment.clearValue = { 0.0, 0.0, 0.0, 0.0 };

    wgpu::RenderPassDescriptor desc{};
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &attachment;

    wgpu::CommandEncoder encoder = ctx.device.CreateCommandEncoder();
    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&desc);
    pass.SetPipeline(mPipeline);
    pass.SetBindGroup(0, group);
    pass.Draw(4);
    pass.End();
    wgpu::CommandBuffer commands = encoder.Finish();
    ctx.device.GetQueue().Submit(1, &commands);

    Value out{};
    out.type = ValueType::Texture;
    out.texture = mColor;
    out.texWidth = mWidth;
    out.texHeight = mHeight;
    outputs[0].value = out;
    outputs[0].dirty = true;
}

// ---- CompositorNode ----

namespace {

const char* kCompositeShader = R"(
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var src_sampler: sampler;

@fragment
fn composite_fs(@location(0) uv: vec2f) -> @location(0) vec4f {
    return textureSample(src, src_sampler, uv);
}
)";

} // namespace

CompositorNode::CompositorNode()
{
    outputs.resize(1);
    outputs[0].value.type = ValueType::Texture;
}

bool CompositorNode::ensurePipeline(FrameContext& ctx)
{
    if (mPipeline) {
        return true;
    }
    const std::string source = std::string(kVertexPrelude) + kCompositeShader;
    wgpu::ShaderModule module = makeModule(ctx.device, source);
    if (!module) {
        return false;
    }

    // Premultiplied source-over (§9.5).
    wgpu::BlendState blend{};
    blend.color.srcFactor = wgpu::BlendFactor::One;
    blend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
    blend.alpha.srcFactor = wgpu::BlendFactor::One;
    blend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;

    wgpu::ColorTargetState color{};
    color.format = wgpu::TextureFormat::RGBA16Float;
    color.blend = &blend;

    wgpu::FragmentState fragment{};
    fragment.module = module;
    fragment.entryPoint = "composite_fs";
    fragment.targetCount = 1;
    fragment.targets = &color;

    wgpu::RenderPipelineDescriptor desc{};
    desc.vertex.module = module;
    desc.vertex.entryPoint = "drift_vs";
    desc.fragment = &fragment;
    mPipeline = ctx.device.CreateRenderPipeline(&desc);
    if (!mPipeline) {
        return false;
    }
    mSampler = makeLinearClampSampler(ctx.device);
    return true;
}

void CompositorNode::evaluate(FrameContext& ctx)
{
    if (!ensurePipeline(ctx)) {
        return;
    }

    if (!mColor || mWidth != ctx.targetWidth || mHeight != ctx.targetHeight) {
        wgpu::TextureDescriptor desc{};
        desc.format = wgpu::TextureFormat::RGBA16Float;
        desc.size = { ctx.targetWidth, ctx.targetHeight, 1 };
        desc.usage = wgpu::TextureUsage::RenderAttachment |
                     wgpu::TextureUsage::TextureBinding |
                     wgpu::TextureUsage::CopySrc;
        mColor = ctx.device.CreateTexture(&desc);
        mWidth = ctx.targetWidth;
        mHeight = ctx.targetHeight;
    }

    wgpu::RenderPassColorAttachment attachment{};
    attachment.view = mColor.CreateView();
    attachment.loadOp = wgpu::LoadOp::Clear;
    attachment.storeOp = wgpu::StoreOp::Store;
    attachment.clearValue = { 0.0, 0.0, 0.0, 0.0 };

    wgpu::RenderPassDescriptor desc{};
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &attachment;

    wgpu::CommandEncoder encoder = ctx.device.CreateCommandEncoder();
    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&desc);
    pass.SetPipeline(mPipeline);
    for (size_t i = 0; i < inputs.size(); ++i) {
        const Value layer = inputValue(i);
        if (!layer.texture) {
            continue;
        }
        wgpu::BindGroupEntry entries[2] = {};
        entries[0].binding = 0;
        entries[0].textureView = layer.texture.CreateView();
        entries[1].binding = 1;
        entries[1].sampler = mSampler;
        wgpu::BindGroupDescriptor bgDesc{};
        bgDesc.layout = mPipeline.GetBindGroupLayout(0);
        bgDesc.entryCount = 2;
        bgDesc.entries = entries;
        pass.SetBindGroup(0, ctx.device.CreateBindGroup(&bgDesc));
        pass.Draw(3);
    }
    pass.End();
    wgpu::CommandBuffer commands = encoder.Finish();
    ctx.device.GetQueue().Submit(1, &commands);

    Value out{};
    out.type = ValueType::Texture;
    out.texture = mColor;
    out.texWidth = mWidth;
    out.texHeight = mHeight;
    outputs[0].value = out;
    outputs[0].dirty = true;
}

// ---- ShaderNode ----

ShaderNode::ShaderNode(std::string fragmentSource, WgslInterface iface,
                       uint32_t explicitWidth, uint32_t explicitHeight)
    : mFragmentSource(std::move(fragmentSource))
    , mInterface(std::move(iface))
    , mExplicitWidth(explicitWidth)
    , mExplicitHeight(explicitHeight)
{
    outputs.resize(1);
    outputs[0].value.type = ValueType::Texture;
}

bool ShaderNode::ensurePipeline(FrameContext& ctx)
{
    if (mPipeline) {
        return true;
    }
    const std::string source = std::string(kVertexPrelude) + mFragmentSource;
    wgpu::ShaderModule module = makeModule(ctx.device, source);
    if (!module) {
        return false;
    }
    // Intermediates are linear rgba16float (SCENE_FORMAT.md §12).
    mPipeline = makeFullscreenPipeline(ctx.device, module, "main",
                                       wgpu::TextureFormat::RGBA16Float);
    if (!mPipeline) {
        return false;
    }
    if (mInterface.hasUniforms) {
        wgpu::BufferDescriptor desc{};
        desc.size = mInterface.uniformSize;
        desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        mUniforms = ctx.device.CreateBuffer(&desc);
    }
    mSampler = makeLinearClampSampler(ctx.device);
    return true;
}

void ShaderNode::evaluate(FrameContext& ctx)
{
    if (!ensurePipeline(ctx)) {
        return;
    }

    // Size: explicit, else first texture input, else the output target
    // (SCENE_FORMAT.md §9.3).
    uint32_t width = mExplicitWidth, height = mExplicitHeight;
    if (width == 0) {
        for (size_t i = textureInputStart(); i < inputs.size(); ++i) {
            const Value v = inputValue(i);
            if (v.texWidth != 0) {
                width = v.texWidth;
                height = v.texHeight;
                break;
            }
        }
    }
    if (width == 0) {
        width = ctx.targetWidth;
        height = ctx.targetHeight;
    }

    if (!mColor || width != mWidth || height != mHeight) {
        wgpu::TextureDescriptor desc{};
        desc.format = wgpu::TextureFormat::RGBA16Float;
        desc.size = { width, height, 1 };
        desc.usage = wgpu::TextureUsage::RenderAttachment |
                     wgpu::TextureUsage::TextureBinding |
                     wgpu::TextureUsage::CopySrc;
        mColor = ctx.device.CreateTexture(&desc);
        mWidth = width;
        mHeight = height;
    }

    if (mInterface.hasUniforms) {
        std::vector<uint8_t> data(mInterface.uniformSize, 0);
        for (size_t i = 0; i < mInterface.fields.size(); ++i) {
            const Value v = inputValue(i);
            std::memcpy(data.data() + mInterface.fields[i].offset, v.v.data(),
                        componentCount(mInterface.fields[i].type) * sizeof(float));
        }
        ctx.device.GetQueue().WriteBuffer(mUniforms, 0, data.data(), data.size());
    }

    // Bind groups per author-chosen group indices. Scaffold: rebuilt every
    // evaluate; cache on input-texture identity later.
    uint32_t maxGroup = 0;
    if (mInterface.hasUniforms) {
        maxGroup = std::max(maxGroup, mInterface.uniformGroup);
    }
    for (const auto& t : mInterface.textures) {
        maxGroup = std::max(maxGroup, t.group);
        if (t.hasSampler) {
            maxGroup = std::max(maxGroup, t.samplerGroup);
        }
    }
    std::vector<wgpu::BindGroup> groups(maxGroup + 1);
    for (uint32_t g = 0; g <= maxGroup; ++g) {
        std::vector<wgpu::BindGroupEntry> entries;
        if (mInterface.hasUniforms && mInterface.uniformGroup == g) {
            wgpu::BindGroupEntry e{};
            e.binding = mInterface.uniformBinding;
            e.buffer = mUniforms;
            e.size = mInterface.uniformSize;
            entries.push_back(e);
        }
        for (size_t i = 0; i < mInterface.textures.size(); ++i) {
            const auto& t = mInterface.textures[i];
            const Value v = inputValue(textureInputStart() + i);
            if (t.group == g) {
                wgpu::BindGroupEntry e{};
                e.binding = t.binding;
                e.textureView = v.texture.CreateView();
                entries.push_back(e);
            }
            if (t.hasSampler && t.samplerGroup == g) {
                wgpu::BindGroupEntry e{};
                e.binding = t.samplerBinding;
                e.sampler = mSampler;
                entries.push_back(e);
            }
        }
        wgpu::BindGroupDescriptor desc{};
        desc.layout = mPipeline.GetBindGroupLayout(g);
        desc.entryCount = entries.size();
        desc.entries = entries.data();
        groups[g] = ctx.device.CreateBindGroup(&desc);
    }

    wgpu::RenderPassColorAttachment attachment{};
    attachment.view = mColor.CreateView();
    attachment.loadOp = wgpu::LoadOp::Clear;
    attachment.storeOp = wgpu::StoreOp::Store;
    attachment.clearValue = { 0.0, 0.0, 0.0, 0.0 };

    wgpu::RenderPassDescriptor desc{};
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &attachment;

    wgpu::CommandEncoder encoder = ctx.device.CreateCommandEncoder();
    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&desc);
    pass.SetPipeline(mPipeline);
    for (uint32_t g = 0; g < groups.size(); ++g) {
        pass.SetBindGroup(g, groups[g]);
    }
    pass.Draw(3);
    pass.End();
    wgpu::CommandBuffer commands = encoder.Finish();
    ctx.device.GetQueue().Submit(1, &commands);

    Value out{};
    out.type = ValueType::Texture;
    out.texture = mColor;
    out.texWidth = mWidth;
    out.texHeight = mHeight;
    outputs[0].value = out;
    outputs[0].dirty = true;
}

// ---- OutputNode ----

OutputNode::OutputNode()
{
    // No outputs; single texture input (color).
}

void OutputNode::evaluate(FrameContext& ctx)
{
    if (!mPipeline || mPipelineFormat != ctx.targetFormat) {
        const std::string source = std::string(kVertexPrelude) + kBlitShader;
        wgpu::ShaderModule module = makeModule(ctx.device, source);
        mPipeline = makeFullscreenPipeline(ctx.device, module, "drift_blit_fs",
                                           ctx.targetFormat);
        mPipelineFormat = ctx.targetFormat;
        mSampler = makeLinearClampSampler(ctx.device);
    }

    const Value color = inputValue(0);
    if (!color.texture) {
        return;
    }

    wgpu::BindGroupEntry entries[2] = {};
    entries[0].binding = 0;
    entries[0].textureView = color.texture.CreateView();
    entries[1].binding = 1;
    entries[1].sampler = mSampler;
    wgpu::BindGroupDescriptor bgDesc{};
    bgDesc.layout = mPipeline.GetBindGroupLayout(0);
    bgDesc.entryCount = 2;
    bgDesc.entries = entries;
    wgpu::BindGroup group = ctx.device.CreateBindGroup(&bgDesc);

    wgpu::RenderPassColorAttachment attachment{};
    attachment.view = ctx.target;
    attachment.loadOp = wgpu::LoadOp::Clear;
    attachment.storeOp = wgpu::StoreOp::Store;
    attachment.clearValue = { 0.0, 0.0, 0.0, 1.0 };

    wgpu::RenderPassDescriptor desc{};
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &attachment;

    wgpu::CommandEncoder encoder = ctx.device.CreateCommandEncoder();
    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&desc);
    pass.SetPipeline(mPipeline);
    pass.SetBindGroup(0, group);
    pass.Draw(3);
    pass.End();
    wgpu::CommandBuffer commands = encoder.Finish();
    ctx.device.GetQueue().Submit(1, &commands);

    ctx.presented = true;
}

} // namespace drift::core
