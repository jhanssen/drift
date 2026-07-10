// drift logic module (DESIGN.md §4.5): a damped 2D spring chasing
// `target` — the pure CPU transform case: values in, values out, no GPU.
//
// The I/O block layout follows spring.json with ports ordered
// lexicographically (§4.5): 32-byte header, then inputs damping,
// stiffness, target.xy, tick, then output position.xy — 60 bytes total.
// drift_init receives that size and must return a block for it; the
// static array below is sized to match, so an interface edit that changes
// the layout fails the handshake instead of scribbling.
//
// Rebuild (freestanding wasm32 — no libc, no emscripten glue):
//   clang --target=wasm32 -O2 -nostdlib -Wl,--no-entry -o spring.wasm spring.c

typedef unsigned int u32;

static unsigned char io[60];
static float px, py, vx, vy;

__attribute__((export_name("drift_abi"))) u32 drift_abi(void)
{
    return 1;
}

__attribute__((export_name("drift_init"))) void* drift_init(u32 size)
{
    return size == sizeof(io) ? io : (void*)0;
}

__attribute__((export_name("drift_update"))) void drift_update(void)
{
    const float* f = (const float*)io;
    const u32* hdr = (const u32*)io;
    float* out = (float*)io;

    const float damping = f[8];
    const float stiffness = f[9];
    const float tx = f[10], ty = f[11];
    float dt = f[1]; // header dt: seconds since our last update

    if (hdr[3] & 1u) { // first update after (re)load: snap to the target
        px = tx;
        py = ty;
        vx = vy = 0.0f;
    }
    if (dt > 0.05f) { // a scheduling hitch must not slingshot the spring
        dt = 0.05f;
    }
    vx += (stiffness * (tx - px) - damping * vx) * dt;
    vy += (stiffness * (ty - py) - damping * vy) * dt;
    px += vx * dt;
    py += vy * dt;

    out[13] = px;
    out[14] = py;
}
