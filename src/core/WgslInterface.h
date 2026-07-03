#pragma once

// Extracts a shader node's port interface from its WGSL fragment source
// (SCENE_FORMAT.md §9.10): a uniform struct bound as `params` whose fields
// become value ports, texture_2d<f32> bindings that become texture ports,
// and `<name>_sampler` sampler bindings the runtime provides.
//
// Deliberately a small recognizer for the contract's subset of WGSL, not a
// general parser — and deliberately not Tint, so the same code runs in the
// browser build.

#include <cstdint>
#include <string>
#include <vector>

#include "Value.h"

namespace drift::core {

struct WgslUniformField {
    std::string name;
    ValueType type;
    uint32_t offset; // WGSL uniform address space layout
};

struct WgslTexture {
    std::string name;
    uint32_t group = 0, binding = 0;
    bool hasSampler = false;
    uint32_t samplerGroup = 0, samplerBinding = 0;
};

struct WgslInterface {
    bool hasUniforms = false;
    uint32_t uniformGroup = 0, uniformBinding = 0;
    uint32_t uniformSize = 0;
    std::vector<WgslUniformField> fields;
    std::vector<WgslTexture> textures;

    static bool parse(const std::string& source, WgslInterface& out, std::string& error);
};

} // namespace drift::core
