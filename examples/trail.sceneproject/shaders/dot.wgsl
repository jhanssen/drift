// A soft glowing dot at pos (uv space), premultiplied alpha.

struct Params {
    pos: vec2f,
}
@group(0) @binding(0) var<uniform> params: Params;

@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let d = distance(uv, params.pos);
    let a = smoothstep(0.045, 0.015, d);
    return vec4f(vec3f(1.0, 0.75, 0.35) * a, a);
}
