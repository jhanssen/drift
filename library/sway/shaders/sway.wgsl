// Masked sway: offset the source lookup by an animated wave field,
// scaled per-pixel by the flex map's red channel (white moves, black
// pinned). The flex map is sampled at the rest position, so the mask is
// authored over the undistorted image.
struct Params {
    time: f32,
    speed: f32,
    amplitude: f32,
    scale: f32,
    direction: vec2f,
}
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var source: texture_2d<f32>;
@group(0) @binding(2) var source_sampler: sampler;
@group(0) @binding(3) var flex: texture_2d<f32>;
@group(0) @binding(4) var flex_sampler: sampler;

@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let t = params.time * params.speed;
    let p = uv * params.scale;
    // Three detuned octaves per axis: coherent gusts plus fine flutter.
    let dx = sin(p.y * 2.3 + t) * 0.6
           + sin(p.y * 5.1 - t * 1.4 + p.x * 2.0) * 0.3
           + sin(p.x * 7.7 + t * 2.3) * 0.1;
    let dy = cos(p.x * 2.1 + t * 0.8) * 0.5
           + sin((p.x + p.y) * 4.3 + t * 1.7) * 0.3;
    let f = textureSample(flex, flex_sampler, uv).r;
    let offset = vec2f(dx, dy) * params.direction * (params.amplitude * f);
    return textureSample(source, source_sampler, uv + offset);
}
