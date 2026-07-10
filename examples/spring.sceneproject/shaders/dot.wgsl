// A soft glowing disc on a transparent field, premultiplied (§12).
struct Params {
    warm: f32,
}
@group(0) @binding(0) var<uniform> params: Params;

@fragment fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let d = distance(uv, vec2f(0.5, 0.5)) * 2.0;
    let core = exp(-d * d * 18.0);
    let halo = exp(-d * d * 4.0) * 0.35;
    let a = clamp(core + halo, 0.0, 1.0);
    let cool = vec3f(0.45, 0.75, 1.0);
    let warm = vec3f(1.0, 0.7, 0.35);
    let rgb = mix(cool, warm, params.warm) * (core + halo);
    return vec4f(rgb, a);
}
