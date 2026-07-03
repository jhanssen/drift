// Classic plasma. phase cycles 0..1 (one full loop); scale sets pattern size.

struct Params {
    phase: f32,
    scale: f32,
}
@group(0) @binding(0) var<uniform> params: Params;

const TAU: f32 = 6.28318530718;

@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let p = uv * params.scale;
    let t = params.phase * TAU;

    var v = sin(p.x * 2.0 + t * 3.0);
    v += sin((p.y + t) * 2.0);
    v += sin((p.x + p.y + t * 2.0) * 1.5);
    let cx = p.x + 0.5 * sin(t);
    let cy = p.y + 0.5 * cos(t * 2.0);
    v += sin(sqrt(cx * cx + cy * cy + 1.0) * 3.0 + t * 3.0);
    v *= 0.25;

    let col = vec3f(0.5 + 0.5 * sin(v * TAU * 0.5),
                    0.5 + 0.5 * sin(v * TAU * 0.5 + 2.094),
                    0.5 + 0.5 * sin(v * TAU * 0.5 + 4.188));
    // dim it: this is a wallpaper, not a rave
    return vec4f(col * col * 0.55, 1.0);
}
