#include "ModuleWasmtime.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>

#include <wasm.h>
#include <wasmtime.h>

namespace drift::platform {

namespace {

using drift::core::ModuleInstance;

// Watchdog cadence: the ticker bumps the engine epoch every 100ms and
// every guest call gets a 2-tick deadline, so a runaway module traps
// within 100–200ms — generous against any legitimate update() (a frame
// budget is ~16ms) while keeping the wall responsive.
constexpr auto kEpochTick = std::chrono::milliseconds(100);
constexpr uint64_t kDeadlineTicks = 2;
// Store limiter: a module's linear memory may grow to this and no
// further (its I/O block plus whatever its own allocator wants).
constexpr int64_t kMemoryLimit = 256ll << 20;

std::string messageOf(wasmtime_error_t* error)
{
    wasm_byte_vec_t msg;
    wasmtime_error_message(error, &msg);
    std::string out(msg.data, msg.size);
    wasm_byte_vec_delete(&msg);
    wasmtime_error_delete(error);
    return out;
}

std::string messageOf(wasm_trap_t* trap)
{
    wasm_byte_vec_t msg;
    wasm_trap_message(trap, &msg);
    std::string out(msg.data, msg.size);
    wasm_byte_vec_delete(&msg);
    wasm_trap_delete(trap);
    return out;
}

// env.drift_log(ptr, len): the one host import (§4.5).
wasm_trap_t* driftLog(void*, wasmtime_caller_t* caller,
                      const wasmtime_val_t* args, size_t nargs,
                      wasmtime_val_t*, size_t)
{
    if (nargs != 2) {
        return nullptr;
    }
    wasmtime_extern_t item;
    if (!wasmtime_caller_export_get(caller, "memory", 6, &item) ||
        item.kind != WASMTIME_EXTERN_MEMORY) {
        return nullptr;
    }
    wasmtime_context_t* ctx = wasmtime_caller_context(caller);
    const uint8_t* data = wasmtime_memory_data(ctx, &item.of.memory);
    const size_t size = wasmtime_memory_data_size(ctx, &item.of.memory);
    const uint64_t ptr = (uint32_t)args[0].of.i32;
    const uint64_t len = (uint32_t)args[1].of.i32;
    if (ptr + len <= size) {
        fprintf(stderr, "drift module: %.*s\n", (int)len,
                (const char*)data + ptr);
    }
    return nullptr;
}

// Process-wide engine + compiled-module cache + epoch ticker. Started by
// the first module node; the destructor (static teardown, after main's
// scene is gone) stops the ticker and frees the engine.
struct Runtime {
    wasm_engine_t* engine = nullptr;
    std::map<uint64_t, wasmtime_module_t*> modules; // by content hash
    std::thread ticker;
    std::atomic<bool> stop{ false };

    static wasm_engine_t* makeEngine(bool pulley)
    {
        wasm_config_t* config = wasm_config_new();
        wasmtime_config_epoch_interruption_set(config, true);
        // Golden stability (§4.5): canonical NaN payloads across machines
        // and backends.
        wasmtime_config_cranelift_nan_canonicalization_set(config, true);
        if (pulley) {
            if (wasmtime_error_t* err =
                    wasmtime_config_target_set(config, "pulley64")) {
                messageOf(err);
                wasm_config_delete(config);
                return nullptr;
            }
        }
        return wasm_engine_new_with_config(config);
    }

    Runtime()
    {
        // Pulley (interpreter, no executable-memory allocation) is the
        // hardened default; DRIFT_MODULE_JIT=1 opts into native Cranelift.
        // The stock C-API artifact is built without the Pulley feature —
        // its engines accept the target but refuse to compile for it — so
        // probe with an empty module and fall back.
        const char* jit = getenv("DRIFT_MODULE_JIT");
        if (!jit || strcmp(jit, "1") != 0) {
            engine = makeEngine(/*pulley=*/true);
            if (engine) {
                static const uint8_t kEmpty[] = { 0, 'a', 's', 'm',
                                                  1, 0,   0,   0 };
                wasmtime_module_t* probe = nullptr;
                if (wasmtime_error_t* err = wasmtime_module_new(
                        engine, kEmpty, sizeof(kEmpty), &probe)) {
                    messageOf(err);
                    wasm_engine_delete(engine);
                    engine = nullptr;
                    fprintf(stderr, "drift: wasmtime lacks Pulley; modules "
                                    "run on the Cranelift JIT\n");
                } else {
                    wasmtime_module_delete(probe);
                }
            }
        }
        if (!engine) {
            engine = makeEngine(/*pulley=*/false);
        }
        ticker = std::thread([this] {
            while (!stop.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(kEpochTick);
                wasmtime_engine_increment_epoch(engine);
            }
        });
    }

    ~Runtime()
    {
        stop.store(true, std::memory_order_relaxed);
        if (ticker.joinable()) {
            ticker.join();
        }
        for (auto& [hash, module] : modules) {
            wasmtime_module_delete(module);
        }
        if (engine) {
            wasm_engine_delete(engine);
        }
    }

    // Compile-once per unique wasm; modules are engine-level objects and
    // safely shared across stores (each node still gets its own instance
    // and memory).
    wasmtime_module_t* moduleFor(const std::string& bytes, std::string& error)
    {
        uint64_t hash = 1469598103934665603ull;
        for (unsigned char c : bytes) {
            hash = (hash ^ c) * 1099511628211ull;
        }
        if (auto it = modules.find(hash); it != modules.end()) {
            return it->second;
        }
        wasmtime_module_t* module = nullptr;
        if (wasmtime_error_t* err = wasmtime_module_new(
                engine, (const uint8_t*)bytes.data(), bytes.size(), &module)) {
            error = messageOf(err);
            return nullptr;
        }
        modules[hash] = module;
        return module;
    }
};

Runtime& runtime()
{
    static Runtime r;
    return r;
}

class WasmtimeModuleInstance : public ModuleInstance {
public:
    // Takes ownership of store on success only; use create().
    static std::unique_ptr<ModuleInstance> create(wasmtime_module_t* module,
                                                  uint32_t ioSize,
                                                  std::string& error)
    {
        auto self = std::unique_ptr<WasmtimeModuleInstance>(
            new WasmtimeModuleInstance());
        self->mStore = wasmtime_store_new(runtime().engine, nullptr, nullptr);
        self->mContext = wasmtime_store_context(self->mStore);
        wasmtime_store_limiter(self->mStore, kMemoryLimit, -1, 1, 1, 1);

        wasmtime_linker_t* linker = wasmtime_linker_new(runtime().engine);
        wasm_functype_t* logType = wasm_functype_new_2_0(
            wasm_valtype_new_i32(), wasm_valtype_new_i32());
        wasmtime_error_t* err = wasmtime_linker_define_func(
            linker, "env", 3, "drift_log", 9, logType, driftLog, nullptr,
            nullptr);
        wasm_functype_delete(logType);
        if (err) {
            error = messageOf(err);
            wasmtime_linker_delete(linker);
            return nullptr;
        }

        // Instantiation runs the start function, so it gets a deadline
        // like every other guest entry.
        wasmtime_context_set_epoch_deadline(self->mContext, kDeadlineTicks);
        wasm_trap_t* trap = nullptr;
        err = wasmtime_linker_instantiate(linker, self->mContext, module,
                                          &self->mInstance, &trap);
        wasmtime_linker_delete(linker);
        if (err) {
            error = messageOf(err);
            return nullptr;
        }
        if (trap) {
            error = "instantiation trapped: " + messageOf(trap);
            return nullptr;
        }

        const auto exportOf = [&](const char* name, size_t len,
                                  wasmtime_extern_kind_t kind,
                                  wasmtime_extern_t& out) {
            if (!wasmtime_instance_export_get(self->mContext,
                                              &self->mInstance, name, len,
                                              &out) ||
                out.kind != kind) {
                error = std::string("module does not export ") + name;
                return false;
            }
            return true;
        };
        wasmtime_extern_t abi, init, update, memory;
        if (!exportOf("drift_abi", 9, WASMTIME_EXTERN_FUNC, abi) ||
            !exportOf("drift_init", 10, WASMTIME_EXTERN_FUNC, init) ||
            !exportOf("drift_update", 12, WASMTIME_EXTERN_FUNC, update) ||
            !exportOf("memory", 6, WASMTIME_EXTERN_MEMORY, memory)) {
            return nullptr;
        }
        self->mUpdate = update.of.func;
        self->mMemory = memory.of.memory;

        // The §4.5 handshake: abi version, then the io block address.
        wasmtime_val_t arg, result;
        if (!self->call(abi.of.func, nullptr, 0, &result, 1, error)) {
            return nullptr;
        }
        if (result.kind != WASMTIME_I32 ||
            (uint32_t)result.of.i32 != drift::core::kModuleAbiVersion) {
            error = "unsupported module ABI " +
                    std::to_string(result.of.i32);
            return nullptr;
        }
        arg.kind = WASMTIME_I32;
        arg.of.i32 = (int32_t)ioSize;
        if (!self->call(init.of.func, &arg, 1, &result, 1, error)) {
            return nullptr;
        }
        const uint64_t io =
            result.kind == WASMTIME_I32 ? (uint32_t)result.of.i32 : 0;
        if (!io || io + ioSize > wasmtime_memory_data_size(self->mContext,
                                                           &self->mMemory)) {
            error = "drift_init returned a bad io block";
            return nullptr;
        }
        self->mIo = (uint32_t)io;
        self->mIoSize = ioSize;
        return self;
    }

    ~WasmtimeModuleInstance() override
    {
        if (mStore) {
            wasmtime_store_delete(mStore);
        }
    }

    bool writeIo(uint32_t offset, const void* src, uint32_t len) override
    {
        uint8_t* data = ioSpan(offset, len);
        if (!data) {
            return false;
        }
        std::memcpy(data, src, len);
        return true;
    }

    bool readIo(uint32_t offset, void* dst, uint32_t len) override
    {
        const uint8_t* data = ioSpan(offset, len);
        if (!data) {
            return false;
        }
        std::memcpy(dst, data, len);
        return true;
    }

    bool update(std::string& error) override
    {
        return call(mUpdate, nullptr, 0, nullptr, 0, error);
    }

private:
    WasmtimeModuleInstance() = default;

    // Every guest entry is armed with the watchdog deadline.
    bool call(const wasmtime_func_t& func, const wasmtime_val_t* args,
              size_t nargs, wasmtime_val_t* results, size_t nresults,
              std::string& error)
    {
        wasmtime_context_set_epoch_deadline(mContext, kDeadlineTicks);
        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* err = wasmtime_func_call(
            mContext, &func, args, nargs, results, nresults, &trap);
        if (err) {
            error = messageOf(err);
            return false;
        }
        if (trap) {
            error = messageOf(trap);
            return false;
        }
        return true;
    }

    uint8_t* ioSpan(uint32_t offset, uint32_t len)
    {
        if ((uint64_t)mIo + offset + len >
            wasmtime_memory_data_size(mContext, &mMemory)) {
            return nullptr;
        }
        return wasmtime_memory_data(mContext, &mMemory) + mIo + offset;
    }

    wasmtime_store_t* mStore = nullptr;
    wasmtime_context_t* mContext = nullptr;
    wasmtime_instance_t mInstance{};
    wasmtime_func_t mUpdate{};
    wasmtime_memory_t mMemory{};
    uint32_t mIo = 0, mIoSize = 0;
};

} // namespace

drift::core::ModuleLoader wasmtimeModuleLoader()
{
    return [](const std::string& wasmBytes, uint32_t ioSize,
              std::string& error) -> std::unique_ptr<ModuleInstance> {
        wasmtime_module_t* module = runtime().moduleFor(wasmBytes, error);
        if (!module) {
            return nullptr;
        }
        return WasmtimeModuleInstance::create(module, ioSize, error);
    };
}

} // namespace drift::platform
