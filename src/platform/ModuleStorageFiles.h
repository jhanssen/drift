#pragma once

// §4.4 module storage persistence, native: one blob file per namespace
// under $XDG_STATE_HOME/drift/module-storage/<project-hash>/<ns>.bin,
// written atomically (tmp + rename). The project hash keys the store to
// the project's real path so two projects never share state; the
// namespace is the module node's document-unique id.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "core/Module.h"

namespace drift::platform {

class FileStoragePersistence : public drift::core::ModuleStoragePersistence {
public:
    explicit FileStoragePersistence(const std::string& projectRealPath)
    {
        uint64_t hash = 1469598103934665603ull;
        for (unsigned char c : projectRealPath) {
            hash = (hash ^ c) * 1099511628211ull;
        }
        char dir[32];
        snprintf(dir, sizeof(dir), "%016llx", (unsigned long long)hash);
        if (const char* state = getenv("XDG_STATE_HOME"); state && *state) {
            mDir = std::filesystem::path(state);
        } else if (const char* home = getenv("HOME"); home && *home) {
            mDir = std::filesystem::path(home) / ".local" / "state";
        } else {
            return; // no home: stays in-memory
        }
        mDir = mDir / "drift" / "module-storage" / dir;
    }

    bool load(const std::string& ns, std::string& blob) override
    {
        if (mDir.empty()) {
            return false;
        }
        std::ifstream in(mDir / (ns + ".bin"), std::ios::binary);
        if (!in) {
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        blob = ss.str();
        return true;
    }

    void save(const std::string& ns, const std::string& blob) override
    {
        if (mDir.empty()) {
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(mDir, ec);
        const std::filesystem::path dest = mDir / (ns + ".bin");
        const std::filesystem::path tmp = mDir / (ns + ".bin.tmp");
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                return;
            }
            out.write(blob.data(), (std::streamsize)blob.size());
            if (!out.good()) {
                return;
            }
        }
        std::filesystem::rename(tmp, dest, ec); // atomic on POSIX
    }

private:
    std::filesystem::path mDir;
};

} // namespace drift::platform
