// Orbiting motes: incremental integration over a ping-pong buffer (§18.2).
// Dead slots (lifetime == 0 — which is what a zeroed first-frame history
// reads as, §18.1) seed themselves deterministically from their index, so
// the simulation is exact under fixed-step rendering.

struct Particle {
    pos: vec3f, age: f32,
    vel: vec3f, lifetime: f32,
    color: vec4f,
    size: f32, rotation: f32, seed: f32, reserved: f32,
}

struct Params { dt: f32 }
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> prev: array<Particle>;
@group(0) @binding(2) var<storage, read_write> state: array<Particle>;

fn hash(n: u32) -> f32 {
    var x = n * 747796405u + 2891336453u;
    x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    return f32((x >> 22u) ^ x) / 4294967296.0;
}

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) id: vec3u) {
    let i = id.x;
    if (i >= arrayLength(&state)) {
        return;
    }
    var p = prev[i];
    if (p.lifetime == 0.0) {
        let angle = hash(i * 2u) * 6.2831853;
        let radius = 0.12 + 0.3 * hash(i * 2u + 1u);
        p.pos = vec3f(0.5 + radius * cos(angle), 0.5 + radius * sin(angle), 0.0);
        p.vel = vec3f(-sin(angle), cos(angle), 0.0) * (0.55 * radius);
        p.lifetime = 1.0;
        p.size = 0.02 + 0.03 * hash(i * 13u);
        p.color = vec4f(0.35 + 0.65 * hash(i * 3u), 0.55,
                        1.0 - 0.6 * hash(i * 5u), 1.0);
        p.seed = hash(i);
    } else {
        let d = p.pos.xy - vec2f(0.5, 0.5);
        let len = sqrt(max(dot(d, d), 1e-6));
        let pull = -(d / len) * 0.16;
        p.vel += vec3f(pull, 0.0) * params.dt;
        p.pos += p.vel * params.dt;
        p.age += params.dt;
    }
    state[i] = p;
}
