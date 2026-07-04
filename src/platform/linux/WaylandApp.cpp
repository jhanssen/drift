#include "WaylandApp.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include <wayland-client.h>
#include <drm_fourcc.h>

#include "xdg-shell-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"

namespace drift::platform {

namespace {

// ---- C listener trampolines ----

void registryGlobal(void* data, wl_registry*, uint32_t name,
                    const char* interface, uint32_t version)
{
    static_cast<WaylandApp*>(data)->onGlobal(name, interface, version);
}
void registryGlobalRemove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener = { registryGlobal, registryGlobalRemove };

void dmabufFormat(void*, zwp_linux_dmabuf_v1*, uint32_t) {} // deprecated, ignore
void dmabufModifier(void* data, zwp_linux_dmabuf_v1*, uint32_t format,
                    uint32_t modifierHi, uint32_t modifierLo)
{
    static_cast<WaylandApp*>(data)->onDmabufModifier(
        format, ((uint64_t)modifierHi << 32) | modifierLo);
}
const zwp_linux_dmabuf_v1_listener kDmabufListener = { dmabufFormat, dmabufModifier };

void seatCapabilities(void* data, wl_seat*, uint32_t capabilities)
{
    static_cast<WaylandApp*>(data)->onSeatCapabilities(capabilities);
}
void seatName(void*, wl_seat*, const char*) {}
const wl_seat_listener kSeatListener = { seatCapabilities, seatName };

void pointerEnter(void* data, wl_pointer*, uint32_t /*serial*/,
                  wl_surface* surface, wl_fixed_t sx, wl_fixed_t sy)
{
    static_cast<WaylandApp*>(data)->onPointerEnter(
        surface, wl_fixed_to_double(sx), wl_fixed_to_double(sy));
}
void pointerLeave(void* data, wl_pointer*, uint32_t, wl_surface* surface)
{
    static_cast<WaylandApp*>(data)->onPointerLeave(surface);
}
void pointerMotion(void* data, wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy)
{
    static_cast<WaylandApp*>(data)->onPointerMotion(wl_fixed_to_double(sx),
                                                    wl_fixed_to_double(sy));
}
void pointerButton(void*, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t) {}
void pointerAxis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
const wl_pointer_listener kPointerListener = {
    pointerEnter, pointerLeave, pointerMotion, pointerButton, pointerAxis
};

void wmBasePing(void*, xdg_wm_base* wmBase, uint32_t serial)
{
    xdg_wm_base_pong(wmBase, serial);
}
const xdg_wm_base_listener kWmBaseListener = { wmBasePing };

void xdgSurfaceConfigure(void* data, xdg_surface*, uint32_t serial)
{
    static_cast<WaylandApp*>(data)->onXdgSurfaceConfigure(serial);
}
const xdg_surface_listener kXdgSurfaceListener = { xdgSurfaceConfigure };

void toplevelConfigure(void* data, xdg_toplevel*, int32_t width, int32_t height,
                       wl_array*)
{
    static_cast<WaylandApp*>(data)->onToplevelConfigure(width, height);
}
void toplevelClose(void* data, xdg_toplevel*)
{
    static_cast<WaylandApp*>(data)->onClosed();
}
void toplevelConfigureBounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void toplevelWmCapabilities(void*, xdg_toplevel*, wl_array*) {}
const xdg_toplevel_listener kToplevelListener = {
    toplevelConfigure, toplevelClose, toplevelConfigureBounds, toplevelWmCapabilities
};

void layerConfigure(void* data, zwlr_layer_surface_v1*, uint32_t serial,
                    uint32_t width, uint32_t height)
{
    static_cast<WaylandApp*>(data)->onLayerConfigure(serial, width, height);
}
void layerClosed(void* data, zwlr_layer_surface_v1*)
{
    static_cast<WaylandApp*>(data)->onClosed();
}
const zwlr_layer_surface_v1_listener kLayerListener = { layerConfigure, layerClosed };

void frameDone(void* data, wl_callback* callback, uint32_t)
{
    wl_callback_destroy(callback);
    static_cast<WaylandApp*>(data)->onFrameDone();
}
const wl_callback_listener kFrameListener = { frameDone };

void bufferRelease(void* data, wl_buffer* buffer)
{
    static_cast<WaylandApp*>(data)->onBufferRelease(buffer);
}
const wl_buffer_listener kBufferListener = { bufferRelease };

// Buffer format candidates: opaque formats first (better for a wallpaper —
// the compositor can skip blending), premultiplied-alpha variants as
// fallback. wgpu format is what the imported SharedTextureMemory views as.
struct FormatCandidate {
    uint32_t fourcc;
    wgpu::TextureFormat wgpu;
};
const FormatCandidate kFormatCandidates[] = {
    { DRM_FORMAT_XBGR8888, wgpu::TextureFormat::RGBA8Unorm },
    { DRM_FORMAT_XRGB8888, wgpu::TextureFormat::BGRA8Unorm },
    { DRM_FORMAT_ABGR8888, wgpu::TextureFormat::RGBA8Unorm },
    { DRM_FORMAT_ARGB8888, wgpu::TextureFormat::BGRA8Unorm },
};

constexpr int kBufferCount = 2;

} // namespace

WaylandApp::~WaylandApp()
{
    destroyRing();
    if (mPointer) wl_pointer_destroy(mPointer);
    if (mSeat) wl_seat_destroy(mSeat);
    if (mLayerSurface) zwlr_layer_surface_v1_destroy(mLayerSurface);
    if (mToplevel) xdg_toplevel_destroy(mToplevel);
    if (mXdgSurface) xdg_surface_destroy(mXdgSurface);
    if (mSurface) wl_surface_destroy(mSurface);
    if (mDmabuf) zwp_linux_dmabuf_v1_destroy(mDmabuf);
    if (mLayerShell) zwlr_layer_shell_v1_destroy(mLayerShell);
    if (mWmBase) xdg_wm_base_destroy(mWmBase);
    if (mCompositor) wl_compositor_destroy(mCompositor);
    if (mRegistry) wl_registry_destroy(mRegistry);
    if (mDisplay) wl_display_disconnect(mDisplay);
}

void WaylandApp::onGlobal(uint32_t name, const char* interface, uint32_t version)
{
    if (!strcmp(interface, wl_compositor_interface.name)) {
        mCompositor = (wl_compositor*)wl_registry_bind(
            mRegistry, name, &wl_compositor_interface, std::min(version, 4u));
    } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
        mWmBase = (xdg_wm_base*)wl_registry_bind(
            mRegistry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(mWmBase, &kWmBaseListener, this);
    } else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
        mLayerShell = (zwlr_layer_shell_v1*)wl_registry_bind(
            mRegistry, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (!strcmp(interface, zwp_linux_dmabuf_v1_interface.name)) {
        // Bind v3 for the flat (format, modifier) event list. TODO: v4
        // feedback for per-surface format preferences.
        mDmabuf = (zwp_linux_dmabuf_v1*)wl_registry_bind(
            mRegistry, name, &zwp_linux_dmabuf_v1_interface, std::min(version, 3u));
        zwp_linux_dmabuf_v1_add_listener(mDmabuf, &kDmabufListener, this);
    } else if (!strcmp(interface, wl_seat_interface.name)) {
        mSeat = (wl_seat*)wl_registry_bind(mRegistry, name, &wl_seat_interface,
                                           std::min(version, 2u));
        wl_seat_add_listener(mSeat, &kSeatListener, this);
    }
}

void WaylandApp::onSeatCapabilities(uint32_t capabilities)
{
    const bool hasPointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
    if (hasPointer && !mPointer) {
        mPointer = wl_seat_get_pointer(mSeat);
        wl_pointer_add_listener(mPointer, &kPointerListener, this);
    } else if (!hasPointer && mPointer) {
        wl_pointer_destroy(mPointer);
        mPointer = nullptr;
        mPointerOver = false;
    }
}

void WaylandApp::onPointerEnter(wl_surface* surface, double x, double y)
{
    if (surface != mSurface) {
        return;
    }
    mPointerOver = true;
    mPointerSeen = true;
    mPointerX = x;
    mPointerY = y;
}

void WaylandApp::onPointerLeave(wl_surface* surface)
{
    if (surface == mSurface) {
        mPointerOver = false;
    }
}

void WaylandApp::onPointerMotion(double x, double y)
{
    if (mPointerOver) {
        mPointerX = x;
        mPointerY = y;
    }
}

void WaylandApp::onDmabufModifier(uint32_t fourcc, uint64_t modifier)
{
    mCompositorModifiers[fourcc].push_back(modifier);
}

void WaylandApp::onXdgSurfaceConfigure(uint32_t serial)
{
    xdg_surface_ack_configure(mXdgSurface, serial);
    mConfigured = true;
}

void WaylandApp::onToplevelConfigure(int32_t width, int32_t height)
{
    if (width > 0 && height > 0) {
        mPendingWidth = (uint32_t)width;
        mPendingHeight = (uint32_t)height;
    }
}

void WaylandApp::onLayerConfigure(uint32_t serial, uint32_t width, uint32_t height)
{
    zwlr_layer_surface_v1_ack_configure(mLayerSurface, serial);
    if (width > 0 && height > 0) {
        mPendingWidth = width;
        mPendingHeight = height;
    }
    mConfigured = true;
}

void WaylandApp::onClosed()
{
    mRunning = false;
}

void WaylandApp::onFrameDone()
{
    drawFrame();
}

void WaylandApp::onBufferRelease(wl_buffer* buffer)
{
    for (auto& buf : mBuffers) {
        if (buf.buffer == buffer) {
            buf.busy = false;
        }
    }
    if (mNeedRedraw) {
        mNeedRedraw = false;
        drawFrame();
    }
}

bool WaylandApp::setup(Gpu& gpu, SurfaceMode mode, uint32_t width, uint32_t height)
{
    mGpu = &gpu;
    mMode = mode;
    mPendingWidth = width;
    mPendingHeight = height;

    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    mStartTime = ts.tv_sec + ts.tv_nsec * 1e-9;

    mDisplay = wl_display_connect(nullptr);
    if (!mDisplay) {
        fprintf(stderr, "drift: cannot connect to Wayland display\n");
        return false;
    }
    mRegistry = wl_display_get_registry(mDisplay);
    wl_registry_add_listener(mRegistry, &kRegistryListener, this);
    wl_display_roundtrip(mDisplay); // globals
    wl_display_roundtrip(mDisplay); // dmabuf modifier events

    if (!mCompositor || !mDmabuf) {
        fprintf(stderr, "drift: compositor lacks wl_compositor or zwp_linux_dmabuf_v1\n");
        return false;
    }

    mSurface = wl_compositor_create_surface(mCompositor);

    if (mMode == SurfaceMode::Wallpaper) {
        if (!mLayerShell) {
            fprintf(stderr,
                    "drift: compositor does not support wlr-layer-shell "
                    "(required for wallpaper mode; try --windowed)\n");
            return false;
        }
        mLayerSurface = zwlr_layer_shell_v1_get_layer_surface(
            mLayerShell, mSurface, nullptr /* compositor picks output */,
            ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "wallpaper");
        zwlr_layer_surface_v1_add_listener(mLayerSurface, &kLayerListener, this);
        zwlr_layer_surface_v1_set_size(mLayerSurface, 0, 0);
        zwlr_layer_surface_v1_set_anchor(
            mLayerSurface,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_exclusive_zone(mLayerSurface, -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            mLayerSurface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    } else {
        if (!mWmBase) {
            fprintf(stderr, "drift: compositor lacks xdg_wm_base\n");
            return false;
        }
        mXdgSurface = xdg_wm_base_get_xdg_surface(mWmBase, mSurface);
        xdg_surface_add_listener(mXdgSurface, &kXdgSurfaceListener, this);
        mToplevel = xdg_surface_get_toplevel(mXdgSurface);
        xdg_toplevel_add_listener(mToplevel, &kToplevelListener, this);
        xdg_toplevel_set_title(mToplevel, "drift");
        xdg_toplevel_set_app_id(mToplevel, "drift");
    }

    wl_surface_commit(mSurface);
    while (!mConfigured && mRunning) {
        if (wl_display_dispatch(mDisplay) == -1) {
            return false;
        }
    }
    if (!mRunning) {
        return false;
    }

    if (!chooseFormatAndCreateRing()) {
        fprintf(stderr, "drift: no dmabuf format supported by both compositor and GPU\n");
        return false;
    }
    return true;
}

bool WaylandApp::chooseFormatAndCreateRing()
{
    mWidth = mPendingWidth;
    mHeight = mPendingHeight;

    for (const auto& cand : kFormatCandidates) {
        auto it = mCompositorModifiers.find(cand.fourcc);
        if (it == mCompositorModifiers.end()) {
            continue;
        }
        // Intersect Dawn-importable with compositor-accepted, keeping Dawn's
        // preference order.
        std::vector<uint64_t> mods;
        for (uint64_t m : mGpu->importableModifiers(cand.wgpu)) {
            for (uint64_t cm : it->second) {
                if (m == cm) {
                    mods.push_back(m);
                    break;
                }
            }
        }
        if (mods.empty()) {
            continue;
        }

        std::vector<Buffer> buffers(kBufferCount);
        bool ok = true;
        for (auto& buf : buffers) {
            if (!mGpu->createTarget(mWidth, mHeight, cand.fourcc, mods, buf.target) ||
                !createWlBuffer(buf)) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            for (auto& buf : buffers) {
                if (buf.buffer) wl_buffer_destroy(buf.buffer);
                mGpu->destroyTarget(buf.target);
            }
            continue;
        }

        mBuffers = std::move(buffers);
        mFourcc = cand.fourcc;
        mFormat = cand.wgpu;

        // The whole surface is opaque (background wallpaper).
        wl_region* region = wl_compositor_create_region(mCompositor);
        wl_region_add(region, 0, 0, (int32_t)mWidth, (int32_t)mHeight);
        wl_surface_set_opaque_region(mSurface, region);
        wl_region_destroy(region);
        return true;
    }
    return false;
}

bool WaylandApp::createWlBuffer(Buffer& buf)
{
    zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(mDmabuf);
    for (size_t i = 0; i < buf.target.planes.size(); ++i) {
        zwp_linux_buffer_params_v1_add(
            params, buf.target.fd, (uint32_t)i,
            buf.target.planes[i].offset, buf.target.planes[i].stride,
            (uint32_t)(buf.target.modifier >> 32),
            (uint32_t)(buf.target.modifier & 0xffffffff));
    }
    buf.buffer = zwp_linux_buffer_params_v1_create_immed(
        params, (int32_t)buf.target.width, (int32_t)buf.target.height,
        buf.target.fourcc, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    if (!buf.buffer) {
        return false;
    }
    wl_buffer_add_listener(buf.buffer, &kBufferListener, this);
    return true;
}

void WaylandApp::destroyRing()
{
    for (auto& buf : mBuffers) {
        if (buf.buffer) {
            wl_buffer_destroy(buf.buffer);
        }
        if (mGpu) {
            mGpu->destroyTarget(buf.target);
        }
    }
    mBuffers.clear();
}

double WaylandApp::now() const
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec + ts.tv_nsec * 1e-9) - mStartTime;
}

void WaylandApp::drawFrame()
{
    if (!mRunning) {
        return;
    }

    // Resize: reallocate the ring at the new size.
    if (mPendingWidth != mWidth || mPendingHeight != mHeight) {
        destroyRing();
        if (!chooseFormatAndCreateRing()) {
            fprintf(stderr, "drift: buffer reallocation after resize failed\n");
            mRunning = false;
            return;
        }
    }

    Buffer* buf = nullptr;
    for (auto& b : mBuffers) {
        if (!b.busy) {
            buf = &b;
            break;
        }
    }
    if (!buf) {
        // All buffers held by the compositor; redraw on next release.
        mNeedRedraw = true;
        return;
    }

    mGpu->beginFrame(buf->target);
    mRenderFrame(buf->target.texture.CreateView(), mWidth, mHeight, (float)now());
    mGpu->endFrame(buf->target);

    wl_surface_attach(mSurface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(mSurface, 0, 0, INT32_MAX, INT32_MAX);
    wl_callback* cb = wl_surface_frame(mSurface);
    wl_callback_add_listener(cb, &kFrameListener, this);
    wl_surface_commit(mSurface);
    buf->busy = true;
}

int WaylandApp::run(RenderFrame renderFrame)
{
    mRenderFrame = std::move(renderFrame);
    drawFrame();
    while (mRunning && wl_display_dispatch(mDisplay) != -1) {
    }
    return 0;
}

} // namespace drift::platform
