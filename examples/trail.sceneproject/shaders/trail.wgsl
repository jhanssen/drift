// Feedback accumulator: this frame's dot plus a decayed copy of our own
// previous output (SCENE_FORMAT.md §10). The history port is wired with
// { "node": "trail", "previous": true } in scene.json.

struct Params {
    decay: f32,
}
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var current: texture_2d<f32>;
@group(0) @binding(2) var current_sampler: sampler;
@group(0) @binding(3) var history: texture_2d<f32>;
@group(0) @binding(4) var history_sampler: sampler;

@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let now = textureSample(current, current_sampler, uv);
    let past = textureSample(history, history_sampler, uv) * params.decay;
    // Premultiplied source-over keeps the trail below the fresh dot.
    return now + past * (1.0 - now.a);
}
