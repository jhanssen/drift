// Browser runtime: the same drift_core, driven by a canvas surface and a
// requestAnimationFrame loop (DESIGN.md §3: WebGPU calls forward to the
// browser via the emdawnwebgpu bindings; Dawn is not part of this build).
//
// The scene renders into an intermediate texture, which is copied to the
// canvas only on frames the scene actually presented. A WebGPU canvas is
// recomposited (starting from cleared) whenever its current texture is
// acquired, so the native "skip the commit when nothing changed" contract
// (§11) maps to: don't touch the canvas at all — it then keeps showing the
// last presented image.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Last loader failure, readable from JS (drift_errors) so the editor can
// show why an edited document or shader was rejected.
static std::string gLastLoadErrors;

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include "core/Scene.h"
#include "core/WgslInterface.h"
#include "platform/ParamJson.h"
#include "platform/web/VideoDecoderWebCodecs.h"

namespace {

constexpr const char* kCanvas = "#canvas";

struct App {
    wgpu::Instance instance;
    wgpu::Adapter adapter;
    wgpu::Device device;
    wgpu::Surface surface;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
    wgpu::CompositeAlphaMode alphaMode = wgpu::CompositeAlphaMode::Auto;
    wgpu::Texture sceneTarget; // scene renders here; copied to canvas on present
    uint32_t width = 0, height = 0;
    double cssWidth = 0.0, cssHeight = 0.0;

    std::string scenePath;
    std::string sceneJson; // the running document (bundle or pushed edit)
    std::unique_ptr<drift::core::Scene> scene;

    bool started = false;
    bool paused = false;
    double startMs = 0.0;
    double pendingSeekSeconds = -1.0; // applied on the next frame
    double lastSeconds = 0.0;
    float mouseX = 0.5f, mouseY = 0.5f;
    bool mouseActive = false;
    bool warnedNotPresenting = false;
};

App gApp;

// Mirrors the native loader (src/main.cpp) minus video: assets come from the
// bundle's MEMFS under /scenes/<name>, with the same project-root
// confinement. jsonInOut non-empty = load that document (editor pushes);
// empty = read scene.json from the bundle and fill it in.
std::unique_ptr<drift::core::Scene> loadScene(const std::string& root,
                                              const wgpu::Device& device,
                                              std::string* jsonInOut)
{
    namespace fs = std::filesystem;

    auto confined = [root](const std::string& relPath, fs::path& out) -> bool {
        for (const auto& part : fs::path(relPath)) {
            if (part == "..") {
                return false;
            }
        }
        if (fs::path(relPath).is_absolute()) {
            return false;
        }
        out = fs::path(root) / relPath;
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
        fs::path full;
        if (!confined(relPath, full)) {
            error = "path escapes the project root";
            return nullptr;
        }
        return drift::web::createVideoDecoder(full.string(), loop, error);
    };

    std::string sceneJson;
    if (jsonInOut && !jsonInOut->empty()) {
        sceneJson = *jsonInOut;
    } else if (!readAsset("scene.json", sceneJson)) {
        fprintf(stderr, "drift: cannot read %s/scene.json\n", root.c_str());
        return nullptr;
    }

    std::vector<std::string> errors, warnings;
    auto scene = drift::core::Scene::load(sceneJson, readAsset, videoFactory,
                                          device, errors, warnings);
    for (const auto& w : warnings) {
        fprintf(stderr, "drift: scene warning: %s\n", w.c_str());
    }
    gLastLoadErrors.clear();
    for (const auto& e : errors) {
        fprintf(stderr, "drift: scene: %s\n", e.c_str());
        if (!gLastLoadErrors.empty()) {
            gLastLoadErrors += "\n";
        }
        gLastLoadErrors += e;
    }
    if (scene) {
        if (jsonInOut) {
            *jsonInOut = sceneJson;
        }
        printf("drift: loaded scene '%s'\n", scene->name().c_str());
    }
    return scene;
}

// (Re)configures the surface and the intermediate target to the canvas's
// current device-pixel size. Returns false while the canvas has no size.
bool syncSurfaceSize()
{
    double cssW = 0.0, cssH = 0.0;
    emscripten_get_element_css_size(kCanvas, &cssW, &cssH);
    const double dpr = emscripten_get_device_pixel_ratio();
    const uint32_t w = (uint32_t)(cssW * dpr + 0.5);
    const uint32_t h = (uint32_t)(cssH * dpr + 0.5);
    if (w == 0 || h == 0) {
        return false;
    }
    gApp.cssWidth = cssW;
    gApp.cssHeight = cssH;
    if (w == gApp.width && h == gApp.height) {
        return true;
    }
    gApp.width = w;
    gApp.height = h;

    wgpu::SurfaceConfiguration cfg{};
    cfg.device = gApp.device;
    cfg.format = gApp.format;
    cfg.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopyDst;
    cfg.alphaMode = gApp.alphaMode;
    cfg.width = w;
    cfg.height = h;
    gApp.surface.Configure(&cfg);

    wgpu::TextureDescriptor td{};
    td.format = gApp.format;
    td.size = { w, h, 1 };
    td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
    gApp.sceneTarget = gApp.device.CreateTexture(&td);
    return true;
}

bool onFrame(double nowMs, void*)
{
    if (!syncSurfaceSize()) {
        return true; // canvas not laid out yet; try again next frame
    }
    if (!gApp.started) {
        gApp.started = true;
        gApp.startMs = nowMs;
    }
    if (gApp.pendingSeekSeconds >= 0.0) {
        gApp.startMs = nowMs - gApp.pendingSeekSeconds * 1000.0;
        gApp.pendingSeekSeconds = -1.0;
    }
    if (gApp.paused) {
        // Re-anchor each frame so the clock holds still; the frozen time
        // node dirties nothing and the graph quiesces (§11), while input
        // and parameter changes still re-render.
        gApp.startMs = nowMs - gApp.lastSeconds * 1000.0;
    }

    drift::core::FrameContext ctx{};
    ctx.device = gApp.device;
    ctx.seconds = (nowMs - gApp.startMs) / 1000.0;
    gApp.lastSeconds = ctx.seconds;
    ctx.mouseX = gApp.mouseX;
    ctx.mouseY = gApp.mouseY;
    ctx.mouseActive = gApp.mouseActive;
    ctx.target = gApp.sceneTarget.CreateView();
    ctx.targetWidth = gApp.width;
    ctx.targetHeight = gApp.height;
    ctx.targetFormat = gApp.format;
    if (!gApp.scene->render(ctx)) {
        return true; // nothing changed; leave the canvas untouched (§11)
    }

    wgpu::SurfaceTexture st{};
    gApp.surface.GetCurrentTexture(&st);
    if (st.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
        st.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
        if (!gApp.warnedNotPresenting) {
            gApp.warnedNotPresenting = true;
            fprintf(stderr, "drift: GetCurrentTexture failed (%d)\n", (int)st.status);
        }
        return true;
    }

    wgpu::CommandEncoder encoder = gApp.device.CreateCommandEncoder();
    wgpu::TexelCopyTextureInfo src{};
    src.texture = gApp.sceneTarget;
    wgpu::TexelCopyTextureInfo dst{};
    dst.texture = st.texture;
    wgpu::Extent3D extent = { gApp.width, gApp.height, 1 };
    encoder.CopyTextureToTexture(&src, &dst, &extent);
    wgpu::CommandBuffer commands = encoder.Finish();
    gApp.device.GetQueue().Submit(1, &commands);
    return true;
}

bool onMouseMove(int, const EmscriptenMouseEvent* e, void*)
{
    if (gApp.cssWidth > 0.0 && gApp.cssHeight > 0.0) {
        gApp.mouseX = (float)(e->targetX / gApp.cssWidth);
        gApp.mouseY = (float)(e->targetY / gApp.cssHeight);
        gApp.mouseActive = true;
    }
    return true;
}

bool onMouseEnterLeave(int eventType, const EmscriptenMouseEvent*, void*)
{
    // position holds its last value across leave (§9.8)
    gApp.mouseActive = (eventType == EMSCRIPTEN_EVENT_MOUSEENTER);
    return true;
}

void onDeviceReady()
{
    wgpu::SurfaceCapabilities caps{};
    gApp.surface.GetCapabilities(gApp.adapter, &caps);
    if (caps.formatCount == 0) {
        fprintf(stderr, "drift: surface reports no formats\n");
        return;
    }
    gApp.format = caps.formats[0];
    gApp.alphaMode = caps.alphaModeCount ? caps.alphaModes[0]
                                         : wgpu::CompositeAlphaMode::Auto;

    gApp.scene = loadScene(gApp.scenePath, gApp.device, &gApp.sceneJson);
    if (!gApp.scene) {
        return;
    }

    emscripten_set_mousemove_callback(kCanvas, nullptr, false, onMouseMove);
    emscripten_set_mouseenter_callback(kCanvas, nullptr, false, onMouseEnterLeave);
    emscripten_set_mouseleave_callback(kCanvas, nullptr, false, onMouseEnterLeave);
    emscripten_request_animation_frame_loop(onFrame, nullptr);
}

void requestDevice()
{
    // BC compression carries the KTX2 path natively (Gpu.cpp); on the web it
    // is optional — without it, scenes using Basis-compressed textures fail
    // with a validation error when core creates the BC7 texture.
    std::vector<wgpu::FeatureName> features;
    if (gApp.adapter.HasFeature(wgpu::FeatureName::TextureCompressionBC)) {
        features.push_back(wgpu::FeatureName::TextureCompressionBC);
    } else {
        fprintf(stderr, "drift: no BC texture compression; KTX2 scenes will fail\n");
    }

    wgpu::DeviceDescriptor dd{};
    dd.requiredFeatures = features.data();
    dd.requiredFeatureCount = features.size();
    dd.SetUncapturedErrorCallback(
        [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView msg) {
            fprintf(stderr, "drift: [webgpu error %d] %.*s\n",
                    (int)type, (int)msg.length, msg.data);
        });
    dd.SetDeviceLostCallback(
        wgpu::CallbackMode::AllowSpontaneous,
        [](const wgpu::Device&, wgpu::DeviceLostReason reason,
           wgpu::StringView msg) {
            if (reason != wgpu::DeviceLostReason::Destroyed) {
                fprintf(stderr, "drift: [webgpu device lost %d] %.*s\n",
                        (int)reason, (int)msg.length, msg.data);
            }
        });

    gApp.adapter.RequestDevice(
        &dd, wgpu::CallbackMode::AllowSpontaneous,
        [](wgpu::RequestDeviceStatus status, wgpu::Device device,
           wgpu::StringView msg) {
            if (status != wgpu::RequestDeviceStatus::Success) {
                fprintf(stderr, "drift: RequestDevice failed: %.*s\n",
                        (int)msg.length, msg.data);
                return;
            }
            gApp.device = std::move(device);
            onDeviceReady();
        });
}

} // namespace

// JS API for the editor page (same describe schema as the native control
// endpoint — platform/ParamJson.h): call via ccall/cwrap after the scene
// loads (describe returns {"scene":null} until then).
extern "C" {

EMSCRIPTEN_KEEPALIVE const char* drift_describe()
{
    static std::string json;
    const drift::core::Scene* scene = gApp.scene.get();
    json = drift::platform::describeJson(
        scene != nullptr, scene ? scene->name() : std::string(),
        scene && scene->animated(),
        scene ? scene->parameters()
              : std::vector<drift::core::SceneParam>(),
        scene ? drift::platform::sequenceDescs(*scene)
              : std::vector<drift::platform::SequenceDesc>());
    return json.c_str();
}

// Current scene time in seconds — drives the editor's timeline playhead.
EMSCRIPTEN_KEEPALIVE double drift_time()
{
    return gApp.lastSeconds;
}

// Sets the preview's scene clock, so the editor can sync it to a live
// runtime's. Forward is a clock shift (§9.7 allows forward snaps); going
// backward re-creates the scene — time within one instance never
// decreases — so the caller must re-apply parameter overrides. Returns 1
// when it reloaded.
EMSCRIPTEN_KEEPALIVE int drift_seek(double seconds)
{
    if (!gApp.scene || seconds < 0.0) {
        return 0;
    }
    int reloaded = 0;
    if (seconds < gApp.lastSeconds) {
        // Fresh instance on the same document (§9.7).
        gApp.scene = loadScene(gApp.scenePath, gApp.device, &gApp.sceneJson);
        if (!gApp.scene) {
            return 0; // reload failed; console has the loader errors
        }
        reloaded = 1;
    }
    // Applied in the next frame callback, where a trustworthy rAF
    // timestamp exists (a seek can arrive before the first frame).
    gApp.pendingSeekSeconds = seconds;
    gApp.lastSeconds = seconds;
    return reloaded;
}

EMSCRIPTEN_KEEPALIVE void drift_pause(int paused)
{
    gApp.paused = paused != 0;
}

// Re-reads the scene from the bundle (dropping any pushed document),
// keeping the clock (fresh instance starting mid-time — forward-only per
// §9.7). The caller re-applies parameter values. Returns 1 on success.
EMSCRIPTEN_KEEPALIVE int drift_reload()
{
    if (gApp.scenePath.empty()) {
        return 0;
    }
    std::string json; // empty: read from the bundle
    auto fresh = loadScene(gApp.scenePath, gApp.device, &json);
    if (!fresh) {
        return 0;
    }
    gApp.sceneJson = std::move(json);
    gApp.scene = std::move(fresh);
    return 1;
}

// Manually fires an event output ("fire now", §16.1).
EMSCRIPTEN_KEEPALIVE int drift_fire(const char* node, const char* port)
{
    return gApp.scene && gApp.scene->fireEvent(node, port) ? 1 : 0;
}

// Scene ids bundled under /scenes — the editor's scene picker fills from
// this, so the CMake preload list stays the single source of truth.
EMSCRIPTEN_KEEPALIVE const char* drift_scenes()
{
    static std::string json;
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator("/scenes", ec)) {
        if (entry.is_directory()) {
            names.push_back(entry.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());
    json = "[";
    for (size_t i = 0; i < names.size(); ++i) {
        json += (i ? ",\"" : "\"") + names[i] + "\"";
    }
    json += "]";
    return json.c_str();
}

// Graph files under the active project's graphs/ (§19) — the editor's
// node palette and instance pins fill from this.
EMSCRIPTEN_KEEPALIVE const char* drift_graphs()
{
    static std::string json;
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::path(gApp.scenePath) / "graphs", ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            names.push_back("graphs/" + entry.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());
    json = "[";
    for (size_t i = 0; i < names.size(); ++i) {
        json += (i ? ",\"" : "\"") + names[i] + "\"";
    }
    json += "]";
    return json.c_str();
}

// The running scene document, for the editor.
EMSCRIPTEN_KEEPALIVE const char* drift_source()
{
    return gApp.sceneJson.c_str();
}

// Loader errors from the most recent (failed) load, one per line.
EMSCRIPTEN_KEEPALIVE const char* drift_errors()
{
    return gLastLoadErrors.c_str();
}

// Writes a project file (creating directories) under the given root —
// how the editor materializes a from-scratch scene in MEMFS. The page is
// same-origin trusted; the ".." check just keeps paths tidy.
EMSCRIPTEN_KEEPALIVE int drift_write_asset(const char* root, const char* path,
                                           const char* contents)
{
    const std::string rel(path);
    if (rel.empty() || rel.find("..") != std::string::npos || rel[0] == '/') {
        return 0;
    }
    std::error_code ec;
    const std::filesystem::path full = std::filesystem::path(root) / rel;
    std::filesystem::create_directories(full.parent_path(), ec);
    std::ofstream out(full, std::ios::binary | std::ios::trunc);
    if (!out) {
        return 0;
    }
    out << contents;
    return out.good() ? 1 : 0;
}

// Reflects a project-relative WGSL file's port interface (§9.10) so the
// editor's graph view can draw pins for shader ports that are not bound
// yet. Returns {"ports":[{"name","type"},…]} — texture ports first in
// (group, binding) order, the §17.5 convention — or {"error":"…"}.
EMSCRIPTEN_KEEPALIVE const char* drift_reflect(const char* path)
{
    static std::string json;
    auto quote = [](const std::string& s) {
        std::string out = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (c == '\n') {
                out += "\\n";
            } else {
                out += c;
            }
        }
        out += '"';
        return out;
    };
    const std::string rel(path ? path : "");
    if (rel.empty() || rel.find("..") != std::string::npos || rel[0] == '/') {
        json = "{\"error\":\"bad path\"}";
        return json.c_str();
    }
    std::ifstream in(std::filesystem::path(gApp.scenePath) / rel,
                     std::ios::binary);
    if (!in) {
        json = "{\"error\":" + quote(rel + ": cannot read") + "}";
        return json.c_str();
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    drift::core::WgslInterface iface;
    std::string error;
    if (!drift::core::WgslInterface::parse(ss.str(), iface, error)) {
        json = "{\"error\":" + quote(error) + "}";
        return json.c_str();
    }
    auto textures = iface.textures;
    std::sort(textures.begin(), textures.end(),
              [](const drift::core::WgslTexture& a,
                 const drift::core::WgslTexture& b) {
                  return a.group != b.group ? a.group < b.group
                                            : a.binding < b.binding;
              });
    json = "{\"ports\":[";
    bool first = true;
    auto add = [&](const std::string& name, const std::string& type,
                   const char* dir) {
        json += first ? "" : ",";
        first = false;
        json += "{\"name\":" + quote(name) + ",\"type\":\"" + type +
                "\",\"dir\":\"" + dir + "\"}";
    };
    for (const auto& t : textures) {
        add(t.name, "texture", "in");
    }
    for (const auto& f : iface.fields) {
        add(f.name, drift::core::valueTypeName(f.type), "in");
    }
    // §18.2 compute interfaces: storage buffers and storage textures.
    for (const auto& b : iface.storageBuffers) {
        add(b.name, "buffer", b.readWrite ? "out" : "in");
    }
    for (const auto& t : iface.storageTextures) {
        add(t.name, "texture", "out");
    }
    json += "]}";
    return json.c_str();
}

// Reads a project-relative text file (e.g. a shader into the editor pane).
EMSCRIPTEN_KEEPALIVE const char* drift_read_asset(const char* path)
{
    static std::string contents;
    contents.clear();
    const std::string rel(path);
    if (rel.find("..") != std::string::npos ||
        (!rel.empty() && rel[0] == '/')) {
        return contents.c_str();
    }
    std::ifstream in(std::filesystem::path(gApp.scenePath) / rel,
                     std::ios::binary);
    if (in) {
        std::ostringstream ss;
        ss << in.rdbuf();
        contents = ss.str();
    }
    return contents.c_str();
}

// Switches the preview to another project root (e.g. the editor's scratch
// scene). The clock keeps running; the caller re-applies parameters.
EMSCRIPTEN_KEEPALIVE int drift_open(const char* root)
{
    std::string json;
    std::string previousPath = gApp.scenePath;
    gApp.scenePath = root;
    auto fresh = loadScene(gApp.scenePath, gApp.device, &json);
    if (!fresh) {
        gApp.scenePath = std::move(previousPath);
        return 0;
    }
    gApp.sceneJson = std::move(json);
    gApp.scene = std::move(fresh);
    return 1;
}

// Swaps to an edited document (assets still resolve from the bundled
// project). The old scene keeps running if validation fails; on success
// the clock is kept and the caller re-applies parameter values.
EMSCRIPTEN_KEEPALIVE int drift_load(const char* sceneJson)
{
    if (gApp.scenePath.empty() || !sceneJson || !*sceneJson) {
        return 0;
    }
    std::string json(sceneJson);
    auto fresh = loadScene(gApp.scenePath, gApp.device, &json);
    if (!fresh) {
        return 0;
    }
    gApp.sceneJson = std::move(json);
    gApp.scene = std::move(fresh);
    return 1;
}

// count selects scalar (1) through vec4 (4). Returns 0 for an unknown
// name/type mismatch, like Scene::setParameter.
EMSCRIPTEN_KEEPALIVE int drift_set_parameter(const char* name, double x,
                                             double y, double z, double w,
                                             int count)
{
    if (!gApp.scene || count < 1 || count > 4) {
        return 0;
    }
    drift::core::Value value;
    value.v = { x, y, z, w };
    value.type = count == 1 ? drift::core::ValueType::Scalar
               : count == 2 ? drift::core::ValueType::Vec2
               : count == 3 ? drift::core::ValueType::Vec3
                            : drift::core::ValueType::Vec4;
    return gApp.scene->setParameter(name, value) ? 1 : 0;
}

} // extern "C"

int main(int argc, char** argv)
{
    std::string name = argc > 1 ? argv[1] : "plasma";
    if (name.find('/') != std::string::npos ||
        !std::filesystem::exists("/scenes/" + name)) {
        fprintf(stderr, "drift: no bundled scene '%s'; have:\n", name.c_str());
        for (const auto& entry : std::filesystem::directory_iterator("/scenes")) {
            fprintf(stderr, "  %s\n", entry.path().filename().c_str());
        }
        return 1;
    }
    gApp.scenePath = "/scenes/" + name;

    gApp.instance = wgpu::CreateInstance(nullptr);
    if (!gApp.instance) {
        fprintf(stderr, "drift: WebGPU is not available in this browser\n");
        return 1;
    }

    wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector canvas{};
    canvas.selector = kCanvas;
    wgpu::SurfaceDescriptor sd{};
    sd.nextInChain = &canvas;
    gApp.surface = gApp.instance.CreateSurface(&sd);

    wgpu::RequestAdapterOptions opts{};
    opts.featureLevel = wgpu::FeatureLevel::Core;
    opts.compatibleSurface = gApp.surface;
    gApp.instance.RequestAdapter(
        &opts, wgpu::CallbackMode::AllowSpontaneous,
        [](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter,
           wgpu::StringView msg) {
            if (status != wgpu::RequestAdapterStatus::Success) {
                fprintf(stderr, "drift: RequestAdapter failed: %.*s\n",
                        (int)msg.length, msg.data);
                return;
            }
            gApp.adapter = std::move(adapter);
            requestDevice();
        });

    // The runtime stays alive after main returns (EXIT_RUNTIME=0); rendering
    // continues from the adapter/device callbacks and the rAF loop.
    return 0;
}
