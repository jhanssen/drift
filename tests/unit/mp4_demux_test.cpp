#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

#include "core/Mp4Demux.h"

// Mp4Demux tests against real encoder output: the handoff example videos
// plus tiny fixtures generated with ffmpeg (tests/fixtures/). Expected
// values cross-checked with ffprobe at fixture-generation time.

using drift::core::Mp4Track;
using drift::core::parseMp4;

namespace {

std::vector<uint8_t> readFile(const std::string& relPath)
{
    std::ifstream in(std::string(DRIFT_SOURCE_DIR) + "/" + relPath,
                     std::ios::binary);
    REQUIRE_MESSAGE(in.good(), relPath.c_str());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string s = ss.str();
    return { s.begin(), s.end() };
}

struct Parsed {
    Mp4Track track;
    std::string error;
    bool ok;
};

Parsed parse(const std::string& relPath)
{
    Parsed p;
    const auto bytes = readFile(relPath);
    p.ok = parseMp4(bytes.data(), bytes.size(), p.track, p.error);
    return p;
}

void checkWellFormed(const Mp4Track& track, size_t fileSize)
{
    REQUIRE(!track.samples.empty());
    CHECK(track.samples.front().keyframe);
    CHECK(track.duration > 0.0);
    double maxPts = -1.0;
    double minPts = 1e9;
    for (const auto& s : track.samples) {
        CHECK(s.size > 0);
        CHECK(s.offset + s.size <= fileSize);
        maxPts = std::max(maxPts, s.pts);
        minPts = std::min(minPts, s.pts);
    }
    CHECK(minPts == 0.0); // normalized presentation start
    CHECK(maxPts < track.duration + 0.5);
}

} // namespace

TEST_CASE("mp4: handoff h264 assets parse (a.mp4: 160x90, 90 frames)")
{
    auto p = parse("examples/handoff.sceneproject/assets/a.mp4");
    CAPTURE(p.error);
    REQUIRE(p.ok);
    CHECK(p.track.codec == "avc1.64000b"); // High profile, level 1.1
    CHECK(p.track.width == 160);
    CHECK(p.track.height == 90);
    CHECK(p.track.samples.size() == 90);
    CHECK(!p.track.description.empty());
    CHECK(p.track.description[0] == 1); // avcC configurationVersion
    checkWellFormed(p.track,
                    readFile("examples/handoff.sceneproject/assets/a.mp4").size());
}

TEST_CASE("mp4: av1 fixture parses with an av01 codec string, no description")
{
    auto p = parse("tests/fixtures/tiny_av1.mp4");
    CAPTURE(p.error);
    REQUIRE(p.ok);
    CHECK(p.track.codec == "av01.0.00M.08"); // Main profile, level 2.0, 8bit
    CHECK(p.track.width == 128);
    CHECK(p.track.height == 72);
    CHECK(p.track.samples.size() == 12);
    CHECK(p.track.description.empty()); // AV1 config is in-band
    const auto key = std::count_if(p.track.samples.begin(),
                                   p.track.samples.end(),
                                   [](const auto& s) { return s.keyframe; });
    CHECK(key == 2); // g=6 over 12 frames
    checkWellFormed(p.track, readFile("tests/fixtures/tiny_av1.mp4").size());
}

TEST_CASE("mp4: h265 fixture parses with an hvc1 codec string")
{
    auto p = parse("tests/fixtures/tiny_h265.mp4");
    CAPTURE(p.error);
    REQUIRE(p.ok);
    CHECK(p.track.codec.substr(0, 12) == "hvc1.1.6.L30"); // Main, level 1.0
    CHECK(!p.track.description.empty());
    CHECK(p.track.samples.size() == 12);
    checkWellFormed(p.track, readFile("tests/fixtures/tiny_h265.mp4").size());
}

TEST_CASE("mp4: b-frames give pts != decode order but normalize to 0")
{
    auto p = parse("tests/fixtures/tiny_h264_bframes.mp4");
    CAPTURE(p.error);
    REQUIRE(p.ok);
    CHECK(p.track.samples.size() == 12);
    // With bf=2 the decode order differs from presentation order: pts must
    // not be monotonic in sample (decode) order...
    bool monotonic = true;
    for (size_t i = 1; i < p.track.samples.size(); i++) {
        monotonic &= p.track.samples[i].pts > p.track.samples[i - 1].pts;
    }
    CHECK(!monotonic);
    // ...yet the sorted timestamps still form the expected 10fps grid.
    std::vector<double> sorted;
    for (const auto& s : p.track.samples) {
        sorted.push_back(s.pts);
    }
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 0; i < sorted.size(); i++) {
        CHECK(sorted[i] == doctest::Approx(i * 0.1).epsilon(0.01));
    }
}

TEST_CASE("mp4: unsupported envelopes are refused with a reason")
{
    auto fragmented = parse("tests/fixtures/tiny_fragmented.mp4");
    CHECK(!fragmented.ok);
    CHECK(fragmented.error.find("fragmented") != std::string::npos);

    const uint8_t garbage[] = "this is not an mp4 file at all, sorry";
    Mp4Track track;
    std::string error;
    CHECK(!parseMp4(garbage, sizeof(garbage), track, error));
    CHECK(!error.empty());

    // Truncation mid-moov must fail cleanly, never crash.
    auto bytes = readFile("tests/fixtures/tiny_av1.mp4");
    for (const size_t keep : { size_t{100}, bytes.size() / 4 }) {
        Mp4Track t;
        std::string e;
        CHECK(!parseMp4(bytes.data(), keep, t, e));
    }
}
