#include <doctest/doctest.h>

#include <chrono>
#include <fstream>
#include <sstream>

#include <wasm.h>
#include <wasmtime.h>

#include "core/Nodes.h"
#include "platform/ModuleWasmtime.h"

// The native module engine (DESIGN.md §4.5): the real Wasmtime embedding
// driving the real spring.wasm from the example project, plus the
// watchdog and handshake failure paths via tiny WAT-assembled modules.

using namespace drift::core;

namespace {

std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string wat2wasm(const char* wat)
{
    wasm_byte_vec_t bytes;
    wasmtime_error_t* err = wasmtime_wat2wasm(wat, strlen(wat), &bytes);
    REQUIRE(err == nullptr);
    std::string out(bytes.data, bytes.size);
    wasm_byte_vec_delete(&bytes);
    return out;
}

} // namespace

TEST_CASE("wasmtime: the example spring module runs natively (§4.5)")
{
    const std::string base =
        std::string(DRIFT_SOURCE_DIR) + "/examples/spring.sceneproject/";
    ModuleInterface iface;
    std::string err;
    REQUIRE_MESSAGE(
        ModuleInterface::parse(readFile(base + "modules/spring.json"), iface,
                               err),
        err);
    iface.computeLayout();

    auto loader = drift::platform::wasmtimeModuleLoader();
    auto instance =
        loader(readFile(base + "modules/spring.wasm"), iface.ioSize, err);
    REQUIRE_MESSAGE(instance != nullptr, err);

    // Drive the real wasm through the real node. Inputs sorted:
    // damping, stiffness, target, tick; output: position.
    ModuleNode node(std::move(instance), iface);
    node.inputs.resize(iface.inputs.size());
    node.inputs[0].constant = Value{ ValueType::Scalar, { 6.0 } };
    node.inputs[1].constant = Value{ ValueType::Scalar, { 90.0 } };
    node.inputs[2].constant = Value{ ValueType::Vec2, { 0.8, 0.3 } };

    FrameContext ctx{};
    ctx.seconds = 0.0;
    node.evaluate(ctx);
    node.firstEvaluate = false;
    // First update snaps to the target.
    CHECK(node.outputs[0].value.v[0] == doctest::Approx(0.8));
    CHECK(node.outputs[0].value.v[1] == doctest::Approx(0.3));

    // Retarget by teleporting the constant; the spring must converge
    // (through real f32 integration in the wasm) over two seconds.
    node.inputs[2].constant = Value{ ValueType::Vec2, { 0.2, 0.7 } };
    bool overshot = false;
    for (int frame = 1; frame <= 120; ++frame) {
        ctx.seconds = frame / 60.0;
        node.evaluate(ctx);
        overshot = overshot || node.outputs[0].value.v[0] < 0.2;
    }
    CHECK(node.outputs[0].value.v[0] == doctest::Approx(0.2).epsilon(0.05));
    CHECK(node.outputs[0].value.v[1] == doctest::Approx(0.7).epsilon(0.05));
    CHECK(overshot); // it is a spring, not a lerp
}

TEST_CASE("wasmtime: the watchdog traps a runaway update (§4.5)")
{
    // An update() that never returns: the epoch deadline must trap it in
    // ~kEpochTick×2 rather than freezing the wall.
    const std::string wasm = wat2wasm(R"((module
        (memory (export "memory") 1)
        (func (export "drift_abi") (result i32) i32.const 1)
        (func (export "drift_init") (param i32) (result i32) i32.const 1024)
        (func (export "drift_update") (loop $spin br $spin))))");

    auto loader = drift::platform::wasmtimeModuleLoader();
    std::string err;
    auto instance = loader(wasm, 64, err);
    REQUIRE_MESSAGE(instance != nullptr, err);

    const auto start = std::chrono::steady_clock::now();
    CHECK(!instance->update(err));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(!err.empty());
    CHECK(elapsed < std::chrono::seconds(5));
}

TEST_CASE("wasmtime: handshake failures are load errors (§4.5)")
{
    auto loader = drift::platform::wasmtimeModuleLoader();
    std::string err;

    // Wrong ABI version.
    CHECK(loader(wat2wasm(R"((module
        (memory (export "memory") 1)
        (func (export "drift_abi") (result i32) i32.const 99)
        (func (export "drift_init") (param i32) (result i32) i32.const 1024)
        (func (export "drift_update"))))"),
                 64, err) == nullptr);
    CHECK(err.find("ABI") != std::string::npos);

    // Missing exports.
    CHECK(loader(wat2wasm("(module)"), 64, err) == nullptr);
    CHECK(err.find("does not export") != std::string::npos);

    // An io block that does not fit the module's memory.
    CHECK(loader(wat2wasm(R"((module
        (memory (export "memory") 1)
        (func (export "drift_abi") (result i32) i32.const 1)
        (func (export "drift_init") (param i32) (result i32) i32.const 0)
        (func (export "drift_update"))))"),
                 64, err) == nullptr);
    CHECK(err.find("bad io block") != std::string::npos);

    // Not wasm at all.
    CHECK(loader("garbage", 64, err) == nullptr);
    CHECK(!err.empty());
}
