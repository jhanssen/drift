#include "WaylandApp.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include <drm_fourcc.h>

#include "xdg-shell-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"

namespace drift::platform {

namespace {

using OutputSurface = WaylandApp::OutputSurface;

// ---- C listener trampolines ----

void registryGlobal(void* data, wl_registry*, uint32_t name,
                    const char* interface, uint32_t version)
{
    static_cast<WaylandApp*>(data)->onGlobal(name, interface, version);
}
void registryGlobalRemove(void* data, wl_registry*, uint32_t name)
{
    static_cast<WaylandApp*>(data)->onGlobalRemove(name);
}
const wl_registry_listener kRegistryListener = { registryGlobal, registryGlobalRemove };

void dmabufFormat(void*, zwp_linux_dmabuf_v1*, uint32_t) {} // deprecated, ignore
void dmabufModifier(void* data, zwp_linux_dmabuf_v1*, uint32_t format,
                    uint32_t modifierHi, uint32_t modifierLo)
{
    static_cast<WaylandApp*>(data)->onDmabufModifier(
        format, ((uint64_t)modifierHi << 32) | modifierLo);
}
const zwp_linux_dmabuf_v1_listener kDmabufListener = { dmabufFormat, dmabufModifier };

void feedbackDone(void* data, zwp_linux_dmabuf_feedback_v1*)
{
    static_cast<WaylandApp*>(data)->onFeedbackDone();
}
void feedbackFormatTable(void* data, zwp_linux_dmabuf_feedback_v1*, int32_t fd,
                         uint32_t size)
{
    static_cast<WaylandApp*>(data)->onFeedbackFormatTable(fd, size);
}
void feedbackMainDevice(void*, zwp_linux_dmabuf_feedback_v1*, wl_array*) {}
void feedbackTrancheDone(void*, zwp_linux_dmabuf_feedback_v1*) {}
void feedbackTrancheTargetDevice(void*, zwp_linux_dmabuf_feedback_v1*, wl_array*) {}
void feedbackTrancheFormats(void* data, zwp_linux_dmabuf_feedback_v1*,
                            wl_array* indices)
{
    static_cast<WaylandApp*>(data)->onFeedbackTrancheFormats(
        (const uint16_t*)indices->data, indices->size / sizeof(uint16_t));
}
void feedbackTrancheFlags(void*, zwp_linux_dmabuf_feedback_v1*, uint32_t) {}
const zwp_linux_dmabuf_feedback_v1_listener kFeedbackListener = {
    feedbackDone,        feedbackFormatTable,          feedbackMainDevice,
    feedbackTrancheDone, feedbackTrancheTargetDevice,  feedbackTrancheFormats,
    feedbackTrancheFlags
};

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

void fractionalScalePreferred(void* data, wp_fractional_scale_v1*,
                              uint32_t scale120)
{
    auto* surf = static_cast<OutputSurface*>(data);
    surf->app->onPreferredScale(surf, scale120);
}
const wp_fractional_scale_v1_listener kFractionalScaleListener = {
    fractionalScalePreferred
};

void wmBasePing(void*, xdg_wm_base* wmBase, uint32_t serial)
{
    xdg_wm_base_pong(wmBase, serial);
}
const xdg_wm_base_listener kWmBaseListener = { wmBasePing };

void xdgSurfaceConfigure(void* data, xdg_surface*, uint32_t serial)
{
    auto* surf = static_cast<OutputSurface*>(data);
    surf->app->onXdgSurfaceConfigure(surf, serial);
}
const xdg_surface_listener kXdgSurfaceListener = { xdgSurfaceConfigure };

void toplevelConfigure(void* data, xdg_toplevel*, int32_t width, int32_t height,
                       wl_array*)
{
    auto* surf = static_cast<OutputSurface*>(data);
    surf->app->onToplevelConfigure(surf, width, height);
}
void toplevelClose(void* data, xdg_toplevel*)
{
    auto* surf = static_cast<OutputSurface*>(data);
    surf->app->onSurfaceClosed(surf);
}
void toplevelConfigureBounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void toplevelWmCapabilities(void*, xdg_toplevel*, wl_array*) {}
const xdg_toplevel_listener kToplevelListener = {
    toplevelConfigure, toplevelClose, toplevelConfigureBounds, toplevelWmCapabilities
};

void layerConfigure(void* data, zwlr_layer_surface_v1*, uint32_t serial,
                    uint32_t width, uint32_t height)
{
    auto* surf = static_cast<OutputSurface*>(data);
    surf->app->onLayerConfigure(surf, serial, width, height);
}
void layerClosed(void* data, zwlr_layer_surface_v1*)
{
    auto* surf = static_cast<OutputSurface*>(data);
    surf->app->onSurfaceClosed(surf);
}
const zwlr_layer_surface_v1_listener kLayerListener = { layerConfigure, layerClosed };

void frameDone(void* data, wl_callback* callback, uint32_t)
{
    wl_callback_destroy(callback);
    auto* surf = static_cast<OutputSurface*>(data);
    surf->app->onFrameDone(surf);
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
    while (!mSurfaces.empty()) {
        destroyOutputSurface(mSurfaces.back().get());
    }
    if (mPointer) wl_pointer_destroy(mPointer);
    if (mSeat) wl_seat_destroy(mSeat);
    if (mFractionalScaleManager)
        wp_fractional_scale_manager_v1_destroy(mFractionalScaleManager);
    if (mViewporter) wp_viewporter_destroy(mViewporter);
    if (mFeedback) zwp_linux_dmabuf_feedback_v1_destroy(mFeedback);
    if (mFormatTable) munmap(const_cast<void*>(mFormatTable), mFormatTableSize);
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
        // v4: format+modifier table via the default feedback object. v3
        // fallback: the flat modifier event list. (Per-surface feedback and
        // scanout tranche preferences are a future optimization — the
        // default tranches are what we can honor with one ring per output.)
        const uint32_t bound = std::min(version, 4u);
        mDmabuf = (zwp_linux_dmabuf_v1*)wl_registry_bind(
            mRegistry, name, &zwp_linux_dmabuf_v1_interface, bound);
        if (bound >= 4) {
            mFeedback = zwp_linux_dmabuf_v1_get_default_feedback(mDmabuf);
            zwp_linux_dmabuf_feedback_v1_add_listener(mFeedback,
                                                      &kFeedbackListener, this);
        } else {
            zwp_linux_dmabuf_v1_add_listener(mDmabuf, &kDmabufListener, this);
        }
    } else if (!strcmp(interface, wp_viewporter_interface.name)) {
        mViewporter = (wp_viewporter*)wl_registry_bind(
            mRegistry, name, &wp_viewporter_interface, 1);
    } else if (!strcmp(interface, wp_fractional_scale_manager_v1_interface.name)) {
        mFractionalScaleManager =
            (wp_fractional_scale_manager_v1*)wl_registry_bind(
                mRegistry, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (!strcmp(interface, wl_seat_interface.name)) {
        mSeat = (wl_seat*)wl_registry_bind(mRegistry, name, &wl_seat_interface,
                                           std::min(version, 2u));
        wl_seat_add_listener(mSeat, &kSeatListener, this);
    } else if (!strcmp(interface, wl_output_interface.name)) {
        if (mMode != SurfaceMode::Wallpaper) {
            return;
        }
        auto* output = (wl_output*)wl_registry_bind(
            mRegistry, name, &wl_output_interface, std::min(version, 2u));
        if (mSetupDone) {
            createOutputSurface(output, name); // hotplug
        } else {
            // The initial registry burst may announce outputs before
            // wl_compositor / layer shell; setup() creates the surfaces
            // after the roundtrips.
            mPendingOutputs.emplace_back(output, name);
        }
    }
}

void WaylandApp::onGlobalRemove(uint32_t name)
{
    for (auto& surf : mSurfaces) {
        if (surf->id == name) {
            destroyOutputSurface(surf.get());
            return;
        }
    }
}

void WaylandApp::onDmabufModifier(uint32_t fourcc, uint64_t modifier)
{
    mCompositorModifiers[fourcc].push_back(modifier);
}

void WaylandApp::onFeedbackFormatTable(int32_t fd, uint32_t size)
{
    if (mFormatTable) {
        munmap(const_cast<void*>(mFormatTable), mFormatTableSize);
        mFormatTable = nullptr;
        mFormatTableSize = 0;
    }
    void* table = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (table == MAP_FAILED) {
        fprintf(stderr, "drift: cannot map dmabuf format table\n");
        return;
    }
    mFormatTable = table;
    mFormatTableSize = size;
}

void WaylandApp::onFeedbackTrancheFormats(const uint16_t* indices, size_t count)
{
    if (!mFormatTable) {
        return;
    }
    // Feedback parameters arrive as a full set ending in done: the first
    // tranche of a new set replaces the previous table-derived state.
    if (!mFeedbackAccumulating) {
        mCompositorModifiers.clear();
        mFeedbackAccumulating = true;
    }
    struct Entry {
        uint32_t format;
        uint32_t pad;
        uint64_t modifier;
    };
    static_assert(sizeof(Entry) == 16);
    const auto* entries = (const Entry*)mFormatTable;
    const size_t total = mFormatTableSize / sizeof(Entry);
    for (size_t i = 0; i < count; ++i) {
        if (indices[i] >= total) {
            continue;
        }
        const Entry& e = entries[indices[i]];
        auto& mods = mCompositorModifiers[e.format];
        if (std::find(mods.begin(), mods.end(), e.modifier) == mods.end()) {
            mods.push_back(e.modifier);
        }
    }
}

void WaylandApp::onFeedbackDone()
{
    mFeedbackAccumulating = false;
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
        mPointerSurface = nullptr;
        for (auto& surf : mSurfaces) {
            surf->pointerOver = false;
        }
    }
}

WaylandApp::OutputSurface* WaylandApp::surfaceFor(wl_surface* surface)
{
    for (auto& surf : mSurfaces) {
        if (surf->surface == surface) {
            return surf.get();
        }
    }
    return nullptr;
}

void WaylandApp::onPointerEnter(wl_surface* surface, double x, double y)
{
    OutputSurface* surf = surfaceFor(surface);
    if (!surf) {
        return;
    }
    mPointerSurface = surface;
    surf->pointerOver = true;
    surf->pointerSeen = true;
    surf->pointerX = x;
    surf->pointerY = y;
    surf->wantRedraw = true;
}

void WaylandApp::onPointerLeave(wl_surface* surface)
{
    OutputSurface* surf = surfaceFor(surface);
    if (surf) {
        surf->pointerOver = false;
        surf->wantRedraw = true;
    }
    if (mPointerSurface == surface) {
        mPointerSurface = nullptr;
    }
}

void WaylandApp::onPointerMotion(double x, double y)
{
    OutputSurface* surf = mPointerSurface ? surfaceFor(mPointerSurface) : nullptr;
    if (surf && surf->pointerOver) {
        surf->pointerX = x;
        surf->pointerY = y;
        surf->wantRedraw = true;
    }
}

void WaylandApp::onXdgSurfaceConfigure(OutputSurface* surf, uint32_t serial)
{
    xdg_surface_ack_configure(surf->xdgSurface, serial);
    surf->configured = true;
    surf->wantRedraw = true;
}

void WaylandApp::onToplevelConfigure(OutputSurface* surf, int32_t width,
                                     int32_t height)
{
    if (width > 0 && height > 0) {
        surf->pendingWidth = (uint32_t)width;
        surf->pendingHeight = (uint32_t)height;
        surf->wantRedraw = true;
    }
}

void WaylandApp::onLayerConfigure(OutputSurface* surf, uint32_t serial,
                                  uint32_t width, uint32_t height)
{
    zwlr_layer_surface_v1_ack_configure(surf->layerSurface, serial);
    if (width > 0 && height > 0) {
        surf->pendingWidth = width;
        surf->pendingHeight = height;
    }
    surf->configured = true;
    surf->wantRedraw = true;
}

void WaylandApp::onSurfaceClosed(OutputSurface* surf)
{
    if (mMode == SurfaceMode::Windowed) {
        mRunning = false;
        return;
    }
    destroyOutputSurface(surf);
    // Keep running with zero surfaces: outputs may come back.
}

void WaylandApp::onPreferredScale(OutputSurface* surf, uint32_t scale120)
{
    const double scale = scale120 / 120.0;
    if (scale > 0.0 && scale != surf->scale) {
        surf->scale = scale;
        surf->wantRedraw = true; // drawFrame reallocates the ring
    }
}

void WaylandApp::onFrameDone(OutputSurface* surf)
{
    surf->framePending = false;
    drawFrame(*surf);
}

void WaylandApp::onBufferRelease(wl_buffer* buffer)
{
    for (auto& surf : mSurfaces) {
        for (auto& buf : surf->buffers) {
            if (buf.buffer == buffer) {
                buf.busy = false;
                if (surf->needRedraw) {
                    surf->needRedraw = false;
                    drawFrame(*surf);
                }
                return;
            }
        }
    }
}

bool WaylandApp::createOutputSurface(wl_output* output, uint32_t id)
{
    auto surf = std::make_unique<OutputSurface>();
    surf->app = this;
    surf->id = id;
    surf->output = output;
    surf->surface = wl_compositor_create_surface(mCompositor);

    if (mMode == SurfaceMode::Wallpaper) {
        surf->layerSurface = zwlr_layer_shell_v1_get_layer_surface(
            mLayerShell, surf->surface, output,
            ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "wallpaper");
        zwlr_layer_surface_v1_add_listener(surf->layerSurface, &kLayerListener,
                                           surf.get());
        zwlr_layer_surface_v1_set_size(surf->layerSurface, 0, 0);
        zwlr_layer_surface_v1_set_anchor(
            surf->layerSurface,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_exclusive_zone(surf->layerSurface, -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            surf->layerSurface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        printf("drift: output %u added\n", id);
    } else {
        surf->xdgSurface = xdg_wm_base_get_xdg_surface(mWmBase, surf->surface);
        xdg_surface_add_listener(surf->xdgSurface, &kXdgSurfaceListener,
                                 surf.get());
        surf->toplevel = xdg_surface_get_toplevel(surf->xdgSurface);
        xdg_toplevel_add_listener(surf->toplevel, &kToplevelListener, surf.get());
        xdg_toplevel_set_title(surf->toplevel, "drift");
        xdg_toplevel_set_app_id(surf->toplevel, "drift");
        surf->pendingWidth = mInitialWidth;
        surf->pendingHeight = mInitialHeight;
    }

    // Fractional HiDPI: render at physical pixels, present at logical size.
    // Both objects are optional; without them the surface renders at scale 1
    // (the compositor upscales, as before).
    if (mViewporter && mFractionalScaleManager) {
        surf->viewport = wp_viewporter_get_viewport(mViewporter, surf->surface);
        surf->fractionalScale =
            wp_fractional_scale_manager_v1_get_fractional_scale(
                mFractionalScaleManager, surf->surface);
        wp_fractional_scale_v1_add_listener(surf->fractionalScale,
                                            &kFractionalScaleListener,
                                            surf.get());
    }

    wl_surface_commit(surf->surface);
    mSurfaces.push_back(std::move(surf));
    return true;
}

void WaylandApp::destroyOutputSurface(OutputSurface* surf)
{
    destroyRing(*surf);
    if (surf->fractionalScale) wp_fractional_scale_v1_destroy(surf->fractionalScale);
    if (surf->viewport) wp_viewport_destroy(surf->viewport);
    if (surf->layerSurface) zwlr_layer_surface_v1_destroy(surf->layerSurface);
    if (surf->toplevel) xdg_toplevel_destroy(surf->toplevel);
    if (surf->xdgSurface) xdg_surface_destroy(surf->xdgSurface);
    if (surf->surface) wl_surface_destroy(surf->surface);
    if (surf->output) wl_output_destroy(surf->output);
    if (mPointerSurface == surf->surface) {
        mPointerSurface = nullptr;
    }

    const uint32_t id = surf->id;
    for (size_t i = 0; i < mSurfaces.size(); ++i) {
        if (mSurfaces[i].get() == surf) {
            mSurfaces.erase(mSurfaces.begin() + i);
            break;
        }
    }
    if (mMode == SurfaceMode::Wallpaper) {
        printf("drift: output %u removed\n", id);
    }
    if (mOutputRemoved) {
        mOutputRemoved(id);
    }
}

bool WaylandApp::setup(Gpu& gpu, SurfaceMode mode, uint32_t width, uint32_t height)
{
    mGpu = &gpu;
    mMode = mode;
    mInitialWidth = width;
    mInitialHeight = height;

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
    wl_display_roundtrip(mDisplay); // globals (outputs may fail surface creation)
    wl_display_roundtrip(mDisplay); // dmabuf modifier + output events

    if (!mCompositor || !mDmabuf) {
        fprintf(stderr, "drift: compositor lacks wl_compositor or zwp_linux_dmabuf_v1\n");
        return false;
    }

    if (mMode == SurfaceMode::Wallpaper) {
        if (!mLayerShell) {
            fprintf(stderr,
                    "drift: compositor does not support wlr-layer-shell "
                    "(required for wallpaper mode; try --windowed)\n");
            return false;
        }
        for (auto& [output, name] : mPendingOutputs) {
            createOutputSurface(output, name);
        }
        mPendingOutputs.clear();
        if (mSurfaces.empty()) {
            fprintf(stderr, "drift: no wl_output advertised\n");
            return false;
        }
    } else {
        if (!mWmBase) {
            fprintf(stderr, "drift: compositor lacks xdg_wm_base\n");
            return false;
        }
        if (!createOutputSurface(nullptr, 0)) {
            return false;
        }
    }

    // Wait for the initial configure of every surface created so far.
    auto allConfigured = [this] {
        for (const auto& surf : mSurfaces) {
            if (!surf->configured) {
                return false;
            }
        }
        return !mSurfaces.empty();
    };
    while (!allConfigured() && mRunning) {
        if (wl_display_dispatch(mDisplay) == -1) {
            return false;
        }
    }
    if (!mRunning) {
        return false;
    }

    if (!chooseFormat()) {
        fprintf(stderr, "drift: no dmabuf format supported by both compositor and GPU\n");
        return false;
    }
    mSetupDone = true;
    return true;
}

bool WaylandApp::chooseFormat()
{
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
        mRingModifiers = std::move(mods);
        mFourcc = cand.fourcc;
        mFormat = cand.wgpu;
        return true;
    }
    return false;
}

bool WaylandApp::createRing(OutputSurface& surf)
{
    surf.width = surf.pendingWidth;
    surf.height = surf.pendingHeight;
    if (surf.width == 0 || surf.height == 0) {
        return false;
    }
    surf.ringScale = surf.viewport ? surf.scale : 1.0;
    surf.bufferWidth = (uint32_t)std::lround(surf.width * surf.ringScale);
    surf.bufferHeight = (uint32_t)std::lround(surf.height * surf.ringScale);

    std::vector<Buffer> buffers(kBufferCount);
    for (auto& buf : buffers) {
        if (!mGpu->createTarget(surf.bufferWidth, surf.bufferHeight, mFourcc,
                                mRingModifiers, buf.target) ||
            !createWlBuffer(surf, buf)) {
            for (auto& b : buffers) {
                if (b.buffer) wl_buffer_destroy(b.buffer);
                mGpu->destroyTarget(b.target);
            }
            return false;
        }
    }
    surf.buffers = std::move(buffers);

    if (surf.viewport) {
        wp_viewport_set_destination(surf.viewport, (int32_t)surf.width,
                                    (int32_t)surf.height);
    }
    if (surf.ringScale != 1.0) {
        printf("drift: output %u: %ux%u buffer for %ux%u logical (scale %.2f)\n",
               surf.id, surf.bufferWidth, surf.bufferHeight, surf.width,
               surf.height, surf.ringScale);
    }

    // The whole surface is opaque (background wallpaper / dev window).
    wl_region* region = wl_compositor_create_region(mCompositor);
    wl_region_add(region, 0, 0, (int32_t)surf.width, (int32_t)surf.height);
    wl_surface_set_opaque_region(surf.surface, region);
    wl_region_destroy(region);
    return true;
}

bool WaylandApp::createWlBuffer(OutputSurface&, Buffer& buf)
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

void WaylandApp::destroyRing(OutputSurface& surf)
{
    for (auto& buf : surf.buffers) {
        if (buf.buffer) {
            wl_buffer_destroy(buf.buffer);
        }
        if (mGpu) {
            mGpu->destroyTarget(buf.target);
        }
    }
    surf.buffers.clear();
}

double WaylandApp::now() const
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec + ts.tv_nsec * 1e-9) - mStartTime;
}

void WaylandApp::drawFrame(OutputSurface& surf)
{
    if (!mRunning || !surf.configured || !mSetupDone) {
        return;
    }

    // First frame / resize / scale change: (re)allocate the ring.
    if (surf.buffers.empty() || surf.pendingWidth != surf.width ||
        surf.pendingHeight != surf.height ||
        (surf.viewport && surf.scale != surf.ringScale)) {
        destroyRing(surf);
        if (!createRing(surf)) {
            fprintf(stderr, "drift: buffer allocation failed for output %u\n",
                    surf.id);
            return;
        }
    }

    Buffer* buf = nullptr;
    for (auto& b : surf.buffers) {
        if (!b.busy) {
            buf = &b;
            break;
        }
    }
    if (!buf) {
        // All buffers held by the compositor; redraw on next release.
        surf.needRedraw = true;
        return;
    }

    // Scene time advances by capped wall-clock deltas: across a pause
    // (occlusion, lock — no frames drawn) it stays put, so scenes resume
    // where they left off (SCENE_FORMAT.md §9.7).
    const double t = now();
    if (surf.lastDrawTime >= 0.0 && !mScenePaused) {
        surf.sceneTime += std::min(t - surf.lastDrawTime, 0.1);
    }
    surf.lastDrawTime = t;

    FrameRequest request;
    request.outputId = surf.id;
    request.target = buf->target.texture.CreateView();
    request.width = surf.bufferWidth;
    request.height = surf.bufferHeight;
    request.seconds = surf.sceneTime;
    request.mouseX = surf.pointerSeen && surf.width
                         ? (float)(surf.pointerX / surf.width)
                         : 0.5f;
    request.mouseY = surf.pointerSeen && surf.height
                         ? (float)(surf.pointerY / surf.height)
                         : 0.5f;
    request.mouseActive = surf.pointerOver;

    mGpu->beginFrame(buf->target);
    const bool presented = mRenderFrame(request);
    mGpu->endFrame(buf->target, presented);

    if (!presented) {
        // Nothing changed: no commit, the compositor keeps showing the
        // previously attached buffer. Animated scenes are re-ticked by the
        // run loop's timer; everything else waits for events.
        return;
    }

    wl_surface_attach(surf.surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(surf.surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_callback* cb = wl_surface_frame(surf.surface);
    wl_callback_add_listener(cb, &kFrameListener, &surf);
    wl_surface_commit(surf.surface);
    buf->busy = true;
    surf.framePending = true;
}

int WaylandApp::run(RenderFrame renderFrame, bool animated)
{
    mRenderFrame = std::move(renderFrame);
    mAnimated = animated;
    for (auto& surf : mSurfaces) {
        drawFrame(*surf);
    }

    const int fd = wl_display_get_fd(mDisplay);
    while (mRunning) {
        if (wl_display_dispatch_pending(mDisplay) < 0) {
            return 1;
        }
        if (!mRunning) {
            break;
        }
        for (size_t i = 0; i < mSurfaces.size(); ++i) {
            OutputSurface* surf = mSurfaces[i].get();
            if (!surf->framePending && surf->wantRedraw) {
                surf->wantRedraw = false;
                drawFrame(*surf);
            }
        }

        while (wl_display_prepare_read(mDisplay) != 0) {
            if (wl_display_dispatch_pending(mDisplay) < 0) {
                return 1;
            }
        }
        wl_display_flush(mDisplay);

        // Animated scene with at least one surface not mid-commit: tick at
        // ~60Hz so time keeps flowing — the ticks are CPU-side value-graph
        // evaluations only unless something dirties. Otherwise sleep until
        // an event or frame callback.
        bool tick = false;
        if (mAnimated) {
            for (const auto& surf : mSurfaces) {
                if (surf->configured && !surf->framePending) {
                    tick = true;
                    break;
                }
            }
        }
        const int timeout = tick ? 16 : -1;
        pollfd pfds[2] = { { fd, POLLIN, 0 }, { mControlFd, POLLIN, 0 } };
        const nfds_t nfds = mControlFd >= 0 ? 2 : 1;
        const int ready = poll(pfds, nfds, timeout);
        if (ready > 0 && (pfds[0].revents & POLLIN)) {
            wl_display_read_events(mDisplay);
        } else {
            wl_display_cancel_read(mDisplay);
            if (ready < 0 && errno != EINTR) {
                return 1;
            }
            if (ready == 0) {
                for (size_t i = 0; i < mSurfaces.size(); ++i) {
                    OutputSurface* surf = mSurfaces[i].get();
                    if (!surf->framePending) {
                        drawFrame(*surf);
                    }
                }
            }
        }
        if (ready > 0 && nfds == 2 && (pfds[1].revents & (POLLIN | POLLHUP)) &&
            mControlCb) {
            mControlCb();
        }
    }
    return 0;
}

} // namespace drift::platform
