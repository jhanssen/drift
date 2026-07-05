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

`graphs/` holds subgraph definitions (§19). `packages/` holds project-local
copies of installed packages (§20.2 — normally packages resolve from the
shared store instead). `modules/` (see DESIGN.md §6.1) is reserved for
deferred features and ignored by the v1 runtime if present.

All paths inside `scene.json` are relative to the `.sceneproject` root. Paths
must not escape the project root (`..` is rejected at load). Paths beginning
`packages/` additionally resolve through the package store (§20.2).

## 2. `scene.json` Top Level

```json
{
  "version": 1,
  "name": "Nebula",
  "author": "optional",
  "description": "optional",
  "parameters": { },
  "nodes": [ ],
  "editor": { }
}
```

- `version` (required, integer): format version. The runtime rejects scenes with
  a version it does not support.
- `name` (required, string): display name.
- `author`, `description` (optional, string): metadata.
- `parameters` (optional, object): user-tweakable scene parameters (§6).
- `nodes` (required, array): the scene graph (§8).
- `editor` (optional, object): tool-owned metadata (§2.1).

Unknown top-level fields are a load warning, not an error (forward
compatibility within a version).

### 2.1 The `editor` Block

Editing tools need scene-scoped state that has no runtime meaning — node
positions in a graph view, collapsed panels. The `editor` top-level object is
that home. The runtime validates only that it is an object (anything else is
a load warning and the block is ignored) and never reads inside; pushed and
hot-reloaded documents carry it along unchanged. Runtimes predating this
section degrade safely: an unknown top-level field is a warning, not an error.

Keys defined so far (tools must preserve keys they do not understand):

- `positions` (object): graph-view placement — node id → `[x, y]`, the node's
  top-left corner in abstract canvas units (pixels at zoom 1, +y down). The
  reserved implicit ids `time` and `mouse` (§3) may appear as keys; they can
  never clash with authored nodes. A `$`-prefixed key holds the placement of
  a *parameter* for graph tools that draw parameters as source nodes — `$`
  cannot begin a node id (§3), so the namespaces cannot collide. Entries
  whose id (or parameter) no longer exists are ignored and may be dropped on
  save. Nodes without an entry are auto-laid-out by the tool.

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
| `event`  | — (connections only)         | fires or not, per frame (see below)|
| `buffer` | — (connections only)         | GPU array of structs (§18.1)       |
| `bool`   | `true` / `false`             | properties only, not wireable      |
| `string` | `"..."`                      | properties only, not wireable      |

Rules:

- Typing is static; all connections are checked at load time.
- Exactly one implicit conversion exists: a `scalar` may connect to (or be the
  literal for) any `vecN` input — it splats to all components. No other
  implicit conversions; `event` takes part in none.
- Some value-node ports are **polymorphic** (written `T`): the port's type is
  resolved at load from whatever connects to it, and all `T` ports on that node
  instance must resolve to the same type. `T` ranges over `scalar`/`vec2`/
  `vec3`/`vec4` only.

Event edges: an event edge carries no value — on any given frame it either
**fires** or does not (payloads are reserved). Connections only; parameters
cannot be of type `event`. Dirty semantics (§11): an event output is dirty
exactly on frames it fires; a fire is a one-frame phenomenon with no
"current value" to retain, so change detection does not apply. A connection
with `previous: true` (§7) reads "fired on frame N−1"; the §10 acyclicity
rule applies unchanged.

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

| Kind     | Name       | Type      | Default | Notes                          |
|----------|------------|-----------|---------|--------------------------------|
| property | `src`      | `string`  | —       | path                           |
| property | `loop`     | `bool`    | `true`  |                                |
| input    | `playing`  | `scalar`  | `1`     | > 0.5 plays; otherwise pauses  |
| input    | `restart`  | `event`   | —       | on fire: seek to 0, decode first frame |
| output   | `result`†  | `texture` |         | dirty on each new decoded frame|
| output   | `finished` | `event`   |         | fires when the final frame of a `loop: false` stream is produced |

- **Pause, not stop**: while `playing` ≤ 0.5, decoding is suspended (the same
  machinery as presentation-suspend), the playback position freezes, and
  `result` holds the last decoded frame, non-dirty. Resuming continues from
  the paused position.
- Change detection (§11) means a `playing` signal wired from a step-like
  source (e.g. a `sequence` `hold` track) is dirty only on the frames it
  actually flips — the node sees clean start/stop transitions for free.
- `restart` fires while paused: the first frame is decoded and `result` goes
  dirty once (a poster frame); playback then waits on `playing`.
- **End of stream** (`loop: false`): after the final frame is produced the
  node holds that frame (`result` non-dirty) and enters the finished state;
  `playing` has no further effect until `restart` fires, which rearms
  playback from the start. `finished` never fires while `loop: true`.
  Chaining is the point: `finished` wired into another video's `restart`
  enables playlist- and intro/loop-style scenes.

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
Nodes may rely on this (e.g. `sequence` cue-crossing, §9.9, is defined only
for forward motion). Editors that scrub backward do so by re-evaluating the scene
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
through named **tracks**, each of which becomes an output port. Value
tracks emit interpolated levels; event tracks fire at cue times.

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
- `kind` (required): `"value"` or `"event"`.

Value tracks:

- `type` (required): `scalar`, `vec2`, `vec3`, or `vec4`.
- `interpolate` (optional, default `"linear"`): `"hold"` (step), `"linear"`,
  or `"smooth"` (smoothstep between keys). Per-key easing is reserved.
- `keys` (required, non-empty): array of `{ "t": seconds, "value": literal }`,
  strictly ascending `t`, each `t` in `[0, duration]`.
- Before the first key the output is the first key's value; after the last,
  the last key's value. There is no wrap-around interpolation: when looping,
  the value holds until local time wraps back past the first key — for a
  seamless loop, end with a key at `duration` matching the first key.

Event tracks (output type `event`, §4):

- `fires` (required, non-empty): ascending array of times in `[0, duration)`.
- Crossing semantics: with previous local time `t0` and current `t1`, cues in
  `(t0, t1]` fire; if the loop wrapped, cues in `(t0, duration)` and `[0, t1]`
  fire. On the node's first evaluation, cues in `[0, t1]` fire. A cue
  therefore fires exactly once per pass of the playhead, including once per
  loop iteration.
- Crossing assumes a monotonic `time` input. Value tracks are pure functions
  of `time` and tolerate any signal (the curve-lookup use below), but an
  event track on a `sequence` whose `time` input is not wired from `@time`
  is a load warning.

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
  by the runtime (linear filtering, clamp-to-edge). Naming it
  `foo_sampler_repeat` instead requests repeat addressing (linear filtering,
  wrap both axes) — the tiling/scroll idiom. Further sampler configuration is
  reserved.
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
unbound shader port; `event` connection to a non-`event` port or vice versa
(§4: events take part in no conversions; the same for `buffer`). For
`compute` (§18.2): missing `@compute` entry point; a compute interface in a
`shader` node or vice versa; a `buffer` connection whose element strides
differ; missing/invalid `capacity`; malformed `dispatch`; no storage
outputs. For `sequence`: `duration` missing
or ≤ 0; missing or empty `tracks`; duplicate or invalid track `name`;
unknown track `kind`; value track with empty `keys`, non-ascending `t`, `t`
outside `[0, duration]`, or `value` not matching `type`; event track with
empty or non-ascending `fires` or a time outside `[0, duration)`.

Load warnings: unknown top-level field; a non-object `editor` block (§2.1);
unknown node property; node unreachable from `output`; unknown parameter
`hint`; a `sequence` output port that nothing references; an event track on a
`sequence` whose `time` input is not wired from `@time` (§9.9).

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

- Event payloads, and event→level `latch`/`toggle` value nodes (§4, §9.9)
- Per-key easing and wake-until-next-key scheduling hints (§9.9)
- Inline references inside vector literals, as sugar desugaring to `combine` (§7, §9.9)
- Per-layer blend modes on `compositor` (§9.5)
- Pixel-exact `fit` mode on `transform` (§9.4)
- Custom sampler configuration for shader texture ports (§9.10)
- Damage-rectangle dirty granularity for textures (§11)
- Additional value nodes (add/multiply/mix/clamp, …) (§9.9)
- Additional implicit input nodes (e.g. `audio`) (§9.7)
- `modules/` (WASM logic) project directory (§1); `graphs/` is discharged
  by §19

## 16. Events & Sequencing (adopted)

Status: **fully adopted 2026-07-04** — first the level-triggered half
(`video.playing`, `sequence` value tracks), then the edge-triggered half
(the `event` edge type, `sequence` event tracks, `video.restart` and
`finished`). Normative text lives in §4 (the `event` type), §9.2 (video
playback control and end of stream), and §9.9 (`sequence`). Reserved
growth moved to §15: event payloads, `latch`/`toggle` nodes, per-key
easing, wake-until-next-cue scheduling hints.

### 16.1 Editor Mapping (non-normative)

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

## 17. Proposed Amendments: Spec Gaps (draft, not yet adopted)

Status: proposals. Each subsection is independently adoptable and states which
existing section it amends.
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
goes through `fit`.

Implementation status: **adopted and implemented 2026-07-05.**

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
fail with an f32 value path. `sequence` (§9.9) performs the same reduction,
unit-tested at a month of scene time likewise.

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

**Adopted 2026-07-04** into §9.2 alongside the event half of §16: the
finished state, the `finished: event` output, and `restart` rearming are
specified there and implemented (the decoder marks the final frame of a
non-looping stream; a restart seeks to the start and re-decodes).

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

## 18. Buffers, Compute, and Particles

Status: **fully adopted and implemented 2026-07-04** — §18.1/§18.2 with
`examples/swarm.sceneproject` (raw compute idioms), §18.3 with
`examples/sparks.sceneproject` (the stock particle nodes: cue-driven
bursts, pointer attraction, life curves), both golden-tested. §18.5
(particle growth: trails, spritesheets, multi-/sub-emitters, emission
windows, depth + parallax, ring vortices, collisions, sorted draws)
adopted the same day. Buffer
feedback is realized as history copies exactly like textures. Simulation
runs on the GPU: state lives in storage buffers, emission and update are
compute passes, rendering is an instanced draw. The CPU contributes
uniforms — for `particles`, that is dt plus the deterministic emission
window (rate accumulator and burst counts mapped onto a ring cursor, so
the oldest slots recycle at capacity).

Authoring note (non-normative): WGSL reserved words (`target`, `filter`,
`sample`, …) are rejected by the compiler as binding names even though the
reflection recognizer accepts them; the loader surfaces the compile error.

Design note (non-normative): the §18.3 vocabulary covers what established
2D particle systems offer (shaped emitters; randomized color/size/alpha/
lifetime/velocity at emission; gravity/drag/turbulence/vortex/attract
forces and fade/size/color-over-life curves; pointer-linked forces) with
the structural upgrades the graph model makes natural: every knob is a
port (wireable from `sequence` curves, `@mouse`, a future `audio` node),
bursts are event-driven (§16), and authored `compute` nodes interoperate
with the stock nodes through a public buffer contract.

### 18.1 The `buffer` Edge Type

Amendment to §4 — new edge/value type:

| Type     | JSON literal form    | Notes                                    |
|----------|----------------------|------------------------------------------|
| `buffer` | — (connections only) | fixed-capacity array of structs, GPU-resident |

- A `buffer` value is an array of `capacity` fixed-size elements in a GPU
  storage buffer. Capacity and element stride are set at load by the
  producing node and never change at runtime.
- Typing: a `buffer` connection is valid when producer and consumer declare
  the same element *stride* (WGSL memory layout). Field-level interpretation
  is a convention between producer and consumer; §18.3 fixes one such
  convention (the `Particle` struct) for the stock nodes.
- `buffer` takes part in no implicit conversions; parameters cannot be of
  type `buffer`.
- Feedback: `previous: true` on a `buffer` edge is realized as ping-pong
  buffers, exactly like textures (§10). On the first frame (and after a
  node reload) a previous-frame buffer reads as all zeroes.
- Dirty semantics (§11 amendment): change detection is impractical for GPU
  buffer contents, so a `buffer` output is dirty whenever its node executes.
  A self-sustaining simulation therefore follows the §10 feedback rule: it
  keeps running once kicked, and quiesces only when nothing upstream of it
  is dirty — wire `@time.delta` into a simulation to tick it every rendered
  frame, and gate that wire to pause it.

### 18.2 `compute`

GPU compute pass. Like `shader` (§9.10), the WGSL file *is* the node's port
declaration; the runtime reflects it and binds by name.

| Kind     | Name       | Type      | Default  | Notes                              |
|----------|------------|-----------|----------|------------------------------------|
| property | `shader`   | `string`  | —        | path to WGSL with a `@compute` entry point `main` |
| property | `capacity` | `scalar`/object | —  | element count for owned buffer outputs; object form maps port → count when there are several |
| property | `dispatch` | `[x,y,z]` | `"auto"` | workgroup counts; `"auto"` covers the primary output (buffer capacity or storage-texture size) with the entry point's workgroup size |
| input    | *(from WGSL)* | *(reflected)* |  | see below                          |
| output   | *(from WGSL)* | *(reflected)* |  | first storage binding is the default output† |

Reflection rules, extending §9.10:

- A uniform struct binding named `params` contributes value input ports per
  field, exactly as §9.10.
- `texture_2d<f32>` bindings are texture input ports (with the `_sampler`
  convention); `texture_storage_2d<…, write>` bindings are texture output
  ports, allocated by this node (`size` rules follow §9.3/§17.5).
- `var<storage, read> name: array<S>` is a `buffer` input port.
- `var<storage, read_write> name: array<S>` is a `buffer` output port,
  allocated by this node with `capacity` elements. It may also be read in
  the kernel; combined with a `previous: true` self-connection it is the
  ping-pong simulation idiom.
- All reflected ports must be bound (§9.10's rule); `capacity` is required
  when any storage output exists.

Validation additions (§13): missing `@compute` entry point; a `buffer`
connection whose element strides differ; `capacity` missing, non-integral,
or ≤ 0; `dispatch` malformed.

### 18.3 Particle Nodes: `particles` and `sprites`

The no-WGSL particle path, split so simulation and rendering compose: a
`particles` buffer can feed several renderers, a custom `compute` node can
post-process it, and an authored simulation can drive the stock renderer,
all through the public element contract:

```wgsl
struct Particle {         // 64 bytes; the buffer contract for stock nodes.
                          // Scalars are packed into the vec3 alignment
                          // slots, so the WGSL layout is exactly 64 bytes.
    pos: vec3f,           // x/y in output space (§9 conventions); z is
                          // depth, reserved — the v1 simulation writes 0
    age: f32,             // seconds since emission
    vel: vec3f,           // output-space units per second; z reserved
    lifetime: f32,        // seconds; 0 = dead slot (renderers skip it)
    color: vec4f,         // premultiplied tint
    size: f32,            // fraction of output height (§9.4 sizing spirit)
    rotation: f32,        // degrees, clockwise
    seed: f32,            // emission ordinal, integer-valued f32 (§18.5.1);
                          // hash it for per-particle randoms
    emitter: f32,         // producing emitter index (§18.5.3); 0 when
                          // single-emitter (was `reserved`, see §18.5.1)
}
```

`particles` — emission + simulation over one particle pool:

| Kind     | Name         | Type      | Default      | Notes                       |
|----------|--------------|-----------|--------------|-----------------------------|
| property | `capacity`   | `scalar`  | `1000`       | pool size; dead slots recycle |
| property | `emitter`    | `string`  | `"point"`    | `point`, `box`, `disc`      |
| input    | `rate`       | `scalar`  | `10`         | particles/second; 0 stops emission |
| input    | `burst`      | `event`   | —            | on fire: emit `burstCount` at once |
| input    | `burstCount` | `scalar`  | `32`         |                             |
| input    | `origin`     | `vec2`    | `[0.5, 0.5]` | emitter center, output space |
| input    | `extent`     | `vec2`    | `[0, 0]`     | box half-size / disc radius (x) |
| input    | `direction`  | `vec2`    | `[0, -1]`    | mean initial velocity direction |
| input    | `spread`     | `scalar`  | `180`        | degrees of random cone around `direction` |
| input    | `speed`      | `vec2`    | `[0.05, 0.15]` | [min, max] initial speed  |
| input    | `lifetime`   | `vec2`    | `[1, 3]`     | [min, max] seconds          |
| input    | `size`       | `vec2`    | `[0.01, 0.03]` | [min, max] at emission    |
| input    | `spin`       | `vec2`    | `[0, 0]`     | [min, max] degrees/second   |
| input    | `colorStart` | `vec4`    | `[1,1,1,1]`  | tint at birth               |
| input    | `colorEnd`   | `vec4`    | `[1,1,1,0]`  | tint at death (lerped over life) |
| input    | `fadeIn`     | `scalar`  | `0.1`        | seconds to full alpha       |
| input    | `sizeEnd`    | `scalar`  | `1`          | size multiplier at death    |
| input    | `gravity`    | `vec2`    | `[0, 0]`     | output-heights/s²           |
| input    | `drag`       | `scalar`  | `0`          | 1/s velocity damping        |
| input    | `turbulence` | `scalar`  | `0`          | curl-noise force strength   |
| input    | `turbulenceScale` | `scalar` | `4`      | noise spatial frequency     |
| input    | `attractor`  | `vec2`    | `[0.5, 0.5]` | force target (wire `@mouse.position`) |
| input    | `attract`    | `scalar`  | `0`          | signed strength; negative repels |
| input    | `vortex`     | `scalar`  | `0`          | signed swirl strength around `attractor` |
| input    | `time`       | `scalar`  | —            | required; wire `@time.delta` (the sim tick) |
| output   | `result`†    | `buffer`  |              | `Particle[capacity]`        |

Ranged inputs (`speed`, `lifetime`, `size`, `spin`) are `[min, max]` pairs
sampled uniformly per particle. Randomness is deterministic: a fixed
per-slot/per-emission hash, so identical documents stepped with identical
`time` deltas produce identical pools (headless/golden rendering stays
exact).

`sprites` — instanced draw of a particle buffer into an output-sized layer:

| Kind     | Name        | Type      | Default  | Notes                          |
|----------|-------------|-----------|----------|--------------------------------|
| property | `blend`     | `string`  | `"add"`  | `add` or `over` (§12 premultiplied either way) |
| input    | `particles` | `buffer`  | —        | required; `Particle` contract  |
| input    | `texture`   | `texture` | —        | optional sprite; absent = built-in soft disc |
| output   | `result`†   | `texture` |          | output-sized, premultiplied    |

`over` draws in slot order (no depth sort in v1 — additive is order-free
and is the wallpaper workhorse).

### 18.4 Example

Sparks that erupt on a sequence cue and drift toward the pointer (a
complete, runnable variation ships as `examples/sparks.sceneproject`,
adding a parameter-driven rate and sequence-driven turbulence):

```json
{ "id": "cues", "type": "sequence", "duration": 10, "loop": true,
  "tracks": [ { "name": "boom", "kind": "event", "fires": [ 2, 7 ] } ],
  "inputs": { "time": "@time.seconds" } },

{ "id": "sparks", "type": "particles", "capacity": 4000,
  "emitter": "disc",
  "inputs": { "time": "@time.delta", "rate": 40,
              "burst": "@cues.boom", "burstCount": 600,
              "origin": [0.5, 0.8], "extent": [0.02, 0.02],
              "direction": [0, -1], "spread": 60,
              "speed": [0.1, 0.4], "lifetime": [1, 2.5],
              "gravity": [0, 0.25], "drag": 0.5, "turbulence": 0.35,
              "attractor": "@mouse.position", "attract": 0.1,
              "colorStart": [1, 0.8, 0.3, 1], "colorEnd": [1, 0.2, 0.1, 0] } },

{ "id": "glitter", "type": "sprites",
  "inputs": { "particles": "@sparks" } },

{ "id": "comp", "type": "compositor",
  "inputs": { "layers": ["@bg", "@glitter"] } }
```

### 18.5 Particle Growth (adopted)

Status: **adopted and implemented 2026-07-04**. Everything here extends
§18.3 without breaking existing documents: every new port has a default
that reproduces the old behavior, and the `Particle` contract keeps its
64-byte layout. Runnable, golden-tested examples:
`examples/comets.sceneproject` (trails over a ring vortex, depth +
pointer parallax, sorted `over` sprites) and
`examples/fireworks.sceneproject` (multi-emitter windows, sub-emitter
bursts, spritesheet twinkle, texture collision).

#### 18.5.1 Contract errata

Two fields of the §18.3 `Particle` struct are respecified to match need
and shipped behavior:

- `seed` is the **emission ordinal**: an integer-valued f32 (the running
  count of emissions modulo 2²⁴, so it stays f32-exact). Per-particle
  randoms are derived by hashing it; the §18.3 wording ("random in
  [0, 1)") described one such derived value, not the stored field. Custom
  consumers that only need *a* per-particle random can hash `seed` with
  any function of their choice.
- `reserved` becomes `emitter`: the index of the emitter that produced
  the particle (§18.5.3), 0 for single-emitter nodes. Custom simulations
  that write 0 (the old rule) remain conforming.

#### 18.5.2 `particles`: emission windows, ring vortex, depth, collisions

New inputs on `particles` (all optional):

| Kind  | Name       | Type      | Default    | Notes                            |
|-------|------------|-----------|------------|----------------------------------|
| input | `delay`    | `scalar`  | `0`        | seconds of sim time before emission starts |
| input | `duration` | `scalar`  | `0`        | emission window length; 0 = unbounded |
| input | `ring`     | `scalar`  | `0`        | when > 0, `attract`/`vortex` act toward the nearest point of the circle of this radius around `attractor` |
| input | `depth`    | `vec2`    | `[0, 0]`   | [min, max] initial z, sampled per particle |
| input | `collide`  | `texture` | —          | collision mask; alpha ≥ 0.5 is solid |
| input | `bounce`   | `scalar`  | `0.5`      | restitution on collision, 0..1   |

- **Emission windows.** Sim time is the accumulated sum of `time` deltas
  since load (not `@time.seconds` — deterministic under the sim tick).
  Emission (both `rate` and `burst`) is active only while
  `delay ≤ simTime` and, when `duration > 0`,
  `simTime < delay + duration`. Live particles keep simulating after the
  window closes.
- **Ring vortex.** With `ring = 0` the force center is `attractor`
  (§18.3, unchanged). With `ring > 0` the center is the nearest point on
  the ring — particles attract onto the circle and `vortex` swirls them
  around its circumference. Radius is in output-space units (no aspect
  correction, matching every other §18.3 spatial input).
- **Depth.** z is sampled uniformly in `depth` at emission and held
  constant over life (forces remain 2D in v1); it exists to feed parallax
  and sorting (§18.5.5). Convention: larger z reads as "farther" for
  sorting (drawn first under `over`); parallax offsets scale linearly
  with z, so foreground-moves-more is a negative `depth` range.
- **Collisions.** When `collide` is bound, the kernel samples it at the
  integrated position (output-space UV); alpha ≥ 0.5 is solid. On
  contact the particle is pushed out along the mask's alpha gradient and
  its velocity reflects with restitution `bounce`
  (`v' = v − (1 + bounce)(v·n)n`). A texture mask is deliberately the
  interface — any shape source works (an `image`, a `shader`, even a
  feedback texture); a true SDF input may join later for thin features.

#### 18.5.3 Multiple emitters

The `emitters` property turns one `particles` node into several emission
sources sharing a pool, forces, and life curves:

```json
{ "id": "show", "type": "particles", "capacity": 6000,
  "emitters": [
    { "emitter": "point", "origin": [0.3, 0.9], "weight": 2,
      "colorStart": [1, 0.6, 0.2, 1] },
    { "emitter": "disc", "origin": "@orbit", "extent": [0.05, 0],
      "delay": 3, "duration": 4 }
  ],
  "inputs": { "time": "@time.delta", "rate": 120 } }
```

- `emitters` is an array of 1–8 objects. Each entry may set the emitter
  shape (`emitter`) and any of the *emission-time* inputs — `origin`,
  `extent`, `direction`, `spread`, `speed`, `lifetime`, `size`, `spin`,
  `colorStart`, `colorEnd`, `depth`, `delay`, `duration` — plus `weight`
  (scalar, default 1). Fields accept the full §7 grammar (literals,
  `$param`, `@node.port` connections), and wired fields are live —
  entry fields are real input ports.
- An entry field that is absent falls back to the node-level input of
  the same name, so shared values are written once.
- The node-level `rate` (and `burstCount`) is the total; each frame's
  emissions are distributed across emitters in proportion to `weight`,
  deterministically (largest-remainder on the accumulated fractions, so
  identical documents stepped identically emit identically). An emitter
  whose `delay`/`duration` window is closed takes no share.
- Forces and per-frame life curves (`gravity` … `vortex`, `fadeIn`,
  `sizeEnd`) remain node-global. `colorStart`/`colorEnd` are per-emitter:
  the shipped simulation shades from the emitting entry's colors via the
  particle's `emitter` field (§18.5.1).
- Without an `emitters` property the node behaves exactly as §18.3 (one
  emitter, index 0).

#### 18.5.4 Sub-emitters (spawn on death)

A `particles` node whose `spawn` input is connected emits from *deaths*
in the source buffer instead of from an emitter shape:

| Kind     | Name         | Type     | Default | Notes                        |
|----------|--------------|----------|---------|------------------------------|
| input    | `spawn`      | `buffer` | —       | `Particle` source; emission sites are its deaths this tick |
| property | `spawnCount` | `scalar` | `4`     | children per death (load-time constant) |
| input    | `inherit`    | `scalar` | `0`     | fraction of the parent's velocity added to children |

- A death is a source slot whose `lifetime` was > 0 on the previous tick
  and is 0 now (the runtime keeps the one-frame history; the author wires
  only `spawn`). Particles culled by pool recycling also count — they
  died, just early.
- Children emit at the parent's last position (z included — the node's
  `depth` input does not apply), with the node's own
  `direction`/`spread`/`speed`/`lifetime`/`size`/`spin`/colors, plus
  `inherit` × the parent's final velocity.
- **Capacity is derived**: `spawn.capacity × spawnCount`, so every death
  has a home and slot assignment is static — source slot *s* owns child
  slots `[s·spawnCount, (s+1)·spawnCount)`. A redeath of a recycled
  source slot re-emits over its own children (its previous brood is the
  stalest by construction). A `capacity` property on a spawn-connected
  node is ignored with a warning.
- `rate`, `burst`, `burstCount`, `origin`, `extent`, `emitter`, `delay`,
  and `duration` do not apply when `spawn` is connected (warning if set);
  combining `emitters` with `spawn` is an error. Chaining sub-emitters is
  legal; capacities multiply.

#### 18.5.5 `sprites`: spritesheets, parallax, sorted & culled draws

New surface on `sprites`:

| Kind     | Name        | Type     | Default  | Notes                         |
|----------|-------------|----------|----------|-------------------------------|
| property | `sheet`     | `[cols, rows]` | —  | grid layout of `texture`; requires `texture` |
| input    | `frameRate` | `scalar` | `0`      | frames/second; 0 = one pass over the sheet across each particle's lifetime |
| input    | `parallax`  | `vec2`   | `[0, 0]` | screen offset per unit z, output space |

- **Spritesheet.** Frames are read row-major. With `frameRate = 0` a
  particle plays the whole sheet exactly once over its lifetime; with
  `frameRate > 0` it advances at that rate and loops, starting on a
  per-particle random frame (hashed from `seed`) so pools don't strobe
  in sync.
- **Parallax.** Each particle is offset by `parallax × z` at draw time
  (the simulation is untouched — several `sprites` with different
  `parallax` wires can draw one pool at several apparent distances).
  Wire something centered on zero (e.g. `@mouse.position` through a
  `remap` to [-0.5, 0.5]).
- **Sorted, culled draws** (behavioral change, no new surface): `sprites`
  compacts live slots on the GPU and draws only the live count via an
  indirect draw. Under `blend: "over"` the compacted instances draw
  back-to-front — descending z, ties broken oldest-first — which makes
  `over` deterministic and correct for parallax layering; §18.3's "slot
  order, no sort" rule is discharged. `add` remains order-free and skips
  the sort.

#### 18.5.6 `trails`

The rope/trail renderer — Catmull-Rom ribbons through per-particle
position history, over the same public buffer contract:

| Kind     | Name        | Type      | Default  | Notes                        |
|----------|-------------|-----------|----------|------------------------------|
| property | `length`    | `scalar`  | `16`     | history samples per particle, 2–64 |
| property | `blend`     | `string`  | `"add"`  | `add` or `over` (§12)        |
| input    | `particles` | `buffer`  | —        | required; `Particle` contract |
| input    | `width`     | `scalar`  | `1`      | ribbon width at the head, × particle `size` |
| input    | `taper`     | `scalar`  | `0`      | width multiplier at the tail |
| input    | `fade`      | `scalar`  | `0`      | alpha multiplier at the tail |
| input    | `parallax`  | `vec2`    | `[0, 0]` | as §18.5.5, applied per history sample |
| output   | `result`†   | `texture` |          | output-sized, premultiplied  |

- Each time the input buffer produces (each sim tick), the node records
  one history sample per particle into an internal ring; a slot whose
  particle was just emitted (`age == 0`) resets its ring to the emission
  point, so recycled slots never draw a streak from their previous life.
  Sample spacing is therefore the sim tick — history covers the last
  `length` ticks.
- The ribbon is a Catmull-Rom spline through the ring, tessellated at a
  fixed 4 subdivisions per segment, extruded perpendicular to its local
  tangent. Width runs `width × size` at the head to `taper ×` that at
  the tail; alpha runs the particle's color alpha at the head to
  `fade ×` it at the tail. Color is the particle's current premultiplied
  tint. Dead slots draw nothing.
- Determinism matches §18.3: identical documents stepped with identical
  deltas produce identical ribbons.

#### 18.5.7 Validation additions (§13)

`emitters` empty, longer than 8, containing a non-object, or an entry
field that is not an emission-time input; `sheet` malformed, non-integral,
< 1, or set without `texture`; `spawnCount` non-integral or < 1; `spawn`
stride mismatch (§18.1 rule); `length` outside 2–64; derived sub-emitter
capacity exceeding the implementation pool limit; warnings per §18.5.4
for inapplicable emission inputs and ignored `capacity`.

### 18.6 Reserved Growth

- `audio` implicit input node (§9.7 growth) — FFT bands as value ports;
  every §18.3/§18.5 input is already wireable, so audio-reactive
  particles need no new particle format.
- Event ports on authored `compute` nodes.
- 3D forces (gravity/turbulence in z; the contract already carries
  z-velocity), true SDF collision inputs, textured/spritesheet trails,
  per-emitter force overrides, emission along a spline.

### 18.7 Effect on §15

§15's `buffer` reservation is discharged. Rope/trail renderers,
spritesheets, sub-emitters, multi-emitters, parallax depth, and sorted
draws are adopted (§18.5); the audio node and compute event ports remain
on the reserved list (§18.6).

## 19. Subgraphs

Status: **adopted 2026-07-05**. Reusable building blocks composed from
nodes: a subgraph is a graph file under `graphs/` with a declared port
interface, and an instance of it is an ordinary node in any graph. The
runtime never sees subgraphs — instances flatten at load — so every
semantic of §10, §11, §16, and §18 applies to the inner nodes exactly as
if they were authored inline, at zero runtime cost.

### 19.1 Graph Files

One subgraph per file, `graphs/<name>.json`:

```json
{
  "version": 1,
  "name": "Glow",
  "description": "Threshold + two-pass blur, added back over the source.",
  "inputs": {
    "source":   { "type": "texture", "bind": "@thresh.source" },
    "strength": { "type": "scalar", "default": 0.6,
                  "bind": ["@blurX.amount", "@blurY.amount"] }
  },
  "outputs": { "result": "@mix" },
  "nodes": [ /* §8 nodes, unchanged */ ]
}
```

- `version` (required): 1.
- `name`/`description` (optional): editor labels; `name` defaults to the
  file stem.
- `nodes` (required): exactly §8. All built-in types are legal except
  `output` (a subgraph produces ports, not the frame). Implicit `@time`
  and `@mouse` references work as usual — they are global inputs.
- `$parameter` references are **not** allowed inside graph files: a
  subgraph's knobs are its declared inputs, so the file is self-contained
  and reusable across scenes. (The *instance* may bind its inputs to the
  scene's parameters.)
- Graph files may instantiate other subgraphs (§19.3). The instantiation
  depth is capped at 8 and cycles among graph files are a load error.
- Shader/asset paths inside a graph file resolve against the project root
  (§1), same as everywhere else.

### 19.2 The Interface

`inputs` — object, exported name → declaration:

| Field     | Type            | Notes                                        |
|-----------|-----------------|----------------------------------------------|
| `type`    | §4 type name    | required; `texture`, `event`, `buffer` allowed |
| `bind`    | ref or [refs]   | required; inner *input* port(s) this feeds    |
| `default` | literal         | value types only; absent ⇒ the input is required at the instance |

- Every `bind` target must be an input port of an inner node whose port
  type equals the declared `type` (a scalar declaration may bind to a
  vecN inner port; it splats per §4). One exported input may feed several
  inner ports.
- A `buffer` input's element stride is the bound inner port's declared
  stride; several binds must agree.
- An inner input port fed by an exported input cannot also be set in the
  inner node's own `inputs` (load error — one writer per port).

`outputs` — object, exported name → an inner `@node` / `@node.port`
reference. The exported port's type (and buffer stride) is the inner
port's. `result` must be present; it is the default output (§7's bare
`@instance` form). Exported names follow §3 identifiers in both tables.

### 19.3 The `graph` Node (instances)

| Kind     | Name    | Type     | Default | Notes                          |
|----------|---------|----------|---------|--------------------------------|
| property | `graph` | `string` | —       | path to the graph file         |
| input    | *(interface)* |    |         | the file's declared `inputs`   |
| output   | *(interface)* |    |         | the file's declared `outputs`  |

Instances participate in the outer graph like any node: interface inputs
accept the full §7 grammar (literals, `$param`, connections, `previous:
true` feedback edges), and interface outputs are wireable, including as
feedback sources.

**Flattening.** At load, each instance is replaced by its inner nodes
with namespaced ids — `<instance>/<inner>` (unambiguous: §3 ids cannot
contain `/`). Outer references to `@instance.port` rewrite to the mapped
inner node's port; exported inputs apply the instance's bindings (or the
declared defaults) to the bound inner ports. Diagnostics about inner
nodes use the namespaced id, so errors and warnings point into the right
instance. Two instances of one file share nothing at runtime — each gets
its own nodes, state, and dirty tracking.

### 19.4 Validation Additions (§13)

Graph file missing, unparseable, or wrong `version`; an `output` node or
`$parameter` reference inside a graph file; unknown `type` in an input
declaration; `bind` naming an unknown inner node/port, mismatching the
declared type, or colliding with the inner node's own binding; `default`
on a non-value type; missing `outputs.result`; an output reference to an
unknown inner node/port; an instance binding an input not in the
interface, or omitting a required one; nesting deeper than 8;
instantiation cycles among graph files.

### 19.5 Editor Mapping (non-normative)

An instance draws as a single node with the declared pins. Double- or
middle-click enters the subgraph's own canvas; a breadcrumb returns to
the parent. A connected multi-node selection collapses into a new graph
file via "make subgraph": boundary-crossing wires become the interface
(incoming → declared inputs, outgoing → outputs), and the selection is
replaced by one instance bound to the same outer wires.

### 19.6 Effect on §15

§1's `graphs/` reservation is discharged. Distributable node packages
around graph files are §20. Reserved growth: inline graph definitions in
`scene.json`, and re-exporting a subgraph input as a scene parameter
declaration in one step.

## 20. Packages

Status: **adopted 2026-07-05**. A package is a reusable, versioned bundle
of subgraphs plus the shaders and assets they need (DESIGN.md §8.2/§9). A
scene consumes a package by path — `packages/<name>/…` — resolved from
the machine's package store; nothing is copied into the project. The
future runtime distribution format (`.wallpkg`, DESIGN.md §6.2) is where
a scene's packages get bundled into a single standalone artifact; the
editable `.sceneproject` deliberately stays a thin reference.

### 20.1 Package Layout

```
<name>/
  manifest.json
  graphs/             # ≥ 1 graph file (§19.1) — the package's nodes
  shaders/            # WGSL referenced by the package's graphs
  assets/             # images/videos referenced by the package's graphs
```

`manifest.json`:

| Field         | Type     | Notes                                          |
|---------------|----------|------------------------------------------------|
| `name`        | string   | required; `[a-z0-9-]+`, must equal the directory name |
| `version`     | string   | required; dotted decimal integers, e.g. `"1.0.0"` |
| `description` | string   | optional; editor/repository label              |
| `license`     | string   | optional; SPDX identifier                      |
| `preview`     | string   | optional; package-relative image path          |

A package is **self-contained**: paths inside its graph files resolve
against the *package* root (amending §19.1's project-root rule, which
continues to apply to project-local graph files), and may not begin with
`packages/` — a package referencing another package is a load error
(dependency metadata is reserved future growth, not implied). `$param`
references stay banned inside graph files (§19.1); a package's knobs are
its graphs' declared inputs.

### 20.2 References and Resolution

A scene references package content with ordinary §1 paths that begin
with `packages/`:

```json
{ "id": "sway", "type": "graph", "graph": "packages/sway/graphs/sway.json" }
```

Any path position may use the form — `graph`, `shader`, `src` — so a
package can also ship bare shaders or textures.

Resolution order for `packages/<name>/<rest>`:

1. `<project>/packages/<name>/<rest>` — a project-local copy always wins
   (regardless of pins): it is the manual-vendor escape hatch and behaves
   as ordinary project content.
2. The package store: `<store>/<name>/<version>/<rest>`, where `<store>`
   is each entry of `DRIFT_PACKAGE_PATH` (colon-separated) if set, else
   `$XDG_DATA_HOME/drift/packages`, else `~/.local/share/drift/packages`.
   Stores are searched in order; within the first store holding a version
   that matches the pin (§20.3), the highest wins — versions compare as
   dotted integer segments, missing segments zero.

Store content is read-only to the runtime and tools. If no candidate
resolves, the load fails with the ordinary missing-asset error naming the
package and pin.

### 20.3 Version Pinning

Optional `scene.json` top level:

```json
"packages": { "sway": "1" }
```

Each entry pins one package name to a version *prefix*: `"1"` admits any
installed `1.x…`, `"1.2"` admits `1.2.x…`, a full version is exact. Names
not referenced by any `packages/` path produce a load warning (stale
pin). Unpinned references take the newest installed version. The block is
also the scene's dependency manifest: it is what a future resolver
fetches and what the `.wallpkg` exporter bundles.

### 20.4 Repositories

A repository is a directory or static HTTP host:

```
index.json
<name>/            # one directory per package, laid out as §20.1
```

`index.json`: `{ "version": 1, "packages": [ … ] }`, one entry per
package version:

| Field         | Type   | Notes                                            |
|---------------|--------|--------------------------------------------------|
| `name`        | string | matches the package `manifest.json`              |
| `version`     | string | matches the package `manifest.json`              |
| `description` | string | optional, from the manifest                      |
| `files`       | array  | `{ "path", "sha256", "size" }` per file, sorted by path |
| `hash`        | string | sha256 of the canonical file list (see below)    |

The package `hash` is `sha256` over the text `"<sha256>  <path>\n"` for
every file, sorted by path — computable from `files` alone, so a client
can verify a package fetched file-by-file over HTTP without a tarball.
Install verifies every file hash and the tree hash, then records
`{ "repository", "hash" }` beside the package in the store
(`.installed.json`, ignored by resolution). First-party packages live in
this repository's top-level `library/`, which is itself a valid package
repository.

### 20.5 Validation Additions (§13)

Package manifest missing/unparseable, `name` not matching its directory,
or malformed `version`; a `packages/` path inside a package's own files;
a `packages/` path whose `<rest>` does not name package content
(`graphs/`, `shaders/`, `assets/`); a malformed `packages` pin block
(non-object, non-string pin, or unknown-name warning per §20.3); a
`packages/` reference that resolves to no installed version.

### 20.6 Editor Mapping (non-normative)

The add-node palette lists graphs from resolvable packages alongside the
project's own `graphs/`. Drill-in (§19.5) into a package instance is
read-only: the store is not project content, and `write-asset` cannot
reach it — "edit a copy" is a future affordance built on the §20.2
project-local override.
