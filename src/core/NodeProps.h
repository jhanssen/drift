#pragma once

#include <string>

// Declared node properties (§9, §13): the single source of truth for the
// per-type property surface. The loader derives its unknown-property
// warning from this table, and the web runtime serializes it
// (drift_node_props) so editors build creation forms and inspectors from
// the runtime instead of hardcoding node types.
//
// kind: "asset" (project-relative path), "enum" (options lists the legal
// values, space-separated), "bool", "int", "number", or "json"
// (structured — tracks, emitters, sheet, size; edited by richer tooling,
// not a creation form). def is the JSON-encoded value an absent property
// runs at; nullptr for required properties and for optional ones without
// a single default. The parse sites in SceneLoader.cpp stay the
// authority on semantics; a mismatch with this table is a bug.

namespace drift::core {

struct NodePropDef {
    const char* node;
    const char* name;
    const char* kind;
    bool required;
    const char* def;     // JSON-encoded
    const char* options; // enum kinds: space-separated legal values
};

inline constexpr NodePropDef kNodeProps[] = {
    { "image", "src", "asset", true, nullptr, nullptr },
    { "video", "src", "asset", true, nullptr, nullptr },
    { "video", "loop", "bool", false, "true", nullptr },
    { "shader", "shader", "asset", true, nullptr, nullptr },
    { "shader", "size", "json", false, "\"auto\"", nullptr },
    { "compute", "shader", "asset", true, nullptr, nullptr },
    { "compute", "capacity", "int", false, nullptr, nullptr },
    { "compute", "dispatch", "json", false, "\"auto\"", nullptr },
    { "wave", "shape", "enum", false, "\"sine\"",
      "sine triangle saw square" },
    { "remap", "clamp", "bool", false, "true", nullptr },
    { "sequence", "duration", "number", true, nullptr, nullptr },
    { "sequence", "loop", "bool", false, "true", nullptr },
    { "sequence", "tracks", "json", false, nullptr, nullptr },
    { "particles", "capacity", "int", false, "1000", nullptr },
    { "particles", "emitter", "enum", false, "\"point\"",
      "point box disc" },
    { "particles", "spawnCount", "int", false, nullptr, nullptr },
    { "particles", "spawnMode", "enum", false, "\"death\"",
      "death birth life" },
    { "particles", "emitters", "json", false, nullptr, nullptr },
    { "sprites", "blend", "enum", false, "\"add\"", "add over" },
    { "sprites", "sheet", "json", false, nullptr, nullptr },
    { "trails", "blend", "enum", false, "\"add\"", "add over" },
    { "trails", "length", "int", false, "16", nullptr },
    { "compositor", "blend", "json", false, nullptr, nullptr },
    { "edge", "mode", "enum", false, "\"rise\"", "rise fall both" },
    { "fit", "mode", "enum", false, "\"cover\"", "cover contain stretch" },
    { "graph", "graph", "asset", true, nullptr, nullptr },
    { "module", "module", "asset", true, nullptr, nullptr },
    { "module", "interface", "asset", true, nullptr, nullptr },
    { "module", "capacity", "json", false, nullptr, nullptr },
};

// The per-type port surface (input/output names, wire categories,
// defaults) as JSON for editors (drift_node_ports). Defined in
// SceneLoader.cpp so the loader's own port tables stay the single source
// of truth; reflected/dynamic ports (shader/compute WGSL, §19.2 graph
// interfaces, sequence tracks) are omitted for editors to resolve from
// the document.
std::string nodePortsJson();

// Every §9 node type, including the property-less ones — the loader
// warns on unknown properties for all of them.
inline constexpr const char* kNodeTypes[] = {
    "image", "video",  "shader",    "compute", "wave",   "remap",
    "add", "multiply", "mix", "clamp", "noise", "damp", "edge",
    "combine", "split", "sequence", "particles", "sprites", "trails",
    "transform", "fit", "compositor", "output", "graph", "module",
};

} // namespace drift::core
