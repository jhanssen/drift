// Rolling ground silhouette. Doubles as the §18.5.2 collision mask: solid
// where alpha >= 0.5, and the alpha gradient across the edge gives sparks
// their bounce normal.
@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let crest = 0.88 - 0.020 * sin(uv.x * 7.0) - 0.012 * sin(uv.x * 17.0 + 2.0);
    let a = smoothstep(crest - 0.008, crest + 0.008, uv.y);
    return vec4f(vec3f(0.016, 0.020, 0.028) * a, a);
}
