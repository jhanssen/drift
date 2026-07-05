// Linear two-color gradient across the output. angle in degrees,
// clockwise: 0 = left→right, 90 = top→bottom. Colors are straight
// alpha; the output is premultiplied (§12).
struct Params {
    colorA: vec4f,
    colorB: vec4f,
    angle: f32,
}
@group(0) @binding(0) var<uniform> params: Params;

@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let rad = params.angle * 0.017453292519943295;
    let dir = vec2f(cos(rad), sin(rad));
    let t = clamp(dot(uv - vec2f(0.5), dir) + 0.5, 0.0, 1.0);
    let c = mix(params.colorA, params.colorB, t);
    return vec4f(c.rgb * c.a, c.a);
}
