#pragma once

// WASM logic modules (DESIGN.md §4.5): a module node's ports are declared
// in an interface JSON (there is no WGSL to reflect), and everything that
// crosses the module boundary is data through one I/O block — values,
// events, buffer contents — never a GPU handle.
//
// The block layout is computed here, host-side, from the interface alone:
//
//   DriftHeader (32 bytes)
//   value inputs, declaration order (f32 × components, 4-byte aligned)
//   value outputs, declaration order (same packing)
//   per buffer output: u32 count, u32 written, then capacity × stride
//                      bytes of staging
//
// Event inputs/outputs occupy no block space; they ride the header's
// events_in/events_out bitmasks by declaration order among their
// direction's events (≤ 32 each).
//
// The module exports drift_abi() (must return kAbiVersion),
// drift_init(ioSize) -> block pointer in module memory, and
// drift_update(). The host writes the header + input region, calls
// update, then reads outputs back. ModuleInstance abstracts where that
// memory lives: natively it is the embedded engine's linear memory; in
// the browser each module is its own WebAssembly.Instance whose memory
// is a separate ArrayBuffer from the runtime's heap, so the exchange is
// expressed as reads/writes, not pointers.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Value.h"

namespace drift::core {

inline constexpr uint32_t kModuleAbiVersion = 1;
inline constexpr uint32_t kModuleHeaderSize = 32;
inline constexpr uint32_t kModuleMaxEvents = 32; // per direction
// Guard against absurd staging demands (per buffer output).
inline constexpr uint64_t kModuleMaxBufferBytes = 64u << 20;

// Header field offsets (all little-endian, the WASM memory order).
enum ModuleHeaderField : uint32_t {
    kModuleHdrTime = 0,        // f32 scene time, seconds
    kModuleHdrDt = 4,          // f32 seconds since this node's last update
    kModuleHdrFrame = 8,       // u32 scene frame counter
    kModuleHdrFlags = 12,      // u32, bit 0 = first update after (re)load
    kModuleHdrEventsIn = 16,   // u32 bitmask over declared input events
    kModuleHdrEventsOut = 20,  // u32, module sets; host reads and clears
    kModuleHdrWakeAfterMs = 24, // u32, 0 = none (reserved; not yet honored)
    kModuleHdrReserved = 28,
};

struct ModuleInterface {
    struct Input {
        std::string name;
        ValueType type = ValueType::Scalar; // Scalar..Vec4 or Event
        Value def;              // value inputs; meaningless for events
        bool hasDefault = false;
        uint32_t offset = 0;    // io block offset (value inputs)
        int eventBit = -1;      // events_in bit (event inputs)
    };
    struct Output {
        std::string name;
        ValueType type = ValueType::Scalar; // Scalar..Vec4, Event, Buffer
        uint32_t offset = 0;    // value: f32s; buffer: {count, written} pair
        int eventBit = -1;      // events_out bit (event outputs)
        uint32_t stride = 0;    // buffer outputs (§18.1)
        uint32_t capacity = 0;
    };

    uint32_t abi = 0;
    std::vector<Input> inputs;
    std::vector<Output> outputs;
    // Set by computeLayout():
    uint32_t ioSize = 0;       // total block size
    uint32_t inputEnd = 0;     // header + value inputs: the host-written span
    uint32_t valueOutBegin = 0, valueOutEnd = 0; // packed value outputs

    // Parses and validates the interface JSON. On success the layout is
    // not yet computed — the loader may still override buffer capacities
    // (the compute-node pattern) — call computeLayout() after.
    static bool parse(const std::string& json, ModuleInterface& out,
                      std::string& error);
    void computeLayout();

    uint32_t bufferDataOffset(const Output& out) const
    {
        return out.offset + 8; // past {count, written}
    }
};

// One instantiated module (one per node — two nodes sharing a .wasm get
// private memories). update() returns false on a trap, with the engine's
// message in error; the node then goes dead rather than freezing the wall.
class ModuleInstance {
public:
    virtual ~ModuleInstance() = default;
    virtual bool writeIo(uint32_t offset, const void* src, uint32_t len) = 0;
    virtual bool readIo(uint32_t offset, void* dst, uint32_t len) = 0;
    virtual bool update(std::string& error) = 0;
};

// Platform factory handed to Scene::load (the VideoDecoderFactory
// pattern): wasm bytes + required io block size -> running instance, or
// nullptr with error set. A null factory means this runtime cannot run
// modules; scenes containing one fail to load with a clear message.
using ModuleLoader = std::function<std::unique_ptr<ModuleInstance>(
    const std::string& wasmBytes, uint32_t ioSize, std::string& error)>;

} // namespace drift::core
