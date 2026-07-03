# drift

A declarative, GPU-accelerated desktop scene runtime for animated wallpapers
and ambient visual experiences. See [docs/DESIGN.md](docs/DESIGN.md) and
[docs/SCENE_FORMAT.md](docs/SCENE_FORMAT.md).

Status: early. Loads and renders `.sceneproject` scenes with a first node set
(shader, wave, remap, combine, split, time, output); image/video/transform/
compositor/mouse nodes are next. Wayland (wlr-layer-shell compositors) only.

## Build

Requires: CMake ≥ 3.24, a C++23 compiler, and dev packages for
`wayland-client`, `gbm`, and `libdrm`. Dawn is downloaded as a prebuilt
tarball on first configure.

```
cmake -B build -G Ninja
ninja -C build
```

## Run

```
build/drift examples/plasma.sceneproject              # wallpaper (layer-shell)
build/drift examples/plasma.sceneproject --windowed   # dev window
build/drift examples/plasma.sceneproject --headless 60 --out /tmp/frames
```

Without a scene argument, a builtin placeholder gradient is rendered.

## Layout

- `src/core/` — platform-agnostic runtime (portable WebGPU API only; this is
  the code that also compiles to WASM for the browser runtime later)
- `src/platform/linux/` — Dawn native device, GBM/dmabuf render targets,
  Wayland presentation
- `docs/` — design and format specs
