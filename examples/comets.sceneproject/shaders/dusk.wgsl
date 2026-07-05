struct Params {
    glow: f32,
}
@group(0) @binding(0) var<uniform> params: Params;

@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let d = uv - vec2f(0.5, 0.45);
    let r = dot(d, d);
    let base = mix(vec3f(0.012, 0.008, 0.030), vec3f(0.001, 0.001, 0.006),
                   smoothstep(0.0, 0.45, r));
    let halo = vec3f(0.020, 0.010, 0.045) * params.glow *
               smoothstep(0.16, 0.02, abs(r - 0.055));
    return vec4f(base + halo, 1.0);
}
