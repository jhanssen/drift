# Real-Time Desktop Scene System (Working Requirements)

## 1. Purpose

The system provides a declarative, GPU-accelerated desktop scene runtime for animated wallpapers and ambient visual experiences.

It is designed to replace static images and video wallpapers with a hybrid system of video, shaders, and procedural effects, optimized for efficiency and extensibility.

The ecosystem prioritizes:

- Low idle GPU/CPU usage
- Composability of visual effects
- Creator accessibility
- Extensibility without native plugins
- Cross-platform portability
- Open distribution via static repositories

## 2. Core Concept

A wallpaper is represented as a **scene graph** composed of:

- Video textures (e.g. AV1/H.264 decoded frames)
- Images/textures (KTX2 or similar GPU-ready formats)
- WGSL shader nodes (WebGPU-compatible)
- Compute nodes (GPU simulation / effects)
- WASM logic modules (deterministic control logic)
- Inputs (mouse, audio, time, system state)
- Outputs (parameter bindings, visual properties)

Everything is connected via a render + compute graph.

## 3. Runtime Requirements

### 3.1 Core Runtime

The runtime is a single C++ codebase written against the standard `webgpu.h` API, built for two targets:

- **Native**: links Dawn as the WebGPU implementation
- **Browser**: compiled to WASM (Emscripten), with WebGPU calls forwarded to the browser's native WebGPU implementation (Dawn is not part of this build)

One codebase for the scene graph and execution logic; only the GPU backend and platform integration (e.g. video decoding: native decoder vs. WebCodecs) differ per target.

Responsibilities:

- Scene graph execution
- Resource management (textures, buffers, assets)
- Shader pipeline execution (WGSL)
- Compute dispatch
- Frame scheduling and optimization
- WASM module execution host

### 3.2 Rendering Model

- Based on a graph of nodes
- Nodes represent:
  - Texture sources (video, images)
  - Shader passes
  - Compute passes
  - Compositing stages
- Frame updates are dependency-driven, not fixed-timestep
- Feedback loops:
  - The graph is acyclic within a single frame
  - A node may read its own (or another node's) output from the previous frame, realized as ping-pong buffers with a one-frame delay
  - This supports iterative effects (fluid simulation, trails, reaction-diffusion) without in-frame cycles
- Idle optimization:
  - Nodes only update when inputs change
  - Supports non-frame-based updates (audio, events, timers)

### 3.3 GPU Backend

- WebGPU via Dawn
- WGSL as shader language
- Compute shaders used for:
  - particles
  - fluid systems
  - procedural effects
  - data-driven animation
- **Per-device Basis transcode target (planned):** KTX2/Basis sources
  currently transcode to BC7 unconditionally. Request the adapter's
  compression features (`texture-compression-bc` / `-astc` / `-etc2`)
  at device creation and pick the target per device — BC7 on desktop,
  ASTC 4×4 where that's all there is, RGBA8 decompression as the
  graceful floor (mobile browsers). Authoring constraints
  (premultiplied-or-opaque, multiple-of-4 dimensions) hold for every
  block target; the decode path needs the device's feature set plumbed
  in, which it currently does not receive.

## 4. Scripting / Logic System

### 4.1 WASM Modules

- Logic is implemented as WebAssembly modules
- No native plugins or dynamic libraries allowed
- Modules are sandboxed with no ambient authority: the host API is the only capability surface
- Modules are host-agnostic: the same module runs unmodified in the native and browser runtimes

### 4.2 Execution Model

Modules operate on:

- **Input buffer** (provided by host)
  - mouse position (declared input — §4.4)
  - audio analysis (declared input — §4.4)
  - time
  - scene parameters
- **Output buffer**
  - parameter updates
  - event triggers

Execution pattern:

```
Host writes inputs → WASM update() → outputs written → host applies changes
```

### 4.3 Host API

- Minimal, data-oriented ABI
- No direct rendering access — no GPU object ever crosses the module
  boundary; GPU-using effects are composed declaratively (§4.5)
- Default-deny I/O: a module gets no storage or network unless it
  declares the capability and the user grants it (§4.4)
- Stable versioned interface

### 4.4 Declared Capabilities: Storage & Network

Design revised 2026-07-10 (modules themselves remain post-v1, §13.3).
The original blanket "no filesystem or network access" is relaxed to
**default-deny plus declared capabilities**. The load-bearing invariant
was never "no I/O" — it is §4.1's *no ambient authority*: the host API
is the only capability surface. Storage and HTTP extend that surface as
capabilities a module must declare in its manifest and the user must
grant at install time; an undeclared capability does not exist, so
today's behavior remains the default.

Because the same module runs unmodified in both runtimes (§4.1), the
portability rule is: **the browser is the stricter envelope — design
the ABI to the browser's envelope and re-implement that envelope
natively.** In the browser, enforcement (CORS, storage quotas,
private-network blocking) comes from the platform; natively the runtime
is the enforcer — the WASM engine only guarantees memory isolation, and
all policy lives in the host functions.

**Manifest permissions.** The module's package manifest grows a
`permissions` block; absence of a key means no access. Time and scene
parameters are always available; mouse and audio are declared inputs:

```json
"permissions": {
  "inputs": ["mouse"],
  "storage": { "quota": 1048576 },
  "network": { "origins": ["https://api.open-meteo.com"] }
}
```

Network grants are origin allowlists (the shape of CSP `connect-src`),
never blanket access. The installer/editor surfaces the grant, and
surfaces declared inputs and network origins *together* (§11).

**Storage: key-value, not a filesystem.** `get`/`put`/`delete`/`list`
over a namespace scoped per (scene, module), quota-enforced. Native
backs it with a per-namespace file under `$XDG_STATE_HOME/drift/`; the
browser backs it with OPFS partitioned the same way. The module never
sees a path, so traversal is structurally impossible and the semantics
are identical on both targets. If the browser module host runs in a
worker, OPFS sync-access handles permit a *synchronous* storage ABI
matching native — decide before freezing the ABI (§13.1). Storage
shared across scenes by package identity is deliberately absent (it is
a tracking channel); opt-in sharing is reserved if a real need appears.

**HTTP: fetch-shaped, asynchronous, handle-based.** `update()` is a
synchronous buffer exchange and browser fetch cannot block, so a module
issues a request descriptor, receives a handle, and observes completion
in a later `update()`. Native enforcement to match the browser
envelope: origins from the manifest allowlist only; HTTPS-only (plain
`http://localhost` permitted only in `--windowed`/dev modes);
private/link-local address ranges blocked, with the check applied
*post-DNS-resolution* (DNS-rebinding defense); redirects confined to
the allowlist; response-size and request-rate caps. CORS applies in the
browser build and cannot be waived, so the browser is the compatibility
floor: modules must target CORS-permissive endpoints, or they will work
natively and silently fail in the web editor.

**Determinism.** Deterministic modules underpin golden tests and scrub
preview (SCENE_FORMAT.md §16.1). Capabilities are injected effects: in
headless/golden/scrub modes the host denies them with a defined error
code (indistinguishable from being offline, which modules must handle
anyway) or replays recorded responses. The failure/offline semantics
are part of the ABI from day one, not an afterthought.

**Power.** Network completions and retry timers ride §3.2's
non-frame-based updates; the host coalesces and rate-limits requests so
a polling module (weather, transit) does not defeat the idle story.

### 4.5 Modules and the GPU: Ports, Not Pipelines

Design adopted 2026-07-10; **implemented the same day on both targets**
— the `module` node runs in the browser runtime and editor (per-node
`WebAssembly.Instance`s) and natively via the Wasmtime embedding
(`platform/ModuleWasmtime`), with `examples/spring.sceneproject` as the
pure CPU-transform demo, unit tests driving the whole contract through
a fake instance, real-engine tests (convergence, watchdog trap,
handshake failures), and a golden. The Pulley default holds: the pinned
C-API artifact (v46.0.1) ships the Pulley feature — v36-line LTS
artifacts do not — and the engine still probes `pulley64` with an empty
module at startup, falling back to the Cranelift JIT with one stderr
line if a Pulley-less build is ever substituted. Pulley and JIT produce
bit-identical module output (NaN canonicalization + IEEE f32), so
goldens hold across backends. Not yet implemented: `wake_after_ms`
(accepted, not honored) and the §4.4 storage/network capability
imports.
**Everything that crosses the module boundary is data** — values,
events, buffer contents — never a GPU handle. Modules do not create
pipelines, encode passes, or submit work; a "GPU-using WASM effect" is
a *package* (SCENE_FORMAT.md §20) whose subgraph composes ordinary
reflected `shader`/`compute`/`sprites` nodes with one **`module` node**
— the WASM brain — wired to them with ordinary edges. The reduction
that makes this sufficient: a declared pipeline is exactly a
`shader`/`compute` node (WGSL reflected into ports, §9.10/§18.2); a set
of pipelines with intermediate textures/buffers is exactly a subgraph
(§19); the distributable bundle is exactly a package. N pipelines need
no new machinery — they are edges, and the engine owns allocation,
topological ordering, synchronization, and dirty propagation.

**The `module` node.** Properties: `module` (wasm path), `interface`
(JSON path — there is no WGSL to reflect, so ports are declared):

```json
{
  "abi": 1,
  "inputs": {
    "attractor": { "type": "vec2",   "default": [0.5, 0.5] },
    "tick":      { "type": "scalar", "default": 0.0 },
    "excite":    { "type": "event" }
  },
  "outputs": {
    "goals": { "type": "buffer", "stride": 16, "capacity": 4096 },
    "heat":  { "type": "scalar" },
    "burst": { "type": "event" }
  }
}
```

Types are the SCENE_FORMAT.md §4 system; `buffer` is permitted on
**outputs only** (below). Buffer outputs follow §18.1 — fixed stride
and capacity at load; a graph node property may override `capacity`
(the `compute` pattern). Module ports are ordinary ports: wireable,
referenced as `brain.goals`, rendered by the editor like any node.

**ABI.** Module exports `drift_abi()` (version, must match the
interface's `abi`), `drift_init(ioSize)` (returns the block's address in
module memory), `drift_update()`. The host computes one I/O block layout
*from the interface JSON* — canonical port order is **lexicographic by
name within each direction**, so no tool that rewrites the JSON with
reordered keys can change the ABI; values pack as consecutive f32
components, 4-byte aligned — and exchanges data around each
`drift_update()` — no per-value host calls. Browser glue reads/writes the WASM memory directly, so the
ABI is identical on both targets by construction. The block begins with
an implicit header:

```c
struct DriftHeader {
  float    time;           // scene time, seconds (non-decreasing)
  float    dt;             // since this node's last update; 0 on first
  uint32_t frame;          // scene frame counter
  uint32_t flags;          // bit 0: first update after (re)load
  uint32_t events_in;      // bitmask over declared input events
  uint32_t events_out;     // module sets; host reads and clears
  uint32_t wake_after_ms;  // 0 = none; else "run me again in ≤ N ms"
  uint32_t _reserved;
};
```

then input values, then value outputs, then per buffer output a
`{count, written}` control pair followed by `capacity × stride` bytes
of staging. On `written = 1` the host performs one `queue.writeBuffer`
from the staging region and clears the flag. The engine creates and
owns every GPU resource; the module only ever writes its own linear
memory. GPU-to-GPU intermediates between pipelines never round-trip
through the module.

**Execution hosts (decided 2026-07-10).** Native embeds **Wasmtime**
via its C API, as a prebuilt per-platform tarball (the Dawn precedent —
pin versions and hashes, use signed releases). Configuration: WASI
disabled entirely (the §4.3/§4.4 host imports are the only capability
surface); epoch interruption armed around each `drift_update()` as the
runaway-module watchdog (trap → disable the node, never freeze the
wall); NaN canonicalization on, so goldens and §16.1 recorded passes
hold across machines and backends; the Pulley interpreter as the
hardened default (no executable-memory allocation —
`MemoryDenyWriteExecute`-compatible) with Cranelift JIT as opt-in; one
store/instance per node, so two nodes sharing a module file get private
memories. Decisive criteria over the contingency: the engine is the
security boundary for untrusted community content in a persistent
desktop process, so a memory-safe implementation, first-class
interruption, and determinism knobs outweigh footprint. **WAMR**
(interpreter-only, FetchContent from source) is the recorded fallback
if the tarball or binary size ever becomes untenable. In the
**browser** there is no embedded engine: each module is an ordinary
`WebAssembly.Instance` beside the Emscripten-compiled runtime — one
compiled `WebAssembly.Module` per file, one instance per node — driven
by the same shared-C++ graph evaluator through a thin JS
memcpy-and-call bridge (scheduling, block layout, change detection, and
capability *policy* all stay in shared core code; the bridge is
deliberately too dumb to diverge). Buffer staging may upload straight
from the module instance's memory via `queue.writeBuffer`, skipping the
runtime heap entirely.

**Scheduling & dirtiness.** The node executes when an input port is
dirty, an input event fires, `wake_after_ms` elapses (§3.2's timer
wake — what a §4.4 polling module uses), or its tick input is wired to
`@time.delta` (the §18.1 simulation idiom; gate the wire to pause).
Value outputs are change-detected like value nodes; events are dirty
when fired; buffer outputs are dirty **only when `written` is set** — a
refinement of §18.1's dirty-on-execute that the staging flag enables. A
brain that wakes and writes nothing propagates no GPU work: the idle
story holds end to end.

**Shaders stay separate text files.** The wasm contains no WGSL and
knows nothing of pipelines. Sibling `.wgsl` files keep reflection as
the interface mechanism, stay auditable/signable/load-validated and
repository-hashable, hot-reload through the editor's existing path, and
remain reusable by other scenes (§20.2 bare-shader references). The
brain↔shader contract cannot version-skew (the package is the atomic
unit, resolved per version) and is checked mechanically at load
(§18.1 stride matching).

**Deliberately absent.** Runtime-generated WGSL (unauditable,
unsignable, unvalidatable at load — permanently excluded). Buffer and
texture *inputs* to modules — GPU→CPU readback stalls the frame;
restructure so the GPU consumes CPU data, or use indirect dispatch
(reserved) when the GPU must drive its own workload size. An explicitly
asynchronous, frames-late readback port is a possible later addition,
as a separate decision.

**Rejected alternatives** (recorded so they are not re-litigated):

- *Raw WebGPU access from modules.* Survivable on security grounds
  (Dawn/browser validation is designed for hostile input) but not on
  architectural ones: an opaque command stream defeats dirty-driven
  idling, editor introspection, determinism/goldens, and texture-pool
  lifetime analysis; it re-exports a huge, still-evolving API through a
  boundary that promises a stable versioned interface (§4.3); and
  resource appetite (VRAM exhaustion, pathological dispatches, GPU
  hangs) lands on the device shared with the whole desktop session.
- *A mediated handle-table GPU ABI* (declared ports for I/O, but the
  module records passes against host-owned encoders via opaque handles;
  `dawn_wire` prior art). Fixes the security and graph-integration
  problems, but is a permanent ABI-maintenance tax that the declarative
  form makes unnecessary.

**Reserved growth** (mirrored in SCENE_FORMAT.md §15): a declared
`draw`/mesh node (vertex+fragment WGSL reflected like everything else,
geometry via `buffer` ports — the §13.3 3D plan, which this makes
implementable as a package); `compute.dispatch` promoted from property
to wireable port; indirect dispatch; subgraph iteration for dynamic
pass counts; async readback ports.

## 5. Editor System

### 5.1 Browser-Based Editor

- Runs in WebGPU-enabled browser
- Uses the same C++ runtime core compiled to WASM, so preview behavior matches the native runtime by construction
- Provides live preview of scenes

### 5.2 Native Companion App (optional)

- Connects to runtime via IPC/WebSocket
- Provides system-level wallpaper integration
- Shares same scene format and protocol as browser editor

### 5.3 Editor Architecture

- Two-way live synchronization with runtime
- Uses a structured protocol (e.g. JSON-RPC or WebSocket messages)
- Supports:
  - node creation/removal
  - parameter editing
  - shader hot reload
  - module recompilation
  - live preview updates (<100ms target)
- **Undo/redo (planned, not yet implemented):** document snapshots
  captured at the editor's two write chokepoints (the scene-document
  push and graph-file writes), so Ctrl+Z restores the previous document
  and writes through — undoing the project folder too, which §5.4's
  continuous save makes necessary. Runtime-only state (parameter slider
  values) and the WGSL pane's own text history are deliberately outside
  it.

### 5.4 Project Storage Providers

Design adopted 2026-07-05; first implementation same day (directory and
origin-private projects, recents, write-through, the failure-driven
store overlay, asset drag-drop — zip import/export and `list-assets`
remain). The editor reads and writes projects through a **storage
provider** abstraction; the WASM runtime's in-memory filesystem is only
the working copy the preview renders from, never the source of truth.
Every project write funnels through one path and fans out to the active
provider.

Providers:

- **Bundle** (exists): the examples baked into the web build. Read-only;
  editing one forks it into another provider ("save as").
- **Live wall** (exists): a native runtime with the control endpoint;
  `read-asset`/`write-asset` operate on the real project directory. The
  protocol needs a `list-assets` verb (enumerate the project tree) so
  the editor can browse and mirror an opened project — today it can only
  address files it already knows by name.
- **Local directory**: the user picks a real `.sceneproject` folder via
  the browser's directory-picker API (`showDirectoryPicker`,
  read-write mode). Access is user-granted, scoped to the picked
  subtree, and revocable — the same confinement contract as the
  runtime's project root (SCENE_FORMAT.md §1), so a project edited in
  the browser is byte-for-byte the folder the native runtime runs.
  Handles persist (IndexedDB) to power "recent projects", with a
  re-confirmation prompt per session. Chromium-only; other browsers fall
  back to the providers below.
- **Origin-private storage (OPFS)**: default home for "new project"
  when no directory is picked — persistent across reloads, no
  permission friction, works in every modern browser. Invisible to the
  user's real filesystem, so it always pairs with export.
- **Zip import/export**: universal bridge and the offline interchange —
  "download project" packs the tree; dropping a zip (or a folder via a
  directory-typed file input) opens one anywhere. Shares its packing
  code with the future `.wallpkg` exporter (§6.2), which is this plus
  vendored packages and a manifest.

Asset ingestion rides the same funnel: drag-drop or file-pick →
`assets/`/`shaders/` in the active provider (drop on the graph canvas
also creates the `image`/`video`/`shader` node). Surfaced constraints,
not discovered ones: video must be mp4 (the built-in demuxer; remux
with `-c copy`), and the working copy is RAM — large files warn, and
streaming from the provider instead of mirroring is the reserved
follow-up.

**Packages.** The bundled `/packages` store is frozen at build time, so
packages installed after the build resolve on a wall but not in the
preview. Two closures, both riding SCENE_FORMAT.md §20.2 unchanged:

- Wall-connected: `read-asset` already resolves `packages/` paths
  through the wall's store (pin-aware). The editor mirrors a referenced
  package's files into the working copy's project-local `packages/`
  dir — the §20.2 override, which wins over every store by design.
  Enumeration comes from the `list-assets` verb, which resolves
  `packages/` prefixes the same way.
- Standalone: the browser is effectively a second machine with its own
  store, overlaid on the bundled one. It fills three ways: pointing the
  editor at a local store directory via the directory picker (the
  persisted handle makes it a live overlay — packages installed
  natively appear on the next visit, no copy step); the in-browser
  §20.4 repository client (`index.json`'s per-file hashes exist so a
  static host can be fetched file-by-file and verified) installing into
  origin storage; or a dropped package zip. Pickers cannot be seeded
  with arbitrary paths (browser policy) and the default store lives
  under a hidden directory, so the store-pick dialog shows the expected
  path (and the show-hidden-files hint); the documented setup for
  browser-centric use is `DRIFT_PACKAGE_PATH` pointed at a visible
  folder, which the runtime, the install tool, and the picker then all
  share — relocation, not symlinking: browser file access deliberately
  does not follow symlinks inside a granted tree (scope-escape
  defense), so a linked store is not reliable. Picker `id`s keep store
  and project picks remembering their locations independently. The
  store prompt is failure-driven: it appears only when a `packages/`
  reference misses after the project-local override, the bundled
  store, and any already-attached overlay.

## 6. Scene Format

### 6.1 Project Format

Human-editable project (specified in detail in [SCENE_FORMAT.md](SCENE_FORMAT.md)):

```
.sceneproject/
  scene.json
  shaders/
  modules/
  assets/
  graphs/
```

### 6.2 Runtime Package Format

Distribution format:

```
.wallpkg/
  manifest.json
  scene.bin
  shaders/
  modules/
  assets/
  preview.webp
```

- Content-addressed assets supported (hash-based optional)
- Immutable package artifacts preferred for distribution

## 7. Shader System

- Language: WGSL
- Executed via WebGPU
- Encapsulated as reusable effect nodes
- Supports:
  - fragment shaders
  - compute shaders
  - multi-pass pipelines

## 8. Node System

### 8.1 Node Types

- Video decoder
- Image texture
- Shader effect
- Compute effect
- WASM logic module
- Input sources (mouse, audio, time)
- Compositors

### 8.2 Node Packages

Nodes are distributable units:

```
node-package/
  manifest.json
  logic.wasm (optional)
  shader.wgsl
  ui.json
  thumbnail.png
```

## 9. Distribution System

### 9.1 Repository-Based Distribution

- Content hosted in static repositories
- Repositories may be Git-based or any HTTP static host
- No centralized server required

### 9.2 Repository Format

```
index.json
/packages/*
```

### 9.3 Features

- Multiple repositories supported
- Versioned packages
- Optional dependency resolution
- Cached content fetching
- Integrity via hashing (recommended)

## 10. Performance Goals

- GPU/CPU cost proportional to visible change: a fully static scene costs near nothing; continuous sources (e.g. video) pay their decode/render cost and no more
- Efficient partial updates (dependency-driven rendering)
- Fast shader hot reload (<100ms target)
- Minimal CPU overhead for idle scenes
- Scales down gracefully on low-end hardware
- **Transient texture pooling (planned):** today every texture-producing
  node owns a persistent intermediate; an animated effects chain holds
  N output-sized float16 targets that are dead after the frame's last
  read. The dirty semantics (SCENE_FORMAT.md §11) identify which targets
  truly persist — only nodes whose output may be read in a later frame
  without re-evaluating (static/intermittent sources). Every-frame-dirty
  nodes can lease targets from a per-frame pool, with lifetimes from the
  existing topological order; ports feeding `previous:` edges and the
  scene output are excluded. Pure runtime change, no format impact,
  golden-verifiable. A later, narrower second step: fusing consecutive
  *pointwise* fullscreen passes (adjust/tint/threshold — not
  neighborhood passes like blur) into one shader, trading WGSL interface
  composition for the intermediate and its bandwidth.

## 11. Security Model

- No native code execution
- No dynamic libraries
- WASM sandbox isolation
- Explicit host API surface
- Default-deny I/O for modules: storage and network exist only as
  manifest-declared, user-granted capabilities (§4.4)
- Optional signed packages for trust verification

**Exfiltration threat model (2026-07-10).** Granting a module both a
sensitive input and network access creates a channel the sandbox cannot
close: an author who controls both the module and an allowlisted origin
can stream inputs out, and no server-consent mechanism (CORS) applies
when the receiving server is a cooperating party — the same is true of
any web page. What makes the web's version tolerable is scoping: a page
sees the mouse only over its own viewport, only while its tab is open,
on a site the user chose to visit. A wallpaper breaks both assumptions —
it is persistent and its inputs are global (desktop-wide mouse, later
audio). Defenses, honestly ranked:

1. **Legible consent** — the installer shows declared inputs and
   network origins together: "reads mouse, talks to
   api.wallpaperguy.example" reads very differently from "talks to
   api.open-meteo.com, reads nothing". The combination is the signal;
   either alone looks innocent.
2. **Capability minimization** — mouse/audio enter the input buffer
   only if declared (§4.4), so the dangerous combination is rare and
   conspicuous rather than default. Most network-using modules need no
   pointer data at all.
3. **Channel degradation** — quantized or rate-limited pointer data for
   network-capable modules raises effort but prevents nothing;
   exfiltration needs trivial bandwidth.
4. **Distribution trust** — signing, review, and reputation at the
   repository layer (§9), where a malicious author is actually fought.

A user who grants mouse + network-to-author-origin to a malicious
package has authorized the exfiltration. The design goal is to make
that grant explicit, rare, and legible — not to pretend the sandbox can
prevent a data flow the user approved.

## 12. Ecosystem Goals

- Enable creation of:
  - animated wallpapers
  - ambient desktop scenes
  - screensavers
  - overlays/widgets
- Encourage reusable effect/node ecosystem
- Support both no-code and advanced developer workflows
- Enable community-driven repositories of effects and scenes

## 13. Milestones

### 13.1 Pre-code Design Work

To be nailed down on paper before runtime implementation begins:

- `scene.json` schema: nodes, connections, parameter bindings, asset references, user-tweakable parameters
- Graph type system and evaluation semantics: edge types (texture, buffer, scalar, vector, event), load-time validation, invalid-graph behavior, precise dirty/damage semantics for dependency-driven updates
- WASM host ABI: buffer memory layout, parameter identification, `update()` signature, version negotiation; capability ABI (§4.4) — storage calls (sync vs. worker-backed), HTTP request handles, and the defined denial/offline error codes; the §4.5 module-node interface JSON and I/O block layout. The embedded engine is decided: Wasmtime natively, browser-native instances in the web build (§4.5)
- Two or three hand-written example scenes against the draft schema, to validate the format before any code exists

### 13.2 V1 Scope

- Native runtime only, Wayland only (layer-shell compositors; GNOME out of scope)
- Scene loaded from a local `.sceneproject` directory (JSON; no `scene.bin`)
- Node types: image texture, video, shader effect, transform/layer (2D position/rotation/scale/anchor as wireable ports), compositor, time/mouse input sources, basic value nodes (remap, wave)
- Content model: a background layer (image or video) plus animated foreground layers with transparency; all texture edges carry premultiplied alpha
- Single output first; multi-monitor and hotplug as a fast follow within v1
- Frame-callback-driven rendering with idle pause (occlusion/lock/DPMS handled by compositor callback throttling)
- Presentation: bypass Dawn's swapchain — render into self-allocated dmabuf textures imported into Dawn, committed to the layer surface via `linux-dmabuf` (prior art: [overdraw](https://github.com/jhanssen/overdraw)); the same dmabuf import path serves zero-copy video decode later
- Dev modes, first-class from the start: `--windowed` (xdg-toplevel, develop without replacing the wallpaper) and `--headless` (offscreen render, dump frames as PNG — doubles as the golden-image test harness)
- Toolchain: C++23, CMake + Ninja, single-threaded core (one decode thread per video node); Dawn via tarball download; glaze (JSON), ffmpeg/VAAPI (video), stb_image + libktx (images), raw wayland-client with scanner-generated protocols
- Dependency policy: system-coupled C libraries (ffmpeg, libva, wayland-client, gbm, libdrm) come from the system via pkg-config — never vendored (ffmpeg is LGPL; dynamic system linking keeps the Apache-2.0 project clean); portable C++ dependencies (glaze, libktx) via CMake FetchContent pinned to exact release tags, each wrapped in a `3rdparty/<name>/CMakeLists.txt`; Dawn is the one prebuilt tarball; trivial single-headers (stb) vendored in `3rdparty/`

### 13.3 Explicitly Deferred (post-v1)

Deferred from implementation, but the scene format must account for them:

- Compute nodes and GPU simulation
- 3D meshes and mesh animation — added later as self-contained render-to-texture node types (geometry, camera, and animation internal to the node; output is an ordinary `texture` edge), requiring no scene-format changes
- WASM logic modules — since implemented on both targets (2026-07-10,
  §4.5): browser runtime/editor and the native Wasmtime embedding
- Browser runtime (WASM build) and browser-based editor
- Editor↔runtime live-sync protocol
- Node packages, `.wallpkg` packaging, and the distribution/repository system
- Audio input and audio-reactive scenes
- Power policy knobs (battery FPS cap, user-idle pause) beyond compositor-driven pause
