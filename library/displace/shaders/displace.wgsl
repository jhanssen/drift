// Offset the source lookup by a displacement map: the map's red/green
// channels are x/y displacement around 0.5, scaled by amount (uv units).
// The map is sampled at the undistorted uv, so it can be authored (or
// animated) over the source directly. Refraction, heat haze, frosted
// glass — feed it noise, ripples, or a scrolling pattern.
struct Params {
    amount: f32,
}
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var source: texture_2d<f32>;
@group(0) @binding(2) var source_sampler: sampler;
@group(0) @binding(3) var map: texture_2d<f32>;
@group(0) @binding(4) var map_sampler: sampler;

@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let d = textureSample(map, map_sampler, uv).rg - vec2f(0.5);
    return textureSample(source, source_sampler, uv + d * params.amount);
}
