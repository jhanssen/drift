@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let sky = mix(vec3f(0.002, 0.002, 0.008), vec3f(0.014, 0.010, 0.034),
                  smoothstep(1.0, 0.1, uv.y));
    // A sparse fixed starfield: hash the cell, keep the brightest few.
    // Integer hash: WGSL only specifies sin() accuracy near [-pi, pi], so the
    // classic fract(sin(...)*43758) trick is implementation-defined here; u32
    // math is bit-exact on every backend.
    let cell = floor(uv * vec2f(64.0, 36.0));
    var n = u32(cell.x) * 0x9E3779B9u + u32(cell.y) * 0x85EBCA6Bu;
    n = (n ^ (n >> 16u)) * 0x7FEB352Du;
    n = (n ^ (n >> 15u)) * 0x846CA68Bu;
    n = n ^ (n >> 16u);
    let h = f32(n >> 8u) * (1.0 / 16777216.0);
    let local = fract(uv * vec2f(64.0, 36.0)) - 0.5;
    let star = smoothstep(0.985, 1.0, h) *
               max(0.0, 1.0 - dot(local, local) * 12.0) *
               smoothstep(0.9, 0.3, uv.y);
    return vec4f(sky + vec3f(0.7, 0.8, 1.0) * star * 0.35, 1.0);
}
