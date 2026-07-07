// The "New project" starter: a sequence-driven glow with one parameter,
// ready to run and edit. A leaf module (no imports, no DOM) so Node
// tooling shares the editor's exact skeleton (tools/driftpkg.mjs init).

export const templateScene = (name) => JSON.stringify({
  version: 1,
  name,
  parameters: {
    tint: { type: 'vec3', default: [0.4, 0.6, 1.0], hint: 'color',
            label: 'Tint' },
  },
  nodes: [
    { id: 'seq', type: 'sequence', duration: 8, loop: true,
      tracks: [
        { name: 'level', kind: 'value', type: 'scalar',
          interpolate: 'smooth',
          keys: [{ t: 0, value: 0.2 }, { t: 4, value: 1 },
                 { t: 8, value: 0.2 }] },
      ],
      inputs: { time: '@time.seconds' } },
    { id: 'main', type: 'shader', shader: 'shaders/main.wgsl',
      inputs: { level: '@seq.level', tint: '$tint' } },
    { id: 'out', type: 'output', inputs: { color: '@main' } },
  ],
}, null, 2) + '\n';

export const TEMPLATE_WGSL = `struct Params {
    level: f32,
    tint: vec3<f32>,
}
@group(0) @binding(0) var<uniform> params: Params;

@fragment fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let d = distance(uv, vec2f(0.5, 0.5));
    let glow = params.level * exp(-d * d * 8.0);
    return vec4f(params.tint * glow, 1.0);
}
`;
