#pragma once

// Built-in node types (SCENE_FORMAT.md §9). Instantiated by the loader.

#include <string>

#include "Scene.h"
#include "WgslInterface.h"

namespace drift::core {

// §9.7 time (implicit): outputs seconds, delta.
class TimeNode : public Node {
public:
    TimeNode();
    void evaluate(FrameContext& ctx) override;
    bool alwaysEvaluate() const override { return true; }

private:
    float mLast = 0.0f;
};

// §9.9 wave: inputs input, frequency, phase; property shape.
class WaveNode : public Node {
public:
    enum class Shape { Sine, Triangle, Saw, Square };
    explicit WaveNode(Shape shape);
    void evaluate(FrameContext& ctx) override;

private:
    Shape mShape;
};

// §9.9 remap: polymorphic linear range map; property clamp.
class RemapNode : public Node {
public:
    explicit RemapNode(bool clamp);
    void evaluate(FrameContext& ctx) override;

private:
    bool mClamp;
};

// §9.9 combine: scalars -> vecN (N fixed at load).
class CombineNode : public Node {
public:
    explicit CombineNode(ValueType resultType);
    void evaluate(FrameContext& ctx) override;
};

// §9.9 split: vecN -> component scalars (arity fixed at load).
class SplitNode : public Node {
public:
    explicit SplitNode(int arity);
    void evaluate(FrameContext& ctx) override;
};

// §9.3 shader: fullscreen fragment pass into an rgba16float intermediate.
// Ports come from the WGSL interface: value inputs first (uniform fields, in
// declaration order), then texture inputs.
class ShaderNode : public Node {
public:
    ShaderNode(std::string fragmentSource, WgslInterface iface,
               uint32_t explicitWidth, uint32_t explicitHeight);
    void evaluate(FrameContext& ctx) override;

    const WgslInterface& interface() const { return mInterface; }
    size_t textureInputStart() const { return mInterface.fields.size(); }

private:
    bool ensurePipeline(FrameContext& ctx);

    std::string mFragmentSource;
    WgslInterface mInterface;
    uint32_t mExplicitWidth = 0, mExplicitHeight = 0; // 0 = auto

    wgpu::RenderPipeline mPipeline;
    wgpu::Buffer mUniforms;
    wgpu::Sampler mSampler;
    wgpu::Texture mColor;
    uint32_t mWidth = 0, mHeight = 0;
};

// §9.6 output: blits the final linear texture to the platform target with
// sRGB encoding.
class OutputNode : public Node {
public:
    OutputNode();
    void evaluate(FrameContext& ctx) override;

private:
    wgpu::RenderPipeline mPipeline;
    wgpu::TextureFormat mPipelineFormat = wgpu::TextureFormat::Undefined;
    wgpu::Sampler mSampler;
};

} // namespace drift::core
