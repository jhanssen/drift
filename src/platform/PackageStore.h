#pragma once

// Package store resolution (SCENE_FORMAT.md §20.2), shared by the desktop
// and web runtimes. The loader canonicalizes pins into the path — a
// reference arrives here as "packages/<name>[@pin]/<rest>" — and this
// header turns it into a filesystem path: the project's own packages/
// directory wins, then the stores, searched in order, newest matching
// version within the first store that has one.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace drift::platform {

inline bool versionSegments(const std::string& v, std::vector<long>& out)
{
    out.clear();
    size_t start = 0;
    while (start <= v.size()) {
        const size_t dot = v.find('.', start);
        const std::string seg =
            v.substr(start, dot == std::string::npos ? dot : dot - start);
        if (seg.empty() ||
            seg.find_first_not_of("0123456789") != std::string::npos) {
            return false;
        }
        out.push_back(std::stol(seg));
        if (dot == std::string::npos) {
            return true;
        }
        start = dot + 1;
    }
    return false;
}

// pin is a version prefix (§20.3): "1" admits 1.x…, "1.2" admits 1.2.x…
inline bool versionMatchesPin(const std::vector<long>& version,
                              const std::vector<long>& pin)
{
    if (pin.size() > version.size()) {
        return false;
    }
    return std::equal(pin.begin(), pin.end(), version.begin());
}

// Dotted integer segments, missing segments zero (§20.2).
inline int versionCompare(const std::vector<long>& a,
                          const std::vector<long>& b)
{
    const size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const long x = i < a.size() ? a[i] : 0;
        const long y = i < b.size() ? b[i] : 0;
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return 0;
}

inline std::vector<std::filesystem::path> packageStores()
{
    std::vector<std::filesystem::path> stores;
    if (const char* env = std::getenv("DRIFT_PACKAGE_PATH")) {
        std::string all(env);
        size_t start = 0;
        while (start <= all.size()) {
            const size_t colon = all.find(':', start);
            const std::string entry = all.substr(
                start, colon == std::string::npos ? colon : colon - start);
            if (!entry.empty()) {
                stores.emplace_back(entry);
            }
            if (colon == std::string::npos) {
                break;
            }
            start = colon + 1;
        }
        return stores;
    }
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        stores.emplace_back(std::filesystem::path(xdg) / "drift" /
                            "packages");
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        stores.emplace_back(std::filesystem::path(home) / ".local" /
                            "share" / "drift" / "packages");
    }
    return stores;
}

// Resolve a project-relative path, which may begin with "packages/"
// (§20.2). Non-package paths map to root/rel without an existence check,
// preserving the pre-§20 contract. Returns false when the path is
// malformed, escapes the project, or names package content no store can
// provide.
inline bool resolveProjectPath(const std::filesystem::path& root,
                               const std::string& rel,
                               std::filesystem::path& out)
{
    namespace fs = std::filesystem;
    for (const auto& part : fs::path(rel)) {
        if (part == "..") {
            return false;
        }
    }
    if (rel.empty() || fs::path(rel).is_absolute()) {
        return false;
    }

    constexpr std::string_view kPrefix = "packages/";
    if (!rel.starts_with(kPrefix)) {
        out = root / rel;
        return true;
    }

    const size_t nameStart = kPrefix.size();
    const size_t slash = rel.find('/', nameStart);
    if (slash == std::string::npos || slash + 1 >= rel.size()) {
        return false;
    }
    std::string name = rel.substr(nameStart, slash - nameStart);
    const std::string rest = rel.substr(slash + 1);
    std::string pin;
    if (const size_t at = name.find('@'); at != std::string::npos) {
        pin = name.substr(at + 1);
        name = name.substr(0, at);
    }
    if (name.empty()) {
        return false;
    }

    std::error_code ec;

    // 1. Project-local copy wins, pin or no pin (§20.2).
    const fs::path local = root / "packages" / name / rest;
    if (fs::exists(local, ec)) {
        out = local;
        return true;
    }

    std::vector<long> pinSegs;
    if (!pin.empty() && !versionSegments(pin, pinSegs)) {
        return false;
    }

    // 2. Stores in order; newest matching version within the first store
    //    that holds one.
    for (const fs::path& store : packageStores()) {
        const fs::path dir = store / name;
        std::vector<long> best;
        fs::path bestPath;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_directory(ec)) {
                continue;
            }
            std::vector<long> segs;
            if (!versionSegments(entry.path().filename().string(), segs)) {
                continue;
            }
            if (!pin.empty() && !versionMatchesPin(segs, pinSegs)) {
                continue;
            }
            if (best.empty() || versionCompare(segs, best) > 0) {
                best = std::move(segs);
                bestPath = entry.path();
            }
        }
        if (!bestPath.empty()) {
            out = bestPath / rest;
            return true;
        }
    }
    return false;
}

} // namespace drift::platform
