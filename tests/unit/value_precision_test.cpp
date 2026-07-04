#include <doctest/doctest.h>

#include <cmath>

#include "core/Nodes.h"

// The CPU value graph computes in double precision (SCENE_FORMAT.md §17.2):
// scene time enters the graph unbounded, and after weeks of uptime f32 can no
// longer resolve a single frame step (at 30 days the f32 ULP is 0.25 s).
// These tests drive time -> wave at long uptimes and would fail with an f32
// value path.

using namespace drift::core;

namespace {

constexpr double kMonth = 30.0 * 24.0 * 60.0 * 60.0; // 2'592'000 s
constexpr double kStep = 1.0 / 60.0;

// time -> wave.input, frequency/phase as constants.
struct TimeWave {
    TimeNode time;
    WaveNode wave;

    explicit TimeWave(WaveNode::Shape shape, double frequency)
        : wave(shape)
    {
        wave.inputs.resize(3);
        wave.inputs[0].srcNode = &time;
        wave.inputs[0].srcPort = 0;
        wave.inputs[1].constant.v[0] = frequency;
        wave.inputs[2].constant.v[0] = 0.0;
    }

    double evaluate(double seconds)
    {
        FrameContext ctx{};
        ctx.seconds = seconds;
        time.evaluate(ctx);
        time.firstEvaluate = false;
        wave.evaluate(ctx);
        return wave.outputs[0].value.v[0];
    }
};

} // namespace

TEST_CASE("time survives the graph at long uptimes")
{
    TimeNode time;
    FrameContext ctx{};
    ctx.seconds = kMonth + kStep;
    time.evaluate(ctx);
    // An f32 output would round to 2'592'000 exactly, erasing the frame step.
    CHECK(time.outputs[0].value.v[0] == doctest::Approx(kMonth + kStep).epsilon(1e-12));
}

TEST_CASE("wave advances by single frame steps after a month of scene time")
{
    TimeWave tw(WaveNode::Shape::Sine, 0.25); // gentle ambient rate
    const double a = tw.evaluate(kMonth);
    const double b = tw.evaluate(kMonth + kStep);

    // With f32 time both frames collapse to the same sample.
    CHECK(a != b);

    // And the sampled values match the ideal double computation.
    auto ideal = [](double t) {
        const double x = t * 0.25;
        return std::sin((x - std::floor(x)) * 6.28318530717958647692);
    };
    CHECK(a == doctest::Approx(ideal(kMonth)).epsilon(1e-9));
    CHECK(b == doctest::Approx(ideal(kMonth + kStep)).epsilon(1e-9));
}

TEST_CASE("wave phase reduction stays accurate at a year of scene time")
{
    const double year = 365.0 * 24.0 * 60.0 * 60.0;
    TimeWave tw(WaveNode::Shape::Saw, 1.0);
    const double a = tw.evaluate(year);
    const double b = tw.evaluate(year + kStep);
    // Saw at 1 Hz advances by exactly one frame step per frame.
    CHECK(b - a == doctest::Approx(2.0 * kStep).epsilon(1e-6));
}
