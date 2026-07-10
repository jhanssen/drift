#pragma once

// The native WASM module engine (DESIGN.md §4.5): Wasmtime via the C API,
// WASI disabled (the drift host imports are the only capability surface),
// epoch interruption armed around every guest call as the runaway-module
// watchdog, NaN canonicalization on for golden stability, and the Pulley
// interpreter as the hardened default (no executable-memory allocation —
// MemoryDenyWriteExecute-compatible); DRIFT_MODULE_JIT=1 opts into the
// Cranelift JIT. One store/instance per node; compiled modules are cached
// by content hash across nodes.

#include "core/Module.h"

namespace drift::platform {

// The process-wide ModuleLoader for Scene::load. Engine and watchdog
// start lazily on the first module and live for the process.
drift::core::ModuleLoader wasmtimeModuleLoader();

} // namespace drift::platform
