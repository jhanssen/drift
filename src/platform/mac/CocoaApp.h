#pragma once

// Cocoa presentation: a dev window backed by a CAMetalLayer that Dawn
// presents through a wgpu::Surface. Windowed mode only so far — wallpaper
// (per-screen desktop-level windows) and fullscreen are future work, and
// setup() refuses those modes.
//
// Mirrors WaylandApp's contract so main.cpp drives either through one
// alias: the same FrameRequest/RenderFrame shapes, the same scene-clock
// rules (capped resume deltas per SCENE_FORMAT.md §9.7; only an armed
// module-timer sleep may stride past the cap; occlusion freezes the
// clock), and the same control/wake fd integration.

#include <cstdint>
#include <functional>
#include <memory>

#include "Gpu.h"

namespace drift::platform {

enum class SurfaceMode {
    Wallpaper,  // per-output background surfaces (not yet implemented here)
    Windowed,   // single dev window
    Fullscreen, // single fullscreen window (not yet implemented here)
};

class CocoaApp {
public:
    CocoaApp();
    ~CocoaApp();

    // Creates the window, its CAMetalLayer and the wgpu::Surface, and picks
    // the surface format. width/height are the initial content size in
    // points; rendering happens at physical pixels (backing scale).
    bool setup(Gpu& gpu, SurfaceMode mode, uint32_t width, uint32_t height);

    wgpu::TextureFormat targetFormat() const;

    // One render callback invocation per frame. outputId is 0 (single
    // window). seconds is the scene clock: capped per-frame deltas, frozen
    // across pauses (occlusion, miniaturization) per SCENE_FORMAT.md §9.7.
    // Returns whether the target was written; when false nothing is
    // presented (§11) and the previous frame stays on screen.
    struct FrameRequest {
        uint32_t outputId = 0;
        wgpu::TextureView target;
        uint32_t width = 0, height = 0;
        double seconds = 0.0;
        float mouseX = 0.5f, mouseY = 0.5f;
        bool mouseActive = false;
    };
    using RenderFrame = std::function<bool(const FrameRequest&)>;

    // Never fires in windowed mode; kept for interface parity (wallpaper
    // mode will use it for display hotplug).
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

    // Frame loop until the window closes (or Esc). animated: frames are
    // produced continuously at the display rate; non-animated scenes render
    // only on input/resize/wake events.
    int run(RenderFrame renderFrame, bool animated);

    struct Impl; // all Cocoa state lives in the .mm

private:
    std::unique_ptr<Impl> mImpl;
};

} // namespace drift::platform
