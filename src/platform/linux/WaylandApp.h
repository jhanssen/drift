#pragma once

// Wayland presentation: connects to the compositor and runs a frame loop
// presenting dmabuf-backed render targets via linux-dmabuf.
//
// Wallpaper mode creates one background layer surface per wl_output (with
// runtime hotplug: outputs appearing/disappearing create/destroy surfaces),
// optionally restricted to named outputs (--output; one process per output
// is how different outputs run different scenes); windowed dev mode creates
// a single xdg toplevel. Each surface has its own buffer ring,
// frame-callback state, pointer state, and scene clock.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
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
struct wl_keyboard;
struct wl_output;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;
struct zwp_linux_dmabuf_v1;
struct zwp_linux_dmabuf_feedback_v1;
struct wp_viewporter;
struct wp_viewport;
struct wp_fractional_scale_manager_v1;
struct wp_fractional_scale_v1;

namespace drift::platform {

enum class SurfaceMode {
    Wallpaper,  // wlr-layer-shell background layer per output
    Windowed,   // single xdg toplevel dev window
    Fullscreen, // single xdg toplevel, fullscreened by the compositor
};

class WaylandApp {
public:
    ~WaylandApp();

    // Connects, binds globals, creates the initial surfaces and negotiates
    // the buffer format (compositor-supported ∩ Dawn-importable modifiers).
    // width/height are the initial size in Windowed mode and ignored in
    // Wallpaper mode (the compositor sizes layer surfaces).
    bool setup(Gpu& gpu, SurfaceMode mode, uint32_t width, uint32_t height);

    // Wallpaper mode: only claim outputs whose connector name (wl_output.name,
    // e.g. "DP-1") is listed; empty = every output. Set before setup().
    // Names need wl_output v4 — on older compositors a filter matches
    // nothing. Filtered-out outputs are released; names requested but not
    // present attach if the output hotplugs later (with a filter, setup()
    // succeeds even when no output matches yet and waits for hotplug).
    void setOutputFilter(std::vector<std::string> names)
    {
        mOutputFilter = std::move(names);
    }

    // Connector names of the outputs claimed so far (surfaces created).
    // After setup() the app uses this to drop validation scene instances
    // for named outputs that are absent; hotplug re-loads lazily.
    std::vector<std::string> claimedOutputs() const
    {
        std::vector<std::string> names;
        for (const auto& surf : mSurfaces) {
            names.push_back(surf->name);
        }
        return names;
    }

    wgpu::TextureFormat targetFormat() const { return mFormat; }

    // One render callback invocation per surface per frame. outputId is a
    // stable identity for the lifetime of that output's surface (the
    // wl_output registry name; 0 in windowed mode) — the app keeps one
    // scene instance per outputId. outputName is the connector name
    // ("DP-1"; empty in windowed mode and on compositors without wl_output
    // v4) — the app picks which scene to instantiate by it. seconds is
    // that surface's scene clock: it advances by capped per-frame deltas,
    // so it freezes across pauses (occlusion, lock) per SCENE_FORMAT.md
    // §9.7. Returns whether the target was written; when false nothing is
    // committed (§11).
    struct FrameRequest {
        uint32_t outputId = 0;
        std::string outputName;
        wgpu::TextureView target;
        uint32_t width = 0, height = 0;
        double seconds = 0.0;
        float mouseX = 0.5f, mouseY = 0.5f;
        bool mouseActive = false;
    };
    using RenderFrame = std::function<bool(const FrameRequest&)>;

    // Notifies when an output's surface is destroyed (hotplug removal) so
    // the app can drop that output's scene.
    using OutputRemoved = std::function<void(uint32_t)>;
    void setOutputRemoved(OutputRemoved cb) { mOutputRemoved = std::move(cb); }

    // External control endpoint (ControlServer): fd is polled alongside the
    // display and cb runs when it is readable. The server owns its own
    // epoll set, so one fd covers all of its connections.
    void setControl(int fd, std::function<void()> cb)
    {
        mControlFd = fd;
        mControlCb = std::move(cb);
    }

    // §4.4 external wakes. fd (an eventfd, owned by the caller) becomes
    // readable when a module delivery needs a frame — the ModuleNet wake
    // callback writes it from the network thread; the loop drains it and
    // redraws, and the scene graph decides what actually re-executes.
    void setWakeFd(int fd) { mWakeFd = fd; }

    // Earliest module timer deadline (wake_after_ms, in *scene time*) for
    // an output's scene; negative = none. Consulted for the idle poll
    // timeout, and to let the scene clock stride past the usual resume
    // cap up to a due deadline — a deliberate timer sleep is elapsed
    // scene time, unlike an occlusion pause (SCENE_FORMAT.md §9.7).
    using WakeQuery = std::function<double(uint32_t outputId)>;
    void setWakeQuery(WakeQuery query) { mWakeQuery = std::move(query); }

    // Requests a redraw on every surface (a parameter changed): the frame
    // evaluates the graph, which decides what actually re-executes (§11) —
    // for non-animated scenes this is the only wakeup a set would get.
    void requestRedrawAll()
    {
        for (auto& surf : mSurfaces) {
            surf->wantRedraw = true;
        }
    }

    // The scene clock, for the control endpoint's "time" method. Surfaces
    // keep per-surface clocks that pause under occlusion; the shared clock
    // (§17.6) is the furthest one along.
    double currentSceneTime() const
    {
        double t = 0.0;
        for (const auto& surf : mSurfaces) {
            t = std::max(t, surf->sceneTime);
        }
        return t;
    }

    // Transport (control endpoint). Pause freezes the scene clock — the
    // time node then produces nothing dirty, so the graph quiesces on its
    // own (§11); rendering still reacts to input/parameter changes.
    void setScenePaused(bool paused) { mScenePaused = paused; }
    bool scenePaused() const { return mScenePaused; }

    // Jumps every surface clock (forward-only within live instances, §9.7;
    // the caller handles backward by re-creating the scene instances).
    void seekSceneTime(double seconds)
    {
        for (auto& surf : mSurfaces) {
            surf->sceneTime = seconds;
            surf->wantRedraw = true;
        }
    }

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
        uint32_t id = 0;  // wl_output registry name; 0 = windowed
        std::string name; // connector name; empty = windowed or wl_output <v4
        uint32_t outputVersion = 0; // bound wl_output version (release vs destroy)
        wl_output* output = nullptr;
        wl_surface* surface = nullptr;
        zwlr_layer_surface_v1* layerSurface = nullptr;
        xdg_surface* xdgSurface = nullptr;
        xdg_toplevel* toplevel = nullptr;
        wp_viewport* viewport = nullptr;
        wp_fractional_scale_v1* fractionalScale = nullptr;
        std::vector<Buffer> buffers;
        // Logical (surface-coordinate) size from configure; buffers are
        // allocated at logical x scale physical pixels and mapped back via
        // the viewport, so scaled outputs render at native resolution.
        uint32_t width = 0, height = 0;
        uint32_t pendingWidth = 0, pendingHeight = 0;
        uint32_t bufferWidth = 0, bufferHeight = 0;
        double scale = 1.0;     // wp-fractional-scale preferred scale
        double ringScale = 1.0; // scale the current ring was built for
        bool configured = false;
        bool framePending = false; // a commit awaits its frame callback
        bool wantRedraw = false;   // input/configure while idle
        bool needRedraw = false;   // all buffers busy; retry on release
        bool pointerOver = false;
        bool pointerSeen = false;
        double pointerX = 0.0, pointerY = 0.0; // surface-local
        double sceneTime = 0.0;
        double lastDrawTime = -1.0;
        // §4.4 module timer: wall-clock instant the earliest
        // wake_after_ms deadline is due, anchored once when the idle
        // loop arms a sleep for it (so interleaved wakeups shrink the
        // remaining wait instead of restarting it) and cleared by every
        // draw. Negative = not armed. Only armed sleeps may stretch the
        // scene clock past the §9.7 resume cap — an occlusion pause
        // never arms, so it still resumes at +0.1s.
        double wakeDueWall = -1.0;
    };

    // A bound wl_output whose surface decision is still pending: the name
    // event (v4) must arrive before the filter can be evaluated, so
    // surface creation waits for the initial property burst (the done
    // event; for hotplug — setup()'s roundtrips cover the initial ones).
    struct PendingOutput {
        WaylandApp* app = nullptr;
        wl_output* output = nullptr;
        uint32_t id = 0;      // wl_output registry name
        uint32_t version = 0; // bound version
        std::string name;     // connector name; empty until the v4 event
        bool done = false;    // initial property burst completed
    };

    void onGlobal(uint32_t name, const char* interface, uint32_t version);
    void onGlobalRemove(uint32_t name);
    void onOutputName(PendingOutput* pending, const char* name);
    void onOutputDone(PendingOutput* pending);
    void onDmabufModifier(uint32_t fourcc, uint64_t modifier);
    void onFeedbackFormatTable(int32_t fd, uint32_t size);
    void onFeedbackTrancheFormats(const uint16_t* indices, size_t count);
    void onFeedbackDone();
    void onSeatCapabilities(uint32_t capabilities);
    void onPointerEnter(wl_surface* surface, double x, double y);
    void onPointerLeave(wl_surface* surface);
    void onPointerMotion(double x, double y);
    void onKey(uint32_t key, uint32_t state);
    void onXdgSurfaceConfigure(OutputSurface* surf, uint32_t serial);
    void onToplevelConfigure(OutputSurface* surf, int32_t width, int32_t height);
    void onLayerConfigure(OutputSurface* surf, uint32_t serial, uint32_t width,
                          uint32_t height);
    void onSurfaceClosed(OutputSurface* surf);
    void onPreferredScale(OutputSurface* surf, uint32_t scale120);
    void onFrameDone(OutputSurface* surf);
    void onBufferRelease(wl_buffer* buffer);

private:
    bool createOutputSurface(wl_output* output, uint32_t id,
                             const std::string& name = {},
                             uint32_t outputVersion = 0);
    void destroyOutputSurface(OutputSurface* surf);
    // Resolves a pending output: creates its surface if the filter admits
    // it, releases it otherwise. Removes it from mPendingOutputs.
    void adoptPendingOutput(PendingOutput* pending);
    bool matchesFilter(const std::string& name) const
    {
        return mOutputFilter.empty() ||
               std::find(mOutputFilter.begin(), mOutputFilter.end(), name) !=
                   mOutputFilter.end();
    }
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
    wp_viewporter* mViewporter = nullptr;
    wp_fractional_scale_manager_v1* mFractionalScaleManager = nullptr;

    // dmabuf v4 default feedback: mmap'd format+modifier table. On v3 the
    // flat modifier event list fills mCompositorModifiers directly.
    zwp_linux_dmabuf_feedback_v1* mFeedback = nullptr;
    const void* mFormatTable = nullptr;
    uint32_t mFormatTableSize = 0;
    bool mFeedbackAccumulating = false;

    wl_seat* mSeat = nullptr;
    wl_pointer* mPointer = nullptr;
    wl_keyboard* mKeyboard = nullptr; // toplevel modes only (Esc closes)
    wl_surface* mPointerSurface = nullptr; // surface under the pointer

    std::vector<std::unique_ptr<OutputSurface>> mSurfaces;
    std::vector<std::unique_ptr<PendingOutput>> mPendingOutputs;
    std::vector<std::string> mOutputFilter;
    bool mSetupDone = false;

    // (fourcc, modifier) pairs the compositor accepts, from zwp_linux_dmabuf v3.
    std::map<uint32_t, std::vector<uint64_t>> mCompositorModifiers;
    std::vector<uint64_t> mRingModifiers; // chosen format's usable modifiers
    uint32_t mFourcc = 0;
    wgpu::TextureFormat mFormat = wgpu::TextureFormat::Undefined;

    bool mRunning = true;
    bool mAnimated = false;
    bool mScenePaused = false;
    RenderFrame mRenderFrame;
    OutputRemoved mOutputRemoved;
    int mControlFd = -1;
    std::function<void()> mControlCb;
    int mWakeFd = -1;
    WakeQuery mWakeQuery;
    double mStartTime = 0.0;
};

} // namespace drift::platform
