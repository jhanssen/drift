#pragma once

// Shared JSON formatting for the control surfaces: the native WebSocket
// endpoint (ControlServer) and the wasm runtime's JS API (platform/web)
// serve the same describe document, so the editor sees one schema
// regardless of which runtime it is talking to.

#include <cstdio>
#include <string>
#include <vector>

#include "core/Json.h"
#include "core/Nodes.h"
#include "core/Scene.h"

namespace drift::platform {

// The fragment formatters live in core/Json.h so core serializers
// (nodePortsJson) emit the same forms; keep the unqualified names the
// callers here use.
using core::jsonEscape;
using core::numberJson;

// Copyable snapshot of a sequence node for describe (the native endpoint
// hands describe data across a callback boundary by value).
struct SequenceDesc {
    std::string id;
    double duration = 0.0;
    bool loop = true;
    std::vector<core::SequenceNode::Track> tracks;

    static SequenceDesc from(const core::SequenceNode& node)
    {
        return { node.id, node.duration(), node.loop(), node.tracks() };
    }
};

inline std::vector<SequenceDesc> sequenceDescs(const core::Scene& scene)
{
    std::vector<SequenceDesc> out;
    for (const core::SequenceNode* node : scene.sequences()) {
        out.push_back(SequenceDesc::from(*node));
    }
    return out;
}

// Scene-literal form (§4): scalar as a number, vecN as an array.
inline std::string valueJson(const core::Value& v)
{
    const int n = core::componentCount(v.type);
    if (n <= 1) {
        return numberJson(v.v[0]);
    }
    std::string out = "[";
    for (int i = 0; i < n; ++i) {
        if (i) {
            out += ",";
        }
        out += numberJson(v.v[i]);
    }
    return out + "]";
}

// The describe result (ControlServer.h): {"protocol":1,"scene":...}.
inline std::string describeJson(bool loaded, const std::string& name,
                                bool animated,
                                const std::vector<core::SceneParam>& params,
                                const std::vector<SequenceDesc>& sequences)
{
    std::string out = "{\"protocol\":1";
    if (!loaded) {
        return out + ",\"scene\":null}";
    }
    out += ",\"scene\":\"" + jsonEscape(name) + "\"";
    out += ",\"animated\":";
    out += animated ? "true" : "false";
    out += ",\"parameters\":[";
    for (size_t i = 0; i < params.size(); ++i) {
        const core::SceneParam& p = params[i];
        if (i) {
            out += ",";
        }
        out += "{\"name\":\"" + jsonEscape(p.name) + "\"";
        out += ",\"type\":\"";
        out += core::valueTypeName(p.type);
        out += "\",\"value\":" + valueJson(p.value);
        if (p.hasMin) {
            out += ",\"min\":" + valueJson(p.min);
        }
        if (p.hasMax) {
            out += ",\"max\":" + valueJson(p.max);
        }
        if (p.step != 0.0f) {
            out += ",\"step\":" + numberJson(p.step);
        }
        if (!p.label.empty()) {
            out += ",\"label\":\"" + jsonEscape(p.label) + "\"";
        }
        if (!p.hint.empty()) {
            out += ",\"hint\":\"" + jsonEscape(p.hint) + "\"";
        }
        out += "}";
    }
    out += "]";

    // Sequence timelines (§9.9) — what the editor's timeline panel draws.
    out += ",\"sequences\":[";
    for (size_t i = 0; i < sequences.size(); ++i) {
        const SequenceDesc& seq = sequences[i];
        if (i) {
            out += ",";
        }
        out += "{\"id\":\"" + jsonEscape(seq.id) + "\"";
        out += ",\"duration\":" + numberJson(seq.duration);
        out += ",\"loop\":";
        out += seq.loop ? "true" : "false";
        out += ",\"tracks\":[";
        for (size_t t = 0; t < seq.tracks.size(); ++t) {
            const core::SequenceNode::Track& track = seq.tracks[t];
            if (t) {
                out += ",";
            }
            out += "{\"name\":\"" + jsonEscape(track.name) + "\"";
            if (track.event) {
                out += ",\"kind\":\"event\",\"fires\":[";
                for (size_t f = 0; f < track.fires.size(); ++f) {
                    if (f) {
                        out += ",";
                    }
                    out += numberJson(track.fires[f]);
                }
                out += "]}";
                continue;
            }
            out += ",\"kind\":\"value\",\"type\":\"";
            out += core::valueTypeName(track.type);
            out += "\",\"interpolate\":\"";
            switch (track.interpolate) {
            case core::SequenceNode::Interpolate::Hold: out += "hold"; break;
            case core::SequenceNode::Interpolate::Linear: out += "linear"; break;
            case core::SequenceNode::Interpolate::Smooth: out += "smooth"; break;
            }
            out += "\",\"keys\":[";
            for (size_t k = 0; k < track.keys.size(); ++k) {
                if (k) {
                    out += ",";
                }
                out += "{\"t\":" + numberJson(track.keys[k].t);
                out += ",\"value\":" + valueJson(track.keys[k].value) + "}";
            }
            out += "]}";
        }
        out += "]}";
    }
    return out + "]}";
}

} // namespace drift::platform
