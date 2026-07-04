#include <doctest/doctest.h>

#include <map>

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

LoadResult load(const std::string& json)
{
    const std::map<std::string, std::string> assets = {
        { "shaders/minimal.wgsl", kMinimalWgsl },
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
        wgpu::Device(), r.errors);
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

TEST_CASE("previous-frame reads are a load error until implemented")
{
    auto r = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "a", "type": "remap",
              "inputs": { "value": { "node": "a", "previous": true } } }
        ]
    })");
    CHECK(r.scene == nullptr);
    CHECK(r.hasError("not implemented"));
}

TEST_CASE("mouse input is a load error until implemented")
{
    auto r = load(R"({
        "version": 1, "name": "x",
        "nodes": [
            { "id": "a", "type": "split", "inputs": { "value": "@mouse.position" } }
        ]
    })");
    CHECK(r.scene == nullptr);
    CHECK(r.hasError("not implemented"));
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
