#include "Nodes.h"

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

VideoNode::VideoNode(std::unique_ptr<VideoDecoder> decoder)
    : mDecoder(std::move(decoder))
{
    outputs.resize(1);
    outputs[0].value.type = ValueType::Texture;
}

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

void VideoNode::evaluate(FrameContext& ctx)
{
    const VideoFrame* frame = mDecoder->frameAt(ctx.seconds);
    if (!frame) {
        return; // decode error; hold the previous frame
    }

    if (frame->planes.size() == 2) {
        if (frame->index == mLastIndex) {
            return;
        }
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
