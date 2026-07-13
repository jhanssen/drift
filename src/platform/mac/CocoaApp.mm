#include "CocoaApp.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>

#include <unistd.h>

#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CADisplayLink.h>
#import <QuartzCore/CATransaction.h>

// Forward declarations so the C++ state can hold the Objective-C helpers.
@class DriftView;
@class DriftWindowDelegate;
@class DriftLinkTarget;

namespace drift::platform {

namespace {

uint32_t displayIdFor(NSScreen* screen)
{
    return [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
}

std::string displayNameFor(NSScreen* screen)
{
    return std::string(screen.localizedName.UTF8String);
}

} // namespace

// One presented surface: the dev window, or one desktop-level window per
// display in wallpaper mode. Each has its own buffer swapchain, display
// link, pointer state, and scene clock — mirroring WaylandApp's
// OutputSurface. Everything runs on the main thread.
struct CocoaApp::Output {
    Impl* impl = nullptr;
    uint32_t id = 0;  // CGDirectDisplayID; 0 = windowed
    std::string name; // NSScreen localizedName; "" = windowed

    // The custom classes are only forward-declared at this point, so the
    // members carry their AppKit base types; creation/teardown cast.
    NSWindow* window = nil;
    NSView* view = nil;
    ::id windowDelegate = nil; // ::id — the member above shadows the type
    ::id linkTarget = nil;
    CADisplayLink* link = nil;
    // CAMetalLayer on the swapchain path; a plain CALayer whose contents
    // flip between IOSurfaces on the bypass path (DRIFT_PRESENT=iosurface).
    CALayer* layer = nil;
    NSTimer* wakeTimer = nil;

    wgpu::Surface surface; // swapchain path only
    uint32_t configuredWidth = 0, configuredHeight = 0;

    // Swapchain bypass: a ring of self-allocated IOSurfaces imported into
    // Dawn as render targets, presented by assigning layer.contents once
    // the GPU is done — the CAMetalLayer drawable machinery (nextDrawable
    // pacing, per-frame drawable churn) never runs. The display system
    // marks a surface in use while it reads it, which is the ring's
    // release signal.
    struct Buffer {
        IOSurfaceRef ioSurface = nullptr;
        wgpu::SharedTextureMemory memory;
        wgpu::Texture texture;
        bool everInitialized = false;
        bool pendingPresent = false; // GPU still writing; flip queued
    };
    std::vector<Buffer> ring;
    uint64_t ringGeneration = 0; // invalidates in-flight present callbacks

    bool visible = true;
    bool wantRedraw = false;

    bool pointerOver = false;
    bool pointerSeen = false;
    double pointerX = 0.0, pointerY = 0.0; // view points, top-left origin

    double sceneTime = 0.0;
    double lastDrawTime = -1.0;
    // §4.4 module timer: wall-clock instant the earliest wake_after_ms
    // deadline is due, anchored once when the idle wake timer arms (so
    // interleaved wakeups shrink the remaining wait instead of restarting
    // it) and cleared by every draw. Negative = not armed. Only armed
    // sleeps may stretch the scene clock past the §9.7 resume cap.
    double wakeDueWall = -1.0;
};

struct CocoaApp::Impl {
    Gpu* gpu = nullptr;
    SurfaceMode mode = SurfaceMode::Windowed;
    uint32_t initialWidth = 0, initialHeight = 0;
    std::vector<std::string> outputFilter;

    std::vector<std::unique_ptr<Output>> outputs;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;

    RenderFrame renderFrame;
    OutputRemoved outputRemoved;
    WakeQuery wakeQuery;
    AnimatedQuery animatedQuery;
    int controlFd = -1;
    std::function<void()> controlCb;
    int wakeFd = -1;
    dispatch_source_t controlSource = nullptr;
    dispatch_source_t wakeSource = nullptr;
    id mouseMonitor = nil;   // wallpaper: global pointer events
    id screenObserver = nil; // wallpaper: display hotplug

    bool running = true;
    bool started = false; // run() reached; display links may tick
    bool scenePaused = false;
    bool bypassSwapchain = false; // DRIFT_PRESENT=iosurface
    double maxFrameRate = 0.0;    // 0 = display rate
    double startTime = 0.0;

    double now() const
    {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (ts.tv_sec + ts.tv_nsec * 1e-9) - startTime;
    }

    bool matchesFilter(const std::string& name) const
    {
        return outputFilter.empty() ||
               std::find(outputFilter.begin(), outputFilter.end(), name) !=
                   outputFilter.end();
    }

    // Re-asked as instances come and go; unresolved (not yet drawn)
    // outputs count as animated until their first frame settles it.
    bool animated(const Output& surf) const
    {
        return !animatedQuery || animatedQuery(surf.id);
    }

    void kick(Output& surf)
    {
        if (started && surf.link && surf.visible) {
            surf.link.paused = NO;
        }
    }

    void requestRedraw(Output& surf)
    {
        surf.wantRedraw = true;
        kick(surf);
    }

    void requestRedrawAll()
    {
        for (auto& surf : outputs) {
            requestRedraw(*surf);
        }
    }

    void stopRun()
    {
        if (!running) {
            return;
        }
        running = false;
        [NSApp stop:nil];
        // stop: takes effect after the next event; make one arrive.
        NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                           location:NSZeroPoint
                                      modifierFlags:0
                                          timestamp:0
                                       windowNumber:0
                                            context:nil
                                            subtype:0
                                              data1:0
                                              data2:0];
        [NSApp postEvent:wake atStart:YES];
    }

    void onOcclusionChanged(Output& surf)
    {
        const bool nowVisible =
            ([surf.window occlusionState] & NSWindowOcclusionStateVisible) != 0;
        if (nowVisible == surf.visible) {
            return;
        }
        surf.visible = nowVisible;
        if (surf.visible) {
            // Frames resume; the capped clock delta absorbs the gap (§9.7).
            if (animated(surf) || surf.wantRedraw) {
                kick(surf);
            }
            armWakeTimer(surf);
        } else {
            // Draws stop, freezing this surface's scene clock; wantRedraw
            // stays pending until frames resume.
            if (surf.link) {
                surf.link.paused = YES;
            }
            disarmWakeTimer(surf);
        }
    }

    // Wallpaper windows are mouse-transparent, so the pointer arrives via
    // a global monitor: route the location to the display under it.
    void onGlobalMouse()
    {
        const NSPoint p = [NSEvent mouseLocation]; // global, bottom-left
        for (auto& surfPtr : outputs) {
            Output& surf = *surfPtr;
            const NSRect frame = surf.window.frame;
            const bool over = NSMouseInRect(p, frame, NO);
            if (over) {
                surf.pointerOver = true;
                surf.pointerSeen = true;
                surf.pointerX = p.x - frame.origin.x;
                surf.pointerY = frame.size.height - (p.y - frame.origin.y);
                requestRedraw(surf);
            } else if (surf.pointerOver) {
                surf.pointerOver = false;
                requestRedraw(surf);
            }
        }
    }

    bool configureSurface(Output& surf, uint32_t width, uint32_t height)
    {
        wgpu::SurfaceConfiguration config{};
        config.device = gpu->device();
        config.format = format;
        config.usage = wgpu::TextureUsage::RenderAttachment;
        config.width = width;
        config.height = height;
        config.presentMode = wgpu::PresentMode::Fifo;
        config.alphaMode = wgpu::CompositeAlphaMode::Opaque;
        surf.surface.Configure(&config);
        surf.configuredWidth = width;
        surf.configuredHeight = height;
        return true;
    }

    void destroyRing(Output& surf)
    {
        for (auto& buf : surf.ring) {
            buf.texture = nullptr;
            buf.memory = nullptr;
            if (buf.ioSurface) {
                CFRelease(buf.ioSurface); // display holds its own reference
            }
        }
        surf.ring.clear();
        ++surf.ringGeneration;
    }

    bool ensureRing(Output& surf, uint32_t width, uint32_t height)
    {
        if (!surf.ring.empty() && surf.configuredWidth == width &&
            surf.configuredHeight == height) {
            return true;
        }
        destroyRing(surf);
        for (int i = 0; i < 3; ++i) {
            Output::Buffer buf;
            NSDictionary* props = @{
                (id)kIOSurfaceWidth : @(width),
                (id)kIOSurfaceHeight : @(height),
                (id)kIOSurfaceBytesPerElement : @4,
                (id)kIOSurfacePixelFormat : @((uint32_t)'BGRA'),
            };
            buf.ioSurface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
            if (!buf.ioSurface) {
                destroyRing(surf);
                return false;
            }
            wgpu::SharedTextureMemoryIOSurfaceDescriptor io{};
            io.ioSurface = buf.ioSurface;
            wgpu::SharedTextureMemoryDescriptor desc{};
            desc.nextInChain = &io;
            buf.memory = gpu->device().ImportSharedTextureMemory(&desc);
            wgpu::SharedTextureMemoryProperties memProps{};
            buf.memory.GetProperties(&memProps);
            if (memProps.size.width == 0) {
                CFRelease(buf.ioSurface);
                destroyRing(surf);
                return false;
            }
            buf.texture = buf.memory.CreateTexture();
            surf.ring.push_back(std::move(buf));
        }
        surf.configuredWidth = width;
        surf.configuredHeight = height;
        return true;
    }

    // GPU completion for a bypass frame: flip the layer to the finished
    // IOSurface. Runs on the main queue; generation and index re-validate
    // against hotplug/resize teardown that may have raced the callback.
    void presentBuffer(uint32_t outputId, uint64_t generation, size_t index)
    {
        if (!running) {
            return;
        }
        for (auto& surfPtr : outputs) {
            Output& surf = *surfPtr;
            if (surf.id != outputId) {
                continue;
            }
            if (surf.ringGeneration != generation ||
                index >= surf.ring.size()) {
                return;
            }
            Output::Buffer& buf = surf.ring[index];
            buf.pendingPresent = false;
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            surf.layer.contents = (__bridge id)buf.ioSurface;
            [CATransaction commit];
            return;
        }
    }

    void drawFrame(Output& surf)
    {
        if (!running || !renderFrame || !surf.visible) {
            return;
        }
        const NSSize bounds = surf.view.bounds.size;
        const double scale = surf.window.backingScaleFactor;
        const auto pixelWidth = (uint32_t)llround(bounds.width * scale);
        const auto pixelHeight = (uint32_t)llround(bounds.height * scale);
        if (pixelWidth == 0 || pixelHeight == 0) {
            return;
        }
        if (pixelWidth != surf.configuredWidth ||
            pixelHeight != surf.configuredHeight) {
            surf.layer.contentsScale = scale;
            if (bypassSwapchain) {
                if (!ensureRing(surf, pixelWidth, pixelHeight)) {
                    fprintf(stderr,
                            "drift: buffer allocation failed for output %u\n",
                            surf.id);
                    return;
                }
            } else {
                configureSurface(surf, pixelWidth, pixelHeight);
            }
        }

        // Bypass: pick a ring buffer neither in flight on our queue nor
        // still being read by the display; acquired before the clock
        // advances so a fully busy ring doesn't consume scene time.
        Output::Buffer* buffer = nullptr;
        if (bypassSwapchain) {
            for (auto& buf : surf.ring) {
                if (!buf.pendingPresent && !IOSurfaceIsInUse(buf.ioSurface)) {
                    buffer = &buf;
                    break;
                }
            }
            if (!buffer) {
                surf.wantRedraw = true; // retry on the next tick
                return;
            }
        }

        // Scene time advances by capped wall-clock deltas: across a pause
        // (occlusion — no frames drawn) it stays put, so scenes resume
        // where they left off (SCENE_FORMAT.md §9.7). Exception: while an
        // idle sleep is armed toward a §4.4 wake_after_ms deadline
        // (wakeDueWall), the slept wall time is elapsed scene time and the
        // clock may stride up to the deadline.
        const double t = now();
        if (surf.lastDrawTime >= 0.0 && !scenePaused) {
            double cap = 0.1;
            if (surf.wakeDueWall >= 0.0 && wakeQuery) {
                const double deadline = wakeQuery(surf.id);
                if (deadline > surf.sceneTime) {
                    cap = std::max(cap, deadline - surf.sceneTime + 0.017);
                }
            }
            surf.sceneTime += std::min(t - surf.lastDrawTime, cap);
        }
        surf.lastDrawTime = t;
        surf.wakeDueWall = -1.0; // deadlines re-derive after every draw

        FrameRequest request;
        request.outputId = surf.id;
        request.outputName = surf.name;
        request.width = pixelWidth;
        request.height = pixelHeight;
        request.seconds = surf.sceneTime;
        request.mouseX = surf.pointerSeen && bounds.width > 0
                             ? (float)(surf.pointerX / bounds.width)
                             : 0.5f;
        request.mouseY = surf.pointerSeen && bounds.height > 0
                             ? (float)(surf.pointerY / bounds.height)
                             : 0.5f;
        request.mouseActive = surf.pointerOver;

        if (bypassSwapchain) {
            wgpu::SharedTextureMemoryBeginAccessDescriptor ba{};
            ba.initialized = buffer->everInitialized;
            ba.concurrentRead = false;
            if (buffer->memory.BeginAccess(buffer->texture, &ba) !=
                wgpu::Status::Success) {
                return;
            }
            request.target = buffer->texture.CreateView();
            const bool presented = renderFrame(request);
            wgpu::SharedTextureMemoryEndAccessState end{};
            buffer->memory.EndAccess(buffer->texture, &end);
            if (!presented) {
                return; // nothing changed: no flip (§11)
            }
            buffer->everInitialized = true;
            buffer->pendingPresent = true;
            // Flip the layer only once the GPU has finished this frame's
            // work — the display reads the IOSurface with no implicit
            // sync against our queue. The completion fires on Dawn's
            // thread; the flip belongs to the main one.
            Impl* impl = this;
            const uint32_t outputId = surf.id;
            const uint64_t generation = surf.ringGeneration;
            const size_t index = (size_t)(buffer - surf.ring.data());
            gpu->device().GetQueue().OnSubmittedWorkDone(
                wgpu::CallbackMode::AllowSpontaneous,
                [impl, outputId, generation, index](wgpu::QueueWorkDoneStatus,
                                                    wgpu::StringView) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        impl->presentBuffer(outputId, generation, index);
                    });
                });
            return;
        }

        wgpu::SurfaceTexture surfaceTexture{};
        surf.surface.GetCurrentTexture(&surfaceTexture);
        if (surfaceTexture.status !=
                wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
            surfaceTexture.status !=
                wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
            // e.g. Outdated after a resize race: reconfigure and retry once.
            configureSurface(surf, pixelWidth, pixelHeight);
            surf.surface.GetCurrentTexture(&surfaceTexture);
            if (surfaceTexture.status !=
                    wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
                surfaceTexture.status !=
                    wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
                fprintf(stderr, "drift: cannot acquire surface texture (%d)\n",
                        (int)surfaceTexture.status);
                return;
            }
        }

        request.target = surfaceTexture.texture.CreateView();
        if (renderFrame(request)) {
            surf.surface.Present();
        }
        // Nothing rendered: no present, the previous frame stays visible
        // and the acquired drawable is dropped (§11).
    }

    // One display-link tick: draw if there is a reason to, then quiesce
    // (pause this surface's link, arm its module-timer sleep) when the
    // surface's scene does not animate.
    void onTick(Output& surf)
    {
        if (!running) {
            return;
        }
        const bool draw = animated(surf) || surf.wantRedraw;
        surf.wantRedraw = false;
        if (draw) {
            drawFrame(surf);
        }
        // Re-asked after the draw: the first frame may have just
        // instantiated the scene and settled whether it animates.
        if (!animated(surf) && !surf.wantRedraw) {
            surf.link.paused = YES;
            armWakeTimer(surf);
        }
    }

    void disarmWakeTimer(Output& surf)
    {
        if (surf.wakeTimer) {
            [surf.wakeTimer invalidate];
            surf.wakeTimer = nil;
        }
    }

    // §4.4 module timers: a quiescent scene with a wake_after_ms deadline
    // sleeps exactly until it is due (drawFrame then lets the clock stride
    // to the deadline), instead of forever. The deadline lives in scene
    // time, which is frozen while idle, so it is anchored in wall clock
    // once per arming — later re-arms keep the anchor instead of
    // restarting the wait. A paused clock can never reach a scene-time
    // deadline, so paused scenes do not arm. Animated surfaces tick anyway.
    void armWakeTimer(Output& surf)
    {
        disarmWakeTimer(surf);
        if (animated(surf) || scenePaused || !surf.visible || !wakeQuery ||
            !running) {
            surf.wakeDueWall = -1.0;
            return;
        }
        const double deadline = wakeQuery(surf.id);
        if (deadline < 0.0) {
            surf.wakeDueWall = -1.0;
            return;
        }
        const double tnow = now();
        if (surf.wakeDueWall < 0.0) {
            surf.wakeDueWall = tnow + std::max(0.0, deadline - surf.sceneTime);
        }
        const double wait = std::max(0.0, surf.wakeDueWall - tnow) + 0.001;
        Impl* impl = this;
        Output* surfPtr = &surf;
        surf.wakeTimer = [NSTimer
            scheduledTimerWithTimeInterval:wait
                                   repeats:NO
                                     block:^(NSTimer*) {
                                         surfPtr->wakeTimer = nil;
                                         // Draw with wakeDueWall still set:
                                         // the clock strides to the
                                         // deadline (§9.7 exception).
                                         impl->requestRedraw(*surfPtr);
                                     }];
    }

    bool createOutput(NSScreen* screen); // nil = the dev window
    void destroyOutput(Output* surf);
    void syncScreens();
};

} // namespace drift::platform

using Impl = drift::platform::CocoaApp::Impl;
using Output = drift::platform::CocoaApp::Output;

// The content view: owns the CAMetalLayer; in windowed mode it also tracks
// the pointer (top-left origin via isFlipped) and closes on Esc, mirroring
// the Wayland toplevel. Wallpaper windows never receive events.
@interface DriftView : NSView
@property(nonatomic, assign) Output* surf;
@end

@implementation DriftView {
    NSTrackingArea* _tracking;
}

- (BOOL)isFlipped
{
    return YES;
}

- (BOOL)acceptsFirstResponder
{
    return YES;
}

- (BOOL)wantsUpdateLayer
{
    return YES;
}

- (CALayer*)makeBackingLayer
{
    // Bypass path presents by flipping contents on a plain layer; the
    // swapchain path needs the Metal layer's drawable machinery.
    if (self.surf && self.surf->impl->bypassSwapchain) {
        return [CALayer layer];
    }
    return [CAMetalLayer layer];
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (_tracking) {
        [self removeTrackingArea:_tracking];
    }
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
                     NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:_tracking];
}

- (void)setPointer:(NSEvent*)event over:(BOOL)over
{
    if (!self.surf) {
        return;
    }
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    self.surf->pointerOver = over;
    if (over) {
        self.surf->pointerSeen = true;
        self.surf->pointerX = p.x;
        self.surf->pointerY = p.y;
    }
    self.surf->impl->requestRedraw(*self.surf);
}

- (void)mouseEntered:(NSEvent*)event
{
    [self setPointer:event over:YES];
}

- (void)mouseExited:(NSEvent*)event
{
    [self setPointer:event over:NO];
}

- (void)mouseMoved:(NSEvent*)event
{
    [self setPointer:event over:YES];
}

- (void)mouseDragged:(NSEvent*)event
{
    [self setPointer:event over:YES];
}

- (void)keyDown:(NSEvent*)event
{
    if (event.keyCode == 53 /* Esc */) {
        if (self.surf) {
            self.surf->impl->stopRun();
        }
        return;
    }
    [super keyDown:event];
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    if (self.surf) {
        // drawFrame reconfigures for the scale.
        self.surf->impl->requestRedraw(*self.surf);
    }
}

- (void)setFrameSize:(NSSize)newSize
{
    [super setFrameSize:newSize];
    if (self.surf) {
        self.surf->impl->requestRedraw(*self.surf);
    }
}

@end

@interface DriftWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) Output* surf;
@end

@implementation DriftWindowDelegate

- (void)windowWillClose:(NSNotification*)notification
{
    if (!self.surf) {
        return;
    }
    Impl* impl = self.surf->impl;
    if (impl->mode != drift::platform::SurfaceMode::Wallpaper) {
        impl->stopRun();
        return;
    }
    // Keep running with the other displays; this one may come back.
    impl->destroyOutput(self.surf);
}

- (void)windowDidChangeOcclusionState:(NSNotification*)notification
{
    if (self.surf) {
        self.surf->impl->onOcclusionChanged(*self.surf);
    }
}

@end

// CADisplayLink needs an Objective-C target/selector pair.
@interface DriftLinkTarget : NSObject
@property(nonatomic, assign) Output* surf;
- (void)onTick:(CADisplayLink*)link;
@end

@implementation DriftLinkTarget

- (void)onTick:(CADisplayLink*)link
{
    if (self.surf) {
        self.surf->impl->onTick(*self.surf);
    }
}

@end

namespace drift::platform {

bool CocoaApp::Impl::createOutput(NSScreen* screen)
{
    auto surf = std::make_unique<Output>();
    surf->impl = this;

    NSRect rect;
    if (screen) {
        surf->id = displayIdFor(screen);
        surf->name = displayNameFor(screen);
        rect = screen.frame; // global coordinates
        surf->window = [[NSWindow alloc]
            initWithContentRect:rect
                      styleMask:NSWindowStyleMaskBorderless
                        backing:NSBackingStoreBuffered
                          defer:NO
                         screen:screen];
        // Desktop level sits behind the icons; the window covers its
        // screen on every Space, never moves in Mission Control, and
        // passes the mouse through to the Finder desktop.
        surf->window.level =
            (NSWindowLevel)CGWindowLevelForKey(kCGDesktopWindowLevelKey);
        surf->window.collectionBehavior =
            NSWindowCollectionBehaviorCanJoinAllSpaces |
            NSWindowCollectionBehaviorStationary |
            NSWindowCollectionBehaviorIgnoresCycle;
        surf->window.ignoresMouseEvents = YES;
        surf->window.hasShadow = NO;
        [surf->window setFrame:rect display:NO];
    } else {
        rect = NSMakeRect(0, 0, initialWidth, initialHeight);
        surf->window = [[NSWindow alloc]
            initWithContentRect:rect
                      styleMask:NSWindowStyleMaskTitled |
                                NSWindowStyleMaskClosable |
                                NSWindowStyleMaskMiniaturizable |
                                NSWindowStyleMaskResizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        surf->window.title = @"drift";
    }
    surf->window.releasedWhenClosed = NO;
    surf->window.backgroundColor = [NSColor blackColor];

    DriftView* view = [[DriftView alloc] initWithFrame:NSMakeRect(
                                             0, 0, rect.size.width,
                                             rect.size.height)];
    view.surf = surf.get();
    view.wantsLayer = YES;
    surf->view = view;
    surf->layer = view.layer;
    surf->layer.contentsScale = surf->window.backingScaleFactor;
    surf->window.contentView = view;

    DriftWindowDelegate* delegate = [DriftWindowDelegate new];
    delegate.surf = surf.get();
    surf->windowDelegate = delegate;
    surf->window.delegate = delegate;

    if (bypassSwapchain) {
        // The ring's IOSurfaces are BGRA; drawFrame allocates it lazily.
        format = wgpu::TextureFormat::BGRA8Unorm;
    } else {
        wgpu::SurfaceSourceMetalLayer layerSource{};
        layerSource.layer = (__bridge void*)surf->layer;
        wgpu::SurfaceDescriptor surfaceDesc{};
        surfaceDesc.nextInChain = &layerSource;
        surf->surface = gpu->instance().CreateSurface(&surfaceDesc);
        if (!surf->surface) {
            fprintf(stderr, "drift: cannot create surface for CAMetalLayer\n");
            surf->window.delegate = nil;
            view.surf = nullptr;
            delegate.surf = nullptr;
            [surf->window close];
            return false;
        }
        if (format == wgpu::TextureFormat::Undefined) {
            wgpu::SurfaceCapabilities caps{};
            if (surf->surface.GetCapabilities(gpu->adapter(), &caps) !=
                    wgpu::Status::Success ||
                caps.formatCount == 0) {
                fprintf(stderr, "drift: no supported surface format\n");
                return false;
            }
            format = caps.formats[0];
        }
    }

    DriftLinkTarget* linkTarget = [DriftLinkTarget new];
    linkTarget.surf = surf.get();
    surf->linkTarget = linkTarget;
    surf->link = [view displayLinkWithTarget:linkTarget
                                    selector:@selector(onTick:)];
    // Common modes keep frames coming during live window resize.
    [surf->link addToRunLoop:[NSRunLoop mainRunLoop]
                     forMode:NSRunLoopCommonModes];
    if (maxFrameRate > 0.0) {
        // A maximum: the link ticks at the nearest achievable rate (a
        // divisor of a fixed display's refresh).
        surf->link.preferredFrameRateRange = CAFrameRateRangeMake(
            1.0f, (float)maxFrameRate, (float)maxFrameRate);
    }
    surf->link.paused = YES; // kick() unpauses once run() has started

    if (screen) {
        [surf->window orderFront:nil];
        printf("drift: output %u (%s) added\n", surf->id, surf->name.c_str());
    } else {
        [surf->window makeFirstResponder:view];
        [surf->window center];
        [surf->window makeKeyAndOrderFront:nil];
        [NSApp activate];
    }
    surf->visible =
        ([surf->window occlusionState] & NSWindowOcclusionStateVisible) != 0;

    Output& ref = *surf;
    outputs.push_back(std::move(surf));
    if (started) {
        requestRedraw(ref); // hotplug: first frame instantiates the scene
    }
    return true;
}

void CocoaApp::Impl::destroyOutput(Output* surf)
{
    disarmWakeTimer(*surf);
    destroyRing(*surf);
    if (surf->link) {
        [surf->link invalidate];
        surf->link = nil;
    }
    ((DriftLinkTarget*)surf->linkTarget).surf = nullptr;
    ((DriftView*)surf->view).surf = nullptr;
    ((DriftWindowDelegate*)surf->windowDelegate).surf = nullptr;
    surf->window.delegate = nil;
    [surf->window close];
    surf->window = nil;

    const uint32_t id = surf->id;
    const std::string name = surf->name;
    for (size_t i = 0; i < outputs.size(); ++i) {
        if (outputs[i].get() == surf) {
            outputs.erase(outputs.begin() + i);
            break;
        }
    }
    if (mode == SurfaceMode::Wallpaper) {
        printf("drift: output %u (%s) removed\n", id, name.c_str());
    }
    if (outputRemoved) {
        outputRemoved(id);
    }
}

// Display hotplug (and resolution changes): diff the current screens
// against the live windows, one wallpaper window per matching display.
void CocoaApp::Impl::syncScreens()
{
    std::set<uint32_t> present;
    for (NSScreen* screen in [NSScreen screens]) {
        const uint32_t id = displayIdFor(screen);
        present.insert(id);
        Output* existing = nullptr;
        for (auto& surf : outputs) {
            if (surf->id == id) {
                existing = surf.get();
                break;
            }
        }
        if (existing) {
            if (!NSEqualRects(existing->window.frame, screen.frame)) {
                [existing->window setFrame:screen.frame display:NO];
                requestRedraw(*existing);
            }
            continue;
        }
        if (!matchesFilter(displayNameFor(screen))) {
            continue;
        }
        createOutput(screen);
    }
    for (size_t i = outputs.size(); i-- > 0;) {
        if (!present.contains(outputs[i]->id)) {
            destroyOutput(outputs[i].get());
        }
    }
}

CocoaApp::CocoaApp() : mImpl(std::make_unique<Impl>()) {}

CocoaApp::~CocoaApp()
{
    Impl& impl = *mImpl;
    impl.running = false;
    if (impl.screenObserver) {
        [[NSNotificationCenter defaultCenter] removeObserver:impl.screenObserver];
        impl.screenObserver = nil;
    }
    if (impl.mouseMonitor) {
        [NSEvent removeMonitor:impl.mouseMonitor];
        impl.mouseMonitor = nil;
    }
    if (impl.controlSource) {
        dispatch_source_cancel(impl.controlSource);
        impl.controlSource = nullptr;
    }
    if (impl.wakeSource) {
        dispatch_source_cancel(impl.wakeSource);
        impl.wakeSource = nullptr;
    }
    while (!impl.outputs.empty()) {
        impl.destroyOutput(impl.outputs.back().get());
    }
}

bool CocoaApp::setup(Gpu& gpu, SurfaceMode mode, uint32_t width,
                     uint32_t height)
{
    Impl& impl = *mImpl;
    impl.gpu = &gpu;
    impl.mode = mode;
    impl.initialWidth = width;
    impl.initialHeight = height;

    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    impl.startTime = ts.tv_sec + ts.tv_nsec * 1e-9;

    if (mode == SurfaceMode::Fullscreen) {
        fprintf(stderr, "drift: --fullscreen is not implemented on macOS\n");
        return false;
    }

    // DRIFT_PRESENT=iosurface: render into a self-allocated IOSurface ring
    // and flip layer contents, skipping the CAMetalLayer drawable
    // machinery; =drawable (default) presents through the Dawn surface.
    if (const char* present = getenv("DRIFT_PRESENT"); present && *present) {
        if (!strcmp(present, "iosurface")) {
            impl.bypassSwapchain = true;
        } else if (strcmp(present, "drawable") != 0) {
            fprintf(stderr, "drift: unknown DRIFT_PRESENT '%s' (drawable, "
                            "iosurface)\n", present);
            return false;
        }
        printf("drift: presentation: %s\n",
               impl.bypassSwapchain ? "iosurface ring" : "drawable");
    }

    [NSApplication sharedApplication];
    if (mode == SurfaceMode::Wallpaper) {
        // No Dock icon, no menu bar, never steals focus.
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        if ([NSScreen screens].count == 0 && impl.outputFilter.empty()) {
            fprintf(stderr, "drift: no display attached\n");
            return false;
        }
        Impl* implPtr = &impl;
        impl.screenObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSApplicationDidChangeScreenParametersNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification*) {
                        if (implPtr->running) {
                            implPtr->syncScreens();
                        }
                    }];
        // Pointer position for mouse-transparent windows. Global monitors
        // receive mouse events without extra permissions (unlike keyboard).
        impl.mouseMonitor = [NSEvent
            addGlobalMonitorForEventsMatchingMask:NSEventMaskMouseMoved |
                                                  NSEventMaskLeftMouseDragged |
                                                  NSEventMaskRightMouseDragged |
                                                  NSEventMaskOtherMouseDragged
                                          handler:^(NSEvent*) {
                                              if (implPtr->running) {
                                                  implPtr->onGlobalMouse();
                                              }
                                          }];
        impl.syncScreens();
        // Zero matches with a filter: keep waiting for hotplug (the named
        // display may attach later). The format is still needed for the
        // caller's pipelines before any window exists; probe a detached
        // layer once.
        if (impl.bypassSwapchain) {
            impl.format = wgpu::TextureFormat::BGRA8Unorm;
        }
        if (impl.format == wgpu::TextureFormat::Undefined) {
            wgpu::SurfaceSourceMetalLayer layerSource{};
            CAMetalLayer* probe = [CAMetalLayer layer];
            layerSource.layer = (__bridge void*)probe;
            wgpu::SurfaceDescriptor surfaceDesc{};
            surfaceDesc.nextInChain = &layerSource;
            wgpu::Surface surface = gpu.instance().CreateSurface(&surfaceDesc);
            wgpu::SurfaceCapabilities caps{};
            if (!surface ||
                surface.GetCapabilities(gpu.adapter(), &caps) !=
                    wgpu::Status::Success ||
                caps.formatCount == 0) {
                fprintf(stderr, "drift: no supported surface format\n");
                return false;
            }
            impl.format = caps.formats[0];
        }
        if (impl.outputs.empty() && !impl.outputFilter.empty()) {
            fprintf(stderr,
                    "drift: no display matched --output yet; waiting for "
                    "hotplug\n");
        }
        return true;
    }

    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    return impl.createOutput(nil);
}

void CocoaApp::setOutputFilter(std::vector<std::string> names)
{
    mImpl->outputFilter = std::move(names);
}

std::vector<std::string> CocoaApp::claimedOutputs() const
{
    std::vector<std::string> names;
    for (const auto& surf : mImpl->outputs) {
        names.push_back(surf->name);
    }
    return names;
}

wgpu::TextureFormat CocoaApp::targetFormat() const
{
    return mImpl->format;
}

void CocoaApp::setOutputRemoved(OutputRemoved cb)
{
    mImpl->outputRemoved = std::move(cb);
}

void CocoaApp::setControl(int fd, std::function<void()> cb)
{
    mImpl->controlFd = fd;
    mImpl->controlCb = std::move(cb);
}

void CocoaApp::setWakeFd(int fd)
{
    mImpl->wakeFd = fd;
}

void CocoaApp::setWakeQuery(WakeQuery query)
{
    mImpl->wakeQuery = std::move(query);
}

void CocoaApp::setAnimatedQuery(AnimatedQuery query)
{
    mImpl->animatedQuery = std::move(query);
}

void CocoaApp::setMaxFrameRate(double fps)
{
    mImpl->maxFrameRate = fps;
}

void CocoaApp::requestRedrawAll()
{
    mImpl->requestRedrawAll();
}

// The shared clock (§17.6) is the furthest surface along, matching the
// Wayland layer.
double CocoaApp::currentSceneTime() const
{
    double t = 0.0;
    for (const auto& surf : mImpl->outputs) {
        t = std::max(t, surf->sceneTime);
    }
    return t;
}

void CocoaApp::setScenePaused(bool paused)
{
    Impl& impl = *mImpl;
    impl.scenePaused = paused;
    for (auto& surf : impl.outputs) {
        if (paused) {
            // A paused clock can never reach a scene-time deadline (§4.4).
            impl.disarmWakeTimer(*surf);
            surf->wakeDueWall = -1.0;
        } else {
            impl.armWakeTimer(*surf);
        }
    }
}

bool CocoaApp::scenePaused() const
{
    return mImpl->scenePaused;
}

void CocoaApp::seekSceneTime(double seconds)
{
    for (auto& surf : mImpl->outputs) {
        surf->sceneTime = seconds;
        mImpl->requestRedraw(*surf);
    }
}

int CocoaApp::run(RenderFrame renderFrame)
{
    Impl& impl = *mImpl;
    impl.renderFrame = std::move(renderFrame);

    if (impl.controlFd >= 0 && impl.controlCb) {
        impl.controlSource = dispatch_source_create(
            DISPATCH_SOURCE_TYPE_READ, (uintptr_t)impl.controlFd, 0,
            dispatch_get_main_queue());
        Impl* implPtr = &impl;
        dispatch_source_set_event_handler(impl.controlSource, ^{
            if (implPtr->running && implPtr->controlCb) {
                implPtr->controlCb();
            }
        });
        dispatch_resume(impl.controlSource);
    }
    if (impl.wakeFd >= 0) {
        impl.wakeSource = dispatch_source_create(
            DISPATCH_SOURCE_TYPE_READ, (uintptr_t)impl.wakeFd, 0,
            dispatch_get_main_queue());
        Impl* implPtr = &impl;
        const int fd = impl.wakeFd;
        dispatch_source_set_event_handler(impl.wakeSource, ^{
            char buf[64];
            while (read(fd, buf, sizeof(buf)) > 0) {
            }
            if (implPtr->running) {
                // A module delivery is pending: redraw; the graph evaluates
                // only the woken nodes (§11).
                implPtr->requestRedrawAll();
            }
        });
        dispatch_resume(impl.wakeSource);
    }

    impl.started = true;
    impl.requestRedrawAll();

    [NSApp run];

    impl.running = false;
    for (auto& surf : impl.outputs) {
        impl.disarmWakeTimer(*surf);
        if (surf->link) {
            [surf->link invalidate];
            surf->link = nil;
        }
    }
    return 0;
}

} // namespace drift::platform
