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
#include <map>
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
// §4.4 capability guards.
inline constexpr uint64_t kModuleMaxStorageQuota = 256u << 20;
inline constexpr size_t kModuleMaxOrigins = 16;

// §4.4 declared capabilities. The module's interface JSON is where a
// module *requests* them (derive-not-duplicate: tooling unions these for
// package-level display; there is no manifest copy to drift out of sync).
// The same structure holds the *granted* policy read back from a grant
// record — and the record is what the runtime enforces: requests beyond
// it are denied with the defined offline-equivalent error, never a load
// failure (§4.4 soft-deny).
struct ModulePermissions {
    uint64_t storageQuota = 0;              // 0 = no storage capability
    std::vector<std::string> networkOrigins; // exact origins, sorted

    bool empty() const { return storageQuota == 0 && networkOrigins.empty(); }

    // True when this request is fully covered by `granted`; otherwise
    // appends one human-readable line per missing capability (the load
    // warning bodies).
    bool coveredBy(const ModulePermissions& granted,
                   std::vector<std::string>& missing) const;

    // Validates one origin: https://host[:port], or plain http for
    // localhost/127.0.0.1/[::1] (dev only). No path, no wildcard.
    static bool validOrigin(const std::string& origin);
};

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
    ModulePermissions permissions; // §4.4 requests; empty = none
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
// storage (may be null: module declared none) is the node's §4.4 store;
// the engine wires the drift_storage_* imports to it and must not
// outlive it (the node owns both, storage destroyed last).
class ModuleStorage;
using ModuleLoader = std::function<std::unique_ptr<ModuleInstance>(
    const std::string& wasmBytes, uint32_t ioSize, ModuleStorage* storage,
    std::string& error)>;

// ---- §4.4 storage capability ----
// A synchronous key-value store per (scene, module): the working copy is
// a host-memory map (read-your-writes, identical on both targets — no
// worker migration, no async ABI), persisted through a platform blob
// backend with debounced write-behind. No backend means in-memory only —
// which IS the headless/golden behavior: deterministic empty start.
//
// Host imports (all synchronous; key ≤ 256 bytes):
//   drift_storage_get(kptr, klen, dptr, dcap) -> i32
//       full value size (copies min(size, dcap) bytes; dcap 0 probes),
//       or kModuleStorageMissing / kModuleStorageDenied
//   drift_storage_put(kptr, klen, vptr, vlen) -> i32   0 or error
//   drift_storage_delete(kptr, klen) -> i32            0 or error
//   drift_storage_keys(dptr, dcap) -> i32
//       total bytes of all keys each terminated by '\0' (copies what
//       fits), or kModuleStorageDenied
//
// Quota is the *granted* policy (the §4.4 record), counted as
// Σ(klen + vlen). Ungranted = quota 0: writes are denied, reads miss.
inline constexpr int32_t kModuleStorageMissing = -1;
inline constexpr int32_t kModuleStorageDenied = -2;
inline constexpr int32_t kModuleStorageQuota = -3;
inline constexpr int32_t kModuleStorageInvalid = -4;
inline constexpr uint32_t kModuleStorageMaxKey = 256;
inline constexpr double kModuleStorageFlushSeconds = 1.0;

// Platform blob persistence: one opaque blob per namespace (the node's
// document-unique id; the backend instance is already per-project).
// save() may complete asynchronously and must tolerate being called
// during teardown.
class ModuleStoragePersistence {
public:
    virtual ~ModuleStoragePersistence() = default;
    virtual bool load(const std::string& ns, std::string& blob) = 0;
    virtual void save(const std::string& ns, const std::string& blob) = 0;
};

class ModuleStorage {
public:
    explicit ModuleStorage(uint64_t quota)
        : mQuota(quota)
    {
    }
    ~ModuleStorage(); // flushes if dirty

    // Loads the namespace's blob from the backend (tolerates a missing
    // or corrupt blob: starts empty). Without attach the store is
    // in-memory only.
    void attach(std::shared_ptr<ModuleStoragePersistence> backend,
                std::string ns);

    int32_t get(const uint8_t* key, uint32_t klen, uint8_t* dst,
                uint32_t dcap) const;
    int32_t put(const uint8_t* key, uint32_t klen, const uint8_t* val,
                uint32_t vlen);
    int32_t erase(const uint8_t* key, uint32_t klen);
    int32_t keys(uint8_t* dst, uint32_t dcap) const;

    // Debounced write-behind: called after each module update with scene
    // time; persists at most every kModuleStorageFlushSeconds.
    void maybeFlush(double now);

    uint64_t used() const { return mUsed; }

private:
    std::string serialize() const;
    void deserialize(const std::string& blob);

    const uint64_t mQuota;
    std::map<std::string, std::string> mEntries;
    uint64_t mUsed = 0;
    std::shared_ptr<ModuleStoragePersistence> mBackend;
    std::string mNs;
    bool mDirty = false;
    double mLastFlush = -1e300;
};

// Parses a grant record's "permissions" into the granted policy (§4.4).
// Grant records are written by consent surfaces — driftpkg at install
// (the store's .installed.json), `driftpkg grant` for projects, editors
// later — and only ever *read* by the runtime. Returns false when the
// record is absent, unparseable, or grants nothing.
bool parseGrantRecord(const std::string& json, ModulePermissions& out);

// Everything the platform contributes to module nodes. Package modules'
// grants resolve through the ordinary asset reader (the package's
// .installed.json travels with the store); projectGrants covers modules
// living in the project itself. Null members degrade legibly: no loader
// fails module scenes with a clear message, no grants means ungranted
// (soft-deny warnings, §4.4).
struct ModulePlatform {
    ModuleLoader load;
    std::function<bool(ModulePermissions&)> projectGrants;
    // §4.4 storage blob persistence, already scoped to the project. Null
    // means in-memory-only stores — the headless/golden configuration
    // (deterministic empty start every run).
    std::shared_ptr<ModuleStoragePersistence> storage;
};

} // namespace drift::core
