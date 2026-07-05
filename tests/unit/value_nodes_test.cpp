#include <doctest/doctest.h>

#include <cmath>

#include "core/Nodes.h"

// §9.9 arithmetic/noise/damp/edge evaluation semantics. Pure CPU — nodes
// are driven directly through constant inputs like sequence_test does.

using namespace drift::core;

namespace {

Value scalar(double v)
{
    Value out{};
    out.type = ValueType::Scalar;
    out.v[0] = v;
    return out;
}

Value vec2(double x, double y)
{
    Value out{};
    out.type = ValueType::Vec2;
    out.v = { x, y, 0.0, 0.0 };
    return out;
}

// Drives any pure value node through constant inputs.
template <typename NodeT> struct Harness {
    NodeT node;

    template <typename... Args>
    explicit Harness(size_t inputs, Args&&... args)
        : node(std::forward<Args>(args)...)
    {
        node.inputs.resize(inputs);
    }

    const Value& eval(std::initializer_list<Value> values)
    {
        size_t i = 0;
        for (const Value& v : values) {
            node.inputs[i++].constant = v;
        }
        for (auto& out : node.outputs) {
            out.dirty = false;
        }
        FrameContext ctx{};
        node.evaluate(ctx);
        return node.outputs[0].value;
    }
};

} // namespace

TEST_CASE("arithmetic: add/multiply/mix/clamp (§9.9)")
{
    Harness<ArithmeticNode> add(2, ArithmeticNode::Op::Add);
    CHECK(add.eval({ scalar(2), scalar(3) }).v[0] == 5);
    CHECK(add.eval({ vec2(1, 2), vec2(10, 20) }).v[1] == 22);

    Harness<ArithmeticNode> mul(2, ArithmeticNode::Op::Multiply);
    CHECK(mul.eval({ scalar(4), scalar(2.5) }).v[0] == 10);

    Harness<ArithmeticNode> mix(3, ArithmeticNode::Op::Mix);
    CHECK(mix.eval({ scalar(0), scalar(10), scalar(0.25) }).v[0] == 2.5);
    // t is unclamped: extrapolation is deliberate.
    CHECK(mix.eval({ scalar(0), scalar(10), scalar(1.5) }).v[0] == 15);

    Harness<ArithmeticNode> clamp(3, ArithmeticNode::Op::Clamp);
    CHECK(clamp.eval({ scalar(5), scalar(0), scalar(1) }).v[0] == 1);
    CHECK(clamp.eval({ vec2(-1, 0.5), vec2(0, 0), vec2(1, 1) }).v[0] == 0);
    CHECK(clamp.eval({ vec2(-1, 0.5), vec2(0, 0), vec2(1, 1) }).v[1] == 0.5);
}

TEST_CASE("noise: bounded, deterministic, seed-decorrelated, C0 (§9.9)")
{
    Harness<NoiseNode> noise(3);
    double first = noise.eval({ scalar(1.7), scalar(1), scalar(0) }).v[0];
    for (int i = 0; i < 200; ++i) {
        const double v =
            noise.eval({ scalar(i * 0.37), scalar(1), scalar(0) }).v[0];
        CHECK(v >= -1.0);
        CHECK(v <= 1.0);
    }
    // Deterministic: same input, same value.
    CHECK(noise.eval({ scalar(1.7), scalar(1), scalar(0) }).v[0] == first);
    // Seeds decorrelate.
    CHECK(noise.eval({ scalar(1.7), scalar(1), scalar(1) }).v[0] != first);
    // Continuous across a lattice boundary.
    const double before =
        noise.eval({ scalar(3 - 1e-9), scalar(1), scalar(0) }).v[0];
    const double after =
        noise.eval({ scalar(3 + 1e-9), scalar(1), scalar(0) }).v[0];
    CHECK(std::abs(after - before) < 1e-6);
}

TEST_CASE("damp: snaps first, halves per halflife, frame-rate independent (§9.9)")
{
    Harness<DampNode> damp(3);
    // First evaluation snaps to the input.
    CHECK(damp.eval({ scalar(10), scalar(0.016), scalar(0.5) }).v[0] == 10);
    // One halflife closes exactly half the gap (target 0 from 10).
    CHECK(damp.eval({ scalar(0), scalar(0.5), scalar(0.5) }).v[0]
          == doctest::Approx(5.0));
    // Two quarter-halflife steps equal one half-halflife step.
    Harness<DampNode> a(3), b(3);
    a.eval({ scalar(10), scalar(0), scalar(0.5) });
    b.eval({ scalar(10), scalar(0), scalar(0.5) });
    a.eval({ scalar(0), scalar(0.25), scalar(0.5) });
    const double split = a.eval({ scalar(0), scalar(0.25), scalar(0.5) }).v[0];
    const double whole = b.eval({ scalar(0), scalar(0.5), scalar(0.5) }).v[0];
    CHECK(split == doctest::Approx(whole));
}

TEST_CASE("edge: arms silently, fires on crossings per mode (§9.9)")
{
    const auto fired = [](auto& h, double value) {
        h.eval({ scalar(value), scalar(0.5) });
        return h.node.outputs[0].dirty;
    };

    Harness<EdgeNode> rise(2, EdgeNode::Mode::Rise);
    CHECK(!fired(rise, 1.0)); // first sample arms, even above threshold
    CHECK(!fired(rise, 1.0));
    CHECK(!fired(rise, 0.0)); // falling edge ignored in rise mode
    CHECK(fired(rise, 1.0));  // rising crossing fires
    CHECK(!fired(rise, 0.9)); // staying above does not re-fire

    Harness<EdgeNode> fall(2, EdgeNode::Mode::Fall);
    CHECK(!fired(fall, 1.0));
    CHECK(fired(fall, 0.0));
    CHECK(!fired(fall, 0.0));

    Harness<EdgeNode> both(2, EdgeNode::Mode::Both);
    CHECK(!fired(both, 0.0));
    CHECK(fired(both, 1.0));
    CHECK(fired(both, 0.0));
}
