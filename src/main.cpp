#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <fcntl.h>
#include <unistd.h>

#include "core/Renderer.h"
#include "core/Scene.h"
#include "platform/ControlServer.h"
#include "platform/ModuleNetCurl.h"
#include "platform/ModuleStorageFiles.h"
#include "platform/ModuleWasmtime.h"
#include "platform/PackageStore.h"
#include "platform/ProjectGrants.h"
#include "platform/VideoDecoderFFmpeg.h"

#ifdef __APPLE__
#include "platform/mac/CocoaApp.h"
#include "platform/mac/Gpu.h"
using App = drift::platform::CocoaApp;
#else
#include <sys/eventfd.h>

#include "platform/linux/Gpu.h"
#include "platform/linux/WaylandApp.h"
using App = drift::platform::WaylandApp;
#endif

namespace {

void usage(const char* argv0)
{
    fprintf(stderr,
            "usage: %s [scene.sceneproject] [options]\n"
#ifdef __APPLE__
            "  (default)          run as wallpaper (not yet available on\n"
            "                     macOS; pass --windowed)\n"
#else
            "  (default)          run as wallpaper (wlr-layer-shell background)\n"
#endif
            "  -w, --windowed     run in a regular window (dev mode)\n"
            "  -f, --fullscreen   run as a fullscreen window (preview without\n"
            "                     other windows covering the wallpaper)\n"
            "      --headless N   render N frames offscreen and write PNGs\n"
            "      --frames LIST  only write the comma-separated frame indices\n"
            "                     (still evaluates every frame up to the last;\n"
            "                     implies --headless if N is omitted)\n"
            "      --mouse X,Y    headless: fixed pointer position in [0,1]\n"
            "                     output space, active (default: inactive)\n"
            "      --set NAME=V   override a scene parameter (V: number or\n"
            "                     comma-separated components); repeatable\n"
            "      --size WxH     initial window size / headless resolution\n"
            "                     (default 1280x720 windowed, 1920x1080 headless)\n"
            "      --output NAMES wallpaper: only claim the named outputs\n"
            "                     (comma-separated connector names, e.g. DP-1;\n"
            "                     repeatable; default: every output)\n"
            "      --output NAME=SCENE\n"
            "                     wallpaper: run SCENE on output NAME\n"
            "                     (repeatable; replaces the scene argument;\n"
            "                     unlisted outputs are left unclaimed)\n"
            "      --out DIR      output directory for --headless (default .)\n"
            "      --listen PORT  WebSocket control endpoint on 127.0.0.1:PORT\n"
            "                     (describe/set; not available with --headless)\n"
            "  -h, --help         show this help\n"
            "\n"
            "Without a scene, renders a builtin placeholder gradient.\n"
            "Frames advance scene time at a fixed 1/60s step; headless exits\n"
            "nonzero if any GPU error occurred.\n",
            argv0);
}

using ParamOverride = std::pair<std::string, drift::core::Value>;

// NAME=V where V is a number or 2-4 comma-separated components.
bool parseParamOverride(const char* arg, ParamOverride& out)
{
    const char* eq = strchr(arg, '=');
    if (!eq || eq == arg) {
        return false;
    }
    out.first.assign(arg, eq - arg);
    drift::core::Value& v = out.second;
    int n = 0;
    const char* p = eq + 1;
    while (n < 4) {
        char* end = nullptr;
        v.v[n] = strtod(p, &end);
        if (end == p) {
            return false;
        }
        ++n;
        if (*end == '\0') {
            break;
        }
        if (*end != ',') {
            return false;
        }
        p = end + 1;
    }
    switch (n) {
    case 1: v.type = drift::core::ValueType::Scalar; break;
    case 2: v.type = drift::core::ValueType::Vec2; break;
    case 3: v.type = drift::core::ValueType::Vec3; break;
    case 4: v.type = drift::core::ValueType::Vec4; break;
    default: return false;
    }
    return true;
}

// Headless/golden runs keep module storage in-memory (§4.4: deterministic
// empty start); set at the top of runHeadless.
static bool gHeadless = false;

// §4.4 network transport, wayland runs only — headless/golden stays
// offline (null backend: the offline face). One curl-multi thread for
// the whole process; scene ModuleNets co-own it via shared_ptr.
static std::shared_ptr<drift::core::ModuleNetBackend> gNetBackend;
// Thread-safe "a module wake is pending" signal: writes the run loop's
// eventfd. Deliveries fire it from the network thread.
static std::function<void()> gRequestFrame;

// Loads a .sceneproject directory (or a scene.json path) with project-root
// confinement for asset reads.
//
// sceneJsonInOut (optional): non-empty = load this document instead of the
// on-disk scene.json (assets still resolve from the project dir; this is
// how the editor pushes edits); empty = read from disk and fill it in.
// errorsOut (optional) receives the loader's error messages.
std::unique_ptr<drift::core::Scene> loadScene(
    const std::string& scenePath, const wgpu::Device& device,
    std::string* sceneJsonInOut = nullptr,
    std::vector<std::string>* errorsOut = nullptr)
{
    namespace fs = std::filesystem;
    fs::path root(scenePath);
    if (fs::is_regular_file(root)) {
        root = root.parent_path();
    }
    const fs::path sceneFile = root / "scene.json";

    // Project-root confinement plus §20.2 package-store resolution.
    auto confined = [root](const std::string& relPath, fs::path& out) -> bool {
        return drift::platform::resolveProjectPath(root, relPath, out);
    };

    auto readAsset = [confined](const std::string& relPath, std::string& out) -> bool {
        fs::path path;
        if (!confined(relPath, path)) {
            return false;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    };

    auto videoFactory = [confined](const std::string& relPath, bool loop,
                                   std::string& error)
        -> std::unique_ptr<drift::core::VideoDecoder> {
        fs::path path;
        if (!confined(relPath, path)) {
            error = "path escapes the project";
            return nullptr;
        }
        return drift::platform::createFFmpegVideoDecoder(path.string(), loop,
                                                         error);
    };

    std::string sceneJson;
    if (sceneJsonInOut && !sceneJsonInOut->empty()) {
        sceneJson = *sceneJsonInOut;
    } else if (!readAsset("scene.json", sceneJson)) {
        fprintf(stderr, "drift: cannot read %s\n", sceneFile.c_str());
        if (errorsOut) {
            errorsOut->push_back("cannot read scene.json");
        }
        return nullptr;
    }

    // §4.4 project grants: `driftpkg grant <project>` records the granted
    // policy in the state-dir grants file, keyed by the project's real
    // path; package grants ride the store's .installed.json via readAsset.
    auto projectGrants =
        [root](drift::core::ModulePermissions& out) -> bool {
        std::error_code ec;
        const fs::path real = fs::canonical(root, ec);
        if (ec) {
            return false;
        }
        return drift::platform::readProjectGrant(real.string(), out);
    };

    std::vector<std::string> errors, warnings;
    drift::core::ModulePlatform modules{
        drift::platform::wasmtimeModuleLoader(), projectGrants, nullptr,
        gNetBackend, gRequestFrame
    };
    // §4.4 storage persistence — except headless/golden runs, whose
    // stores stay in-memory for a deterministic empty start every run.
    if (!gHeadless) {
        std::error_code ec;
        const fs::path real = fs::canonical(root, ec);
        modules.storage =
            std::make_shared<drift::platform::FileStoragePersistence>(
                (ec ? root : real).string());
    }
    auto scene = drift::core::Scene::load(sceneJson, readAsset, videoFactory,
                                          modules, device, errors, warnings);
    for (const auto& w : warnings) {
        fprintf(stderr, "drift: scene warning: %s\n", w.c_str());
    }
    for (const auto& e : errors) {
        fprintf(stderr, "drift: scene: %s\n", e.c_str());
    }
    if (scene && sceneJsonInOut) {
        *sceneJsonInOut = sceneJson;
    }
    if (errorsOut) {
        *errorsOut = errors;
    }
    if (scene) {
        printf("drift: loaded scene '%s'\n", scene->name().c_str());
    }
    return scene;
}

// --set overrides apply to every scene that declares the parameter (one
// per-output group need not declare another's, §17.6); acceptedOut
// collects the names this scene took, so callers can reject an override
// no scene declares.
void applyOverrides(drift::core::Scene& scene,
                    const std::vector<ParamOverride>& overrides,
                    std::set<std::string>* acceptedOut = nullptr)
{
    for (const auto& [name, value] : overrides) {
        if (scene.setParameter(name, value) && acceptedOut) {
            acceptedOut->insert(name);
        }
    }
}

// writeFrames: frame indices to write as PNGs; empty = all. Every frame up
// to the last is evaluated regardless, since dirty state (and later,
// feedback loops) depends on frame history.
struct MouseArg {
    float x = 0.5f, y = 0.5f;
    bool active = false;
};

int runHeadless(const std::string& scenePath, int frames, uint32_t width,
                uint32_t height, const std::string& outDir,
                const std::set<int>& writeFrames, const MouseArg& mouse,
                const std::vector<ParamOverride>& overrides)
{
    gHeadless = true;
    drift::platform::Gpu gpu;
    if (!gpu.init(/*needPresent=*/false)) {
        return 1;
    }

    const wgpu::Device& device = gpu.device();
    const wgpu::TextureFormat format = wgpu::TextureFormat::RGBA8Unorm;

    std::unique_ptr<drift::core::Scene> scene;
    drift::core::Renderer placeholder;
    if (!scenePath.empty()) {
        scene = loadScene(scenePath, device);
        if (!scene) {
            return 1;
        }
        std::set<std::string> accepted;
        applyOverrides(*scene, overrides, &accepted);
        for (const auto& [name, value] : overrides) {
            if (!accepted.contains(name)) {
                fprintf(stderr,
                        "drift: --set: no parameter '%s' of that type\n",
                        name.c_str());
                return 1;
            }
        }
    } else if (!placeholder.init(device, format)) {
        fprintf(stderr, "drift: renderer init failed\n");
        return 1;
    }

    wgpu::TextureDescriptor td{};
    td.format = format;
    td.size = { width, height, 1 };
    td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
    wgpu::Texture texture = device.CreateTexture(&td);

    const uint32_t bytesPerRow = ((width * 4) + 255) & ~255u; // 256-align
    wgpu::BufferDescriptor bd{};
    bd.size = (uint64_t)bytesPerRow * height;
    bd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    wgpu::Buffer readback = device.CreateBuffer(&bd);

    int presentedCount = 0;
    for (int i = 0; i < frames; ++i) {
        const double t = i / 60.0;
        if (scene) {
            drift::core::FrameContext ctx{};
            ctx.device = device;
            ctx.seconds = t;
            ctx.mouseX = mouse.x;
            ctx.mouseY = mouse.y;
            ctx.mouseActive = mouse.active;
            ctx.target = texture.CreateView();
            ctx.targetWidth = width;
            ctx.targetHeight = height;
            ctx.targetFormat = format;
            if (scene->render(ctx)) {
                ++presentedCount;
            }
        } else {
            placeholder.render(texture.CreateView(), (float)t);
            ++presentedCount;
        }

        if (!writeFrames.empty() && !writeFrames.contains(i)) {
            continue;
        }

        wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
        wgpu::TexelCopyTextureInfo src{};
        src.texture = texture;
        wgpu::TexelCopyBufferInfo dst{};
        dst.buffer = readback;
        dst.layout.bytesPerRow = bytesPerRow;
        dst.layout.rowsPerImage = height;
        wgpu::Extent3D extent = { width, height, 1 };
        encoder.CopyTextureToBuffer(&src, &dst, &extent);
        wgpu::CommandBuffer commands = encoder.Finish();
        device.GetQueue().Submit(1, &commands);

        bool mapped = false, ok = false;
        readback.MapAsync(wgpu::MapMode::Read, 0, bd.size,
                          wgpu::CallbackMode::AllowProcessEvents,
                          [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
                              mapped = true;
                              ok = (status == wgpu::MapAsyncStatus::Success);
                          });
        while (!mapped) {
            gpu.processEvents();
        }
        if (!ok) {
            fprintf(stderr, "drift: readback map failed\n");
            return 1;
        }

        const uint8_t* pixels =
            (const uint8_t*)readback.GetConstMappedRange(0, bd.size);
        char path[1024];
        snprintf(path, sizeof(path), "%s/frame_%04d.png", outDir.c_str(), i);
        if (!stbi_write_png(path, (int)width, (int)height, 4, pixels,
                            (int)bytesPerRow)) {
            fprintf(stderr, "drift: failed to write %s\n", path);
            return 1;
        }
        readback.Unmap();
        printf("drift: wrote %s\n", path);
    }
    gpu.waitForQueue();
    if (gpu.hasError()) {
        fprintf(stderr, "drift: GPU errors occurred during headless render\n");
        return 1;
    }
    // Skipped frames left the target untouched, which is the efficiency
    // contract working (§11) — golden.sh can assert on this line.
    printf("drift: presented %d of %d frames\n", presentedCount, frames);
    return 0;
}

int runApp(const std::string& scenePath, drift::platform::SurfaceMode mode,
           uint32_t width, uint32_t height,
           const std::vector<ParamOverride>& overrides, uint16_t listenPort,
           const std::vector<std::string>& outputFilter,
           const std::vector<std::pair<std::string, std::string>>& outputScenes)
{
    drift::platform::Gpu gpu;
    if (!gpu.init(/*needPresent=*/true)) {
        return 1;
    }

    // §4.4 network wakes. The wake fd outlives the scenes (declared
    // first): ModuleNet deliveries may fire gRequestFrame from the
    // network thread until the last scene's teardown cancels them.
    // Linux: an eventfd. Elsewhere: a nonblocking self-pipe (a full
    // pipe already holds a pending wake, so a dropped write is fine).
    struct WakeFd {
        int readFd = -1, writeFd = -1;
        WakeFd()
        {
#ifdef __linux__
            readFd = writeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
#else
            int fds[2];
            if (pipe(fds) == 0) {
                for (const int fd : fds) {
                    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
                    fcntl(fd, F_SETFD, FD_CLOEXEC);
                }
                readFd = fds[0];
                writeFd = fds[1];
            }
#endif
        }
        ~WakeFd()
        {
            if (readFd >= 0) {
                close(readFd);
            }
            if (writeFd >= 0 && writeFd != readFd) {
                close(writeFd);
            }
        }
    } wakeFd;
    gNetBackend = drift::platform::createCurlNetBackend();
    gRequestFrame = [fd = wakeFd.writeFd] {
        if (fd >= 0) {
            const uint64_t one = 1;
            (void)!write(fd, &one, sizeof(one));
        }
    };

    // Which scene runs on which output: named entries from --output
    // NAME=SCENE, or one ""-keyed default (the positional scene) for every
    // claimed output. Empty = builtin placeholder.
    std::map<std::string, std::string> scenePaths;
    for (const auto& [name, path] : outputScenes) {
        scenePaths[name] = path;
    }
    if (scenePaths.empty() && !scenePath.empty()) {
        scenePaths[""] = scenePath;
    }

    // Declared before app: app teardown destroys surfaces, which fires the
    // output-removed callback into this map, so it must still be alive then.
    // preloaded likewise — the wake-query callback captures both.
    std::map<uint32_t, std::unique_ptr<drift::core::Scene>> scenes;
    // Validation instances, keyed like scenePaths; each is moved into
    // `scenes` when its output's first frame arrives.
    std::map<std::string, std::unique_ptr<drift::core::Scene>> preloaded;
    // The document each entry currently runs: the on-disk scene.json, or
    // the last one pushed over the control endpoint ("load"). Every
    // instance creation path uses it, so hotplug and seek-rebuilds stay on
    // the same document as the visible outputs.
    std::map<std::string, std::string> currentDocs;

    App app;
#ifndef __APPLE__
    // Wallpaper mode only claims the outputs scenes were assigned to;
    // empty = all of them. (macOS has no wallpaper mode yet, and main()
    // refuses --output there.)
    std::vector<std::string> filter = outputFilter;
    for (const auto& [name, path] : outputScenes) {
        filter.push_back(name);
    }
    app.setOutputFilter(std::move(filter));
#else
    (void)outputFilter;
#endif
    if (!app.setup(gpu, mode, width, height)) {
        return 1;
    }
    app.setWakeFd(wakeFd.readFd);

    // Outputs can differ in size, so each gets its own scene instance (with
    // its own graph state and clock); the first output on each entry reuses
    // that entry's validation load. nullptr in the scenes map = a load that
    // failed; don't retry per frame.
    drift::core::Renderer placeholder;
    std::set<std::string> acceptedOverrides;
    for (const auto& [key, path] : scenePaths) {
        auto scene = loadScene(path, gpu.device(), &currentDocs[key]);
        if (!scene) {
            return 1;
        }
        applyOverrides(*scene, overrides, &acceptedOverrides);
        preloaded[key] = std::move(scene);
    }
    for (const auto& [name, value] : overrides) {
        if (!scenePaths.empty() && !acceptedOverrides.contains(name)) {
            fprintf(stderr,
                    "drift: --set: no parameter '%s' of that type in any "
                    "scene\n",
                    name.c_str());
            return 1;
        }
    }
    if (scenePaths.empty() &&
        !placeholder.init(gpu.device(), app.targetFormat())) {
        fprintf(stderr, "drift: renderer init failed\n");
        return 1;
    }

    const wgpu::Device device = gpu.device();
    const wgpu::TextureFormat format = app.targetFormat();
    // Animation is decided over every validation instance — including ones
    // dropped below — so a named output hotplugging later still ticks.
    bool animated = preloaded.empty(); // the placeholder animates
    for (const auto& [key, scene] : preloaded) {
        animated = animated || scene->animated();
    }

#ifndef __APPLE__
    // Validation instances for named outputs that are absent would idle
    // indefinitely (video decoder threads, module instances) — drop them;
    // the frame callback lazy-loads from the validated document when the
    // output attaches.
    {
        const auto claimed = app.claimedOutputs();
        std::erase_if(preloaded, [&claimed](const auto& entry) {
            if (entry.first.empty()) {
                return claimed.empty(); // default entry: keep if any output
            }
            return std::find(claimed.begin(), claimed.end(), entry.first) ==
                   claimed.end();
        });
    }
#endif

    // §4.4 module timers: the run loop asks each output's scene for its
    // earliest wake_after_ms deadline to size its idle sleep.
    app.setWakeQuery([&scenes, &preloaded](uint32_t outputId) -> double {
        auto it = scenes.find(outputId);
        if (it != scenes.end()) {
            return it->second ? it->second->nextWake() : -1.0;
        }
        // Not instantiated yet: earliest across the validation instances.
        double next = -1.0;
        for (const auto& [key, scene] : preloaded) {
            const double wake = scene ? scene->nextWake() : -1.0;
            if (wake >= 0.0 && (next < 0.0 || wake < next)) {
                next = wake;
            }
        }
        return next;
    });

    app.setOutputRemoved([&scenes](uint32_t outputId) {
        scenes.erase(outputId);
    });

    // Parameter values set at runtime (control endpoint). Applied to scene
    // instances created later (output hotplug, reload) so parameters stay
    // shared across outputs (§17.6).
    std::map<std::string, drift::core::Value> runtimeParams;

    // Control endpoint (--listen): parameters are shared across outputs
    // (§17.6), so a set applies to every instance.
    drift::platform::ControlServer control;
    if (listenPort) {
        auto anyScene = [&]() -> drift::core::Scene* {
            for (auto& [id, scene] : scenes) {
                if (scene) {
                    return scene.get();
                }
            }
            for (auto& [key, scene] : preloaded) {
                if (scene) {
                    return scene.get();
                }
            }
            return nullptr;
        };
        drift::platform::ControlServer::Callbacks callbacks;
        // Union across instance groups (§17.6): every settable parameter
        // and sequence is discoverable whichever outputs are attached;
        // duplicates (the same document on N outputs) collapse by name.
        callbacks.describe = [&scenes, &preloaded] {
            drift::platform::ControlServer::SceneInfo info;
            auto add = [&info](drift::core::Scene& scene) {
                if (!info.loaded) {
                    info.loaded = true;
                    info.name = scene.name();
                }
                info.animated = info.animated || scene.animated();
                for (const auto& p : scene.parameters()) {
                    const bool seen = std::any_of(
                        info.parameters.begin(), info.parameters.end(),
                        [&p](const auto& q) { return q.name == p.name; });
                    if (!seen) {
                        info.parameters.push_back(p);
                    }
                }
                for (auto& seq : drift::platform::sequenceDescs(scene)) {
                    const bool seen = std::any_of(
                        info.sequences.begin(), info.sequences.end(),
                        [&seq](const auto& other) { return other.id == seq.id; });
                    if (!seen) {
                        info.sequences.push_back(std::move(seq));
                    }
                }
            };
            for (auto& [id, scene] : scenes) {
                if (scene) {
                    add(*scene);
                }
            }
            for (auto& [key, scene] : preloaded) {
                if (scene) {
                    add(*scene);
                }
            }
            return info;
        };
        callbacks.setParameter = [anyScene, &preloaded, &scenes, &app,
                                  &runtimeParams](
                                     const std::string& name,
                                     const drift::core::Value& value,
                                     std::string& error,
                                     drift::core::Value& applied) {
            if (!anyScene()) {
                error = "no scene loaded";
                return false;
            }
            drift::core::Scene* accepted = nullptr;
            auto apply = [&](drift::core::Scene& scene) {
                if (scene.setParameter(name, value) && !accepted) {
                    accepted = &scene;
                }
            };
            for (auto& [key, scene] : preloaded) {
                if (scene) {
                    apply(*scene);
                }
            }
            for (auto& [id, scene] : scenes) {
                if (scene) {
                    apply(*scene);
                }
            }
            if (!accepted) {
                error = "no parameter '" + name + "' of that type";
                return false;
            }
            // Post-clamp value (§6) read back from a scene that took the
            // set — not an arbitrary group, which may not declare the
            // parameter at all.
            for (const auto& p : accepted->parameters()) {
                if (p.name == name) {
                    applied = p.value;
                    break;
                }
            }
            runtimeParams[name] = applied;
            app.requestRedrawAll();
            return true;
        };
        callbacks.time = [&app] { return app.currentSceneTime(); };
        callbacks.paused = [&app] { return app.scenePaused(); };
        callbacks.setPaused = [&app](bool paused) {
            app.setScenePaused(paused);
        };

        // Fresh instances for every entry, keeping runtime parameter
        // values; the clock is left alone (reload/load) or set by the
        // caller (seek). newJson: a pushed document (single-document runs
        // only — the load callback rejects it under --output NAME=SCENE);
        // otherwise each entry rebuilds from its current document, or
        // re-reads its scene.json when rereadDisk. The old instances keep
        // running if any new document fails to load.
        auto applyDocuments = [&scenePaths, &device, &overrides, &scenes,
                               &preloaded, &runtimeParams, &currentDocs,
                               &app](const std::string* newJson,
                                     bool rereadDisk, std::string& error) {
            if (scenePaths.empty()) {
                error = "no scene loaded";
                return false;
            }
            std::map<std::string, std::unique_ptr<drift::core::Scene>> fresh;
            std::map<std::string, std::string> docs;
            for (const auto& [key, path] : scenePaths) {
                std::string json = newJson      ? *newJson
                                   : rereadDisk ? std::string()
                                                : currentDocs[key];
                std::vector<std::string> errors;
                auto scene = loadScene(path, device, &json, &errors);
                if (!scene) {
                    error = errors.empty() ? "scene load failed" : errors[0];
                    return false;
                }
                applyOverrides(*scene, overrides);
                for (const auto& [name, value] : runtimeParams) {
                    scene->setParameter(name, value);
                }
                fresh[key] = std::move(scene);
                docs[key] = std::move(json);
            }
            preloaded = std::move(fresh);
            currentDocs = std::move(docs);
            scenes.clear();
            app.requestRedrawAll();
            return true;
        };
        callbacks.seek = [&app, applyDocuments](double seconds,
                                                std::string& error) {
            // Backward violates §9.7 within an instance; re-create on the
            // same documents instead.
            if (seconds < app.currentSceneTime() &&
                !applyDocuments(nullptr, /*rereadDisk=*/false, error)) {
                return false;
            }
            app.seekSceneTime(seconds);
            return true;
        };
        callbacks.reload = [applyDocuments](std::string& error) {
            return applyDocuments(nullptr, /*rereadDisk=*/true, error);
        };
        // source/load push one document, so they only serve single-document
        // runs; a per-output-scene process is not an editing target.
        callbacks.source = [&currentDocs] {
            auto it = currentDocs.find("");
            return it != currentDocs.end() ? it->second : std::string();
        };
        callbacks.load = [&scenePaths, applyDocuments](
                             const std::string& sceneJson,
                             std::string& error) {
            if (!scenePaths.contains("")) {
                error = scenePaths.empty()
                            ? "no scene loaded"
                            : "load: not supported with per-output scenes";
                return false;
            }
            return applyDocuments(&sceneJson, /*rereadDisk=*/false, error);
        };
        // Project file access (write-asset/read-asset): same confinement
        // as the loader's AssetReader — project-root relative, no "..",
        // no absolute paths. Writes create directories, like the web
        // runtime's drift_write_asset; a follow-up load/reload applies.
        auto confinedPath = [&scenePath, &outputScenes](
                                const std::string& rel,
                                std::filesystem::path& out,
                                std::string& error) {
            namespace fs = std::filesystem;
            if (scenePath.empty()) {
                // Asset access needs the single project root; a
                // per-output-scene process has several.
                error = outputScenes.empty()
                            ? "no scene loaded"
                            : "not supported with per-output scenes";
                return false;
            }
            for (const auto& part : fs::path(rel)) {
                if (part == "..") {
                    error = "path escapes the project";
                    return false;
                }
            }
            if (rel.empty() || fs::path(rel).is_absolute()) {
                error = "path must be project-relative";
                return false;
            }
            fs::path root(scenePath);
            if (fs::is_regular_file(root)) {
                root = root.parent_path();
            }
            out = root / rel;
            return true;
        };
        callbacks.writeAsset = [confinedPath](const std::string& rel,
                                              const std::string& contents,
                                              std::string& error) {
            std::filesystem::path full;
            if (!confinedPath(rel, full, error)) {
                return false;
            }
            std::error_code ec;
            std::filesystem::create_directories(full.parent_path(), ec);
            std::ofstream out(full, std::ios::binary | std::ios::trunc);
            if (!out || !(out << contents).good()) {
                error = "cannot write '" + rel + "'";
                return false;
            }
            return true;
        };
        // Reads also resolve through the package store (§20.6) so the
        // editor can drill into package graphs; writes stay project-only.
        callbacks.readAsset = [&scenePath, &outputScenes](
                                  const std::string& rel, std::string& out,
                                  std::string& error) {
            namespace fs = std::filesystem;
            if (scenePath.empty()) {
                error = outputScenes.empty()
                            ? "no scene loaded"
                            : "not supported with per-output scenes";
                return false;
            }
            fs::path root(scenePath);
            if (fs::is_regular_file(root)) {
                root = root.parent_path();
            }
            fs::path full;
            if (!drift::platform::resolveProjectPath(root, rel, full)) {
                error = "cannot resolve '" + rel + "'";
                return false;
            }
            std::ifstream in(full, std::ios::binary);
            if (!in) {
                error = "cannot read '" + rel + "'";
                return false;
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            out = ss.str();
            return true;
        };
        callbacks.fire = [anyScene, &preloaded, &scenes, &app](
                             const std::string& node, const std::string& port,
                             std::string& error) {
            if (!anyScene()) {
                error = "no scene loaded";
                return false;
            }
            bool ok = false;
            for (auto& [key, scene] : preloaded) {
                if (scene) {
                    ok = scene->fireEvent(node, port) || ok;
                }
            }
            for (auto& [id, scene] : scenes) {
                if (scene) {
                    ok = scene->fireEvent(node, port) || ok;
                }
            }
            if (!ok) {
                error = "no event output '" + node + "." + port + "'";
                return false;
            }
            app.requestRedrawAll();
            return true;
        };
        std::string error;
        if (!control.start(listenPort, std::move(callbacks), error)) {
            fprintf(stderr, "drift: control: %s\n", error.c_str());
            return 1;
        }
        app.setControl(control.fd(), [&control] { control.drive(); });
        printf("drift: control on ws://127.0.0.1:%u\n", listenPort);
    }

    return app.run(
        [&](const App::FrameRequest& req) -> bool {
            if (scenePaths.empty()) {
                placeholder.render(req.target, (float)req.seconds);
                return true;
            }
            auto it = scenes.find(req.outputId);
            if (it == scenes.end()) {
#ifdef __APPLE__
                // Single window; --output is refused on macOS so far.
                const std::string key;
#else
                // The entry assigned to this output, else the ""-keyed
                // default (the positional scene).
                const std::string key =
                    scenePaths.contains(req.outputName) ? req.outputName
                                                        : std::string();
#endif
                std::unique_ptr<drift::core::Scene> instance;
                if (auto pre = preloaded.find(key); pre != preloaded.end()) {
                    instance = std::move(pre->second);
                    preloaded.erase(pre);
                } else if (auto path = scenePaths.find(key);
                           path != scenePaths.end()) {
                    // Late instance (hotplug/reload): same document,
                    // matching runtime state.
                    instance = loadScene(path->second, device,
                                         &currentDocs[key]);
                    if (instance) {
                        applyOverrides(*instance, overrides);
                        for (const auto& [name, value] : runtimeParams) {
                            instance->setParameter(name, value);
                        }
                    }
                }
                it = scenes.emplace(req.outputId, std::move(instance)).first;
            }
            if (!it->second) {
                return false;
            }
            drift::core::FrameContext ctx{};
            ctx.device = device;
            ctx.seconds = req.seconds;
            ctx.mouseX = req.mouseX;
            ctx.mouseY = req.mouseY;
            ctx.mouseActive = req.mouseActive;
            ctx.target = req.target;
            ctx.targetWidth = req.width;
            ctx.targetHeight = req.height;
            ctx.targetFormat = format;
            return it->second->render(ctx);
        },
        animated);
}

} // namespace

int main(int argc, char** argv)
{
    bool windowed = false;
    bool fullscreen = false;
    int headlessFrames = -1;
    uint16_t listenPort = 0;
    uint32_t width = 0, height = 0;
    std::string outDir = ".";
    std::string scenePath;
    std::set<int> writeFrames;
    MouseArg mouse;
    std::vector<ParamOverride> overrides;
    std::vector<std::string> outputFilter;                          // --output NAME
    std::vector<std::pair<std::string, std::string>> outputScenes;  // --output NAME=SCENE

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!strcmp(arg, "-w") || !strcmp(arg, "--windowed")) {
            windowed = true;
        } else if (!strcmp(arg, "-f") || !strcmp(arg, "--fullscreen")) {
            fullscreen = true;
        } else if (!strcmp(arg, "--headless") && i + 1 < argc) {
            headlessFrames = atoi(argv[++i]);
        } else if (!strcmp(arg, "--frames") && i + 1 < argc) {
            std::stringstream ss(argv[++i]);
            std::string item;
            while (std::getline(ss, item, ',')) {
                char* end = nullptr;
                const long v = strtol(item.c_str(), &end, 10);
                if (end == item.c_str() || *end != '\0' || v < 0) {
                    fprintf(stderr, "drift: bad --frames entry '%s'\n", item.c_str());
                    return 2;
                }
                writeFrames.insert((int)v);
            }
            if (writeFrames.empty()) {
                usage(argv[0]);
                return 2;
            }
        } else if (!strcmp(arg, "--set") && i + 1 < argc) {
            ParamOverride override;
            if (!parseParamOverride(argv[++i], override)) {
                fprintf(stderr, "drift: bad --set '%s' (want NAME=V)\n", argv[i]);
                return 2;
            }
            overrides.push_back(std::move(override));
        } else if (!strcmp(arg, "--mouse") && i + 1 < argc) {
            if (sscanf(argv[++i], "%f,%f", &mouse.x, &mouse.y) != 2) {
                usage(argv[0]);
                return 2;
            }
            mouse.active = true;
        } else if (!strcmp(arg, "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%ux%u", &width, &height) != 2) {
                usage(argv[0]);
                return 2;
            }
        } else if (!strcmp(arg, "--output") && i + 1 < argc) {
            const char* val = argv[++i];
            if (const char* eq = strchr(val, '=')) {
                std::string name(val, eq - val);
                std::string scene(eq + 1);
                if (name.empty() || scene.empty()) {
                    fprintf(stderr, "drift: bad --output '%s' (want NAME=SCENE)\n", val);
                    return 2;
                }
                for (const auto& [existing, s] : outputScenes) {
                    if (existing == name) {
                        fprintf(stderr, "drift: duplicate --output for '%s'\n",
                                name.c_str());
                        return 2;
                    }
                }
                outputScenes.emplace_back(std::move(name), std::move(scene));
            } else {
                std::stringstream ss(val);
                std::string name;
                while (std::getline(ss, name, ',')) {
                    if (name.empty()) {
                        fprintf(stderr, "drift: bad --output '%s'\n", val);
                        return 2;
                    }
                    outputFilter.push_back(name);
                }
            }
        } else if (!strcmp(arg, "--out") && i + 1 < argc) {
            outDir = argv[++i];
        } else if (!strcmp(arg, "--listen") && i + 1 < argc) {
            char* end = nullptr;
            const long port = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || port < 1 || port > 65535) {
                fprintf(stderr, "drift: bad --listen port '%s'\n", argv[i]);
                return 2;
            }
            listenPort = (uint16_t)port;
        } else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            usage(argv[0]);
            return 0;
        } else if (arg[0] != '-' && scenePath.empty()) {
            scenePath = arg;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!outputScenes.empty() && !outputFilter.empty()) {
        fprintf(stderr,
                "drift: cannot mix --output NAME and --output NAME=SCENE\n");
        return 2;
    }
    if (!outputScenes.empty() && !scenePath.empty()) {
        fprintf(stderr,
                "drift: --output NAME=SCENE replaces the scene argument\n");
        return 2;
    }
    if ((!outputScenes.empty() || !outputFilter.empty()) &&
        (windowed || fullscreen || headlessFrames >= 0 || !writeFrames.empty())) {
        fprintf(stderr, "drift: --output requires wallpaper mode\n");
        return 2;
    }
    if (!writeFrames.empty() && headlessFrames < 0) {
        headlessFrames = *writeFrames.rbegin() + 1;
    }
    if (headlessFrames >= 0) {
        if (listenPort) {
            fprintf(stderr, "drift: --listen is not available with --headless\n");
            return 2;
        }
        if (width == 0) { width = 1920; height = 1080; }
        return runHeadless(scenePath, headlessFrames, width, height, outDir,
                           writeFrames, mouse, overrides);
    }
    if (width == 0) { width = 1280; height = 720; }
#ifdef __APPLE__
    // Wallpaper (the default) and fullscreen are not implemented on macOS
    // yet; refuse rather than silently run something else.
    if (!windowed || fullscreen) {
        fprintf(stderr, "drift: only --windowed (and --headless) runs are "
                        "supported on macOS so far\n");
        return 2;
    }
#endif
    return runApp(scenePath,
                  fullscreen ? drift::platform::SurfaceMode::Fullscreen
                  : windowed ? drift::platform::SurfaceMode::Windowed
                             : drift::platform::SurfaceMode::Wallpaper,
                  width, height, overrides, listenPort, outputFilter,
                  outputScenes);
}
