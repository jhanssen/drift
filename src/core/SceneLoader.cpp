#include <algorithm>
#include <cctype>
#include <map>

#include <glaze/glaze.hpp>

#include "Nodes.h"
#include "Scene.h"

// scene.json -> validated node graph (SCENE_FORMAT.md). All validation
// listed in §13 that applies to the implemented node subset happens here.

namespace drift::core {

struct SceneBuilder {
    static std::unique_ptr<Scene> make(std::string name,
                                       std::vector<std::unique_ptr<Node>> nodes,
                                       std::vector<SceneParam> params,
                                       Node* output)
    {
        auto scene = std::unique_ptr<Scene>(new Scene());
        scene->mName = std::move(name);
        scene->mNodes = std::move(nodes);
        scene->mParams = std::move(params);
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
    bool topoSort();
    bool instantiate(const RawNode& raw);
    Node* makeNode(const RawNode& raw, std::vector<PortDef>& portsOut);
    bool bindInputs(const RawNode& raw, Node* node, const std::vector<PortDef>& ports);
    bool resolveInputType(const RawNode& raw, const RawInput& in, ValueType& out);
    bool bindDeferred();

    // Feedback edges bind after all nodes exist — the producer may come
    // later in topo order (or be the consumer itself).
    struct DeferredBind {
        Node* node;
        size_t inputIdx;
        std::string consumerId; // for error messages
        std::string portName;
        RawConn conn;
        ValueType portType;
    };
    std::vector<DeferredBind> mDeferredBinds;

    const AssetReader& mReadAsset;
    const VideoDecoderFactory& mVideoFactory;
    const wgpu::Device& mDevice;
    std::vector<std::string>& mErrors;
    std::vector<std::string>& mWarnings;

    std::vector<Param> mParams;
    std::map<std::string, int> mParamIndex;
    std::vector<RawNode> mRawNodes;
    std::map<std::string, size_t> mRawIndex;
    std::vector<size_t> mTopoOrder;
    bool mNeedsTime = false;
    bool mNeedsMouse = false;

    std::map<std::string, Node*> mInstances;
    std::vector<std::unique_ptr<Node>> mNodes;
    Node* mOutput = nullptr;
};

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
        if (!entry.is_object()) {
            fail("node entries must be objects");
            return false;
        }
        const auto& obj = entry.get_object();
        RawNode raw;
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
        if (mRawIndex.count(raw.id)) {
            fail("duplicate node id '" + raw.id + "'");
            return false;
        }

        // Unknown properties are a warning, not an error (§13) — but a
        // typo'd property silently changing behavior is the worst authoring
        // footgun, so say something.
        static const std::map<std::string, std::vector<std::string>> kProps = {
            { "wave", { "shape" } },
            { "remap", { "clamp" } },
            { "combine", {} },
            { "split", {} },
            { "shader", { "shader", "size" } },
            { "image", { "src" } },
            { "video", { "src", "loop" } },
            { "transform", {} },
            { "compositor", {} },
            { "output", {} },
        };
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
        mRawIndex[raw.id] = mRawNodes.size();
        mRawNodes.push_back(std::move(raw));
    }
    return true;
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
        const auto ports = outputPortsFor(
            isImplicitNode(in.conn.node) ? in.conn.node
                                         : mRawNodes[mRawIndex[in.conn.node]].type);
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

    if (raw.type == "shader") {
        const auto& obj = raw.json->get_object();
        auto pathIt = obj.find("shader");
        if (pathIt == obj.end() || !pathIt->second.is_string()) {
            fail("node '" + raw.id + "': shader node needs a string 'shader' path");
            return nullptr;
        }
        const std::string& path = pathIt->second.get_string();
        if (path.find("..") != std::string::npos || path.starts_with("/")) {
            fail("node '" + raw.id + "': shader path escapes the project");
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

    if (raw.type == "image") {
        const auto& obj = raw.json->get_object();
        auto srcIt = obj.find("src");
        if (srcIt == obj.end() || !srcIt->second.is_string()) {
            fail("node '" + raw.id + "': image node needs a string 'src' path");
            return nullptr;
        }
        const std::string& path = srcIt->second.get_string();
        if (path.find("..") != std::string::npos || path.starts_with("/")) {
            fail("node '" + raw.id + "': image path escapes the project");
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
        const std::string& path = srcIt->second.get_string();
        if (path.find("..") != std::string::npos || path.starts_with("/")) {
            fail("node '" + raw.id + "': video path escapes the project");
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
        portsOut.clear();
        return new VideoNode(std::move(decoder));
    }

    if (raw.type == "transform") {
        portsOut.assign(std::begin(kTransformInputs), std::end(kTransformInputs));
        return new TransformNode();
    }

    if (raw.type == "compositor") {
        portsOut.assign(std::begin(kCompositorInputs), std::end(kCompositorInputs));
        return new CompositorNode();
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
                const auto srcPorts = outputPortsFor(
                    mRawNodes[mRawIndex[element.conn.node]].type);
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
                                       rawIn.conn, portType });
            continue;
        }

        ValueType srcType;
        if (!resolveInputType(raw, rawIn, srcType)) {
            return false;
        }
        if (srcType != portType) {
            if (srcType == ValueType::Scalar && portType != ValueType::Texture) {
                in.splat = true; // §4: scalar splats to vecN
            } else {
                fail("node '" + raw.id + "' input '" + portName + "': type mismatch (" +
                     valueTypeName(srcType) + " -> " + valueTypeName(portType) + ")");
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
                const auto srcPorts = outputPortsFor(
                    isImplicitNode(rawIn.conn.node)
                        ? rawIn.conn.node
                        : mRawNodes[mRawIndex[rawIn.conn.node]].type);
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
        const auto ports = outputPortsFor(
            isImplicitNode(d.conn.node) ? d.conn.node
                                        : mRawNodes[mRawIndex[d.conn.node]].type);
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
            if (srcType == ValueType::Scalar && d.portType != ValueType::Texture) {
                in.splat = true;
            } else {
                fail("node '" + d.consumerId + "' input '" + d.portName +
                     "': type mismatch (" + valueTypeName(srcType) + " -> " +
                     valueTypeName(d.portType) + ")");
                return false;
            }
        }
        in.srcNode = src;
        in.srcPort = idx;
        in.previous = true;
        if (srcType == ValueType::Texture) {
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
        if (key != "version" && key != "name" && key != "author" &&
            key != "description" && key != "parameters" && key != "nodes") {
            warn("unknown top-level field '" + key + "' ignored");
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
    if (!mOutput) {
        fail("scene has no output node");
        return nullptr;
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
    std::vector<std::unique_ptr<Node>> kept;
    for (auto& node : mNodes) {
        if (reachable[node.get()]) {
            kept.push_back(std::move(node));
        } else if (!isImplicitNode(node->id)) {
            warn("node '" + node->id + "' is not reachable from the output "
                 "node and will never run");
        }
    }
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
                              std::move(params), mOutput);
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
