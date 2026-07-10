#include "Module.h"

#include <algorithm>
#include <cctype>

#include <glaze/glaze.hpp>

namespace drift::core {

namespace {

bool validPortName(const std::string& s)
{
    if (s.empty() || (!isalpha((unsigned char)s[0]) && s[0] != '_')) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return isalnum(c) || c == '_';
    });
}

bool parseType(const std::string& s, ValueType& out, bool allowBuffer)
{
    if (s == "scalar") { out = ValueType::Scalar; return true; }
    if (s == "vec2") { out = ValueType::Vec2; return true; }
    if (s == "vec3") { out = ValueType::Vec3; return true; }
    if (s == "vec4") { out = ValueType::Vec4; return true; }
    if (s == "event") { out = ValueType::Event; return true; }
    if (allowBuffer && s == "buffer") { out = ValueType::Buffer; return true; }
    return false;
}

bool parseLiteral(const glz::generic& j, Value& out)
{
    if (j.is_number()) {
        out.type = ValueType::Scalar;
        out.v[0] = j.get_number();
        return true;
    }
    if (j.is_array()) {
        const auto& arr = j.get_array();
        if (arr.size() < 2 || arr.size() > 4) {
            return false;
        }
        out.type = arr.size() == 2 ? ValueType::Vec2
                 : arr.size() == 3 ? ValueType::Vec3
                                   : ValueType::Vec4;
        for (size_t i = 0; i < arr.size(); ++i) {
            if (!arr[i].is_number()) {
                return false;
            }
            out.v[i] = arr[i].get_number();
        }
        return true;
    }
    return false;
}

} // namespace

bool ModuleInterface::parse(const std::string& json, ModuleInterface& out,
                            std::string& error)
{
    glz::generic doc{};
    if (auto ec = glz::read_json(doc, json); ec) {
        error = glz::format_error(ec, json);
        return false;
    }
    if (!doc.is_object()) {
        error = "top level must be an object";
        return false;
    }
    const auto& top = doc.get_object();

    auto abiIt = top.find("abi");
    if (abiIt == top.end() || !abiIt->second.is_number() ||
        (uint32_t)abiIt->second.get_number() != kModuleAbiVersion) {
        error = "missing or unsupported 'abi' (expected " +
                std::to_string(kModuleAbiVersion) + ")";
        return false;
    }
    out.abi = kModuleAbiVersion;

    // Ports are declared as JSON objects (DESIGN.md §4.5). The canonical
    // port order — block layout, event bit assignment, node port order —
    // is lexicographic by name within each direction, sorted here
    // explicitly, so no tool that rewrites the JSON with reordered keys
    // can change the ABI.
    const auto sortedEntries = [](const glz::generic& obj) {
        std::vector<std::pair<std::string, const glz::generic*>> entries;
        for (const auto& [name, spec] : obj.get_object()) {
            entries.emplace_back(name, &spec);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        return entries;
    };

    int eventBits = 0;
    if (auto it = top.find("inputs"); it != top.end()) {
        if (!it->second.is_object()) {
            error = "'inputs' must be an object";
            return false;
        }
        for (const auto& [name, specPtr] : sortedEntries(it->second)) {
            const glz::generic& spec = *specPtr;
            if (!validPortName(name)) {
                error = "input '" + name + "': invalid port name";
                return false;
            }
            if (!spec.is_object()) {
                error = "input '" + name + "': must be an object";
                return false;
            }
            const auto& obj = spec.get_object();
            Input in;
            in.name = name;
            auto typeIt = obj.find("type");
            if (typeIt == obj.end() || !typeIt->second.is_string() ||
                !parseType(typeIt->second.get_string(), in.type,
                           /*allowBuffer=*/false)) {
                error = "input '" + name +
                        "': 'type' must be scalar/vec2/vec3/vec4/event";
                return false;
            }
            if (auto defIt = obj.find("default"); defIt != obj.end()) {
                if (in.type == ValueType::Event) {
                    error = "input '" + name + "': events take no default";
                    return false;
                }
                if (!parseLiteral(defIt->second, in.def)) {
                    error = "input '" + name + "': bad 'default' literal";
                    return false;
                }
                if (in.def.type == ValueType::Scalar &&
                    in.type != ValueType::Scalar) {
                    const double s = in.def.v[0]; // §4: scalar splats
                    in.def.type = in.type;
                    in.def.v = { s, s, s, s };
                }
                if (in.def.type != in.type) {
                    error = "input '" + name + "': 'default' does not match "
                            "the declared type";
                    return false;
                }
                in.hasDefault = true;
            }
            if (in.type == ValueType::Event) {
                if (eventBits >= (int)kModuleMaxEvents) {
                    error = "more than " + std::to_string(kModuleMaxEvents) +
                            " event inputs";
                    return false;
                }
                in.eventBit = eventBits++;
            } else {
                in.def.type = in.type; // unset default reads as zeroes
            }
            out.inputs.push_back(std::move(in));
        }
    }

    auto outsIt = top.find("outputs");
    if (outsIt == top.end() || !outsIt->second.is_object() ||
        outsIt->second.get_object().empty()) {
        error = "'outputs' must be a non-empty object";
        return false;
    }
    eventBits = 0;
    for (const auto& [name, specPtr] : sortedEntries(outsIt->second)) {
        const glz::generic& spec = *specPtr;
        if (!validPortName(name)) {
            error = "output '" + name + "': invalid port name";
            return false;
        }
        if (!spec.is_object()) {
            error = "output '" + name + "': must be an object";
            return false;
        }
        const auto& obj = spec.get_object();
        Output o;
        o.name = name;
        auto typeIt = obj.find("type");
        if (typeIt == obj.end() || !typeIt->second.is_string() ||
            !parseType(typeIt->second.get_string(), o.type,
                       /*allowBuffer=*/true)) {
            error = "output '" + name +
                    "': 'type' must be scalar/vec2/vec3/vec4/event/buffer";
            return false;
        }
        const auto uintField = [&](const char* key, uint32_t& val) -> bool {
            auto it = obj.find(key);
            if (it == obj.end() || !it->second.is_number() ||
                it->second.get_number() <= 0 ||
                it->second.get_number() !=
                    (double)(uint32_t)it->second.get_number()) {
                return false;
            }
            val = (uint32_t)it->second.get_number();
            return true;
        };
        if (o.type == ValueType::Buffer) {
            if (!uintField("stride", o.stride) || o.stride % 4 != 0) {
                error = "output '" + name + "': buffer needs a positive "
                        "'stride' that is a multiple of 4";
                return false;
            }
            if (!uintField("capacity", o.capacity)) {
                error = "output '" + name +
                        "': buffer needs a positive integer 'capacity'";
                return false;
            }
            if ((uint64_t)o.stride * o.capacity > kModuleMaxBufferBytes) {
                error = "output '" + name + "': stride × capacity exceeds " +
                        std::to_string(kModuleMaxBufferBytes >> 20) + " MiB";
                return false;
            }
        } else if (obj.find("stride") != obj.end() ||
                   obj.find("capacity") != obj.end()) {
            error = "output '" + name +
                    "': 'stride'/'capacity' apply to buffer outputs only";
            return false;
        }
        if (o.type == ValueType::Event) {
            if (eventBits >= (int)kModuleMaxEvents) {
                error = "more than " + std::to_string(kModuleMaxEvents) +
                        " event outputs";
                return false;
            }
            o.eventBit = eventBits++;
        }
        out.outputs.push_back(std::move(o));
    }

    return true;
}

void ModuleInterface::computeLayout()
{
    uint32_t offset = kModuleHeaderSize;
    for (Input& in : inputs) {
        if (in.type == ValueType::Event) {
            continue;
        }
        in.offset = offset;
        offset += 4 * componentCount(in.type);
    }
    inputEnd = offset;
    valueOutBegin = offset;
    for (Output& o : outputs) {
        if (o.type == ValueType::Event || o.type == ValueType::Buffer) {
            continue; // events ride the header; buffers go last, below
        }
        o.offset = offset;
        offset += 4 * componentCount(o.type);
    }
    valueOutEnd = offset;
    for (Output& o : outputs) {
        if (o.type != ValueType::Buffer) {
            continue;
        }
        o.offset = offset;                  // {count, written}
        offset += 8 + o.stride * o.capacity; // then the staging bytes
    }
    ioSize = offset;
}

} // namespace drift::core
