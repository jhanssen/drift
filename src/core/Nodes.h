#pragma once

// Built-in node types (SCENE_FORMAT.md §9). Instantiated by the loader.

#include <array>
#include <map>
#include <string>

#include "Scene.h"
#include "Video.h"
#include "WgslInterface.h"

namespace drift::core {

// §9.7 time (implicit): outputs seconds, delta.
class TimeNode : public Node {
public:
    TimeNode();
    void evaluate(FrameContext& ctx) override;
    bool alwaysEvaluate() const override { return true; }
    bool drivesFrames() const override { return true; }

private:
    double mLast = 0.0;
};

// §9.8 mouse (implicit): outputs position (last-known, output space),
// active (1 while the pointer is over the scene surface).
class MouseNode : public Node {
public:
    MouseNode();
    void evaluate(FrameContext& ctx) override;
    bool alwaysEvaluate() const override { return true; }
};

// §9.9 wave: inputs input, frequency, phase; property shape.
class WaveNode : public Node {
public:
    enum class Shape { Sine, Triangle, Saw, Square };
    explicit WaveNode(Shape shape);
    void evaluate(FrameContext& ctx) override;

private:
    Shape mShape;
};

// §9.9 remap: polymorphic linear range map; property clamp.
class RemapNode : public Node {
public:
    explicit RemapNode(bool clamp);
    void evaluate(FrameContext& ctx) override;

private:
    bool mClamp;
};

// §9.9 combine: scalars -> vecN (N fixed at load).
class CombineNode : public Node {
public:
    explicit CombineNode(ValueType resultType);
    void evaluate(FrameContext& ctx) override;
};

// §9.9 split: vecN -> component scalars (arity fixed at load).
class SplitNode : public Node {
public:
    explicit SplitNode(int arity);
    void evaluate(FrameContext& ctx) override;
};

// §9.9 arithmetic: add/multiply/mix/clamp, T-polymorphic. Input order:
// add/multiply (a, b), mix (a, b, t), clamp (value, lo, hi).
class ArithmeticNode : public Node {
public:
    enum class Op { Add, Multiply, Mix, Clamp };
    explicit ArithmeticNode(Op op);
    void evaluate(FrameContext& ctx) override;

private:
    Op mOp;
};

// §9.9 noise: band-limited value noise over the input axis — C1-smooth,
// deterministic, [-1, 1]. Input order: input, frequency, seed.
class NoiseNode : public Node {
public:
    NoiseNode();
    void evaluate(FrameContext& ctx) override;
};

// §9.9 damp: framerate-independent smoothed follow (exponential, halflife
// seconds); snaps to the input on the first evaluation. Input order:
// value, time (delta), halflife.
class DampNode : public Node {
public:
    DampNode();
    void evaluate(FrameContext& ctx) override;

private:
    Value mState{};
    bool mPrimed = false;
};

// §9.9 edge: level→event — fires when value crosses threshold in the
// armed direction; the first evaluation arms without firing. Input
// order: value, threshold.
class EdgeNode : public Node {
public:
    enum class Mode { Rise, Fall, Both };
    explicit EdgeNode(Mode mode);
    void evaluate(FrameContext& ctx) override;

private:
    Mode mMode;
    double mPrev = 0.0;
    bool mPrimed = false;
};

// §9.9 sequence: a timeline — maps a scalar input through named keyframe
// tracks, one output port per track in declaration order. Value tracks only
// (event tracks are reserved, §16). Local-time reduction happens in double
// (§17.2) so a looping sequence stays frame-accurate at any uptime.
class SequenceNode : public Node {
public:
    enum class Interpolate { Hold, Linear, Smooth };
    struct Key {
        double t = 0.0;
        Value value;
    };
    struct Track {
        std::string name;
        bool event = false; // §16: cue track; fires instead of keys
        ValueType type = ValueType::Scalar;
        Interpolate interpolate = Interpolate::Linear;
        std::vector<Key> keys;      // value tracks: strictly ascending t
        std::vector<double> fires;  // event tracks: ascending, [0, duration)
    };

    SequenceNode(double duration, bool loop, std::vector<Track> tracks);
    void evaluate(FrameContext& ctx) override;

    const std::vector<Track>& tracks() const { return mTracks; }
    double duration() const { return mDuration; }
    bool loop() const { return mLoop; }

private:
    const double mDuration;
    const bool mLoop;
    const std::vector<Track> mTracks;
    double mLastLocalTime = 0.0; // cue-crossing window start (§9.9)
};

// §9.1 image: static texture source, dirty on load only. Decoding happens in
// the loader (validation without a GPU); PNG/JPEG via stb, WebP via libwebp,
// KTX2 via libktx. Basis-supercompressed KTX2 transcodes to BC7 and uploads
// compressed (with its stored mip chain); since compressed blocks can't be
// premultiplied after the fact, such content must be authored premultiplied
// or opaque — anything else is a load error, never a silent decompress. All
// other sources convert on CPU to premultiplied linear rgba16float. Upload
// happens on first evaluate.
class ImageNode : public Node {
public:
    // bytes: encoded file contents. Returns nullptr with error set on decode
    // failure.
    static ImageNode* decode(const std::string& bytes, std::string& error);

    void evaluate(FrameContext& ctx) override;

private:
    struct Level {
        std::vector<uint8_t> data; // texel or BC7 block bytes, tightly packed
        uint32_t width = 0, height = 0;
    };
    ImageNode(std::vector<Level> levels, wgpu::TextureFormat format);

    std::vector<Level> mLevels; // [0] = base; released after upload
    wgpu::TextureFormat mFormat;
    uint32_t mWidth = 0, mHeight = 0;
    wgpu::Texture mTexture;
};

// §9.2 video: streaming texture source, dirty on each new decoded frame.
// Decoding runs on the platform-provided decoder's thread; evaluate pulls
// the frame due at scene time. CPU frames upload once per new frame;
// zero-copy frames import the decoder surface's dmabuf planes (cached per
// surface — decoders recycle a small pool) and convert YUV to linear RGB in
// a render pass. A failed import tells the decoder to fall back.
class VideoNode : public Node {
public:
    explicit VideoNode(std::unique_ptr<VideoDecoder> decoder);
    void evaluate(FrameContext& ctx) override;
    // Frame advance is driven by scene time, not graph inputs.
    bool alwaysEvaluate() const override { return true; }
    bool drivesFrames() const override { return true; }

private:
    std::unique_ptr<VideoDecoder> mDecoder;
    wgpu::Texture mTexture; // CPU path upload target
    uint32_t mWidth = 0, mHeight = 0;
    int64_t mLastIndex = -1;
    // §9.2 playing: playback clock = scene time minus accumulated paused
    // spans, so an unpaused video tracks scene time exactly.
    double mPausedTotal = 0.0;
    double mLastSeconds = 0.0;
    // §17.4: final frame produced (loop: false); holds until 'restart'.
    bool mFinished = false;

#ifndef __EMSCRIPTEN__
    // The zero-copy path is the one sanctioned exception to the portable-API
    // rule: dmabuf import uses Dawn's SharedTextureMemory extension surface,
    // which the browser's webgpu.h does not declare. Compiled out for the
    // web, whose decoder (WebCodecs, §9.2) won't produce dmabuf planes.
    bool evaluateZeroCopy(FrameContext& ctx, const VideoFrame& frame);
    bool ensureConvertPipeline(FrameContext& ctx);

    struct ImportedSurface {
        wgpu::SharedTextureMemory memory[2];
        wgpu::Texture texture[2]; // Y, UV
    };
    std::map<uint64_t, ImportedSurface> mSurfaces;
    wgpu::RenderPipeline mConvertPipeline;
    wgpu::Buffer mConvertUniforms;
    wgpu::Sampler mConvertSampler;
    wgpu::Texture mConverted; // rgba16float conversion target
    uint32_t mConvertedWidth = 0, mConvertedHeight = 0;
#endif
};

// §9.4 transform: renders source into a scene-output-sized transparent
// canvas with 2D position/rotation/scale/anchor/opacity applied. Sizing is
// resolution-independent: at scale [1,1] the displayed height equals the
// output height, width follows the source aspect.
class TransformNode : public Node {
public:
    // Input order: source, position, rotation, scale, anchor, opacity.
    TransformNode();
    void evaluate(FrameContext& ctx) override;

private:
    bool ensurePipeline(FrameContext& ctx);

    wgpu::RenderPipeline mPipeline;
    wgpu::Buffer mUniforms;
    wgpu::Sampler mSampler;
    wgpu::Texture mColor;
    uint32_t mWidth = 0, mHeight = 0;
};

// §17.1 fit: maps source onto a scene-output-sized canvas — cover (uniform
// scale to fill, centered, overflow cropped), contain (uniform scale to fit
// entirely, centered, uncovered area transparent), or stretch (non-uniform
// fill).
class FitNode : public Node {
public:
    enum class Mode { Cover, Contain, Stretch };
    explicit FitNode(Mode mode);
    void evaluate(FrameContext& ctx) override;

private:
    bool ensurePipeline(FrameContext& ctx);

    Mode mMode;
    wgpu::RenderPipeline mPipeline;
    wgpu::Buffer mUniforms;
    wgpu::Sampler mSampler;
    wgpu::Texture mColor;
    uint32_t mWidth = 0, mHeight = 0;
};

// §9.5 compositor: stacks its inputs (all texture layers, bottom first) into
// a scene-output-sized target. Each layer blends with what is beneath it
// per its declared mode (premultiplied, linear): over (default), add,
// multiply, screen.
class CompositorNode : public Node {
public:
    enum class Blend { Over, Add, Multiply, Screen };
    explicit CompositorNode(std::vector<Blend> blends); // per layer; short
                                                        // list ⇒ over
    void evaluate(FrameContext& ctx) override;

private:
    bool ensurePipeline(FrameContext& ctx);

    std::vector<Blend> mBlends;
    wgpu::RenderPipeline mPipelines[4];
    wgpu::Sampler mSampler;
    wgpu::Texture mColor;
    uint32_t mWidth = 0, mHeight = 0;
};

// §9.3 shader: fullscreen fragment pass into an rgba16float intermediate.
// Ports come from the WGSL interface: value inputs first (uniform fields, in
// declaration order), then texture inputs.
class ShaderNode : public Node {
public:
    ShaderNode(std::string fragmentSource, WgslInterface iface,
               uint32_t explicitWidth, uint32_t explicitHeight);
    void evaluate(FrameContext& ctx) override;

    const WgslInterface& interface() const { return mInterface; }
    size_t textureInputStart() const { return mInterface.fields.size(); }

private:
    bool ensurePipeline(FrameContext& ctx);

    std::string mFragmentSource;
    WgslInterface mInterface;
    uint32_t mExplicitWidth = 0, mExplicitHeight = 0; // 0 = auto

    wgpu::RenderPipeline mPipeline;
    wgpu::Buffer mUniforms;
    wgpu::Sampler mSampler;
    wgpu::Sampler mRepeatSampler; // §9.10 _sampler_repeat bindings
    wgpu::Texture mColor;
    uint32_t mWidth = 0, mHeight = 0;
};

// §18.2 compute: a GPU compute pass. Ports come from the WGSL interface:
// value inputs (uniform fields), sampled textures, then read-only storage
// buffers; outputs are the read_write storage buffers it owns (allocated
// zeroed at `capacity` elements), then storage textures. Buffer outputs are
// dirty whenever the node executes (§18.1 — no change detection on GPU
// contents); a previous-edge self-connection is the ping-pong simulation
// idiom, realized by Scene::render's history copies like textures.
class ComputeNode : public Node {
public:
    ComputeNode(std::string source, WgslInterface iface,
                std::vector<uint32_t> capacities, // per read_write buffer
                std::array<uint32_t, 3> dispatch); // {0,0,0} = auto
    void evaluate(FrameContext& ctx) override;

    const WgslInterface& interface() const { return mInterface; }
    size_t textureInputStart() const { return mInterface.fields.size(); }
    size_t bufferInputStart() const
    {
        return textureInputStart() + mInterface.textures.size();
    }

private:
    bool ensurePipeline(FrameContext& ctx);

    std::string mSource;
    WgslInterface mInterface;
    std::vector<uint32_t> mCapacities;
    std::array<uint32_t, 3> mDispatch;

    wgpu::ComputePipeline mPipeline;
    wgpu::Buffer mUniforms;
    wgpu::Sampler mSampler;
    wgpu::Sampler mRepeatSampler; // §9.10 _sampler_repeat bindings
    std::vector<wgpu::Buffer> mOwned;        // read_write storage buffers
    std::vector<wgpu::Texture> mStorageTex;  // storage texture outputs
    uint32_t mTexWidth = 0, mTexHeight = 0;
};

// §18.3 particles: GPU emission + simulation over one pool. The CPU's whole
// contribution per frame is the uniform block: dt, the deterministic
// emission window (rate accumulator + burst counts mapped onto a ring
// cursor), and the current values of the wireable ports. Per-particle
// randomness derives from a stored emission ordinal, so emission-time
// quantities (size, spin) are recomputable in later frames without extra
// state. Internally double-buffered; the §18.3 Particle contract is the
// output.
class ParticlesNode : public Node {
public:
    enum class Emitter { Point, Box, Disc };
    // §18.5.4 spawnMode: which parent event the owned child range emits on.
    enum class SpawnMode { Death, Birth, Life };
    // Input port order — the loader's port table must match.
    enum Port : size_t {
        PortRate, PortBurst, PortBurstCount, PortOrigin, PortExtent,
        PortEmitScale, PortDirection, PortSpread, PortSpeed, PortLifetime,
        PortSize, PortSpin, PortColorStart, PortColorEnd, PortFadeIn,
        PortFadeOut, PortSizeEnd, PortSizeWindow, PortGravity, PortDrag,
        PortTurbulence, PortTurbulenceScale, PortTurbulenceMask,
        PortAttractor, PortAttract, PortVortex, PortDelay, PortDuration,
        PortRing, PortDepth, PortCollide, PortBounce, PortTintVary,
        PortTintVaryMax, PortVelocityMin, PortVelocityMax, PortTwinkle,
        PortTwinkleRate, PortPrewarm, PortSpawn, PortInherit, PortSpawnRate,
        PortSpawnPrev, PortTime, PortCount,
    };
    // §18.5.3 per-emitter override fields. Multi-emitter nodes carry
    // EfCount extra input ports per entry after PortCount, in this order —
    // the loader's kEmitterFields table must match. An entry field that was
    // not set falls back to the base port at evaluation time (mMasks).
    enum EmitterField : size_t {
        EfOrigin, EfExtent, EfDirection, EfSpread, EfSpeed, EfLifetime,
        EfSize, EfSpin, EfColorStart, EfColorEnd, EfDepth, EfDelay,
        EfDuration, EfWeight, EfCount,
    };
    static constexpr uint32_t kStride = 64;      // the Particle contract
    static constexpr uint32_t kMaxEmitters = 8;  // §18.5.3
    static constexpr uint32_t kEmitterStride = 112; // EmitterP, WGSL layout

    // One entry per emitter: its shape and the bitmask (1 << EmitterField)
    // of fields the document set on it. Single-emitter nodes pass one entry
    // with mask 0. spawnCount > 0 selects sub-emitter mode (§18.5.4): the
    // loader derives capacity as spawn.capacity × spawnCount and wires the
    // hidden spawnPrev feedback edge for death detection.
    ParticlesNode(uint32_t capacity, std::vector<Emitter> emitters,
                  std::vector<uint32_t> overrideMasks,
                  uint32_t spawnCount = 0,
                  SpawnMode spawnMode = SpawnMode::Death);
    void evaluate(FrameContext& ctx) override;

    // §18.5.2: binding velocityMin/velocityMax selects the axis-
    // independent spawn-velocity box over the direction/spread/speed cone.
    void setVelocityBox(bool on) { mVelocityBox = on; }
    // §18.5.2: binding tintVaryMax switches tintVary to independent
    // per-channel draws in the [tintVary, tintVaryMax] box.
    void setTintBox(bool on) { mTintBox = on; }

private:
    bool ensurePipeline(FrameContext& ctx);
    Value emitterValue(size_t e, EmitterField f) const;

    const uint32_t mCapacity;
    const std::vector<Emitter> mEmitters;
    const std::vector<uint32_t> mMasks;
    const uint32_t mSpawnCount; // §18.5.4: children per parent; 0 = ring mode
    const SpawnMode mSpawnMode = SpawnMode::Death; // §18.5.4 trigger
    bool mVelocityBox = false;  // §18.5.2 spawn-velocity box vs cone
    bool mTintBox = false;      // §18.5.2 per-channel tint box
    bool mPrewarmed = false;    // §18.5.2 prewarm ran (first evaluate)
    double mDtOverride = -1.0;  // ≥ 0 during prewarm sub-steps
    WgslInterface mIface; // built-in kernel reflection: the layout oracle

    wgpu::ComputePipeline mPipeline;
    wgpu::Buffer mUniforms;
    wgpu::Buffer mEmitterUniforms; // EmitterP[kMaxEmitters] storage block
    wgpu::Sampler mSampler;  // collide mask sampling (§18.5.2)
    bool mCollide = false;   // pipeline permutation: collide port bound
    wgpu::Buffer mState[2]; // ping-pong pool
    int mFront = 0;
    // Fractional emissions carried between frames, per emitter (§18.5.3:
    // the rate splits across emitters by weight, deterministically).
    std::array<double, kMaxEmitters> mAccum{};
    double mEmitted = 0.0;  // total emissions: the RNG ordinal base
    uint32_t mCursor = 0;   // next emission slot (ring)
    double mSimTime = 0.0;  // accumulated sim ticks: the §18.5.2 window clock
};

// §18.3/§18.5.5 sprites: instanced draw of a Particle buffer into an
// output-sized premultiplied layer. A prepare pass compacts live slots into
// an index list and sizes an indirect draw; 'over' additionally sorts the
// list back-to-front (z descending, then oldest-first) with a bitonic
// network, so order is deterministic. The fragment is either a built-in
// soft disc or the optional sprite texture, with §18.5.5 spritesheet frame
// animation and per-particle parallax applied in the vertex stage.
class SpritesNode : public Node {
public:
    enum class Blend { Add, Over };
    enum Port : size_t {
        PortParticles, PortTexture, PortFrameRate, PortParallax,
        PortFlutter, PortFlutterRate, PortStretch, PortFrameBlend,
        PortAlign, PortHardness,
    };
    // sheetCols/Rows 0 = whole texture (no sheet).
    SpritesNode(Blend blend, uint32_t sheetCols, uint32_t sheetRows);
    void evaluate(FrameContext& ctx) override;

private:
    bool ensurePipeline(FrameContext& ctx);
    void ensurePool(FrameContext& ctx, uint32_t capacity);

    const Blend mBlend;
    const uint32_t mSheetCols, mSheetRows;
    wgpu::RenderPipeline mPipeline;
    wgpu::ComputePipeline mPrepare;
    wgpu::ComputePipeline mSort;
    bool mTextured = false;
    wgpu::Buffer mUniforms;
    wgpu::Sampler mSampler;
    wgpu::Texture mColor;
    uint32_t mWidth = 0, mHeight = 0;
    // §18.5.5 draw list: alive slot indices (padded to a power of two for
    // the sort network; padding is a sentinel that sinks) + indirect args.
    wgpu::Buffer mIndices;
    wgpu::Buffer mIndirect;
    uint32_t mPadded = 0;
    std::vector<wgpu::Buffer> mSortUniforms; // one (j, k) pair per stage
    // Sort bind groups reference the producer's ping-pong buffer, so cache
    // one set per source buffer object.
    std::map<WGPUBuffer, std::vector<wgpu::BindGroup>> mSortGroups;
};

// §18.5.6 trails: Catmull-Rom ribbons through per-particle position
// history, over the public Particle contract. A record pass appends one
// ring sample per particle per sim tick (a fresh emission resets its whole
// ring, so recycled slots never streak); the render pass extrudes a
// tessellated strip per live particle, tapering width and alpha toward the
// tail and optionally feathering alpha across the width.
class TrailsNode : public Node {
public:
    enum class Blend { Add, Over };
    enum Port : size_t {
        PortParticles, PortWidth, PortTaper, PortFade, PortParallax,
        PortFeather,
    };
    static constexpr uint32_t kSubdiv = 4; // tessellation per history segment

    TrailsNode(Blend blend, uint32_t length);
    void evaluate(FrameContext& ctx) override;

private:
    bool ensurePipeline(FrameContext& ctx);

    const Blend mBlend;
    const uint32_t mLength; // history samples per particle (§18.5.6: 2-64)
    wgpu::ComputePipeline mRecord;
    wgpu::RenderPipeline mPipeline;
    wgpu::Buffer mUniforms;
    wgpu::Buffer mHistory; // capacity × length × vec4(pos.xy, size, alive)
    uint32_t mHistCapacity = 0;
    wgpu::Texture mColor;
    uint32_t mWidth = 0, mHeight = 0;
    uint32_t mTick = 0; // ring head = mTick % mLength
};

// §9.6 output: blits the final linear texture to the platform target with
// sRGB encoding.
class OutputNode : public Node {
public:
    OutputNode();
    void evaluate(FrameContext& ctx) override;

private:
    wgpu::RenderPipeline mPipeline;
    wgpu::TextureFormat mPipelineFormat = wgpu::TextureFormat::Undefined;
    wgpu::Sampler mSampler;
};

} // namespace drift::core
