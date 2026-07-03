# Scene Format Specification (v1 draft)

Status: working draft. This describes the hand-authorable `.sceneproject` format
consumed by the v1 runtime. The packaged `.wallpkg` distribution format is out of
scope here (deferred; see DESIGN.md §13.3).

## 1. Project Layout

```
myscene.sceneproject/
  scene.json          # the scene graph (this spec)
  shaders/            # WGSL files referenced by shader nodes
  assets/             # images, videos
```

`modules/` and `graphs/` (see DESIGN.md §6.1) are reserved for deferred features
and ignored by the v1 runtime if present.

All paths inside `scene.json` are relative to the `.sceneproject` root. Paths
must not escape the project root (`..` is rejected at load).

## 2. `scene.json` Top Level

```json
{
  "version": 1,
  "name": "Nebula",
  "author": "optional",
  "description": "optional",
  "parameters": { },
  "nodes": [ ]
}
```

- `version` (required, integer): format version. The runtime rejects scenes with
  a version it does not support.
- `name` (required, string): display name.
- `author`, `description` (optional, string): metadata.
- `parameters` (optional, object): user-tweakable scene parameters (§6).
- `nodes` (required, array): the scene graph (§8).

Unknown top-level fields are a load warning, not an error (forward
compatibility within a version).

## 3. Identifiers

Node ids and parameter names match `[A-Za-z_][A-Za-z0-9_]*`. Node ids must be
unique within a scene. (No `.` or `$` — both are reserved by the reference
grammar.)

The ids `time` and `mouse` are reserved: they name the implicit input nodes
(§9.7–§9.8) and may not be declared in `nodes`.

## 4. Type System

Edge/value types in v1:

| Type     | JSON literal form            | Notes                              |
|----------|------------------------------|------------------------------------|
| `scalar` | `0.5`                        | f32                                |
| `vec2`   | `[x, y]`                     |                                    |
| `vec3`   | `[x, y, z]`                  |                                    |
| `vec4`   | `[x, y, z, w]`               | also used for colors (RGBA)        |
| `texture`| — (connections only)         | 2D texture, premultiplied alpha    |
| `bool`   | `true` / `false`             | properties only, not wireable      |
| `string` | `"..."`                      | properties only, not wireable      |

Reserved for future versions: `event`, `buffer`.

Rules:

- Typing is static; all connections are checked at load time.
- Exactly one implicit conversion exists: a `scalar` may connect to (or be the
  literal for) any `vecN` input — it splats to all components. No other
  implicit conversions.
- Some value-node ports are **polymorphic** (written `T`): the port's type is
  resolved at load from whatever connects to it, and all `T` ports on that node
  instance must resolve to the same type. `T` ranges over `scalar`/`vec2`/
  `vec3`/`vec4` only.

## 5. Ports vs. Properties

Every node has:

- **Properties** — load-time configuration (file paths, flags, mode strings).
  Always JSON literals; never wireable; changing one means reloading the node.
- **Input ports** — typed values that may vary per frame. Each accepts a
  literal constant, a parameter reference, or a connection (§7). Ports with a
  default may be omitted.
- **Output ports** — typed values other nodes may reference. Every node type
  designates one output as its *default output*.

## 6. Parameters

The `parameters` block declares the scene's user-tweakable values. It doubles
as the settings-UI manifest and, later, as the surface exposed to WASM logic
modules.

```json
"parameters": {
  "intensity": {
    "type": "scalar",
    "default": 0.6,
    "min": 0.0,
    "max": 1.0,
    "step": 0.05,
    "label": "Glow intensity"
  },
  "tint": {
    "type": "vec4",
    "default": [1.0, 0.9, 0.8, 1.0],
    "hint": "color",
    "label": "Tint"
  }
}
```

- `type` (required): `scalar`, `vec2`, `vec3`, or `vec4`.
- `default` (required): literal of that type.
- `min`, `max`, `step` (optional): UI + clamping metadata. Applied
  per-component for vector types.
- `label` (optional): display name; the key is used if absent.
- `hint` (optional): UI presentation hint. v1 defines `"color"` (valid on
  `vec3`/`vec4`). Unknown hints are ignored.

Parameter values are inputs to the graph: when the user changes one, ports
bound to it become dirty (§11).

## 7. Reference Grammar

An input port's JSON value is one of:

| Form                  | Meaning                                            |
|-----------------------|----------------------------------------------------|
| literal (`0.5`, `[1,0]`) | constant                                        |
| `"$name"`             | scene parameter reference                          |
| `"@node"`             | connection to `node`'s default output              |
| `"@node.port"`        | connection to a named output port                  |
| object form (below)   | expanded connection                                |

Expanded object form, required for previous-frame reads:

```json
{ "node": "trail", "port": "result", "previous": true }
```

- `node` (required), `port` (optional, defaults to the default output),
  `previous` (optional bool, default false): read the port's value from the
  previous frame (§10).

A string starting with a literal `@` or `$` cannot occur as a plain string
constant; v1 has no wireable string ports, so no escaping is needed.

## 8. Nodes

Each entry in `nodes`:

```json
{
  "id": "glow",
  "type": "shader",
  "shader": "shaders/glow.wgsl",
  "inputs": { "source": "@bg", "strength": "$intensity" }
}
```

- `id` (required): unique identifier (§3).
- `type` (required): a built-in node type (§9).
- `inputs` (optional, object): port name → reference-grammar value.
- All other fields are the node type's properties.

Ignoring `previous: true` edges, the graph must be a DAG. Nodes not reachable
from the `output` node (directly or via feedback edges) are a load warning and
are never executed.

## 9. Built-in Node Types (v1)

Conventions used below: coordinates are normalized to the scene output —
origin top-left, `[0,0]`–`[1,1]`, y-down ("uv space"). Rotations are degrees,
clockwise. All texture ports carry premultiplied alpha in linear color (§12).

### 9.1 `image`

Static image source.

| Kind     | Name     | Type      | Default | Notes                          |
|----------|----------|-----------|---------|--------------------------------|
| property | `src`    | `string`  | —       | path; PNG/JPEG/WebP/KTX2       |
| output   | `result`†| `texture` |         | dirty on load only             |

### 9.2 `video`

Video source. Plays muted; decoding is suspended while no output is presenting.

| Kind     | Name     | Type      | Default | Notes                          |
|----------|----------|-----------|---------|--------------------------------|
| property | `src`    | `string`  | —       | path                           |
| property | `loop`   | `bool`    | `true`  |                                |
| output   | `result`†| `texture` |         | dirty on each new decoded frame|

### 9.3 `shader`

Fullscreen fragment-shader pass. Input ports are defined by the WGSL itself
(§9.10); `source`/`strength` below are examples, not fixed.

| Kind     | Name     | Type      | Default  | Notes                              |
|----------|----------|-----------|----------|------------------------------------|
| property | `shader` | `string`  | —        | path to WGSL                       |
| property | `size`   | `string`/`vec2` | `"auto"` | `"auto"`: size of first texture input, else output size; or explicit `[w, h]` in pixels |
| input    | *(from WGSL)* | *(reflected)* |     | see §9.10                          |
| output   | `result`†| `texture` |          |                                    |

### 9.4 `transform`

2D layer transform. Renders `source` into a scene-output-sized canvas
(transparent background) with the transform applied — the workhorse for
animated foreground layers.

| Kind   | Name       | Type      | Default      | Notes                             |
|--------|------------|-----------|--------------|-----------------------------------|
| input  | `source`   | `texture` | —            | required                          |
| input  | `position` | `vec2`    | `[0.5, 0.5]` | placement of anchor, output space |
| input  | `rotation` | `scalar`  | `0`          | degrees, clockwise, about anchor  |
| input  | `scale`    | `vec2`    | `[1, 1]`     | scalar splat allowed              |
| input  | `anchor`   | `vec2`    | `[0.5, 0.5]` | normalized in source texture      |
| input  | `opacity`  | `scalar`  | `1`          | multiplies alpha                  |
| output | `result`†  | `texture` |              |                                   |

Sizing is resolution-independent, defined relative to the output: at
`scale = [1,1]` the layer's displayed height equals the output height, and its
width follows the source's aspect ratio (displayed width = `scale.x` ×
srcAspect × output height). `scale: 0.2` therefore covers 20% of the screen
height on every output, regardless of resolution or DPI. A pixel-exact `fit`
mode is reserved for a future version.

### 9.5 `compositor`

Stacks layers bottom-to-top with premultiplied source-over blending.

| Kind   | Name     | Type        | Default | Notes                          |
|--------|----------|-------------|---------|--------------------------------|
| input  | `layers` | `texture[]` | —       | array of texture references, bottom first |
| output | `result`†| `texture`   |         |                                |

`texture[]` is an array-valued port: its JSON value is an array whose elements
each follow the reference grammar. v1 supports only source-over; per-layer
blend modes are reserved.

### 9.6 `output`

Scene sink. Exactly one per scene (zero or multiple is a load error).

| Kind   | Name    | Type      | Default | Notes |
|--------|---------|-----------|---------|-------|
| input  | `color` | `texture` | —       | required |

### 9.7 `time` (implicit)

`time` and `mouse` are **implicit singletons**: they are never declared in
`nodes`; reference them directly (`@time.seconds`, `@mouse.position`). Their
ids are reserved (§3). Future ambient inputs (e.g. `audio`) will follow the
same pattern.

| Kind   | Name      | Type     | Notes                                   |
|--------|-----------|----------|-----------------------------------------|
| output | `seconds`†| `scalar` | seconds since scene start; dirty every rendered frame |
| output | `delta`   | `scalar` | seconds since previous rendered frame   |

`seconds` does not advance while the scene is paused (occluded/locked); scenes
therefore resume where they left off rather than jumping.

### 9.8 `mouse` (implicit)

| Kind   | Name       | Type     | Notes                                       |
|--------|------------|----------|---------------------------------------------|
| output | `position`†| `vec2`   | output space; holds last value when inactive |
| output | `active`   | `scalar` | 1 while the pointer is over the scene surface, else 0 |

Pointer input is inherently intermittent on Wayland (only delivered while the
cursor is over the bare desktop); scenes should treat `position` as
last-known-value and use `active` to fade effects out rather than snap.

### 9.9 Value nodes: `remap`, `wave`, `combine`, `split`

CPU-evaluated per-frame value plumbing. `T` is polymorphic (§4).

`remap` — linear range mapping, `out = outMin + (value − inMin) / (inMax − inMin) × (outMax − outMin)`:

| Kind   | Name      | Type | Default  | Notes                        |
|--------|-----------|------|----------|------------------------------|
| input  | `value`   | `T`  | —        | required                     |
| input  | `inMin`   | `T`  | `0`      |                              |
| input  | `inMax`   | `T`  | `1`      |                              |
| input  | `outMin`  | `T`  | `0`      |                              |
| input  | `outMax`  | `T`  | `1`      |                              |
| property | `clamp` | `bool` | `true` | clamp result to out range    |
| output | `result`† | `T`  |          |                              |

`wave` — oscillator, `result = sin/tri/saw/square(phase + input × frequency)`
mapped to `[-1, 1]`:

| Kind     | Name        | Type     | Default  | Notes                     |
|----------|-------------|----------|----------|---------------------------|
| input    | `input`     | `scalar` | —        | typically `@time.seconds` |
| input    | `frequency` | `scalar` | `1`      | cycles per unit of input  |
| input    | `phase`     | `scalar` | `0`      | in cycles (0–1 = one period) |
| property | `shape`     | `string` | `"sine"` | `sine`, `triangle`, `saw`, `square` |
| output   | `result`†   | `scalar` |          |                           |

`combine` — builds a vector from scalar components. The output type is `vecN`
where N is the highest component bound (`y` → `vec2`, `z` → `vec3`, `w` →
`vec4`; binding only `x` is a load error — use the scalar directly). Unbound
lower components default to `0`:

| Kind   | Name      | Type     | Default | Notes |
|--------|-----------|----------|---------|-------|
| input  | `x`       | `scalar` | `0`     |       |
| input  | `y`       | `scalar` | `0`     |       |
| input  | `z`       | `scalar` | `0`     |       |
| input  | `w`       | `scalar` | `0`     |       |
| output | `result`† | `vecN`   |         | N from highest bound component |

`split` — extracts scalar components from a vector. Referencing a component
beyond the input's arity (e.g. `.z` of a `vec2`) is a load error:

| Kind   | Name    | Type                 | Notes    |
|--------|---------|----------------------|----------|
| input  | `value` | `vec2`/`vec3`/`vec4` | required |
| output | `x`†    | `scalar`             |          |
| output | `y`     | `scalar`             |          |
| output | `z`     | `scalar`             |          |
| output | `w`     | `scalar`             |          |

This set is expected to grow (add/multiply/mix/clamp etc.) as real scenes
demand; additions are backward compatible.

† = default output.

### 9.10 Shader Node WGSL Contract

The WGSL file *is* the node's port declaration; the runtime reflects it (via
Tint) at load and binds by **name**. Group/binding numbers are the author's
choice; names are what matter.

- The author writes a fragment entry point `@fragment fn main(@location(0) uv:
  vec2<f32>) -> @location(0) vec4<f32>`. The runtime supplies the fullscreen
  vertex stage and the `uv` varying (`[0,0]` top-left).
- Each `texture_2d<f32>` binding becomes a `texture` input port with the
  variable's name.
- For a texture port `foo`, a sampler binding named `foo_sampler` is provided
  by the runtime (linear filtering, clamp-to-edge). Custom sampler
  configuration is reserved.
- A single uniform struct binding named `params` may be declared; each field
  becomes an input port of the corresponding type (`f32` → `scalar`,
  `vec2<f32>` → `vec2`, etc.). Other field types are a load error.
- The fragment output is the node's `result`, premultiplied alpha, linear.

Load-time validation: every port referenced in `scene.json` `inputs` must
exist in the reflected interface (unknown name = error), and unbound ports
must have… no defaults exist for shader ports, so every reflected port must
either be bound in `inputs` or be a texture the author is fine sampling as
undefined — therefore v1 requires **all** reflected ports to be bound.

## 10. Feedback (Previous-Frame Reads)

A connection with `previous: true` reads the producer port's value from the
previous frame. Rules:

- The graph must be acyclic when `previous: true` edges are ignored; any cycle
  must pass through at least one `previous` edge.
- `previous: true` on a `texture` edge is realized as ping-pong buffers: the
  consumer samples the producer's frame N−1 output while frame N is written.
- On the first frame (and after a node reload), a previous-frame texture reads
  as fully transparent; a previous-frame value reads as the port's default or
  initial value.
- Dirty propagation: a `previous` edge is dirty on frame N iff the producer
  port was written on frame N−1. A self-sustaining loop (e.g. a trail that
  feeds itself) therefore keeps running once kicked — by design; it quiesces
  only when the whole loop produces no changes.

## 11. Execution & Dirty Semantics

The scene-graph efficiency contract (DESIGN.md §3.2, §10) in precise terms:

- Every output port carries a per-frame dirty flag.
- A node executes on a frame iff at least one of its input ports is dirty
  (connected to a dirty output, or bound to a parameter that changed).
  Otherwise its outputs retain their previous value/contents, non-dirty.
- Value outputs are dirty only if the produced value actually differs from the
  previous one (change detection, not mere re-execution).
- Texture outputs are dirty at whole-texture granularity in v1. Damage
  rectangles are reserved as a future optimization; the format needs no change
  for them.
- `time.seconds` is dirty every *rendered* frame; frames are only rendered in
  response to compositor frame callbacks, so occlusion/lock/off pauses the
  whole graph, and a scene with nothing dirty schedules no GPU work at all.
- The `output` node presents only when its `color` input is dirty.

## 12. Color & Alpha

- All `texture` edges are **premultiplied alpha, linear color**.
- sRGB-encoded sources (PNG/JPEG/WebP, video) are decoded to linear at the
  source node; non-premultiplied sources are premultiplied at the source node.
- Intermediate render targets default to `rgba16float`.
- The `output` node encodes back to the surface's format (sRGB) for
  presentation.

## 13. Validation Summary

Load errors: unsupported `version`; duplicate/invalid/reserved node id; unknown node
type; unknown port name; type mismatch on a connection or literal; missing
required input; unknown `$parameter`; reference to unknown node; cycle without
a `previous` edge; zero or multiple `output` nodes; missing asset or shader
file; path escaping the project root; WGSL that fails to compile or reflect;
unbound shader port.

Load warnings: unknown top-level field; unknown node property; node
unreachable from `output`; unknown parameter `hint`.

## 14. Example Scenes

### 14.1 Static background, bobbing foreground layer

```json
{
  "version": 1,
  "name": "Bobbing Buoy",
  "parameters": {
    "bobSpeed": { "type": "scalar", "default": 0.25, "min": 0, "max": 2, "label": "Bob speed" }
  },
  "nodes": [
    { "id": "bg",   "type": "image", "src": "assets/sea.ktx2" },
    { "id": "buoy", "type": "image", "src": "assets/buoy.png" },

    { "id": "bob",  "type": "wave",  "inputs": { "input": "@time.seconds", "frequency": "$bobSpeed" } },
    { "id": "bobY", "type": "remap",
      "inputs": { "value": "@bob", "inMin": -1, "inMax": 1, "outMin": 0.69, "outMax": 0.71 } },
    { "id": "pos",  "type": "combine", "inputs": { "x": 0.3, "y": "@bobY" } },

    { "id": "place", "type": "transform",
      "inputs": { "source": "@buoy", "position": "@pos", "scale": 0.2 } },

    { "id": "comp", "type": "compositor", "inputs": { "layers": ["@bg", "@place"] } },
    { "id": "out",  "type": "output",     "inputs": { "color": "@comp" } }
  ]
}
```

### 14.2 Video background, glow shader, mouse parallax

```json
{
  "version": 1,
  "name": "Nebula",
  "parameters": {
    "intensity": { "type": "scalar", "default": 0.6, "min": 0, "max": 1, "label": "Glow intensity" }
  },
  "nodes": [
    { "id": "bg",   "type": "video", "src": "assets/nebula.mp4", "loop": true },

    { "id": "glow", "type": "shader", "shader": "shaders/glow.wgsl",
      "inputs": { "source": "@bg", "strength": "$intensity" } },

    { "id": "stars", "type": "image", "src": "assets/stars.png" },
    { "id": "parallax", "type": "remap",
      "inputs": { "value": "@mouse.position",
                  "inMin": 0, "inMax": 1,
                  "outMin": [0.49, 0.49], "outMax": [0.51, 0.51] } },
    { "id": "starsLayer", "type": "transform",
      "inputs": { "source": "@stars", "position": "@parallax", "scale": 1.05 } },

    { "id": "comp", "type": "compositor", "inputs": { "layers": ["@glow", "@starsLayer"] } },
    { "id": "out",  "type": "output",     "inputs": { "color": "@comp" } }
  ]
}
```

with `shaders/glow.wgsl` sketching the contract:

```wgsl
struct Params { strength: f32 }
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var source: texture_2d<f32>;
@group(0) @binding(2) var source_sampler: sampler;

@fragment
fn main(@location(0) uv: vec2<f32>) -> @location(0) vec4<f32> {
  let base = textureSample(source, source_sampler, uv);
  // ... blur/bloom elided ...
  return base * (1.0 + params.strength);
}
```

## 15. Reserved for Future Versions

Collected from the sections above — features the format's structure already
accounts for but v1 does not implement:

- `event` and `buffer` edge types (§4)
- Inline references inside vector literals, as sugar desugaring to `combine` (§7, §9.9)
- Per-layer blend modes on `compositor` (§9.5)
- Pixel-exact `fit` mode on `transform` (§9.4)
- Custom sampler configuration for shader texture ports (§9.10)
- Damage-rectangle dirty granularity for textures (§11)
- Additional value nodes (add/multiply/mix/clamp, …) (§9.9)
- Additional implicit input nodes (e.g. `audio`) (§9.7)
- `modules/` (WASM logic) and `graphs/` project directories (§1)
