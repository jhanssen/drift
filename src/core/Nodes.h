#pragma once

// Built-in node types (SCENE_FORMAT.md §9). Instantiated by the loader.

#include <string>

#include "Scene.h"
#include "Video.h"
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

// §9.8 mouse (implicit): outputs position (last-known, output space),
// active (1 while the pointer is over the scene surface).
class MouseNode : public Node {
public:
    MouseNode();
    void evaluate(FrameContext& ctx) override;
    bool alwaysEvaluate() const override { return true; }
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

// §9.1 image: static texture source, dirty on load only. Decoding happens in
// the loader (validation without a GPU); pixels arrive here already linear,
// premultiplied, packed rgba16float. Upload happens on first evaluate.
class ImageNode : public Node {
public:
    // bytes: encoded file contents. Returns nullptr with error set on decode
    // failure.
    static ImageNode* decode(const std::string& bytes, std::string& error);

    void evaluate(FrameContext& ctx) override;

private:
    ImageNode(std::vector<uint16_t> pixels, uint32_t width, uint32_t height);

    std::vector<uint16_t> mPixels; // released after upload
    uint32_t mWidth = 0, mHeight = 0;
    wgpu::Texture mTexture;
};

// §9.2 video: streaming texture source, dirty on each new decoded frame.
// Decoding runs on the platform-provided decoder's thread; evaluate pulls
// the frame due at scene time and uploads it only when it changed.
class VideoNode : public Node {
public:
    explicit VideoNode(std::unique_ptr<VideoDecoder> decoder);
    void evaluate(FrameContext& ctx) override;
    // Frame advance is driven by scene time, not graph inputs.
    bool alwaysEvaluate() const override { return true; }

private:
    std::unique_ptr<VideoDecoder> mDecoder;
    wgpu::Texture mTexture;
    uint32_t mWidth = 0, mHeight = 0;
    int64_t mLastIndex = -1;
};

// §9.4 transform: renders source into a scene-output-sized transparent
// canvas with 2D position/rotation/scale/anchor/opacity applied. Sizing is
// resolution-independent: at scale [1,1] the displayed height equals the
// output height, width follows the source aspect.
class TransformNode : public Node {
public:
    // Input order: source, position, rotation, scale, anchor, opacity.
    TransformNode();
    void evaluate(FrameContext& ctx) override;

private:
    bool ensurePipeline(FrameContext& ctx);

    wgpu::RenderPipeline mPipeline;
    wgpu::Buffer mUniforms;
    wgpu::Sampler mSampler;
    wgpu::Texture mColor;
    uint32_t mWidth = 0, mHeight = 0;
};

// §9.5 compositor: stacks its inputs (all texture layers, bottom first) into
// a scene-output-sized target with premultiplied source-over blending.
class CompositorNode : public Node {
public:
    CompositorNode();
    void evaluate(FrameContext& ctx) override;

private:
    bool ensurePipeline(FrameContext& ctx);

    wgpu::RenderPipeline mPipeline;
    wgpu::Sampler mSampler;
    wgpu::Texture mColor;
    uint32_t mWidth = 0, mHeight = 0;
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
