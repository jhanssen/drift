#include <doctest/doctest.h>

#include <cstdlib>
#include <fstream>
#include <unistd.h>

#include "platform/PackageStore.h"

// §20.2 store resolution: project-local override, store order, version
// pick, pin prefixes. Uses a throwaway directory tree and points
// DRIFT_PACKAGE_PATH at it.

using drift::platform::resolveProjectPath;
namespace fs = std::filesystem;

namespace {

struct StoreFixture {
    fs::path base;
    fs::path project;
    fs::path storeA;
    fs::path storeB;

    StoreFixture()
    {
        base = fs::temp_directory_path() /
               ("drift-pkgstore-test-" + std::to_string(getpid()));
        fs::remove_all(base);
        project = base / "proj";
        storeA = base / "storeA";
        storeB = base / "storeB";
        fs::create_directories(project);
        setenv("DRIFT_PACKAGE_PATH",
               (storeA.string() + ":" + storeB.string()).c_str(), 1);
    }
    ~StoreFixture()
    {
        unsetenv("DRIFT_PACKAGE_PATH");
        std::error_code ec;
        fs::remove_all(base, ec);
    }

    void put(const fs::path& file)
    {
        fs::create_directories(file.parent_path());
        std::ofstream(file) << "x";
    }
};

} // namespace

TEST_CASE("package store resolution (§20.2)")
{
    StoreFixture fx;
    fs::path out;

    // Non-package paths pass through without an existence check.
    REQUIRE(resolveProjectPath(fx.project, "shaders/a.wgsl", out));
    CHECK(out == fx.project / "shaders/a.wgsl");
    CHECK(!resolveProjectPath(fx.project, "../escape", out));
    CHECK(!resolveProjectPath(fx.project, "/abs", out));

    // Unresolvable package.
    CHECK(!resolveProjectPath(fx.project, "packages/sway/graphs/s.json",
                              out));

    // Store versions: newest wins; numeric compare (1.10 > 1.9).
    fx.put(fx.storeA / "sway/1.9/graphs/s.json");
    fx.put(fx.storeA / "sway/1.10/graphs/s.json");
    REQUIRE(resolveProjectPath(fx.project, "packages/sway/graphs/s.json",
                               out));
    CHECK(out == fx.storeA / "sway/1.10/graphs/s.json");

    // Pin prefix: "1.9" beats the newer 1.10; "2" doesn't resolve.
    REQUIRE(resolveProjectPath(fx.project, "packages/sway@1.9/graphs/s.json",
                               out));
    CHECK(out == fx.storeA / "sway/1.9/graphs/s.json");
    CHECK(!resolveProjectPath(fx.project, "packages/sway@2/graphs/s.json",
                              out));

    // Store order: the first store holding a match wins even when a
    // later store has a newer version.
    fx.put(fx.storeB / "sway/3.0/graphs/s.json");
    REQUIRE(resolveProjectPath(fx.project, "packages/sway/graphs/s.json",
                               out));
    CHECK(out == fx.storeA / "sway/1.10/graphs/s.json");

    // ...but a pin only a later store satisfies falls through to it.
    REQUIRE(resolveProjectPath(fx.project, "packages/sway@3/graphs/s.json",
                               out));
    CHECK(out == fx.storeB / "sway/3.0/graphs/s.json");

    // Project-local copy beats every store, pin or no pin.
    fx.put(fx.project / "packages/sway/graphs/s.json");
    REQUIRE(resolveProjectPath(fx.project, "packages/sway/graphs/s.json",
                               out));
    CHECK(out == fx.project / "packages/sway/graphs/s.json");
    REQUIRE(resolveProjectPath(fx.project, "packages/sway@3/graphs/s.json",
                               out));
    CHECK(out == fx.project / "packages/sway/graphs/s.json");

    // Non-version directories in the store are ignored.
    fx.put(fx.storeA / "glow/notaversion/graphs/g.json");
    CHECK(!resolveProjectPath(fx.project, "packages/glow/graphs/g.json",
                              out));
}
