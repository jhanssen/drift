// Keep what's brighter than the threshold, scaled by how far past it
// the pixel's luma is — a soft knee, premultiplied-safe.
struct Params {
    threshold: f32,
}
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var source: texture_2d<f32>;
@group(0) @binding(2) var source_sampler: sampler;

@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let src = textureSample(source, source_sampler, uv);
    let luma = dot(src.rgb, vec3f(0.2126, 0.7152, 0.0722));
    let keep = max(luma - params.threshold, 0.0) / max(luma, 1e-5);
    return src * keep;
}
