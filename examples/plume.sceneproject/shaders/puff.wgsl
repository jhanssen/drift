// 4x4 spritesheet (§18.5.5): a sixteen-frame smoke puff — the lobe field
// rotates 22.5° and the puff swells each frame, so adjacent frames differ
// enough that nearest-frame playback visibly steps; frameBlend cross-fades
// the sequence smooth.
@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let cell = vec2f(floor(uv.x * 4.0), floor(uv.y * 4.0));
    let frame = cell.y * 4.0 + cell.x;
    var p = (fract(uv * 4.0) - 0.5) * 2.0;
    let ang = frame * 0.3926991;
    let c = cos(ang);
    let s = sin(ang);
    p = vec2f(c * p.x - s * p.y, s * p.x + c * p.y);
    let r2 = dot(p, p);
    let lobes = 0.22 * sin(atan2(p.y, p.x) * 3.0 + frame * 0.9);
    let rad = 0.5 + 0.02 * frame + lobes;
    let a = clamp((rad * rad - r2) * 2.4, 0.0, 1.0) *
            (1.0 - frame / 18.0);
    return vec4f(vec3f(a), a);
}
