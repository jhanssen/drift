# drift

A declarative, GPU-accelerated desktop scene runtime for animated wallpapers
and ambient visual experiences. See [docs/DESIGN.md](docs/DESIGN.md) and
[docs/SCENE_FORMAT.md](docs/SCENE_FORMAT.md).

Status: v1 feature-complete. Full node set — image (PNG/JPEG/WebP/KTX2,
including Basis-supercompressed KTX2 and stored mip chains), video
(ffmpeg software decode; AV1/H.264/anything your ffmpeg has), shader,
transform, compositor, wave, remap, combine, split, the implicit
time/mouse inputs, and previous-frame feedback edges (`"previous": true`)
for trails and iterative effects. Wallpaper mode covers every output with
runtime hotplug; the dirty-driven graph skips GPU work and commits when
nothing changed (a static scene idles at zero GPU cost, and scene time
freezes while occluded); presentation uses sync-fd fences, not CPU waits.
Wayland (wlr-layer-shell compositors) only. Next: hardware video decode
(VAAPI/NVDEC zero-copy).

## Build

Requires: CMake ≥ 3.24, a C++23 compiler, and dev packages for
`wayland-client`, `gbm`, `libdrm`, and ffmpeg (`libavformat`, `libavcodec`,
`libavutil`, `libswscale`). Dawn is downloaded as a prebuilt tarball on
first configure.

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
`DRIFT_ADAPTER=<substring>` selects a specific GPU adapter by device name.
`--set name=value` overrides scene parameters (repeatable). Headless runs
take `--mouse X,Y` to inject a fixed pointer position (scenes using
`@mouse` stay deterministic and golden-testable).

## Test

```
ctest --test-dir build
```

Two kinds of tests: doctest unit tests (`tests/unit/`, scene-loader
validation and WGSL interface reflection) and golden-image tests
(`tests/golden/`) that render example scenes headless and fuzzy-compare
against checked-in PNGs. Goldens render on the lavapipe software rasterizer
(`DRIFT_ADAPTER=llvmpipe`) for reproducibility; `tools/imgcmp.cpp`'s
tolerances absorb real-GPU least-significant-bit differences. To regenerate
after an intentional rendering change:

```
DRIFT_ADAPTER=llvmpipe build/drift examples/plasma.sceneproject \
    --frames 0,30,120 --size 320x180 --out tests/golden/plasma
```

## Layout

- `src/core/` — platform-agnostic runtime (portable WebGPU API only; this is
  the code that also compiles to WASM for the browser runtime later)
- `src/platform/linux/` — Dawn native device, GBM/dmabuf render targets,
  Wayland presentation
- `docs/` — design and format specs
