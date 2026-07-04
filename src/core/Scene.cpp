#include "Scene.h"

namespace drift::core {

Value Node::inputValue(size_t index) const
{
    const Input& in = inputs[index];
    Value v;
    if (in.srcNode) {
        const Output& src = in.srcNode->outputs[in.srcPort];
        v = in.previous ? src.prev : src.value;
    } else {
        v = in.constant;
    }
    if (in.splat && v.type == ValueType::Scalar && in.type != ValueType::Scalar) {
        const float s = v.v[0];
        v.type = in.type;
        v.v = { s, s, s, s };
    }
    return v;
}

bool Node::inputsDirty() const
{
    for (const Input& in : inputs) {
        if (!in.srcNode) {
            continue;
        }
        const Output& src = in.srcNode->outputs[in.srcPort];
        // A previous edge is dirty on frame N iff the producer port was
        // written on frame N-1 (§10).
        if (in.previous ? src.prevDirty : src.dirty) {
            return true;
        }
    }
    return false;
}

bool Scene::render(FrameContext& ctx)
{
    ctx.frame = ++mFrame;
    ctx.presented = false;

    // Target changed (resize / first frame): everything re-evaluates.
    if (ctx.targetWidth != mLastWidth || ctx.targetHeight != mLastHeight ||
        ctx.targetFormat != mLastFormat) {
        mLastWidth = ctx.targetWidth;
        mLastHeight = ctx.targetHeight;
        mLastFormat = ctx.targetFormat;
        for (auto& node : mNodes) {
            node->firstEvaluate = true;
        }
    }

    // Snapshot previous-frame state (§10) before anything renders into this
    // frame. Texture outputs consumed by feedback edges get their last
    // written contents copied to a history texture — the copy reads frame
    // N-1's content because producers haven't re-rendered yet; queue order
    // keeps it ahead of this frame's passes. (Spec describes ping-pong; a
    // copy is semantically identical and keeps producer nodes unaware.)
    wgpu::CommandEncoder copyEncoder;
    for (auto& node : mNodes) {
        for (auto& out : node->outputs) {
            out.prevDirty = out.dirty;
            out.prev = out.value;
            if (!out.needsHistory) {
                continue;
            }
            if (out.dirty && out.value.texture) {
                const uint32_t w = out.value.texWidth, h = out.value.texHeight;
                if (!out.history || out.history.GetWidth() != w ||
                    out.history.GetHeight() != h) {
                    wgpu::TextureDescriptor desc{};
                    desc.format = out.value.texture.GetFormat();
                    desc.size = { w, h, 1 };
                    desc.usage = wgpu::TextureUsage::CopyDst |
                                 wgpu::TextureUsage::TextureBinding;
                    out.history = ctx.device.CreateTexture(&desc);
                }
                if (!copyEncoder) {
                    copyEncoder = ctx.device.CreateCommandEncoder();
                }
                wgpu::TexelCopyTextureInfo src{}, dst{};
                src.texture = out.value.texture;
                dst.texture = out.history;
                wgpu::Extent3D extent = { w, h, 1 };
                copyEncoder.CopyTextureToTexture(&src, &dst, &extent);
            }
            if (out.history) {
                out.prev.texture = out.history;
            } else if (out.value.type == ValueType::Texture) {
                // Never written: reads as fully transparent. Zero-size so
                // auto-sizing consumers don't lock onto the 1x1 blank.
                if (!mBlank) {
                    wgpu::TextureDescriptor desc{};
                    desc.format = wgpu::TextureFormat::RGBA16Float;
                    desc.size = { 1, 1, 1 };
                    desc.usage = wgpu::TextureUsage::TextureBinding;
                    mBlank = ctx.device.CreateTexture(&desc);
                }
                out.prev.texture = mBlank;
                out.prev.texWidth = 0;
                out.prev.texHeight = 0;
            }
        }
    }
    if (copyEncoder) {
        wgpu::CommandBuffer commands = copyEncoder.Finish();
        ctx.device.GetQueue().Submit(1, &commands);
    }

    for (auto& node : mNodes) {
        for (auto& out : node->outputs) {
            out.dirty = false;
        }
    }

    for (auto& node : mNodes) {
        const bool isOutput = node.get() == mOutput;
        if (node->alwaysEvaluate() || node->firstEvaluate || node->inputsDirty() ||
            (isOutput && ctx.forcePresent)) {
            node->evaluate(ctx);
            node->firstEvaluate = false;
        }
    }
    return ctx.presented;
}

} // namespace drift::core
