#include "Nodes.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include <stb_image.h>

#include <ktx.h>
#include <webp/decode.h>

namespace drift::core {

namespace {

constexpr double kTau = 6.28318530717958647692;

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
    // Trilinear when the source has a mip chain (KTX2); a no-op otherwise.
    desc.mipmapFilter = wgpu::MipmapFilterMode::Linear;
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
    delta.v[0] = firstEvaluate ? 0.0 : ctx.seconds - mLast;
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
    const double input = inputValue(0).v[0];
    const double frequency = inputValue(1).v[0];
    const double phase = inputValue(2).v[0];
    // Reduce to one cycle in double before any trig: with f32 (or an
    // unreduced trig argument) the waveform turns steppy after ~a day of
    // scene time (§17.2).
    const double x = input * frequency + phase;
    const double fract = x - std::floor(x);

    double result = 0.0;
    switch (mShape) {
    case Shape::Sine: result = std::sin(fract * kTau); break;
    case Shape::Triangle: result = 4.0 * std::abs(fract - 0.5) - 1.0; break;
    case Shape::Saw: result = 2.0 * fract - 1.0; break;
    case Shape::Square: result = fract < 0.5 ? 1.0 : -1.0; break;
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
        const double range = inMax.v[i] - inMin.v[i];
        double t = range != 0.0 ? (value.v[i] - inMin.v[i]) / range : 0.0;
        double r = outMin.v[i] + t * (outMax.v[i] - outMin.v[i]);
        if (mClamp) {
            const double lo = std::min(outMin.v[i], outMax.v[i]);
            const double hi = std::max(outMin.v[i], outMax.v[i]);
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

// ---- SequenceNode ----

SequenceNode::SequenceNode(double duration, bool loop, std::vector<Track> tracks)
    : mDuration(duration), mLoop(loop), mTracks(std::move(tracks))
{
    outputs.resize(mTracks.size());
    for (size_t i = 0; i < mTracks.size(); ++i) {
        outputs[i].value.type =
            mTracks[i].event ? ValueType::Event : mTracks[i].type;
    }
}

void SequenceNode::evaluate(FrameContext&)
{
    // Local-time reduction in double before any interpolation (§17.2).
    const double time = inputValue(0).v[0];
    double t;
    if (mLoop) {
        t = std::fmod(time, mDuration);
        if (t < 0.0) {
            t += mDuration;
        }
    } else {
        t = std::clamp(time, 0.0, mDuration);
    }

    for (size_t i = 0; i < mTracks.size(); ++i) {
        const Track& track = mTracks[i];
        if (track.event) {
            // Cue crossing (§9.9): with previous local time t0 and current
            // t1, cues in (t0, t1] fire; on a loop wrap, (t0, duration) and
            // [0, t1]; on the first evaluation, [0, t1]. A fire is the
            // output's dirty flag — no value, no change detection.
            const double t0 = mLastLocalTime;
            const bool wrapped = !firstEvaluate && t < t0;
            for (double cue : track.fires) {
                const bool fired =
                    firstEvaluate ? cue <= t
                    : wrapped     ? cue > t0 || cue <= t
                                  : cue > t0 && cue <= t;
                if (fired) {
                    outputs[i].dirty = true;
                    break;
                }
            }
            continue;
        }
        // Last key with key.t <= t; before the first key, the first key's
        // value; after the last, the last key's (§9.9 — no wrap-around
        // interpolation).
        size_t k = 0;
        while (k + 1 < track.keys.size() && track.keys[k + 1].t <= t) {
            ++k;
        }
        Value out = track.keys[k].value;
        if (track.interpolate != Interpolate::Hold &&
            k + 1 < track.keys.size() && t > track.keys[k].t) {
            const Key& a = track.keys[k];
            const Key& b = track.keys[k + 1];
            double f = (t - a.t) / (b.t - a.t);
            if (track.interpolate == Interpolate::Smooth) {
                f = f * f * (3.0 - 2.0 * f);
            }
            const int n = componentCount(track.type);
            for (int c = 0; c < n; ++c) {
                out.v[c] = a.value.v[c] + (b.value.v[c] - a.value.v[c]) * f;
            }
        }
        writeOutput(outputs[i], out);
    }
    mLastLocalTime = t;
}

// ---- ImageNode ----

namespace {

// CPU-side image plumbing for the texture-edge contract (§12): linear
// color, premultiplied alpha. Filtering happens in this space (the only
// space where averaging texels is correct), then packs to rgba16float.
struct FloatImage {
    std::vector<float> px; // rgba
    uint32_t width = 0, height = 0;
};

FloatImage toLinearPremultiplied(const uint8_t* rgba, uint32_t width,
                                 uint32_t height, bool srgb, bool premultiplied)
{
    float toLinear[256];
    for (int i = 0; i < 256; ++i) {
        const float c = (float)i / 255.0f;
        toLinear[i] = srgb ? srgbDecode(c) : c;
    }
    FloatImage img;
    img.width = width;
    img.height = height;
    img.px.resize((size_t)width * height * 4);
    for (size_t i = 0; i < (size_t)width * height; ++i) {
        const float a = (float)rgba[i * 4 + 3] / 255.0f;
        const float mul = premultiplied ? 1.0f : a;
        img.px[i * 4 + 0] = toLinear[rgba[i * 4 + 0]] * mul;
        img.px[i * 4 + 1] = toLinear[rgba[i * 4 + 1]] * mul;
        img.px[i * 4 + 2] = toLinear[rgba[i * 4 + 2]] * mul;
        img.px[i * 4 + 3] = a;
    }
    return img;
}

FloatImage downsample(const FloatImage& src)
{
    FloatImage dst;
    dst.width = std::max(1u, src.width / 2);
    dst.height = std::max(1u, src.height / 2);
    dst.px.resize((size_t)dst.width * dst.height * 4);
    for (uint32_t y = 0; y < dst.height; ++y) {
        for (uint32_t x = 0; x < dst.width; ++x) {
            const uint32_t x0 = 2 * x, y0 = 2 * y;
            const uint32_t x1 = std::min(x0 + 1, src.width - 1);
            const uint32_t y1 = std::min(y0 + 1, src.height - 1);
            const float* p00 = &src.px[((size_t)y0 * src.width + x0) * 4];
            const float* p10 = &src.px[((size_t)y0 * src.width + x1) * 4];
            const float* p01 = &src.px[((size_t)y1 * src.width + x0) * 4];
            const float* p11 = &src.px[((size_t)y1 * src.width + x1) * 4];
            float* d = &dst.px[((size_t)y * dst.width + x) * 4];
            for (int c = 0; c < 4; ++c) {
                d[c] = (p00[c] + p10[c] + p01[c] + p11[c]) * 0.25f;
            }
        }
    }
    return dst;
}

void packHalf(const FloatImage& img, std::vector<uint8_t>& out,
              uint32_t& width, uint32_t& height)
{
    out.resize(img.px.size() * 2);
    auto* half = (uint16_t*)out.data();
    for (size_t i = 0; i < img.px.size(); ++i) {
        half[i] = floatToHalf(img.px[i]);
    }
    width = img.width;
    height = img.height;
}

// Full mip chain from an 8-bit RGBA base (§9.1: CPU-decoded sources get
// generated mips so transform-minified layers don't alias).
template <typename Level>
std::vector<Level> buildMipChain(const uint8_t* rgba, uint32_t width,
                                 uint32_t height, bool srgb, bool premultiplied)
{
    std::vector<Level> levels;
    FloatImage img = toLinearPremultiplied(rgba, width, height, srgb,
                                           premultiplied);
    for (;;) {
        Level& level = levels.emplace_back();
        packHalf(img, level.data, level.width, level.height);
        if (img.width == 1 && img.height == 1) {
            break;
        }
        img = downsample(img);
    }
    return levels;
}

// VkFormat values libktx reports for the payloads we accept.
constexpr uint32_t kVkFormatR8G8B8A8Unorm = 37;
constexpr uint32_t kVkFormatR8G8B8A8Srgb = 43;
constexpr uint32_t kVkFormatBc7Unorm = 145;
constexpr uint32_t kVkFormatBc7Srgb = 146;

// Tightly-packed byte size of one level row block for a format we upload.
uint32_t bytesPerRow(wgpu::TextureFormat format, uint32_t width)
{
    if (format == wgpu::TextureFormat::RGBA16Float) {
        return width * 8;
    }
    return ((width + 3) / 4) * 16; // BC7: 4x4 texel blocks, 16 bytes each
}

uint32_t rowCount(wgpu::TextureFormat format, uint32_t height)
{
    if (format == wgpu::TextureFormat::RGBA16Float) {
        return height;
    }
    return (height + 3) / 4;
}

} // namespace

ImageNode::ImageNode(std::vector<Level> levels, wgpu::TextureFormat format)
    : mLevels(std::move(levels))
    , mFormat(format)
    , mWidth(mLevels[0].width)
    , mHeight(mLevels[0].height)
{
    outputs.resize(1);
    outputs[0].value.type = ValueType::Texture;
}

ImageNode* ImageNode::decode(const std::string& bytes, std::string& error)
{
    const auto* data = (const uint8_t*)bytes.data();
    const size_t size = bytes.size();

    // KTX2 (§9.1): GPU texture container, possibly Basis-supercompressed,
    // with an optional stored mip chain.
    static const uint8_t kKtx2Magic[12] = { 0xAB, 'K',  'T',  'X',  ' ',  '2',
                                            '0',  0xBB, '\r', '\n', 0x1A, '\n' };
    if (size >= 12 && std::memcmp(data, kKtx2Magic, 12) == 0) {
        ktxTexture2* tex = nullptr;
        KTX_error_code rc = ktxTexture2_CreateFromMemory(
            data, size, KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &tex);
        if (rc != KTX_SUCCESS) {
            error = ktxErrorString(rc);
            return nullptr;
        }
        if (tex->numDimensions != 2 || tex->numFaces != 1 ||
            tex->numLayers != 1) {
            error = "only plain 2D KTX2 textures are supported";
            ktxTexture_Destroy((ktxTexture*)tex);
            return nullptr;
        }

        // Basis-supercompressed: transcode to BC7 and upload the compressed
        // blocks as-is. Blocks can't be premultiplied after the fact, so the
        // content must already satisfy the premultiplied contract — either
        // by declaring it (--premultiply at authoring) or by being opaque.
        // Deliberately not decompressing to RGBA behind the author's back.
        if (ktxTexture2_NeedsTranscoding(tex)) {
            const bool premultiplied = ktxTexture2_GetPremultipliedAlpha(tex);
            const bool opaque = ktxTexture2_GetNumComponents(tex) < 4;
            if (!premultiplied && !opaque) {
                error = "Basis KTX2 with straight alpha cannot satisfy the "
                        "premultiplied-alpha contract; author it premultiplied "
                        "(ktx create --assign-texcoord... --premultiply) or "
                        "use WebP/PNG";
                ktxTexture_Destroy((ktxTexture*)tex);
                return nullptr;
            }
            if ((tex->baseWidth % 4) != 0 || (tex->baseHeight % 4) != 0) {
                error = "Basis KTX2 dimensions must be multiples of 4 for "
                        "compressed upload";
                ktxTexture_Destroy((ktxTexture*)tex);
                return nullptr;
            }
            rc = ktxTexture2_TranscodeBasis(tex, KTX_TTF_BC7_RGBA, 0);
            if (rc != KTX_SUCCESS) {
                error = std::string("transcode failed: ") + ktxErrorString(rc);
                ktxTexture_Destroy((ktxTexture*)tex);
                return nullptr;
            }
            const wgpu::TextureFormat format =
                tex->vkFormat == kVkFormatBc7Srgb
                    ? wgpu::TextureFormat::BC7RGBAUnormSrgb
                    : wgpu::TextureFormat::BC7RGBAUnorm;

            std::vector<Level> levels(tex->numLevels);
            for (uint32_t l = 0; l < tex->numLevels; ++l) {
                Level& level = levels[l];
                level.width = std::max(1u, tex->baseWidth >> l);
                level.height = std::max(1u, tex->baseHeight >> l);
                ktx_size_t offset = 0;
                ktxTexture_GetImageOffset((ktxTexture*)tex, l, 0, 0, &offset);
                const size_t levelBytes =
                    (size_t)bytesPerRow(format, level.width) *
                    rowCount(format, level.height);
                const uint8_t* src =
                    ktxTexture_GetData((ktxTexture*)tex) + offset;
                level.data.assign(src, src + levelBytes);
            }
            ktxTexture_Destroy((ktxTexture*)tex);
            return new ImageNode(std::move(levels), format);
        }

        // Raw (non-supercompressed) payloads: RGBA8 only, converted like any
        // other CPU-decoded source.
        if (tex->vkFormat != kVkFormatR8G8B8A8Unorm &&
            tex->vkFormat != kVkFormatR8G8B8A8Srgb) {
            error = "unsupported raw KTX2 payload format (want RGBA8 or Basis)";
            ktxTexture_Destroy((ktxTexture*)tex);
            return nullptr;
        }
        const bool srgb = tex->vkFormat == kVkFormatR8G8B8A8Srgb;
        const bool premultiplied = ktxTexture2_GetPremultipliedAlpha(tex);

        std::vector<Level> levels;
        if (tex->numLevels == 1) {
            // No authored chain: generate one like any CPU-decoded source.
            ktx_size_t offset = 0;
            ktxTexture_GetImageOffset((ktxTexture*)tex, 0, 0, 0, &offset);
            levels = buildMipChain<Level>(
                ktxTexture_GetData((ktxTexture*)tex) + offset, tex->baseWidth,
                tex->baseHeight, srgb, premultiplied);
        } else {
            levels.resize(tex->numLevels);
            for (uint32_t l = 0; l < tex->numLevels; ++l) {
                Level& level = levels[l];
                const uint32_t w = std::max(1u, tex->baseWidth >> l);
                const uint32_t h = std::max(1u, tex->baseHeight >> l);
                ktx_size_t offset = 0;
                ktxTexture_GetImageOffset((ktxTexture*)tex, l, 0, 0, &offset);
                packHalf(toLinearPremultiplied(
                             ktxTexture_GetData((ktxTexture*)tex) + offset, w,
                             h, srgb, premultiplied),
                         level.data, level.width, level.height);
            }
        }
        ktxTexture_Destroy((ktxTexture*)tex);
        return new ImageNode(std::move(levels),
                             wgpu::TextureFormat::RGBA16Float);
    }

    // WebP (§9.1): notably the only format here with lossy-plus-alpha.
    if (size >= 12 && std::memcmp(data, "RIFF", 4) == 0 &&
        std::memcmp(data + 8, "WEBP", 4) == 0) {
        int width = 0, height = 0;
        uint8_t* rgba = WebPDecodeRGBA(data, size, &width, &height);
        if (!rgba) {
            error = "WebP decode failed";
            return nullptr;
        }
        auto levels = buildMipChain<Level>(rgba, (uint32_t)width,
                                           (uint32_t)height, /*srgb=*/true,
                                           /*premultiplied=*/false);
        WebPFree(rgba);
        return new ImageNode(std::move(levels),
                             wgpu::TextureFormat::RGBA16Float);
    }

    // PNG/JPEG and friends via stb.
    int width = 0, height = 0, comp = 0;
    stbi_uc* rgba = stbi_load_from_memory((const stbi_uc*)data, (int)size,
                                          &width, &height, &comp, 4);
    if (!rgba) {
        error = stbi_failure_reason();
        return nullptr;
    }
    auto levels = buildMipChain<Level>(rgba, (uint32_t)width, (uint32_t)height,
                                       /*srgb=*/true, /*premultiplied=*/false);
    stbi_image_free(rgba);
    return new ImageNode(std::move(levels), wgpu::TextureFormat::RGBA16Float);
}

void ImageNode::evaluate(FrameContext& ctx)
{
    if (!mTexture) {
        wgpu::TextureDescriptor desc{};
        desc.format = mFormat;
        desc.size = { mWidth, mHeight, 1 };
        desc.mipLevelCount = (uint32_t)mLevels.size();
        desc.usage = wgpu::TextureUsage::TextureBinding |
                     wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::CopySrc;
        mTexture = ctx.device.CreateTexture(&desc);

        for (uint32_t l = 0; l < mLevels.size(); ++l) {
            const Level& level = mLevels[l];
            wgpu::TexelCopyTextureInfo dst{};
            dst.texture = mTexture;
            dst.mipLevel = l;
            wgpu::TexelCopyBufferLayout layout{};
            layout.bytesPerRow = bytesPerRow(mFormat, level.width);
            layout.rowsPerImage = rowCount(mFormat, level.height);
            wgpu::Extent3D extent = { level.width, level.height, 1 };
            ctx.device.GetQueue().WriteTexture(&dst, level.data.data(),
                                               level.data.size(), &layout,
                                               &extent);
        }
        mLevels = {};
    }

    Value out{};
    out.type = ValueType::Texture;
    out.texture = mTexture;
    out.texWidth = mWidth;
    out.texHeight = mHeight;
    writeOutput(outputs[0], out); // dirty on first evaluate only
}

// ---- VideoNode ----

#ifndef __EMSCRIPTEN__
namespace {

// NV12 -> linear RGB: y from the R8 plane, cb/cr from the RG88 plane
// (bilinear chroma upsampling via the sampler), matrix + range offsets in
// the uniforms, then sRGB decode to land in the linear texture contract.
const char* kYuvShader = R"(
struct YuvParams {
    rowR: vec4f,
    rowG: vec4f,
    rowB: vec4f,
}
@group(0) @binding(0) var<uniform> params: YuvParams;
@group(0) @binding(1) var planeY: texture_2d<f32>;
@group(0) @binding(2) var planeUV: texture_2d<f32>;
@group(0) @binding(3) var planeSampler: sampler;

fn srgbToLinear(c: f32) -> f32 {
    if (c <= 0.04045) {
        return c / 12.92;
    }
    return pow((c + 0.055) / 1.055, 2.4);
}

@fragment
fn yuv_fs(@location(0) uv: vec2f) -> @location(0) vec4f {
    let y = textureSample(planeY, planeSampler, uv).r;
    let c = textureSample(planeUV, planeSampler, uv).rg;
    let yuv1 = vec4f(y, c.r, c.g, 1.0);
    let rgb = clamp(vec3f(dot(params.rowR, yuv1), dot(params.rowG, yuv1),
                          dot(params.rowB, yuv1)),
                    vec3f(0.0), vec3f(1.0));
    return vec4f(srgbToLinear(rgb.r), srgbToLinear(rgb.g),
                 srgbToLinear(rgb.b), 1.0);
}
)";

// rgb = M * (y, cb, cr, 1): rows with range scaling and offsets baked in.
void yuvMatrix(bool bt709, bool fullRange, float rows[12])
{
    const float ys = fullRange ? 1.0f : 255.0f / 219.0f;
    const float y0 = fullRange ? 0.0f : 16.0f / 255.0f;
    const float cs = fullRange ? 1.0f : 255.0f / 224.0f;
    const float a = bt709 ? 1.5748f : 1.402f;     // Cr -> R
    const float b = bt709 ? -0.1873f : -0.344136f; // Cb -> G
    const float c = bt709 ? -0.4681f : -0.714136f; // Cr -> G
    const float d = bt709 ? 1.8556f : 1.772f;      // Cb -> B
    const float base = -ys * y0;
    const float r0[4] = { ys, 0.0f, a * cs, base - a * cs * 0.5f };
    const float r1[4] = { ys, b * cs, c * cs, base - (b + c) * cs * 0.5f };
    const float r2[4] = { ys, d * cs, 0.0f, base - d * cs * 0.5f };
    std::memcpy(rows + 0, r0, sizeof(r0));
    std::memcpy(rows + 4, r1, sizeof(r1));
    std::memcpy(rows + 8, r2, sizeof(r2));
}

} // namespace
#endif // !__EMSCRIPTEN__

VideoNode::VideoNode(std::unique_ptr<VideoDecoder> decoder)
    : mDecoder(std::move(decoder))
{
    outputs.resize(2); // 0=result (default), 1=finished (§17.4)
    outputs[0].value.type = ValueType::Texture;
    outputs[1].value.type = ValueType::Event;
}

#ifndef __EMSCRIPTEN__ // zero-copy import (see Nodes.h on the exception)

bool VideoNode::ensureConvertPipeline(FrameContext& ctx)
{
    if (mConvertPipeline) {
        return true;
    }
    const std::string source = std::string(kVertexPrelude) + kYuvShader;
    wgpu::ShaderModule module = makeModule(ctx.device, source);
    if (!module) {
        return false;
    }
    mConvertPipeline = makeFullscreenPipeline(ctx.device, module, "yuv_fs",
                                              wgpu::TextureFormat::RGBA16Float);
    if (!mConvertPipeline) {
        return false;
    }
    wgpu::BufferDescriptor bd{};
    bd.size = 48; // three vec4 rows
    bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    mConvertUniforms = ctx.device.CreateBuffer(&bd);
    mConvertSampler = makeLinearClampSampler(ctx.device);
    return true;
}

bool VideoNode::evaluateZeroCopy(FrameContext& ctx, const VideoFrame& frame)
{
    if (!ctx.device.HasFeature(wgpu::FeatureName::SharedTextureMemoryDmaBuf)) {
        return false; // this device can't import; fall back quietly
    }
    auto it = mSurfaces.find(frame.surfaceId);
    if (it == mSurfaces.end()) {
        // Capture import rejections (e.g. an unimportable modifier) in an
        // error scope: a failed probe means "fall back", not a GPU error.
        ctx.device.PushErrorScope(wgpu::ErrorFilter::Validation);
        ImportedSurface imported;
        bool ok = true;
        for (int i = 0; ok && i < 2; ++i) {
            const VideoPlane& plane = frame.planes[i];
            wgpu::SharedTextureMemoryDmaBufPlane dmaPlane{};
            dmaPlane.fd = plane.fd;
            dmaPlane.offset = plane.offset;
            dmaPlane.stride = plane.stride;
            wgpu::SharedTextureMemoryDmaBufDescriptor dma{};
            dma.size = { plane.width, plane.height, 1 };
            dma.drmFormat = plane.drmFormat;
            dma.drmModifier = plane.modifier;
            dma.planeCount = 1;
            dma.planes = &dmaPlane;
            wgpu::SharedTextureMemoryDescriptor desc{};
            desc.nextInChain = &dma;
            imported.memory[i] = ctx.device.ImportSharedTextureMemory(&desc);
            wgpu::SharedTextureMemoryProperties props{};
            imported.memory[i].GetProperties(&props);
            if (props.size.width == 0) {
                ok = false;
                break;
            }
            imported.texture[i] = imported.memory[i].CreateTexture();
        }
        // Validation is synchronous in Dawn native; a spontaneous callback
        // resolves during the Pop call. If it somehow doesn't, treat the
        // probe as failed rather than blocking the render thread.
        bool scopeDone = false;
        ctx.device.PopErrorScope(
            wgpu::CallbackMode::AllowSpontaneous,
            [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType type,
                wgpu::StringView) {
                ok = ok && type == wgpu::ErrorType::NoError;
                scopeDone = true;
            });
        if (!scopeDone || !ok) {
            return false; // caller falls back to CPU frames
        }
        it = mSurfaces.emplace(frame.surfaceId, std::move(imported)).first;
    }
    ImportedSurface& surface = it->second;

    if (!ensureConvertPipeline(ctx)) {
        return false;
    }
    if (!mConverted || mConvertedWidth != frame.width ||
        mConvertedHeight != frame.height) {
        wgpu::TextureDescriptor desc{};
        desc.format = wgpu::TextureFormat::RGBA16Float;
        desc.size = { frame.width, frame.height, 1 };
        desc.usage = wgpu::TextureUsage::RenderAttachment |
                     wgpu::TextureUsage::TextureBinding |
                     wgpu::TextureUsage::CopySrc;
        mConverted = ctx.device.CreateTexture(&desc);
        mConvertedWidth = frame.width;
        mConvertedHeight = frame.height;
    }

    float rows[12];
    yuvMatrix(frame.bt709, frame.fullRange, rows);
    ctx.device.GetQueue().WriteBuffer(mConvertUniforms, 0, rows, sizeof(rows));

    // Access-scope the external planes around the conversion pass. The
    // decoder synced the surface before export, so contents are ready.
    for (int i = 0; i < 2; ++i) {
        wgpu::SharedTextureMemoryVkImageLayoutBeginState vkBegin{};
        vkBegin.oldLayout = 1; // VK_IMAGE_LAYOUT_GENERAL
        vkBegin.newLayout = 1;
        wgpu::SharedTextureMemoryBeginAccessDescriptor ba{};
        ba.nextInChain = &vkBegin;
        ba.initialized = true;
        ba.concurrentRead = false;
        if (surface.memory[i].BeginAccess(surface.texture[i], &ba) !=
            wgpu::Status::Success) {
            return false;
        }
    }

    wgpu::BindGroupEntry entries[4] = {};
    entries[0].binding = 0;
    entries[0].buffer = mConvertUniforms;
    entries[0].size = 48;
    entries[1].binding = 1;
    entries[1].textureView = surface.texture[0].CreateView();
    entries[2].binding = 2;
    entries[2].textureView = surface.texture[1].CreateView();
    entries[3].binding = 3;
    entries[3].sampler = mConvertSampler;
    wgpu::BindGroupDescriptor bgDesc{};
    bgDesc.layout = mConvertPipeline.GetBindGroupLayout(0);
    bgDesc.entryCount = 4;
    bgDesc.entries = entries;
    wgpu::BindGroup group = ctx.device.CreateBindGroup(&bgDesc);

    wgpu::RenderPassColorAttachment attachment{};
    attachment.view = mConverted.CreateView();
    attachment.loadOp = wgpu::LoadOp::Clear;
    attachment.storeOp = wgpu::StoreOp::Store;
    attachment.clearValue = { 0.0, 0.0, 0.0, 1.0 };
    wgpu::RenderPassDescriptor desc{};
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &attachment;

    wgpu::CommandEncoder encoder = ctx.device.CreateCommandEncoder();
    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&desc);
    pass.SetPipeline(mConvertPipeline);
    pass.SetBindGroup(0, group);
    pass.Draw(3);
    pass.End();
    wgpu::CommandBuffer commands = encoder.Finish();
    ctx.device.GetQueue().Submit(1, &commands);

    for (int i = 0; i < 2; ++i) {
        wgpu::SharedTextureMemoryVkImageLayoutEndState vkEnd{};
        wgpu::SharedTextureMemoryEndAccessState end{};
        end.nextInChain = &vkEnd;
        surface.memory[i].EndAccess(surface.texture[i], &end);
    }

    Value out{};
    out.type = ValueType::Texture;
    out.texture = mConverted;
    out.texWidth = mConvertedWidth;
    out.texHeight = mConvertedHeight;
    outputs[0].value = out;
    outputs[0].dirty = true;
    return true;
}

#endif // !__EMSCRIPTEN__

void VideoNode::evaluate(FrameContext& ctx)
{
    // §9.2 playing: pause, not stop. While paused the playback clock
    // freezes (paused spans accumulate into mPausedTotal), the decoder is
    // never pulled — frameAt is what drives decode-ahead — and result stays
    // non-dirty. The first evaluate decodes even when paused, so a scene
    // that starts paused still shows a poster frame; a 'restart' fire does
    // the same (§9.2: poster, then playback waits on 'playing').
    const bool playing = inputs.empty() || inputValue(0).v[0] > 0.5;
    const bool restarted = inputs.size() > 1 && inputFired(1);
    if (restarted) {
        mDecoder->restart();
        mPausedTotal = ctx.seconds; // playback clock back to zero
        mFinished = false;
    } else if (!firstEvaluate && !playing) {
        mPausedTotal += ctx.seconds - mLastSeconds;
    }
    mLastSeconds = ctx.seconds;
    if (!playing && !firstEvaluate && !restarted) {
        return;
    }
    if (mFinished) {
        return; // §17.4: hold the final frame until a restart rearms
    }

    const VideoFrame* frame = mDecoder->frameAt(ctx.seconds - mPausedTotal);
    if (!frame) {
        return; // decode error; hold the previous frame
    }
    if (frame->last && outputs.size() > 1) {
        // §17.4 finished: fires on the frame the final decoded frame is
        // produced; never fires while looping (last is never set then).
        mFinished = true;
        outputs[1].dirty = true;
    }
    if (frame->index == mLastIndex) {
        return; // already uploaded/imported this frame
    }

#ifndef __EMSCRIPTEN__
    if (frame->planes.size() == 2) {
        if (evaluateZeroCopy(ctx, *frame)) {
            mLastIndex = frame->index;
        } else {
            // Import failed (or a stale zero-copy frame after fallback):
            // switch the decoder to CPU frames and drop this one.
            mSurfaces.clear();
            mDecoder->disableZeroCopy();
        }
        return;
    }
#endif

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

    constexpr double kDegToRad = kTau / 360.0;
    // GPU boundary: f64 value graph narrows to f32 uniforms here (§17.2).
    TransformUniforms u{};
    u.position[0] = (float)inputValue(1).v[0];
    u.position[1] = (float)inputValue(1).v[1];
    u.scale[0] = (float)inputValue(3).v[0];
    u.scale[1] = (float)inputValue(3).v[1];
    u.anchor[0] = (float)inputValue(4).v[0];
    u.anchor[1] = (float)inputValue(4).v[1];
    u.rotation = (float)(inputValue(2).v[0] * kDegToRad);
    u.opacity = (float)inputValue(5).v[0];
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

    // A texture input may not have produced yet (e.g. a video source whose
    // first frame fell back); run once it exists.
    for (size_t i = textureInputStart(); i < inputs.size(); ++i) {
        if (!inputValue(i).texture) {
            return;
        }
    }

    // Size: explicit, else first texture input, else the output target
    // (SCENE_FORMAT.md §9.3, §17.5). Feedback edges never drive sizing —
    // otherwise a shader reading its own previous frame locks onto its own
    // size and stops tracking output resizes.
    uint32_t width = mExplicitWidth, height = mExplicitHeight;
    if (width == 0) {
        for (size_t i = textureInputStart(); i < inputs.size(); ++i) {
            if (inputs[i].previous) {
                continue;
            }
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
            // GPU boundary: f64 value graph narrows to f32 uniforms (§17.2).
            const int n = componentCount(mInterface.fields[i].type);
            float f[4];
            for (int c = 0; c < n; ++c) {
                f[c] = (float)v.v[c];
            }
            std::memcpy(data.data() + mInterface.fields[i].offset, f,
                        n * sizeof(float));
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

// ---- ComputeNode (§18.2) ----

ComputeNode::ComputeNode(std::string source, WgslInterface iface,
                         std::vector<uint32_t> capacities,
                         std::array<uint32_t, 3> dispatch)
    : mSource(std::move(source))
    , mInterface(std::move(iface))
    , mCapacities(std::move(capacities))
    , mDispatch(dispatch)
{
    // Outputs: read_write buffers (in group/binding order), then storage
    // textures. Stride/capacity metadata is filled here, before any GPU
    // work, so buffer connections validate at load and history buffers can
    // allocate on the first frame.
    size_t rw = 0;
    for (const auto& buf : mInterface.storageBuffers) {
        if (!buf.readWrite) {
            continue;
        }
        Output out;
        out.name = buf.name;
        out.value.type = ValueType::Buffer;
        out.value.bufStride = buf.elementStride;
        out.value.bufCapacity = mCapacities[rw++];
        outputs.push_back(std::move(out));
    }
    for (const auto& tex : mInterface.storageTextures) {
        Output out;
        out.name = tex.name;
        out.value.type = ValueType::Texture;
        outputs.push_back(std::move(out));
    }
}

bool ComputeNode::ensurePipeline(FrameContext& ctx)
{
    if (mPipeline) {
        return true;
    }
    wgpu::ShaderModule module = makeModule(ctx.device, mSource);
    if (!module) {
        return false;
    }
    wgpu::ComputePipelineDescriptor desc{};
    desc.compute.module = module;
    desc.compute.entryPoint = "main";
    mPipeline = ctx.device.CreateComputePipeline(&desc);
    if (!mPipeline) {
        return false;
    }
    if (mInterface.hasUniforms) {
        wgpu::BufferDescriptor bd{};
        bd.size = mInterface.uniformSize;
        bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        mUniforms = ctx.device.CreateBuffer(&bd);
    }
    // Owned storage buffers: WebGPU zero-initializes, satisfying §18.1's
    // first-read rule. CopySrc feeds the §10 history copies.
    size_t rw = 0;
    for (const auto& buf : mInterface.storageBuffers) {
        if (!buf.readWrite) {
            continue;
        }
        wgpu::BufferDescriptor bd{};
        bd.size = (uint64_t)buf.elementStride * mCapacities[rw++];
        bd.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc;
        mOwned.push_back(ctx.device.CreateBuffer(&bd));
    }
    if (!mInterface.textures.empty()) {
        mSampler = makeLinearClampSampler(ctx.device);
    }
    return true;
}

void ComputeNode::evaluate(FrameContext& ctx)
{
    if (!ensurePipeline(ctx)) {
        return;
    }

    // Wait for inputs that have not produced yet (video first frame,
    // an upstream pipeline error) — the shader-node pattern.
    for (size_t i = textureInputStart(); i < bufferInputStart(); ++i) {
        if (!inputValue(i).texture) {
            return;
        }
    }
    for (size_t i = bufferInputStart(); i < inputs.size(); ++i) {
        if (!inputValue(i).buffer) {
            return;
        }
    }

    // Storage textures size like shader outputs (§9.3/§17.5): first sampled
    // texture input (feedback edges excluded), else the output target.
    if (!mInterface.storageTextures.empty()) {
        uint32_t width = 0, height = 0;
        for (size_t i = textureInputStart(); i < bufferInputStart(); ++i) {
            if (inputs[i].previous) {
                continue;
            }
            const Value v = inputValue(i);
            if (v.texWidth != 0) {
                width = v.texWidth;
                height = v.texHeight;
                break;
            }
        }
        if (width == 0) {
            width = ctx.targetWidth;
            height = ctx.targetHeight;
        }
        if (mStorageTex.empty() || width != mTexWidth || height != mTexHeight) {
            mStorageTex.clear();
            for (size_t i = 0; i < mInterface.storageTextures.size(); ++i) {
                wgpu::TextureDescriptor desc{};
                desc.format = wgpu::TextureFormat::RGBA16Float;
                desc.size = { width, height, 1 };
                desc.usage = wgpu::TextureUsage::StorageBinding |
                             wgpu::TextureUsage::TextureBinding |
                             wgpu::TextureUsage::CopySrc;
                mStorageTex.push_back(ctx.device.CreateTexture(&desc));
            }
            mTexWidth = width;
            mTexHeight = height;
        }
    }

    if (mInterface.hasUniforms) {
        std::vector<uint8_t> data(mInterface.uniformSize, 0);
        for (size_t i = 0; i < mInterface.fields.size(); ++i) {
            const Value v = inputValue(i);
            const int n = componentCount(mInterface.fields[i].type);
            float f[4];
            for (int c = 0; c < n; ++c) {
                f[c] = (float)v.v[c]; // §17.2 GPU boundary: f64 -> f32
            }
            std::memcpy(data.data() + mInterface.fields[i].offset, f,
                        n * sizeof(float));
        }
        ctx.device.GetQueue().WriteBuffer(mUniforms, 0, data.data(),
                                          data.size());
    }

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
    for (const auto& b : mInterface.storageBuffers) {
        maxGroup = std::max(maxGroup, b.group);
    }
    for (const auto& t : mInterface.storageTextures) {
        maxGroup = std::max(maxGroup, t.group);
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
            if (t.group == g) {
                wgpu::BindGroupEntry e{};
                e.binding = t.binding;
                e.textureView =
                    inputValue(textureInputStart() + i).texture.CreateView();
                entries.push_back(e);
            }
            if (t.hasSampler && t.samplerGroup == g) {
                wgpu::BindGroupEntry e{};
                e.binding = t.samplerBinding;
                e.sampler = mSampler;
                entries.push_back(e);
            }
        }
        size_t readIdx = 0, rwIdx = 0;
        for (const auto& b : mInterface.storageBuffers) {
            wgpu::Buffer buffer;
            uint64_t size = 0;
            if (b.readWrite) {
                buffer = mOwned[rwIdx];
                size = (uint64_t)b.elementStride *
                       outputs[rwIdx].value.bufCapacity;
                rwIdx++;
            } else {
                const Value v = inputValue(bufferInputStart() + readIdx++);
                buffer = v.buffer;
                size = (uint64_t)v.bufStride * v.bufCapacity;
            }
            if (b.group == g) {
                wgpu::BindGroupEntry e{};
                e.binding = b.binding;
                e.buffer = buffer;
                e.size = size;
                entries.push_back(e);
            }
        }
        for (size_t i = 0; i < mInterface.storageTextures.size(); ++i) {
            if (mInterface.storageTextures[i].group == g) {
                wgpu::BindGroupEntry e{};
                e.binding = mInterface.storageTextures[i].binding;
                e.textureView = mStorageTex[i].CreateView();
                entries.push_back(e);
            }
        }
        wgpu::BindGroupDescriptor desc{};
        desc.layout = mPipeline.GetBindGroupLayout(g);
        desc.entryCount = entries.size();
        desc.entries = entries.data();
        groups[g] = ctx.device.CreateBindGroup(&desc);
    }

    // Dispatch: explicit, else cover the primary output — the first owned
    // buffer's capacity along x, or the storage-texture extent in x/y.
    uint32_t dx = mDispatch[0], dy = mDispatch[1], dz = mDispatch[2];
    if (dx == 0) {
        const uint32_t* wg = mInterface.workgroupSize;
        if (!mOwned.empty()) {
            dx = (outputs[0].value.bufCapacity + wg[0] - 1) / wg[0];
            dy = 1;
            dz = 1;
        } else {
            dx = (mTexWidth + wg[0] - 1) / wg[0];
            dy = (mTexHeight + wg[1] - 1) / wg[1];
            dz = 1;
        }
    }

    wgpu::CommandEncoder encoder = ctx.device.CreateCommandEncoder();
    wgpu::ComputePassEncoder pass = encoder.BeginComputePass();
    pass.SetPipeline(mPipeline);
    for (uint32_t g = 0; g < groups.size(); ++g) {
        pass.SetBindGroup(g, groups[g]);
    }
    pass.DispatchWorkgroups(dx, dy, dz);
    pass.End();
    wgpu::CommandBuffer commands = encoder.Finish();
    ctx.device.GetQueue().Submit(1, &commands);

    // §18.1: buffer outputs are dirty whenever the node executes.
    size_t out = 0;
    for (size_t i = 0; i < mOwned.size(); ++i, ++out) {
        outputs[out].value.buffer = mOwned[i];
        outputs[out].dirty = true;
    }
    for (size_t i = 0; i < mStorageTex.size(); ++i, ++out) {
        outputs[out].value.texture = mStorageTex[i];
        outputs[out].value.texWidth = mTexWidth;
        outputs[out].value.texHeight = mTexHeight;
        outputs[out].dirty = true;
    }
}

// ---- ParticlesNode (§18.3) ----

// The built-in emission + simulation kernel. Uniform fields split into the
// wireable ports (matching ParticlesNode::Port names exactly) and the
// CPU-computed emission window (dt, emitStart/emitCount/seedBase,
// emitterKind). Reflection over this source is the uniform layout oracle —
// no hand-maintained offsets.
const char* kParticleSimWgsl = R"(
struct Particle {
    pos: vec3f, age: f32,
    vel: vec3f, lifetime: f32,
    color: vec4f,
    size: f32, rotation: f32, seed: f32, emitter: f32,
}

struct Params {
    gravity: vec2f, attractor: vec2f,
    collideTexel: vec2f,
    emitCountA: vec4f, emitCountB: vec4f,
    fadeIn: f32, sizeEnd: f32, drag: f32, turbulence: f32,
    turbulenceScale: f32, attract: f32, vortex: f32, ring: f32,
    bounce: f32, dt: f32, emitStart: f32, seedBase: f32,
    spawnCount: f32, inherit: f32,
}

// §18.5.3: one block per emitter; emission-time quantities come from the
// particle's emitter, forces and life-curve shaping from params. Kept as a
// storage block (the reflection oracle allows one uniform binding) and
// hand-packed on the CPU — kEmitterStride must match this layout.
struct EmitterP {
    origin: vec2f, extent: vec2f,
    direction: vec2f, speed: vec2f,
    lifetime: vec2f, size: vec2f,
    spin: vec2f, depth: vec2f,
    colorStart: vec4f, colorEnd: vec4f,
    spread: f32, kind: f32, pad0: f32, pad1: f32,
}

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> prev: array<Particle>;
@group(0) @binding(2) var<storage, read_write> state: array<Particle>;
@group(0) @binding(3) var<storage, read> emitters: array<EmitterP>;
//COLLIDE_BINDINGS
//SPAWN_BINDINGS

fn pcg(v: u32) -> u32 {
    var x = v * 747796405u + 2891336453u;
    x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    return (x >> 22u) ^ x;
}

// Per-particle random streams keyed on the emission ordinal, so any
// emission-time draw is recomputable in later frames from p.seed.
fn rnd(n: u32, k: u32) -> f32 {
    return f32(pcg(n ^ (k * 2654435769u))) / 4294967296.0;
}

// Life curves: premultiplied tint (§18.3 contract) with the fade-in ramp.
fn shade(cs: vec4f, ce: vec4f, age: f32, lifetime: f32) -> vec4f {
    let t = age / lifetime;
    let c = mix(cs, ce, t);
    var a = c.a;
    if (params.fadeIn > 0.0) {
        a = a * clamp(age / params.fadeIn, 0.0, 1.0);
    }
    return vec4f(c.rgb * a, a);
}

// This frame's emission count for emitter j (§18.5.3: the CPU splits the
// window across emitters; their segments concatenate in index order).
fn emitCountAt(j: u32) -> u32 {
    if (j < 4u) {
        return u32(params.emitCountA[j]);
    }
    return u32(params.emitCountB[j - 4u]);
}

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) id: vec3u) {
    let i = id.x;
    let cap = arrayLength(&state);
    if (i >= cap) {
        return;
    }

    //EMISSION

    var p = prev[i];
    if (p.lifetime == 0.0) {
        state[i] = p;
        return;
    }
    p.age += params.dt;
    if (p.age >= p.lifetime) {
        p.lifetime = 0.0; // dead slot (§18.3): renderers skip it
        state[i] = p;
        return;
    }

    let n = u32(p.seed);
    var v = p.vel.xy;
    v += params.gravity * params.dt;
    v *= max(0.0, 1.0 - params.drag * params.dt);
    if (params.turbulence != 0.0) {
        let q = p.pos.xy * params.turbulenceScale;
        let phase = rnd(n, 8u) * 6.2831853;
        v += params.turbulence *
             vec2f(sin(q.y * 6.2831853 + p.age + phase),
                   cos(q.x * 6.2831853 - p.age + phase)) * params.dt;
    }
    if (params.attract != 0.0 || params.vortex != 0.0) {
        let d = params.attractor - p.pos.xy;
        let len = sqrt(max(dot(d, d), 1e-6));
        // §18.5.2 ring: attraction targets the nearest ring point (d scaled
        // by 1 - r/|d| flips outward inside the ring, pulling onto the
        // circle); vortex stays around the center — the circumferential
        // flow. ring = 0 degenerates to §18.3 exactly.
        let da = d * (1.0 - params.ring / len);
        let la = sqrt(max(dot(da, da), 1e-6));
        v += (da / la) * params.attract * params.dt;
        v += (vec2f(-d.y, d.x) / len) * params.vortex * params.dt;
    }
    p.vel = vec3f(v, 0.0);
    p.pos += p.vel * params.dt;
    //COLLIDE

    let t = p.age / p.lifetime;
    // Emission-time draws are recomputable from the stored ordinal; the
    // ranges come from the particle's own emitter (§18.5.3).
    let E = emitters[min(u32(p.emitter), 7u)];
    let emitSize = mix(E.size.x, E.size.y, rnd(n, 6u));
    p.size = emitSize * mix(1.0, params.sizeEnd, t);
    p.rotation += mix(E.spin.x, E.spin.y, rnd(n, 7u)) * params.dt;
    p.color = shade(E.colorStart, E.colorEnd, p.age, p.lifetime);
    state[i] = p;
}
)";

// §18.3 ring-window emission: [emitStart, emitStart + total) mod cap. The
// CPU advances the cursor, so the window is deterministic and the oldest
// slots recycle when the pool saturates.
const char* kParticleEmitRing = R"(
    var total = 0u;
    for (var j = 0u; j < 8u; j++) {
        total += emitCountAt(j);
    }
    let rel = (i + cap - (u32(params.emitStart) % cap)) % cap;
    if (total > 0u && rel < total) {
        var em = 0u; // which emitter's segment this slot falls in (§18.5.3)
        var rem = rel;
        for (var j = 0u; j < 8u; j++) {
            let cj = emitCountAt(j);
            if (rem < cj) {
                em = j;
                break;
            }
            rem -= cj;
        }
        let E = emitters[em];
        let n = u32(params.seedBase) + rel;
        var p: Particle;
        var origin = E.origin;
        let kind = u32(E.kind);
        if (kind == 1u) { // box
            origin += (vec2f(rnd(n, 1u), rnd(n, 2u)) * 2.0 - 1.0) * E.extent;
        } else if (kind == 2u) { // disc, area-uniform
            let ang = rnd(n, 1u) * 6.2831853;
            origin += sqrt(rnd(n, 2u)) * E.extent.x * vec2f(cos(ang), sin(ang));
        }
        let base = atan2(E.direction.y, E.direction.x);
        let theta = base + radians(E.spread) * (rnd(n, 3u) - 0.5);
        let speed = mix(E.speed.x, E.speed.y, rnd(n, 4u));
        // §18.5.2: z is sampled at emission and held over life.
        let z = mix(E.depth.x, E.depth.y, rnd(n, 10u));
        p.pos = vec3f(origin, z);
        p.vel = vec3f(cos(theta) * speed, sin(theta) * speed, 0.0);
        p.age = 0.0;
        p.lifetime = max(mix(E.lifetime.x, E.lifetime.y, rnd(n, 5u)), 0.001);
        p.size = mix(E.size.x, E.size.y, rnd(n, 6u));
        p.rotation = 360.0 * rnd(n, 9u);
        p.seed = f32(n % 16777216u); // f32-exact emission ordinal
        p.emitter = f32(em);
        p.color = shade(E.colorStart, E.colorEnd, 0.0, p.lifetime);
        state[i] = p;
        return;
    }
)";

// §18.5.4 sub-emitter permutation: emission sites are deaths in the spawn
// source this tick (alive in the previous copy, dead now). Source slot s
// statically owns child slots [s*spawnCount, (s+1)*spawnCount), so slot
// assignment needs no atomics and a redeath of a recycled parent re-emits
// over its own (stalest-by-construction) brood.
const char* kParticleSpawnBindings = R"(
@group(0) @binding(6) var<storage, read> spawn: array<Particle>;
@group(0) @binding(7) var<storage, read> spawnPrev: array<Particle>;
)";

const char* kParticleEmitSpawn = R"(
    let sc = max(u32(params.spawnCount), 1u);
    let ps = i / sc;
    if (ps < arrayLength(&spawn)) {
        let parent = spawn[ps];
        if (spawnPrev[ps].lifetime > 0.0 && parent.lifetime == 0.0) {
            // Randoms hash the parent's emission ordinal (its seed), so
            // every life of a recycled parent slot gets a fresh stream —
            // folded to 24 bits up front so the stored seed replays the
            // same stream in later frames.
            let n = ((u32(parent.seed) * sc + (i % sc)) ^
                     (ps * 2654435761u)) % 16777216u;
            let E = emitters[0];
            var p: Particle;
            let base = atan2(E.direction.y, E.direction.x);
            let theta = base + radians(E.spread) * (rnd(n, 3u) - 0.5);
            let speed = mix(E.speed.x, E.speed.y, rnd(n, 4u));
            // Children start where the parent ended (z included) and
            // inherit a fraction of its final velocity.
            p.pos = parent.pos;
            p.vel = vec3f(cos(theta) * speed, sin(theta) * speed, 0.0) +
                    vec3f(parent.vel.xy * params.inherit, 0.0);
            p.age = 0.0;
            p.lifetime = max(mix(E.lifetime.x, E.lifetime.y, rnd(n, 5u)),
                             0.001);
            p.size = mix(E.size.x, E.size.y, rnd(n, 6u));
            p.rotation = 360.0 * rnd(n, 9u);
            p.seed = f32(n);
            p.emitter = 0.0;
            p.color = shade(E.colorStart, E.colorEnd, 0.0, p.lifetime);
            state[i] = p;
            return;
        }
    }
)";

// §18.5.2 collision permutation, spliced in when the collide port is bound.
// The mask samples in output-space UV; alpha >= 0.5 is solid. Compute passes
// have no implicit derivatives, so gradients come from central differences a
// texel and a half apart.
const char* kParticleCollideBindings = R"(
@group(0) @binding(4) var collideTex: texture_2d<f32>;
@group(0) @binding(5) var collideSamp: sampler;
)";

const char* kParticleCollideCode = R"(
    // Sample along this frame's motion so fast particles cannot tunnel
    // through thin (or thick) solids: the first solid sample is the hit.
    let cnew = p.pos.xy;
    let cold = cnew - p.vel.xy * params.dt;
    var hit = 0.0;
    for (var s = 1u; s <= 4u; s++) {
        let f = f32(s) * 0.25;
        if (textureSampleLevel(collideTex, collideSamp, mix(cold, cnew, f),
                               0.0).a >= 0.5) {
            hit = f;
            break;
        }
    }
    if (hit > 0.0) {
        let cuv = mix(cold, cnew, hit);
        let e = params.collideTexel * 1.5;
        let gx = textureSampleLevel(collideTex, collideSamp,
                                    cuv + vec2f(e.x, 0.0), 0.0).a -
                 textureSampleLevel(collideTex, collideSamp,
                                    cuv - vec2f(e.x, 0.0), 0.0).a;
        let gy = textureSampleLevel(collideTex, collideSamp,
                                    cuv + vec2f(0.0, e.y), 0.0).a -
                 textureSampleLevel(collideTex, collideSamp,
                                    cuv - vec2f(0.0, e.y), 0.0).a;
        var nrm = vec2f(-gx, -gy); // toward falling alpha: out of the solid
        let nl = sqrt(dot(nrm, nrm));
        if (nl > 1e-5) {
            nrm /= nl;
        } else {
            // Deep inside (flat alpha): send it back the way it came.
            nrm = -p.vel.xy / max(sqrt(dot(p.vel.xy, p.vel.xy)), 1e-6);
        }
        var q = mix(cold, cnew, hit - 0.25); // last pre-hit point
        for (var s = 0u; s < 8u; s++) {      // and out of any overlap
            if (textureSampleLevel(collideTex, collideSamp, q, 0.0).a <
                0.5) { break; }
            q += nrm * e;
        }
        p.pos = vec3f(q, p.pos.z);
        let vn = dot(p.vel.xy, nrm);
        if (vn < 0.0) { // restitution: v' = v - (1 + bounce)(v.n)n
            p.vel = vec3f(p.vel.xy - (1.0 + params.bounce) * vn * nrm, 0.0);
        }
    }
)";

ParticlesNode::ParticlesNode(uint32_t capacity, std::vector<Emitter> emitters,
                             std::vector<uint32_t> overrideMasks,
                             uint32_t spawnCount)
    : mCapacity(capacity), mEmitters(std::move(emitters)),
      mMasks(std::move(overrideMasks)), mSpawnCount(spawnCount)
{
    std::string err;
    WgslInterface::parse(kParticleSimWgsl, mIface, err); // own source: cannot fail

    outputs.resize(1);
    outputs[0].name = "result";
    outputs[0].value.type = ValueType::Buffer;
    outputs[0].value.bufStride = kStride;
    outputs[0].value.bufCapacity = capacity;
}

// §18.5.3 fallback: an emitter field that the document did not set on the
// entry reads the node-level port of the same name.
Value ParticlesNode::emitterValue(size_t e, EmitterField f) const
{
    if (mMasks[e] & (1u << f)) {
        return inputValue(PortCount + e * EfCount + f);
    }
    static constexpr Port kBase[EfCount] = {
        PortOrigin, PortExtent, PortDirection, PortSpread, PortSpeed,
        PortLifetime, PortSize, PortSpin, PortColorStart, PortColorEnd,
        PortDepth, PortDelay, PortDuration, PortCount /* weight: below */,
    };
    if (f == EfWeight) {
        Value v;
        v.v[0] = 1.0;
        return v;
    }
    return inputValue(kBase[f]);
}

bool ParticlesNode::ensurePipeline(FrameContext& ctx)
{
    // Bound-ness of the collide mask is static after load.
    mCollide = inputs[PortCollide].srcNode != nullptr;
    if (mPipeline) {
        return true;
    }
    std::string source = kParticleSimWgsl;
    const auto replace = [&](const char* tag, const char* text) {
        source.replace(source.find(tag), std::strlen(tag), text);
    };
    replace("//COLLIDE_BINDINGS", mCollide ? kParticleCollideBindings : "");
    replace("//SPAWN_BINDINGS", mSpawnCount ? kParticleSpawnBindings : "");
    replace("//EMISSION", mSpawnCount ? kParticleEmitSpawn : kParticleEmitRing);
    replace("//COLLIDE", mCollide ? kParticleCollideCode : "");
    wgpu::ShaderModule module = makeModule(ctx.device, source);
    if (!module) {
        return false;
    }
    wgpu::ComputePipelineDescriptor desc{};
    desc.compute.module = module;
    desc.compute.entryPoint = "main";
    mPipeline = ctx.device.CreateComputePipeline(&desc);

    wgpu::BufferDescriptor ud{};
    ud.size = mIface.uniformSize;
    ud.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    mUniforms = ctx.device.CreateBuffer(&ud);
    wgpu::BufferDescriptor ed{};
    ed.size = (uint64_t)kEmitterStride * kMaxEmitters;
    ed.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
    mEmitterUniforms = ctx.device.CreateBuffer(&ed);
    if (mCollide) {
        mSampler = makeLinearClampSampler(ctx.device);
    }

    for (int i = 0; i < 2; i++) {
        wgpu::BufferDescriptor bd{};
        bd.size = (uint64_t)kStride * mCapacity;
        bd.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc;
        mState[i] = ctx.device.CreateBuffer(&bd); // zero-initialized: all dead
    }
    return mPipeline != nullptr;
}

void ParticlesNode::evaluate(FrameContext& ctx)
{
    if (!ensurePipeline(ctx)) {
        return;
    }
    if (mCollide && !inputValue(PortCollide).texture) {
        return; // the mask's producer has not run yet
    }
    if (mSpawnCount &&
        (!inputValue(PortSpawn).buffer || !inputValue(PortSpawnPrev).buffer)) {
        return; // the spawn source (or its first history copy) isn't there yet
    }

    // Deterministic emission accounting: rate accumulates fractionally,
    // bursts land whole, and the ring cursor maps the window onto slots.
    const double dt = std::max(0.0, inputValue(PortTime).v[0]);
    const size_t emitterCount = mEmitters.size();
    // §18.5.2/§18.5.3: per-emitter windows on the accumulated sim clock
    // (not @time.seconds), and rate/burst shares proportional to weight.
    // A closed emitter's weight is zero and its accumulator holds at zero,
    // so an opening window doesn't dump the closed period's backlog.
    double weights[kMaxEmitters] = {};
    double weightSum = 0.0;
    for (size_t e = 0; e < emitterCount; ++e) {
        const double delay = std::max(0.0, emitterValue(e, EfDelay).v[0]);
        const double duration = emitterValue(e, EfDuration).v[0];
        const bool open = mSimTime >= delay &&
            (duration <= 0.0 || mSimTime < delay + duration);
        weights[e] =
            open ? std::max(0.0, emitterValue(e, EfWeight).v[0]) : 0.0;
        weightSum += weights[e];
    }
    mSimTime += dt;

    uint32_t counts[kMaxEmitters] = {};
    uint32_t emit = 0;
    if (mSpawnCount) {
        // §18.5.4: emission is death-driven on the GPU; the ring window
        // stays empty and the cursor never moves.
        weightSum = 0.0;
    }
    if (weightSum > 0.0) {
        const double rate = std::max(0.0, inputValue(PortRate).v[0]);
        const uint32_t burstTotal = inputFired(PortBurst)
            ? (uint32_t)std::max(0.0, inputValue(PortBurstCount).v[0])
            : 0;
        // Burst split: floor shares now, the remainder to the largest
        // fractions (ties to the lowest index) — deterministic.
        double shareFrac[kMaxEmitters] = {};
        uint32_t burstGiven = 0;
        for (size_t e = 0; e < emitterCount; ++e) {
            const double w = weights[e] / weightSum;
            mAccum[e] += rate * dt * w;
            counts[e] = (uint32_t)mAccum[e];
            mAccum[e] -= counts[e];
            const double share = burstTotal * w;
            const uint32_t base = (uint32_t)share;
            shareFrac[e] = share - base;
            counts[e] += base;
            burstGiven += base;
        }
        for (uint32_t r = burstGiven; r < burstTotal; ++r) {
            size_t best = 0;
            for (size_t e = 1; e < emitterCount; ++e) {
                if (shareFrac[e] > shareFrac[best]) {
                    best = e;
                }
            }
            counts[best]++;
            shareFrac[best] = -1.0;
        }
        for (size_t e = 0; e < emitterCount; ++e) {
            // Clamp the concatenated window to the pool; later emitters
            // lose first, deterministically.
            counts[e] = std::min(counts[e], mCapacity - emit);
            emit += counts[e];
        }
    } else {
        for (size_t e = 0; e < emitterCount; ++e) {
            mAccum[e] = 0.0;
        }
    }
    const uint32_t emitStart = mCursor;
    mCursor = (mCursor + emit) % mCapacity;

    std::vector<uint8_t> data(mIface.uniformSize, 0);
    for (const auto& field : mIface.fields) {
        Value v;
        if (field.name == "dt") {
            v.v[0] = dt;
        } else if (field.name == "emitStart") {
            v.v[0] = emitStart;
        } else if (field.name == "emitCountA") {
            for (int c = 0; c < 4; ++c) {
                v.v[c] = counts[c];
            }
        } else if (field.name == "emitCountB") {
            for (int c = 0; c < 4; ++c) {
                v.v[c] = counts[4 + c];
            }
        } else if (field.name == "seedBase") {
            v.v[0] = std::fmod(mEmitted, 16777216.0); // stays f32-exact
        } else if (field.name == "collideTexel") {
            const Value mask = inputValue(PortCollide);
            if (mask.texWidth && mask.texHeight) {
                v.v[0] = 1.0 / mask.texWidth;
                v.v[1] = 1.0 / mask.texHeight;
            }
        } else if (field.name == "bounce") {
            v.v[0] = inputValue(PortBounce).v[0];
        } else if (field.name == "spawnCount") {
            v.v[0] = mSpawnCount;
        } else if (field.name == "inherit") {
            v.v[0] = inputValue(PortInherit).v[0];
        } else {
            // The kernel's port fields are named exactly like the input
            // ports; Port enum order matches the loader's table.
            static const std::map<std::string, Port> kByName = {
                { "gravity", PortGravity }, { "attractor", PortAttractor },
                { "fadeIn", PortFadeIn }, { "sizeEnd", PortSizeEnd },
                { "drag", PortDrag }, { "turbulence", PortTurbulence },
                { "turbulenceScale", PortTurbulenceScale },
                { "attract", PortAttract }, { "vortex", PortVortex },
                { "ring", PortRing },
            };
            v = inputValue(kByName.at(field.name));
        }
        const int n = componentCount(field.type);
        float f[4];
        for (int c = 0; c < n; ++c) {
            f[c] = (float)v.v[c];
        }
        std::memcpy(data.data() + field.offset, f, n * sizeof(float));
    }
    mEmitted += emit;
    ctx.device.GetQueue().WriteBuffer(mUniforms, 0, data.data(), data.size());

    // §18.5.3 per-emitter blocks, hand-packed to EmitterP's WGSL layout
    // (the struct is fixed; kEmitterStride pins the stride).
    uint8_t eblock[kEmitterStride * kMaxEmitters] = {};
    const auto packField = [&](size_t e, uint32_t off, const Value& val,
                               int n) {
        float f[4];
        for (int c = 0; c < n; ++c) {
            f[c] = (float)val.v[c];
        }
        std::memcpy(eblock + e * kEmitterStride + off, f, n * sizeof(float));
    };
    for (size_t e = 0; e < emitterCount; ++e) {
        packField(e, 0, emitterValue(e, EfOrigin), 2);
        packField(e, 8, emitterValue(e, EfExtent), 2);
        packField(e, 16, emitterValue(e, EfDirection), 2);
        packField(e, 24, emitterValue(e, EfSpeed), 2);
        packField(e, 32, emitterValue(e, EfLifetime), 2);
        packField(e, 40, emitterValue(e, EfSize), 2);
        packField(e, 48, emitterValue(e, EfSpin), 2);
        packField(e, 56, emitterValue(e, EfDepth), 2);
        packField(e, 64, emitterValue(e, EfColorStart), 4);
        packField(e, 80, emitterValue(e, EfColorEnd), 4);
        packField(e, 96, emitterValue(e, EfSpread), 1);
        Value kind;
        kind.v[0] = (double)(int)mEmitters[e];
        packField(e, 100, kind, 1);
    }
    ctx.device.GetQueue().WriteBuffer(mEmitterUniforms, 0, eblock,
                                      sizeof(eblock));

    const int back = 1 - mFront;
    std::vector<wgpu::BindGroupEntry> entries(4);
    entries[0].binding = 0;
    entries[0].buffer = mUniforms;
    entries[0].size = mIface.uniformSize;
    entries[1].binding = 1;
    entries[1].buffer = mState[mFront];
    entries[1].size = (uint64_t)kStride * mCapacity;
    entries[2].binding = 2;
    entries[2].buffer = mState[back];
    entries[2].size = (uint64_t)kStride * mCapacity;
    entries[3].binding = 3;
    entries[3].buffer = mEmitterUniforms;
    entries[3].size = (uint64_t)kEmitterStride * kMaxEmitters;
    if (mCollide) {
        wgpu::BindGroupEntry t{};
        t.binding = 4;
        t.textureView = inputValue(PortCollide).texture.CreateView();
        entries.push_back(t);
        wgpu::BindGroupEntry s{};
        s.binding = 5;
        s.sampler = mSampler;
        entries.push_back(s);
    }
    if (mSpawnCount) {
        const Value cur = inputValue(PortSpawn);
        const Value prev = inputValue(PortSpawnPrev);
        wgpu::BindGroupEntry c{};
        c.binding = 6;
        c.buffer = cur.buffer;
        c.size = (uint64_t)cur.bufStride * cur.bufCapacity;
        entries.push_back(c);
        wgpu::BindGroupEntry h{};
        h.binding = 7;
        h.buffer = prev.buffer;
        h.size = (uint64_t)prev.bufStride * prev.bufCapacity;
        entries.push_back(h);
    }
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = mPipeline.GetBindGroupLayout(0);
    bgd.entryCount = entries.size();
    bgd.entries = entries.data();
    wgpu::BindGroup group = ctx.device.CreateBindGroup(&bgd);

    wgpu::CommandEncoder encoder = ctx.device.CreateCommandEncoder();
    wgpu::ComputePassEncoder pass = encoder.BeginComputePass();
    pass.SetPipeline(mPipeline);
    pass.SetBindGroup(0, group);
    pass.DispatchWorkgroups((mCapacity + 63) / 64, 1, 1);
    pass.End();
    wgpu::CommandBuffer commands = encoder.Finish();
    ctx.device.GetQueue().Submit(1, &commands);

    mFront = back;
    outputs[0].value.buffer = mState[mFront];
    outputs[0].dirty = true; // §18.1: dirty whenever the node executes
}

// ---- SpritesNode (§18.3, §18.5.5) ----

// Draw-list preparation: one serial invocation compacts live slots into
// `indices` (slot order — deterministic) and sizes the indirect draw. The
// padded tail gets a sentinel that the sort network sinks to the end.
const char* kSpritesPrepareWgsl = R"(
struct Particle {
    pos: vec3f, age: f32,
    vel: vec3f, lifetime: f32,
    color: vec4f,
    size: f32, rotation: f32, seed: f32, emitter: f32,
}

@group(0) @binding(0) var<storage, read> pts: array<Particle>;
@group(0) @binding(1) var<storage, read_write> indices: array<u32>;
@group(0) @binding(2) var<storage, read_write> indirect: array<u32>;

@compute @workgroup_size(1)
fn main() {
    let cap = arrayLength(&pts);
    var n = 0u;
    for (var i = 0u; i < cap; i++) {
        if (pts[i].lifetime != 0.0) {
            indices[n] = i;
            n++;
        }
    }
    let padded = arrayLength(&indices);
    for (var i = n; i < padded; i++) {
        indices[i] = 0xffffffffu;
    }
    indirect[0] = 4u; // vertexCount
    indirect[1] = n;  // instanceCount: the live count (§18.5.5)
    indirect[2] = 0u;
    indirect[3] = 0u;
}
)";

// One bitonic stage over the index list. The comparison is a total order
// (deadness, z descending, emission ordinal ascending, slot ascending), so
// the network's output is deterministic regardless of thread scheduling.
const char* kSpritesSortWgsl = R"(
struct Particle {
    pos: vec3f, age: f32,
    vel: vec3f, lifetime: f32,
    color: vec4f,
    size: f32, rotation: f32, seed: f32, emitter: f32,
}

struct SortParams { j: u32, k: u32 }

@group(0) @binding(0) var<storage, read> pts: array<Particle>;
@group(0) @binding(1) var<storage, read_write> indices: array<u32>;
@group(0) @binding(2) var<uniform> params: SortParams;

fn before(a: u32, b: u32) -> bool {
    // Draw order for 'over' (§18.5.5): far first, then oldest.
    if (a == 0xffffffffu) { return false; }
    if (b == 0xffffffffu) { return true; }
    let pa = pts[a];
    let pb = pts[b];
    if (pa.pos.z != pb.pos.z) { return pa.pos.z > pb.pos.z; }
    if (pa.seed != pb.seed) { return pa.seed < pb.seed; }
    return a < b;
}

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) id: vec3u) {
    let i = id.x;
    let l = i ^ params.j;
    if (l <= i || l >= arrayLength(&indices)) {
        return;
    }
    let a = indices[i];
    let b = indices[l];
    if (((i & params.k) == 0u) != before(a, b)) {
        indices[i] = b;
        indices[l] = a;
    }
}
)";

// Instanced quads from the Particle contract, driven by the prepared index
// list: 4-vertex strip per live instance. DISC/TEXTURED selects the
// fragment via string replacement at pipeline build.
const char* kSpritesWgsl = R"(
struct Particle {
    pos: vec3f, age: f32,
    vel: vec3f, lifetime: f32,
    color: vec4f,
    size: f32, rotation: f32, seed: f32, emitter: f32,
}

// misc = (aspect, frameRate, sheetCols, sheetRows); par = (parallax.xy,-,-)
struct Params { misc: vec4f, par: vec4f }

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> pts: array<Particle>;
@group(0) @binding(2) var<storage, read> indices: array<u32>;
//TEXTURE_BINDINGS

fn pcg(v: u32) -> u32 {
    var x = v * 747796405u + 2891336453u;
    x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    return (x >> 22u) ^ x;
}

fn rnd(n: u32, k: u32) -> f32 {
    return f32(pcg(n ^ (k * 2654435769u))) / 4294967296.0;
}

struct VsOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
    @location(1) color: vec4f,
}

@vertex
fn drift_sprite_vs(@builtin(vertex_index) v: u32,
                   @builtin(instance_index) i: u32) -> VsOut {
    var out: VsOut;
    let slot = indices[i];
    if (slot == 0xffffffffu) {
        out.pos = vec4f(0.0, 0.0, 0.0, 1.0); // degenerate: nothing rastered
        return out;
    }
    let p = pts[slot];
    let corner = vec2f(f32(v & 1u), f32((v >> 1u) & 1u)) * 2.0 - 1.0;
    let c = cos(radians(p.rotation));
    let s = sin(radians(p.rotation));
    // size is a fraction of output height (§18.3); x corrects for aspect.
    let r = vec2f(c * corner.x - s * corner.y, s * corner.x + c * corner.y) *
            p.size;
    // §18.5.5 parallax: a per-particle offset linear in z.
    let pos = p.pos.xy + vec2f(r.x / params.misc.x, r.y) +
              params.par.xy * p.pos.z;
    out.pos = vec4f(pos.x * 2.0 - 1.0, 1.0 - pos.y * 2.0, 0.0, 1.0);
    var uv = corner * 0.5 + 0.5;
    let cols = params.misc.z;
    let rows = params.misc.w;
    if (cols > 0.0) {
        // §18.5.5 spritesheet: frameRate 0 plays the sheet once over the
        // particle's lifetime; > 0 loops at that rate from a per-particle
        // random start frame.
        let frames = cols * rows;
        var fi: f32;
        if (params.misc.y <= 0.0) {
            fi = floor(clamp(p.age / p.lifetime, 0.0, 0.99999) * frames);
        } else {
            let start = floor(rnd(u32(p.seed), 11u) * frames);
            fi = floor(start + p.age * params.misc.y);
            fi = fi - floor(fi / frames) * frames;
        }
        uv = (vec2f(fi - floor(fi / cols) * cols, floor(fi / cols)) + uv) /
             vec2f(cols, rows);
    }
    out.uv = uv;
    out.color = p.color;
    return out;
}

@fragment
fn drift_sprite_fs(@location(0) uv: vec2f,
                   @location(1) color: vec4f) -> @location(0) vec4f {
    //FRAGMENT
}
)";

const char* kSpriteDiscFragment = R"(
    let d2 = dot(uv * 2.0 - 1.0, uv * 2.0 - 1.0);
    let a = max(0.0, 1.0 - d2);
    return color * a * a;
)";

const char* kSpriteTexturedFragment = R"(
    return textureSample(sprite, sprite_sampler, uv) * color;
)";

const char* kSpriteTextureBindings = R"(
@group(0) @binding(3) var sprite: texture_2d<f32>;
@group(0) @binding(4) var sprite_sampler: sampler;
)";

SpritesNode::SpritesNode(Blend blend, uint32_t sheetCols, uint32_t sheetRows)
    : mBlend(blend), mSheetCols(sheetCols), mSheetRows(sheetRows)
{
    outputs.resize(1);
    outputs[0].name = "result";
    outputs[0].value.type = ValueType::Texture;
}

bool SpritesNode::ensurePipeline(FrameContext& ctx)
{
    // Bound-ness of the sprite texture is static after load.
    mTextured = inputs.size() > PortTexture &&
                inputs[PortTexture].srcNode != nullptr;
    if (mPipeline) {
        return true;
    }
    std::string source = kSpritesWgsl;
    const auto replace = [&](const char* tag, const char* text) {
        source.replace(source.find(tag), std::strlen(tag), text);
    };
    replace("//TEXTURE_BINDINGS", mTextured ? kSpriteTextureBindings : "");
    replace("//FRAGMENT",
            mTextured ? kSpriteTexturedFragment : kSpriteDiscFragment);
    wgpu::ShaderModule module = makeModule(ctx.device, source);
    if (!module) {
        return false;
    }

    wgpu::BlendState blend{};
    blend.color.srcFactor = wgpu::BlendFactor::One;
    blend.color.dstFactor = mBlend == Blend::Add
        ? wgpu::BlendFactor::One
        : wgpu::BlendFactor::OneMinusSrcAlpha;
    blend.alpha = blend.color;
    wgpu::ColorTargetState color{};
    color.format = wgpu::TextureFormat::RGBA16Float;
    color.blend = &blend;

    wgpu::FragmentState fragment{};
    fragment.module = module;
    fragment.entryPoint = "drift_sprite_fs";
    fragment.targetCount = 1;
    fragment.targets = &color;

    wgpu::RenderPipelineDescriptor desc{};
    desc.vertex.module = module;
    desc.vertex.entryPoint = "drift_sprite_vs";
    desc.primitive.topology = wgpu::PrimitiveTopology::TriangleStrip;
    desc.fragment = &fragment;
    mPipeline = ctx.device.CreateRenderPipeline(&desc);

    wgpu::ShaderModule prepMod = makeModule(ctx.device, kSpritesPrepareWgsl);
    wgpu::ComputePipelineDescriptor pd{};
    pd.compute.module = prepMod;
    pd.compute.entryPoint = "main";
    mPrepare = ctx.device.CreateComputePipeline(&pd);
    if (mBlend == Blend::Over) {
        wgpu::ShaderModule sortMod = makeModule(ctx.device, kSpritesSortWgsl);
        wgpu::ComputePipelineDescriptor sd{};
        sd.compute.module = sortMod;
        sd.compute.entryPoint = "main";
        mSort = ctx.device.CreateComputePipeline(&sd);
    }

    wgpu::BufferDescriptor ud{};
    ud.size = 32;
    ud.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    mUniforms = ctx.device.CreateBuffer(&ud);
    if (mTextured) {
        mSampler = makeLinearClampSampler(ctx.device);
    }
    return mPipeline != nullptr;
}

// (Re)builds the draw list for a pool size: index list padded to a power of
// two for the bitonic network, indirect args, and the per-stage (j, k)
// uniforms. Sort bind groups are cached per source buffer (ping-pong).
void SpritesNode::ensurePool(FrameContext& ctx, uint32_t capacity)
{
    uint32_t padded = 1;
    while (padded < capacity) {
        padded *= 2;
    }
    if (padded == mPadded) {
        return;
    }
    mPadded = padded;
    mSortGroups.clear();
    mSortUniforms.clear();

    wgpu::BufferDescriptor id{};
    id.size = (uint64_t)mPadded * 4;
    id.usage = wgpu::BufferUsage::Storage;
    mIndices = ctx.device.CreateBuffer(&id);
    wgpu::BufferDescriptor ad{};
    ad.size = 16;
    ad.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::Indirect;
    mIndirect = ctx.device.CreateBuffer(&ad);

    if (mBlend == Blend::Over) {
        for (uint32_t k = 2; k <= mPadded; k *= 2) {
            for (uint32_t j = k / 2; j >= 1; j /= 2) {
                const uint32_t jk[4] = { j, k, 0, 0 };
                wgpu::BufferDescriptor sd{};
                sd.size = sizeof(jk);
                sd.usage =
                    wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
                wgpu::Buffer buf = ctx.device.CreateBuffer(&sd);
                ctx.device.GetQueue().WriteBuffer(buf, 0, jk, sizeof(jk));
                mSortUniforms.push_back(std::move(buf));
            }
        }
    }
}

void SpritesNode::evaluate(FrameContext& ctx)
{
    if (!ensurePipeline(ctx)) {
        return;
    }
    const Value particles = inputValue(PortParticles);
    if (!particles.buffer) {
        return; // the producer has not run yet
    }
    if (mTextured && !inputValue(PortTexture).texture) {
        return;
    }
    ensurePool(ctx, particles.bufCapacity);

    const uint32_t width = ctx.targetWidth, height = ctx.targetHeight;
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

    const float uniforms[8] = {
        (float)width / (float)height,
        (float)inputValue(PortFrameRate).v[0],
        (float)mSheetCols,
        (float)mSheetRows,
        (float)inputValue(PortParallax).v[0],
        (float)inputValue(PortParallax).v[1],
        0.0f,
        0.0f,
    };
    ctx.device.GetQueue().WriteBuffer(mUniforms, 0, uniforms,
                                      sizeof(uniforms));

    const uint64_t poolSize =
        (uint64_t)particles.bufStride * particles.bufCapacity;
    const auto bufferEntry = [](uint32_t binding, const wgpu::Buffer& buf,
                                uint64_t size) {
        wgpu::BindGroupEntry e{};
        e.binding = binding;
        e.buffer = buf;
        e.size = size;
        return e;
    };

    wgpu::CommandEncoder encoder = ctx.device.CreateCommandEncoder();

    // Prepare: compact live slots, size the indirect draw.
    {
        wgpu::BindGroupEntry entries[3] = {
            bufferEntry(0, particles.buffer, poolSize),
            bufferEntry(1, mIndices, (uint64_t)mPadded * 4),
            bufferEntry(2, mIndirect, 16),
        };
        wgpu::BindGroupDescriptor bgd{};
        bgd.layout = mPrepare.GetBindGroupLayout(0);
        bgd.entryCount = 3;
        bgd.entries = entries;
        wgpu::BindGroup group = ctx.device.CreateBindGroup(&bgd);
        wgpu::ComputePassEncoder pass = encoder.BeginComputePass();
        pass.SetPipeline(mPrepare);
        pass.SetBindGroup(0, group);
        pass.DispatchWorkgroups(1, 1, 1);
        pass.End();
    }

    // 'over': bitonic sort, back-to-front (§18.5.5). One dispatch per
    // stage; bind groups are cached per source ping-pong buffer.
    if (mBlend == Blend::Over) {
        auto& groups = mSortGroups[particles.buffer.Get()];
        if (groups.size() != mSortUniforms.size()) {
            groups.clear();
            for (const auto& stage : mSortUniforms) {
                wgpu::BindGroupEntry entries[3] = {
                    bufferEntry(0, particles.buffer, poolSize),
                    bufferEntry(1, mIndices, (uint64_t)mPadded * 4),
                    bufferEntry(2, stage, 16),
                };
                wgpu::BindGroupDescriptor bgd{};
                bgd.layout = mSort.GetBindGroupLayout(0);
                bgd.entryCount = 3;
                bgd.entries = entries;
                groups.push_back(ctx.device.CreateBindGroup(&bgd));
            }
        }
        wgpu::ComputePassEncoder pass = encoder.BeginComputePass();
        pass.SetPipeline(mSort);
        for (const auto& group : groups) {
            pass.SetBindGroup(0, group);
            pass.DispatchWorkgroups((mPadded + 63) / 64, 1, 1);
        }
        pass.End();
    }

    std::vector<wgpu::BindGroupEntry> entries;
    entries.push_back(bufferEntry(0, mUniforms, 32));
    entries.push_back(bufferEntry(1, particles.buffer, poolSize));
    entries.push_back(bufferEntry(2, mIndices, (uint64_t)mPadded * 4));
    if (mTextured) {
        wgpu::BindGroupEntry t{};
        t.binding = 3;
        t.textureView = inputValue(PortTexture).texture.CreateView();
        entries.push_back(t);
        wgpu::BindGroupEntry s{};
        s.binding = 4;
        s.sampler = mSampler;
        entries.push_back(s);
    }
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = mPipeline.GetBindGroupLayout(0);
    bgd.entryCount = entries.size();
    bgd.entries = entries.data();
    wgpu::BindGroup group = ctx.device.CreateBindGroup(&bgd);

    wgpu::RenderPassColorAttachment attachment{};
    attachment.view = mColor.CreateView();
    attachment.loadOp = wgpu::LoadOp::Clear;
    attachment.storeOp = wgpu::StoreOp::Store;
    attachment.clearValue = { 0.0, 0.0, 0.0, 0.0 };
    wgpu::RenderPassDescriptor rpd{};
    rpd.colorAttachmentCount = 1;
    rpd.colorAttachments = &attachment;

    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rpd);
    pass.SetPipeline(mPipeline);
    pass.SetBindGroup(0, group);
    pass.DrawIndirect(mIndirect, 0);
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

// ---- TrailsNode (§18.5.6) ----

// History recording: one ring sample per particle per sim tick. A fresh
// emission (age == 0) refills its whole ring with the emission point so a
// recycled slot never draws a streak from its previous life.
const char* kTrailsRecordWgsl = R"(
struct Particle {
    pos: vec3f, age: f32,
    vel: vec3f, lifetime: f32,
    color: vec4f,
    size: f32, rotation: f32, seed: f32, emitter: f32,
}

// misc = (aspect, tick, length, -); par = parallax; style = (width, taper,
// fade, -). Shared with the render stage; the record pass reads misc only.
struct Params { misc: vec4f, par: vec4f, style: vec4f }

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> pts: array<Particle>;
@group(0) @binding(2) var<storage, read_write> hist: array<vec4f>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) id: vec3u) {
    let i = id.x;
    if (i >= arrayLength(&pts)) {
        return;
    }
    let L = u32(params.misc.z);
    let p = pts[i];
    let s = vec4f(p.pos.xy, p.size, select(0.0, 1.0, p.lifetime != 0.0));
    if (p.age == 0.0 && p.lifetime != 0.0) {
        for (var k = 0u; k < L; k++) {
            hist[i * L + k] = s;
        }
    } else {
        hist[i * L + (u32(params.misc.y) % L)] = s;
    }
}
)";

// Ribbon rendering: per instance (= particle slot), a triangle strip of
// 2 * (kSubdiv * (length - 1) + 1) vertices along the Catmull-Rom spline
// through the ring, extruded perpendicular to the local tangent in
// aspect-corrected space.
const char* kTrailsRenderWgsl = R"(
struct Particle {
    pos: vec3f, age: f32,
    vel: vec3f, lifetime: f32,
    color: vec4f,
    size: f32, rotation: f32, seed: f32, emitter: f32,
}

struct Params { misc: vec4f, par: vec4f, style: vec4f }

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> pts: array<Particle>;
@group(0) @binding(2) var<storage, read> hist: array<vec4f>;

struct VsOut {
    @builtin(position) pos: vec4f,
    @location(0) color: vec4f,
}

// Sample k ticks ago (0 = the sample recorded this tick), aspect-corrected
// x so widths and tangents are isotropic on screen.
fn sampleAgo(i: u32, k: i32) -> vec3f {
    let L = u32(params.misc.z);
    let kk = u32(clamp(k, 0, i32(L) - 1));
    let slot = (u32(params.misc.y) + L - (kk % L)) % L;
    let s = hist[i * L + slot];
    return vec3f(s.x * params.misc.x, s.y, s.z);
}

@vertex
fn drift_trail_vs(@builtin(vertex_index) v: u32,
                  @builtin(instance_index) i: u32) -> VsOut {
    var out: VsOut;
    let p = pts[i];
    if (p.lifetime == 0.0) {
        out.pos = vec4f(0.0, 0.0, 0.0, 1.0); // degenerate: nothing rastered
        return out;
    }
    let L = u32(params.misc.z);
    let sub = SUBDIVu;
    let q = v / 2u;                      // point index along the spline
    let side = f32(v & 1u) * 2.0 - 1.0;  // extrusion side
    let t = f32(q) / f32(sub);           // in [0, L-1]
    let i0 = i32(floor(t));
    let f = fract(t);
    let p0 = sampleAgo(i, i0 - 1);
    let p1 = sampleAgo(i, i0);
    let p2 = sampleAgo(i, i0 + 1);
    let p3 = sampleAgo(i, i0 + 2);
    // Catmull-Rom position and tangent (t runs head -> tail).
    let pos = 0.5 * (2.0 * p1 + (p2 - p0) * f +
                     (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * f * f +
                     (3.0 * p1 - p0 - 3.0 * p2 + p3) * f * f * f);
    let tan2 = 0.5 * ((p2.xy - p0.xy) +
                      2.0 * (2.0 * p0.xy - 5.0 * p1.xy + 4.0 * p2.xy -
                             p3.xy) * f +
                      3.0 * (3.0 * p1.xy - p0.xy - 3.0 * p2.xy + p3.xy) *
                          f * f);
    let tl = sqrt(dot(tan2, tan2));
    var nrm = vec2f(0.0);
    if (tl > 1e-6) {
        nrm = vec2f(-tan2.y, tan2.x) / tl; // zero tangent: zero width
    }
    let u = t / f32(L - 1u); // 0 at the head, 1 at the tail
    let w = pos.z * params.style.x * mix(1.0, params.style.y, u);
    let pc = pos.xy + nrm * w * side;
    let fin = vec2f(pc.x / params.misc.x, pc.y) + params.par.xy * p.pos.z;
    out.pos = vec4f(fin.x * 2.0 - 1.0, 1.0 - fin.y * 2.0, 0.0, 1.0);
    out.color = p.color * mix(1.0, params.style.z, u);
    return out;
}

@fragment
fn drift_trail_fs(@location(0) color: vec4f) -> @location(0) vec4f {
    return color;
}
)";

TrailsNode::TrailsNode(Blend blend, uint32_t length)
    : mBlend(blend), mLength(length)
{
    outputs.resize(1);
    outputs[0].name = "result";
    outputs[0].value.type = ValueType::Texture;
}

bool TrailsNode::ensurePipeline(FrameContext& ctx)
{
    if (mPipeline) {
        return true;
    }
    wgpu::ShaderModule recMod = makeModule(ctx.device, kTrailsRecordWgsl);
    if (!recMod) {
        return false;
    }
    wgpu::ComputePipelineDescriptor rd{};
    rd.compute.module = recMod;
    rd.compute.entryPoint = "main";
    mRecord = ctx.device.CreateComputePipeline(&rd);

    std::string source = kTrailsRenderWgsl;
    const std::string sub = std::to_string(kSubdiv);
    for (size_t at = source.find("SUBDIV"); at != std::string::npos;
         at = source.find("SUBDIV")) {
        source.replace(at, 6, sub);
    }
    wgpu::ShaderModule module = makeModule(ctx.device, source);
    if (!module) {
        return false;
    }

    wgpu::BlendState blend{};
    blend.color.srcFactor = wgpu::BlendFactor::One;
    blend.color.dstFactor = mBlend == Blend::Add
        ? wgpu::BlendFactor::One
        : wgpu::BlendFactor::OneMinusSrcAlpha;
    blend.alpha = blend.color;
    wgpu::ColorTargetState color{};
    color.format = wgpu::TextureFormat::RGBA16Float;
    color.blend = &blend;

    wgpu::FragmentState fragment{};
    fragment.module = module;
    fragment.entryPoint = "drift_trail_fs";
    fragment.targetCount = 1;
    fragment.targets = &color;

    wgpu::RenderPipelineDescriptor desc{};
    desc.vertex.module = module;
    desc.vertex.entryPoint = "drift_trail_vs";
    desc.primitive.topology = wgpu::PrimitiveTopology::TriangleStrip;
    desc.fragment = &fragment;
    mPipeline = ctx.device.CreateRenderPipeline(&desc);

    wgpu::BufferDescriptor ud{};
    ud.size = 48;
    ud.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    mUniforms = ctx.device.CreateBuffer(&ud);
    return mPipeline != nullptr;
}

void TrailsNode::evaluate(FrameContext& ctx)
{
    if (!ensurePipeline(ctx)) {
        return;
    }
    const Value particles = inputValue(PortParticles);
    if (!particles.buffer) {
        return; // the producer has not run yet
    }
    if (particles.bufCapacity != mHistCapacity) {
        wgpu::BufferDescriptor hd{};
        hd.size = (uint64_t)particles.bufCapacity * mLength * 16;
        hd.usage = wgpu::BufferUsage::Storage; // zero-init: empty rings
        mHistory = ctx.device.CreateBuffer(&hd);
        mHistCapacity = particles.bufCapacity;
        mTick = 0;
    }

    const uint32_t width = ctx.targetWidth, height = ctx.targetHeight;
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

    // Record a sample only when the simulation actually ticked — a
    // re-render for a style change must not consume ring history.
    const Node::Input& src = inputs[PortParticles];
    const bool ticked =
        src.srcNode && src.srcNode->outputs[src.srcPort].dirty;
    if (ticked) {
        ++mTick;
    }

    const float uniforms[12] = {
        (float)width / (float)height,
        (float)(mTick % mLength),
        (float)mLength,
        0.0f,
        (float)inputValue(PortParallax).v[0],
        (float)inputValue(PortParallax).v[1],
        0.0f,
        0.0f,
        (float)inputValue(PortWidth).v[0],
        (float)inputValue(PortTaper).v[0],
        (float)inputValue(PortFade).v[0],
        0.0f,
    };
    ctx.device.GetQueue().WriteBuffer(mUniforms, 0, uniforms,
                                      sizeof(uniforms));

    const uint64_t poolSize =
        (uint64_t)particles.bufStride * particles.bufCapacity;
    const uint64_t histSize = (uint64_t)mHistCapacity * mLength * 16;
    wgpu::BindGroupEntry entries[3] = {};
    entries[0].binding = 0;
    entries[0].buffer = mUniforms;
    entries[0].size = 48;
    entries[1].binding = 1;
    entries[1].buffer = particles.buffer;
    entries[1].size = poolSize;
    entries[2].binding = 2;
    entries[2].buffer = mHistory;
    entries[2].size = histSize;

    wgpu::CommandEncoder encoder = ctx.device.CreateCommandEncoder();
    if (ticked) {
        wgpu::BindGroupDescriptor bgd{};
        bgd.layout = mRecord.GetBindGroupLayout(0);
        bgd.entryCount = 3;
        bgd.entries = entries;
        wgpu::BindGroup group = ctx.device.CreateBindGroup(&bgd);
        wgpu::ComputePassEncoder pass = encoder.BeginComputePass();
        pass.SetPipeline(mRecord);
        pass.SetBindGroup(0, group);
        pass.DispatchWorkgroups((mHistCapacity + 63) / 64, 1, 1);
        pass.End();
    }

    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = mPipeline.GetBindGroupLayout(0);
    bgd.entryCount = 3;
    bgd.entries = entries;
    wgpu::BindGroup group = ctx.device.CreateBindGroup(&bgd);

    wgpu::RenderPassColorAttachment attachment{};
    attachment.view = mColor.CreateView();
    attachment.loadOp = wgpu::LoadOp::Clear;
    attachment.storeOp = wgpu::StoreOp::Store;
    attachment.clearValue = { 0.0, 0.0, 0.0, 0.0 };
    wgpu::RenderPassDescriptor rpd{};
    rpd.colorAttachmentCount = 1;
    rpd.colorAttachments = &attachment;

    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rpd);
    pass.SetPipeline(mPipeline);
    pass.SetBindGroup(0, group);
    pass.Draw(2 * (kSubdiv * (mLength - 1) + 1), particles.bufCapacity);
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
