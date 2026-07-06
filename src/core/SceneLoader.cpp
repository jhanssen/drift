#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string_view>

#include <glaze/glaze.hpp>

#include "NodeProps.h"
#include "Nodes.h"
#include "Scene.h"

// scene.json -> validated node graph (SCENE_FORMAT.md). All validation
// listed in §13 that applies to the implemented node subset happens here.

namespace drift::core {

struct SceneBuilder {
    static std::unique_ptr<Scene> make(std::string name,
                                       std::vector<std::unique_ptr<Node>> nodes,
                                       std::vector<SceneParam> params,
                                       std::vector<SequenceNode*> sequences,
                                       Node* output)
    {
        auto scene = std::unique_ptr<Scene>(new Scene());
        scene->mName = std::move(name);
        scene->mNodes = std::move(nodes);
        scene->mParams = std::move(params);
        scene->mSequences = std::move(sequences);
        scene->mOutput = output;
        for (const auto& node : scene->mNodes) {
            if (node->drivesFrames()) {
                scene->mAnimated = true;
                break;
            }
        }
        return scene;
    }
};

namespace {

struct RawConn {
    std::string node;
    std::string port; // empty = default output
    bool previous = false;
};

struct RawInput {
    enum class Kind { Literal, Param, Conn, Array } kind = Kind::Literal;
    Value literal;
    std::string param;
    RawConn conn;
    std::vector<RawInput> elements; // Array: texture[] ports (§9.5)
};

struct RawNode {
    std::string id;
    std::string type;
    std::map<std::string, RawInput> inputs;
    const glz::generic* json = nullptr;
    // §20.1: non-empty ("packages/<name>[@pin]/") for nodes that came from
    // a package's graph file — their relative paths resolve against the
    // package root.
    std::string assetRoot;
};

struct Param {
    std::string name;
    ValueType type;
    Value def;
    Value min, max;
    bool hasMin = false, hasMax = false;
    float step = 0.0f;
    std::string label;
    std::string hint;
};

// §20.1: dotted decimal integers ("1", "1.2.0"); also the pin syntax,
// which is a prefix of the same shape (§20.3).
bool validVersion(const std::string& s)
{
    size_t start = 0;
    while (start <= s.size()) {
        const size_t dot = s.find('.', start);
        const std::string seg =
            s.substr(start, dot == std::string::npos ? dot : dot - start);
        if (seg.empty() ||
            seg.find_first_not_of("0123456789") != std::string::npos) {
            return false;
        }
        if (dot == std::string::npos) {
            return true;
        }
        start = dot + 1;
    }
    return false;
}

bool validPackageName(const std::string& s)
{
    return !s.empty() &&
           s.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") ==
               std::string::npos;
}

bool validId(const std::string& s)
{
    if (s.empty() || (!isalpha((unsigned char)s[0]) && s[0] != '_')) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return isalnum(c) || c == '_';
    });
}

bool parseValueType(const std::string& s, ValueType& out)
{
    if (s == "scalar") { out = ValueType::Scalar; return true; }
    if (s == "vec2") { out = ValueType::Vec2; return true; }
    if (s == "vec3") { out = ValueType::Vec3; return true; }
    if (s == "vec4") { out = ValueType::Vec4; return true; }
    return false;
}

bool parseLiteral(const glz::generic& j, Value& out)
{
    if (j.is_number()) {
        out.type = ValueType::Scalar;
        out.v[0] = j.get_number();
        return true;
    }
    if (j.is_array()) {
        const auto& arr = j.get_array();
        if (arr.size() < 2 || arr.size() > 4) {
            return false;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            if (!arr[i].is_number()) {
                return false;
            }
            out.v[i] = arr[i].get_number();
        }
        out.type = arr.size() == 2 ? ValueType::Vec2
                 : arr.size() == 3 ? ValueType::Vec3
                                   : ValueType::Vec4;
        return true;
    }
    return false;
}

// "@node", "@node.port", "$param", literal, or expanded object form (§7).
bool parseRawInput(const glz::generic& j, RawInput& out, std::string& err)
{
    if (j.is_string()) {
        const std::string& s = j.get_string();
        if (s.starts_with("@")) {
            out.kind = RawInput::Kind::Conn;
            const size_t dot = s.find('.');
            if (dot == std::string::npos) {
                out.conn.node = s.substr(1);
            } else {
                out.conn.node = s.substr(1, dot - 1);
                out.conn.port = s.substr(dot + 1);
            }
            return true;
        }
        if (s.starts_with("$")) {
            out.kind = RawInput::Kind::Param;
            out.param = s.substr(1);
            return true;
        }
        err = "string input must be a @node or $parameter reference";
        return false;
    }
    if (j.is_object()) {
        const auto& obj = j.get_object();
        auto node = obj.find("node");
        if (node == obj.end() || !node->second.is_string()) {
            err = "expanded connection needs a string 'node'";
            return false;
        }
        out.kind = RawInput::Kind::Conn;
        out.conn.node = node->second.get_string();
        if (auto port = obj.find("port"); port != obj.end()) {
            if (!port->second.is_string()) {
                err = "'port' must be a string";
                return false;
            }
            out.conn.port = port->second.get_string();
        }
        if (auto prev = obj.find("previous"); prev != obj.end()) {
            if (!prev->second.is_boolean()) {
                err = "'previous' must be a boolean";
                return false;
            }
            out.conn.previous = prev->second.get_boolean();
        }
        return true;
    }
    // An array whose elements aren't all numbers is an array-valued port
    // (texture[], §9.5): each element follows the reference grammar.
    if (j.is_array()) {
        const auto& arr = j.get_array();
        bool allNumbers = !arr.empty();
        for (const auto& e : arr) {
            if (!e.is_number()) {
                allNumbers = false;
                break;
            }
        }
        if (!allNumbers) {
            out.kind = RawInput::Kind::Array;
            for (const auto& e : arr) {
                RawInput element;
                if (!parseRawInput(e, element, err)) {
                    return false;
                }
                if (element.kind != RawInput::Kind::Conn) {
                    err = "array elements must be connections";
                    return false;
                }
                out.elements.push_back(std::move(element));
            }
            return true;
        }
    }

    if (!parseLiteral(j, out.literal)) {
        err = "invalid literal (number or array of 2-4 numbers)";
        return false;
    }
    out.kind = RawInput::Kind::Literal;
    return true;
}

// Applies fn to every connection in an input, descending into arrays.
template <typename Fn>
void forEachConn(const RawInput& in, Fn&& fn)
{
    if (in.kind == RawInput::Kind::Conn) {
        fn(in.conn);
    } else if (in.kind == RawInput::Kind::Array) {
        for (const auto& e : in.elements) {
            fn(e.conn);
        }
    }
}

// Static input port tables for the fixed-signature node types. Polymorphic
// ports carry a sentinel resolved during instantiation.
constexpr ValueType kPoly = (ValueType)0xff;

struct PortDef {
    std::string name;
    ValueType type;
    bool required;
    std::array<double, 4> def{};
    bool array = false; // texture[]: expands to one Node::Input per element
    uint32_t stride = 0; // buffer ports (§18.1): required element stride
};

const PortDef kWaveInputs[] = {
    { "input", ValueType::Scalar, true },
    { "frequency", ValueType::Scalar, false, { 1, 0, 0, 0 } },
    { "phase", ValueType::Scalar, false, { 0, 0, 0, 0 } },
};
const PortDef kRemapInputs[] = {
    { "value", kPoly, true },
    { "inMin", kPoly, false, { 0, 0, 0, 0 } },
    { "inMax", kPoly, false, { 1, 1, 1, 1 } },
    { "outMin", kPoly, false, { 0, 0, 0, 0 } },
    { "outMax", kPoly, false, { 1, 1, 1, 1 } },
};
const PortDef kCombineInputs[] = {
    { "x", ValueType::Scalar, false, { 0, 0, 0, 0 } },
    { "y", ValueType::Scalar, false, { 0, 0, 0, 0 } },
    { "z", ValueType::Scalar, false, { 0, 0, 0, 0 } },
    { "w", ValueType::Scalar, false, { 0, 0, 0, 0 } },
};
const PortDef kSplitInputs[] = {
    { "value", kPoly, true },
};
const PortDef kAddInputs[] = {
    { "a", kPoly, true },
    { "b", kPoly, false, { 0, 0, 0, 0 } },
};
const PortDef kMultiplyInputs[] = {
    { "a", kPoly, true },
    { "b", kPoly, false, { 1, 1, 1, 1 } },
};
const PortDef kMixInputs[] = {
    { "a", kPoly, true },
    { "b", kPoly, true },
    { "t", ValueType::Scalar, false, { 0.5, 0, 0, 0 } },
};
const PortDef kClampInputs[] = {
    { "value", kPoly, true },
    { "lo", kPoly, false, { 0, 0, 0, 0 } },
    { "hi", kPoly, false, { 1, 1, 1, 1 } },
};
const PortDef kNoiseInputs[] = {
    { "input", ValueType::Scalar, true },
    { "frequency", ValueType::Scalar, false, { 1, 0, 0, 0 } },
    { "seed", ValueType::Scalar, false, { 0, 0, 0, 0 } },
};
const PortDef kDampInputs[] = {
    { "value", kPoly, true },
    { "time", ValueType::Scalar, true },
    { "halflife", ValueType::Scalar, false, { 0.2, 0, 0, 0 } },
};
const PortDef kEdgeInputs[] = {
    { "value", ValueType::Scalar, true },
    { "threshold", ValueType::Scalar, false, { 0.5, 0, 0, 0 } },
};
const PortDef kOutputInputs[] = {
    { "color", ValueType::Texture, true },
};
const PortDef kTransformInputs[] = {
    { "source", ValueType::Texture, true },
    { "position", ValueType::Vec2, false, { 0.5f, 0.5f } },
    { "rotation", ValueType::Scalar, false, { 0, 0, 0, 0 } },
    { "scale", ValueType::Vec2, false, { 1, 1 } },
    { "anchor", ValueType::Vec2, false, { 0.5f, 0.5f } },
    { "opacity", ValueType::Scalar, false, { 1, 0, 0, 0 } },
};
const PortDef kCompositorInputs[] = {
    { "layers", ValueType::Texture, true, {}, true },
};
const PortDef kFitInputs[] = {
    { "source", ValueType::Texture, true },
};
const PortDef kSequenceInputs[] = {
    { "time", ValueType::Scalar, true },
};
const PortDef kVideoInputs[] = {
    { "playing", ValueType::Scalar, false, { 1, 0, 0, 0 } },
    { "restart", ValueType::Event, false },
};
// §18.3 particles — order must match ParticlesNode::Port exactly.
const PortDef kParticlesInputs[] = {
    { "rate", ValueType::Scalar, false, { 10 } },
    { "burst", ValueType::Event, false },
    { "burstCount", ValueType::Scalar, false, { 32 } },
    { "origin", ValueType::Vec2, false, { 0.5, 0.5 } },
    { "extent", ValueType::Vec2, false, { 0, 0 } },
    { "emitScale", ValueType::Vec2, false, { 1, 1 } },
    { "direction", ValueType::Vec2, false, { 0, -1 } },
    { "spread", ValueType::Scalar, false, { 180 } },
    { "speed", ValueType::Vec2, false, { 0.05, 0.15 } },
    { "lifetime", ValueType::Vec2, false, { 1, 3 } },
    { "size", ValueType::Vec2, false, { 0.01, 0.03 } },
    { "spin", ValueType::Vec2, false, { 0, 0 } },
    { "colorStart", ValueType::Vec4, false, { 1, 1, 1, 1 } },
    { "colorEnd", ValueType::Vec4, false, { 1, 1, 1, 0 } },
    { "fadeIn", ValueType::Scalar, false, { 0.1 } },
    { "fadeOut", ValueType::Scalar, false, { 0 } },
    { "sizeEnd", ValueType::Scalar, false, { 1 } },
    { "sizeWindow", ValueType::Vec2, false, { 0, 1 } },
    { "gravity", ValueType::Vec2, false, { 0, 0 } },
    { "drag", ValueType::Scalar, false, { 0 } },
    { "turbulence", ValueType::Scalar, false, { 0 } },
    { "turbulenceScale", ValueType::Scalar, false, { 4 } },
    { "turbulenceMask", ValueType::Vec2, false, { 1, 1 } },
    { "attractor", ValueType::Vec2, false, { 0.5, 0.5 } },
    { "attract", ValueType::Scalar, false, { 0 } },
    { "vortex", ValueType::Scalar, false, { 0 } },
    { "delay", ValueType::Scalar, false, { 0 } },
    { "duration", ValueType::Scalar, false, { 0 } },
    { "ring", ValueType::Scalar, false, { 0 } },
    { "depth", ValueType::Vec2, false, { 0, 0 } },
    { "collide", ValueType::Texture, false },
    { "bounce", ValueType::Scalar, false, { 0.5 } },
    { "tintVary", ValueType::Vec4, false, { 1, 1, 1, 1 } },
    { "tintVaryMax", ValueType::Vec4, false, { 1, 1, 1, 1 } },
    { "velocityMin", ValueType::Vec2, false, { 0, 0 } },
    { "velocityMax", ValueType::Vec2, false, { 0, 0 } },
    { "twinkle", ValueType::Vec2, false, { 1, 1 } },
    { "twinkleRate", ValueType::Vec2, false, { 1, 2 } },
    { "prewarm", ValueType::Scalar, false, { 0 } },
    { "spawn", ValueType::Buffer, false, {}, false, ParticlesNode::kStride },
    { "inherit", ValueType::Scalar, false, { 0 } },
    // Hidden §18.5.4 feedback edge: injected by the loader when 'spawn' is
    // connected (death detection needs the source's previous tick).
    { "spawnPrev", ValueType::Buffer, false, {}, false,
      ParticlesNode::kStride },
    { "time", ValueType::Scalar, true },
};
// Order must match SpritesNode::Port.
const PortDef kSpritesInputs[] = {
    { "particles", ValueType::Buffer, true, {}, false,
      ParticlesNode::kStride },
    { "texture", ValueType::Texture, false },
    { "frameRate", ValueType::Scalar, false, { 0 } },
    { "parallax", ValueType::Vec2, false, { 0, 0 } },
    { "flutter", ValueType::Vec2, false, { 0, 0 } },
    { "flutterRate", ValueType::Scalar, false, { 1 } },
    { "stretch", ValueType::Vec2, false, { 1, 1 } },
    { "frameBlend", ValueType::Scalar, false, { 0 } },
    { "align", ValueType::Scalar, false, { 0 } },
};
// Order must match TrailsNode::Port.
const PortDef kTrailsInputs[] = {
    { "particles", ValueType::Buffer, true, {}, false,
      ParticlesNode::kStride },
    { "width", ValueType::Scalar, false, { 1 } },
    { "taper", ValueType::Scalar, false, { 0 } },
    { "fade", ValueType::Scalar, false, { 0 } },
    { "parallax", ValueType::Vec2, false, { 0, 0 } },
    { "feather", ValueType::Scalar, false, { 0 } },
};
// §18.5.3 emitter-entry fields — order must match ParticlesNode::EmitterField
// exactly. Entries inject real input ports named "emitters[i].<field>".
const struct {
    const char* name;
    ValueType type;
} kEmitterFields[] = {
    { "origin", ValueType::Vec2 }, { "extent", ValueType::Vec2 },
    { "direction", ValueType::Vec2 }, { "spread", ValueType::Scalar },
    { "speed", ValueType::Vec2 }, { "lifetime", ValueType::Vec2 },
    { "size", ValueType::Vec2 }, { "spin", ValueType::Vec2 },
    { "colorStart", ValueType::Vec4 }, { "colorEnd", ValueType::Vec4 },
    { "depth", ValueType::Vec2 }, { "delay", ValueType::Scalar },
    { "duration", ValueType::Scalar }, { "weight", ValueType::Scalar },
};
static_assert(std::size(kEmitterFields) == ParticlesNode::EfCount);

bool parseEmitterShape(const std::string& kind, ParticlesNode::Emitter& out)
{
    if (kind == "point") {
        out = ParticlesNode::Emitter::Point;
    } else if (kind == "box") {
        out = ParticlesNode::Emitter::Box;
    } else if (kind == "disc") {
        out = ParticlesNode::Emitter::Disc;
    } else {
        return false;
    }
    return true;
}

struct OutputPortDef {
    const char* name;
    int index;
};

// name -> output port index per node type; index 0 is the default output.
std::vector<OutputPortDef> outputPortsFor(const std::string& type)
{
    if (type == "time") return { { "seconds", 0 }, { "delta", 1 } };
    if (type == "mouse") return { { "position", 0 }, { "active", 1 } };
    if (type == "split") return { { "x", 0 }, { "y", 1 }, { "z", 2 }, { "w", 3 } };
    if (type == "video") return { { "result", 0 }, { "finished", 1 } };
    if (type == "output") return {};
    return { { "result", 0 } };
}

bool isImplicitNode(const std::string& id)
{
    return id == "time" || id == "mouse";
}

class Loader {
public:
    Loader(const AssetReader& readAsset, const VideoDecoderFactory& videoFactory,
           const wgpu::Device& device, std::vector<std::string>& errors,
           std::vector<std::string>& warnings)
        : mReadAsset(readAsset), mVideoFactory(videoFactory), mDevice(device),
          mErrors(errors), mWarnings(warnings)
    {
    }

    std::unique_ptr<Scene> load(const std::string& sceneJson);

private:
    void fail(const std::string& msg) { mErrors.push_back(msg); }
    void warn(const std::string& msg) { mWarnings.push_back(msg); }

    bool parseParameters(const glz::generic& json);
    bool parseNodes(const glz::generic& json);
    // One §8 node entry → RawNode. Shared by scene.json and graph files
    // (§19.1); inside a graph file `output` nodes and `$param` references
    // are rejected, and particle emitter specs key under specKey so two
    // instances of one file don't collide.
    bool parseRawNode(const glz::generic& entry, RawNode& raw,
                      const std::string& specKey, bool inGraphFile);
    // §18.5.3: validates the 'emitters' property, injects entry fields into
    // raw.inputs as "emitters[i].<field>" ports (so topo sort, parameters,
    // and type checking see them), and records shapes + override masks.
    bool parseEmitters(RawNode& raw, const glz::generic& json,
                       const std::string& specKey);
    // §19.3: replaces every `graph` instance with its inner nodes
    // (namespaced ids), applies interface bindings, and rewrites
    // references — level by level until none remain.
    bool expandGraphs();
    bool topoSort();
    bool instantiate(const RawNode& raw);
    Node* makeNode(const RawNode& raw, std::vector<PortDef>& portsOut);
    bool bindInputs(const RawNode& raw, Node* node, const std::vector<PortDef>& ports);
    bool resolveInputType(const RawNode& raw, const RawInput& in, ValueType& out);
    bool bindDeferred();
    // Output ports of a connection's producer. Static per type except
    // sequence, whose ports are its tracks (the instance must exist — true
    // for every caller: topo order for forward edges, bindDeferred for
    // feedback).
    std::vector<OutputPortDef> producerPorts(const std::string& nodeId);

    // Feedback edges bind after all nodes exist — the producer may come
    // later in topo order (or be the consumer itself).
    struct DeferredBind {
        Node* node;
        size_t inputIdx;
        std::string consumerId; // for error messages
        std::string portName;
        RawConn conn;
        ValueType portType;
        uint32_t portStride = 0; // buffer ports (§18.1)
    };
    std::vector<DeferredBind> mDeferredBinds;

    const AssetReader& mReadAsset;
    const VideoDecoderFactory& mVideoFactory;
    const wgpu::Device& mDevice;
    std::vector<std::string>& mErrors;
    std::vector<std::string>& mWarnings;

    // §18.5.3 parsed emitter entries per particles node: shape (-1 =
    // inherit the node-level 'emitter' property) and override mask. Keyed
    // by node id for scene nodes, "<path>\n<id>" for graph-file nodes
    // (copied to the namespaced id at expansion).
    struct EmitterSpec {
        std::vector<int> kinds;
        std::vector<uint32_t> masks;
    };
    std::map<std::string, EmitterSpec> mEmitterSpecs;

    // §19.1 parsed graph files, cached by project-relative path. The doc
    // keeps every inner RawNode::json pointer alive through load.
    struct GraphDef {
        struct In {
            ValueType type;
            bool hasDefault = false;
            Value def;
            std::vector<RawConn> binds;
        };
        std::string path;
        std::string name;
        std::string assetRoot; // §20.1, "" for project-local graph files
        std::vector<std::pair<std::string, In>> inputs; // declaration order
        std::map<std::string, RawConn> outputs;
        std::vector<RawNode> nodes;
        std::set<std::string> nodeIds;
        std::unique_ptr<glz::generic> doc;
    };
    GraphDef* graphDef(const std::string& path); // nullptr = failed (fail())
    std::map<std::string, std::unique_ptr<GraphDef>> mGraphDefs;

    // §20: version pins from scene.json's "packages" block, canonicalized
    // into every package reference as "packages/<name>@<pin>/…".
    bool assetPath(const RawNode& raw, std::string& path, const char* what);
    bool checkPackage(const std::string& nodeId, const std::string& name,
                      const std::string& ref);
    std::map<std::string, std::string> mPins;
    std::set<std::string> mPinsUsed;
    std::set<std::string> mPackagesChecked;

    std::vector<Param> mParams;
    std::map<std::string, int> mParamIndex;
    std::vector<RawNode> mRawNodes;
    std::map<std::string, size_t> mRawIndex;
    std::vector<size_t> mTopoOrder;
    bool mNeedsTime = false;
    bool mNeedsMouse = false;

    std::map<std::string, Node*> mInstances;
    std::vector<std::unique_ptr<Node>> mNodes;
    std::vector<SequenceNode*> mSequences;
    Node* mOutput = nullptr;
};

std::vector<OutputPortDef> Loader::producerPorts(const std::string& nodeId)
{
    if (isImplicitNode(nodeId)) {
        return outputPortsFor(nodeId);
    }
    const RawNode& raw = mRawNodes[mRawIndex[nodeId]];
    if (raw.type == "sequence") {
        auto* seq = static_cast<SequenceNode*>(mInstances[nodeId]);
        std::vector<OutputPortDef> ports;
        for (int i = 0; i < (int)seq->tracks().size(); ++i) {
            ports.push_back({ seq->tracks()[i].name.c_str(), i });
        }
        return ports;
    }
    if (raw.type == "compute") {
        // Reflected outputs (§18.2), named at construction like sequence
        // tracks; the instance exists for every caller (topo/deferred).
        Node* node = mInstances[nodeId];
        std::vector<OutputPortDef> ports;
        for (int i = 0; i < (int)node->outputs.size(); ++i) {
            ports.push_back({ node->outputs[i].name.c_str(), i });
        }
        return ports;
    }
    return outputPortsFor(raw.type);
}

bool Loader::parseParameters(const glz::generic& json)
{
    if (!json.is_object()) {
        fail("'parameters' must be an object");
        return false;
    }
    for (const auto& [name, decl] : json.get_object()) {
        if (!validId(name)) {
            fail("invalid parameter name '" + name + "'");
            return false;
        }
        if (!decl.is_object()) {
            fail("parameter '" + name + "' must be an object");
            return false;
        }
        const auto& obj = decl.get_object();
        auto typeIt = obj.find("type");
        if (typeIt == obj.end() || !typeIt->second.is_string()) {
            fail("parameter '" + name + "' needs a string 'type'");
            return false;
        }
        Param param;
        param.name = name;
        if (!parseValueType(typeIt->second.get_string(), param.type)) {
            fail("parameter '" + name + "': unknown type '" +
                 typeIt->second.get_string() + "'");
            return false;
        }
        auto defIt = obj.find("default");
        if (defIt == obj.end() || !parseLiteral(defIt->second, param.def)) {
            fail("parameter '" + name + "' needs a literal 'default'");
            return false;
        }
        if (param.def.type != param.type &&
            param.def.type != ValueType::Scalar) {
            fail("parameter '" + name + "': default does not match type");
            return false;
        }
        if (param.def.type == ValueType::Scalar && param.type != ValueType::Scalar) {
            const double s = param.def.v[0];
            param.def.type = param.type;
            param.def.v = { s, s, s, s };
        }
        // min/max (§6): optional, per-component for vectors, scalar splats.
        auto bound = [&](const char* key, Value& out, bool& has) -> bool {
            auto it = obj.find(key);
            if (it == obj.end()) {
                return true;
            }
            if (!parseLiteral(it->second, out) ||
                (out.type != param.type && out.type != ValueType::Scalar)) {
                fail("parameter '" + name + "': '" + std::string(key) +
                     "' does not match type");
                return false;
            }
            if (out.type == ValueType::Scalar && param.type != ValueType::Scalar) {
                const double s = out.v[0];
                out.type = param.type;
                out.v = { s, s, s, s };
            }
            has = true;
            return true;
        };
        if (!bound("min", param.min, param.hasMin) ||
            !bound("max", param.max, param.hasMax)) {
            return false;
        }
        if (auto it = obj.find("step"); it != obj.end() && it->second.is_number()) {
            param.step = (float)it->second.get_number();
        }
        if (auto it = obj.find("label"); it != obj.end() && it->second.is_string()) {
            param.label = it->second.get_string();
        }
        if (auto it = obj.find("hint"); it != obj.end() && it->second.is_string()) {
            param.hint = it->second.get_string();
            const bool color = param.hint == "color" &&
                               (param.type == ValueType::Vec3 ||
                                param.type == ValueType::Vec4);
            if (!color) {
                warn("parameter '" + name + "': unknown hint '" + param.hint +
                     "' ignored");
                param.hint.clear();
            }
        }
        mParamIndex[name] = (int)mParams.size();
        mParams.push_back(std::move(param));
    }
    return true;
}

bool Loader::parseNodes(const glz::generic& json)
{
    if (!json.is_array()) {
        fail("'nodes' must be an array");
        return false;
    }
    for (const auto& entry : json.get_array()) {
        RawNode raw;
        if (!parseRawNode(entry, raw, /*specKey=*/"", /*inGraphFile=*/false)) {
            return false;
        }
        if (mRawIndex.count(raw.id)) {
            fail("duplicate node id '" + raw.id + "'");
            return false;
        }
        mRawIndex[raw.id] = mRawNodes.size();
        mRawNodes.push_back(std::move(raw));
    }
    return true;
}

bool Loader::parseRawNode(const glz::generic& entry, RawNode& raw,
                          const std::string& specKey, bool inGraphFile)
{
    if (!entry.is_object()) {
        fail("node entries must be objects");
        return false;
    }
    const auto& obj = entry.get_object();
    raw.json = &entry;

    auto idIt = obj.find("id");
    auto typeIt = obj.find("type");
    if (idIt == obj.end() || !idIt->second.is_string() ||
        typeIt == obj.end() || !typeIt->second.is_string()) {
        fail("every node needs string 'id' and 'type'");
        return false;
    }
    raw.id = idIt->second.get_string();
    raw.type = typeIt->second.get_string();
    if (!validId(raw.id)) {
        fail("invalid node id '" + raw.id + "'");
        return false;
    }
    if (raw.id == "time" || raw.id == "mouse") {
        fail("node id '" + raw.id + "' is reserved (implicit input node)");
        return false;
    }
    if (inGraphFile && raw.type == "output") {
        fail("graph file node '" + raw.id +
             "': a subgraph produces ports, not the frame (§19.1)");
        return false;
    }

    // Unknown properties are a warning, not an error (§13) — but a
    // typo'd property silently changing behavior is the worst authoring
    // footgun, so say something. The known set derives from the shared
    // NodeProps.h table (also served to editors via drift_node_props).
    static const std::map<std::string, std::vector<std::string>> kProps =
        [] {
            std::map<std::string, std::vector<std::string>> map;
            for (const char* type : kNodeTypes) {
                map[type];
            }
            for (const NodePropDef& p : kNodeProps) {
                map[p.node].push_back(p.name);
            }
            return map;
        }();
    if (auto propsIt = kProps.find(raw.type); propsIt != kProps.end()) {
        for (const auto& [key, value] : obj) {
            if (key == "id" || key == "type" || key == "inputs") {
                continue;
            }
            const auto& known = propsIt->second;
            if (std::find(known.begin(), known.end(), key) == known.end()) {
                warn("node '" + raw.id + "': unknown property '" + key +
                     "' ignored");
            }
        }
    }

    if (auto inputsIt = obj.find("inputs"); inputsIt != obj.end()) {
        if (!inputsIt->second.is_object()) {
            fail("node '" + raw.id + "': 'inputs' must be an object");
            return false;
        }
        for (const auto& [port, value] : inputsIt->second.get_object()) {
            RawInput in;
            std::string err;
            if (!parseRawInput(value, in, err)) {
                fail("node '" + raw.id + "' input '" + port + "': " + err);
                return false;
            }
            forEachConn(in, [&](const RawConn& conn) {
                if (conn.node == "time") {
                    mNeedsTime = true;
                } else if (conn.node == "mouse") {
                    mNeedsMouse = true;
                }
            });
            raw.inputs[port] = std::move(in);
        }
    }
    if (raw.type == "particles") {
        if (auto emIt = obj.find("emitters"); emIt != obj.end()) {
            if (!parseEmitters(raw, emIt->second, specKey + raw.id)) {
                return false;
            }
        }
        // §18.5.4: death detection compares the spawn source against
        // its previous tick — wire the hidden feedback edge here so
        // the history machinery (§10) picks it up.
        if (auto spIt = raw.inputs.find("spawn"); spIt != raw.inputs.end()) {
            if (spIt->second.kind != RawInput::Kind::Conn ||
                spIt->second.conn.previous) {
                fail("node '" + raw.id + "': 'spawn' must be a direct "
                     "(non-feedback) connection (§18.5.4)");
                return false;
            }
            RawInput prev = spIt->second;
            prev.conn.previous = true;
            raw.inputs["spawnPrev"] = std::move(prev);
        }
    }
    if (inGraphFile) {
        // §19.1: a subgraph's knobs are its declared inputs — scene
        // parameters do not reach inside.
        for (const auto& [port, in] : raw.inputs) {
            bool param = in.kind == RawInput::Kind::Param;
            for (const auto& e : in.elements) {
                param = param || e.kind == RawInput::Kind::Param;
            }
            if (param) {
                fail("graph file node '" + raw.id + "' input '" + port +
                     "': $parameter references are not allowed in graph "
                     "files (§19.1)");
                return false;
            }
        }
    }
    return true;
}

bool Loader::parseEmitters(RawNode& raw, const glz::generic& json,
                           const std::string& specKey)
{
    if (!json.is_array() || json.get_array().empty() ||
        json.get_array().size() > ParticlesNode::kMaxEmitters) {
        fail("node '" + raw.id + "': 'emitters' must be an array of 1-" +
             std::to_string(ParticlesNode::kMaxEmitters) +
             " objects (§18.5.3)");
        return false;
    }
    EmitterSpec spec;
    size_t index = 0;
    for (const auto& entry : json.get_array()) {
        if (!entry.is_object()) {
            fail("node '" + raw.id + "': 'emitters' entries must be objects");
            return false;
        }
        int kind = -1;
        uint32_t mask = 0;
        for (const auto& [key, value] : entry.get_object()) {
            if (key == "emitter") {
                ParticlesNode::Emitter shape;
                if (!value.is_string() ||
                    !parseEmitterShape(value.get_string(), shape)) {
                    fail("node '" + raw.id + "' emitters[" +
                         std::to_string(index) +
                         "]: 'emitter' must be \"point\", \"box\", or "
                         "\"disc\"");
                    return false;
                }
                kind = (int)shape;
                continue;
            }
            int field = -1;
            for (size_t f = 0; f < std::size(kEmitterFields); ++f) {
                if (key == kEmitterFields[f].name) {
                    field = (int)f;
                    break;
                }
            }
            if (field < 0) {
                fail("node '" + raw.id + "' emitters[" +
                     std::to_string(index) + "]: '" + key +
                     "' is not an emission-time input (§18.5.3)");
                return false;
            }
            RawInput in;
            std::string err;
            if (!parseRawInput(value, in, err)) {
                fail("node '" + raw.id + "' emitters[" +
                     std::to_string(index) + "]." + key + ": " + err);
                return false;
            }
            forEachConn(in, [&](const RawConn& conn) {
                if (conn.node == "time") {
                    mNeedsTime = true;
                } else if (conn.node == "mouse") {
                    mNeedsMouse = true;
                }
            });
            raw.inputs["emitters[" + std::to_string(index) + "]." + key] =
                std::move(in);
            mask |= 1u << field;
        }
        spec.kinds.push_back(kind);
        spec.masks.push_back(mask);
        ++index;
    }
    mEmitterSpecs[specKey] = std::move(spec);
    return true;
}

// Canonicalize a node's path property (§20.2): package-root prefix for
// nodes that came from a package's graph file, pin insertion and
// self-containment checks for packages/ references.
bool Loader::assetPath(const RawNode& raw, std::string& path,
                       const char* what)
{
    constexpr std::string_view kPrefix = "packages/";
    if (!path.starts_with(kPrefix)) {
        if (!raw.assetRoot.empty()) {
            path = raw.assetRoot + path;
        }
        return true;
    }
    if (!raw.assetRoot.empty()) {
        fail("node '" + raw.id + "': " + what +
             " may not reference another package from inside a package "
             "(§20.1)");
        return false;
    }
    const size_t nameStart = kPrefix.size();
    const size_t slash = path.find('/', nameStart);
    if (slash == std::string::npos || slash + 1 >= path.size()) {
        fail("node '" + raw.id + "': malformed package path '" + path +
             "' (§20.2)");
        return false;
    }
    const std::string name = path.substr(nameStart, slash - nameStart);
    const std::string rest = path.substr(slash + 1);
    if (!validPackageName(name)) {
        fail("node '" + raw.id + "': invalid package name '" + name +
             "' (§20.1)");
        return false;
    }
    if (!rest.starts_with("graphs/") && !rest.starts_with("shaders/") &&
        !rest.starts_with("assets/")) {
        fail("node '" + raw.id + "': package path '" + path +
             "' must name graphs/, shaders/ or assets/ content (§20.5)");
        return false;
    }
    std::string ref = name;
    if (auto it = mPins.find(name); it != mPins.end()) {
        ref += "@" + it->second;
        mPinsUsed.insert(name);
    }
    if (!checkPackage(raw.id, name, ref)) {
        return false;
    }
    path = std::string(kPrefix) + ref + "/" + rest;
    return true;
}

// Validate a package's manifest the first time the package is referenced
// (§20.1/§20.5). `ref` is the pin-canonicalized name.
bool Loader::checkPackage(const std::string& nodeId, const std::string& name,
                          const std::string& ref)
{
    if (mPackagesChecked.count(name)) {
        return true;
    }
    std::string text;
    if (!mReadAsset("packages/" + ref + "/manifest.json", text)) {
        const auto pin = mPins.find(name);
        fail("node '" + nodeId + "': package '" + name + "'" +
             (pin != mPins.end() ? " (pin '" + pin->second + "')" : "") +
             " is not installed (§20.2)");
        return false;
    }
    glz::generic doc;
    if (auto ec = glz::read_json(doc, text); ec || !doc.is_object()) {
        fail("package '" + name + "': manifest.json is unreadable (§20.1)");
        return false;
    }
    const auto& obj = doc.get_object();
    auto nIt = obj.find("name");
    if (nIt == obj.end() || !nIt->second.is_string() ||
        nIt->second.get_string() != name) {
        fail("package '" + name +
             "': manifest 'name' does not match the package directory "
             "(§20.1)");
        return false;
    }
    auto vIt = obj.find("version");
    if (vIt == obj.end() || !vIt->second.is_string() ||
        !validVersion(vIt->second.get_string())) {
        fail("package '" + name +
             "': manifest 'version' must be dotted integers (§20.1)");
        return false;
    }
    mPackagesChecked.insert(name);
    return true;
}

Loader::GraphDef* Loader::graphDef(const std::string& path)
{
    if (auto it = mGraphDefs.find(path); it != mGraphDefs.end()) {
        return it->second.get(); // null = a previous parse already failed
    }
    auto& slot = mGraphDefs[path]; // stays null on every failure path

    std::string text;
    if (!mReadAsset(path, text)) {
        fail("cannot open graph file '" + path + "' (§19.1)");
        return nullptr;
    }
    auto def = std::make_unique<GraphDef>();
    def->path = path;
    if (path.starts_with("packages/")) {
        // "packages/<name>[@pin]/" — the root the file's own paths
        // resolve against (§20.1).
        def->assetRoot = path.substr(0, path.find('/', 9) + 1);
    }
    def->doc = std::make_unique<glz::generic>();
    if (auto ec = glz::read_json(*def->doc, text); ec) {
        fail("graph file '" + path + "': " + glz::format_error(ec, text));
        return nullptr;
    }
    if (!def->doc->is_object()) {
        fail("graph file '" + path + "': top level must be an object");
        return nullptr;
    }
    const auto& top = def->doc->get_object();

    auto versionIt = top.find("version");
    if (versionIt == top.end() || !versionIt->second.is_number() ||
        (int)versionIt->second.get_number() != 1) {
        fail("graph file '" + path +
             "': unsupported or missing 'version' (expected 1)");
        return nullptr;
    }
    def->name = path; // editor label; falls back to the path
    if (auto nameIt = top.find("name");
        nameIt != top.end() && nameIt->second.is_string()) {
        def->name = nameIt->second.get_string();
    }

    auto nodesIt = top.find("nodes");
    if (nodesIt == top.end() || !nodesIt->second.is_array()) {
        fail("graph file '" + path + "': 'nodes' must be an array");
        return nullptr;
    }
    for (const auto& entry : nodesIt->second.get_array()) {
        RawNode raw;
        if (!parseRawNode(entry, raw, path + "\n", /*inGraphFile=*/true)) {
            mErrors.back() = "graph file '" + path + "': " + mErrors.back();
            return nullptr;
        }
        if (def->nodeIds.count(raw.id)) {
            fail("graph file '" + path + "': duplicate node id '" + raw.id +
                 "'");
            return nullptr;
        }
        def->nodeIds.insert(raw.id);
        def->nodes.push_back(std::move(raw));
    }

    // The interface (§19.2). One writer per inner port: an exported input
    // cannot bind a port the inner node sets itself, nor one another
    // exported input already binds.
    std::set<std::pair<std::string, std::string>> bound;
    const auto innerNode = [&](const std::string& id) -> RawNode* {
        for (auto& n : def->nodes) {
            if (n.id == id) {
                return &n;
            }
        }
        return nullptr;
    };
    if (auto inIt = top.find("inputs"); inIt != top.end()) {
        if (!inIt->second.is_object()) {
            fail("graph file '" + path + "': 'inputs' must be an object");
            return nullptr;
        }
        for (const auto& [name, decl] : inIt->second.get_object()) {
            const std::string where =
                "graph file '" + path + "' input '" + name + "'";
            if (!validId(name)) {
                fail(where + ": invalid name (§3)");
                return nullptr;
            }
            if (!decl.is_object()) {
                fail(where + ": declaration must be an object (§19.2)");
                return nullptr;
            }
            const auto& d = decl.get_object();
            GraphDef::In in;
            auto typeIt = d.find("type");
            std::string typeName = typeIt != d.end() &&
                                       typeIt->second.is_string()
                ? typeIt->second.get_string()
                : "";
            if (typeName == "texture") {
                in.type = ValueType::Texture;
            } else if (typeName == "event") {
                in.type = ValueType::Event;
            } else if (typeName == "buffer") {
                in.type = ValueType::Buffer;
            } else if (!parseValueType(typeName, in.type)) {
                fail(where + ": unknown 'type' (§19.2)");
                return nullptr;
            }
            if (auto defIt = d.find("default"); defIt != d.end()) {
                if (componentCount(in.type) == 0) {
                    fail(where + ": 'default' is for value types (§19.2)");
                    return nullptr;
                }
                if (!parseLiteral(defIt->second, in.def) ||
                    (in.def.type != in.type &&
                     in.def.type != ValueType::Scalar)) {
                    fail(where + ": invalid 'default' literal");
                    return nullptr;
                }
                in.hasDefault = true;
            }
            auto bindIt = d.find("bind");
            std::vector<const glz::generic*> binds;
            if (bindIt != d.end() && bindIt->second.is_array()) {
                for (const auto& b : bindIt->second.get_array()) {
                    binds.push_back(&b);
                }
            } else if (bindIt != d.end()) {
                binds.push_back(&bindIt->second);
            }
            if (binds.empty()) {
                fail(where + ": 'bind' is required (§19.2)");
                return nullptr;
            }
            for (const glz::generic* b : binds) {
                RawInput ref;
                std::string err;
                if (!b->is_string() || !parseRawInput(*b, ref, err) ||
                    ref.kind != RawInput::Kind::Conn || ref.conn.previous ||
                    ref.conn.port.empty()) {
                    fail(where + ": 'bind' entries must be \"@node.port\" "
                         "references (§19.2)");
                    return nullptr;
                }
                RawNode* target = innerNode(ref.conn.node);
                if (!target) {
                    fail(where + ": bind target '" + ref.conn.node +
                         "' is not a node in this file");
                    return nullptr;
                }
                if (target->inputs.count(ref.conn.port)) {
                    fail(where + ": '" + ref.conn.node + "." +
                         ref.conn.port +
                         "' is already set by the node itself (§19.2)");
                    return nullptr;
                }
                if (!bound.insert({ ref.conn.node, ref.conn.port }).second) {
                    fail(where + ": '" + ref.conn.node + "." +
                         ref.conn.port +
                         "' is already bound by another input (§19.2)");
                    return nullptr;
                }
                in.binds.push_back(ref.conn);
            }
            def->inputs.emplace_back(name, std::move(in));
        }
    }

    auto outIt = top.find("outputs");
    if (outIt == top.end() || !outIt->second.is_object() ||
        outIt->second.get_object().empty()) {
        fail("graph file '" + path +
             "': 'outputs' must be a non-empty object (§19.2)");
        return nullptr;
    }
    for (const auto& [name, ref] : outIt->second.get_object()) {
        const std::string where =
            "graph file '" + path + "' output '" + name + "'";
        if (!validId(name)) {
            fail(where + ": invalid name (§3)");
            return nullptr;
        }
        RawInput parsed;
        std::string err;
        if (!ref.is_string() || !parseRawInput(ref, parsed, err) ||
            parsed.kind != RawInput::Kind::Conn || parsed.conn.previous) {
            fail(where + ": must be an \"@node\" or \"@node.port\" "
                 "reference (§19.2)");
            return nullptr;
        }
        if (!def->nodeIds.count(parsed.conn.node)) {
            fail(where + ": '" + parsed.conn.node +
                 "' is not a node in this file");
            return nullptr;
        }
        def->outputs[name] = parsed.conn;
    }
    if (!def->outputs.count("result")) {
        fail("graph file '" + path +
             "': 'outputs' must include 'result' (§19.2)");
        return nullptr;
    }

    slot = std::move(def);
    return slot.get();
}

bool Loader::expandGraphs()
{
    for (int depth = 0;; ++depth) {
        if (std::none_of(mRawNodes.begin(), mRawNodes.end(),
                         [](const RawNode& n) { return n.type == "graph"; })) {
            return true;
        }
        if (depth >= 8) {
            fail("subgraph nesting deeper than 8 — instantiation cycle? "
                 "(§19.1)");
            return false;
        }

        // One level of expansion: instances become namespaced copies of
        // their file's nodes, and everything that referenced an instance
        // is rewritten to the mapped inner port afterwards.
        std::vector<RawNode> out;
        std::map<std::string, std::map<std::string, RawConn>> exports;
        struct Apply {
            std::string node; // namespaced inner id
            std::string port;
            RawInput value;
        };
        std::vector<Apply> applies;

        for (RawNode& raw : mRawNodes) {
            if (raw.type != "graph") {
                out.push_back(std::move(raw));
                continue;
            }
            const auto& obj = raw.json->get_object();
            auto gIt = obj.find("graph");
            if (gIt == obj.end() || !gIt->second.is_string()) {
                fail("node '" + raw.id +
                     "': 'graph' must be a graph file path (§19.3)");
                return false;
            }
            std::string gPath = gIt->second.get_string();
            if (gPath.find("..") != std::string::npos ||
                gPath.starts_with("/")) {
                fail("node '" + raw.id +
                     "': graph path escapes the project");
                return false;
            }
            if (!assetPath(raw, gPath, "'graph'")) {
                return false;
            }
            GraphDef* def = graphDef(gPath);
            if (!def) {
                return false;
            }
            const std::string prefix = raw.id + "/";

            // Instance bindings against the declared interface.
            for (const auto& [name, in] : raw.inputs) {
                const bool known = std::any_of(
                    def->inputs.begin(), def->inputs.end(),
                    [&](const auto& p) { return p.first == name; });
                if (!known) {
                    fail("node '" + raw.id + "': '" + name +
                         "' is not an input of graph '" + def->name +
                         "' (§19.2)");
                    return false;
                }
            }
            for (const auto& [name, decl] : def->inputs) {
                const auto vIt = raw.inputs.find(name);
                if (vIt == raw.inputs.end() && !decl.hasDefault) {
                    fail("node '" + raw.id + "': required input '" + name +
                         "' missing (§19.2)");
                    return false;
                }
                RawInput value;
                if (vIt != raw.inputs.end()) {
                    value = vIt->second;
                } else {
                    value.kind = RawInput::Kind::Literal;
                    value.literal = decl.def;
                }
                for (const RawConn& bind : decl.binds) {
                    applies.push_back({ prefix + bind.node, bind.port,
                                        value });
                }
            }

            // Namespaced copies of the file's nodes.
            for (const RawNode& inner : def->nodes) {
                RawNode copy;
                copy.id = prefix + inner.id;
                copy.type = inner.type;
                copy.json = inner.json; // GraphDef::doc keeps this alive
                copy.assetRoot = def->assetRoot;
                copy.inputs = inner.inputs;
                for (auto& [port, in] : copy.inputs) {
                    const auto ns = [&](RawConn& c) {
                        if (def->nodeIds.count(c.node)) {
                            c.node = prefix + c.node;
                        }
                    };
                    ns(in.conn);
                    for (auto& e : in.elements) {
                        ns(e.conn);
                    }
                }
                if (auto sIt =
                        mEmitterSpecs.find(def->path + "\n" + inner.id);
                    sIt != mEmitterSpecs.end()) {
                    mEmitterSpecs[copy.id] = sIt->second;
                }
                out.push_back(std::move(copy));
            }

            for (const auto& [name, conn] : def->outputs) {
                RawConn mapped = conn;
                mapped.node = prefix + mapped.node;
                exports[raw.id][name] = mapped;
            }
        }

        std::map<std::string, size_t> index;
        for (size_t i = 0; i < out.size(); ++i) {
            index[out[i].id] = i;
        }
        for (const Apply& a : applies) {
            RawNode& target = out[index.at(a.node)];
            target.inputs[a.port] = a.value;
            // §18.5.4's hidden feedback edge, for spawn arriving through
            // the interface (the file-parse injection only covers spawn
            // set on the inner node itself).
            if (a.port == "spawn" && target.type == "particles") {
                if (a.value.kind != RawInput::Kind::Conn ||
                    a.value.conn.previous) {
                    fail("node '" + a.node + "': 'spawn' must be a direct "
                         "(non-feedback) connection (§18.5.4)");
                    return false;
                }
                RawInput prev = a.value;
                prev.conn.previous = true;
                target.inputs["spawnPrev"] = std::move(prev);
            }
        }

        // Rewrite references to the instances this round expanded.
        for (RawNode& raw : out) {
            for (auto& [port, in] : raw.inputs) {
                const auto rewrite = [&](RawConn& c) -> bool {
                    auto eIt = exports.find(c.node);
                    if (eIt == exports.end()) {
                        return true;
                    }
                    const std::string want =
                        c.port.empty() ? "result" : c.port;
                    auto pIt = eIt->second.find(want);
                    if (pIt == eIt->second.end()) {
                        fail("node '" + raw.id + "' input '" + port +
                             "': graph instance '" + c.node +
                             "' has no output '" + want + "' (§19.2)");
                        return false;
                    }
                    const bool prev = c.previous;
                    c = pIt->second;
                    c.previous = prev;
                    return true;
                };
                bool ok = true;
                if (in.kind == RawInput::Kind::Conn) {
                    ok = rewrite(in.conn);
                }
                for (auto& e : in.elements) {
                    ok = ok && rewrite(e.conn);
                }
                if (!ok) {
                    return false;
                }
            }
        }

        mRawNodes = std::move(out);
        mRawIndex.clear();
        for (size_t i = 0; i < mRawNodes.size(); ++i) {
            mRawIndex[mRawNodes[i].id] = i;
        }
    }
}

bool Loader::topoSort()
{
    // Kahn over raw nodes; "time" is an implicit source and never queued.
    const size_t n = mRawNodes.size();
    std::vector<int> indegree(n, 0);
    std::vector<std::vector<size_t>> adjacency(n);

    for (size_t i = 0; i < n; ++i) {
        for (const auto& [port, in] : mRawNodes[i].inputs) {
            bool ok = true;
            forEachConn(in, [&](const RawConn& conn) {
                if (isImplicitNode(conn.node)) {
                    return;
                }
                auto it = mRawIndex.find(conn.node);
                if (it == mRawIndex.end()) {
                    fail("node '" + mRawNodes[i].id + "' input '" + port +
                         "': unknown node '" + conn.node + "'");
                    ok = false;
                    return;
                }
                // Feedback edges don't constrain evaluation order; the graph
                // only has to be acyclic without them (§10).
                if (conn.previous) {
                    return;
                }
                adjacency[it->second].push_back(i);
                ++indegree[i];
            });
            if (!ok) {
                return false;
            }
        }
    }

    std::vector<size_t> queue;
    for (size_t i = 0; i < n; ++i) {
        if (indegree[i] == 0) {
            queue.push_back(i);
        }
    }
    while (!queue.empty()) {
        const size_t i = queue.back();
        queue.pop_back();
        mTopoOrder.push_back(i);
        for (size_t next : adjacency[i]) {
            if (--indegree[next] == 0) {
                queue.push_back(next);
            }
        }
    }
    if (mTopoOrder.size() != n) {
        fail("graph contains a cycle");
        return false;
    }
    return true;
}

bool Loader::resolveInputType(const RawNode& raw, const RawInput& in, ValueType& out)
{
    switch (in.kind) {
    case RawInput::Kind::Literal:
        out = in.literal.type;
        return true;
    case RawInput::Kind::Param: {
        auto it = mParamIndex.find(in.param);
        if (it == mParamIndex.end()) {
            fail("node '" + raw.id + "': unknown parameter '$" + in.param + "'");
            return false;
        }
        out = mParams[it->second].type;
        return true;
    }
    case RawInput::Kind::Conn: {
        // Topo order guarantees non-feedback producers exist. A feedback
        // edge can point at a node not created yet — only reachable here
        // when it determines a polymorphic port's type, which we don't
        // support (the producer's own type may depend on this node).
        auto instIt = mInstances.find(in.conn.node);
        if (instIt == mInstances.end()) {
            fail("node '" + raw.id + "': cannot resolve a polymorphic type "
                 "through a feedback edge to '" + in.conn.node +
                 "' (wire the type-determining input without 'previous')");
            return false;
        }
        Node* src = instIt->second;
        const auto ports = producerPorts(in.conn.node);
        int idx = 0;
        if (!in.conn.port.empty()) {
            idx = -1;
            for (const auto& p : ports) {
                if (in.conn.port == p.name) {
                    idx = p.index;
                    break;
                }
            }
            if (idx < 0 || (size_t)idx >= src->outputs.size()) {
                fail("node '" + raw.id + "': node '" + in.conn.node +
                     "' has no output port '" + in.conn.port + "'");
                return false;
            }
        }
        out = src->outputs[idx].value.type;
        return true;
    }
    case RawInput::Kind::Array:
        fail("node '" + raw.id + "': array value is only valid on an array port");
        return false;
    }
    return false;
}

Node* Loader::makeNode(const RawNode& raw, std::vector<PortDef>& portsOut)
{
    if (raw.type == "wave") {
        std::string shape = "sine";
        if (auto it = raw.json->get_object().find("shape");
            it != raw.json->get_object().end() && it->second.is_string()) {
            shape = it->second.get_string();
        }
        WaveNode::Shape s;
        if (shape == "sine") s = WaveNode::Shape::Sine;
        else if (shape == "triangle") s = WaveNode::Shape::Triangle;
        else if (shape == "saw") s = WaveNode::Shape::Saw;
        else if (shape == "square") s = WaveNode::Shape::Square;
        else {
            fail("node '" + raw.id + "': unknown wave shape '" + shape + "'");
            return nullptr;
        }
        portsOut.assign(std::begin(kWaveInputs), std::end(kWaveInputs));
        return new WaveNode(s);
    }

    if (raw.type == "remap") {
        bool clamp = true;
        if (auto it = raw.json->get_object().find("clamp");
            it != raw.json->get_object().end() && it->second.is_boolean()) {
            clamp = it->second.get_boolean();
        }
        // Resolve T from the 'value' input (§4 polymorphic ports).
        auto valueIt = raw.inputs.find("value");
        if (valueIt == raw.inputs.end()) {
            fail("node '" + raw.id + "': required input 'value' missing");
            return nullptr;
        }
        ValueType t;
        if (!resolveInputType(raw, valueIt->second, t)) {
            return nullptr;
        }
        if (t == ValueType::Texture) {
            fail("node '" + raw.id + "': 'value' cannot be a texture");
            return nullptr;
        }
        portsOut.assign(std::begin(kRemapInputs), std::end(kRemapInputs));
        for (auto& p : portsOut) {
            p.type = t;
        }
        auto* node = new RemapNode(clamp);
        node->outputs[0].value.type = t;
        return node;
    }

    const bool isArithmetic = raw.type == "add" || raw.type == "multiply" ||
                              raw.type == "mix" || raw.type == "clamp";
    if (isArithmetic || raw.type == "damp") {
        // Resolve T from the first input (§4 polymorphic ports), like
        // remap; scalar-typed ports (mix.t, damp.time/halflife) stay
        // scalar.
        const char* first =
            raw.type == "clamp" || raw.type == "damp" ? "value" : "a";
        auto firstIt = raw.inputs.find(first);
        if (firstIt == raw.inputs.end()) {
            fail("node '" + raw.id + "': required input '" +
                 std::string(first) + "' missing");
            return nullptr;
        }
        ValueType t;
        if (!resolveInputType(raw, firstIt->second, t)) {
            return nullptr;
        }
        if (t == ValueType::Texture || t == ValueType::Event) {
            fail("node '" + raw.id + "': '" + std::string(first) +
                 "' must be a value");
            return nullptr;
        }
        if (raw.type == "add") {
            portsOut.assign(std::begin(kAddInputs), std::end(kAddInputs));
        } else if (raw.type == "multiply") {
            portsOut.assign(std::begin(kMultiplyInputs),
                            std::end(kMultiplyInputs));
        } else if (raw.type == "mix") {
            portsOut.assign(std::begin(kMixInputs), std::end(kMixInputs));
        } else if (raw.type == "clamp") {
            portsOut.assign(std::begin(kClampInputs),
                            std::end(kClampInputs));
        } else {
            portsOut.assign(std::begin(kDampInputs), std::end(kDampInputs));
        }
        for (auto& p : portsOut) {
            if (p.type == kPoly) {
                p.type = t;
            }
        }
        Node* node;
        if (raw.type == "add") {
            node = new ArithmeticNode(ArithmeticNode::Op::Add);
        } else if (raw.type == "multiply") {
            node = new ArithmeticNode(ArithmeticNode::Op::Multiply);
        } else if (raw.type == "mix") {
            node = new ArithmeticNode(ArithmeticNode::Op::Mix);
        } else if (raw.type == "clamp") {
            node = new ArithmeticNode(ArithmeticNode::Op::Clamp);
        } else {
            node = new DampNode();
        }
        node->outputs[0].value.type = t;
        return node;
    }

    if (raw.type == "noise") {
        portsOut.assign(std::begin(kNoiseInputs), std::end(kNoiseInputs));
        return new NoiseNode();
    }

    if (raw.type == "edge") {
        EdgeNode::Mode mode = EdgeNode::Mode::Rise;
        if (auto it = raw.json->get_object().find("mode");
            it != raw.json->get_object().end()) {
            const std::string m =
                it->second.is_string() ? it->second.get_string()
                                       : std::string();
            if (m == "rise") {
                mode = EdgeNode::Mode::Rise;
            } else if (m == "fall") {
                mode = EdgeNode::Mode::Fall;
            } else if (m == "both") {
                mode = EdgeNode::Mode::Both;
            } else {
                fail("node '" + raw.id + "': 'mode' must be \"rise\", "
                     "\"fall\" or \"both\" (§9.9)");
                return nullptr;
            }
        }
        portsOut.assign(std::begin(kEdgeInputs), std::end(kEdgeInputs));
        return new EdgeNode(mode);
    }

    if (raw.type == "combine") {
        // Result arity from the highest bound component (§9.9).
        int highest = 0;
        const char* names[] = { "x", "y", "z", "w" };
        for (int i = 0; i < 4; ++i) {
            if (raw.inputs.count(names[i])) {
                highest = i;
            }
        }
        if (highest == 0) {
            fail("node '" + raw.id +
                 "': combine needs more than 'x' (use the scalar directly)");
            return nullptr;
        }
        const ValueType t = highest == 1 ? ValueType::Vec2
                          : highest == 2 ? ValueType::Vec3
                                         : ValueType::Vec4;
        portsOut.assign(std::begin(kCombineInputs), std::end(kCombineInputs));
        return new CombineNode(t);
    }

    if (raw.type == "split") {
        auto valueIt = raw.inputs.find("value");
        if (valueIt == raw.inputs.end()) {
            fail("node '" + raw.id + "': required input 'value' missing");
            return nullptr;
        }
        ValueType t;
        if (!resolveInputType(raw, valueIt->second, t)) {
            return nullptr;
        }
        const int arity = componentCount(t);
        if (arity < 2) {
            fail("node '" + raw.id + "': split input must be a vector");
            return nullptr;
        }
        portsOut.assign(std::begin(kSplitInputs), std::end(kSplitInputs));
        portsOut[0].type = t;
        return new SplitNode(arity);
    }

    if (raw.type == "sequence") {
        const auto& obj = raw.json->get_object();
        auto durIt = obj.find("duration");
        if (durIt == obj.end() || !durIt->second.is_number() ||
            durIt->second.get_number() <= 0.0) {
            fail("node '" + raw.id + "': sequence needs a positive 'duration'");
            return nullptr;
        }
        const double duration = durIt->second.get_number();
        bool loop = true;
        if (auto loopIt = obj.find("loop"); loopIt != obj.end()) {
            if (!loopIt->second.is_boolean()) {
                fail("node '" + raw.id + "': 'loop' must be a boolean");
                return nullptr;
            }
            loop = loopIt->second.get_boolean();
        }
        auto tracksIt = obj.find("tracks");
        if (tracksIt == obj.end() || !tracksIt->second.is_array() ||
            tracksIt->second.get_array().empty()) {
            fail("node '" + raw.id + "': sequence needs a non-empty 'tracks' array");
            return nullptr;
        }

        std::vector<SequenceNode::Track> tracks;
        for (const auto& entry : tracksIt->second.get_array()) {
            if (!entry.is_object()) {
                fail("node '" + raw.id + "': each track must be an object");
                return nullptr;
            }
            const auto& tobj = entry.get_object();
            SequenceNode::Track track;

            auto nameIt = tobj.find("name");
            if (nameIt == tobj.end() || !nameIt->second.is_string() ||
                !validId(nameIt->second.get_string())) {
                fail("node '" + raw.id + "': track needs a valid 'name'");
                return nullptr;
            }
            track.name = nameIt->second.get_string();
            for (const auto& existing : tracks) {
                if (existing.name == track.name) {
                    fail("node '" + raw.id + "': duplicate track name '" +
                         track.name + "'");
                    return nullptr;
                }
            }

            auto kindIt = tobj.find("kind");
            if (kindIt == tobj.end() || !kindIt->second.is_string()) {
                fail("node '" + raw.id + "' track '" + track.name +
                     "': needs a string 'kind'");
                return nullptr;
            }
            if (const std::string& kind = kindIt->second.get_string();
                kind == "event") {
                track.event = true;
            } else if (kind != "value") {
                fail("node '" + raw.id + "' track '" + track.name +
                     "': unknown kind '" + kind + "'");
                return nullptr;
            }

            if (track.event) {
                // §9.9 event tracks: 'fires' cues, ascending, [0, duration).
                for (const auto& [key, value] : tobj) {
                    if (key != "name" && key != "kind" && key != "fires") {
                        warn("node '" + raw.id + "' track '" + track.name +
                             "': unknown field '" + key + "' ignored");
                    }
                }
                auto firesIt = tobj.find("fires");
                if (firesIt == tobj.end() || !firesIt->second.is_array() ||
                    firesIt->second.get_array().empty()) {
                    fail("node '" + raw.id + "' track '" + track.name +
                         "': needs a non-empty 'fires' array");
                    return nullptr;
                }
                for (const auto& cue : firesIt->second.get_array()) {
                    if (!cue.is_number()) {
                        fail("node '" + raw.id + "' track '" + track.name +
                             "': 'fires' entries must be numbers");
                        return nullptr;
                    }
                    const double t = cue.get_number();
                    if (t < 0.0 || t >= duration) {
                        fail("node '" + raw.id + "' track '" + track.name +
                             "': fire time outside [0, duration)");
                        return nullptr;
                    }
                    if (!track.fires.empty() && t <= track.fires.back()) {
                        fail("node '" + raw.id + "' track '" + track.name +
                             "': fire times must be ascending");
                        return nullptr;
                    }
                    track.fires.push_back(t);
                }
                tracks.push_back(std::move(track));
                continue;
            }

            auto typeIt = tobj.find("type");
            if (typeIt == tobj.end() || !typeIt->second.is_string() ||
                !parseValueType(typeIt->second.get_string(), track.type)) {
                fail("node '" + raw.id + "' track '" + track.name +
                     "': needs a 'type' of scalar/vec2/vec3/vec4");
                return nullptr;
            }

            if (auto interpIt = tobj.find("interpolate"); interpIt != tobj.end()) {
                const std::string interp = interpIt->second.is_string()
                    ? interpIt->second.get_string() : std::string();
                if (interp == "hold") track.interpolate = SequenceNode::Interpolate::Hold;
                else if (interp == "linear") track.interpolate = SequenceNode::Interpolate::Linear;
                else if (interp == "smooth") track.interpolate = SequenceNode::Interpolate::Smooth;
                else {
                    fail("node '" + raw.id + "' track '" + track.name +
                         "': unknown interpolate '" + interp + "'");
                    return nullptr;
                }
            }

            for (const auto& [key, value] : tobj) {
                if (key != "name" && key != "kind" && key != "type" &&
                    key != "interpolate" && key != "keys") {
                    warn("node '" + raw.id + "' track '" + track.name +
                         "': unknown field '" + key + "' ignored");
                }
            }

            auto keysIt = tobj.find("keys");
            if (keysIt == tobj.end() || !keysIt->second.is_array() ||
                keysIt->second.get_array().empty()) {
                fail("node '" + raw.id + "' track '" + track.name +
                     "': needs a non-empty 'keys' array");
                return nullptr;
            }
            for (const auto& keyEntry : keysIt->second.get_array()) {
                if (!keyEntry.is_object()) {
                    fail("node '" + raw.id + "' track '" + track.name +
                         "': each key must be an object");
                    return nullptr;
                }
                const auto& kobj = keyEntry.get_object();
                SequenceNode::Key key;
                auto tIt = kobj.find("t");
                if (tIt == kobj.end() || !tIt->second.is_number()) {
                    fail("node '" + raw.id + "' track '" + track.name +
                         "': key needs a number 't'");
                    return nullptr;
                }
                key.t = tIt->second.get_number();
                if (key.t < 0.0 || key.t > duration) {
                    fail("node '" + raw.id + "' track '" + track.name +
                         "': key t outside [0, duration]");
                    return nullptr;
                }
                if (!track.keys.empty() && key.t <= track.keys.back().t) {
                    fail("node '" + raw.id + "' track '" + track.name +
                         "': key times must be strictly ascending");
                    return nullptr;
                }
                auto vIt = kobj.find("value");
                if (vIt == kobj.end() || !parseLiteral(vIt->second, key.value)) {
                    fail("node '" + raw.id + "' track '" + track.name +
                         "': key needs a literal 'value'");
                    return nullptr;
                }
                if (key.value.type != track.type) {
                    fail("node '" + raw.id + "' track '" + track.name +
                         "': key value does not match track type");
                    return nullptr;
                }
                track.keys.push_back(std::move(key));
            }
            tracks.push_back(std::move(track));
        }

        // Cue-crossing semantics assume a monotonic time input (§9.9);
        // value tracks are pure and tolerate any signal.
        if (std::any_of(tracks.begin(), tracks.end(),
                        [](const SequenceNode::Track& t) { return t.event; })) {
            auto timeIt = raw.inputs.find("time");
            if (timeIt == raw.inputs.end() ||
                timeIt->second.kind != RawInput::Kind::Conn ||
                timeIt->second.conn.node != "time") {
                warn("node '" + raw.id + "': event tracks assume a "
                     "monotonic 'time' input; wire it from @time");
            }
        }

        portsOut.assign(std::begin(kSequenceInputs), std::end(kSequenceInputs));
        auto* node = new SequenceNode(duration, loop, std::move(tracks));
        mSequences.push_back(node);
        return node;
    }

    if (raw.type == "shader") {
        const auto& obj = raw.json->get_object();
        auto pathIt = obj.find("shader");
        if (pathIt == obj.end() || !pathIt->second.is_string()) {
            fail("node '" + raw.id + "': shader node needs a string 'shader' path");
            return nullptr;
        }
        std::string path = pathIt->second.get_string();
        if (path.find("..") != std::string::npos || path.starts_with("/")) {
            fail("node '" + raw.id + "': shader path escapes the project");
            return nullptr;
        }
        if (!assetPath(raw, path, "'shader'")) {
            return nullptr;
        }
        std::string source;
        if (!mReadAsset(path, source)) {
            fail("node '" + raw.id + "': cannot read shader '" + path + "'");
            return nullptr;
        }
        WgslInterface iface;
        std::string err;
        if (!WgslInterface::parse(source, iface, err)) {
            fail("node '" + raw.id + "': " + path + ": " + err);
            return nullptr;
        }
        if (iface.isCompute || !iface.storageBuffers.empty() ||
            !iface.storageTextures.empty()) {
            fail("node '" + raw.id + "': " + path +
                 " declares a compute interface; use a 'compute' node (§18.2)");
            return nullptr;
        }
        uint32_t width = 0, height = 0;
        if (auto sizeIt = obj.find("size"); sizeIt != obj.end()) {
            if (sizeIt->second.is_array()) {
                Value size;
                if (!parseLiteral(sizeIt->second, size) || size.type != ValueType::Vec2) {
                    fail("node '" + raw.id + "': 'size' must be [w, h] or \"auto\"");
                    return nullptr;
                }
                width = (uint32_t)size.v[0];
                height = (uint32_t)size.v[1];
            } else if (!sizeIt->second.is_string() ||
                       sizeIt->second.get_string() != "auto") {
                fail("node '" + raw.id + "': 'size' must be [w, h] or \"auto\"");
                return nullptr;
            }
        }
        // Ports: uniform fields then textures, all required (§9.10).
        portsOut.clear();
        for (const auto& f : iface.fields) {
            portsOut.push_back({ f.name, f.type, true });
        }
        for (const auto& t : iface.textures) {
            portsOut.push_back({ t.name, ValueType::Texture, true });
        }
        return new ShaderNode(std::move(source), std::move(iface), width, height);
    }

    if (raw.type == "compute") {
        const auto& obj = raw.json->get_object();
        auto pathIt = obj.find("shader");
        if (pathIt == obj.end() || !pathIt->second.is_string()) {
            fail("node '" + raw.id + "': compute node needs a string 'shader' path");
            return nullptr;
        }
        std::string path = pathIt->second.get_string();
        if (path.find("..") != std::string::npos || path.starts_with("/")) {
            fail("node '" + raw.id + "': shader path escapes the project");
            return nullptr;
        }
        if (!assetPath(raw, path, "'shader'")) {
            return nullptr;
        }
        std::string source;
        if (!mReadAsset(path, source)) {
            fail("node '" + raw.id + "': cannot read shader '" + path + "'");
            return nullptr;
        }
        WgslInterface iface;
        std::string err;
        if (!WgslInterface::parse(source, iface, err)) {
            fail("node '" + raw.id + "': " + path + ": " + err);
            return nullptr;
        }
        if (!iface.isCompute) {
            fail("node '" + raw.id + "': " + path +
                 " has no @compute entry point (§18.2)");
            return nullptr;
        }
        size_t rwCount = 0;
        for (const auto& buf : iface.storageBuffers) {
            rwCount += buf.readWrite ? 1 : 0;
        }
        if (rwCount == 0 && iface.storageTextures.empty()) {
            fail("node '" + raw.id + "': " + path +
                 " has no storage outputs (nothing to produce)");
            return nullptr;
        }

        // capacity (§18.2): required when owning buffers. A number applies
        // to every read_write output; the object form maps port -> count.
        std::vector<uint32_t> capacities;
        auto capacityFor = [&](const std::string& port,
                               uint32_t& out) -> bool {
            auto capIt = obj.find("capacity");
            if (capIt == obj.end()) {
                fail("node '" + raw.id +
                     "': 'capacity' is required for buffer outputs (§18.2)");
                return false;
            }
            const glz::generic* entry = &capIt->second;
            if (capIt->second.is_object()) {
                const auto& map = capIt->second.get_object();
                auto it = map.find(port);
                if (it == map.end()) {
                    fail("node '" + raw.id + "': 'capacity' has no entry for '" +
                         port + "'");
                    return false;
                }
                entry = &it->second;
            }
            if (!entry->is_number() ||
                entry->get_number() != (double)(uint32_t)entry->get_number() ||
                entry->get_number() <= 0) {
                fail("node '" + raw.id +
                     "': 'capacity' must be a positive integer");
                return false;
            }
            out = (uint32_t)entry->get_number();
            return true;
        };
        for (const auto& buf : iface.storageBuffers) {
            if (!buf.readWrite) {
                continue;
            }
            uint32_t capacity = 0;
            if (!capacityFor(buf.name, capacity)) {
                return nullptr;
            }
            capacities.push_back(capacity);
        }

        std::array<uint32_t, 3> dispatch = { 0, 0, 0 }; // auto
        if (auto dIt = obj.find("dispatch"); dIt != obj.end()) {
            const bool isAuto =
                dIt->second.is_string() && dIt->second.get_string() == "auto";
            if (!isAuto) {
                if (!dIt->second.is_array() ||
                    dIt->second.get_array().empty() ||
                    dIt->second.get_array().size() > 3) {
                    fail("node '" + raw.id +
                         "': 'dispatch' must be [x], [x,y], [x,y,z], or \"auto\"");
                    return nullptr;
                }
                const auto& arr = dIt->second.get_array();
                dispatch = { 1, 1, 1 };
                for (size_t i = 0; i < arr.size(); ++i) {
                    if (!arr[i].is_number() || arr[i].get_number() <= 0) {
                        fail("node '" + raw.id +
                             "': 'dispatch' counts must be positive");
                        return nullptr;
                    }
                    dispatch[i] = (uint32_t)arr[i].get_number();
                }
            }
        }

        // Ports: uniform fields, sampled textures, then read-only storage
        // buffers; all required (§9.10's rule extends to compute).
        portsOut.clear();
        for (const auto& f : iface.fields) {
            portsOut.push_back({ f.name, f.type, true });
        }
        for (const auto& t : iface.textures) {
            portsOut.push_back({ t.name, ValueType::Texture, true });
        }
        for (const auto& buf : iface.storageBuffers) {
            if (!buf.readWrite) {
                portsOut.push_back({ buf.name, ValueType::Buffer, true, {},
                                     false, buf.elementStride });
            }
        }
        return new ComputeNode(std::move(source), std::move(iface),
                               std::move(capacities), dispatch);
    }

    if (raw.type == "image") {
        const auto& obj = raw.json->get_object();
        auto srcIt = obj.find("src");
        if (srcIt == obj.end() || !srcIt->second.is_string()) {
            fail("node '" + raw.id + "': image node needs a string 'src' path");
            return nullptr;
        }
        std::string path = srcIt->second.get_string();
        if (path.find("..") != std::string::npos || path.starts_with("/")) {
            fail("node '" + raw.id + "': image path escapes the project");
            return nullptr;
        }
        if (!assetPath(raw, path, "'src'")) {
            return nullptr;
        }
        std::string bytes;
        if (!mReadAsset(path, bytes)) {
            fail("node '" + raw.id + "': cannot read image '" + path + "'");
            return nullptr;
        }
        std::string err;
        ImageNode* node = ImageNode::decode(bytes, err);
        if (!node) {
            fail("node '" + raw.id + "': cannot decode '" + path + "': " + err);
            return nullptr;
        }
        portsOut.clear();
        return node;
    }

    if (raw.type == "video") {
        const auto& obj = raw.json->get_object();
        auto srcIt = obj.find("src");
        if (srcIt == obj.end() || !srcIt->second.is_string()) {
            fail("node '" + raw.id + "': video node needs a string 'src' path");
            return nullptr;
        }
        std::string path = srcIt->second.get_string();
        if (path.find("..") != std::string::npos || path.starts_with("/")) {
            fail("node '" + raw.id + "': video path escapes the project");
            return nullptr;
        }
        if (!assetPath(raw, path, "'src'")) {
            return nullptr;
        }
        bool loop = true;
        if (auto loopIt = obj.find("loop"); loopIt != obj.end()) {
            if (!loopIt->second.is_boolean()) {
                fail("node '" + raw.id + "': 'loop' must be a boolean");
                return nullptr;
            }
            loop = loopIt->second.get_boolean();
        }
        if (!mVideoFactory) {
            fail("node '" + raw.id + "': video nodes are not supported by this runtime");
            return nullptr;
        }
        std::string err;
        auto decoder = mVideoFactory(path, loop, err);
        if (!decoder) {
            fail("node '" + raw.id + "': cannot open video '" + path + "': " + err);
            return nullptr;
        }
        portsOut.assign(std::begin(kVideoInputs), std::end(kVideoInputs));
        return new VideoNode(std::move(decoder));
    }

    if (raw.type == "particles") {
        const auto& obj = raw.json->get_object();
        uint32_t capacity = 1000;
        if (auto capIt = obj.find("capacity"); capIt != obj.end()) {
            if (!capIt->second.is_number() || capIt->second.get_number() <= 0 ||
                capIt->second.get_number() !=
                    (double)(uint32_t)capIt->second.get_number()) {
                fail("node '" + raw.id +
                     "': 'capacity' must be a positive integer");
                return nullptr;
            }
            capacity = (uint32_t)capIt->second.get_number();
        }
        // §18.5.4 sub-emitter mode: capacity derives from the spawn source
        // so every death has a statically assigned brood.
        uint32_t spawnCount = 0;
        if (raw.inputs.count("spawn")) {
            spawnCount = 4;
            if (auto scIt = obj.find("spawnCount"); scIt != obj.end()) {
                if (!scIt->second.is_number() ||
                    scIt->second.get_number() < 1 ||
                    scIt->second.get_number() !=
                        (double)(uint32_t)scIt->second.get_number()) {
                    fail("node '" + raw.id +
                         "': 'spawnCount' must be a positive integer");
                    return nullptr;
                }
                spawnCount = (uint32_t)scIt->second.get_number();
            }
            if (mEmitterSpecs.count(raw.id)) {
                fail("node '" + raw.id + "': 'emitters' does not combine "
                     "with 'spawn' (§18.5.4)");
                return nullptr;
            }
            const RawConn& conn = raw.inputs.at("spawn").conn;
            Node* src = mInstances.count(conn.node) ? mInstances[conn.node]
                                                    : nullptr;
            int srcPort = 0;
            if (src && !conn.port.empty()) {
                srcPort = -1;
                for (const auto& p : producerPorts(conn.node)) {
                    if (conn.port == p.name) {
                        srcPort = p.index;
                        break;
                    }
                }
            }
            if (!src || srcPort < 0 ||
                (size_t)srcPort >= src->outputs.size() ||
                src->outputs[srcPort].value.type != ValueType::Buffer) {
                fail("node '" + raw.id +
                     "': 'spawn' must connect to a buffer output (§18.5.4)");
                return nullptr;
            }
            const uint64_t derived =
                (uint64_t)src->outputs[srcPort].value.bufCapacity *
                spawnCount;
            if (derived == 0 || derived > (1u << 22)) {
                fail("node '" + raw.id + "': derived capacity " +
                     std::to_string(derived) +
                     " exceeds the pool limit (§18.5.7)");
                return nullptr;
            }
            if (obj.count("capacity")) {
                warn("node '" + raw.id + "': 'capacity' is ignored — "
                     "sub-emitters derive it from the spawn source "
                     "(§18.5.4)");
            }
            if (obj.count("emitter")) {
                warn("node '" + raw.id +
                     "': 'emitter' does not apply with 'spawn' (§18.5.4)");
            }
            for (const char* dead :
                 { "rate", "burst", "burstCount", "origin", "extent",
                   "delay", "duration" }) {
                if (raw.inputs.count(dead)) {
                    warn("node '" + raw.id + "': input '" + dead +
                         "' does not apply with 'spawn' (§18.5.4)");
                }
            }
            capacity = (uint32_t)derived;
        } else if (obj.count("spawnCount")) {
            warn("node '" + raw.id +
                 "': 'spawnCount' without 'spawn' is ignored (§18.5.4)");
        }
        ParticlesNode::Emitter emitter = ParticlesNode::Emitter::Point;
        if (auto emitIt = obj.find("emitter"); emitIt != obj.end()) {
            if (!emitIt->second.is_string() ||
                !parseEmitterShape(emitIt->second.get_string(), emitter)) {
                fail("node '" + raw.id +
                     "': 'emitter' must be \"point\", \"box\", or \"disc\"");
                return nullptr;
            }
        }
        // The emission window assumes a per-frame delta on 'time' (§18.3);
        // an unbounded signal would emit rate×seconds every frame.
        if (auto inputsIt = raw.inputs.find("time");
            inputsIt != raw.inputs.end() &&
            inputsIt->second.kind == RawInput::Kind::Conn &&
            !(inputsIt->second.conn.node == "time" &&
              inputsIt->second.conn.port == "delta")) {
            warn("node '" + raw.id + "': 'time' should be wired from "
                 "@time.delta (the per-frame simulation step)");
        }
        portsOut.assign(std::begin(kParticlesInputs),
                        std::end(kParticlesInputs));
        // §18.5.3: entries append one port per emitter field; an entry's
        // unset fields stay unbound (the node falls back to the base port).
        std::vector<ParticlesNode::Emitter> kinds;
        std::vector<uint32_t> masks;
        if (auto specIt = mEmitterSpecs.find(raw.id);
            specIt != mEmitterSpecs.end()) {
            const EmitterSpec& spec = specIt->second;
            for (size_t e = 0; e < spec.kinds.size(); ++e) {
                kinds.push_back(spec.kinds[e] < 0
                                    ? emitter
                                    : (ParticlesNode::Emitter)spec.kinds[e]);
                for (const auto& field : kEmitterFields) {
                    portsOut.push_back(
                        { "emitters[" + std::to_string(e) + "]." + field.name,
                          field.type, false });
                }
            }
            masks = spec.masks;
        } else {
            kinds = { emitter };
            masks = { 0 };
        }
        auto* node = new ParticlesNode(capacity, std::move(kinds),
                                       std::move(masks), spawnCount);
        node->setVelocityBox(raw.inputs.count("velocityMin") ||
                             raw.inputs.count("velocityMax"));
        node->setTintBox(raw.inputs.count("tintVaryMax"));
        return node;
    }

    if (raw.type == "sprites") {
        const auto& obj = raw.json->get_object();
        SpritesNode::Blend blend = SpritesNode::Blend::Add;
        if (auto blendIt = obj.find("blend"); blendIt != obj.end()) {
            const std::string mode =
                blendIt->second.is_string() ? blendIt->second.get_string() : "";
            if (mode == "add") {
                blend = SpritesNode::Blend::Add;
            } else if (mode == "over") {
                blend = SpritesNode::Blend::Over;
            } else {
                fail("node '" + raw.id +
                     "': 'blend' must be \"add\" or \"over\"");
                return nullptr;
            }
        }
        // §18.5.5 spritesheet grid.
        uint32_t sheetCols = 0, sheetRows = 0;
        if (auto sheetIt = obj.find("sheet"); sheetIt != obj.end()) {
            const auto bad = [&] {
                fail("node '" + raw.id + "': 'sheet' must be [cols, rows], "
                     "integers >= 1 (§18.5.5)");
                return nullptr;
            };
            if (!sheetIt->second.is_array() ||
                sheetIt->second.get_array().size() != 2) {
                return bad();
            }
            for (const auto& v : sheetIt->second.get_array()) {
                if (!v.is_number() || v.get_number() < 1 ||
                    v.get_number() != (double)(uint32_t)v.get_number()) {
                    return bad();
                }
            }
            sheetCols = (uint32_t)sheetIt->second.get_array()[0].get_number();
            sheetRows = (uint32_t)sheetIt->second.get_array()[1].get_number();
            if (!raw.inputs.count("texture")) {
                fail("node '" + raw.id +
                     "': 'sheet' requires the 'texture' input (§18.5.5)");
                return nullptr;
            }
        }
        portsOut.assign(std::begin(kSpritesInputs), std::end(kSpritesInputs));
        return new SpritesNode(blend, sheetCols, sheetRows);
    }

    if (raw.type == "trails") {
        const auto& obj = raw.json->get_object();
        TrailsNode::Blend blend = TrailsNode::Blend::Add;
        if (auto blendIt = obj.find("blend"); blendIt != obj.end()) {
            const std::string mode =
                blendIt->second.is_string() ? blendIt->second.get_string() : "";
            if (mode == "add") {
                blend = TrailsNode::Blend::Add;
            } else if (mode == "over") {
                blend = TrailsNode::Blend::Over;
            } else {
                fail("node '" + raw.id +
                     "': 'blend' must be \"add\" or \"over\"");
                return nullptr;
            }
        }
        uint32_t length = 16;
        if (auto lenIt = obj.find("length"); lenIt != obj.end()) {
            if (!lenIt->second.is_number() ||
                lenIt->second.get_number() < 2 ||
                lenIt->second.get_number() > 64 ||
                lenIt->second.get_number() !=
                    (double)(uint32_t)lenIt->second.get_number()) {
                fail("node '" + raw.id +
                     "': 'length' must be an integer in 2-64 (§18.5.6)");
                return nullptr;
            }
            length = (uint32_t)lenIt->second.get_number();
        }
        portsOut.assign(std::begin(kTrailsInputs), std::end(kTrailsInputs));
        return new TrailsNode(blend, length);
    }

    if (raw.type == "transform") {
        portsOut.assign(std::begin(kTransformInputs), std::end(kTransformInputs));
        return new TransformNode();
    }

    if (raw.type == "fit") {
        FitNode::Mode mode = FitNode::Mode::Cover;
        const auto& obj = raw.json->get_object();
        if (auto modeIt = obj.find("mode"); modeIt != obj.end()) {
            const std::string m = modeIt->second.is_string()
                ? modeIt->second.get_string() : std::string();
            if (m == "cover") {
                mode = FitNode::Mode::Cover;
            } else if (m == "contain") {
                mode = FitNode::Mode::Contain;
            } else if (m == "stretch") {
                mode = FitNode::Mode::Stretch;
            } else {
                fail("node '" + raw.id + "': 'mode' must be \"cover\", "
                     "\"contain\" or \"stretch\" (§17.1)");
                return nullptr;
            }
        }
        portsOut.assign(std::begin(kFitInputs), std::end(kFitInputs));
        return new FitNode(mode);
    }

    if (raw.type == "compositor") {
        std::vector<CompositorNode::Blend> blends;
        const auto& obj = raw.json->get_object();
        if (auto blendIt = obj.find("blend"); blendIt != obj.end()) {
            if (!blendIt->second.is_array()) {
                fail("node '" + raw.id +
                     "': 'blend' must be an array of modes, one per layer "
                     "(§9.5)");
                return nullptr;
            }
            size_t layerCount = 0;
            if (auto it = raw.inputs.find("layers");
                it != raw.inputs.end()) {
                layerCount = it->second.elements.size();
            }
            const auto& arr = blendIt->second.get_array();
            if (arr.size() > layerCount) {
                fail("node '" + raw.id +
                     "': 'blend' has more entries than layers (§9.5)");
                return nullptr;
            }
            for (const auto& entry : arr) {
                const std::string m =
                    entry.is_string() ? entry.get_string() : std::string();
                if (m == "over") {
                    blends.push_back(CompositorNode::Blend::Over);
                } else if (m == "add") {
                    blends.push_back(CompositorNode::Blend::Add);
                } else if (m == "multiply") {
                    blends.push_back(CompositorNode::Blend::Multiply);
                } else if (m == "screen") {
                    blends.push_back(CompositorNode::Blend::Screen);
                } else {
                    fail("node '" + raw.id + "': blend mode must be "
                         "\"over\", \"add\", \"multiply\" or \"screen\" "
                         "(§9.5)");
                    return nullptr;
                }
            }
        }
        portsOut.assign(std::begin(kCompositorInputs), std::end(kCompositorInputs));
        return new CompositorNode(std::move(blends));
    }

    if (raw.type == "output") {
        if (mOutput) {
            fail("multiple output nodes");
            return nullptr;
        }
        portsOut.assign(std::begin(kOutputInputs), std::end(kOutputInputs));
        return new OutputNode();
    }

    fail("node '" + raw.id + "': unknown node type '" + raw.type + "'");
    return nullptr;
}

bool Loader::bindInputs(const RawNode& raw, Node* node,
                        const std::vector<PortDef>& ports)
{
    // Array-valued port (compositor 'layers'): the node's inputs are the
    // array's elements, one Node::Input per layer.
    if (ports.size() == 1 && ports[0].array) {
        const PortDef& port = ports[0];
        for (const auto& [name, in] : raw.inputs) {
            if (name != port.name) {
                fail("node '" + raw.id + "': unknown input port '" + name + "'");
                return false;
            }
        }
        auto it = raw.inputs.find(port.name);
        if (it == raw.inputs.end()) {
            fail("node '" + raw.id + "': required input '" + port.name +
                 "' missing");
            return false;
        }
        if (it->second.kind != RawInput::Kind::Array ||
            it->second.elements.empty()) {
            fail("node '" + raw.id + "' input '" + port.name +
                 "': must be a non-empty array of connections");
            return false;
        }
        node->inputs.resize(it->second.elements.size());
        for (size_t i = 0; i < it->second.elements.size(); ++i) {
            const RawInput& element = it->second.elements[i];
            if (element.conn.previous) {
                node->inputs[i].type = port.type;
                mDeferredBinds.push_back({ node, i, raw.id,
                                           port.name + "[" + std::to_string(i) + "]",
                                           element.conn, port.type });
                continue;
            }
            ValueType srcType;
            if (!resolveInputType(raw, element, srcType)) {
                return false;
            }
            if (srcType != port.type) {
                fail("node '" + raw.id + "' input '" + port.name + "[" +
                     std::to_string(i) + "]': type mismatch (" +
                     valueTypeName(srcType) + " -> " + valueTypeName(port.type) +
                     ")");
                return false;
            }
            Node::Input& in = node->inputs[i];
            in.type = port.type;
            in.srcNode = mInstances[element.conn.node];
            in.srcPort = 0;
            if (!element.conn.port.empty()) {
                const auto srcPorts = producerPorts(element.conn.node);
                for (const auto& p : srcPorts) {
                    if (element.conn.port == p.name) {
                        in.srcPort = p.index;
                        break;
                    }
                }
            }
        }
        return true;
    }

    node->inputs.resize(ports.size());

    for (const auto& [portName, rawIn] : raw.inputs) {
        int portIdx = -1;
        for (size_t i = 0; i < ports.size(); ++i) {
            if (portName == ports[i].name) {
                portIdx = (int)i;
                break;
            }
        }
        if (portIdx < 0) {
            fail("node '" + raw.id + "': unknown input port '" + portName + "'");
            return false;
        }
        Node::Input& in = node->inputs[portIdx];
        const ValueType portType = ports[portIdx].type;
        in.type = portType;

        if (rawIn.kind == RawInput::Kind::Conn && rawIn.conn.previous) {
            mDeferredBinds.push_back({ node, (size_t)portIdx, raw.id, portName,
                                       rawIn.conn, portType,
                                       ports[portIdx].stride });
            continue;
        }

        ValueType srcType;
        if (!resolveInputType(raw, rawIn, srcType)) {
            return false;
        }
        if (srcType != portType) {
            if (srcType == ValueType::Scalar &&
                portType != ValueType::Texture &&
                portType != ValueType::Event &&
                portType != ValueType::Buffer) {
                in.splat = true; // §4: scalar splats to vecN
            } else {
                fail("node '" + raw.id + "' input '" + portName + "': type mismatch (" +
                     valueTypeName(srcType) + " -> " + valueTypeName(portType) + ")");
                return false;
            }
        }
        if (portType == ValueType::Buffer &&
            rawIn.kind == RawInput::Kind::Conn) {
            // §18.1: buffer connections validate on element stride.
            Node* src = mInstances[rawIn.conn.node];
            int srcPort = 0;
            if (!rawIn.conn.port.empty()) {
                for (const auto& p : producerPorts(rawIn.conn.node)) {
                    if (rawIn.conn.port == p.name) {
                        srcPort = p.index;
                        break;
                    }
                }
            }
            const uint32_t srcStride = src->outputs[srcPort].value.bufStride;
            if (srcStride != ports[portIdx].stride) {
                fail("node '" + raw.id + "' input '" + portName +
                     "': buffer element stride mismatch (" +
                     std::to_string(srcStride) + " -> " +
                     std::to_string(ports[portIdx].stride) + " bytes)");
                return false;
            }
        }

        switch (rawIn.kind) {
        case RawInput::Kind::Literal:
            in.constant = rawIn.literal;
            break;
        case RawInput::Kind::Param: {
            const int idx = mParamIndex[rawIn.param];
            in.paramIndex = idx;
            in.constant = mParams[idx].def;
            break;
        }
        case RawInput::Kind::Conn: {
            Node* src = mInstances[rawIn.conn.node];
            in.srcNode = src;
            in.srcPort = 0;
            if (!rawIn.conn.port.empty()) {
                const auto srcPorts = producerPorts(rawIn.conn.node);
                for (const auto& p : srcPorts) {
                    if (rawIn.conn.port == p.name) {
                        in.srcPort = p.index;
                        break;
                    }
                }
            }
            break;
        }
        }
    }

    // Defaults / required check for unbound ports.
    for (size_t i = 0; i < ports.size(); ++i) {
        Node::Input& in = node->inputs[i];
        if (in.srcNode || in.paramIndex >= 0 ||
            raw.inputs.count(ports[i].name)) {
            continue;
        }
        if (ports[i].required) {
            fail("node '" + raw.id + "': required input '" + ports[i].name +
                 "' missing");
            return false;
        }
        in.type = ports[i].type;
        in.constant.type = ports[i].type;
        in.constant.v = ports[i].def;
    }
    return true;
}

// Second pass: feedback edges, bound once every node exists (§10). Marks
// texture producers so Scene::render maintains their history copies.
bool Loader::bindDeferred()
{
    for (const auto& d : mDeferredBinds) {
        Node* src = mInstances[d.conn.node]; // existence checked in topoSort
        const auto ports = producerPorts(d.conn.node);
        int idx = 0;
        if (!d.conn.port.empty()) {
            idx = -1;
            for (const auto& p : ports) {
                if (d.conn.port == p.name) {
                    idx = p.index;
                    break;
                }
            }
            if (idx < 0 || (size_t)idx >= src->outputs.size()) {
                fail("node '" + d.consumerId + "': node '" + d.conn.node +
                     "' has no output port '" + d.conn.port + "'");
                return false;
            }
        }

        const ValueType srcType = src->outputs[idx].value.type;
        Node::Input& in = d.node->inputs[d.inputIdx];
        if (srcType != d.portType) {
            if (srcType == ValueType::Scalar &&
                d.portType != ValueType::Texture &&
                d.portType != ValueType::Event &&
                d.portType != ValueType::Buffer) {
                in.splat = true;
            } else {
                fail("node '" + d.consumerId + "' input '" + d.portName +
                     "': type mismatch (" + valueTypeName(srcType) + " -> " +
                     valueTypeName(d.portType) + ")");
                return false;
            }
        }
        if (srcType == ValueType::Buffer &&
            src->outputs[idx].value.bufStride != d.portStride) {
            fail("node '" + d.consumerId + "' input '" + d.portName +
                 "': buffer element stride mismatch (" +
                 std::to_string(src->outputs[idx].value.bufStride) + " -> " +
                 std::to_string(d.portStride) + " bytes)");
            return false;
        }
        in.srcNode = src;
        in.srcPort = idx;
        in.previous = true;
        if (srcType == ValueType::Texture || srcType == ValueType::Buffer) {
            src->outputs[idx].needsHistory = true;
        }
    }
    return true;
}

bool Loader::instantiate(const RawNode& raw)
{
    std::vector<PortDef> ports;
    Node* node = makeNode(raw, ports);
    if (!node) {
        return false;
    }
    node->id = raw.id;
    mNodes.emplace_back(node);
    mInstances[raw.id] = node;
    if (raw.type == "output") {
        mOutput = node;
    }
    return bindInputs(raw, node, ports);
}

std::unique_ptr<Scene> Loader::load(const std::string& sceneJson)
{
    glz::generic json{};
    if (auto ec = glz::read_json(json, sceneJson); ec) {
        fail("scene.json: " + glz::format_error(ec, sceneJson));
        return nullptr;
    }
    if (!json.is_object()) {
        fail("scene.json: top level must be an object");
        return nullptr;
    }
    const auto& top = json.get_object();

    auto versionIt = top.find("version");
    if (versionIt == top.end() || !versionIt->second.is_number() ||
        (int)versionIt->second.get_number() != 1) {
        fail("unsupported or missing 'version' (expected 1)");
        return nullptr;
    }
    auto nameIt = top.find("name");
    if (nameIt == top.end() || !nameIt->second.is_string()) {
        fail("missing 'name'");
        return nullptr;
    }

    for (const auto& [key, value] : top) {
        if (key == "editor") {
            // Tool-owned metadata (§2.1): only its shape is validated; the
            // contents belong to editors, never the runtime.
            if (!value.is_object()) {
                warn("'editor' must be an object; ignored");
            }
        } else if (key != "version" && key != "name" && key != "author" &&
                   key != "description" && key != "parameters" &&
                   key != "packages" && key != "nodes") {
            warn("unknown top-level field '" + key + "' ignored");
        }
    }

    if (auto pkgIt = top.find("packages"); pkgIt != top.end()) {
        if (!pkgIt->second.is_object()) {
            fail("'packages' must be an object of name → version pin "
                 "(§20.3)");
            return nullptr;
        }
        for (const auto& [name, pin] : pkgIt->second.get_object()) {
            if (!validPackageName(name)) {
                fail("'packages': invalid package name '" + name +
                     "' (§20.1)");
                return nullptr;
            }
            if (!pin.is_string() || !validVersion(pin.get_string())) {
                fail("'packages': pin for '" + name +
                     "' must be a version prefix string (§20.3)");
                return nullptr;
            }
            mPins[name] = pin.get_string();
        }
    }

    if (auto paramsIt = top.find("parameters"); paramsIt != top.end()) {
        if (!parseParameters(paramsIt->second)) {
            return nullptr;
        }
    }
    auto nodesIt = top.find("nodes");
    if (nodesIt == top.end() || !parseNodes(nodesIt->second)) {
        if (nodesIt == top.end()) {
            fail("missing 'nodes'");
        }
        return nullptr;
    }
    if (!expandGraphs()) {
        return nullptr;
    }
    if (!topoSort()) {
        return nullptr;
    }

    if (mNeedsTime) {
        auto* time = new TimeNode();
        time->id = "time";
        mNodes.emplace_back(time);
        mInstances["time"] = time;
    }
    if (mNeedsMouse) {
        auto* mouse = new MouseNode();
        mouse->id = "mouse";
        mNodes.emplace_back(mouse);
        mInstances["mouse"] = mouse;
    }
    for (size_t idx : mTopoOrder) {
        if (!instantiate(mRawNodes[idx])) {
            return nullptr;
        }
    }
    if (!bindDeferred()) {
        return nullptr;
    }
    for (const auto& [name, pin] : mPins) {
        if (!mPinsUsed.count(name)) {
            warn("packages pin '" + name + "' is unused (§20.3)");
        }
    }
    if (!mOutput) {
        fail("scene has no output node");
        return nullptr;
    }

    // Name every output port so runtime lookups (Scene::fireEvent) can
    // resolve them the way references do.
    for (const auto& [id, node] : mInstances) {
        for (const auto& port : producerPorts(id)) {
            if ((size_t)port.index < node->outputs.size()) {
                node->outputs[port.index].name = port.name;
            }
        }
    }

    // Nodes unreachable from the output (via any edge, feedback included)
    // are a warning and are never executed (§8).
    std::map<const Node*, bool> reachable;
    std::vector<Node*> stack = { mOutput };
    while (!stack.empty()) {
        Node* node = stack.back();
        stack.pop_back();
        if (reachable[node]) {
            continue;
        }
        reachable[node] = true;
        for (const auto& in : node->inputs) {
            if (in.srcNode && !reachable[in.srcNode]) {
                stack.push_back(in.srcNode);
            }
        }
    }
    // §13: a sequence track nothing references is a warning (the whole-node
    // unreachable warning below covers fully unused sequences).
    for (SequenceNode* seq : mSequences) {
        if (!reachable[seq]) {
            continue;
        }
        std::vector<bool> used(seq->outputs.size(), false);
        for (const auto& node : mNodes) {
            for (const auto& in : node->inputs) {
                if (in.srcNode == seq && (size_t)in.srcPort < used.size()) {
                    used[in.srcPort] = true;
                }
            }
        }
        for (size_t i = 0; i < used.size(); ++i) {
            if (!used[i]) {
                warn("node '" + seq->id + "': track '" +
                     seq->tracks()[i].name + "' is not referenced");
            }
        }
    }

    std::vector<std::unique_ptr<Node>> kept;
    for (auto& node : mNodes) {
        if (reachable[node.get()]) {
            kept.push_back(std::move(node));
        } else if (!isImplicitNode(node->id)) {
            warn("node '" + node->id + "' is not reachable from the output "
                 "node and will never run");
        }
    }
    std::erase_if(mSequences,
                  [&](SequenceNode* seq) { return !reachable[seq]; });
    mNodes = std::move(kept);

    std::vector<SceneParam> params;
    for (const auto& p : mParams) {
        SceneParam sp;
        sp.name = p.name;
        sp.type = p.type;
        sp.value = p.def;
        sp.min = p.min;
        sp.max = p.max;
        sp.hasMin = p.hasMin;
        sp.hasMax = p.hasMax;
        sp.step = p.step;
        sp.label = p.label;
        sp.hint = p.hint;
        params.push_back(std::move(sp));
    }
    return SceneBuilder::make(nameIt->second.get_string(), std::move(mNodes),
                              std::move(params), std::move(mSequences),
                              mOutput);
}

} // namespace

std::unique_ptr<Scene> Scene::load(const std::string& sceneJson,
                                   const AssetReader& readAsset,
                                   const VideoDecoderFactory& videoFactory,
                                   const wgpu::Device& device,
                                   std::vector<std::string>& errors,
                                   std::vector<std::string>& warnings)
{
    Loader loader(readAsset, videoFactory, device, errors, warnings);
    auto scene = loader.load(sceneJson);
    if (!scene && errors.empty()) {
        errors.push_back("scene load failed");
    }
    return scene;
}

} // namespace drift::core
