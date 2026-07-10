#pragma once

// §4.4 project grant records: the state-dir grants file written by
// `driftpkg grant <project>` and only ever read here. One JSON object
// keyed by the project's canonical path:
//
//   { "/home/me/wall.sceneproject": { "permissions": { ... } } }
//
// Package modules do not use this — their grants travel in the store's
// .installed.json (read through the ordinary asset reader).

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <glaze/glaze.hpp>

#include "core/Module.h"

namespace drift::platform {

inline std::filesystem::path grantsFilePath()
{
    if (const char* state = getenv("XDG_STATE_HOME"); state && *state) {
        return std::filesystem::path(state) / "drift" / "grants.json";
    }
    if (const char* home = getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "state" / "drift" /
               "grants.json";
    }
    return {};
}

inline bool readProjectGrant(const std::string& projectRealPath,
                             drift::core::ModulePermissions& out)
{
    const std::filesystem::path file = grantsFilePath();
    if (file.empty()) {
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    glz::generic doc{};
    if (glz::read_json(doc, ss.str()) || !doc.is_object()) {
        return false;
    }
    const auto& top = doc.get_object();
    auto it = top.find(projectRealPath);
    if (it == top.end()) {
        return false;
    }
    // The entry has the grant-record shape; reuse its parser.
    auto record = glz::write_json(it->second);
    if (!record) {
        return false;
    }
    return drift::core::parseGrantRecord(*record, out);
}

} // namespace drift::platform
