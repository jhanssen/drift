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
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Last loader failure, readable from JS (drift_errors) so the editor can
// show why an edited document or shader was rejected.
static std::string gLastLoadErrors;

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include "core/NodeProps.h"
#include "core/Scene.h"
#include "core/WgslInterface.h"
#include "platform/PackageStore.h"
#include "platform/ParamJson.h"
#include "platform/web/VideoDecoderWebCodecs.h"

// ---- WASM logic modules (DESIGN.md §4.5) ----
// Each module node is an ordinary WebAssembly.Instance beside this
// runtime — one compiled WebAssembly.Module per file (cached by content
// hash), one instance per node. The shared C++ evaluator drives it; this
// bridge only moves bytes between the two linear memories and calls the
// exports. Compilation is asynchronous (Chrome caps synchronous
// WebAssembly.Module on the main thread at 4 KB), so a scene referencing
// a not-yet-compiled module fails its load with "still compiling" and the
// driftModuleReady callback re-runs the last load request.

// Returns 0 = compiled and cached, 1 = compiling (retry on ready),
// 2 = failed (message via js_module_error).
EM_JS(int, js_module_prepare, (const char* key, const uint8_t* bytes, int len), {
    const k = UTF8ToString(key);
    const reg = Module.driftModules || (Module.driftModules = new Map());
    const ent = reg.get(k);
    if (ent) {
        return ent.error ? 2 : (ent.module ? 0 : 1);
    }
    const buf = HEAPU8.slice(bytes, bytes + len);
    if (len <= 4096) {
        // Small enough for the synchronous path everywhere: no retry trip.
        try {
            reg.set(k, { module: new WebAssembly.Module(buf) });
            return 0;
        } catch (e) {
            reg.set(k, { error: String(e) });
            return 2;
        }
    }
    reg.set(k, {});
    WebAssembly.compile(buf).then(
        (m) => {
            reg.get(k).module = m;
            if (Module.driftModuleReady) Module.driftModuleReady();
        },
        (e) => {
            reg.get(k).error = String(e);
            if (Module.driftModuleReady) Module.driftModuleReady();
        });
    return 1;
});

EM_JS(void, js_module_error, (const char* key, char* out, int cap), {
    const ent = (Module.driftModules || new Map()).get(UTF8ToString(key));
    stringToUTF8(ent && ent.error ? ent.error : 'unknown module error',
                 out, cap);
});

// Instantiates the cached module and runs the §4.5 init handshake
// (drift_abi / drift_init(ioSize) / memory bounds). Returns a handle > 0,
// or 0 with the message in errOut. storagePtr is the node's C++
// ModuleStorage (0 = none): the §4.4 storage imports bridge bytes
// between the module's memory and the runtime heap, then call the
// drift_storage_c_* shims.
EM_JS(int, js_module_instantiate, (const char* key, int ioSize,
                                   int storagePtr, char* errOut,
                                   int errCap), {
    const ent = (Module.driftModules || new Map()).get(UTF8ToString(key));
    const fail = (msg) => { stringToUTF8(String(msg), errOut, errCap); return 0; };
    if (!ent || !ent.module) {
        return fail(ent && ent.error ? ent.error : 'module not compiled');
    }
    let inst;
    const guest = () => new Uint8Array(inst.exports.memory.buffer);
    const toHeap = (view) => {
        const p = _malloc(view.length || 1);
        HEAPU8.set(view, p);
        return p;
    };
    try {
        inst = new WebAssembly.Instance(ent.module, { env: {
            // Debug logging (§4.5).
            drift_log: (ptr, len) => {
                const mem = inst.exports.memory;
                console.log('drift module:', new TextDecoder().decode(
                    new Uint8Array(mem.buffer, ptr >>> 0, len >>> 0)));
            },
            // §4.4 storage (core/Module.h ABI; -4 = invalid span).
            drift_storage_get: (kptr, klen, dptr, dcap) => {
                kptr >>>= 0; klen >>>= 0; dptr >>>= 0; dcap >>>= 0;
                const m = guest();
                if (kptr + klen > m.length || dptr + dcap > m.length) {
                    return -4;
                }
                const kh = toHeap(m.subarray(kptr, kptr + klen));
                const dh = dcap ? _malloc(dcap) : 0;
                const r = _drift_storage_c_get(storagePtr, kh, klen, dh,
                                               dcap);
                if (r > 0 && dcap) {
                    const n = Math.min(r, dcap);
                    guest().set(HEAPU8.subarray(dh, dh + n), dptr);
                }
                if (dh) _free(dh);
                _free(kh);
                return r;
            },
            drift_storage_put: (kptr, klen, vptr, vlen) => {
                kptr >>>= 0; klen >>>= 0; vptr >>>= 0; vlen >>>= 0;
                const m = guest();
                if (kptr + klen > m.length || vptr + vlen > m.length) {
                    return -4;
                }
                const kh = toHeap(m.subarray(kptr, kptr + klen));
                const vh = toHeap(m.subarray(vptr, vptr + vlen));
                const r = _drift_storage_c_put(storagePtr, kh, klen, vh,
                                               vlen);
                _free(vh);
                _free(kh);
                return r;
            },
            drift_storage_delete: (kptr, klen) => {
                kptr >>>= 0; klen >>>= 0;
                const m = guest();
                if (kptr + klen > m.length) {
                    return -4;
                }
                const kh = toHeap(m.subarray(kptr, kptr + klen));
                const r = _drift_storage_c_delete(storagePtr, kh, klen);
                _free(kh);
                return r;
            },
            drift_storage_keys: (dptr, dcap) => {
                dptr >>>= 0; dcap >>>= 0;
                const m = guest();
                if (dptr + dcap > m.length) {
                    return -4;
                }
                const dh = dcap ? _malloc(dcap) : 0;
                const r = _drift_storage_c_keys(storagePtr, dh, dcap);
                if (r > 0 && dcap) {
                    const n = Math.min(r, dcap);
                    guest().set(HEAPU8.subarray(dh, dh + n), dptr);
                }
                if (dh) _free(dh);
                return r;
            },
        } });
    } catch (e) {
        return fail(e);
    }
    const ex = inst.exports;
    if (typeof ex.drift_abi !== 'function' ||
        typeof ex.drift_init !== 'function' ||
        typeof ex.drift_update !== 'function' ||
        !(ex.memory instanceof WebAssembly.Memory)) {
        return fail('missing drift_abi/drift_init/drift_update/memory exports');
    }
    try {
        const abi = ex.drift_abi() | 0;
        if (abi !== 1) {
            return fail('unsupported module ABI ' + abi);
        }
        const io = ex.drift_init(ioSize) >>> 0;
        if (!io || io + ioSize > ex.memory.buffer.byteLength) {
            return fail('drift_init returned a bad io block');
        }
        const insts = Module.driftInstances ||
                      (Module.driftInstances = { next: 1, map: new Map() });
        const h = insts.next++;
        insts.map.set(h, { inst: inst, io: io });
        return h;
    } catch (e) {
        return fail('module init trapped: ' + e);
    }
});

// Byte moves take fresh views every call: either memory may have grown
// (growth detaches the old ArrayBuffer).
EM_JS(int, js_module_write, (int h, int off, const uint8_t* src, int len), {
    const e = Module.driftInstances && Module.driftInstances.map.get(h);
    if (!e) return 0;
    const buf = e.inst.exports.memory.buffer;
    if (e.io + off + len > buf.byteLength) return 0;
    new Uint8Array(buf).set(HEAPU8.subarray(src, src + len), e.io + off);
    return 1;
});

EM_JS(int, js_module_read, (int h, int off, uint8_t* dst, int len), {
    const e = Module.driftInstances && Module.driftInstances.map.get(h);
    if (!e) return 0;
    const buf = e.inst.exports.memory.buffer;
    if (e.io + off + len > buf.byteLength) return 0;
    HEAPU8.set(new Uint8Array(buf, e.io + off, len), dst);
    return 1;
});

EM_JS(int, js_module_update, (int h, char* errOut, int errCap), {
    const e = Module.driftInstances && Module.driftInstances.map.get(h);
    if (!e) return 0;
    try {
        e.inst.exports.drift_update();
        return 1;
    } catch (err) {
        stringToUTF8(String(err), errOut, errCap);
        return 0;
    }
});

EM_JS(void, js_module_release, (int h), {
    if (Module.driftInstances) Module.driftInstances.map.delete(h);
});

EM_JS(void, js_module_set_ready, (), {
    Module.driftModuleReady = () => _drift_modules_ready();
});

// ---- §4.4 module storage: IndexedDB-backed blob cache ----
// The storage ABI is synchronous (core/Module.h), so the persistent
// blobs are preloaded into a JS Map before the first scene load (scene
// start gates on it) and every save writes the cache through to
// IndexedDB fire-and-forget. Keys are "<project root>|<namespace>".

EM_JS(void, js_storage_init, (), {
    Module.driftStorageCache = new Map();
    const done = () => {
        Module.driftStorageReady = true;
        _drift_storage_ready();
    };
    let req;
    try {
        req = indexedDB.open('drift-module-storage', 1);
    } catch (e) {
        done(); // no IndexedDB (privacy mode): in-memory session storage
        return;
    }
    req.onupgradeneeded = () => req.result.createObjectStore('blobs');
    req.onerror = done;
    req.onsuccess = () => {
        const db = req.result;
        Module.driftStorageDb = db;
        const tx = db.transaction('blobs', 'readonly');
        const store = tx.objectStore('blobs');
        const keys = store.getAllKeys();
        const vals = store.getAll();
        tx.oncomplete = () => {
            for (let i = 0; i < keys.result.length; ++i) {
                Module.driftStorageCache.set(keys.result[i],
                                             new Uint8Array(vals.result[i]));
            }
            done();
        };
        tx.onerror = done;
    };
});

EM_JS(int, js_storage_blob_size, (const char* key), {
    const blob = Module.driftStorageCache.get(UTF8ToString(key));
    return blob ? blob.length : -1;
});

EM_JS(void, js_storage_blob_copy, (const char* key, uint8_t* dst), {
    const blob = Module.driftStorageCache.get(UTF8ToString(key));
    if (blob) HEAPU8.set(blob, dst);
});

EM_JS(void, js_storage_blob_save, (const char* key, const uint8_t* data,
                                   int len), {
    const k = UTF8ToString(key);
    const blob = HEAPU8.slice(data, data + len);
    Module.driftStorageCache.set(k, blob);
    const db = Module.driftStorageDb;
    if (!db) return;
    try {
        db.transaction('blobs', 'readwrite').objectStore('blobs')
            .put(blob, k);
    } catch (e) { /* session-only from here */ }
});

// §4.4 storage shims: the JS import trampolines call these with
// heap-copied spans; storage is the node's ModuleStorage (0 = denied).
extern "C" {

EMSCRIPTEN_KEEPALIVE int drift_storage_c_get(void* storage,
                                             const uint8_t* k, int klen,
                                             uint8_t* d, int dcap)
{
    if (!storage) {
        return drift::core::kModuleStorageDenied;
    }
    return ((drift::core::ModuleStorage*)storage)
        ->get(k, (uint32_t)klen, d, (uint32_t)dcap);
}

EMSCRIPTEN_KEEPALIVE int drift_storage_c_put(void* storage,
                                             const uint8_t* k, int klen,
                                             const uint8_t* v, int vlen)
{
    if (!storage) {
        return drift::core::kModuleStorageDenied;
    }
    return ((drift::core::ModuleStorage*)storage)
        ->put(k, (uint32_t)klen, v, (uint32_t)vlen);
}

EMSCRIPTEN_KEEPALIVE int drift_storage_c_delete(void* storage,
                                                const uint8_t* k, int klen)
{
    if (!storage) {
        return drift::core::kModuleStorageDenied;
    }
    return ((drift::core::ModuleStorage*)storage)->erase(k, (uint32_t)klen);
}

EMSCRIPTEN_KEEPALIVE int drift_storage_c_keys(void* storage, uint8_t* d,
                                              int dcap)
{
    if (!storage) {
        return drift::core::kModuleStorageDenied;
    }
    return ((drift::core::ModuleStorage*)storage)
        ->keys(d, (uint32_t)dcap);
}

} // extern "C"

namespace {

constexpr const char* kCanvas = "#canvas";

// §4.4 storage persistence: the preloaded IndexedDB cache, keyed
// "<project root>|<namespace>".
class IdbStoragePersistence : public drift::core::ModuleStoragePersistence {
public:
    explicit IdbStoragePersistence(std::string root)
        : mRoot(std::move(root))
    {
    }
    bool load(const std::string& ns, std::string& blob) override
    {
        const std::string key = mRoot + "|" + ns;
        const int size = js_storage_blob_size(key.c_str());
        if (size < 0) {
            return false;
        }
        blob.resize((size_t)size);
        if (size) {
            js_storage_blob_copy(key.c_str(), (uint8_t*)blob.data());
        }
        return true;
    }
    void save(const std::string& ns, const std::string& blob) override
    {
        js_storage_blob_save((mRoot + "|" + ns).c_str(),
                             (const uint8_t*)blob.data(), (int)blob.size());
    }

private:
    std::string mRoot;
};

class WebModuleInstance : public drift::core::ModuleInstance {
public:
    explicit WebModuleInstance(int handle)
        : mHandle(handle)
    {
    }
    ~WebModuleInstance() override { js_module_release(mHandle); }

    bool writeIo(uint32_t offset, const void* src, uint32_t len) override
    {
        return js_module_write(mHandle, (int)offset, (const uint8_t*)src,
                               (int)len) != 0;
    }
    bool readIo(uint32_t offset, void* dst, uint32_t len) override
    {
        return js_module_read(mHandle, (int)offset, (uint8_t*)dst,
                              (int)len) != 0;
    }
    bool update(std::string& error) override
    {
        char err[512] = {};
        if (js_module_update(mHandle, err, sizeof(err))) {
            return true;
        }
        error = err;
        return false;
    }

private:
    int mHandle;
};

// Set when a scene load failed only because a module was still compiling;
// drift_modules_ready() then re-runs that load.
bool gModulesPending = false;
std::string gRetryJson; // the failed attempt's document; "" = read bundle

std::unique_ptr<drift::core::ModuleInstance>
makeModuleInstance(const std::string& wasmBytes, uint32_t ioSize,
                   drift::core::ModuleStorage* storage,
                   drift::core::ModuleNet* net, std::string& error)
{
    (void)net; // §4.4 network trampolines land with the browser backend

    // Content-hash key (FNV-1a 64): editor rewrites of a module file get a
    // fresh compile, unchanged files share the cached one.
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : wasmBytes) {
        hash = (hash ^ c) * 1099511628211ull;
    }
    char key[32];
    snprintf(key, sizeof(key), "m%016llx", (unsigned long long)hash);

    const int state = js_module_prepare(key, (const uint8_t*)wasmBytes.data(),
                                        (int)wasmBytes.size());
    if (state == 1) {
        gModulesPending = true;
        error = "module is still compiling; the preview retries when ready";
        return nullptr;
    }
    if (state == 2) {
        char err[512] = {};
        js_module_error(key, err, sizeof(err));
        error = err;
        return nullptr;
    }
    char err[512] = {};
    const int handle = js_module_instantiate(key, (int)ioSize,
                                             (int)(uintptr_t)storage, err,
                                             sizeof(err));
    if (!handle) {
        error = err;
        return nullptr;
    }
    return std::make_unique<WebModuleInstance>(handle);
}

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

    // Project-root confinement plus §20.2 package-store resolution (in
    // the browser only a project-local packages/ dir can resolve — there
    // is no shared store on MEMFS unless the host mounts one).
    auto confined = [root](const std::string& relPath, fs::path& out) -> bool {
        return drift::platform::resolveProjectPath(fs::path(root), relPath,
                                                   out);
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

    auto videoFactory = [confined, device](const std::string& relPath,
                                           bool loop, std::string& error)
        -> std::unique_ptr<drift::core::VideoDecoder> {
        fs::path full;
        if (!confined(relPath, full)) {
            error = "path escapes the project root";
            return nullptr;
        }
        return drift::web::createVideoDecoder(full.string(), loop, device,
                                              error);
    };

    std::string sceneJson;
    if (jsonInOut && !jsonInOut->empty()) {
        sceneJson = *jsonInOut;
    } else if (!readAsset("scene.json", sceneJson)) {
        fprintf(stderr, "drift: cannot read %s/scene.json\n", root.c_str());
        return nullptr;
    }

    gModulesPending = false;
    std::vector<std::string> errors, warnings;
    // Package grants ride the store's .installed.json through readAsset;
    // no project-grant surface in the browser yet (the editor prompt is a
    // later slice) — ungranted capabilities warn and read as offline.
    // Storage persists through the preloaded IndexedDB cache.
    drift::core::ModulePlatform modules{
        makeModuleInstance, nullptr,
        std::make_shared<IdbStoragePersistence>(root)
    };
    auto scene = drift::core::Scene::load(sceneJson, readAsset, videoFactory,
                                          modules, device, errors, warnings);
    if (!scene && gModulesPending) {
        // Retried verbatim by drift_modules_ready() once compiles land.
        gRetryJson = (jsonInOut && !jsonInOut->empty()) ? *jsonInOut
                                                        : std::string();
    }
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
    if (!gApp.scene) {
        return true; // no scene yet (e.g. modules still compiling)
    }
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

// Scene start gates on the GPU device AND the storage cache preload
// (§4.4: the synchronous storage ABI needs the blobs in memory before
// the first module instantiates); whichever finishes last starts.
bool gDeviceReady = false;
bool gStorageReady = false;

void tryStart()
{
    static bool started = false;
    if (started || !gDeviceReady || !gStorageReady) {
        return;
    }
    started = true;
    gApp.scene = loadScene(gApp.scenePath, gApp.device, &gApp.sceneJson);
    // Callbacks and the frame loop start even without a scene: the initial
    // load may be waiting on module compiles (drift_modules_ready fills
    // gApp.scene in), and onFrame no-ops until it exists.
    emscripten_set_mousemove_callback(kCanvas, nullptr, false, onMouseMove);
    emscripten_set_mouseenter_callback(kCanvas, nullptr, false, onMouseEnterLeave);
    emscripten_set_mouseleave_callback(kCanvas, nullptr, false, onMouseEnterLeave);
    emscripten_request_animation_frame_loop(onFrame, nullptr);
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
    gDeviceReady = true;
    tryStart();
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

// Fired by the JS bridge when the IndexedDB storage cache preload
// completes (or is unavailable — then storage is session-only).
EMSCRIPTEN_KEEPALIVE void drift_storage_ready()
{
    gStorageReady = true;
    tryStart();
}

// Fired by the JS bridge when an asynchronous module compile lands:
// re-runs the load attempt that failed on "still compiling". Keeps the
// clock (a scene appearing late starts at the current time, like
// drift_open).
EMSCRIPTEN_KEEPALIVE void drift_modules_ready()
{
    if (!gModulesPending) {
        return;
    }
    gModulesPending = false;
    std::string json = gRetryJson;
    auto fresh = loadScene(gApp.scenePath, gApp.device, &json);
    if (!fresh) {
        return; // still pending on another module, or a real load error
    }
    gApp.sceneJson = std::move(json);
    gApp.scene = std::move(fresh);
}

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
    namespace fs = std::filesystem;
    static std::string json;
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(
             fs::path(gApp.scenePath) / "graphs", ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            names.push_back("graphs/" + entry.path().filename().string());
        }
    }

    // §20.6: package graphs — project-local copies shadow the stores,
    // and within the stores the newest installed version of each name.
    std::set<std::string> seenPkgs;
    const auto packageGraphs = [&names](const fs::path& root,
                                        const std::string& pkg) {
        std::error_code ec2;
        for (const auto& g : fs::directory_iterator(root / "graphs", ec2)) {
            if (g.is_regular_file() && g.path().extension() == ".json") {
                names.push_back("packages/" + pkg + "/graphs/" +
                                g.path().filename().string());
            }
        }
    };
    for (const auto& entry : fs::directory_iterator(
             fs::path(gApp.scenePath) / "packages", ec)) {
        if (entry.is_directory(ec) &&
            seenPkgs.insert(entry.path().filename().string()).second) {
            packageGraphs(entry.path(), entry.path().filename().string());
        }
    }
    for (const fs::path& store : drift::platform::packageStores()) {
        for (const auto& entry : fs::directory_iterator(store, ec)) {
            if (!entry.is_directory(ec)) {
                continue;
            }
            const std::string pkg = entry.path().filename().string();
            if (seenPkgs.count(pkg)) {
                continue;
            }
            std::vector<long> best;
            fs::path bestPath;
            std::error_code ec2;
            for (const auto& v : fs::directory_iterator(entry.path(), ec2)) {
                std::vector<long> segs;
                if (!v.is_directory(ec2) ||
                    !drift::platform::versionSegments(
                        v.path().filename().string(), segs)) {
                    continue;
                }
                if (best.empty() ||
                    drift::platform::versionCompare(segs, best) > 0) {
                    best = std::move(segs);
                    bestPath = v.path();
                }
            }
            if (!bestPath.empty()) {
                seenPkgs.insert(pkg);
                packageGraphs(bestPath, pkg);
            }
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
    std::filesystem::path full;
    if (rel.empty() ||
        !drift::platform::resolveProjectPath(gApp.scenePath, rel, full)) {
        json = "{\"error\":\"bad path\"}";
        return json.c_str();
    }
    std::ifstream in(full, std::ios::binary);
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

// The declared property surface per node type (core/NodeProps.h) — the
// editor builds creation forms and the inspector's defaults from this
// instead of hardcoding node types.
EMSCRIPTEN_KEEPALIVE const char* drift_node_props()
{
    static std::string json;
    if (!json.empty()) {
        return json.c_str();
    }
    json = "{";
    const char* current = nullptr;
    for (const auto& p : drift::core::kNodeProps) {
        if (!current || strcmp(current, p.node) != 0) {
            if (current) {
                json += "],";
            }
            json += std::string("\"") + p.node + "\":[";
            current = p.node;
        } else {
            json += ",";
        }
        json += std::string("{\"name\":\"") + p.name + "\",\"kind\":\"" +
                p.kind + "\",\"required\":" +
                (p.required ? "true" : "false");
        if (p.def) {
            json += std::string(",\"default\":") + p.def;
        }
        if (p.options) {
            json += ",\"options\":[";
            std::string opts(p.options);
            size_t start = 0;
            bool first = true;
            while (start <= opts.size()) {
                const size_t space = opts.find(' ', start);
                const std::string opt = opts.substr(
                    start,
                    space == std::string::npos ? space : space - start);
                if (!opt.empty()) {
                    json += std::string(first ? "\"" : ",\"") + opt + "\"";
                    first = false;
                }
                if (space == std::string::npos) {
                    break;
                }
                start = space + 1;
            }
            json += "]";
        }
        json += "}";
    }
    if (current) {
        json += "]";
    }
    json += "}";
    return json.c_str();
}

// The per-type port surface (core/NodeProps.h → SceneLoader's own port
// tables) — the editor draws pins, wire categories, and unbound-port
// defaults from this instead of hardcoding node types.
EMSCRIPTEN_KEEPALIVE const char* drift_node_ports()
{
    static std::string json;
    if (json.empty()) {
        json = drift::core::nodePortsJson();
    }
    return json.c_str();
}

// Reads a project-relative text file (e.g. a shader into the editor
// pane). packages/ paths resolve through the store (§20.6) so the editor
// can drill into package graphs.
EMSCRIPTEN_KEEPALIVE const char* drift_read_asset(const char* path)
{
    static std::string contents;
    contents.clear();
    std::filesystem::path full;
    if (!drift::platform::resolveProjectPath(gApp.scenePath, path, full)) {
        return contents.c_str();
    }
    std::ifstream in(full, std::ios::binary);
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
    // The bundle ships first-party packages as a store at /packages
    // (§20.2); a host page can still override the path.
    setenv("DRIFT_PACKAGE_PATH", "/packages", /*overwrite=*/0);
    js_module_set_ready(); // async module compiles retry the scene load
    js_storage_init();     // §4.4 storage cache; scene start gates on it

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
