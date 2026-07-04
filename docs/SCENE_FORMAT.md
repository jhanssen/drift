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

| Kind     | Name      | Type      | Default | Notes                          |
|----------|-----------|-----------|---------|--------------------------------|
| property | `src`     | `string`  | —       | path                           |
| property | `loop`    | `bool`    | `true`  |                                |
| input    | `playing` | `scalar`  | `1`     | > 0.5 plays; otherwise pauses  |
| output   | `result`† | `texture` |         | dirty on each new decoded frame|

- **Pause, not stop**: while `playing` ≤ 0.5, decoding is suspended (the same
  machinery as presentation-suspend), the playback position freezes, and
  `result` holds the last decoded frame, non-dirty. Resuming continues from
  the paused position.
- Change detection (§11) means a `playing` signal wired from a step-like
  source (e.g. a `sequence` `hold` track) is dirty only on the frames it
  actually flips — the node sees clean start/stop transitions for free.
- A `restart: event` port is reserved (§16).

*(`playing` adopted 2026-07-04; runtime support pending.)*

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

Scene time is **non-decreasing**: it advances or holds, never moves backward.
Nodes may rely on this (e.g. `sequence` cue-crossing, §16, is defined only for
forward motion). Editors that scrub backward do so by re-evaluating the scene
from an earlier point with a fresh instance — time within any one scene
instance still only moves forward.

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

`sequence` — a timeline: maps one scalar input (typically `@time.seconds`)
through named keyframe **tracks**, each of which becomes an output port.
(Event tracks — cues that fire at instants — are the reserved half of this
node; see §16.)

| Kind     | Name       | Type      | Default | Notes                              |
|----------|------------|-----------|---------|------------------------------------|
| property | `duration` | `scalar`  | —       | required, > 0, seconds             |
| property | `loop`     | `bool`    | `true`  |                                    |
| property | `tracks`   | —         | —       | required, array (see below)        |
| input    | `time`     | `scalar`  | —       | required; typically `@time.seconds`|
| output   | *(per track)* | *(per track)* |  | first track is the default output† |

Local time is `t = loop ? mod(time, duration) : clamp(time, 0, duration)`,
computed in double precision before any f32 conversion (§17.2).

**Track objects** (`tracks` is an array; order fixes the default output and an
editor's lane order):

- `name` (required): identifier (§3), unique within the node; becomes the
  output port name.
- `kind` (required): `"value"`. (`"event"` is reserved; §16.)
- `type` (required): `scalar`, `vec2`, `vec3`, or `vec4`.
- `interpolate` (optional, default `"linear"`): `"hold"` (step), `"linear"`,
  or `"smooth"` (smoothstep between keys). Per-key easing is reserved.
- `keys` (required, non-empty): array of `{ "t": seconds, "value": literal }`,
  strictly ascending `t`, each `t` in `[0, duration]`.
- Before the first key the output is the first key's value; after the last,
  the last key's value. There is no wrap-around interpolation: when looping,
  the value holds until local time wraps back past the first key — for a
  seamless loop, end with a key at `duration` matching the first key.

```json
{
  "id": "seq", "type": "sequence",
  "duration": 40.0,
  "loop": true,
  "tracks": [
    { "name": "videoOn", "kind": "value", "type": "scalar", "interpolate": "hold",
      "keys": [ { "t": 0, "value": 0 }, { "t": 10, "value": 1 }, { "t": 25, "value": 0 } ] },
    { "name": "drift", "kind": "value", "type": "vec2", "interpolate": "smooth",
      "keys": [ { "t": 0, "value": [0.5, 0.5] }, { "t": 20, "value": [0.52, 0.48] } ] }
  ],
  "inputs": { "time": "@time.seconds" }
}
```

Dirty semantics: the node executes whenever `time` is dirty — every rendered
frame, like `wave` — and outputs follow §11 change detection, so a `hold`
track is dirty only on key-transition frames and downstream nodes (e.g. a
paused video) stay fully quiescent between steps. Multiple `sequence` nodes
per scene are allowed; each is just a value node. Because `time` is an
ordinary input, `sequence` also serves as a general keyframe-curve lookup for
non-time signals (e.g. an author-drawn pointer-response curve).

Efficiency note (DESIGN.md §10): a visually static scene that is merely
*waiting* for a key transition still ticks the CPU value graph each frame
callback; the GPU schedules nothing. Runtimes may additionally compute the
next transition time and skip even CPU evaluation until then; this is an
implementation optimization with no format impact.

*(Value tracks adopted 2026-07-04; runtime support pending.)*

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
unbound shader port. For `sequence`: `duration` missing or ≤ 0; missing or
empty `tracks`; duplicate or invalid track `name`; unknown or reserved track
`kind`; value track with empty `keys`, non-ascending `t`, `t` outside
`[0, duration]`, or `value` not matching `type`.

Load warnings: unknown top-level field; unknown node property; node
unreachable from `output`; unknown parameter `hint`; a `sequence` output port
that nothing references.

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
- `sequence` event tracks, per-key easing, and wake-until-next-key scheduling
  hints (§9.9, §16)
- Inline references inside vector literals, as sugar desugaring to `combine` (§7, §9.9)
- Per-layer blend modes on `compositor` (§9.5)
- Pixel-exact `fit` mode on `transform` (§9.4)
- Custom sampler configuration for shader texture ports (§9.10)
- Damage-rectangle dirty granularity for textures (§11)
- Additional value nodes (add/multiply/mix/clamp, …) (§9.9)
- Additional implicit input nodes (e.g. `audio`) (§9.7)
- `modules/` (WASM logic) and `graphs/` project directories (§1)

## 16. Proposed Extension: Events & Sequencing (draft, partially adopted)

Status: the **level-triggered half was adopted 2026-07-04** — `video.playing`
(§9.2) and `sequence` value tracks (§9.9) are part of the main format. This
section retains the **edge-triggered half**: the `event` edge type reserved in
§4 and its consumers (video `restart`, `sequence` event tracks), which stay a
draft until a one-shot consumer ships (video restart alone is thin; particles
are the canonical customer).

### 16.1 Motivation

The format currently expresses only *continuous* animation (values as functions
of time). Two further kinds of control are needed:

- **Level-triggered** — a condition that holds over a span ("video plays from
  10s to 25s"). Representable as an ordinary `scalar` that steps between 0 and
  1; needs no new edge type, only a source for such signals and ports that
  consume them.
- **Edge-triggered** — an impulse at an instant ("restart the video", "burst
  100 particles at t=12"). Not representable as a level; this is what `event`
  edges are for.

A future particle node is the canonical consumer of both: a continuous
`rate: scalar` input (particles/sec) and a discrete `burst: event` input.
The level-triggered half needed no new edge type and is now adopted; what
follows is the event half.

### 16.2 The `event` Edge Type

Amends §4. `event` becomes a wireable edge type:

- An event edge carries no value; on any given frame it either **fires** or
  does not. (Payloads are reserved for a future version.)
- Connections only — there is no literal form, and parameters cannot be of
  type `event` (unchanged: parameters are `scalar`/`vecN`).
- No implicit conversions to or from `event`. (An event→level `latch`/`toggle`
  value node is future §9.9 growth.)
- Dirty semantics (§11): an event output is dirty exactly on frames it fires.
  A fire is a one-frame phenomenon; there is no "current value" to retain.
- `previous: true` (§7) is valid on an event edge and reads "fired on frame
  N−1". The §10 acyclicity rule applies unchanged.

### 16.3 `video` Playback Control

`playing` is adopted (§9.2). Remaining: one event-consuming input port
(optional; a bare `video` node behaves exactly as today):

| Kind  | Name      | Type     | Default | Notes                                    |
|-------|-----------|----------|---------|------------------------------------------|
| input | `restart` | `event`  | —       | on fire: seek to 0, decode first frame   |

- `restart` fires while paused: the first frame is decoded and `result` goes
  dirty once (a poster frame); playback then waits on `playing`.

### 16.4 `sequence` Event Tracks

The `sequence` node and its value tracks are adopted (§9.9). Remaining: the
`"event"` track `kind`. An event track becomes an `event`-typed output port
and fires at cue times:

```json
{ "name": "burst", "kind": "event", "fires": [ 12.0, 30.0 ] }
```

- `fires` (required, non-empty): ascending array of times in `[0, duration)`.
- Crossing semantics: with previous local time `t0` and current `t1`, cues in
  `(t0, t1]` fire; if the loop wrapped, cues in `(t0, duration)` and `[0, t1]`
  fire. On the node's first evaluation, cues in `[0, t1]` fire. A cue therefore
  fires exactly once per pass of the playhead, including once per loop
  iteration.
- Crossing semantics assume a monotonic `time` input. Value tracks are pure
  functions of `time` and tolerate any signal (the curve-lookup use, §9.9),
  but an event track on a `sequence` whose `time` input is not derived from
  `@time` should be a load warning.

### 16.5 Editor Mapping

The node is deliberately shaped so a conventional timeline panel edits it
directly — the JSON *is* the UI model:

- One panel per `sequence` node: a time ruler from `0` to `duration`, a loop
  toggle, one lane per track in array order. Value tracks draw as
  step/line/curve lanes with draggable keys; event tracks draw as diamond cue
  markers. Renaming a lane renames the output port.
- **What vs. when**: the graph view owns *what* is affected (wiring
  `@seq.videoOn` → `video.playing`); the timeline panel owns *when*. Neither
  edits the other's domain, so the two views cannot conflict.
- **Scrubbing**: scene time is non-decreasing (§9.7), so an editor never
  rewinds a live instance. Scrub preview plays back a *recorded pass*: the
  editor simulates the scene once at fixed step (determinism makes this
  exact, feedback scenes included) and caches output frames; resuming
  playback from a scrubbed position re-simulates `0 → t` on a fresh
  instance. Timeline lanes themselves need no simulation — value tracks are
  pure functions of `time`. Event cues are edge-triggered and therefore a
  transport-policy question, not a format one — suggested policy: fire cues
  when crossed during editor *playback*, suppress them during the recorded
  pass and drag-scrub, and offer a per-cue "fire now" button (a live-sync
  verb) for testing one-shot effects in isolation.
- **Live edit cost**: `tracks` is a property, so edits reload the node (§5) —
  a CPU-only rebuild of key arrays, well within the <100ms hot-reload target;
  output ports (and thus wiring) survive unless a track is renamed or removed.
- Convenience gestures (non-normative): "add to timeline" on a wireable port
  creates a matching value track and wires it; dragging a lane's header onto a
  port wires that track.

### 16.6 Validation Additions

(Value-track validation is adopted; see §13.) Load errors: event track with
empty or non-ascending `fires` or a time outside `[0, duration)`; `event`
connection to a non-`event` port or vice versa.

Load warnings: an event track on a `sequence` whose `time` input is not
derived from `@time` (§16.4).

### 16.7 Effect on §15

If adopted: `event` moves from "reserved" to specified (§16.2); event
payloads, per-cue payload/latch nodes, and wake-until-next-cue scheduling
hints join the reserved list. (Per-key easing is already listed there via the
adopted value tracks.)

## 17. Proposed Amendments: Spec Gaps (draft, not yet adopted)

Status: proposals. Each subsection is independently adoptable and states which
existing section it amends. §17.4 additionally depends on §16 (`event`).
Where noted, a proposal *codifies* what the runtime already does (verified
against `src/core` as of 2026-07-04) rather than prescribing new behavior.

### 17.1 Layer Sizing and the `fit` Node

Gap: nothing defines how a texture whose size differs from the output maps
onto the output-sized canvas — yet wiring a native-resolution image straight
into `compositor` is the first thing every scene does (§14.1), and the same
scene must hold up on 16:9, 21:9, and portrait outputs.

Amendment to §9.5: a `compositor` layer whose texture size differs from the
output is scaled to the output size (non-uniformly if aspect differs), linear
filtering. This codifies current runtime behavior (each layer draws as a
fullscreen pass). It is rarely what an author wants; aspect-correct placement
goes through `fit` (not yet implemented):

New node type `fit` — maps a source texture onto an output-sized canvas:

| Kind     | Name     | Type      | Default   | Notes                             |
|----------|----------|-----------|-----------|-----------------------------------|
| property | `mode`   | `string`  | `"cover"` | `cover`, `contain`, `stretch`     |
| input    | `source` | `texture` | —         | required                          |
| output   | `result`†| `texture` |           | always output-sized               |

- `cover`: uniform scale to fill the output, centered, overflow cropped — the
  wallpaper default.
- `contain`: uniform scale to fit entirely, centered, uncovered area
  transparent.
- `stretch`: non-uniform scale to fill exactly.

Alignment other than centered is reserved. Under this amendment the §14
examples would route backgrounds through a `fit` node
(`{ "id": "bgFit", "type": "fit", "inputs": { "source": "@bg" } }`).

### 17.2 Time Precision Over Long Uptimes

Gap: `time.seconds` is a `scalar` (f32, §4), and wallpapers run for weeks. At
one day of uptime f32 resolution is ~8 ms; at a month, ~0.25 s — `wave`-driven
motion turns visibly steppy. Scene authors will not anticipate this.

Implementation status: implemented as specified — `Value` carries f64
components end to end, narrowing to f32 only where uniforms are packed for
the GPU (transform, shader); `wave` reduces its phase to one cycle in double
before trig; JSON literals parse without a float round-trip. Unit-tested at
a month and a year of scene time (`value_precision_test.cpp`), both of which
fail with an f32 value path. `sequence` (§9.9) is specified to do the same
reduction (runtime support pending).

Amendments:

- CPU-evaluated value plumbing (§9.9 nodes, parameter application, port
  literals) is computed in **double precision**; values convert to f32 only at
  GPU boundaries (shader uniforms). The `scalar` edge type is unchanged as far
  as authors are concerned.
- Nodes that reduce an unbounded time-like input to a bounded range perform
  the reduction in double precision *before* any f32 conversion: `wave`
  reduces `input × frequency + phase` modulo one cycle; `sequence` (§9.9)
  reduces to local time modulo `duration`.
- Authoring guidance (non-normative): wire bounded signals (a `wave` output, a
  `sequence` track) into shader params rather than raw `@time.seconds`; a raw
  f32 seconds uniform degrades no matter what the CPU side does. A wrapped
  time output (e.g. `@time.wrapped`, period-configurable) is reserved for
  shaders that genuinely need continuous time.

### 17.3 Scene Frame-Rate Cap

Gap: anything wired to `@time.seconds` is dirty on every compositor frame
callback, so a subtle ambient animation renders at full monitor refresh
(144 Hz on a 144 Hz panel) with no way for the scene to declare that less is
plenty.

Amendment to §2 — optional top-level field:

```json
"maxFps": 30
```

- Number, > 0. The runtime skips frame callbacks as needed to approximate the
  cap; `time.delta` reports real elapsed time, so animation speed is
  unaffected.
- This is a *ceiling*, composable with external policy: user settings or power
  management may lower the effective rate further, never raise it above the
  cap.
- On runtimes predating this field it degrades safely: an unknown top-level
  field is a load warning (§2), and the scene simply runs uncapped.
- Per-node/per-subtree update rates are reserved.

### 17.4 `video` End of Stream

Depends on §16. Gap: behavior at end of a `loop: false` video is unspecified,
and nothing can react to playback finishing.

Amendment to §9.2 (with `playing` from §9.2 and `restart` from §16.3):

- With `loop: false`, after the final frame is produced the node holds that
  frame (`result` non-dirty) and enters the finished state; `playing` has no
  further effect until `restart` fires, which rearms playback from the start.
  (Hold-last-frame is already the decoder's behavior — `VideoDecoder::frameAt`
  returns the final frame forever after end of stream; this codifies it.)
- New output port `finished: event` — fires on the frame the final decoded
  frame is produced. Never fires while `loop: true`.
- Chaining is the point: `finished` wired into a `sequence`-adjacent graph (or
  another video's `restart`) enables playlist- and intro/loop-style scenes.

### 17.5 Shader `size: "auto"` Resolution Rules

Gap: §9.3 defines `"auto"` as "size of first texture input", but (a) "first"
is undefined, and (b) the canonical feedback shader's first texture input is
its own previous-frame output — a chicken-and-egg for size inference.

Amendments to §9.3:

- "First texture input" means the reflected `texture_2d<f32>` binding with the
  lowest `@group`, then lowest `@binding`.
- Size inference ignores connections with `previous: true` (§10). If every
  texture input is a `previous` edge, or there are none, `"auto"` resolves to
  the output size.
- Authoring guidance: feedback shaders should declare an explicit
  `size: [w, h]` when the ping-pong buffers must not track output size.

Implementation status: implemented as specified — texture ports are ordered
by (group, binding) at reflection, and size inference skips `previous` edges
(without the exclusion, an auto-sized feedback shader locked onto its own
history size and stopped tracking output resizes).

### 17.6 Multi-Output Semantics

Gap: multi-monitor is a v1 fast-follow (DESIGN.md §13.2) but the format is
silent on what is shared versus per-output when one scene drives several
monitors.

Amendment — a scene on N outputs behaves as N logical instances with the
following sharing rules:

- **Per output**: all node outputs and dirty state, feedback ping-pong buffers
  (§10), `mouse` (§9.8) — the pointer is only ever over one output.
- **Shared: the scene clock.** `@time.seconds` is a single clock sampled by
  each instance at its own frame callbacks. It pauses only while *all*
  instances are paused; an instance resuming from occlusion therefore snaps
  forward to the shared clock, keeping adjacent monitors in sync (rather than
  each output drifting on its own resumed-where-it-left-off clock, §9.7).
- **Shared: parameter values** (§6) — one user-facing setting, all outputs.
- **Shared: video decode.** One decoder per `video` node feeds all instances;
  each instance's `result` goes dirty as it observes new frames at its own
  frame pacing. Decode suspends only when no instance is presenting.
- Instances may share the computed results of any subgraph not downstream of
  a per-output source (in v1: `mouse`) as a pure optimization; this is
  observable-behavior-neutral and needs no format support.

### 17.7 Small Clarifications

Independently adoptable one-liners. The first three codify what the runtime
already does; adopting them is purely a documentation change:

- **`mouse` initial value** (§9.8): before the pointer first enters the
  surface, `position` reads `[0.5, 0.5]` and `active` reads `0`. (Already
  implemented.)
- **`remap` degenerate range** (§9.9): when `inMax == inMin` (per component
  for vectors), the result is `outMin` for that component — no division is
  performed. (Already implemented.)
- **`compositor` empty `layers`** (§9.5): a present-but-empty array is a load
  error (the port is required and an empty stack renders nothing). (Already
  implemented.)
- **Parameter persistence** (§6): `scene.json` holds only *defaults*. User
  overrides live in runtime-managed per-scene settings storage outside the
  project directory; at load the runtime clamps stored values to the declared
  `min`/`max` and drops overrides whose parameter or type no longer exists.
- **Arithmetic value nodes** (§9.9 growth, all `T`-polymorphic per §4):
  `add` (`a + b`), `multiply` (`a × b`), `mix` (`a + (b − a) × t`, `t` is
  `scalar`), `clamp` (`min(max(value, lo), hi)`). Motivating footgun: today
  the only way to scale a value is `remap`, whose `clamp` property defaults to
  `true` — scaling an unbounded input like `@time.seconds` silently saturates
  at `outMax`. `multiply` is the correct tool; docs should point at it.

### 17.8 Effect on §15

If adopted: `fit` alignment modes, `@time.wrapped`, per-node update rates, and
per-layer `fit` on `compositor` join the reserved list; the "additional value
nodes" bullet is partially discharged by §17.7's arithmetic nodes.
