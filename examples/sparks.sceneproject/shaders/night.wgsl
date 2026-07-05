struct Params {
    shift: f32,
}
@group(0) @binding(0) var<uniform> params: Params;

@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let horizon = smoothstep(1.0, 0.35, uv.y);
    let base = mix(vec3f(0.030, 0.014, 0.010), vec3f(0.004, 0.005, 0.012),
                   horizon);
    let glowX = 1.0 - abs(uv.x - 0.5) * (1.6 - params.shift);
    let glow = vec3f(0.05, 0.018, 0.006) * max(0.0, glowX) *
               smoothstep(0.55, 1.0, uv.y);
    return vec4f(base + glow, 1.0);
}
