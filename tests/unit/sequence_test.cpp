#include <doctest/doctest.h>

#include <memory>
#include <vector>

#include "core/Nodes.h"

// sequence evaluation semantics (SCENE_FORMAT.md §9.9) and the video
// 'playing' port (§9.2). Neither touches the GPU: sequence is pure CPU, and
// the video tests use a decoder stub that returns no frame, so evaluate
// returns before any upload.

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

// A sequence driven through its 'time' input as a constant per evaluate.
struct SeqHarness {
    SequenceNode node;

    SeqHarness(double duration, bool loop, std::vector<SequenceNode::Track> tracks)
        : node(duration, loop, std::move(tracks))
    {
        node.inputs.resize(1);
        node.inputs[0].constant.type = ValueType::Scalar;
    }

    // Returns the track output values after evaluating at 'time'.
    const Value& at(double time, size_t track = 0)
    {
        for (auto& out : node.outputs) {
            out.dirty = false;
        }
        node.inputs[0].constant.v[0] = time;
        FrameContext ctx{};
        node.evaluate(ctx);
        node.firstEvaluate = false;
        return node.outputs[track].value;
    }
};

SequenceNode::Track scalarTrack(SequenceNode::Interpolate interp,
                                std::vector<std::pair<double, double>> keys)
{
    SequenceNode::Track track;
    track.name = "t";
    track.type = ValueType::Scalar;
    track.interpolate = interp;
    for (const auto& [t, v] : keys) {
        track.keys.push_back({ t, scalar(v) });
    }
    return track;
}

} // namespace

TEST_CASE("sequence hold steps at key times and wraps when looping")
{
    SeqHarness seq(40.0, true,
                   { scalarTrack(SequenceNode::Interpolate::Hold,
                                 { { 0, 0 }, { 10, 1 }, { 25, 0 } }) });
    CHECK(seq.at(5.0).v[0] == 0.0);
    CHECK(seq.at(10.0).v[0] == 1.0);   // key time belongs to the new segment
    CHECK(seq.at(24.9).v[0] == 1.0);
    CHECK(seq.at(25.0).v[0] == 0.0);
    CHECK(seq.at(39.9).v[0] == 0.0);
    CHECK(seq.at(50.0).v[0] == 1.0);   // wraps to local t=10
}

TEST_CASE("sequence holds the first key's value before it")
{
    SeqHarness seq(40.0, true,
                   { scalarTrack(SequenceNode::Interpolate::Linear,
                                 { { 5, 3 }, { 10, 7 } }) });
    CHECK(seq.at(2.0).v[0] == 3.0);
    CHECK(seq.at(20.0).v[0] == 7.0); // and the last key's after it
}

TEST_CASE("sequence linear and smooth interpolate between keys")
{
    SeqHarness lin(10.0, false,
                   { scalarTrack(SequenceNode::Interpolate::Linear,
                                 { { 0, 0 }, { 10, 1 } }) });
    CHECK(lin.at(5.0).v[0] == doctest::Approx(0.5));
    CHECK(lin.at(2.5).v[0] == doctest::Approx(0.25));

    SeqHarness smooth(10.0, false,
                      { scalarTrack(SequenceNode::Interpolate::Smooth,
                                    { { 0, 0 }, { 10, 1 } }) });
    CHECK(smooth.at(5.0).v[0] == doctest::Approx(0.5));
    CHECK(smooth.at(2.5).v[0] == doctest::Approx(0.15625)); // f²(3−2f), f=0.25
}

TEST_CASE("sequence clamps local time when not looping")
{
    SeqHarness seq(10.0, false,
                   { scalarTrack(SequenceNode::Interpolate::Linear,
                                 { { 0, 0 }, { 10, 1 } }) });
    CHECK(seq.at(15.0).v[0] == 1.0);
    CHECK(seq.at(-5.0).v[0] == 0.0);
}

TEST_CASE("sequence interpolates vector tracks per component")
{
    SequenceNode::Track track;
    track.name = "pos";
    track.type = ValueType::Vec2;
    track.interpolate = SequenceNode::Interpolate::Linear;
    track.keys.push_back({ 0.0, vec2(0.0, 1.0) });
    track.keys.push_back({ 10.0, vec2(1.0, 0.0) });
    SeqHarness seq(10.0, false, { track });

    const Value& v = seq.at(2.5);
    CHECK(v.v[0] == doctest::Approx(0.25));
    CHECK(v.v[1] == doctest::Approx(0.75));
}

TEST_CASE("hold tracks are dirty only on key transitions")
{
    SeqHarness seq(40.0, true,
                   { scalarTrack(SequenceNode::Interpolate::Hold,
                                 { { 0, 0.25 }, { 10, 1 } }) });
    seq.at(1.0);
    CHECK(seq.node.outputs[0].dirty);   // first write of a changed value
    seq.at(2.0);
    CHECK(!seq.node.outputs[0].dirty);  // same segment, same value
    seq.at(10.5);
    CHECK(seq.node.outputs[0].dirty);   // crossed the key
}

namespace {

SequenceNode::Track eventTrack(std::vector<double> fires)
{
    SequenceNode::Track track;
    track.name = "cue";
    track.event = true;
    track.fires = std::move(fires);
    return track;
}

} // namespace

TEST_CASE("event tracks fire on cue crossings (§9.9)")
{
    SeqHarness seq(8.0, true, { eventTrack({ 2.0, 5.0 }) });
    CHECK(seq.node.outputs[0].value.type == ValueType::Event);
    seq.at(1.0); // first evaluation: [0, 1] holds no cue
    CHECK(!seq.node.outputs[0].dirty);
    seq.at(2.0); // (1, 2] contains 2
    CHECK(seq.node.outputs[0].dirty);
    seq.at(3.0);
    CHECK(!seq.node.outputs[0].dirty); // a fire is one frame only
    seq.at(5.5); // (3, 5.5] contains 5
    CHECK(seq.node.outputs[0].dirty);
    seq.at(7.9);
    CHECK(!seq.node.outputs[0].dirty);
    seq.at(10.5); // wrap to local 2.5: (7.9, 8) ∪ [0, 2.5] contains 2
    CHECK(seq.node.outputs[0].dirty);
}

TEST_CASE("event track first evaluation fires cues at or before now")
{
    SeqHarness seq(8.0, true, { eventTrack({ 2.0 }) });
    seq.at(3.0); // [0, 3] contains 2
    CHECK(seq.node.outputs[0].dirty);
}

TEST_CASE("sequence local-time reduction survives a month of scene time")
{
    // A 40 s linear ramp equal to local time. 2'592'000 is an exact multiple
    // of 40, so the expected output at month+step is exactly one frame step;
    // an f32 reduction rounds it away.
    constexpr double kMonth = 30.0 * 24.0 * 60.0 * 60.0;
    constexpr double kStep = 1.0 / 60.0;
    SeqHarness seq(40.0, true,
                   { scalarTrack(SequenceNode::Interpolate::Linear,
                                 { { 0, 0 }, { 40, 40 } }) });
    CHECK(seq.at(kMonth + kStep).v[0] == doctest::Approx(kStep).epsilon(1e-9));
}

// ---- video playing ----

namespace {

struct RecordingDecoder : VideoDecoder {
    std::vector<double> calls;
    int restarts = 0;
    VideoFrame frame; // served when serve is set; else "decode error"
    bool serve = false;
    const VideoFrame* frameAt(double seconds) override
    {
        calls.push_back(seconds);
        return serve ? &frame : nullptr;
    }
    void restart() override { ++restarts; }
};

// An event producer standing in for a sequence cue track.
struct EventSource : Node {
    EventSource()
    {
        outputs.resize(1);
        outputs[0].value.type = ValueType::Event;
    }
    void evaluate(FrameContext&) override {}
};

struct VideoHarness {
    RecordingDecoder* decoder;
    std::unique_ptr<VideoNode> node;
    EventSource cue;

    VideoHarness()
    {
        auto owned = std::make_unique<RecordingDecoder>();
        decoder = owned.get();
        node = std::make_unique<VideoNode>(std::move(owned));
        node->inputs.resize(2); // playing, restart
        node->inputs[0].constant.type = ValueType::Scalar;
        node->inputs[1].type = ValueType::Event;
        node->inputs[1].srcNode = &cue;
        node->inputs[1].srcPort = 0;
    }

    void frame(double seconds, bool playing, bool fireRestart = false)
    {
        cue.outputs[0].dirty = fireRestart;
        for (auto& out : node->outputs) {
            out.dirty = false;
        }
        node->inputs[0].constant.v[0] = playing ? 1.0 : 0.0;
        FrameContext ctx{};
        ctx.seconds = seconds;
        node->evaluate(ctx);
        node->firstEvaluate = false;
    }
};

} // namespace

TEST_CASE("video pulls the decoder at scene time while playing")
{
    VideoHarness v;
    v.frame(0.0, true);
    v.frame(1.0, true);
    v.frame(2.0, true);
    CHECK(v.decoder->calls == std::vector<double>{ 0.0, 1.0, 2.0 });
}

TEST_CASE("video pause freezes the playback clock and stops decode pulls")
{
    VideoHarness v;
    v.frame(0.0, true);
    v.frame(1.0, true);
    v.frame(2.0, false); // paused: no pulls, position frozen at 1.0
    v.frame(3.0, false);
    v.frame(4.0, false);
    v.frame(5.0, true);  // resumes one frame-delta past the freeze point
    CHECK(v.decoder->calls == std::vector<double>{ 0.0, 1.0, 2.0 });
}

TEST_CASE("video starting paused decodes a poster frame only")
{
    VideoHarness v;
    v.frame(0.0, false); // first evaluate always decodes (poster)
    v.frame(1.0, false);
    v.frame(2.0, false);
    v.frame(3.0, true);
    CHECK(v.decoder->calls == std::vector<double>{ 0.0, 1.0 });
}

TEST_CASE("video restart resets the playback clock and seeks the decoder")
{
    VideoHarness v;
    v.frame(0.0, true);
    v.frame(1.0, true);
    v.frame(2.0, true);
    v.frame(3.0, true, /*fireRestart=*/true);
    CHECK(v.decoder->restarts == 1);
    CHECK(v.decoder->calls == std::vector<double>{ 0.0, 1.0, 2.0, 0.0 });
    v.frame(4.0, true);
    CHECK(v.decoder->calls.back() == 1.0); // continues from the restart
}

TEST_CASE("restart while paused pulls one poster frame (§9.2)")
{
    VideoHarness v;
    v.frame(0.0, true);
    v.frame(1.0, false);
    v.frame(2.0, false);
    CHECK(v.decoder->calls == std::vector<double>{ 0.0 });
    v.frame(3.0, false, /*fireRestart=*/true);
    CHECK(v.decoder->restarts == 1);
    CHECK(v.decoder->calls == std::vector<double>{ 0.0, 0.0 });
    v.frame(4.0, false); // still paused: no further pulls
    CHECK(v.decoder->calls.size() == 2);
}

TEST_CASE("video fires finished once and holds until restart (§17.4)")
{
    VideoHarness v;
    v.decoder->serve = true;
    v.decoder->frame.index = -1; // == initial mLastIndex: skips GPU upload
    v.frame(0.0, true);
    CHECK(!v.node->outputs[1].dirty);
    v.decoder->frame.last = true;
    v.frame(1.0, true);
    CHECK(v.node->outputs[1].dirty); // final frame produced -> fire
    v.frame(2.0, true);
    CHECK(!v.node->outputs[1].dirty); // held; no re-fire
    CHECK(v.decoder->calls.size() == 2); // and no decoder pulls while held
    v.decoder->frame.last = false;
    v.frame(3.0, true, /*fireRestart=*/true); // rearms playback
    CHECK(v.decoder->calls.size() == 3);
    CHECK(v.decoder->calls.back() == 0.0);
}
