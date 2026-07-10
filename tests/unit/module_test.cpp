#include <doctest/doctest.h>

#include <cstring>
#include <functional>
#include <map>

#include "core/Nodes.h"
#include "core/Scene.h"

// DESIGN.md §4.5 module nodes: interface JSON validation, I/O block
// layout, and evaluate semantics — driven through a fake ModuleInstance
// (plain host memory + a C++ update function), so the whole contract is
// testable natively with no WASM engine and no GPU. Buffer *upload* needs
// a device and is covered by the browser example instead; everything up
// to the staging bytes is covered here.

using namespace drift::core;

namespace {

const char* kBrainIface = R"({
    "abi": 1,
    "inputs": {
        "gain": { "type": "scalar", "default": 1.0 },
        "kick": { "type": "event" },
        "pos":  { "type": "vec2" }
    },
    "outputs": {
        "burst": { "type": "event" },
        "level": { "type": "scalar" }
    }
})";

struct FakeModule : ModuleInstance {
    explicit FakeModule(uint32_t ioSize)
        : mem(ioSize, 0)
    {
    }
    bool writeIo(uint32_t offset, const void* src, uint32_t len) override
    {
        if ((uint64_t)offset + len > mem.size()) {
            return false;
        }
        std::memcpy(mem.data() + offset, src, len);
        return true;
    }
    bool readIo(uint32_t offset, void* dst, uint32_t len) override
    {
        if ((uint64_t)offset + len > mem.size()) {
            return false;
        }
        std::memcpy(dst, mem.data() + offset, len);
        return true;
    }
    bool update(std::string& error) override
    {
        if (trap) {
            error = "boom";
            return false;
        }
        ++updates;
        if (onUpdate) {
            onUpdate(mem);
        }
        return true;
    }

    std::vector<uint8_t> mem;
    std::function<void(std::vector<uint8_t>&)> onUpdate;
    bool trap = false;
    int updates = 0;
};

float readF(const std::vector<uint8_t>& m, uint32_t off)
{
    float f;
    std::memcpy(&f, m.data() + off, 4);
    return f;
}

uint32_t readU(const std::vector<uint8_t>& m, uint32_t off)
{
    uint32_t u;
    std::memcpy(&u, m.data() + off, 4);
    return u;
}

void writeF(std::vector<uint8_t>& m, uint32_t off, float f)
{
    std::memcpy(m.data() + off, &f, 4);
}

Value scalar(double v)
{
    Value out{};
    out.type = ValueType::Scalar;
    out.v[0] = v;
    return out;
}

Value vec2(double x, double y)
{
    Value out{};
    out.type = ValueType::Vec2;
    out.v = { x, y, 0.0, 0.0 };
    return out;
}

// A minimal producer for wiring event edges into a directly-constructed
// node.
struct TestSource : Node {
    TestSource()
    {
        outputs.resize(1);
        outputs[0].value.type = ValueType::Event;
    }
    void evaluate(FrameContext&) override {}
};

ModuleInterface parseIface(const char* json)
{
    ModuleInterface iface;
    std::string err;
    REQUIRE_MESSAGE(ModuleInterface::parse(json, iface, err), err);
    iface.computeLayout();
    return iface;
}

std::string parseError(const std::string& json)
{
    ModuleInterface iface;
    std::string err;
    CHECK(!ModuleInterface::parse(json, iface, err));
    return err;
}

} // namespace

TEST_CASE("module interface: lexicographic layout (§4.5)")
{
    ModuleInterface iface = parseIface(kBrainIface);

    // Inputs sorted by name: gain, kick (event), pos.
    REQUIRE(iface.inputs.size() == 3);
    CHECK(iface.inputs[0].name == "gain");
    CHECK(iface.inputs[0].type == ValueType::Scalar);
    CHECK(iface.inputs[0].hasDefault);
    CHECK(iface.inputs[0].offset == kModuleHeaderSize);
    CHECK(iface.inputs[1].name == "kick");
    CHECK(iface.inputs[1].eventBit == 0);
    CHECK(iface.inputs[2].name == "pos");
    CHECK(!iface.inputs[2].hasDefault);
    CHECK(iface.inputs[2].offset == kModuleHeaderSize + 4);
    CHECK(iface.inputEnd == kModuleHeaderSize + 12);

    // Outputs sorted: burst (event), level.
    REQUIRE(iface.outputs.size() == 2);
    CHECK(iface.outputs[0].name == "burst");
    CHECK(iface.outputs[0].eventBit == 0);
    CHECK(iface.outputs[1].name == "level");
    CHECK(iface.outputs[1].offset == iface.valueOutBegin);
    CHECK(iface.valueOutEnd == iface.valueOutBegin + 4);
    CHECK(iface.ioSize == iface.valueOutEnd);
}

TEST_CASE("module interface: buffer layout and validation (§4.5)")
{
    ModuleInterface iface = parseIface(R"({
        "abi": 1,
        "outputs": {
            "pts": { "type": "buffer", "stride": 16, "capacity": 8 },
            "n":   { "type": "scalar" }
        }
    })");
    REQUIRE(iface.outputs.size() == 2);
    CHECK(iface.outputs[0].name == "n");
    CHECK(iface.outputs[1].stride == 16);
    CHECK(iface.outputs[1].capacity == 8);
    // n at 32; pts control after the value region, staging after that.
    CHECK(iface.outputs[0].offset == 32);
    CHECK(iface.outputs[1].offset == 36);
    CHECK(iface.bufferDataOffset(iface.outputs[1]) == 44);
    CHECK(iface.ioSize == 44 + 16 * 8);

    CHECK(parseError(R"({ "outputs": { "x": { "type": "scalar" } } })")
              .find("abi") != std::string::npos);
    CHECK(parseError(R"({ "abi": 1 })").find("outputs") != std::string::npos);
    CHECK(parseError(R"({ "abi": 1,
        "inputs": { "b": { "type": "buffer", "stride": 4, "capacity": 1 } },
        "outputs": { "x": { "type": "scalar" } } })")
              .find("'type'") != std::string::npos); // no buffer inputs
    CHECK(parseError(R"({ "abi": 1, "outputs": {
        "b": { "type": "buffer", "stride": 10, "capacity": 1 } } })")
              .find("multiple of 4") != std::string::npos);
    CHECK(parseError(R"({ "abi": 1, "outputs": {
        "b": { "type": "buffer", "stride": 16 } } })")
              .find("capacity") != std::string::npos);
    CHECK(parseError(R"({ "abi": 1, "outputs": {
        "x": { "type": "scalar", "stride": 4 } } })")
              .find("buffer outputs only") != std::string::npos);
    CHECK(parseError(R"({ "abi": 1,
        "inputs": { "e": { "type": "event", "default": 1 } },
        "outputs": { "x": { "type": "scalar" } } })")
              .find("no default") != std::string::npos);
    CHECK(parseError(R"({ "abi": 1,
        "inputs": { "v": { "type": "vec2", "default": [1, 2, 3] } },
        "outputs": { "x": { "type": "scalar" } } })")
              .find("does not match") != std::string::npos);

    // Scalar defaults splat to the declared vector type (§4).
    ModuleInterface splat = parseIface(R"({
        "abi": 1,
        "inputs": { "v": { "type": "vec4", "default": 0.5 } },
        "outputs": { "x": { "type": "scalar" } } })");
    CHECK(splat.inputs[0].def.type == ValueType::Vec4);
    CHECK(splat.inputs[0].def.v[3] == 0.5);

    // Event bitmask capacity is 32 per direction.
    std::string many = R"({ "abi": 1, "inputs": {)";
    for (int i = 0; i < 33; ++i) {
        many += (i ? "," : "") + std::string("\"e") + (char)('a' + i / 10) +
                (char)('0' + i % 10) + "\": { \"type\": \"event\" }";
    }
    many += R"(}, "outputs": { "x": { "type": "scalar" } } })";
    CHECK(parseError(many).find("event inputs") != std::string::npos);
}

TEST_CASE("module node: pure CPU transform — values in, values out (§4.5)")
{
    ModuleInterface iface = parseIface(kBrainIface);
    auto fake = std::make_unique<FakeModule>(iface.ioSize);
    FakeModule* fp = fake.get();
    const uint32_t gainOff = iface.inputs[0].offset;
    const uint32_t posOff = iface.inputs[2].offset;
    const uint32_t levelOff = iface.outputs[1].offset;
    fp->onUpdate = [&](std::vector<uint8_t>& m) {
        const float gain = readF(m, gainOff);
        const float x = readF(m, posOff), y = readF(m, posOff + 4);
        writeF(m, levelOff, gain * (x + y));
        if (readU(m, kModuleHdrEventsIn) & 1u) { // kick -> burst
            uint32_t out = 1u;
            std::memcpy(m.data() + kModuleHdrEventsOut, &out, 4);
        }
    };

    ModuleNode node(std::move(fake), iface);
    node.id = "brain";
    node.inputs.resize(iface.inputs.size());
    node.inputs[0].constant = scalar(2.0);
    node.inputs[2].constant = vec2(3.0, 4.0);

    FrameContext ctx{};
    ctx.seconds = 1.0;
    ctx.frame = 1;
    node.evaluate(ctx);
    node.firstEvaluate = false;

    CHECK(fp->updates == 1);
    CHECK(readF(fp->mem, kModuleHdrTime) == 1.0f);
    CHECK(readF(fp->mem, kModuleHdrDt) == 0.0f); // first update
    CHECK(readU(fp->mem, kModuleHdrFlags) == 1u);
    CHECK(node.outputs[1].value.v[0] == 14.0);
    CHECK(node.outputs[1].dirty);
    CHECK(!node.outputs[0].dirty); // no kick, no burst

    // Same inputs -> same value -> not dirty (change detection).
    for (auto& out : node.outputs) {
        out.dirty = false;
    }
    ctx.seconds = 1.5;
    node.evaluate(ctx);
    CHECK(readF(fp->mem, kModuleHdrDt) == 0.5f);
    CHECK(readU(fp->mem, kModuleHdrFlags) == 0u);
    CHECK(!node.outputs[1].dirty);

    // Changed input -> new value -> dirty.
    for (auto& out : node.outputs) {
        out.dirty = false;
    }
    node.inputs[0].constant = scalar(3.0);
    node.evaluate(ctx);
    CHECK(node.outputs[1].value.v[0] == 21.0);
    CHECK(node.outputs[1].dirty);
}

TEST_CASE("module node: event edges cross the boundary as bits (§4.5)")
{
    ModuleInterface iface = parseIface(kBrainIface);
    auto fake = std::make_unique<FakeModule>(iface.ioSize);
    FakeModule* fp = fake.get();
    fp->onUpdate = [&](std::vector<uint8_t>& m) {
        if (readU(m, kModuleHdrEventsIn) & 1u) {
            uint32_t out = 1u;
            std::memcpy(m.data() + kModuleHdrEventsOut, &out, 4);
        }
    };

    ModuleNode node(std::move(fake), iface);
    node.inputs.resize(iface.inputs.size());
    TestSource trigger;
    node.inputs[1].srcNode = &trigger;
    node.inputs[1].srcPort = 0;
    node.inputs[1].type = ValueType::Event;

    FrameContext ctx{};
    trigger.outputs[0].dirty = true; // fires this frame
    node.evaluate(ctx);
    node.firstEvaluate = false;
    CHECK(readU(fp->mem, kModuleHdrEventsIn) == 1u);
    CHECK(node.outputs[0].dirty); // burst fired

    trigger.outputs[0].dirty = false;
    for (auto& out : node.outputs) {
        out.dirty = false;
    }
    node.evaluate(ctx);
    CHECK(readU(fp->mem, kModuleHdrEventsIn) == 0u);
    CHECK(!node.outputs[0].dirty); // a fire is a one-frame phenomenon
}

TEST_CASE("module node: a trapped module goes dead, not down (§4.5)")
{
    ModuleInterface iface = parseIface(kBrainIface);
    auto fake = std::make_unique<FakeModule>(iface.ioSize);
    FakeModule* fp = fake.get();

    ModuleNode node(std::move(fake), iface);
    node.inputs.resize(iface.inputs.size());
    FrameContext ctx{};
    node.evaluate(ctx);
    node.firstEvaluate = false;
    CHECK(fp->updates == 1);

    fp->trap = true;
    node.evaluate(ctx);
    CHECK(fp->updates == 1); // trapped: no update happened

    fp->trap = false;
    node.evaluate(ctx);
    CHECK(fp->updates == 1); // dead stays dead
}

// ---- loader-level validation ----

namespace {

const char* kMinimalWgsl = R"(
    struct Params { phase: f32 }
    @group(0) @binding(0) var<uniform> params: Params;
    @fragment fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
        return vec4f(params.phase);
    }
)";

const char* kVec4ConsumerWgsl = R"(
    @group(0) @binding(0) var<storage, read> pts: array<vec4f>;
    @group(0) @binding(1) var canvas: texture_storage_2d<rgba16float, write>;
    @compute @workgroup_size(8, 8)
    fn main(@builtin(global_invocation_id) id: vec3u) {
        textureStore(canvas, vec2i(id.xy), pts[0]);
    }
)";

const char* kPts12Iface = R"({
    "abi": 1,
    "outputs": { "pts": { "type": "buffer", "stride": 12, "capacity": 4 } }
})";

const char* kPts16Iface = R"({
    "abi": 1,
    "outputs": { "pts": { "type": "buffer", "stride": 16, "capacity": 4 } }
})";

struct LoadResult {
    std::unique_ptr<Scene> scene;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    std::vector<FakeModule*> modules; // borrowed; owned by scene nodes
    std::vector<uint32_t> ioSizes;

    bool hasError(const std::string& needle) const
    {
        for (const auto& e : errors) {
            if (e.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

LoadResult loadScene(const std::string& nodesJson, bool withLoader = true)
{
    const std::map<std::string, std::string> assets = {
        { "modules/brain.json", kBrainIface },
        { "modules/brain.wasm", std::string("\0asm-fake", 9) },
        { "modules/pts12.json", kPts12Iface },
        { "modules/pts16.json", kPts16Iface },
        { "shaders/minimal.wgsl", kMinimalWgsl },
        { "shaders/vec4consumer.wgsl", kVec4ConsumerWgsl },
    };
    LoadResult r;
    ModuleLoader loader;
    if (withLoader) {
        loader = [&r](const std::string&, uint32_t ioSize,
                      std::string&) -> std::unique_ptr<ModuleInstance> {
            auto fake = std::make_unique<FakeModule>(ioSize);
            r.modules.push_back(fake.get());
            r.ioSizes.push_back(ioSize);
            return fake;
        };
    }
    const std::string json = R"({ "version": 1, "name": "m", "nodes": [)" +
                             nodesJson + "]}";
    r.scene = Scene::load(
        json,
        [&assets](const std::string& path, std::string& out) {
            auto it = assets.find(path);
            if (it == assets.end()) {
                return false;
            }
            out = it->second;
            return true;
        },
        nullptr, loader, wgpu::Device(), r.errors, r.warnings);
    return r;
}

} // namespace

TEST_CASE("module loader: ports resolve, wiring type-checks (§4.5)")
{
    auto r = loadScene(R"(
        { "id": "brain", "type": "module", "module": "modules/brain.wasm",
          "interface": "modules/brain.json",
          "inputs": { "pos": "@mouse.position" } },
        { "id": "fx", "type": "shader", "shader": "shaders/minimal.wgsl",
          "inputs": { "phase": "@brain.level" } },
        { "id": "out", "type": "output", "inputs": { "color": "@fx.result" } })");
    CHECK(r.errors.empty());
    REQUIRE(r.scene != nullptr);
    REQUIRE(r.modules.size() == 1);
    CHECK(r.ioSizes[0] == 48); // header 32 + gain 4 + pos 8 + level 4

    // pos is declared without a default: required.
    r = loadScene(R"(
        { "id": "brain", "type": "module", "module": "modules/brain.wasm",
          "interface": "modules/brain.json" },
        { "id": "fx", "type": "shader", "shader": "shaders/minimal.wgsl",
          "inputs": { "phase": "@brain.level" } },
        { "id": "out", "type": "output", "inputs": { "color": "@fx.result" } })");
    CHECK(r.scene == nullptr);
    CHECK(r.hasError("required input 'pos' missing"));

    r = loadScene(R"(
        { "id": "brain", "type": "module", "module": "modules/brain.wasm",
          "interface": "modules/brain.json",
          "inputs": { "pos": "@mouse.position", "bogus": 1 } },
        { "id": "out", "type": "output", "inputs": { "color": "@brain.level" } })");
    CHECK(r.scene == nullptr);
    CHECK(r.hasError("unknown input port 'bogus'"));

    // Literals cannot feed event ports.
    r = loadScene(R"(
        { "id": "brain", "type": "module", "module": "modules/brain.wasm",
          "interface": "modules/brain.json",
          "inputs": { "pos": "@mouse.position", "kick": 1 } },
        { "id": "fx", "type": "shader", "shader": "shaders/minimal.wgsl",
          "inputs": { "phase": "@brain.level" } },
        { "id": "out", "type": "output", "inputs": { "color": "@fx.result" } })");
    CHECK(r.scene == nullptr);
    CHECK(r.hasError("type mismatch"));
}

TEST_CASE("module loader: platform without a module engine says so (§4.5)")
{
    auto r = loadScene(R"(
        { "id": "brain", "type": "module", "module": "modules/brain.wasm",
          "interface": "modules/brain.json",
          "inputs": { "pos": "@mouse.position" } },
        { "id": "out", "type": "output", "inputs": { "color": "@brain.level" } })",
                       /*withLoader=*/false);
    CHECK(r.scene == nullptr);
    CHECK(r.hasError("cannot run WASM modules"));
}

TEST_CASE("module loader: buffer outputs stride-check like compute (§18.1)")
{
    // 12-byte module elements into a 16-byte consumer: rejected at load.
    auto r = loadScene(R"(
        { "id": "brain", "type": "module", "module": "modules/brain.wasm",
          "interface": "modules/pts12.json" },
        { "id": "paint", "type": "compute", "shader": "shaders/vec4consumer.wgsl",
          "inputs": { "pts": "@brain.pts" } },
        { "id": "out", "type": "output", "inputs": { "color": "@paint.canvas" } })");
    CHECK(r.scene == nullptr);
    CHECK(r.hasError("stride mismatch"));

    // Matching strides load; the capacity override applies before layout.
    r = loadScene(R"(
        { "id": "brain", "type": "module", "module": "modules/brain.wasm",
          "interface": "modules/pts16.json", "capacity": { "pts": 16 } },
        { "id": "paint", "type": "compute", "shader": "shaders/vec4consumer.wgsl",
          "inputs": { "pts": "@brain.pts" } },
        { "id": "out", "type": "output", "inputs": { "color": "@paint.canvas" } })");
    CHECK(r.errors.empty());
    REQUIRE(r.scene != nullptr);
    // header + {count, written} + 16 elements * 16 bytes
    CHECK(r.ioSizes.back() == 32 + 8 + 16 * 16);

    r = loadScene(R"(
        { "id": "brain", "type": "module", "module": "modules/brain.wasm",
          "interface": "modules/pts16.json", "capacity": 2.5 },
        { "id": "out", "type": "output", "inputs": { "color": "@brain.pts" } })");
    CHECK(r.scene == nullptr);
    CHECK(r.hasError("'capacity' must be a positive integer"));
}
