#pragma once

#include <array>
#include <cstdint>

#include <webgpu/webgpu_cpp.h>

namespace drift::core {

// Edge/value types per SCENE_FORMAT.md §4.
enum class ValueType : uint8_t { Scalar, Vec2, Vec3, Vec4, Texture };

inline int componentCount(ValueType t)
{
    switch (t) {
    case ValueType::Scalar: return 1;
    case ValueType::Vec2: return 2;
    case ValueType::Vec3: return 3;
    case ValueType::Vec4: return 4;
    case ValueType::Texture: return 0;
    }
    return 0;
}

inline const char* valueTypeName(ValueType t)
{
    switch (t) {
    case ValueType::Scalar: return "scalar";
    case ValueType::Vec2: return "vec2";
    case ValueType::Vec3: return "vec3";
    case ValueType::Vec4: return "vec4";
    case ValueType::Texture: return "texture";
    }
    return "?";
}

struct Value {
    ValueType type = ValueType::Scalar;
    std::array<float, 4> v{}; // Scalar..Vec4 components
    wgpu::Texture texture;    // Texture: premultiplied alpha, linear
    uint32_t texWidth = 0, texHeight = 0;

    bool sameValueAs(const Value& o) const
    {
        if (type != o.type) {
            return false;
        }
        if (type == ValueType::Texture) {
            return texture.Get() == o.texture.Get();
        }
        const int n = componentCount(type);
        for (int i = 0; i < n; ++i) {
            if (v[i] != o.v[i]) {
                return false;
            }
        }
        return true;
    }
};

} // namespace drift::core
