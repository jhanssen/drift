@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let sky = mix(vec3f(0.002, 0.002, 0.008), vec3f(0.014, 0.010, 0.034),
                  smoothstep(1.0, 0.1, uv.y));
    // A sparse fixed starfield: hash the cell, keep the brightest few.
    let cell = floor(uv * vec2f(64.0, 36.0));
    let h = fract(sin(dot(cell, vec2f(127.1, 311.7))) * 43758.5453);
    let local = fract(uv * vec2f(64.0, 36.0)) - 0.5;
    let star = smoothstep(0.985, 1.0, h) *
               max(0.0, 1.0 - dot(local, local) * 12.0) *
               smoothstep(0.9, 0.3, uv.y);
    return vec4f(sky + vec3f(0.7, 0.8, 1.0) * star * 0.35, 1.0);
}
