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
                                       Node* output)
    {
        auto scene = std::unique_ptr<Scene>(new Scene());
        scene->mName = std::move(name);
        scene->mNodes = std::move(nodes);
        scene->mOutput = output;
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
    enum class Kind { Literal, Param, Conn } kind = Kind::Literal;
    Value literal;
    std::string param;
    RawConn conn;
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
        out.v[0] = (float)j.get_number();
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
            out.v[i] = (float)arr[i].get_number();
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
    if (!parseLiteral(j, out.literal)) {
        err = "invalid literal (number or array of 2-4 numbers)";
        return false;
    }
    out.kind = RawInput::Kind::Literal;
    return true;
}

// Static input port tables for the fixed-signature node types. Polymorphic
// ports carry a sentinel resolved during instantiation.
constexpr ValueType kPoly = (ValueType)0xff;

struct PortDef {
    std::string name;
    ValueType type;
    bool required;
    std::array<float, 4> def{};
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

struct OutputPortDef {
    const char* name;
    int index;
};

// name -> output port index per node type; index 0 is the default output.
std::vector<OutputPortDef> outputPortsFor(const std::string& type)
{
    if (type == "time") return { { "seconds", 0 }, { "delta", 1 } };
    if (type == "split") return { { "x", 0 }, { "y", 1 }, { "z", 2 }, { "w", 3 } };
    if (type == "output") return {};
    return { { "result", 0 } };
}

class Loader {
public:
    Loader(const AssetReader& readAsset, const wgpu::Device& device,
           std::vector<std::string>& errors)
        : mReadAsset(readAsset), mDevice(device), mErrors(errors)
    {
    }

    std::unique_ptr<Scene> load(const std::string& sceneJson);

private:
    void fail(const std::string& msg) { mErrors.push_back(msg); }

    bool parseParameters(const glz::generic& json);
    bool parseNodes(const glz::generic& json);
    bool topoSort();
    bool instantiate(const RawNode& raw);
    Node* makeNode(const RawNode& raw, std::vector<PortDef>& portsOut);
    bool bindInputs(const RawNode& raw, Node* node, const std::vector<PortDef>& ports);
    bool resolveInputType(const RawNode& raw, const RawInput& in, ValueType& out);

    const AssetReader& mReadAsset;
    const wgpu::Device& mDevice;
    std::vector<std::string>& mErrors;

    std::vector<Param> mParams;
    std::map<std::string, int> mParamIndex;
    std::vector<RawNode> mRawNodes;
    std::map<std::string, size_t> mRawIndex;
    std::vector<size_t> mTopoOrder;
    bool mNeedsTime = false;

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
            const float s = param.def.v[0];
            param.def.type = param.type;
            param.def.v = { s, s, s, s };
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
                if (in.kind == RawInput::Kind::Conn) {
                    if (in.conn.previous) {
                        fail("node '" + raw.id + "' input '" + port +
                             "': previous-frame reads are not implemented yet");
                        return false;
                    }
                    if (in.conn.node == "mouse") {
                        fail("node '" + raw.id + "' input '" + port +
                             "': mouse input is not implemented yet");
                        return false;
                    }
                    if (in.conn.node == "time") {
                        mNeedsTime = true;
                    }
                }
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
            if (in.kind != RawInput::Kind::Conn || in.conn.node == "time") {
                continue;
            }
            auto it = mRawIndex.find(in.conn.node);
            if (it == mRawIndex.end()) {
                fail("node '" + mRawNodes[i].id + "' input '" + port +
                     "': unknown node '" + in.conn.node + "'");
                return false;
            }
            adjacency[it->second].push_back(i);
            ++indegree[i];
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
        Node* src = mInstances[in.conn.node]; // topo order: already created
        const auto ports = outputPortsFor(
            in.conn.node == "time" ? "time" : mRawNodes[mRawIndex[in.conn.node]].type);
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
                    rawIn.conn.node == "time"
                        ? "time"
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
    for (size_t idx : mTopoOrder) {
        if (!instantiate(mRawNodes[idx])) {
            return nullptr;
        }
    }
    if (!mOutput) {
        fail("scene has no output node");
        return nullptr;
    }

    return SceneBuilder::make(nameIt->second.get_string(), std::move(mNodes),
                              mOutput);
}

} // namespace

std::unique_ptr<Scene> Scene::load(const std::string& sceneJson,
                                   const AssetReader& readAsset,
                                   const wgpu::Device& device,
                                   std::vector<std::string>& errors)
{
    Loader loader(readAsset, device, errors);
    auto scene = loader.load(sceneJson);
    if (!scene && errors.empty()) {
        errors.push_back("scene load failed");
    }
    return scene;
}

} // namespace drift::core
