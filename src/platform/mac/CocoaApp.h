#pragma once

// Cocoa presentation: CAMetalLayer-backed windows that Dawn presents
// through wgpu::Surfaces. Wallpaper mode creates one borderless window per
// NSScreen at desktop level (behind the icons; all Spaces; mouse-
// transparent, so the pointer arrives via a global monitor) with runtime
// display hotplug; windowed dev mode creates a single titled window.
// Fullscreen is future work and setup() refuses it.
//
// Mirrors WaylandApp's contract so main.cpp drives either through one
// alias: the same FrameRequest/RenderFrame shapes, the same per-surface
// scene-clock rules (capped resume deltas per SCENE_FORMAT.md §9.7; only
// an armed module-timer sleep may stride past the cap; occlusion freezes
// the clock), and the same control/wake fd integration.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Gpu.h"

namespace drift::platform {

enum class SurfaceMode {
    Wallpaper,  // desktop-level window per display, with hotplug
    Windowed,   // single dev window
    Fullscreen, // single fullscreen window (not yet implemented here)
};

class CocoaApp {
public:
    CocoaApp();
    ~CocoaApp();

    // Creates the window(s), their CAMetalLayers and wgpu::Surfaces, and
    // picks the surface format. width/height are the initial content size
    // in points for Windowed mode and ignored in Wallpaper mode (windows
    // cover their screens); rendering happens at physical pixels (backing
    // scale).
    bool setup(Gpu& gpu, SurfaceMode mode, uint32_t width, uint32_t height);

    // Wallpaper mode: only claim displays whose name (NSScreen
    // localizedName, e.g. "Built-in Retina Display") is listed; empty =
    // every display. Set before setup(). Names requested but not present
    // attach if the display hotplugs later (with a filter, setup()
    // succeeds even when no display matches yet and waits for hotplug).
    void setOutputFilter(std::vector<std::string> names);

    // Names of the displays claimed so far; the single unnamed ("")
    // output in windowed mode. After setup() the app uses this to drop
    // validation scene instances for named outputs that are absent.
    std::vector<std::string> claimedOutputs() const;

    wgpu::TextureFormat targetFormat() const;

    // One render callback invocation per surface per frame. outputId is a
    // stable identity for the lifetime of that display's window (the
    // CGDirectDisplayID; 0 in windowed mode) — the app keeps one scene
    // instance per outputId. outputName is the display name (empty in
    // windowed mode). seconds is that surface's scene clock: capped
    // per-frame deltas, frozen across pauses (occlusion, miniaturization)
    // per SCENE_FORMAT.md §9.7. Returns whether the target was written;
    // when false nothing is presented (§11) and the previous frame stays
    // on screen.
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

    // Notifies when a display's window is destroyed (hotplug removal) so
    // the app can drop that output's scene. Never fires in windowed mode.
    using OutputRemoved = std::function<void(uint32_t)>;
    void setOutputRemoved(OutputRemoved cb);

    // External control endpoint (ControlServer): fd is watched alongside
    // the UI event stream and cb runs when it is readable.
    void setControl(int fd, std::function<void()> cb);

    // §4.4 external wakes: fd (owned by the caller) becomes readable when a
    // module delivery needs a frame; the loop drains it and redraws.
    void setWakeFd(int fd);

    // Earliest module timer deadline (wake_after_ms, in scene time) for an
    // output's scene; negative = none. Consulted to arm an idle sleep, and
    // to let the scene clock stride past the §9.7 resume cap up to a due
    // deadline (a deliberate timer sleep is elapsed scene time).
    using WakeQuery = std::function<double(uint32_t outputId)>;
    void setWakeQuery(WakeQuery query);

    // Whether an output's scene animates (advances with time): consulted
    // per display-link tick so a static scene quiesces. Unset = animated.
    using AnimatedQuery = std::function<bool(uint32_t outputId)>;
    void setAnimatedQuery(AnimatedQuery query);

    // Present at most fps frames per second (0 = display rate). A maximum:
    // the display link picks the nearest rate it can honor (a divisor of a
    // fixed display's refresh). Set before setup().
    void setMaxFrameRate(double fps);

    // Requests a redraw (a parameter changed): the frame evaluates the
    // graph, which decides what actually re-executes (§11).
    void requestRedrawAll();

    // The scene clock, for the control endpoint's "time" method.
    double currentSceneTime() const;

    // Transport (control endpoint). Pause freezes the scene clock; the
    // graph quiesces on its own (§11).
    void setScenePaused(bool paused);
    bool scenePaused() const;

    // Jumps the scene clock (forward-only within live instances, §9.7; the
    // caller handles backward by re-creating the scene instances).
    void seekSceneTime(double seconds);

    // Frame loop: until the window closes (or Esc) in windowed mode, until
    // killed in wallpaper mode. Outputs whose scene animates (per the
    // animated query) produce frames continuously at their display's rate;
    // static outputs render only on input/resize/wake events.
    int run(RenderFrame renderFrame);

    struct Impl;   // all Cocoa state lives in the .mm
    struct Output; // one presented surface (window/display)

private:
    std::unique_ptr<Impl> mImpl;
};

} // namespace drift::platform
