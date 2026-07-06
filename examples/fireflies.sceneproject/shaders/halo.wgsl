// A deliberately over-soft radial halo (wide exponential alpha skirt),
// premultiplied. On its own it reads as a fuzzy blob — the sprites node's
// hardness input (§18.5.5) tightens it into a bright core at draw time.
@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let p = (uv - 0.5) * 2.0;
    let a = exp(-dot(p, p) * 3.0) * (1.0 - smoothstep(0.8, 1.0, length(p)));
    return vec4f(vec3f(a), a);
}
