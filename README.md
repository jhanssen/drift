# drift

A declarative, GPU-accelerated desktop scene runtime for animated wallpapers
and ambient visual experiences. See [docs/DESIGN.md](docs/DESIGN.md) and
[docs/SCENE_FORMAT.md](docs/SCENE_FORMAT.md).

Status: scaffold — renders an animated placeholder gradient. Wayland
(wlr-layer-shell compositors) only.

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
build/drift                    # wallpaper (layer-shell background surface)
build/drift --windowed         # dev window
build/drift --headless 60 --out /tmp/frames   # offscreen, writes PNGs
```

## Layout

- `src/core/` — platform-agnostic runtime (portable WebGPU API only; this is
  the code that also compiles to WASM for the browser runtime later)
- `src/platform/linux/` — Dawn native device, GBM/dmabuf render targets,
  Wayland presentation
- `docs/` — design and format specs
