#include "WgslInterface.h"

#include <algorithm>
#include <regex>

namespace drift::core {

namespace {

std::string stripComments(const std::string& src)
{
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size();) {
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') ++i;
        } else if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) ++i;
            i = std::min(i + 2, src.size());
            out.push_back(' ');
        } else {
            out.push_back(src[i++]);
        }
    }
    return out;
}

bool mapValueType(const std::string& t, ValueType& out)
{
    if (t == "f32") { out = ValueType::Scalar; return true; }
    if (t == "vec2f" || t == "vec2<f32>") { out = ValueType::Vec2; return true; }
    if (t == "vec3f" || t == "vec3<f32>") { out = ValueType::Vec3; return true; }
    if (t == "vec4f" || t == "vec4<f32>") { out = ValueType::Vec4; return true; }
    return false;
}

void layoutSizeAlign(ValueType t, uint32_t& size, uint32_t& align)
{
    switch (t) {
    case ValueType::Scalar: size = 4; align = 4; break;
    case ValueType::Vec2: size = 8; align = 8; break;
    case ValueType::Vec3: size = 12; align = 16; break;
    case ValueType::Vec4: size = 16; align = 16; break;
    default: size = 0; align = 4; break;
    }
}

uint32_t alignUp(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

// Lays out a struct (f32/vecN fields only, like the params struct) and
// returns its WGSL size and alignment; array stride = alignUp(size, align).
bool layoutStruct(const std::string& source, const std::string& structName,
                  uint32_t& size, uint32_t& align, std::string& error)
{
    const std::regex structRe("struct\\s+" + structName + "\\s*\\{([^}]*)\\}");
    std::smatch sm;
    if (!std::regex_search(source, sm, structRe)) {
        error = "struct '" + structName + "' not found";
        return false;
    }
    const std::string body = sm[1];
    static const std::regex fieldRe(
        R"(([A-Za-z_]\w*)\s*:\s*([A-Za-z_][\w]*(?:<[^,>]*>)?))");
    uint32_t cursor = 0;
    align = 4;
    bool any = false;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), fieldRe);
         it != std::sregex_iterator(); ++it) {
        const std::smatch& m = *it;
        ValueType vt;
        if (!mapValueType(m[2], vt)) {
            error = "struct '" + structName + "' field '" + std::string(m[1]) +
                    "' has unsupported type '" + std::string(m[2]) +
                    "' (f32/vecN only)";
            return false;
        }
        uint32_t fieldSize, fieldAlign;
        layoutSizeAlign(vt, fieldSize, fieldAlign);
        cursor = alignUp(cursor, fieldAlign) + fieldSize;
        align = std::max(align, fieldAlign);
        any = true;
    }
    if (!any) {
        error = "struct '" + structName + "' has no fields";
        return false;
    }
    size = alignUp(cursor, align);
    return true;
}

// The stride of array<Elem> in the storage address space.
bool elementStride(const std::string& source, const std::string& element,
                   uint32_t& stride, std::string& error)
{
    ValueType vt;
    if (mapValueType(element, vt)) {
        uint32_t size, align;
        layoutSizeAlign(vt, size, align);
        stride = alignUp(size, align);
        return true;
    }
    uint32_t size, align;
    if (!layoutStruct(source, element, size, align, error)) {
        return false;
    }
    stride = size; // already rounded to the struct's alignment
    return true;
}

} // namespace

bool WgslInterface::parse(const std::string& rawSource, WgslInterface& out, std::string& error)
{
    const std::string source = stripComments(rawSource);

    // @group(G) @binding(B) var<...>? name : type ;
    static const std::regex bindingRe(
        R"(@group\s*\(\s*(\d+)\s*\)\s*@binding\s*\(\s*(\d+)\s*\)\s*var\s*(<[^>]*>)?\s*([A-Za-z_]\w*)\s*:\s*([A-Za-z_][\w]*(?:<[^;>]*>)?)\s*;)");

    struct Sampler { std::string name; uint32_t group, binding; };
    std::vector<Sampler> samplers;
    std::string uniformStructName;

    for (auto it = std::sregex_iterator(source.begin(), source.end(), bindingRe);
         it != std::sregex_iterator(); ++it) {
        const std::smatch& m = *it;
        const uint32_t group = (uint32_t)std::stoul(m[1]);
        const uint32_t binding = (uint32_t)std::stoul(m[2]);
        const std::string addressSpace = m[3];
        const std::string name = m[4];
        const std::string type = m[5];

        if (addressSpace.find("uniform") != std::string::npos) {
            if (name != "params") {
                error = "uniform binding must be named 'params' (found '" + name + "')";
                return false;
            }
            if (out.hasUniforms) {
                error = "multiple uniform bindings";
                return false;
            }
            out.hasUniforms = true;
            out.uniformGroup = group;
            out.uniformBinding = binding;
            uniformStructName = type;
        } else if (addressSpace.find("storage") != std::string::npos) {
            // §18.2: storage arrays become buffer ports; read_write ones
            // are owned outputs.
            static const std::regex arrayRe(R"(^array<\s*([A-Za-z_]\w*)\s*>$)");
            std::smatch am;
            if (!std::regex_match(type, am, arrayRe)) {
                error = "storage binding '" + name +
                        "' must be an array<...> (found '" + type + "')";
                return false;
            }
            WgslStorageBuffer buf;
            buf.name = name;
            buf.group = group;
            buf.binding = binding;
            buf.readWrite =
                addressSpace.find("read_write") != std::string::npos;
            buf.elementType = am[1];
            if (!elementStride(source, buf.elementType, buf.elementStride,
                               error)) {
                return false;
            }
            out.storageBuffers.push_back(std::move(buf));
        } else if (type.starts_with("texture_storage_2d")) {
            // Only the §12 intermediate format; anything else would leak
            // format policy into scene content.
            static const std::regex storageTexRe(
                R"(^texture_storage_2d<\s*rgba16float\s*,\s*write\s*>$)");
            if (!std::regex_match(type, storageTexRe)) {
                error = "storage texture '" + name +
                        "' must be texture_storage_2d<rgba16float, write>";
                return false;
            }
            out.storageTextures.push_back({ name, group, binding });
        } else if (type == "texture_2d<f32>") {
            out.textures.push_back({ name, group, binding, false, 0, 0 });
        } else if (type == "sampler") {
            samplers.push_back({ name, group, binding });
        } else {
            error = "unsupported binding type '" + type + "' for '" + name + "'";
            return false;
        }
    }

    // §18.2 compute entry point: @compute + @workgroup_size on fn main.
    if (source.find("@compute") != std::string::npos) {
        out.isCompute = true;
        static const std::regex wgRe(
            R"(@workgroup_size\s*\(\s*(\d+)\s*(?:,\s*(\d+)\s*)?(?:,\s*(\d+)\s*)?\))");
        std::smatch wm;
        if (!std::regex_search(source, wm, wgRe)) {
            error = "@compute entry point needs @workgroup_size";
            return false;
        }
        for (int i = 0; i < 3; i++) {
            out.workgroupSize[i] =
                wm[i + 1].matched ? (uint32_t)std::stoul(wm[i + 1]) : 1;
            if (out.workgroupSize[i] == 0) {
                error = "@workgroup_size dimensions must be > 0";
                return false;
            }
        }
        static const std::regex mainRe(R"(fn\s+main\s*\()");
        if (!std::regex_search(source, mainRe)) {
            error = "compute entry point must be named 'main'";
            return false;
        }
    }

    // Port order within each storage kind is (group, binding), like sampled
    // textures below.
    const auto byBinding = [](const auto& a, const auto& b) {
        return a.group != b.group ? a.group < b.group : a.binding < b.binding;
    };
    std::sort(out.storageBuffers.begin(), out.storageBuffers.end(), byBinding);
    std::sort(out.storageTextures.begin(), out.storageTextures.end(),
              byBinding);

    // Texture port order is (group, binding), not declaration order — it
    // defines "first texture input" for auto-sizing (SCENE_FORMAT.md §17.5).
    std::sort(out.textures.begin(), out.textures.end(),
              [](const WgslTexture& a, const WgslTexture& b) {
                  return a.group != b.group ? a.group < b.group
                                            : a.binding < b.binding;
              });

    // Match samplers to textures by the <texture>_sampler convention.
    for (const auto& s : samplers) {
        bool matched = false;
        for (auto& t : out.textures) {
            // The _repeat suffix requests repeat addressing instead of
            // clamp-to-edge (§9.10).
            const bool plain = s.name == t.name + "_sampler";
            const bool repeat = s.name == t.name + "_sampler_repeat";
            if (plain || repeat) {
                t.hasSampler = true;
                t.repeat = repeat;
                t.samplerGroup = s.group;
                t.samplerBinding = s.binding;
                matched = true;
                break;
            }
        }
        if (!matched) {
            error = "sampler '" + s.name +
                    "' does not match any texture (expected <texture>_sampler "
                    "or <texture>_sampler_repeat)";
            return false;
        }
    }

    if (out.hasUniforms) {
        const std::regex structRe(
            "struct\\s+" + uniformStructName + "\\s*\\{([^}]*)\\}");
        std::smatch sm;
        if (!std::regex_search(source, sm, structRe)) {
            error = "params struct '" + uniformStructName + "' not found";
            return false;
        }
        const std::string body = sm[1];
        static const std::regex fieldRe(
            R"(([A-Za-z_]\w*)\s*:\s*([A-Za-z_][\w]*(?:<[^,>]*>)?))");
        uint32_t cursor = 0;
        for (auto it = std::sregex_iterator(body.begin(), body.end(), fieldRe);
             it != std::sregex_iterator(); ++it) {
            const std::smatch& m = *it;
            ValueType vt;
            if (!mapValueType(m[2], vt)) {
                error = "params field '" + std::string(m[1]) +
                        "' has unsupported type '" + std::string(m[2]) + "'";
                return false;
            }
            uint32_t size, align;
            layoutSizeAlign(vt, size, align);
            const uint32_t offset = alignUp(cursor, align);
            cursor = offset + size;
            out.fields.push_back({ m[1], vt, offset });
        }
        if (out.fields.empty()) {
            error = "params struct '" + uniformStructName + "' has no fields";
            return false;
        }
        out.uniformSize = alignUp(cursor, 16);
    }

    return true;
}

} // namespace drift::core
