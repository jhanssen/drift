#pragma once

// The scene graph: nodes, typed ports, and dirty-driven evaluation
// (SCENE_FORMAT.md §8, §11). Platform-agnostic — sees only the portable
// WebGPU API and caller-provided data.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Module.h"
#include "Value.h"
#include "Video.h"

namespace drift::core {

struct FrameContext {
    wgpu::Device device;
    // Scene time; does not advance while paused. Double so long runs (a
    // wallpaper is up for weeks) keep sub-frame precision — video frame
    // selection consumes this directly. Value ports are f32, so
    // @time.seconds itself quantizes at large magnitudes; that is a
    // documented limit of the f32 value graph.
    double seconds = 0.0;
    // Pointer state in output space (§9.8). x/y hold the last known
    // position; the platform keeps them across leave events.
    float mouseX = 0.5f, mouseY = 0.5f;
    bool mouseActive = false;
    uint64_t frame = 0;
    wgpu::TextureView target;
    uint32_t targetWidth = 0, targetHeight = 0;
    wgpu::TextureFormat targetFormat = wgpu::TextureFormat::Undefined;
    // Out: the output node wrote ctx.target this frame. When false, the
    // platform must not commit — the previously committed buffer already
    // shows the current content (§11: present only when dirty).
    bool presented = false;
};

class Node {
public:
    struct Input {
        Value constant;          // literal or parameter snapshot
        int paramIndex = -1;     // scene parameter backing, -1 if none
        Node* srcNode = nullptr; // connection, null if constant
        int srcPort = -1;
        ValueType type = ValueType::Scalar; // resolved port type
        bool splat = false;      // scalar source feeding a vecN port
        bool previous = false;   // feedback edge: read frame N-1 (§10)
    };
    struct Output {
        std::string name; // port name, filled by the loader
        Value value;
        bool dirty = false;
        // Previous-frame state (§10), maintained by Scene::render. prev and
        // prevDirty snapshot value/dirty at frame start; texture and buffer
        // outputs read through a feedback edge additionally keep their last
        // written contents in a history copy (needsHistory is set by the
        // loader).
        Value prev;
        bool prevDirty = false;
        bool needsHistory = false;
        wgpu::Texture history;
        wgpu::Buffer historyBuffer; // §18.1 buffer feedback
    };

    virtual ~Node() = default;
    virtual void evaluate(FrameContext& ctx) = 0;
    // Input-source nodes (time, mouse) evaluate every frame and decide
    // dirtiness themselves; everything else evaluates only when an input is
    // dirty.
    virtual bool alwaysEvaluate() const { return false; }
    // True for nodes whose output advances with scene time (time, video):
    // scenes containing one need frames even without external events.
    virtual bool drivesFrames() const { return false; }
    // External wake (§4.4): the node asked to run at `now` without any
    // input dirtying — a module's wake_after_ms deadline is due, or its
    // network capability delivered a completion/message/lifecycle event.
    // The platform must schedule a frame when one is pending (the wake
    // callback natively; the rAF loop in the browser).
    virtual bool wakePending(double now) const
    {
        (void)now;
        return false;
    }
    // Earliest future wake_after_ms deadline, or a negative value: what a
    // platform arms its idle timer with.
    virtual double nextWake() const { return -1.0; }

    Value inputValue(size_t index) const;
    bool inputsDirty() const;
    // Event inputs (§16.2): fired this frame (or last frame for a
    // `previous` edge). Always false for unconnected ports.
    bool inputFired(size_t index) const;

    std::string id;
    std::vector<Input> inputs;
    std::vector<Output> outputs;
    bool firstEvaluate = true;
    bool paramsChanged = false; // a bound scene parameter changed (§11)
    // Bumped by Scene::render whenever a texture output was written this
    // frame; editors poll it to refresh node previews only on change.
    uint64_t textureRevision = 0;
};

// A user-tweakable scene parameter (§6): declaration metadata plus the
// current value. Doubles as the settings-UI manifest.
struct SceneParam {
    std::string name;
    ValueType type = ValueType::Scalar;
    Value value; // current (starts at the declared default)
    Value min, max;
    bool hasMin = false, hasMax = false;
    float step = 0.0f;   // 0 = unspecified
    std::string label;   // empty = use the name
    std::string hint;    // e.g. "color"; empty = none
};

// Reads a project-relative file (text or binary) into out. The platform
// enforces project-root confinement.
using AssetReader = std::function<bool(const std::string& path, std::string& out)>;

class Scene {
public:
    // Parses and validates scene.json. Returns nullptr on failure with
    // human-readable messages appended to errors; non-fatal findings (§13:
    // unknown fields/properties/hints, unreachable nodes) go to warnings.
    // videoFactory may be null: scenes with video nodes then fail to load.
    // modules.load likewise (§4.5): null means this runtime cannot run
    // WASM modules, and scenes containing a module node fail to load.
    // Ungranted §4.4 capabilities are warnings, never errors (soft-deny).
    static std::unique_ptr<Scene> load(const std::string& sceneJson,
                                       const AssetReader& readAsset,
                                       const VideoDecoderFactory& videoFactory,
                                       const ModulePlatform& modules,
                                       const wgpu::Device& device,
                                       std::vector<std::string>& errors,
                                       std::vector<std::string>& warnings);

    // Evaluates the graph for one frame. Returns ctx.presented.
    bool render(FrameContext& ctx);

    const std::string& name() const { return mName; }

    // True if the scene needs a steady frame supply (contains time/video
    // nodes). A non-animated scene only needs frames on external events
    // (input, resize, parameter changes).
    bool animated() const { return mAnimated; }

    // Sets a user parameter (§6). Ports bound to it become dirty and their
    // nodes re-execute on the next frame (§11). A scalar splats to vector
    // parameters; values clamp to the declared min/max. Returns false for
    // an unknown name or a type mismatch.
    bool setParameter(const std::string& name, Value value);
    const std::vector<SceneParam>& parameters() const { return mParams; }

    // Sequence timelines (§9.9), read-only — the editor's timeline panel
    // is built from these. Pointers stay valid for the scene's lifetime.
    const std::vector<class SequenceNode*>& sequences() const
    {
        return mSequences;
    }

    // §4.4 scheduling queries for the platform's idle loop: does any
    // node hold an undelivered external wake, and what is the earliest
    // future timer deadline (negative = none)?
    bool wakesPending(double now) const;
    double nextWake() const;

    // Manually fires an event output on the next frame (the editor's
    // "fire now" verb, §16.1). Returns false for an unknown node/port or a
    // non-event port. The injected fire behaves exactly like a produced
    // one: downstream consumers see it dirty for that one frame.
    bool fireEvent(const std::string& nodeId, const std::string& port);

    // Editor preview tap: downscales a node's current texture output into
    // a straight-alpha, sRGB-encoded RGBA8 thumbnail (internal edges are
    // premultiplied linear, §12). request encodes the GPU work against the
    // last evaluated output and returns false when the node is unknown or
    // holds no texture yet; the readback completes via the device's
    // callback processing, so poll take on later frames. One request in
    // flight; a newer request supersedes an unclaimed result. take returns
    // nullptr while pending and a 0x0 preview on readback failure.
    struct NodePreview {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };
    bool requestNodePreview(const wgpu::Device& device,
                            const std::string& nodeId, uint32_t maxWidth,
                            uint32_t maxHeight);
    std::unique_ptr<NodePreview> takeNodePreview();
    // The node's texture-output revision (see Node::textureRevision), or
    // -1 for an unknown node or one that never produces a texture.
    int64_t nodeOutputRevision(const std::string& nodeId) const;

private:
    Scene() = default;
    friend struct SceneBuilder;

    std::string mName;
    std::vector<std::unique_ptr<Node>> mNodes; // topological order
    std::vector<SceneParam> mParams;
    std::vector<class SequenceNode*> mSequences; // owned via mNodes
    std::vector<std::pair<Node*, size_t>> mPendingFires;
    Node* mOutput = nullptr;
    bool mAnimated = false;
    uint64_t mFrame = 0;
    uint32_t mLastWidth = 0, mLastHeight = 0;
    wgpu::TextureFormat mLastFormat = wgpu::TextureFormat::Undefined;
    // 1x1 transparent texture: what a feedback edge reads before its
    // producer has ever been written (§10 first-frame rule).
    wgpu::Texture mBlank;

    // Preview tap state (implementation lives in Nodes.cpp beside the
    // shared shader helpers).
    wgpu::RenderPipeline mPreviewPipeline;
    wgpu::Sampler mPreviewSampler;
    struct PreviewPending;
    std::shared_ptr<PreviewPending> mPreviewPending;
};

} // namespace drift::core
