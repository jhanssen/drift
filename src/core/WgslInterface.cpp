#include "WgslInterface.h"

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
        } else if (type == "texture_2d<f32>") {
            out.textures.push_back({ name, group, binding, false, 0, 0 });
        } else if (type == "sampler") {
            samplers.push_back({ name, group, binding });
        } else {
            error = "unsupported binding type '" + type + "' for '" + name + "'";
            return false;
        }
    }

    // Match samplers to textures by the <texture>_sampler convention.
    for (const auto& s : samplers) {
        bool matched = false;
        for (auto& t : out.textures) {
            if (s.name == t.name + "_sampler") {
                t.hasSampler = true;
                t.samplerGroup = s.group;
                t.samplerBinding = s.binding;
                matched = true;
                break;
            }
        }
        if (!matched) {
            error = "sampler '" + s.name + "' does not match any texture (expected <texture>_sampler)";
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
