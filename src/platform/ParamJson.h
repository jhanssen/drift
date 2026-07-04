#pragma once

// Shared JSON formatting for the control surfaces: the native WebSocket
// endpoint (ControlServer) and the wasm runtime's JS API (platform/web)
// serve the same describe document, so the editor sees one schema
// regardless of which runtime it is talking to.

#include <cstdio>
#include <string>
#include <vector>

#include "core/Scene.h"

namespace drift::platform {

inline std::string jsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(c);
            }
        }
    }
    return out;
}

inline std::string numberJson(double v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
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
                                const std::vector<core::SceneParam>& params)
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
    return out + "]}";
}

} // namespace drift::platform
