#pragma once

// Shared JSON fragment formatting for the hand-rolled serializers (the
// control surfaces' describe via platform/ParamJson.h, drift_node_ports
// via nodePortsJson): one number/string form so the same value reads
// identically through every endpoint.

#include <cmath>
#include <cstdio>
#include <string>

namespace drift::core {

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
    if (!std::isfinite(v)) {
        return "0"; // JSON has no nan/inf; a zero beats invalid output
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

} // namespace drift::core
