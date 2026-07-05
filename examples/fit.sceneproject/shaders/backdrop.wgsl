// Quiet vertical gradient behind the letterboxed card.
struct Params {
    level: f32,
}
@group(0) @binding(0) var<uniform> params: Params;

@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let c = mix(vec3f(0.06, 0.07, 0.10), vec3f(0.13, 0.11, 0.18), uv.y);
    return vec4f(c * params.level, 1.0);
}
