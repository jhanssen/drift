// One axis of a separable gaussian: 13 taps spread over ±3σ along dir.
// radius is σ in texels of the source. Premultiplied values blur
// directly. Run twice — dir [1,0] then [0,1] — for the full blur.
struct Params {
    radius: f32,
    dir: vec2f,
}
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var source: texture_2d<f32>;
@group(0) @binding(2) var source_sampler: sampler;

@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let texel = params.dir / vec2f(textureDimensions(source));
    let sigma = max(params.radius, 0.01);
    var sum = vec4f(0.0);
    var weight = 0.0;
    for (var i = -6; i <= 6; i++) {
        let x = f32(i) * sigma / 2.0;
        let w = exp(-x * x / (2.0 * sigma * sigma));
        sum += textureSample(source, source_sampler, uv + texel * x) * w;
        weight += w;
    }
    return sum / weight;
}
