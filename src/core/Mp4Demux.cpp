#include "Mp4Demux.h"

#include <algorithm>
#include <cstdio>

// ISO/IEC 14496-12 box structure; 14496-15 (avcC/hvcC) and the AV1-in-ISOBMFF
// spec (av1C) for the sample-entry config records; RFC 6381 + ISO 14496-15
// Annex E for the codec strings.

namespace drift::core {

namespace {

// Big-endian cursor over a byte range. Out-of-range reads latch `ok` false
// and return zero, so parse code can read a whole structure and check once.
struct Reader {
    const uint8_t* data;
    size_t begin, end;
    size_t pos;
    bool ok = true;

    Reader(const uint8_t* base, size_t from, size_t to)
        : data(base), begin(from), end(to), pos(from)
    {
    }

    size_t remaining() const { return ok && pos < end ? end - pos : 0; }

    bool need(size_t n)
    {
        if (!ok || end - pos < n || pos + n < pos) {
            ok = false;
            return false;
        }
        return true;
    }

    uint32_t u8() { return need(1) ? data[pos++] : 0; }
    uint32_t u16() { const uint32_t hi = u8(); return hi << 8 | u8(); }
    uint32_t u32() { const uint32_t hi = u16(); return hi << 16 | u16(); }
    uint64_t u64() { const uint64_t hi = u32(); return hi << 32 | u32(); }
    void skip(size_t n)
    {
        if (need(n)) {
            pos += n;
        }
    }
};

struct BoxRange {
    size_t begin = 0, end = 0; // payload range
    bool found() const { return end != 0; }
};

constexpr uint32_t fourcc(const char (&s)[5])
{
    return (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 |
           (uint32_t)s[2] << 8 | (uint32_t)s[3];
}

// Walks the boxes within [begin, end), calling visit(type, payloadBegin,
// payloadEnd). Returns false on a malformed size field.
template <typename Visit>
bool walkBoxes(const uint8_t* data, size_t begin, size_t end, Visit&& visit)
{
    size_t pos = begin;
    while (pos + 8 <= end) {
        Reader head(data, pos, end);
        uint64_t size = head.u32();
        const uint32_t type = head.u32();
        if (size == 1) {
            size = head.u64(); // 64-bit largesize
        } else if (size == 0) {
            size = end - pos; // extends to the end of the enclosing box
        }
        if (!head.ok || size < head.pos - pos || pos + size > end) {
            return false;
        }
        visit(type, head.pos, pos + size);
        pos += size;
    }
    return true;
}

std::string hex2(uint32_t v)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%02x", v & 0xff);
    return buf;
}

// 14496-15 E.3: general_profile_compatibility_flags appear in the codec
// string with their bits in reverse order, printed as hex without leading
// zeros.
std::string reversedBitsHex(uint32_t flags)
{
    uint32_t reversed = 0;
    for (int i = 0; i < 32; i++) {
        reversed = reversed << 1 | (flags >> i & 1);
    }
    char buf[12];
    snprintf(buf, sizeof(buf), "%x", reversed);
    return buf;
}

std::string codecStringAvc(const std::vector<uint8_t>& avcc)
{
    if (avcc.size() < 4) {
        return {};
    }
    return "avc1." + hex2(avcc[1]) + hex2(avcc[2]) + hex2(avcc[3]);
}

std::string codecStringHevc(const std::vector<uint8_t>& hvcc)
{
    if (hvcc.size() < 13) {
        return {};
    }
    const uint8_t byte1 = hvcc[1];
    const uint32_t profileSpace = byte1 >> 6;
    const bool highTier = (byte1 >> 5 & 1) != 0;
    const uint32_t profileIdc = byte1 & 0x1f;
    const uint32_t compat = (uint32_t)hvcc[2] << 24 | (uint32_t)hvcc[3] << 16 |
                            (uint32_t)hvcc[4] << 8 | hvcc[5];
    const uint8_t* constraints = &hvcc[6]; // 6 bytes
    const uint32_t levelIdc = hvcc[12];

    std::string s = "hvc1.";
    if (profileSpace) {
        s += (char)('A' + profileSpace - 1);
    }
    s += std::to_string(profileIdc);
    s += "." + reversedBitsHex(compat);
    s += highTier ? ".H" : ".L";
    s += std::to_string(levelIdc);
    int lastNonZero = 5;
    while (lastNonZero >= 0 && constraints[lastNonZero] == 0) {
        lastNonZero--;
    }
    for (int i = 0; i <= lastNonZero; i++) {
        s += "." + hex2(constraints[i]);
    }
    return s;
}

std::string codecStringAv1(const std::vector<uint8_t>& av1c)
{
    if (av1c.size() < 4) {
        return {};
    }
    const uint32_t profile = av1c[1] >> 5;
    const uint32_t level = av1c[1] & 0x1f;
    const bool highTier = (av1c[2] >> 7 & 1) != 0;
    const bool highBitDepth = (av1c[2] >> 6 & 1) != 0;
    const bool twelveBit = (av1c[2] >> 5 & 1) != 0;
    const uint32_t bits = highBitDepth ? (twelveBit ? 12 : 10) : 8;
    char buf[24];
    snprintf(buf, sizeof(buf), "av01.%u.%02u%c.%02u", profile, level,
             highTier ? 'H' : 'M', bits);
    return buf;
}

struct SampleTables {
    BoxRange stsd, stts, ctts, stsc, stsz, stco, co64, stss;
};

} // namespace

bool parseMp4(const uint8_t* data, size_t size, Mp4Track& track,
              std::string& error)
{
    error.clear();
    BoxRange moov;
    bool fragmented = false;
    if (!walkBoxes(data, 0, size, [&](uint32_t type, size_t b, size_t e) {
            if (type == fourcc("moov") && !moov.found()) {
                moov = { b, e };
            } else if (type == fourcc("moof")) {
                fragmented = true;
            }
        })) {
        error = "mp4: malformed box structure";
        return false;
    }
    if (fragmented) {
        error = "mp4: fragmented (moof) files are not supported";
        return false;
    }
    if (!moov.found()) {
        error = "mp4: no moov box (not an mp4, or streaming layout)";
        return false;
    }

    // Locate the first video trak's mdhd + stbl.
    BoxRange stbl, mdhd;
    walkBoxes(data, moov.begin, moov.end,
              [&](uint32_t type, size_t b, size_t e) {
        if (type != fourcc("trak") || stbl.found()) {
            return;
        }
        BoxRange trakMdhd, trakStbl;
        bool video = false;
        walkBoxes(data, b, e, [&](uint32_t t2, size_t b2, size_t e2) {
            if (t2 != fourcc("mdia")) {
                return;
            }
            walkBoxes(data, b2, e2, [&](uint32_t t3, size_t b3, size_t e3) {
                if (t3 == fourcc("mdhd")) {
                    trakMdhd = { b3, e3 };
                } else if (t3 == fourcc("hdlr")) {
                    Reader r(data, b3, e3);
                    r.skip(8); // version/flags + pre_defined
                    video = r.u32() == fourcc("vide");
                } else if (t3 == fourcc("minf")) {
                    walkBoxes(data, b3, e3,
                              [&](uint32_t t4, size_t b4, size_t e4) {
                        if (t4 == fourcc("stbl")) {
                            trakStbl = { b4, e4 };
                        }
                    });
                }
            });
        });
        if (video && trakStbl.found() && trakMdhd.found()) {
            stbl = trakStbl;
            mdhd = trakMdhd;
        }
    });
    if (!stbl.found()) {
        error = "mp4: no video track";
        return false;
    }

    // Media timescale and duration.
    Reader mdhdReader(data, mdhd.begin, mdhd.end);
    const uint32_t mdhdVersion = mdhdReader.u8();
    mdhdReader.skip(3);                          // flags
    mdhdReader.skip(mdhdVersion == 1 ? 16 : 8);  // creation/modification
    const uint32_t timescale = mdhdReader.u32();
    const uint64_t mediaDuration =
        mdhdVersion == 1 ? mdhdReader.u64() : mdhdReader.u32();
    if (!mdhdReader.ok || timescale == 0) {
        error = "mp4: malformed mdhd";
        return false;
    }

    SampleTables tables;
    walkBoxes(data, stbl.begin, stbl.end,
              [&](uint32_t type, size_t b, size_t e) {
        BoxRange range{ b, e };
        if (type == fourcc("stsd")) tables.stsd = range;
        else if (type == fourcc("stts")) tables.stts = range;
        else if (type == fourcc("ctts")) tables.ctts = range;
        else if (type == fourcc("stsc")) tables.stsc = range;
        else if (type == fourcc("stsz")) tables.stsz = range;
        else if (type == fourcc("stco")) tables.stco = range;
        else if (type == fourcc("co64")) tables.co64 = range;
        else if (type == fourcc("stss")) tables.stss = range;
    });
    if (!tables.stsd.found() || !tables.stts.found() ||
        !tables.stsc.found() || !tables.stsz.found() ||
        (!tables.stco.found() && !tables.co64.found())) {
        error = "mp4: missing sample tables";
        return false;
    }

    // stsd: the first sample entry decides the codec.
    {
        Reader r(data, tables.stsd.begin, tables.stsd.end);
        r.skip(4); // version/flags
        if (r.u32() == 0) {
            error = "mp4: empty stsd";
            return false;
        }
        const size_t entryStart = r.pos;
        const uint64_t entrySize = r.u32();
        const uint32_t format = r.u32();
        if (!r.ok || entrySize < 8 || entryStart + entrySize > tables.stsd.end) {
            error = "mp4: malformed stsd entry";
            return false;
        }
        const size_t entryEnd = entryStart + entrySize;
        const bool avc = format == fourcc("avc1") || format == fourcc("avc3");
        const bool hevc = format == fourcc("hvc1") || format == fourcc("hev1");
        const bool av1 = format == fourcc("av01");
        if (!avc && !hevc && !av1) {
            const char name[5] = { (char)(format >> 24), (char)(format >> 16),
                                   (char)(format >> 8), (char)format, 0 };
            error = std::string("mp4: unsupported codec '") + name +
                    "' (avc1/hvc1/av01 are supported)";
            return false;
        }
        // VisualSampleEntry: 6 reserved + 2 data_reference_index, then
        // 16 bytes of pre_defined/reserved before width/height.
        r.skip(8 + 16);
        track.width = r.u16();
        track.height = r.u16();
        r.skip(4 + 4 + 4 + 2 + 32 + 2 + 2); // resolutions .. pre_defined
        if (!r.ok) {
            error = "mp4: malformed sample entry";
            return false;
        }
        std::vector<uint8_t> config;
        const uint32_t wantBox = avc    ? fourcc("avcC")
                               : hevc   ? fourcc("hvcC")
                                        : fourcc("av1C");
        walkBoxes(data, r.pos, entryEnd,
                  [&](uint32_t type, size_t b, size_t e) {
            if (type == wantBox && config.empty()) {
                config.assign(data + b, data + e);
            }
        });
        if (config.empty()) {
            error = "mp4: sample entry has no decoder configuration box";
            return false;
        }
        track.codec = avc    ? codecStringAvc(config)
                    : hevc   ? codecStringHevc(config)
                             : codecStringAv1(config);
        if (track.codec.empty()) {
            error = "mp4: malformed decoder configuration";
            return false;
        }
        if (!av1) {
            track.description = std::move(config); // in-band for AV1
        }
    }

    // stts → decode timestamps; ctts → composition offsets on top.
    std::vector<uint64_t> dts;
    {
        Reader r(data, tables.stts.begin, tables.stts.end);
        r.skip(4);
        const uint32_t entries = r.u32();
        uint64_t t = 0;
        for (uint32_t i = 0; i < entries && r.ok; i++) {
            const uint32_t count = r.u32();
            const uint32_t delta = r.u32();
            if (dts.size() + count > 4u << 20) {
                r.ok = false; // absurd sample count; refuse
                break;
            }
            for (uint32_t k = 0; k < count; k++) {
                dts.push_back(t);
                t += delta;
            }
        }
        if (!r.ok || dts.empty()) {
            error = "mp4: malformed stts";
            return false;
        }
    }
    std::vector<double> pts(dts.size());
    for (size_t i = 0; i < dts.size(); i++) {
        pts[i] = (double)dts[i];
    }
    if (tables.ctts.found()) {
        Reader r(data, tables.ctts.begin, tables.ctts.end);
        const uint32_t version = r.u8();
        r.skip(3);
        const uint32_t entries = r.u32();
        size_t sample = 0;
        for (uint32_t i = 0; i < entries && r.ok; i++) {
            const uint32_t count = r.u32();
            const uint32_t raw = r.u32();
            const double offset =
                version == 1 ? (double)(int32_t)raw : (double)raw;
            for (uint32_t k = 0; k < count && sample < pts.size(); k++) {
                pts[sample++] += offset;
            }
        }
        if (!r.ok) {
            error = "mp4: malformed ctts";
            return false;
        }
    }

    // stsz sizes.
    std::vector<uint32_t> sizes;
    {
        Reader r(data, tables.stsz.begin, tables.stsz.end);
        r.skip(4);
        const uint32_t constantSize = r.u32();
        const uint32_t count = r.u32();
        if (!r.ok || count != dts.size()) {
            error = "mp4: stsz/stts sample counts disagree";
            return false;
        }
        sizes.resize(count);
        for (uint32_t i = 0; i < count; i++) {
            sizes[i] = constantSize ? constantSize : r.u32();
        }
        if (!r.ok) {
            error = "mp4: malformed stsz";
            return false;
        }
    }

    // stsc runs + stco/co64 chunk offsets → per-sample file offsets.
    std::vector<uint64_t> offsets(sizes.size());
    {
        struct Run {
            uint32_t firstChunk, samplesPerChunk;
        };
        std::vector<Run> runs;
        Reader r(data, tables.stsc.begin, tables.stsc.end);
        r.skip(4);
        const uint32_t entries = r.u32();
        for (uint32_t i = 0; i < entries && r.ok; i++) {
            const uint32_t first = r.u32();
            const uint32_t perChunk = r.u32();
            r.u32(); // sample_description_index; single-entry stsd assumed
            runs.push_back({ first, perChunk });
        }
        const bool use64 = tables.co64.found();
        Reader c(data, use64 ? tables.co64.begin : tables.stco.begin,
                 use64 ? tables.co64.end : tables.stco.end);
        c.skip(4);
        const uint32_t chunkCount = c.u32();
        if (!r.ok || runs.empty() || !c.ok) {
            error = "mp4: malformed stsc/stco";
            return false;
        }
        size_t sample = 0;
        size_t run = 0;
        for (uint32_t chunk = 1; chunk <= chunkCount && sample < sizes.size();
             chunk++) {
            const uint64_t chunkOffset = use64 ? c.u64() : c.u32();
            while (run + 1 < runs.size() && runs[run + 1].firstChunk <= chunk) {
                run++;
            }
            uint64_t offset = chunkOffset;
            for (uint32_t k = 0;
                 k < runs[run].samplesPerChunk && sample < sizes.size(); k++) {
                offsets[sample] = offset;
                offset += sizes[sample];
                sample++;
            }
        }
        if (!c.ok || sample != sizes.size()) {
            error = "mp4: sample-to-chunk mapping does not cover all samples";
            return false;
        }
    }

    // stss keyframes (absent = every sample is a sync sample).
    std::vector<bool> keyframes(sizes.size(), !tables.stss.found());
    if (tables.stss.found()) {
        Reader r(data, tables.stss.begin, tables.stss.end);
        r.skip(4);
        const uint32_t count = r.u32();
        for (uint32_t i = 0; i < count && r.ok; i++) {
            const uint32_t sampleNumber = r.u32(); // 1-based
            if (sampleNumber >= 1 && sampleNumber <= keyframes.size()) {
                keyframes[sampleNumber - 1] = true;
            }
        }
        if (!r.ok) {
            error = "mp4: malformed stss";
            return false;
        }
    }

    // Assemble, normalizing pts so the earliest sample presents at 0 (the
    // common single-entry edit list does exactly this shift; full elst
    // handling is out of scope).
    const double earliest = *std::min_element(pts.begin(), pts.end());
    track.samples.resize(sizes.size());
    for (size_t i = 0; i < sizes.size(); i++) {
        Mp4Sample& s = track.samples[i];
        s.offset = offsets[i];
        s.size = sizes[i];
        s.pts = (pts[i] - earliest) / timescale;
        s.keyframe = keyframes[i];
        if (s.offset + s.size > size) {
            error = "mp4: sample data extends past the end of the file";
            return false;
        }
    }
    if (!track.samples.front().keyframe) {
        error = "mp4: first sample is not a sync sample";
        return false;
    }
    track.duration = mediaDuration ? (double)mediaDuration / timescale : 0.0;
    if (track.duration == 0.0) {
        for (const Mp4Sample& s : track.samples) {
            track.duration = std::max(track.duration, s.pts);
        }
    }
    return true;
}

} // namespace drift::core
