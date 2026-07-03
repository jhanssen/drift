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

## 4. Scripting / Logic System

### 4.1 WASM Modules

- Logic is implemented as WebAssembly modules
- No native plugins or dynamic libraries allowed
- Modules are sandboxed with no ambient authority: the host API is the only capability surface
- Modules are host-agnostic: the same module runs unmodified in the native and browser runtimes

### 4.2 Execution Model

Modules operate on:

- **Input buffer** (provided by host)
  - mouse position
  - audio analysis
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
- No direct rendering access
- No filesystem or network access
- Stable versioned interface

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

## 11. Security Model

- No native code execution
- No dynamic libraries
- WASM sandbox isolation
- Explicit host API surface
- No filesystem/network access for modules
- Optional signed packages for trust verification

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
- WASM host ABI: buffer memory layout, parameter identification, `update()` signature, version negotiation; choice of embedded WASM engine
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

### 13.3 Explicitly Deferred (post-v1)

Deferred from implementation, but the scene format must account for them:

- Compute nodes and GPU simulation
- 3D meshes and mesh animation — added later as self-contained render-to-texture node types (geometry, camera, and animation internal to the node; output is an ordinary `texture` edge), requiring no scene-format changes
- WASM logic modules
- Browser runtime (WASM build) and browser-based editor
- Editor↔runtime live-sync protocol
- Node packages, `.wallpkg` packaging, and the distribution/repository system
- Audio input and audio-reactive scenes
- Power policy knobs (battery FPS cap, user-idle pause) beyond compositor-driven pause
