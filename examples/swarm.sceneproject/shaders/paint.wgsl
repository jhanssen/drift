// Paints the particle buffer into a storage texture (§18.2): per pixel,
// accumulate a gaussian glow from every live particle. O(pixels × N) — fine
// at mote counts; the §18.3 sprites node will be the instanced fast path.

struct Particle {
    pos: vec3f, age: f32,
    vel: vec3f, lifetime: f32,
    color: vec4f,
    size: f32, rotation: f32, seed: f32, reserved: f32,
}

@group(0) @binding(0) var<storage, read> pts: array<Particle>;
@group(0) @binding(1) var canvas: texture_storage_2d<rgba16float, write>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) id: vec3u) {
    let dims = textureDimensions(canvas);
    if (id.x >= dims.x || id.y >= dims.y) {
        return;
    }
    let uv = (vec2f(id.xy) + 0.5) / vec2f(dims);
    let aspect = f32(dims.x) / f32(dims.y);
    var rgb = vec3f(0.010, 0.012, 0.020);
    for (var i = 0u; i < arrayLength(&pts); i++) {
        let p = pts[i];
        if (p.lifetime == 0.0) {
            continue;
        }
        var d = uv - p.pos.xy;
        d.x *= aspect; // sizes are fractions of output height (§9.4 spirit)
        let r = max(p.size, 1e-4);
        rgb += p.color.rgb * exp(-dot(d, d) / (r * r));
    }
    textureStore(canvas, vec2i(id.xy), vec4f(rgb, 1.0));
}
