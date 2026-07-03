#include "Scene.h"

namespace drift::core {

Value Node::inputValue(size_t index) const
{
    const Input& in = inputs[index];
    Value v = in.srcNode ? in.srcNode->outputs[in.srcPort].value : in.constant;
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
        if (in.srcNode && in.srcNode->outputs[in.srcPort].dirty) {
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
