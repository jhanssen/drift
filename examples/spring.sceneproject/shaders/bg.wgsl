@fragment fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let deep = vec3f(0.02, 0.03, 0.06);
    let lift = vec3f(0.05, 0.08, 0.14);
    let rgb = mix(deep, lift, 1.0 - uv.y);
    return vec4f(rgb, 1.0);
}
