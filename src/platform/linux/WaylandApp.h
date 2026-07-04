#pragma once

// Wayland presentation: connects to the compositor and runs a frame loop
// presenting dmabuf-backed render targets via linux-dmabuf.
//
// Wallpaper mode creates one background layer surface per wl_output (with
// runtime hotplug: outputs appearing/disappearing create/destroy surfaces);
// windowed dev mode creates a single xdg toplevel. Each surface has its own
// buffer ring, frame-callback state, pointer state, and scene clock.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "Gpu.h"

struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_surface;
struct wl_buffer;
struct wl_callback;
struct wl_seat;
struct wl_pointer;
struct wl_output;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;
struct zwp_linux_dmabuf_v1;
struct zwp_linux_dmabuf_feedback_v1;

namespace drift::platform {

enum class SurfaceMode {
    Wallpaper, // wlr-layer-shell background layer per output
    Windowed,  // single xdg toplevel dev window
};

class WaylandApp {
public:
    ~WaylandApp();

    // Connects, binds globals, creates the initial surfaces and negotiates
    // the buffer format (compositor-supported ∩ Dawn-importable modifiers).
    // width/height are the initial size in Windowed mode and ignored in
    // Wallpaper mode (the compositor sizes layer surfaces).
    bool setup(Gpu& gpu, SurfaceMode mode, uint32_t width, uint32_t height);

    wgpu::TextureFormat targetFormat() const { return mFormat; }

    // One render callback invocation per surface per frame. outputId is a
    // stable identity for the lifetime of that output's surface (the
    // wl_output registry name; 0 in windowed mode) — the app keeps one
    // scene instance per outputId. seconds is that surface's scene clock:
    // it advances by capped per-frame deltas, so it freezes across pauses
    // (occlusion, lock) per SCENE_FORMAT.md §9.7. Returns whether the
    // target was written; when false nothing is committed (§11).
    struct FrameRequest {
        uint32_t outputId = 0;
        wgpu::TextureView target;
        uint32_t width = 0, height = 0;
        float seconds = 0.0f;
        float mouseX = 0.5f, mouseY = 0.5f;
        bool mouseActive = false;
    };
    using RenderFrame = std::function<bool(const FrameRequest&)>;

    // Notifies when an output's surface is destroyed (hotplug removal) so
    // the app can drop that output's scene.
    using OutputRemoved = std::function<void(uint32_t)>;
    void setOutputRemoved(OutputRemoved cb) { mOutputRemoved = std::move(cb); }

    // Frame loop until the (windowed) surface is closed. animated: the
    // scene advances with time, so frames are produced continuously
    // (frame-callback throttled, timer-ticked while nothing commits);
    // non-animated scenes render only on input/configure events.
    int run(RenderFrame renderFrame, bool animated);

    // -- internal, public for C listener trampolines --
    struct Buffer {
        DmabufTarget target;
        wl_buffer* buffer = nullptr;
        bool busy = false;
    };
    struct OutputSurface {
        WaylandApp* app = nullptr;
        uint32_t id = 0; // wl_output registry name; 0 = windowed
        wl_output* output = nullptr;
        wl_surface* surface = nullptr;
        zwlr_layer_surface_v1* layerSurface = nullptr;
        xdg_surface* xdgSurface = nullptr;
        xdg_toplevel* toplevel = nullptr;
        std::vector<Buffer> buffers;
        uint32_t width = 0, height = 0;
        uint32_t pendingWidth = 0, pendingHeight = 0;
        bool configured = false;
        bool framePending = false; // a commit awaits its frame callback
        bool wantRedraw = false;   // input/configure while idle
        bool needRedraw = false;   // all buffers busy; retry on release
        bool pointerOver = false;
        bool pointerSeen = false;
        double pointerX = 0.0, pointerY = 0.0; // surface-local
        double sceneTime = 0.0;
        double lastDrawTime = -1.0;
    };

    void onGlobal(uint32_t name, const char* interface, uint32_t version);
    void onGlobalRemove(uint32_t name);
    void onDmabufModifier(uint32_t fourcc, uint64_t modifier);
    void onFeedbackFormatTable(int32_t fd, uint32_t size);
    void onFeedbackTrancheFormats(const uint16_t* indices, size_t count);
    void onFeedbackDone();
    void onSeatCapabilities(uint32_t capabilities);
    void onPointerEnter(wl_surface* surface, double x, double y);
    void onPointerLeave(wl_surface* surface);
    void onPointerMotion(double x, double y);
    void onXdgSurfaceConfigure(OutputSurface* surf, uint32_t serial);
    void onToplevelConfigure(OutputSurface* surf, int32_t width, int32_t height);
    void onLayerConfigure(OutputSurface* surf, uint32_t serial, uint32_t width,
                          uint32_t height);
    void onSurfaceClosed(OutputSurface* surf);
    void onFrameDone(OutputSurface* surf);
    void onBufferRelease(wl_buffer* buffer);

private:
    bool createOutputSurface(wl_output* output, uint32_t id);
    void destroyOutputSurface(OutputSurface* surf);
    bool chooseFormat();
    bool createRing(OutputSurface& surf);
    void destroyRing(OutputSurface& surf);
    bool createWlBuffer(OutputSurface& surf, Buffer& buf);
    void drawFrame(OutputSurface& surf);
    OutputSurface* surfaceFor(wl_surface* surface);
    double now() const;

    Gpu* mGpu = nullptr;
    SurfaceMode mMode = SurfaceMode::Wallpaper;
    uint32_t mInitialWidth = 0, mInitialHeight = 0; // windowed

    wl_display* mDisplay = nullptr;
    wl_registry* mRegistry = nullptr;
    wl_compositor* mCompositor = nullptr;
    xdg_wm_base* mWmBase = nullptr;
    zwlr_layer_shell_v1* mLayerShell = nullptr;
    zwp_linux_dmabuf_v1* mDmabuf = nullptr;

    // dmabuf v4 default feedback: mmap'd format+modifier table. On v3 the
    // flat modifier event list fills mCompositorModifiers directly.
    zwp_linux_dmabuf_feedback_v1* mFeedback = nullptr;
    const void* mFormatTable = nullptr;
    uint32_t mFormatTableSize = 0;
    bool mFeedbackAccumulating = false;

    wl_seat* mSeat = nullptr;
    wl_pointer* mPointer = nullptr;
    wl_surface* mPointerSurface = nullptr; // surface under the pointer

    std::vector<std::unique_ptr<OutputSurface>> mSurfaces;
    std::vector<std::pair<wl_output*, uint32_t>> mPendingOutputs;
    bool mSetupDone = false;

    // (fourcc, modifier) pairs the compositor accepts, from zwp_linux_dmabuf v3.
    std::map<uint32_t, std::vector<uint64_t>> mCompositorModifiers;
    std::vector<uint64_t> mRingModifiers; // chosen format's usable modifiers
    uint32_t mFourcc = 0;
    wgpu::TextureFormat mFormat = wgpu::TextureFormat::Undefined;

    bool mRunning = true;
    bool mAnimated = false;
    RenderFrame mRenderFrame;
    OutputRemoved mOutputRemoved;
    double mStartTime = 0.0;
};

} // namespace drift::platform
