#pragma once

// Wayland presentation: connects to the compositor, creates either a
// background layer surface (wallpaper) or an xdg toplevel (windowed dev
// mode), and runs a frame-callback-driven loop presenting dmabuf-backed
// render targets via linux-dmabuf.

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "Gpu.h"

struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_surface;
struct wl_buffer;
struct wl_callback;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;
struct zwp_linux_dmabuf_v1;

namespace drift::platform {

enum class SurfaceMode {
    Wallpaper, // wlr-layer-shell background layer, sized by the compositor
    Windowed,  // xdg toplevel dev window
};

class WaylandApp {
public:
    ~WaylandApp();

    // Connects, creates the surface, negotiates the buffer format
    // (compositor-supported ∩ Dawn-importable modifiers) and allocates the
    // buffer ring. width/height are the initial size in Windowed mode and
    // ignored in Wallpaper mode.
    bool setup(Gpu& gpu, SurfaceMode mode, uint32_t width, uint32_t height);

    wgpu::TextureFormat targetFormat() const { return mFormat; }

    // Frame loop until the surface is closed. renderFrame(targetView, width,
    // height, timeSeconds) records and submits the frame's GPU work.
    using RenderFrame =
        std::function<void(const wgpu::TextureView&, uint32_t, uint32_t, float)>;
    int run(RenderFrame renderFrame);

    // -- internal, public for C listener trampolines --
    void onGlobal(uint32_t name, const char* interface, uint32_t version);
    void onDmabufModifier(uint32_t fourcc, uint64_t modifier);
    void onXdgSurfaceConfigure(uint32_t serial);
    void onToplevelConfigure(int32_t width, int32_t height);
    void onLayerConfigure(uint32_t serial, uint32_t width, uint32_t height);
    void onClosed();
    void onFrameDone();
    void onBufferRelease(wl_buffer* buffer);

private:
    struct Buffer {
        DmabufTarget target;
        wl_buffer* buffer = nullptr;
        bool busy = false;
    };

    bool chooseFormatAndCreateRing();
    void destroyRing();
    bool createWlBuffer(Buffer& buf);
    void drawFrame();
    double now() const;

    Gpu* mGpu = nullptr;
    SurfaceMode mMode = SurfaceMode::Wallpaper;

    wl_display* mDisplay = nullptr;
    wl_registry* mRegistry = nullptr;
    wl_compositor* mCompositor = nullptr;
    xdg_wm_base* mWmBase = nullptr;
    zwlr_layer_shell_v1* mLayerShell = nullptr;
    zwp_linux_dmabuf_v1* mDmabuf = nullptr;

    wl_surface* mSurface = nullptr;
    xdg_surface* mXdgSurface = nullptr;
    xdg_toplevel* mToplevel = nullptr;
    zwlr_layer_surface_v1* mLayerSurface = nullptr;

    // (fourcc, modifier) pairs the compositor accepts, from zwp_linux_dmabuf v3.
    std::map<uint32_t, std::vector<uint64_t>> mCompositorModifiers;

    std::vector<Buffer> mBuffers;
    uint32_t mWidth = 0, mHeight = 0;         // current ring size
    uint32_t mPendingWidth = 0, mPendingHeight = 0;
    bool mConfigured = false;
    bool mRunning = true;
    bool mNeedRedraw = false;
    uint32_t mFourcc = 0;
    wgpu::TextureFormat mFormat = wgpu::TextureFormat::Undefined;
    RenderFrame mRenderFrame;
    double mStartTime = 0.0;
};

} // namespace drift::platform
