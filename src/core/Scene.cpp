#include "Scene.h"

#include <algorithm>

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
        const double s = v.v[0];
        v.type = in.type;
        v.v = { s, s, s, s };
    }
    return v;
}

bool Node::inputFired(size_t index) const
{
    const Input& in = inputs[index];
    if (!in.srcNode) {
        return false;
    }
    const Output& src = in.srcNode->outputs[in.srcPort];
    return in.previous ? src.prevDirty : src.dirty;
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

bool Scene::fireEvent(const std::string& nodeId, const std::string& port)
{
    for (auto& node : mNodes) {
        if (node->id != nodeId) {
            continue;
        }
        for (size_t i = 0; i < node->outputs.size(); ++i) {
            const Node::Output& out = node->outputs[i];
            if (out.name == port && out.value.type == ValueType::Event) {
                mPendingFires.push_back({ node.get(), i });
                return true;
            }
        }
        return false;
    }
    return false;
}

bool Scene::setParameter(const std::string& name, Value value)
{
    for (size_t i = 0; i < mParams.size(); ++i) {
        SceneParam& param = mParams[i];
        if (param.name != name) {
            continue;
        }
        if (value.type == ValueType::Scalar && param.type != ValueType::Scalar) {
            const double s = value.v[0];
            value.type = param.type;
            value.v = { s, s, s, s };
        }
        if (value.type != param.type) {
            return false;
        }
        const int n = componentCount(param.type);
        for (int c = 0; c < n; ++c) {
            if (param.hasMin) {
                value.v[c] = std::max(value.v[c], param.min.v[c]);
            }
            if (param.hasMax) {
                value.v[c] = std::min(value.v[c], param.max.v[c]);
            }
        }
        if (param.value.sameValueAs(value)) {
            return true;
        }
        param.value = value;
        for (auto& node : mNodes) {
            for (auto& in : node->inputs) {
                if (in.paramIndex == (int)i) {
                    in.constant = value;
                    node->paramsChanged = true;
                }
            }
        }
        return true;
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
            if (out.value.type == ValueType::Buffer) {
                // Buffer feedback (§18.1): the history buffer is created
                // eagerly — WebGPU zero-initializes it, which IS the
                // first-frame all-zeroes rule — and refreshed with a copy
                // whenever the producer wrote this past frame.
                const uint64_t size =
                    (uint64_t)out.value.bufStride * out.value.bufCapacity;
                if (!out.historyBuffer && size) {
                    wgpu::BufferDescriptor desc{};
                    desc.size = size;
                    desc.usage = wgpu::BufferUsage::Storage |
                                 wgpu::BufferUsage::CopyDst;
                    out.historyBuffer = ctx.device.CreateBuffer(&desc);
                }
                if (out.dirty && out.value.buffer && out.historyBuffer) {
                    if (!copyEncoder) {
                        copyEncoder = ctx.device.CreateCommandEncoder();
                    }
                    copyEncoder.CopyBufferToBuffer(out.value.buffer, 0,
                                                   out.historyBuffer, 0, size);
                }
                out.prev.buffer = out.historyBuffer;
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

    // Injected event fires (fireEvent): applied after the clear so they
    // read as fired for this whole frame; topo order puts every consumer
    // after the producer, so all of them observe it.
    for (auto& [node, index] : mPendingFires) {
        node->outputs[index].dirty = true;
    }
    mPendingFires.clear();

    for (auto& node : mNodes) {
        if (node->alwaysEvaluate() || node->firstEvaluate ||
            node->paramsChanged || node->wakePending(ctx.seconds) ||
            node->inputsDirty()) {
            node->evaluate(ctx);
            node->firstEvaluate = false;
            node->paramsChanged = false;
        }
    }
    return ctx.presented;
}

bool Scene::wakesPending(double now) const
{
    for (const auto& node : mNodes) {
        if (node->wakePending(now)) {
            return true;
        }
    }
    return false;
}

double Scene::nextWake() const
{
    double next = -1.0;
    for (const auto& node : mNodes) {
        const double t = node->nextWake();
        if (t >= 0.0 && (next < 0.0 || t < next)) {
            next = t;
        }
    }
    return next;
}

} // namespace drift::core
