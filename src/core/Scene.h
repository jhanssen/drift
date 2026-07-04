#pragma once

// The scene graph: nodes, typed ports, and dirty-driven evaluation
// (SCENE_FORMAT.md §8, §11). Platform-agnostic — sees only the portable
// WebGPU API and caller-provided data.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Value.h"

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
    // Scaffold: the platform double-buffers, so the output blit must run
    // every frame even when nothing upstream changed. TODO: per-buffer
    // content tracking so unchanged frames skip the blit and the commit.
    bool forcePresent = true;
    bool presented = false; // out: output node wrote ctx.target
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
    };
    struct Output {
        Value value;
        bool dirty = false;
    };

    virtual ~Node() = default;
    virtual void evaluate(FrameContext& ctx) = 0;
    // Input-source nodes (time) evaluate every frame and decide dirtiness
    // themselves; everything else evaluates only when an input is dirty.
    virtual bool alwaysEvaluate() const { return false; }

    Value inputValue(size_t index) const;
    bool inputsDirty() const;

    std::string id;
    std::vector<Input> inputs;
    std::vector<Output> outputs;
    bool firstEvaluate = true;
};

// Reads a project-relative file (text or binary) into out. The platform
// enforces project-root confinement.
using AssetReader = std::function<bool(const std::string& path, std::string& out)>;

class Scene {
public:
    // Parses and validates scene.json. Returns nullptr on failure with
    // human-readable messages appended to errors.
    static std::unique_ptr<Scene> load(const std::string& sceneJson,
                                       const AssetReader& readAsset,
                                       const wgpu::Device& device,
                                       std::vector<std::string>& errors);

    // Evaluates the graph for one frame. Returns ctx.presented.
    bool render(FrameContext& ctx);

    const std::string& name() const { return mName; }

private:
    Scene() = default;
    friend struct SceneBuilder;

    std::string mName;
    std::vector<std::unique_ptr<Node>> mNodes; // topological order
    Node* mOutput = nullptr;
    uint64_t mFrame = 0;
    uint32_t mLastWidth = 0, mLastHeight = 0;
    wgpu::TextureFormat mLastFormat = wgpu::TextureFormat::Undefined;
};

} // namespace drift::core
