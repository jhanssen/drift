// 2x2 spritesheet (§18.5.5): a four-frame sparkle — the ray pair rotates
// 45° per frame, so looping through the sheet reads as a twinkle.
@fragment
fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let cell = vec2f(floor(uv.x * 2.0), floor(uv.y * 2.0));
    let frame = cell.y * 2.0 + cell.x;
    let p = (fract(uv * 2.0) - 0.5) * 2.0;
    let r2 = dot(p, p);
    let core = max(0.0, 1.0 - r2 * 3.5);
    let ang = atan2(p.y, p.x) + frame * 0.7853982;
    let rays = pow(abs(cos(ang * 2.0)), 12.0) * max(0.0, 1.0 - r2 * 1.2);
    let a = clamp(core * core + rays * 0.8, 0.0, 1.0);
    return vec4f(vec3f(a), a);
}
