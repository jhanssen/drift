#pragma once

// The scene graph: nodes, typed ports, and dirty-driven evaluation
// (SCENE_FORMAT.md §8, §11). Platform-agnostic — sees only the portable
// WebGPU API and caller-provided data.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Value.h"
#include "Video.h"

namespace drift::core {

struct FrameContext {
    wgpu::Device device;
    float seconds = 0.0f; // scene time; does not advance while paused
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
        Value value;
        bool dirty = false;
        // Previous-frame state (§10), maintained by Scene::render. prev and
        // prevDirty snapshot value/dirty at frame start; texture outputs read
        // through a feedback edge additionally keep their last written
        // contents in a history copy (needsHistory is set by the loader).
        Value prev;
        bool prevDirty = false;
        bool needsHistory = false;
        wgpu::Texture history;
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

    Value inputValue(size_t index) const;
    bool inputsDirty() const;

    std::string id;
    std::vector<Input> inputs;
    std::vector<Output> outputs;
    bool firstEvaluate = true;
    bool paramsChanged = false; // a bound scene parameter changed (§11)
};

// A user-tweakable scene parameter (§6): declaration metadata plus the
// current value.
struct SceneParam {
    std::string name;
    ValueType type = ValueType::Scalar;
    Value value; // current (starts at the declared default)
    Value min, max;
    bool hasMin = false, hasMax = false;
};

// Reads a project-relative file (text or binary) into out. The platform
// enforces project-root confinement.
using AssetReader = std::function<bool(const std::string& path, std::string& out)>;

class Scene {
public:
    // Parses and validates scene.json. Returns nullptr on failure with
    // human-readable messages appended to errors. videoFactory may be null:
    // scenes with video nodes then fail to load.
    static std::unique_ptr<Scene> load(const std::string& sceneJson,
                                       const AssetReader& readAsset,
                                       const VideoDecoderFactory& videoFactory,
                                       const wgpu::Device& device,
                                       std::vector<std::string>& errors);

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

private:
    Scene() = default;
    friend struct SceneBuilder;

    std::string mName;
    std::vector<std::unique_ptr<Node>> mNodes; // topological order
    std::vector<SceneParam> mParams;
    Node* mOutput = nullptr;
    bool mAnimated = false;
    uint64_t mFrame = 0;
    uint32_t mLastWidth = 0, mLastHeight = 0;
    wgpu::TextureFormat mLastFormat = wgpu::TextureFormat::Undefined;
    // 1x1 transparent texture: what a feedback edge reads before its
    // producer has ever been written (§10 first-frame rule).
    wgpu::Texture mBlank;
};

} // namespace drift::core
