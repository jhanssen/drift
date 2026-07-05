// Base plus the blurred halo, additive, halo scaled by strength.
struct Params {
    strength: f32,
}
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var base: texture_2d<f32>;
@group(0) @binding(2) var base_sampler: sampler;
@group(0) @binding(3) var halo: texture_2d<f32>;
@group(0) @binding(4) var halo_sampler: sampler;

@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    return textureSample(base, base_sampler, uv) +
           textureSample(halo, halo_sampler, uv) * params.strength;
}
