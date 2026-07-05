// Color grade over premultiplied linear input: un-premultiply, apply
// brightness → contrast (pivot 0.5) → saturation (Rec.709 luma) → hue
// rotation (about the gray axis) → tint, then re-premultiply.
struct Params {
    brightness: f32,
    contrast: f32,
    saturation: f32,
    hue: f32,
    tint: vec4f,
}
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var source: texture_2d<f32>;
@group(0) @binding(2) var source_sampler: sampler;

@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let src = textureSample(source, source_sampler, uv);
    let a = max(src.a, 1e-5);
    var c = src.rgb / a;
    c = c * params.brightness;
    c = (c - vec3f(0.5)) * params.contrast + vec3f(0.5);
    let luma = dot(c, vec3f(0.2126, 0.7152, 0.0722));
    c = mix(vec3f(luma), c, params.saturation);
    let rad = params.hue * 0.017453292519943295;
    let k = vec3f(0.5773502691896258);
    c = c * cos(rad) + cross(k, c) * sin(rad) +
        k * dot(k, c) * (1.0 - cos(rad));
    c = c * params.tint.rgb;
    let outA = src.a * params.tint.a;
    return vec4f(max(c, vec3f(0.0)) * outA, outA);
}
