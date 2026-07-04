#include <doctest/doctest.h>

#include <map>

#include <ktx.h>

#include "core/Scene.h"

// Scene::load validation tests (SCENE_FORMAT.md §13). Load never touches the
// GPU (pipelines are created lazily at first evaluate), so a null device is
// fine here.

using drift::core::Scene;

namespace {

const char* kMinimalWgsl = R"(
    struct Params { phase: f32 }
    @group(0) @binding(0) var<uniform> params: Params;
    @fragment fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
        return vec4f(params.phase);
    }
)";

const char* kTrailWgsl = R"(
    @group(0) @binding(0) var current: texture_2d<f32>;
    @group(0) @binding(1) var current_sampler: sampler;
    @group(0) @binding(2) var history: texture_2d<f32>;
    @group(0) @binding(3) var history_sampler: sampler;
    @fragment fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
        return textureSample(current, current_sampler, uv) +
               textureSample(history, history_sampler, uv) * 0.9;
    }
)";

// A valid 2x2 RGBA PNG.
const unsigned char kTinyPng[] = {
    137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 2,
    0, 0, 0, 2, 8, 6, 0, 0, 0, 114, 182, 13, 36, 0, 0, 0, 24, 73, 68, 65, 84,
    120, 156, 5, 193, 1, 1, 0, 0, 8, 195, 32, 222, 220, 230, 19, 68, 114, 35,
    165, 7, 61, 226, 7, 123, 172, 157, 78, 228, 0, 0, 0, 0, 73, 69, 78, 68,
    174, 66, 96, 130,
};

// The same 2x2 image as a lossless WebP.
const unsigned char kTinyWebp[] = {
    82, 73, 70, 70, 52, 0, 0, 0, 87, 69, 66, 80, 86, 80, 56, 76, 39, 0, 0, 0,
    47, 1, 64, 0, 16, 31, 48, 255, 2, 130, 34, 255, 71, 19, 16, 20, 249, 63,
    154, 128, 160, 232, 186, 229, 2, 236, 166, 130, 154, 182, 13, 88, 252, 38,
    29, 17, 253, 143, 3, 0,
};

// A 2x2 sRGB RGBA8 KTX2 with a full mip chain (2x2 + 1x1), authored through
// libktx itself.
std::string makeTinyKtx2()
{
    ktxTextureCreateInfo ci{};
    ci.vkFormat = 43; // VK_FORMAT_R8G8B8A8_SRGB
    ci.baseWidth = 2;
    ci.baseHeight = 2;
    ci.baseDepth = 1;
    ci.numDimensions = 2;
    ci.numLevels = 2;
    ci.numLayers = 1;
    ci.numFaces = 1;

    ktxTexture2* tex = nullptr;
    REQUIRE(ktxTexture2_Create(&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex) ==
            KTX_SUCCESS);
    const unsigned char base[16] = { 255, 0, 0,   255, 0,   255, 0,   128,
                                     0,   0, 255, 0,   255, 255, 255, 255 };
    const unsigned char mip[4] = { 128, 128, 128, 255 };
    REQUIRE(ktxTexture_SetImageFromMemory((ktxTexture*)tex, 0, 0, 0, base,
                                          sizeof(base)) == KTX_SUCCESS);
    REQUIRE(ktxTexture_SetImageFromMemory((ktxTexture*)tex, 1, 0, 0, mip,
                                          sizeof(mip)) == KTX_SUCCESS);
    ktx_uint8_t* mem = nullptr;
    ktx_size_t size = 0;
    REQUIRE(ktxTexture_WriteToMemory((ktxTexture*)tex, &mem, &size) ==
            KTX_SUCCESS);
    std::string bytes((const char*)mem, size);
    free(mem);
    ktxTexture_Destroy((ktxTexture*)tex);
    return bytes;
}

// Canned decoder so video-node wiring is testable without ffmpeg or a GPU.
struct StubVideoDecoder : drift::core::VideoDecoder {
    drift::core::VideoFrame frame;
    StubVideoDecoder()
    {
        frame.width = 2;
        frame.height = 2;
        frame.index = 0;
        frame.rgba.assign(2 * 2 * 4, 255);
    }
    const drift::core::VideoFrame* frameAt(double) override { return &frame; }
};

drift::core::VideoDecoderFactory stubVideoFactory()
{
    return [](const std::string& path, bool,
              std::string& error) -> std::unique_ptr<drift::core::VideoDecoder> {
        if (path != "assets/clip.mp4") {
            error = "no such file";
            return nullptr;
        }
        return std::make_unique<StubVideoDecoder>();
    };
}

// A 4x4 Basis-supercompressed (ETC1S) KTX2. withAlpha selects RGBA input
// with a varying (straight) alpha channel vs. opaque RGB.
std::string makeBasisKtx2(bool withAlpha)
{
    ktxTextureCreateInfo ci{};
    ci.vkFormat = withAlpha ? 43 /*R8G8B8A8_SRGB*/ : 29 /*R8G8B8_SRGB*/;
    ci.baseWidth = 4;
    ci.baseHeight = 4;
    ci.baseDepth = 1;
    ci.numDimensions = 2;
    ci.numLevels = 1;
    ci.numLayers = 1;
    ci.numFaces = 1;

    ktxTexture2* tex = nullptr;
    REQUIRE(ktxTexture2_Create(&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex) ==
            KTX_SUCCESS);
    unsigned char pixels[4 * 4 * 4];
    const int comp = withAlpha ? 4 : 3;
    for (int i = 0; i < 16; ++i) {
        pixels[i * comp + 0] = (unsigned char)(i * 16);
        pixels[i * comp + 1] = (unsigned char)(255 - i * 16);
        pixels[i * comp + 2] = 128;
        if (withAlpha) {
            pixels[i * comp + 3] = (unsigned char)(i * 17);
        }
    }
    REQUIRE(ktxTexture_SetImageFromMemory((ktxTexture*)tex, 0, 0, 0, pixels,
                                          (ktx_size_t)(16 * comp)) ==
            KTX_SUCCESS);
    REQUIRE(ktxTexture2_CompressBasis(tex, 0) == KTX_SUCCESS);
    ktx_uint8_t* mem = nullptr;
    ktx_size_t size = 0;
    REQUIRE(ktxTexture_WriteToMemory((ktxTexture*)tex, &mem, &size) ==
            KTX_SUCCESS);
    std::string bytes((const char*)mem, size);
    free(mem);
    ktxTexture_Destroy((ktxTexture*)tex);
    return bytes;
}

struct LoadResult {
    std::unique_ptr<Scene> scene;
    std::vector<std::string> errors;

    bool hasError(const std::string& needle) const
    {
        for (const auto& e : errors) {
            if (e.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
    std::string joined() const
    {
        std::string all;
        for (const auto& e : errors) {
            all += e + "\n";
        }
        return all;
    }
};

LoadResult load(const std::string& json,
                const drift::core::VideoDecoderFactory& videoFactory = nullptr)
{
    const std::map<std::string, std::string> assets = {
        { "shaders/minimal.wgsl", kMinimalWgsl },
        { "shaders/trail.wgsl", kTrailWgsl },
        { "assets/tiny.png",
          std::string((const char*)kTinyPng, sizeof(kTinyPng)) },
        { "assets/tiny.webp",
          std::string((const char*)kTinyWebp, sizeof(kTinyWebp)) },
        { "assets/tiny.ktx2", makeTinyKtx2() },
        { "assets/opaque.basis.ktx2", makeBasisKtx2(false) },
        { "assets/alpha.basis.ktx2", makeBasisKtx2(true) },
        { "assets/corrupt.png", "this is not a png" },
        { "assets/corrupt.webp",
          std::string("RIFF\x10\0\0\0WEBPgarbage", 19) },
    };
    LoadResult r;
    r.scene = Scene::load(
        json,
        [&assets](const std::string& path, std::string& out) {
            auto it = assets.find(path);
            if (it == assets.end()) {
                return false;
            }
            out = it->second;
            return true;
        },
        videoFactory, wgpu::Device(), r.errors);
    return r;
}

} // namespace

TEST_CASE("minimal valid scene loads")
{
    auto r = load(R"({
        "version": 1, "name": "Minimal",
        "nodes": [
            { "id": "fx", "type": "shader", "shader": "shaders/minimal.wgsl",
              "inputs": { "phase": 0.5 } },
            { "id": "out", "type": "output", "inputs": { "color": "@fx" } }
        ]
    })");
    CAPTURE(r.joined());
    REQUIRE(r.scene != nullptr);
    CHECK(r.scene->name() == "Minimal");
    CHECK(r.errors.empty());
}

TEST_CASE("wrong version is rejected")
{
    auto r = load(R"({ "version": 2, "name": "x", "nodes": [] })");
    CHECK(r.scene == nullptr);
    CHECK(r.hasError("version"));
}

TEST_CASE("duplicate node ids are rejected")
{
    auto r = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "a", "type": "wave", "inputs": { "input": "@time.seconds" } },
            { "id": "a", "type": "wave", "inputs": { "input": "@time.seconds" } }
        ]
    })");
    CHECK(r.scene == nullptr);
    CHECK(r.hasError("duplicate node id 'a'"));
}

TEST_CASE("reserved implicit ids cannot be declared")
{
    auto r = load(R"({
        "version": 1, "name": "x",
        "nodes": [ { "id": "time", "type": "wave", "inputs": { "input": 0 } } ]
    })");
    CHECK(r.scene == nullptr);
    CHECK(r.hasError("reserved"));
}

TEST_CASE("unknown node type is rejected")
{
    auto r = load(R"({
        "version": 1, "name": "x",
        "nodes": [ { "id": "a", "type": "vortex" } ]
    })");
    CHECK(r.scene == nullptr);
    CHECK(r.hasError("unknown node type 'vortex'"));
}

TEST_CASE("unknown parameter reference is rejected")
{
    auto r = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "a", "type": "wave", "inputs": { "input": "$nope" } }
        ]
    })");
    CHECK(r.scene == nullptr);
    CHECK(r.hasError("unknown parameter '$nope'"));
}

TEST_CASE("cycles without previous edges are rejected")
{
    auto r = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "a", "type": "remap", "inputs": { "value": "@b" } },
            { "id": "b", "type": "remap", "inputs": { "value": "@a" } }
        ]
    })");
    CHECK(r.scene == nullptr);
    CHECK(r.hasError("cycle"));
}

TEST_CASE("self-feedback texture edge loads (trail pattern)")
{
    auto r = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "fx", "type": "shader", "shader": "shaders/minimal.wgsl",
              "inputs": { "phase": 0.5 } },
            { "id": "trail", "type": "shader", "shader": "shaders/trail.wgsl",
              "inputs": { "current": "@fx",
                          "history": { "node": "trail", "previous": true } } },
            { "id": "out", "type": "output", "inputs": { "color": "@trail" } }
        ]
    })");
    CAPTURE(r.joined());
    REQUIRE(r.scene != nullptr);
    CHECK(r.errors.empty());
}

TEST_CASE("value cycle through a previous edge loads")
{
    // w1 <- previous(w2) <- w1: legal because one edge is a feedback edge.
    auto r = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "w1", "type": "wave",
              "inputs": { "input": { "node": "w2", "previous": true } } },
            { "id": "w2", "type": "wave", "inputs": { "input": "@w1" } },
            { "id": "fx", "type": "shader", "shader": "shaders/minimal.wgsl",
              "inputs": { "phase": "@w2" } },
            { "id": "out", "type": "output", "inputs": { "color": "@fx" } }
        ]
    })");
    CAPTURE(r.joined());
    REQUIRE(r.scene != nullptr);
}

TEST_CASE("previous edge to an unknown node is rejected")
{
    auto r = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "w", "type": "wave",
              "inputs": { "input": { "node": "ghost", "previous": true } } }
        ]
    })");
    CHECK(r.scene == nullptr);
    CHECK(r.hasError("unknown node 'ghost'"));
}

TEST_CASE("polymorphic type cannot resolve through a feedback edge")
{
    // remap's T comes from 'value'; a self-feedback edge there is circular.
    auto r = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "acc", "type": "remap",
              "inputs": { "value": { "node": "acc", "previous": true } } }
        ]
    })");
    CHECK(r.scene == nullptr);
    CHECK(r.hasError("feedback edge"));
}

TEST_CASE("previous edge type mismatches are still rejected")
{
    auto r = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "fx", "type": "shader", "shader": "shaders/minimal.wgsl",
              "inputs": { "phase": 0.5 } },
            { "id": "w", "type": "wave",
              "inputs": { "input": { "node": "fx", "previous": true } } }
        ]
    })");
    CHECK(r.scene == nullptr);
    CHECK(r.hasError("type mismatch"));
}

TEST_CASE("mouse is an implicit input node")
{
    // position (vec2, default output) and active (scalar) both wire up.
    auto r = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "s", "type": "split", "inputs": { "value": "@mouse.position" } },
            { "id": "fade", "type": "remap",
              "inputs": { "value": "@mouse.active", "outMin": 0.2, "outMax": 1 } },
            { "id": "fx", "type": "shader", "shader": "shaders/minimal.wgsl",
              "inputs": { "phase": "@s.x" } },
            { "id": "out", "type": "output", "inputs": { "color": "@fx" } }
        ]
    })");
    CAPTURE(r.joined());
    REQUIRE(r.scene != nullptr);

    auto defaultPort = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "s", "type": "split", "inputs": { "value": "@mouse" } }
        ]
    })");
    CHECK(defaultPort.scene == nullptr); // fails only on missing output node
    CHECK(defaultPort.hasError("no output node"));

    auto badPort = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "s", "type": "split", "inputs": { "value": "@mouse.wheel" } }
        ]
    })");
    CHECK(badPort.scene == nullptr);
    CHECK(badPort.hasError("no output port 'wheel'"));

    auto typeMismatch = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "w", "type": "wave", "inputs": { "input": "@mouse.position" } }
        ]
    })");
    CHECK(typeMismatch.scene == nullptr);
    CHECK(typeMismatch.hasError("type mismatch"));
}

TEST_CASE("a scene must have exactly one output node")
{
    auto none = load(R"({ "version": 1, "name": "x", "nodes": [] })");
    CHECK(none.scene == nullptr);

    auto two = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "fx", "type": "shader", "shader": "shaders/minimal.wgsl",
              "inputs": { "phase": 0.5 } },
            { "id": "o1", "type": "output", "inputs": { "color": "@fx" } },
            { "id": "o2", "type": "output", "inputs": { "color": "@fx" } }
        ]
    })");
    CHECK(two.scene == nullptr);
}

TEST_CASE("literal type mismatches on ports are rejected")
{
    auto r = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "a", "type": "wave",
              "inputs": { "input": "@time.seconds", "frequency": [1, 2] } }
        ]
    })");
    CHECK(r.scene == nullptr);
    CHECK_FALSE(r.errors.empty());
}

TEST_CASE("unbound shader ports are rejected")
{
    auto r = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "fx", "type": "shader", "shader": "shaders/minimal.wgsl" },
            { "id": "out", "type": "output", "inputs": { "color": "@fx" } }
        ]
    })");
    CHECK(r.scene == nullptr);
    CHECK_FALSE(r.errors.empty());
}

TEST_CASE("missing shader file is a load error")
{
    auto r = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "fx", "type": "shader", "shader": "shaders/nope.wgsl",
              "inputs": {} },
            { "id": "out", "type": "output", "inputs": { "color": "@fx" } }
        ]
    })");
    CHECK(r.scene == nullptr);
    CHECK_FALSE(r.errors.empty());
}

TEST_CASE("polymorphic remap resolves vec2 and allows scalar splat")
{
    auto r = load(R"({
        "version": 1, "name": "x",
        "parameters": {
            "p": { "type": "vec2", "default": [0.5, 0.5] }
        },
        "nodes": [
            { "id": "a", "type": "remap",
              "inputs": { "value": "$p", "outMin": 0.4, "outMax": [0.6, 0.6] } },
            { "id": "sx", "type": "split", "inputs": { "value": "@a" } },
            { "id": "fx", "type": "shader", "shader": "shaders/minimal.wgsl",
              "inputs": { "phase": "@sx.x" } },
            { "id": "out", "type": "output", "inputs": { "color": "@fx" } }
        ]
    })");
    CAPTURE(r.joined());
    CHECK(r.scene != nullptr);
}

TEST_CASE("split component beyond input arity is rejected")
{
    auto r = load(R"({
        "version": 1, "name": "x",
        "parameters": {
            "p": { "type": "vec2", "default": [0, 0] }
        },
        "nodes": [
            { "id": "s", "type": "split", "inputs": { "value": "$p" } },
            { "id": "fx", "type": "shader", "shader": "shaders/minimal.wgsl",
              "inputs": { "phase": "@s.z" } },
            { "id": "out", "type": "output", "inputs": { "color": "@fx" } }
        ]
    })");
    CHECK(r.scene == nullptr);
    CHECK_FALSE(r.errors.empty());
}

TEST_CASE("image + transform + compositor scene loads (spec example 14.1 shape)")
{
    auto r = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "bg",   "type": "image", "src": "assets/tiny.png" },
            { "id": "fg",   "type": "image", "src": "assets/tiny.png" },
            { "id": "place", "type": "transform",
              "inputs": { "source": "@fg", "position": [0.3, 0.7], "scale": 0.2 } },
            { "id": "comp", "type": "compositor",
              "inputs": { "layers": ["@bg", "@place"] } },
            { "id": "out",  "type": "output", "inputs": { "color": "@comp" } }
        ]
    })");
    CAPTURE(r.joined());
    REQUIRE(r.scene != nullptr);
    CHECK(r.errors.empty());
}

TEST_CASE("webp and ktx2 image sources load")
{
    for (const char* src : { "assets/tiny.webp", "assets/tiny.ktx2" }) {
        auto r = load(std::string(R"({
            "version": 1, "name": "x",
            "nodes": [
                { "id": "bg", "type": "image", "src": ")") + src + R"(" },
                { "id": "out", "type": "output", "inputs": { "color": "@bg" } }
            ]
        })");
        CAPTURE(src);
        CAPTURE(r.joined());
        CHECK(r.scene != nullptr);
    }

    auto corrupt = load(R"({
        "version": 1, "name": "x",
        "nodes": [ { "id": "a", "type": "image", "src": "assets/corrupt.webp" } ]
    })");
    CHECK(corrupt.scene == nullptr);
    CHECK(corrupt.hasError("cannot decode"));
}

TEST_CASE("basis ktx2 uploads compressed: opaque accepted, straight alpha rejected")
{
    auto opaque = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "bg", "type": "image", "src": "assets/opaque.basis.ktx2" },
            { "id": "out", "type": "output", "inputs": { "color": "@bg" } }
        ]
    })");
    CAPTURE(opaque.joined());
    CHECK(opaque.scene != nullptr);

    auto alpha = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "bg", "type": "image", "src": "assets/alpha.basis.ktx2" },
            { "id": "out", "type": "output", "inputs": { "color": "@bg" } }
        ]
    })");
    CHECK(alpha.scene == nullptr);
    CHECK(alpha.hasError("premultiplied"));
}

TEST_CASE("image validation: missing src, missing file, corrupt file, escaping path")
{
    auto noSrc = load(R"({
        "version": 1, "name": "x",
        "nodes": [ { "id": "a", "type": "image" } ]
    })");
    CHECK(noSrc.scene == nullptr);
    CHECK(noSrc.hasError("needs a string 'src'"));

    auto missing = load(R"({
        "version": 1, "name": "x",
        "nodes": [ { "id": "a", "type": "image", "src": "assets/nope.png" } ]
    })");
    CHECK(missing.scene == nullptr);
    CHECK(missing.hasError("cannot read image"));

    auto corrupt = load(R"({
        "version": 1, "name": "x",
        "nodes": [ { "id": "a", "type": "image", "src": "assets/corrupt.png" } ]
    })");
    CHECK(corrupt.scene == nullptr);
    CHECK(corrupt.hasError("cannot decode"));

    auto escape = load(R"({
        "version": 1, "name": "x",
        "nodes": [ { "id": "a", "type": "image", "src": "../../etc/passwd" } ]
    })");
    CHECK(escape.scene == nullptr);
    CHECK(escape.hasError("escapes the project"));
}

TEST_CASE("video node loads through the decoder factory")
{
    auto r = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "bg", "type": "video", "src": "assets/clip.mp4", "loop": false },
            { "id": "out", "type": "output", "inputs": { "color": "@bg" } }
        ]
    })", stubVideoFactory());
    CAPTURE(r.joined());
    REQUIRE(r.scene != nullptr);
    CHECK(r.errors.empty());
}

TEST_CASE("video validation: factory errors, missing factory, bad loop")
{
    auto missing = load(R"({
        "version": 1, "name": "x",
        "nodes": [ { "id": "v", "type": "video", "src": "assets/nope.mp4" } ]
    })", stubVideoFactory());
    CHECK(missing.scene == nullptr);
    CHECK(missing.hasError("cannot open video"));

    auto noFactory = load(R"({
        "version": 1, "name": "x",
        "nodes": [ { "id": "v", "type": "video", "src": "assets/clip.mp4" } ]
    })");
    CHECK(noFactory.scene == nullptr);
    CHECK(noFactory.hasError("not supported"));

    auto badLoop = load(R"({
        "version": 1, "name": "x",
        "nodes": [ { "id": "v", "type": "video", "src": "assets/clip.mp4",
                     "loop": "yes" } ]
    })", stubVideoFactory());
    CHECK(badLoop.scene == nullptr);
    CHECK(badLoop.hasError("'loop' must be a boolean"));

    auto noSrc = load(R"({
        "version": 1, "name": "x",
        "nodes": [ { "id": "v", "type": "video" } ]
    })", stubVideoFactory());
    CHECK(noSrc.scene == nullptr);
    CHECK(noSrc.hasError("needs a string 'src'"));
}

TEST_CASE("compositor layers must be a non-empty array of texture connections")
{
    auto scalarConn = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "w", "type": "wave", "inputs": { "input": "@time.seconds" } },
            { "id": "comp", "type": "compositor", "inputs": { "layers": ["@w"] } }
        ]
    })");
    CHECK(scalarConn.scene == nullptr);
    CHECK(scalarConn.hasError("type mismatch"));

    auto empty = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "comp", "type": "compositor", "inputs": { "layers": [] } }
        ]
    })");
    CHECK(empty.scene == nullptr);
    CHECK(empty.hasError("non-empty array"));

    auto single = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "bg", "type": "image", "src": "assets/tiny.png" },
            { "id": "comp", "type": "compositor", "inputs": { "layers": "@bg" } }
        ]
    })");
    CHECK(single.scene == nullptr);
    CHECK(single.hasError("non-empty array"));

    auto numbers = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "comp", "type": "compositor", "inputs": { "layers": [1, 2] } }
        ]
    })");
    CHECK(numbers.scene == nullptr);

    auto literalElement = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "bg", "type": "image", "src": "assets/tiny.png" },
            { "id": "comp", "type": "compositor",
              "inputs": { "layers": ["@bg", 3] } }
        ]
    })");
    CHECK(literalElement.scene == nullptr);
    CHECK(literalElement.hasError("must be connections"));
}

TEST_CASE("arrays are rejected on non-array ports")
{
    auto r = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "bg", "type": "image", "src": "assets/tiny.png" },
            { "id": "w", "type": "wave", "inputs": { "input": ["@bg"] } }
        ]
    })");
    CHECK(r.scene == nullptr);
    CHECK(r.hasError("array"));
}

TEST_CASE("transform requires source and rejects unknown ports")
{
    auto noSource = load(R"({
        "version": 1, "name": "x",
        "nodes": [ { "id": "t", "type": "transform" } ]
    })");
    CHECK(noSource.scene == nullptr);
    CHECK(noSource.hasError("required input 'source' missing"));

    auto badPort = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "bg", "type": "image", "src": "assets/tiny.png" },
            { "id": "t", "type": "transform",
              "inputs": { "source": "@bg", "skew": 3 } }
        ]
    })");
    CHECK(badPort.scene == nullptr);
    CHECK(badPort.hasError("unknown input port 'skew'"));
}

TEST_CASE("setParameter validates, clamps, and splats")
{
    auto r = load(R"({
        "version": 1, "name": "x",
        "parameters": {
            "intensity": { "type": "scalar", "default": 0.6, "min": 0, "max": 1 },
            "offset": { "type": "vec2", "default": [0.5, 0.5] }
        },
        "nodes": [
            { "id": "fx", "type": "shader", "shader": "shaders/minimal.wgsl",
              "inputs": { "phase": "$intensity" } },
            { "id": "out", "type": "output", "inputs": { "color": "@fx" } }
        ]
    })");
    CAPTURE(r.joined());
    REQUIRE(r.scene != nullptr);

    drift::core::Value v{};
    v.type = drift::core::ValueType::Scalar;
    v.v[0] = 0.25f;
    CHECK(r.scene->setParameter("intensity", v));
    CHECK(r.scene->parameters()[0].value.v[0] == 0.25f);

    v.v[0] = 7.0f; // clamped to declared max
    CHECK(r.scene->setParameter("intensity", v));
    CHECK(r.scene->parameters()[0].value.v[0] == 1.0f);

    CHECK_FALSE(r.scene->setParameter("nope", v));

    // scalar splats to a vec2 parameter
    v.v[0] = 0.3f;
    CHECK(r.scene->setParameter("offset", v));
    CHECK(r.scene->parameters()[1].value.type == drift::core::ValueType::Vec2);
    CHECK(r.scene->parameters()[1].value.v[1] == 0.3f);

    // vec2 into a scalar parameter is a type mismatch
    drift::core::Value v2{};
    v2.type = drift::core::ValueType::Vec2;
    CHECK_FALSE(r.scene->setParameter("intensity", v2));
}

TEST_CASE("combine with only x bound is rejected")
{
    auto r = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "c", "type": "combine", "inputs": { "x": 1 } }
        ]
    })");
    CHECK(r.scene == nullptr);
    CHECK_FALSE(r.errors.empty());
}
