#include "Module.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <glaze/glaze.hpp>

namespace drift::core {

namespace {

bool validPortName(const std::string& s)
{
    if (s.empty() || (!isalpha((unsigned char)s[0]) && s[0] != '_')) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return isalnum(c) || c == '_';
    });
}

bool parseType(const std::string& s, ValueType& out, bool allowBuffer)
{
    if (s == "scalar") { out = ValueType::Scalar; return true; }
    if (s == "vec2") { out = ValueType::Vec2; return true; }
    if (s == "vec3") { out = ValueType::Vec3; return true; }
    if (s == "vec4") { out = ValueType::Vec4; return true; }
    if (s == "event") { out = ValueType::Event; return true; }
    if (allowBuffer && s == "buffer") { out = ValueType::Buffer; return true; }
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
        out.type = arr.size() == 2 ? ValueType::Vec2
                 : arr.size() == 3 ? ValueType::Vec3
                                   : ValueType::Vec4;
        for (size_t i = 0; i < arr.size(); ++i) {
            if (!arr[i].is_number()) {
                return false;
            }
            out.v[i] = arr[i].get_number();
        }
        return true;
    }
    return false;
}

// Shared by the interface's request block and grant records' granted
// block — one grammar, so the consent display and the enforced policy
// cannot diverge structurally.
bool parsePermissions(const glz::generic& spec, ModulePermissions& out,
                      std::string& error)
{
    if (!spec.is_object()) {
        error = "'permissions' must be an object";
        return false;
    }
    const auto& obj = spec.get_object();
    if (auto it = obj.find("storage"); it != obj.end()) {
        if (!it->second.is_object()) {
            error = "'permissions.storage' must be an object";
            return false;
        }
        const auto& storage = it->second.get_object();
        auto quotaIt = storage.find("quota");
        if (quotaIt == storage.end() || !quotaIt->second.is_number() ||
            quotaIt->second.get_number() <= 0 ||
            quotaIt->second.get_number() !=
                (double)(uint64_t)quotaIt->second.get_number() ||
            (uint64_t)quotaIt->second.get_number() > kModuleMaxStorageQuota) {
            error = "'permissions.storage.quota' must be a positive integer "
                    "of at most " +
                    std::to_string(kModuleMaxStorageQuota >> 20) + " MiB";
            return false;
        }
        out.storageQuota = (uint64_t)quotaIt->second.get_number();
    }
    if (auto it = obj.find("network"); it != obj.end()) {
        if (!it->second.is_object()) {
            error = "'permissions.network' must be an object";
            return false;
        }
        const auto& network = it->second.get_object();
        auto originsIt = network.find("origins");
        if (originsIt == network.end() || !originsIt->second.is_array() ||
            originsIt->second.get_array().empty()) {
            error = "'permissions.network.origins' must be a non-empty array";
            return false;
        }
        const auto& arr = originsIt->second.get_array();
        if (arr.size() > kModuleMaxOrigins) {
            error = "more than " + std::to_string(kModuleMaxOrigins) +
                    " network origins";
            return false;
        }
        for (const auto& entry : arr) {
            if (!entry.is_string() ||
                !ModulePermissions::validOrigin(entry.get_string())) {
                error = "network origin must be https://host[:port] "
                        "(plain http only for localhost)";
                return false;
            }
            out.networkOrigins.push_back(entry.get_string());
        }
        std::sort(out.networkOrigins.begin(), out.networkOrigins.end());
        out.networkOrigins.erase(std::unique(out.networkOrigins.begin(),
                                             out.networkOrigins.end()),
                                 out.networkOrigins.end());
    }
    return true;
}

} // namespace

bool ModulePermissions::validOrigin(const std::string& origin)
{
    std::string rest;
    bool localOnly = false;
    if (origin.starts_with("https://")) {
        rest = origin.substr(8);
    } else if (origin.starts_with("http://")) {
        rest = origin.substr(7);
        localOnly = true;
    } else {
        return false;
    }
    std::string host;
    if (!rest.empty() && rest[0] == '[') { // bracketed IPv6 literal
        const size_t bracket = rest.find(']');
        if (bracket == std::string::npos) {
            return false;
        }
        host = rest.substr(0, bracket + 1);
        const std::string inner = rest.substr(1, bracket - 1);
        if (inner.empty() ||
            inner.find_first_not_of("0123456789abcdef:.") !=
                std::string::npos) {
            return false;
        }
        rest = rest.substr(bracket + 1);
    } else {
        const size_t colon = rest.find(':');
        host = colon == std::string::npos ? rest : rest.substr(0, colon);
        rest = colon == std::string::npos ? std::string()
                                          : rest.substr(colon);
        if (host.empty() ||
            host.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-") !=
                std::string::npos) {
            return false;
        }
    }
    if (!rest.empty()) { // :port
        if (rest[0] != ':' || rest.size() < 2 || rest.size() > 6 ||
            rest.find_first_not_of("0123456789", 1) != std::string::npos) {
            return false;
        }
    }
    if (localOnly) {
        return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
    }
    return true;
}

bool ModulePermissions::coveredBy(const ModulePermissions& granted,
                                  std::vector<std::string>& missing) const
{
    const size_t before = missing.size();
    if (storageQuota > granted.storageQuota) {
        missing.push_back(
            "storage (" + std::to_string(storageQuota) + " bytes)");
    }
    for (const std::string& origin : networkOrigins) {
        if (!std::binary_search(granted.networkOrigins.begin(),
                                granted.networkOrigins.end(), origin)) {
            missing.push_back("network origin " + origin);
        }
    }
    return missing.size() == before;
}

// ---- ModuleStorage (§4.4) ----

namespace {

// Blob format: "DMS1", u32 count, then per entry u32 klen, u32 vlen,
// key bytes, value bytes. Tolerant loading: anything malformed reads as
// an empty store (never a load failure — state is a cache, not content).
constexpr char kStorageMagic[4] = { 'D', 'M', 'S', '1' };

void putU32(std::string& out, uint32_t v)
{
    out.append((const char*)&v, 4);
}

bool getU32(const std::string& in, size_t& off, uint32_t& v)
{
    if (off + 4 > in.size()) {
        return false;
    }
    std::memcpy(&v, in.data() + off, 4);
    off += 4;
    return true;
}

} // namespace

ModuleStorage::~ModuleStorage()
{
    if (mDirty && mBackend) {
        mBackend->save(mNs, serialize());
    }
}

void ModuleStorage::attach(std::shared_ptr<ModuleStoragePersistence> backend,
                           std::string ns)
{
    mBackend = std::move(backend);
    mNs = std::move(ns);
    std::string blob;
    if (mBackend && mBackend->load(mNs, blob)) {
        deserialize(blob);
    }
}

int32_t ModuleStorage::get(const uint8_t* key, uint32_t klen, uint8_t* dst,
                           uint32_t dcap) const
{
    if (klen == 0 || klen > kModuleStorageMaxKey) {
        return kModuleStorageInvalid;
    }
    auto it = mEntries.find(std::string((const char*)key, klen));
    if (it == mEntries.end()) {
        return kModuleStorageMissing;
    }
    const std::string& value = it->second;
    if (dst && dcap) {
        std::memcpy(dst, value.data(),
                    std::min<size_t>(value.size(), dcap));
    }
    return (int32_t)value.size();
}

int32_t ModuleStorage::put(const uint8_t* key, uint32_t klen,
                           const uint8_t* val, uint32_t vlen)
{
    if (mQuota == 0) {
        return kModuleStorageDenied; // ungranted (§4.4 soft-deny)
    }
    if (klen == 0 || klen > kModuleStorageMaxKey) {
        return kModuleStorageInvalid;
    }
    std::string k((const char*)key, klen);
    auto it = mEntries.find(k);
    const uint64_t oldBytes =
        it == mEntries.end() ? 0 : klen + it->second.size();
    const uint64_t newUsed = mUsed - oldBytes + klen + vlen;
    if (newUsed > mQuota) {
        return kModuleStorageQuota;
    }
    mEntries[std::move(k)] = std::string((const char*)val, vlen);
    mUsed = newUsed;
    mDirty = true;
    return 0;
}

int32_t ModuleStorage::erase(const uint8_t* key, uint32_t klen)
{
    if (mQuota == 0) {
        return kModuleStorageDenied;
    }
    if (klen == 0 || klen > kModuleStorageMaxKey) {
        return kModuleStorageInvalid;
    }
    auto it = mEntries.find(std::string((const char*)key, klen));
    if (it != mEntries.end()) {
        mUsed -= klen + it->second.size();
        mEntries.erase(it);
        mDirty = true;
    }
    return 0; // idempotent
}

int32_t ModuleStorage::keys(uint8_t* dst, uint32_t dcap) const
{
    if (mQuota == 0) {
        return kModuleStorageDenied;
    }
    uint64_t total = 0;
    for (const auto& [k, v] : mEntries) {
        total += k.size() + 1;
    }
    if (dst && dcap) {
        uint32_t off = 0;
        for (const auto& [k, v] : mEntries) {
            for (size_t i = 0; i < k.size() + 1 && off < dcap; ++i) {
                dst[off++] = i < k.size() ? (uint8_t)k[i] : 0;
            }
        }
    }
    return (int32_t)total;
}

void ModuleStorage::maybeFlush(double now)
{
    if (!mDirty || !mBackend ||
        now - mLastFlush < kModuleStorageFlushSeconds) {
        return;
    }
    mBackend->save(mNs, serialize());
    mDirty = false;
    mLastFlush = now;
}

std::string ModuleStorage::serialize() const
{
    std::string out(kStorageMagic, 4);
    putU32(out, (uint32_t)mEntries.size());
    for (const auto& [k, v] : mEntries) {
        putU32(out, (uint32_t)k.size());
        putU32(out, (uint32_t)v.size());
        out += k;
        out += v;
    }
    return out;
}

void ModuleStorage::deserialize(const std::string& blob)
{
    mEntries.clear();
    mUsed = 0;
    if (blob.size() < 8 || std::memcmp(blob.data(), kStorageMagic, 4) != 0) {
        return;
    }
    size_t off = 4;
    uint32_t count = 0;
    getU32(blob, off, count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t klen = 0, vlen = 0;
        if (!getU32(blob, off, klen) || !getU32(blob, off, vlen) ||
            off + (uint64_t)klen + vlen > blob.size() || klen == 0 ||
            klen > kModuleStorageMaxKey) {
            mEntries.clear();
            mUsed = 0;
            return; // corrupt: empty store, never a failure
        }
        std::string k = blob.substr(off, klen);
        off += klen;
        std::string v = blob.substr(off, vlen);
        off += vlen;
        const uint64_t bytes = klen + vlen;
        if (mUsed + bytes > mQuota) {
            break; // quota shrank since this blob was written: keep what fits
        }
        mUsed += bytes;
        mEntries[std::move(k)] = std::move(v);
    }
}

bool parseGrantRecord(const std::string& json, ModulePermissions& out)
{
    glz::generic doc{};
    if (auto ec = glz::read_json(doc, json); ec) {
        return false;
    }
    if (!doc.is_object()) {
        return false;
    }
    const auto& top = doc.get_object();
    auto it = top.find("permissions");
    if (it == top.end()) {
        return false;
    }
    std::string error;
    ModulePermissions granted;
    if (!parsePermissions(it->second, granted, error) || granted.empty()) {
        return false;
    }
    out = granted;
    return true;
}

bool ModuleInterface::parse(const std::string& json, ModuleInterface& out,
                            std::string& error)
{
    glz::generic doc{};
    if (auto ec = glz::read_json(doc, json); ec) {
        error = glz::format_error(ec, json);
        return false;
    }
    if (!doc.is_object()) {
        error = "top level must be an object";
        return false;
    }
    const auto& top = doc.get_object();

    auto abiIt = top.find("abi");
    if (abiIt == top.end() || !abiIt->second.is_number() ||
        (uint32_t)abiIt->second.get_number() != kModuleAbiVersion) {
        error = "missing or unsupported 'abi' (expected " +
                std::to_string(kModuleAbiVersion) + ")";
        return false;
    }
    out.abi = kModuleAbiVersion;

    // §4.4 capability requests. Declared here — the interface travels
    // with the module and is what the host reads; tooling derives any
    // package-level display from these.
    if (auto it = top.find("permissions"); it != top.end()) {
        if (!parsePermissions(it->second, out.permissions, error)) {
            return false;
        }
    }

    // Ports are declared as JSON objects (DESIGN.md §4.5). The canonical
    // port order — block layout, event bit assignment, node port order —
    // is lexicographic by name within each direction, sorted here
    // explicitly, so no tool that rewrites the JSON with reordered keys
    // can change the ABI.
    const auto sortedEntries = [](const glz::generic& obj) {
        std::vector<std::pair<std::string, const glz::generic*>> entries;
        for (const auto& [name, spec] : obj.get_object()) {
            entries.emplace_back(name, &spec);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        return entries;
    };

    int eventBits = 0;
    if (auto it = top.find("inputs"); it != top.end()) {
        if (!it->second.is_object()) {
            error = "'inputs' must be an object";
            return false;
        }
        for (const auto& [name, specPtr] : sortedEntries(it->second)) {
            const glz::generic& spec = *specPtr;
            if (!validPortName(name)) {
                error = "input '" + name + "': invalid port name";
                return false;
            }
            if (!spec.is_object()) {
                error = "input '" + name + "': must be an object";
                return false;
            }
            const auto& obj = spec.get_object();
            Input in;
            in.name = name;
            auto typeIt = obj.find("type");
            if (typeIt == obj.end() || !typeIt->second.is_string() ||
                !parseType(typeIt->second.get_string(), in.type,
                           /*allowBuffer=*/false)) {
                error = "input '" + name +
                        "': 'type' must be scalar/vec2/vec3/vec4/event";
                return false;
            }
            if (auto defIt = obj.find("default"); defIt != obj.end()) {
                if (in.type == ValueType::Event) {
                    error = "input '" + name + "': events take no default";
                    return false;
                }
                if (!parseLiteral(defIt->second, in.def)) {
                    error = "input '" + name + "': bad 'default' literal";
                    return false;
                }
                if (in.def.type == ValueType::Scalar &&
                    in.type != ValueType::Scalar) {
                    const double s = in.def.v[0]; // §4: scalar splats
                    in.def.type = in.type;
                    in.def.v = { s, s, s, s };
                }
                if (in.def.type != in.type) {
                    error = "input '" + name + "': 'default' does not match "
                            "the declared type";
                    return false;
                }
                in.hasDefault = true;
            }
            if (in.type == ValueType::Event) {
                if (eventBits >= (int)kModuleMaxEvents) {
                    error = "more than " + std::to_string(kModuleMaxEvents) +
                            " event inputs";
                    return false;
                }
                in.eventBit = eventBits++;
            } else {
                in.def.type = in.type; // unset default reads as zeroes
            }
            out.inputs.push_back(std::move(in));
        }
    }

    auto outsIt = top.find("outputs");
    if (outsIt == top.end() || !outsIt->second.is_object() ||
        outsIt->second.get_object().empty()) {
        error = "'outputs' must be a non-empty object";
        return false;
    }
    eventBits = 0;
    for (const auto& [name, specPtr] : sortedEntries(outsIt->second)) {
        const glz::generic& spec = *specPtr;
        if (!validPortName(name)) {
            error = "output '" + name + "': invalid port name";
            return false;
        }
        if (!spec.is_object()) {
            error = "output '" + name + "': must be an object";
            return false;
        }
        const auto& obj = spec.get_object();
        Output o;
        o.name = name;
        auto typeIt = obj.find("type");
        if (typeIt == obj.end() || !typeIt->second.is_string() ||
            !parseType(typeIt->second.get_string(), o.type,
                       /*allowBuffer=*/true)) {
            error = "output '" + name +
                    "': 'type' must be scalar/vec2/vec3/vec4/event/buffer";
            return false;
        }
        const auto uintField = [&](const char* key, uint32_t& val) -> bool {
            auto it = obj.find(key);
            if (it == obj.end() || !it->second.is_number() ||
                it->second.get_number() <= 0 ||
                it->second.get_number() !=
                    (double)(uint32_t)it->second.get_number()) {
                return false;
            }
            val = (uint32_t)it->second.get_number();
            return true;
        };
        if (o.type == ValueType::Buffer) {
            if (!uintField("stride", o.stride) || o.stride % 4 != 0) {
                error = "output '" + name + "': buffer needs a positive "
                        "'stride' that is a multiple of 4";
                return false;
            }
            if (!uintField("capacity", o.capacity)) {
                error = "output '" + name +
                        "': buffer needs a positive integer 'capacity'";
                return false;
            }
            if ((uint64_t)o.stride * o.capacity > kModuleMaxBufferBytes) {
                error = "output '" + name + "': stride × capacity exceeds " +
                        std::to_string(kModuleMaxBufferBytes >> 20) + " MiB";
                return false;
            }
        } else if (obj.find("stride") != obj.end() ||
                   obj.find("capacity") != obj.end()) {
            error = "output '" + name +
                    "': 'stride'/'capacity' apply to buffer outputs only";
            return false;
        }
        if (o.type == ValueType::Event) {
            if (eventBits >= (int)kModuleMaxEvents) {
                error = "more than " + std::to_string(kModuleMaxEvents) +
                        " event outputs";
                return false;
            }
            o.eventBit = eventBits++;
        }
        out.outputs.push_back(std::move(o));
    }

    return true;
}

void ModuleInterface::computeLayout()
{
    uint32_t offset = kModuleHeaderSize;
    for (Input& in : inputs) {
        if (in.type == ValueType::Event) {
            continue;
        }
        in.offset = offset;
        offset += 4 * componentCount(in.type);
    }
    inputEnd = offset;
    valueOutBegin = offset;
    for (Output& o : outputs) {
        if (o.type == ValueType::Event || o.type == ValueType::Buffer) {
            continue; // events ride the header; buffers go last, below
        }
        o.offset = offset;
        offset += 4 * componentCount(o.type);
    }
    valueOutEnd = offset;
    for (Output& o : outputs) {
        if (o.type != ValueType::Buffer) {
            continue;
        }
        o.offset = offset;                  // {count, written}
        offset += 8 + o.stride * o.capacity; // then the staging bytes
    }
    ioSize = offset;
}

} // namespace drift::core
