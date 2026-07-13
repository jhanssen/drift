#include "CocoaApp.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

#include <unistd.h>

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CADisplayLink.h>

// Forward declarations so Impl can hold the Objective-C helpers.
@class DriftView;
@class DriftWindowDelegate;
@class DriftLinkTarget;

namespace drift::platform {

// All Cocoa state and the frame/clock logic. Single window (outputId 0);
// everything runs on the main thread — AppKit events, the display link,
// and the dispatch sources all land on the main run loop.
struct CocoaApp::Impl {
    Gpu* gpu = nullptr;
    SurfaceMode mode = SurfaceMode::Windowed;
    uint32_t initialWidth = 0, initialHeight = 0;

    // The custom classes are only forward-declared at this point, so the
    // members carry their AppKit base types; setup()/teardown cast.
    NSWindow* window = nil;
    NSView* view = nil;
    id windowDelegate = nil;
    id linkTarget = nil;
    CADisplayLink* link = nil;
    CAMetalLayer* layer = nil;

    wgpu::Surface surface;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
    uint32_t configuredWidth = 0, configuredHeight = 0;

    RenderFrame renderFrame;
    OutputRemoved outputRemoved;
    WakeQuery wakeQuery;
    AnimatedQuery animatedQuery;
    int controlFd = -1;
    std::function<void()> controlCb;
    int wakeFd = -1;
    dispatch_source_t controlSource = nullptr;
    dispatch_source_t wakeSource = nullptr;
    NSTimer* wakeTimer = nil;

    bool running = true;
    bool scenePaused = false;
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
    double startTime = 0.0;

    double now() const
    {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (ts.tv_sec + ts.tv_nsec * 1e-9) - startTime;
    }

    // Re-asked as instances come and go (outputId is always 0 here).
    bool animated() const
    {
        return !animatedQuery || animatedQuery(0);
    }

    void kick()
    {
        if (link && visible) {
            link.paused = NO;
        }
    }

    void requestRedraw()
    {
        wantRedraw = true;
        kick();
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

    void onOcclusionChanged()
    {
        const bool nowVisible =
            ([window occlusionState] & NSWindowOcclusionStateVisible) != 0;
        if (nowVisible == visible) {
            return;
        }
        visible = nowVisible;
        if (visible) {
            // Frames resume; the capped clock delta absorbs the gap (§9.7).
            if (animated() || wantRedraw) {
                kick();
            }
            armWakeTimer();
        } else {
            // Draws stop, freezing the scene clock; wantRedraw stays
            // pending until frames resume.
            if (link) {
                link.paused = YES;
            }
            disarmWakeTimer();
        }
    }

    bool configureSurface(uint32_t width, uint32_t height)
    {
        wgpu::SurfaceConfiguration config{};
        config.device = gpu->device();
        config.format = format;
        config.usage = wgpu::TextureUsage::RenderAttachment;
        config.width = width;
        config.height = height;
        config.presentMode = wgpu::PresentMode::Fifo;
        config.alphaMode = wgpu::CompositeAlphaMode::Opaque;
        surface.Configure(&config);
        configuredWidth = width;
        configuredHeight = height;
        return true;
    }

    void drawFrame()
    {
        if (!running || !renderFrame || !visible) {
            return;
        }
        const NSSize bounds = view.bounds.size;
        const double scale = window.backingScaleFactor;
        const auto pixelWidth = (uint32_t)llround(bounds.width * scale);
        const auto pixelHeight = (uint32_t)llround(bounds.height * scale);
        if (pixelWidth == 0 || pixelHeight == 0) {
            return;
        }
        if (pixelWidth != configuredWidth || pixelHeight != configuredHeight) {
            layer.contentsScale = scale;
            configureSurface(pixelWidth, pixelHeight);
        }

        // Scene time advances by capped wall-clock deltas: across a pause
        // (occlusion — no frames drawn) it stays put, so scenes resume
        // where they left off (SCENE_FORMAT.md §9.7). Exception: while an
        // idle sleep is armed toward a §4.4 wake_after_ms deadline
        // (wakeDueWall), the slept wall time is elapsed scene time and the
        // clock may stride up to the deadline.
        const double t = now();
        if (lastDrawTime >= 0.0 && !scenePaused) {
            double cap = 0.1;
            if (wakeDueWall >= 0.0 && wakeQuery) {
                const double deadline = wakeQuery(0);
                if (deadline > sceneTime) {
                    cap = std::max(cap, deadline - sceneTime + 0.017);
                }
            }
            sceneTime += std::min(t - lastDrawTime, cap);
        }
        lastDrawTime = t;
        wakeDueWall = -1.0; // deadlines re-derive after every draw

        wgpu::SurfaceTexture surfaceTexture{};
        surface.GetCurrentTexture(&surfaceTexture);
        if (surfaceTexture.status !=
                wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
            surfaceTexture.status !=
                wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
            // e.g. Outdated after a resize race: reconfigure and retry once.
            configureSurface(pixelWidth, pixelHeight);
            surface.GetCurrentTexture(&surfaceTexture);
            if (surfaceTexture.status !=
                    wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
                surfaceTexture.status !=
                    wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
                fprintf(stderr, "drift: cannot acquire surface texture (%d)\n",
                        (int)surfaceTexture.status);
                return;
            }
        }

        FrameRequest request;
        request.outputId = 0;
        request.target = surfaceTexture.texture.CreateView();
        request.width = pixelWidth;
        request.height = pixelHeight;
        request.seconds = sceneTime;
        request.mouseX = pointerSeen && bounds.width > 0
                             ? (float)(pointerX / bounds.width)
                             : 0.5f;
        request.mouseY = pointerSeen && bounds.height > 0
                             ? (float)(pointerY / bounds.height)
                             : 0.5f;
        request.mouseActive = pointerOver;

        if (renderFrame(request)) {
            surface.Present();
        }
        // Nothing rendered: no present, the previous frame stays visible
        // and the acquired drawable is dropped (§11).
    }

    // One display-link tick: draw if there is a reason to, then quiesce
    // (pause the link, arm the module-timer sleep) when nothing animates.
    void onTick()
    {
        if (!running) {
            return;
        }
        const bool draw = animated() || wantRedraw;
        wantRedraw = false;
        if (draw) {
            drawFrame();
        }
        // Re-asked after the draw: the first frame may have just
        // instantiated the scene and settled whether it animates.
        if (!animated() && !wantRedraw) {
            link.paused = YES;
            armWakeTimer();
        }
    }

    void disarmWakeTimer()
    {
        if (wakeTimer) {
            [wakeTimer invalidate];
            wakeTimer = nil;
        }
    }

    // §4.4 module timers: a quiescent scene with a wake_after_ms deadline
    // sleeps exactly until it is due (drawFrame then lets the clock stride
    // to the deadline), instead of forever. The deadline lives in scene
    // time, which is frozen while idle, so it is anchored in wall clock
    // once per arming — later re-arms keep the anchor instead of
    // restarting the wait. A paused clock can never reach a scene-time
    // deadline, so paused scenes do not arm. Animated scenes tick anyway.
    void armWakeTimer()
    {
        disarmWakeTimer();
        if (animated() || scenePaused || !visible || !wakeQuery || !running) {
            wakeDueWall = -1.0;
            return;
        }
        const double deadline = wakeQuery(0);
        if (deadline < 0.0) {
            wakeDueWall = -1.0;
            return;
        }
        const double tnow = now();
        if (wakeDueWall < 0.0) {
            wakeDueWall = tnow + std::max(0.0, deadline - sceneTime);
        }
        const double wait = std::max(0.0, wakeDueWall - tnow) + 0.001;
        Impl* impl = this;
        wakeTimer = [NSTimer
            scheduledTimerWithTimeInterval:wait
                                   repeats:NO
                                     block:^(NSTimer*) {
                                         impl->wakeTimer = nil;
                                         // Draw with wakeDueWall still set:
                                         // the clock strides to the
                                         // deadline (§9.7 exception).
                                         impl->requestRedraw();
                                     }];
    }
};

} // namespace drift::platform

using Impl = drift::platform::CocoaApp::Impl;

// The content view: owns the CAMetalLayer, tracks the pointer (top-left
// origin via isFlipped) and closes on Esc, mirroring the Wayland toplevel.
@interface DriftView : NSView
@property(nonatomic, assign) Impl* impl;
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
    if (!self.impl) {
        return;
    }
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    self.impl->pointerOver = over;
    if (over) {
        self.impl->pointerSeen = true;
        self.impl->pointerX = p.x;
        self.impl->pointerY = p.y;
    }
    self.impl->requestRedraw();
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
        if (self.impl) {
            self.impl->stopRun();
        }
        return;
    }
    [super keyDown:event];
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    if (self.impl) {
        self.impl->requestRedraw(); // drawFrame reconfigures for the scale
    }
}

- (void)setFrameSize:(NSSize)newSize
{
    [super setFrameSize:newSize];
    if (self.impl) {
        self.impl->requestRedraw();
    }
}

@end

@interface DriftWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) Impl* impl;
@end

@implementation DriftWindowDelegate

- (void)windowWillClose:(NSNotification*)notification
{
    if (self.impl) {
        self.impl->stopRun();
    }
}

- (void)windowDidChangeOcclusionState:(NSNotification*)notification
{
    if (self.impl) {
        self.impl->onOcclusionChanged();
    }
}

@end

// CADisplayLink needs an Objective-C target/selector pair.
@interface DriftLinkTarget : NSObject
@property(nonatomic, assign) Impl* impl;
- (void)onTick:(CADisplayLink*)link;
@end

@implementation DriftLinkTarget

- (void)onTick:(CADisplayLink*)link
{
    if (self.impl) {
        self.impl->onTick();
    }
}

@end

namespace drift::platform {

CocoaApp::CocoaApp() : mImpl(std::make_unique<Impl>()) {}

CocoaApp::~CocoaApp()
{
    Impl& impl = *mImpl;
    impl.disarmWakeTimer();
    if (impl.link) {
        [impl.link invalidate];
        impl.link = nil;
    }
    if (impl.controlSource) {
        dispatch_source_cancel(impl.controlSource);
        impl.controlSource = nullptr;
    }
    if (impl.wakeSource) {
        dispatch_source_cancel(impl.wakeSource);
        impl.wakeSource = nullptr;
    }
    if (impl.view) {
        ((DriftView*)impl.view).impl = nullptr;
    }
    if (impl.windowDelegate) {
        ((DriftWindowDelegate*)impl.windowDelegate).impl = nullptr;
    }
    if (impl.linkTarget) {
        ((DriftLinkTarget*)impl.linkTarget).impl = nullptr;
    }
    if (impl.window) {
        impl.window.delegate = nil;
        [impl.window close];
        impl.window = nil;
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

    if (mode != SurfaceMode::Windowed) {
        fprintf(stderr, "drift: only --windowed is implemented on macOS\n");
        return false;
    }

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    const NSRect rect = NSMakeRect(0, 0, width, height);
    impl.window = [[NSWindow alloc]
        initWithContentRect:rect
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    impl.window.title = @"drift";
    impl.window.releasedWhenClosed = NO;

    DriftView* view = [[DriftView alloc] initWithFrame:rect];
    view.impl = &impl;
    view.wantsLayer = YES;
    impl.view = view;
    impl.layer = (CAMetalLayer*)view.layer;
    impl.layer.contentsScale = impl.window.backingScaleFactor;
    impl.window.contentView = view;
    [impl.window makeFirstResponder:view];

    DriftWindowDelegate* delegate = [DriftWindowDelegate new];
    delegate.impl = &impl;
    impl.windowDelegate = delegate;
    impl.window.delegate = delegate;

    wgpu::SurfaceSourceMetalLayer layerSource{};
    layerSource.layer = (__bridge void*)impl.layer;
    wgpu::SurfaceDescriptor surfaceDesc{};
    surfaceDesc.nextInChain = &layerSource;
    impl.surface = gpu.instance().CreateSurface(&surfaceDesc);
    if (!impl.surface) {
        fprintf(stderr, "drift: cannot create surface for CAMetalLayer\n");
        return false;
    }

    wgpu::SurfaceCapabilities caps{};
    if (impl.surface.GetCapabilities(gpu.adapter(), &caps) !=
            wgpu::Status::Success ||
        caps.formatCount == 0) {
        fprintf(stderr, "drift: no supported surface format\n");
        return false;
    }
    impl.format = caps.formats[0];

    [impl.window center];
    [impl.window makeKeyAndOrderFront:nil];
    [NSApp activate];
    impl.visible =
        ([impl.window occlusionState] & NSWindowOcclusionStateVisible) != 0;
    return true;
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

void CocoaApp::requestRedrawAll()
{
    mImpl->requestRedraw();
}

double CocoaApp::currentSceneTime() const
{
    return mImpl->sceneTime;
}

void CocoaApp::setScenePaused(bool paused)
{
    Impl& impl = *mImpl;
    impl.scenePaused = paused;
    if (paused) {
        // A paused clock can never reach a scene-time deadline (§4.4).
        impl.disarmWakeTimer();
        impl.wakeDueWall = -1.0;
    } else {
        impl.armWakeTimer();
    }
}

bool CocoaApp::scenePaused() const
{
    return mImpl->scenePaused;
}

void CocoaApp::seekSceneTime(double seconds)
{
    mImpl->sceneTime = seconds;
    mImpl->requestRedraw();
}

int CocoaApp::run(RenderFrame renderFrame)
{
    Impl& impl = *mImpl;
    impl.renderFrame = std::move(renderFrame);

    DriftLinkTarget* linkTarget = [DriftLinkTarget new];
    linkTarget.impl = &impl;
    impl.linkTarget = linkTarget;
    impl.link = [impl.view displayLinkWithTarget:linkTarget
                                        selector:@selector(onTick:)];
    // Common modes keep frames coming during live window resize.
    [impl.link addToRunLoop:[NSRunLoop mainRunLoop]
                    forMode:NSRunLoopCommonModes];

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
                implPtr->requestRedraw();
            }
        });
        dispatch_resume(impl.wakeSource);
    }

    impl.wantRedraw = true;
    impl.kick();

    [NSApp run];

    impl.running = false;
    impl.disarmWakeTimer();
    if (impl.link) {
        [impl.link invalidate];
        impl.link = nil;
    }
    return 0;
}

} // namespace drift::platform
