#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/WgslInterface.h"

using drift::core::ValueType;
using drift::core::WgslInterface;

namespace {

WgslInterface parseOk(const std::string& src)
{
    WgslInterface iface;
    std::string error;
    const bool ok = WgslInterface::parse(src, iface, error);
    CAPTURE(error);
    REQUIRE(ok);
    return iface;
}

std::string parseFail(const std::string& src)
{
    WgslInterface iface;
    std::string error;
    REQUIRE_FALSE(WgslInterface::parse(src, iface, error));
    REQUIRE_FALSE(error.empty());
    return error;
}

} // namespace

TEST_CASE("uniform struct fields become value ports with WGSL layout offsets")
{
    const auto iface = parseOk(R"(
        struct Params {
            phase: f32,
            scale: f32,
        }
        @group(0) @binding(0) var<uniform> params: Params;
        @fragment fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
            return vec4f(params.phase * params.scale);
        }
    )");
    REQUIRE(iface.hasUniforms);
    REQUIRE(iface.fields.size() == 2);
    CHECK(iface.fields[0].name == "phase");
    CHECK(iface.fields[0].type == ValueType::Scalar);
    CHECK(iface.fields[0].offset == 0);
    CHECK(iface.fields[1].name == "scale");
    CHECK(iface.fields[1].offset == 4);
    CHECK(iface.uniformSize == 16);
    CHECK(iface.textures.empty());
}

TEST_CASE("vec3 fields get 16-byte alignment")
{
    const auto iface = parseOk(R"(
        struct Params { a: f32, b: vec3<f32>, c: f32 }
        @group(0) @binding(0) var<uniform> params: Params;
    )");
    REQUIRE(iface.fields.size() == 3);
    CHECK(iface.fields[0].offset == 0);
    CHECK(iface.fields[1].offset == 16);
    CHECK(iface.fields[1].type == ValueType::Vec3);
    CHECK(iface.fields[2].offset == 28);
    CHECK(iface.uniformSize == 32);
}

TEST_CASE("textures pair with <name>_sampler bindings")
{
    const auto iface = parseOk(R"(
        @group(0) @binding(1) var source: texture_2d<f32>;
        @group(0) @binding(2) var source_sampler: sampler;
    )");
    CHECK_FALSE(iface.hasUniforms);
    REQUIRE(iface.textures.size() == 1);
    CHECK(iface.textures[0].name == "source");
    CHECK(iface.textures[0].binding == 1);
    CHECK(iface.textures[0].hasSampler);
    CHECK(iface.textures[0].samplerBinding == 2);
}

TEST_CASE("comments do not confuse the recognizer")
{
    const auto iface = parseOk(R"(
        // @group(0) @binding(7) var bogus: texture_2d<f32>;
        /* struct Nope { x: i32 } */
        struct Params { x: f32 } // trailing
        @group(0) @binding(0) var<uniform> params: Params;
    )");
    REQUIRE(iface.fields.size() == 1);
    CHECK(iface.fields[0].name == "x");
    CHECK(iface.textures.empty());
}

TEST_CASE("texture ports order by (group, binding), not declaration order")
{
    // "First texture input" drives auto-sizing (SCENE_FORMAT.md §17.5), so
    // the order must not depend on how the author arranged the file.
    const auto iface = parseOk(R"(
        @group(1) @binding(0) var late: texture_2d<f32>;
        @group(1) @binding(1) var late_sampler: sampler;
        @group(0) @binding(1) var early: texture_2d<f32>;
        @group(0) @binding(2) var early_sampler: sampler;
        @group(0) @binding(0) var first: texture_2d<f32>;
        @group(0) @binding(3) var first_sampler: sampler;
    )");
    REQUIRE(iface.textures.size() == 3);
    CHECK(iface.textures[0].name == "first");
    CHECK(iface.textures[1].name == "early");
    CHECK(iface.textures[2].name == "late");
}

TEST_CASE("orphan sampler is an error")
{
    const auto error = parseFail(R"(
        @group(0) @binding(0) var lonely_sampler: sampler;
    )");
    CHECK(error.find("does not match any texture") != std::string::npos);
}

TEST_CASE("uniform binding must be named params")
{
    const auto error = parseFail(R"(
        struct Config { x: f32 }
        @group(0) @binding(0) var<uniform> config: Config;
    )");
    CHECK(error.find("must be named 'params'") != std::string::npos);
}

TEST_CASE("non-float field types are an error")
{
    const auto error = parseFail(R"(
        struct Params { count: i32 }
        @group(0) @binding(0) var<uniform> params: Params;
    )");
    CHECK(error.find("unsupported type") != std::string::npos);
}

// ---- §18.2 compute interfaces ----------------------------------------------

namespace {

// The §18.3 Particle contract: 64 bytes under WGSL storage layout, scalars
// packed into the vec3 alignment slots.
const char* kParticleStruct = R"(
    struct Particle {
        pos: vec3f, age: f32,
        vel: vec3f, lifetime: f32,
        color: vec4f,
        size: f32, rotation: f32, seed: f32, reserved: f32,
    }
)";

} // namespace

TEST_CASE("compute: storage arrays reflect with WGSL storage strides")
{
    const auto iface = parseOk(std::string(kParticleStruct) + R"(
        struct Params { dt: f32 }
        @group(0) @binding(0) var<uniform> params: Params;
        @group(0) @binding(1) var<storage, read> prev: array<Particle>;
        @group(0) @binding(2) var<storage, read_write> state: array<Particle>;
        @compute @workgroup_size(64)
        fn main(@builtin(global_invocation_id) id: vec3u) {
            state[id.x] = prev[id.x];
        }
    )");
    CHECK(iface.isCompute);
    CHECK(iface.workgroupSize[0] == 64);
    CHECK(iface.workgroupSize[1] == 1);
    REQUIRE(iface.storageBuffers.size() == 2);
    CHECK(iface.storageBuffers[0].name == "prev");
    CHECK(!iface.storageBuffers[0].readWrite);
    CHECK(iface.storageBuffers[0].elementStride == 64);
    CHECK(iface.storageBuffers[1].name == "state");
    CHECK(iface.storageBuffers[1].readWrite);
    CHECK(iface.storageBuffers[1].elementStride == 64);
    REQUIRE(iface.fields.size() == 1);
    CHECK(iface.fields[0].name == "dt");
}

TEST_CASE("compute: scalar and vector element strides")
{
    const auto iface = parseOk(R"(
        @group(0) @binding(0) var<storage, read> weights: array<f32>;
        @group(0) @binding(1) var<storage, read> dirs: array<vec3f>;
        @group(0) @binding(2) var<storage, read_write> sums: array<vec4f>;
        @compute @workgroup_size(8, 8)
        fn main() {}
    )");
    CHECK(iface.workgroupSize[0] == 8);
    CHECK(iface.workgroupSize[1] == 8);
    REQUIRE(iface.storageBuffers.size() == 3);
    CHECK(iface.storageBuffers[0].elementStride == 4);  // f32
    CHECK(iface.storageBuffers[1].elementStride == 16); // vec3f: stride 16
    CHECK(iface.storageBuffers[2].elementStride == 16); // vec4f
}

TEST_CASE("compute: storage textures accept only the §12 intermediate format")
{
    const auto iface = parseOk(R"(
        @group(0) @binding(0) var target: texture_storage_2d<rgba16float, write>;
        @compute @workgroup_size(8, 8) fn main() {}
    )");
    REQUIRE(iface.storageTextures.size() == 1);
    CHECK(iface.storageTextures[0].name == "target");

    parseFail(R"(
        @group(0) @binding(0) var target: texture_storage_2d<rgba8unorm, write>;
        @compute @workgroup_size(8, 8) fn main() {}
    )");
}

TEST_CASE("compute: entry point requirements")
{
    // @compute without @workgroup_size
    CHECK(parseFail(R"(
        @group(0) @binding(0) var<storage, read_write> s: array<f32>;
        @compute fn main() {}
    )").find("workgroup_size") != std::string::npos);
    // entry point not named main
    CHECK(parseFail(R"(
        @group(0) @binding(0) var<storage, read_write> s: array<f32>;
        @compute @workgroup_size(64) fn tick() {}
    )").find("main") != std::string::npos);
    // zero workgroup dimension
    parseFail(R"(
        @compute @workgroup_size(0) fn main() {}
    )");
}

TEST_CASE("compute: element structs are f32/vecN only, and must exist")
{
    CHECK(parseFail(R"(
        struct Cell { alive: u32 }
        @group(0) @binding(0) var<storage, read_write> cells: array<Cell>;
        @compute @workgroup_size(64) fn main() {}
    )").find("unsupported type") != std::string::npos);
    CHECK(parseFail(R"(
        @group(0) @binding(0) var<storage, read_write> cells: array<Ghost>;
        @compute @workgroup_size(64) fn main() {}
    )").find("not found") != std::string::npos);
}
