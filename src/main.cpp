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

#include "core/Renderer.h"
#include "core/Scene.h"
#include "platform/linux/Gpu.h"
#include "platform/linux/VideoDecoderFFmpeg.h"
#include "platform/linux/WaylandApp.h"

namespace {

void usage(const char* argv0)
{
    fprintf(stderr,
            "usage: %s [scene.sceneproject] [options]\n"
            "  (default)          run as wallpaper (wlr-layer-shell background)\n"
            "  -w, --windowed     run in a regular window (dev mode)\n"
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
            "      --out DIR      output directory for --headless (default .)\n"
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
        v.v[n] = strtof(p, &end);
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

// Loads a .sceneproject directory (or a scene.json path) with project-root
// confinement for asset reads.
std::unique_ptr<drift::core::Scene> loadScene(const std::string& scenePath,
                                              const wgpu::Device& device,
                                              const std::vector<ParamOverride>& overrides)
{
    namespace fs = std::filesystem;
    fs::path root(scenePath);
    if (fs::is_regular_file(root)) {
        root = root.parent_path();
    }
    const fs::path sceneFile = root / "scene.json";

    auto confined = [root](const std::string& relPath, fs::path& out) -> bool {
        for (const auto& part : fs::path(relPath)) {
            if (part == "..") {
                return false;
            }
        }
        if (fs::path(relPath).is_absolute()) {
            return false;
        }
        out = root / relPath;
        return true;
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
    if (!readAsset("scene.json", sceneJson)) {
        fprintf(stderr, "drift: cannot read %s\n", sceneFile.c_str());
        return nullptr;
    }

    std::vector<std::string> errors;
    auto scene = drift::core::Scene::load(sceneJson, readAsset, videoFactory,
                                          device, errors);
    for (const auto& e : errors) {
        fprintf(stderr, "drift: scene: %s\n", e.c_str());
    }
    if (scene) {
        for (const auto& [name, value] : overrides) {
            if (!scene->setParameter(name, value)) {
                fprintf(stderr, "drift: --set: no parameter '%s' of that type\n",
                        name.c_str());
                return nullptr;
            }
        }
        printf("drift: loaded scene '%s'\n", scene->name().c_str());
    }
    return scene;
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
    drift::platform::Gpu gpu;
    if (!gpu.init(/*needPresent=*/false)) {
        return 1;
    }

    const wgpu::Device& device = gpu.device();
    const wgpu::TextureFormat format = wgpu::TextureFormat::RGBA8Unorm;

    std::unique_ptr<drift::core::Scene> scene;
    drift::core::Renderer placeholder;
    if (!scenePath.empty()) {
        scene = loadScene(scenePath, device, overrides);
        if (!scene) {
            return 1;
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
        const float t = (float)i / 60.0f;
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
            placeholder.render(texture.CreateView(), t);
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

int runWayland(const std::string& scenePath, drift::platform::SurfaceMode mode,
               uint32_t width, uint32_t height,
               const std::vector<ParamOverride>& overrides)
{
    drift::platform::Gpu gpu;
    if (!gpu.init(/*needPresent=*/true)) {
        return 1;
    }

    drift::platform::WaylandApp app;
    if (!app.setup(gpu, mode, width, height)) {
        return 1;
    }

    // Outputs can differ in size, so each gets its own scene instance (with
    // its own graph state and clock); the first one reuses this validation
    // load. nullptr in the map = a load that failed; don't retry per frame.
    std::unique_ptr<drift::core::Scene> firstScene;
    drift::core::Renderer placeholder;
    if (!scenePath.empty()) {
        firstScene = loadScene(scenePath, gpu.device(), overrides);
        if (!firstScene) {
            return 1;
        }
    } else if (!placeholder.init(gpu.device(), app.targetFormat())) {
        fprintf(stderr, "drift: renderer init failed\n");
        return 1;
    }

    const wgpu::Device device = gpu.device();
    const wgpu::TextureFormat format = app.targetFormat();
    const bool animated = !firstScene || firstScene->animated();

    std::map<uint32_t, std::unique_ptr<drift::core::Scene>> scenes;
    app.setOutputRemoved([&scenes](uint32_t outputId) {
        scenes.erase(outputId);
    });

    return app.run(
        [&](const drift::platform::WaylandApp::FrameRequest& req) -> bool {
            if (scenePath.empty()) {
                placeholder.render(req.target, req.seconds);
                return true;
            }
            auto it = scenes.find(req.outputId);
            if (it == scenes.end()) {
                it = scenes
                         .emplace(req.outputId,
                                  firstScene ? std::move(firstScene)
                                             : loadScene(scenePath, device,
                                                         overrides))
                         .first;
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
    int headlessFrames = -1;
    uint32_t width = 0, height = 0;
    std::string outDir = ".";
    std::string scenePath;
    std::set<int> writeFrames;
    MouseArg mouse;
    std::vector<ParamOverride> overrides;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!strcmp(arg, "-w") || !strcmp(arg, "--windowed")) {
            windowed = true;
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
        } else if (!strcmp(arg, "--out") && i + 1 < argc) {
            outDir = argv[++i];
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

    if (!writeFrames.empty() && headlessFrames < 0) {
        headlessFrames = *writeFrames.rbegin() + 1;
    }
    if (headlessFrames >= 0) {
        if (width == 0) { width = 1920; height = 1080; }
        return runHeadless(scenePath, headlessFrames, width, height, outDir,
                           writeFrames, mouse, overrides);
    }
    if (width == 0) { width = 1280; height = 720; }
    return runWayland(scenePath,
                      windowed ? drift::platform::SurfaceMode::Windowed
                               : drift::platform::SurfaceMode::Wallpaper,
                      width, height, overrides);
}
