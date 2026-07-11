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

TEST_CASE("module permissions: request grammar (§4.4)")
{
    ModuleInterface iface = parseIface(R"({
        "abi": 1,
        "permissions": {
            "storage": { "quota": 65536 },
            "network": { "origins": ["https://api.example.com",
                                     "http://localhost:8080"] }
        },
        "outputs": { "x": { "type": "scalar" } } })");
    CHECK(iface.permissions.storageQuota == 65536);
    REQUIRE(iface.permissions.networkOrigins.size() == 2);
    CHECK(iface.permissions.networkOrigins[0] == "http://localhost:8080");

    // Plain http only for localhost; no paths; positive bounded quota.
    CHECK(parseError(R"({ "abi": 1, "permissions": {
            "network": { "origins": ["http://evil.example.com"] } },
        "outputs": { "x": { "type": "scalar" } } })")
              .find("origin") != std::string::npos);
    CHECK(parseError(R"({ "abi": 1, "permissions": {
            "network": { "origins": ["https://api.example.com/v1"] } },
        "outputs": { "x": { "type": "scalar" } } })")
              .find("origin") != std::string::npos);
    CHECK(parseError(R"({ "abi": 1, "permissions": {
            "storage": { "quota": 0 } },
        "outputs": { "x": { "type": "scalar" } } })")
              .find("quota") != std::string::npos);

    CHECK(ModulePermissions::validOrigin("https://[2001:db8::1]:8443"));
    CHECK(!ModulePermissions::validOrigin("https://"));
    CHECK(ModulePermissions::validOrigin("wss://api.example.com"));
    CHECK(!ModulePermissions::validOrigin("ftp://api.example.com"));
    CHECK(!ModulePermissions::validOrigin("https://api.example.com:"));

    // Coverage: the record is the policy; excess requests are the
    // missing list (the load-warning bodies).
    ModulePermissions granted;
    granted.storageQuota = 1024;
    granted.networkOrigins = { "https://api.example.com" };
    ModulePermissions want;
    want.storageQuota = 2048;
    want.networkOrigins = { "https://api.example.com",
                            "https://other.example.com" };
    std::vector<std::string> missing;
    CHECK(!want.coveredBy(granted, missing));
    REQUIRE(missing.size() == 2);
    CHECK(missing[0].find("storage") != std::string::npos);
    CHECK(missing[1].find("other.example.com") != std::string::npos);
    want.storageQuota = 1024;
    want.networkOrigins = { "https://api.example.com" };
    missing.clear();
    CHECK(want.coveredBy(granted, missing));
}

TEST_CASE("module storage: quota, roundtrip, write-behind (§4.4)")
{
    struct FakeBackend : ModuleStoragePersistence {
        std::map<std::string, std::string> blobs;
        int saves = 0;
        bool load(const std::string& ns, std::string& blob) override
        {
            auto it = blobs.find(ns);
            if (it == blobs.end()) {
                return false;
            }
            blob = it->second;
            return true;
        }
        void save(const std::string& ns, const std::string& blob) override
        {
            blobs[ns] = blob;
            ++saves;
        }
    };
    auto backend = std::make_shared<FakeBackend>();

    const auto K = [](const char* s) { return (const uint8_t*)s; };
    {
        ModuleStorage st(64);
        st.attach(backend, "brain");
        uint8_t buf[32];
        CHECK(st.get(K("pos"), 3, buf, sizeof(buf)) == kModuleStorageMissing);
        CHECK(st.put(K("pos"), 3, K("12345678"), 8) == 0);
        CHECK(st.used() == 11);
        REQUIRE(st.get(K("pos"), 3, buf, sizeof(buf)) == 8);
        CHECK(std::memcmp(buf, "12345678", 8) == 0);
        // Probe (dcap 0) returns the size without copying.
        CHECK(st.get(K("pos"), 3, nullptr, 0) == 8);
        // Replacement re-accounts; over-quota is refused atomically.
        CHECK(st.put(K("pos"), 3, K("xy"), 2) == 0);
        CHECK(st.used() == 5);
        std::vector<uint8_t> big(60, 7);
        CHECK(st.put(K("big"), 3, big.data(), 60) == kModuleStorageQuota);
        CHECK(st.used() == 5);
        CHECK(st.put(K("b"), 1, big.data(), 40) == 0);
        // keys: '\0'-terminated names, probe-then-copy.
        const int32_t need = st.keys(nullptr, 0);
        CHECK(need == 6); // "b\0pos\0"
        std::vector<uint8_t> names(need);
        st.keys(names.data(), (uint32_t)names.size());
        CHECK(std::memcmp(names.data(), "b\0pos\0", 6) == 0);
        CHECK(st.erase(K("b"), 1) == 0);
        CHECK(st.erase(K("b"), 1) == 0); // idempotent
        // Debounced write-behind: first flush at t, not again until t+1.
        st.maybeFlush(10.0);
        CHECK(backend->saves == 1);
        CHECK(st.put(K("pos"), 3, K("zz"), 2) == 0);
        st.maybeFlush(10.5);
        CHECK(backend->saves == 1);
        st.maybeFlush(11.5);
        CHECK(backend->saves == 2);
    } // dtor: not dirty, no extra save
    CHECK(backend->saves == 2);

    // A fresh store loads the persisted state.
    ModuleStorage st2(64);
    st2.attach(backend, "brain");
    uint8_t buf[8];
    REQUIRE(st2.get(K("pos"), 3, buf, sizeof(buf)) == 2);
    CHECK(std::memcmp(buf, "zz", 2) == 0);

    // Ungranted (quota 0): writes denied, reads miss (§4.4 soft-deny).
    ModuleStorage denied(0);
    CHECK(denied.put(K("k"), 1, K("v"), 1) == kModuleStorageDenied);
    CHECK(denied.get(K("k"), 1, buf, sizeof(buf)) == kModuleStorageMissing);
    CHECK(denied.keys(nullptr, 0) == kModuleStorageDenied);
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

LoadResult loadScene(const std::string& nodesJson, bool withLoader = true,
                     const std::map<std::string, std::string>& extraAssets = {},
                     const std::function<bool(ModulePermissions&)>&
                         projectGrants = nullptr)
{
    std::map<std::string, std::string> assets = {
        { "modules/brain.json", kBrainIface },
        { "modules/brain.wasm", std::string("\0asm-fake", 9) },
        { "modules/pts12.json", kPts12Iface },
        { "modules/pts16.json", kPts16Iface },
        { "shaders/minimal.wgsl", kMinimalWgsl },
        { "shaders/vec4consumer.wgsl", kVec4ConsumerWgsl },
    };
    for (const auto& [path, contents] : extraAssets) {
        assets[path] = contents;
    }
    LoadResult r;
    ModulePlatform platform;
    platform.projectGrants = projectGrants;
    if (withLoader) {
        platform.load = [&r](const std::string&, uint32_t ioSize,
                             ModuleStorage*, ModuleNet*,
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
        nullptr, platform, wgpu::Device(), r.errors, r.warnings);
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

TEST_CASE("module grants: soft-deny — ungranted warns, still loads (§4.4)")
{
    const char* kCapIface = R"({
        "abi": 1,
        "permissions": {
            "network": { "origins": ["https://api.example.com"] } },
        "outputs": { "level": { "type": "scalar" } } })";
    const std::map<std::string, std::string> capAssets = {
        { "modules/cap.json", kCapIface },
    };
    const std::string nodes = R"(
        { "id": "brain", "type": "module", "module": "modules/brain.wasm",
          "interface": "modules/cap.json" },
        { "id": "fx", "type": "shader", "shader": "shaders/minimal.wgsl",
          "inputs": { "phase": "@brain.level" } },
        { "id": "out", "type": "output", "inputs": { "color": "@fx.result" } })";

    // No grant record anywhere: loads with the §4.4 warning.
    auto r = loadScene(nodes, true, capAssets);
    REQUIRE(r.scene != nullptr);
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0].find("api.example.com") != std::string::npos);
    CHECK(r.warnings[0].find("not granted") != std::string::npos);

    // A covering project grant: no warning.
    r = loadScene(nodes, true, capAssets, [](ModulePermissions& out) {
        out.networkOrigins = { "https://api.example.com" };
        return true;
    });
    REQUIRE(r.scene != nullptr);
    CHECK(r.warnings.empty());
}

TEST_CASE("module grants: package grants ride .installed.json (§4.4)")
{
    const std::map<std::string, std::string> pkg = {
        { "packages/cap/manifest.json",
          R"({ "name": "cap", "version": "1.0.0" })" },
        { "packages/cap/graphs/cap.json", R"({
            "version": 1, "name": "Cap",
            "outputs": { "result": "@m.level" },
            "nodes": [ { "id": "m", "type": "module",
                         "module": "modules/cap.wasm",
                         "interface": "modules/cap.json" } ] })" },
        { "packages/cap/modules/cap.wasm", "fake" },
        { "packages/cap/modules/cap.json", R"({
            "abi": 1,
            "permissions": {
                "network": { "origins": ["https://api.example.com"] } },
            "outputs": { "level": { "type": "scalar" } } })" },
    };
    const std::string nodes = R"(
        { "id": "g", "type": "graph", "graph": "packages/cap/graphs/cap.json" },
        { "id": "fx", "type": "shader", "shader": "shaders/minimal.wgsl",
          "inputs": { "phase": "@g" } },
        { "id": "out", "type": "output", "inputs": { "color": "@fx.result" } })";

    // Installed but never granted: warning.
    auto r = loadScene(nodes, true, pkg);
    REQUIRE(r.scene != nullptr);
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0].find("not granted") != std::string::npos);

    // The record driftpkg writes at consent: covered, silent.
    auto granted = pkg;
    granted["packages/cap/.installed.json"] = R"({
        "repository": "https://repo.example.com",
        "permissions": {
            "network": { "origins": ["https://api.example.com"] } } })";
    r = loadScene(nodes, true, granted);
    REQUIRE(r.scene != nullptr);
    CHECK(r.warnings.empty());
}

TEST_CASE("module net: policy, offline face, mailboxes, queues (§4.4)")
{
    struct FakeBackend : ModuleNetBackend {
        std::vector<std::pair<int32_t, std::string>> http, ws, sent;
        void httpRequest(ModuleNet*, int32_t h, const std::string& u,
                         std::string) override
        {
            http.push_back({ h, u });
        }
        void wsOpen(ModuleNet*, int32_t h, const std::string& u) override
        {
            ws.push_back({ h, u });
        }
        void wsSend(ModuleNet*, int32_t h, std::string d, bool) override
        {
            sent.push_back({ h, std::move(d) });
        }
        void cancel(ModuleNet*, int32_t) override {}
    };
    auto backend = std::make_shared<FakeBackend>();
    const auto U = [](const char* s) { return (const uint8_t*)s; };

    ModuleNet net({ "https://api.example.com", "wss://feed.example.com" },
                  { "https://api.example.com", "wss://feed.example.com" },
                  backend);
    net.beginUpdate(0.0);

    // Undeclared origin: the handle exists and reports the offline face —
    // indistinguishable from an unplugged cable; nothing reaches the wire.
    int32_t h = net.httpRequest(U("https://evil.example.com/x"), 26,
                                nullptr, 0, 0);
    CHECK(h > 0);
    CHECK(net.stat(h, 0) == kModuleNetStateFailed);
    CHECK(net.poll(h, nullptr, 0) == kModuleNetFailed);
    CHECK(backend->http.empty());

    // Allowed origin (path allowed in the URL, origin matched exactly).
    h = net.httpRequest(U("https://api.example.com/v1?q=1"), 30, nullptr,
                        0, 0);
    REQUIRE(backend->http.size() == 1);
    CHECK(backend->http[0].second == "https://api.example.com/v1?q=1");
    CHECK(net.poll(h, nullptr, 0) == kModuleNetPending);
    CHECK(!net.wakePending());
    net.deliverHttp(h, kModuleNetStateReady, 200, "hello");
    CHECK(net.wakePending());
    CHECK(net.stat(h, 1) == 200);
    uint8_t buf[16];
    REQUIRE(net.poll(h, buf, sizeof(buf)) == 5);
    CHECK(std::memcmp(buf, "hello", 5) == 0);
    CHECK(net.poll(h, buf, sizeof(buf)) == 5); // re-readable until close
    net.close(h);
    CHECK(net.poll(h, nullptr, 0) == kModuleNetInvalid);

    // WS lifecycle: send gated on open; messages pop on full copy only;
    // the bounded queue drops oldest and reports via self-clearing stat.
    const int32_t w = net.wsOpen(U("wss://feed.example.com/live"), 27);
    REQUIRE(backend->ws.size() == 1);
    CHECK(net.stat(w, 0) == kModuleNetStateConnecting);
    CHECK(net.send(w, U("x"), 1, 1) == kModuleNetPending);
    net.deliverWsState(w, kModuleNetStateReady);
    CHECK(net.send(w, U("sub"), 3, 1) == 0);
    REQUIRE(backend->sent.size() == 1);
    net.deliverWsMessage(w, "m1");
    net.deliverWsMessage(w, "m2");
    CHECK(net.poll(w, nullptr, 0) == 2);             // probe, not popped
    CHECK(net.poll(w, buf, 1) == kModuleNetInvalid); // no partial messages
    CHECK(net.poll(w, buf, sizeof(buf)) == 2);       // pops m1
    CHECK(net.poll(w, buf, sizeof(buf)) == 2);       // pops m2
    CHECK(net.poll(w, buf, sizeof(buf)) == kModuleNetPending);
    for (size_t i = 0; i < kModuleNetQueueMessages + 5; ++i) {
        net.deliverWsMessage(w, "x");
    }
    CHECK(net.stat(w, 2) == 5); // dropped-oldest count
    CHECK(net.stat(w, 2) == 0); // self-clearing
    net.deliverWsState(w, kModuleNetStateClosed);
    CHECK(net.send(w, U("y"), 1, 1) == kModuleNetClosed);

    // Token bucket: the burst runs out, then Again until scene time
    // refills it.
    int issued = 0;
    while (net.httpRequest(U("https://api.example.com/t"), 25, nullptr, 0,
                           0) > 0) {
        ++issued;
        REQUIRE(issued < 32);
    }
    CHECK(issued > 0);
    net.beginUpdate(100.0); // refill
    CHECK(net.httpRequest(U("https://api.example.com/t"), 25, nullptr, 0,
                          0) > 0);

    // Null backend: the headless/golden/offline configuration.
    ModuleNet offline({ "https://api.example.com" },
                      { "https://api.example.com" }, nullptr);
    offline.beginUpdate(0.0);
    h = offline.httpRequest(U("https://api.example.com/x"), 25, nullptr, 0,
                            0);
    CHECK(h > 0);
    CHECK(offline.stat(h, 0) == kModuleNetStateFailed);

    // Declared but not granted: offline face too (§4.4 soft-deny).
    ModuleNet ungranted({ "https://api.example.com" }, {}, backend);
    ungranted.beginUpdate(0.0);
    h = ungranted.httpRequest(U("https://api.example.com/x"), 25, nullptr,
                              0, 0);
    CHECK(ungranted.stat(h, 0) == kModuleNetStateFailed);
}

TEST_CASE("module node: external wakes — deliveries and wake_after_ms "
          "(§4.4)")
{
    ModuleInterface iface = parseIface(R"({
        "abi": 1,
        "permissions": {
            "network": { "origins": ["https://api.example.com"] } },
        "outputs": { "n": { "type": "scalar" } } })");
    auto fake = std::make_unique<FakeModule>(iface.ioSize);
    FakeModule* fp = fake.get();
    struct NullBackend : ModuleNetBackend {
        void httpRequest(ModuleNet*, int32_t, const std::string&,
                         std::string) override {}
        void wsOpen(ModuleNet*, int32_t, const std::string&) override {}
        void wsSend(ModuleNet*, int32_t, std::string, bool) override {}
        void cancel(ModuleNet*, int32_t) override {}
    };
    auto net = std::make_unique<ModuleNet>(
        iface.permissions.networkOrigins, iface.permissions.networkOrigins,
        std::make_shared<NullBackend>());
    ModuleNet* np = net.get();

    ModuleNode node(std::move(fake), iface, {}, nullptr, std::move(net));
    FrameContext ctx{};
    ctx.seconds = 0.0;
    CHECK(!node.wakePending(0.0)); // pre-first-run: firstEvaluate drives
    node.evaluate(ctx);
    node.firstEvaluate = false;
    CHECK(fp->updates == 1);
    CHECK(!node.wakePending(10.0)); // idle: no deliveries, no timer

    // A delivery wakes exactly this node; evaluation consumes the wake.
    np->beginUpdate(0.0);
    const int32_t h = np->httpRequest(
        (const uint8_t*)"https://api.example.com/x", 25, nullptr, 0, 0);
    (void)h;
    np->deliverHttp(h, kModuleNetStateFailed, 0, {});
    CHECK(node.wakePending(0.0));
    ctx.seconds = 1.0;
    node.evaluate(ctx);
    CHECK(fp->updates == 2);
    CHECK(!node.wakePending(1.0));

    // wake_after_ms: re-stated per update; due -> wakePending; a later
    // update writing 0 clears it.
    fp->onUpdate = [&](std::vector<uint8_t>& m) {
        const uint32_t ms = 500;
        std::memcpy(m.data() + kModuleHdrWakeAfterMs, &ms, 4);
    };
    ctx.seconds = 2.0;
    node.evaluate(ctx);
    CHECK(node.nextWake() == doctest::Approx(2.5));
    CHECK(!node.wakePending(2.4));
    CHECK(node.wakePending(2.6));
    fp->onUpdate = nullptr; // writes nothing; host cleared the field
    ctx.seconds = 2.6;
    node.evaluate(ctx);
    CHECK(node.nextWake() < 0.0);
    CHECK(!node.wakePending(100.0));
}
