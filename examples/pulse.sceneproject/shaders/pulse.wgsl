struct Params {
    level: f32,
    warm: f32,
    intensity: f32,
}
@group(0) @binding(0) var<uniform> params: Params;

@fragment fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let d = distance(uv, vec2f(0.5, 0.5));
    let glow = params.level * params.intensity * exp(-d * d * 14.0);
    let cool = vec3f(0.25, 0.5, 1.0);
    let warm = vec3f(1.0, 0.55, 0.25);
    let tint = mix(cool, warm, params.warm);
    let rgb = vec3f(0.02, 0.02, 0.05) + tint * glow;
    return vec4f(rgb, 1.0);
}
