// Masked mix: where the mask's red channel is white the effected layer
// shows, where black the base does — how any effect gets limited to an
// area (paint the mask, or generate it with gradient/noise/anything).
// Premultiplied values mix linearly, so this is exact. amount scales the
// whole mask (0 disables the effect layer entirely).
struct Params {
    amount: f32,
}
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var base: texture_2d<f32>;
@group(0) @binding(2) var base_sampler: sampler;
@group(0) @binding(3) var over: texture_2d<f32>;
@group(0) @binding(4) var over_sampler: sampler;
@group(0) @binding(5) var mask: texture_2d<f32>;
@group(0) @binding(6) var mask_sampler: sampler;

@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let t = textureSample(mask, mask_sampler, uv).r * params.amount;
    return mix(textureSample(base, base_sampler, uv),
               textureSample(over, over_sampler, uv), t);
}
