// Cheap glow: brighten toward the source's own blurred-ish neighborhood.
// (SCENE_FORMAT.md §14.2 sketches this contract.)

struct Params {
    strength: f32,
}
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var source: texture_2d<f32>;
@group(0) @binding(2) var source_sampler: sampler;

@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let base = textureSample(source, source_sampler, uv);
    var halo = vec4f(0.0);
    let r = 0.004;
    halo += textureSample(source, source_sampler, uv + vec2f(r, 0.0));
    halo += textureSample(source, source_sampler, uv - vec2f(r, 0.0));
    halo += textureSample(source, source_sampler, uv + vec2f(0.0, r));
    halo += textureSample(source, source_sampler, uv - vec2f(0.0, r));
    return base + halo * 0.25 * params.strength;
}
