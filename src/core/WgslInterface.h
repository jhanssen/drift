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

// §18.2: var<storage, read|read_write> name: array<Elem>. Element structs
// are restricted to f32/vecN fields (like the params struct); the stride
// follows WGSL storage layout, and is what buffer connections validate on.
struct WgslStorageBuffer {
    std::string name;
    uint32_t group = 0, binding = 0;
    bool readWrite = false;      // read_write = owned output port
    uint32_t elementStride = 0;
    std::string elementType;     // for error messages
};

// §18.2: texture_storage_2d<rgba16float, write> — a compute-written texture
// output (only the §12 intermediate format is accepted).
struct WgslStorageTexture {
    std::string name;
    uint32_t group = 0, binding = 0;
};

struct WgslInterface {
    bool hasUniforms = false;
    uint32_t uniformGroup = 0, uniformBinding = 0;
    uint32_t uniformSize = 0;
    std::vector<WgslUniformField> fields;
    std::vector<WgslTexture> textures;

    // §18.2 compute interface; empty/false for fragment shaders. Fragment
    // shader nodes reject sources that declare any of these.
    bool isCompute = false;
    uint32_t workgroupSize[3] = { 1, 1, 1 };
    std::vector<WgslStorageBuffer> storageBuffers;
    std::vector<WgslStorageTexture> storageTextures;

    static bool parse(const std::string& source, WgslInterface& out, std::string& error);
};

} // namespace drift::core
