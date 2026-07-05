@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let grad = mix(vec3f(0.030, 0.016, 0.026), vec3f(0.004, 0.004, 0.012),
                   smoothstep(0.0, 1.0, uv.y));
    let d = uv - vec2f(0.5, 0.5);
    let vignette = 1.0 - dot(d, d) * 0.7;
    return vec4f(grad * vignette, 1.0);
}
