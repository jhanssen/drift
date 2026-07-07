// Node-type metadata the graph view draws and creates from. Output/input
// port tables mirror §9; declared properties come from the runtime
// (drift_node_props) instead of editor hardcoding.

import { wasm } from './preview.js';

// Output ports per built-in type as [name, category] in declaration order —
// the first is the default output (§9). Categories color the wires: texture,
// event, or plain value. sequence ports come from its tracks.
export const NODE_OUTPUTS = {
  image: [['result', 'texture']],
  video: [['result', 'texture'], ['finished', 'event']],
  shader: [['result', 'texture']],
  particles: [['result', 'buffer']],
  sprites: [['result', 'texture']],
  trails: [['result', 'texture']],
  transform: [['result', 'texture']],
  fit: [['result', 'texture']],
  compositor: [['result', 'texture']],
  output: [],
  remap: [['result', 'value']],
  wave: [['result', 'value']],
  add: [['result', 'value']],
  multiply: [['result', 'value']],
  mix: [['result', 'value']],
  clamp: [['result', 'value']],
  noise: [['result', 'value']],
  damp: [['result', 'value']],
  edge: [['result', 'event']],
  combine: [['result', 'value']],
  split: [['x', 'value'], ['y', 'value'], ['z', 'value'], ['w', 'value']],
};
export const IMPLICIT_OUTPUTS = {
  time: [['seconds', 'value'], ['delta', 'value']],
  mouse: [['position', 'value'], ['active', 'value']],
};
// Input ports per built-in type as [name, category, isArray, default] (§9),
// so ports that are not bound in the document still get pins to drop wires
// on, showing the default they run at. shader ports come from WGSL
// reflection instead (specInputsOf).
export const NODE_INPUTS = {
  image: [],
  video: [['playing', 'value', false, 1], ['restart', 'event']],
  particles: [['rate', 'value', false, 10], ['burst', 'event'],
              ['burstCount', 'value', false, 32],
              ['origin', 'value', false, [0.5, 0.5]],
              ['extent', 'value', false, [0, 0]],
              ['emitScale', 'value', false, [1, 1]],
              ['direction', 'value', false, [0, -1]],
              ['spread', 'value', false, 180],
              ['speed', 'value', false, [0.05, 0.15]],
              ['lifetime', 'value', false, [1, 3]],
              ['size', 'value', false, [0.01, 0.03]],
              ['spin', 'value', false, [0, 0]],
              ['colorStart', 'value', false, [1, 1, 1, 1]],
              ['colorEnd', 'value', false, [1, 1, 1, 0]],
              ['fadeIn', 'value', false, 0.1],
              ['fadeOut', 'value', false, 0],
              ['sizeEnd', 'value', false, 1],
              ['sizeWindow', 'value', false, [0, 1]],
              ['gravity', 'value', false, [0, 0]],
              ['drag', 'value', false, 0],
              ['turbulence', 'value', false, 0],
              ['turbulenceScale', 'value', false, 4],
              ['turbulenceMask', 'value', false, [1, 1]],
              ['attractor', 'value', false, [0.5, 0.5]],
              ['attract', 'value', false, 0],
              ['vortex', 'value', false, 0],
              ['delay', 'value', false, 0],
              ['duration', 'value', false, 0],
              ['ring', 'value', false, 0],
              ['depth', 'value', false, [0, 0]],
              ['collide', 'texture'],
              ['bounce', 'value', false, 0.5],
              ['tintVary', 'value', false, [1, 1, 1, 1]],
              ['tintVaryMax', 'value', false, [1, 1, 1, 1]],
              ['velocityMin', 'value', false, [0, 0]],
              ['velocityMax', 'value', false, [0, 0]],
              ['twinkle', 'value', false, [1, 1]],
              ['twinkleRate', 'value', false, [1, 2]],
              ['prewarm', 'value', false, 0],
              ['spawn', 'buffer'],
              ['inherit', 'value', false, 0],
              ['spawnRate', 'value', false, 0],
              ['time', 'value']],
  sprites: [['particles', 'buffer'], ['texture', 'texture'],
            ['frameRate', 'value', false, 0],
            ['parallax', 'value', false, [0, 0]],
            ['flutter', 'value', false, [0, 0]],
            ['flutterRate', 'value', false, 1],
            ['stretch', 'value', false, [1, 1]],
            ['frameBlend', 'value', false, 0],
            ['align', 'value', false, 0],
            ['hardness', 'value', false, 1]],
  trails: [['particles', 'buffer'],
           ['width', 'value', false, 1],
           ['taper', 'value', false, 0],
           ['fade', 'value', false, 0],
           ['parallax', 'value', false, [0, 0]],
           ['feather', 'value', false, 0]],
  transform: [['source', 'texture'],
              ['position', 'value', false, [0.5, 0.5]],
              ['rotation', 'value', false, 0],
              ['scale', 'value', false, [1, 1]],
              ['anchor', 'value', false, [0.5, 0.5]],
              ['opacity', 'value', false, 1]],
  fit: [['source', 'texture']],
  compositor: [['layers', 'texture', true]],
  output: [['color', 'texture']],
  remap: [['value', 'value'], ['inMin', 'value', false, 0],
          ['inMax', 'value', false, 1], ['outMin', 'value', false, 0],
          ['outMax', 'value', false, 1]],
  wave: [['input', 'value'], ['frequency', 'value', false, 1],
         ['phase', 'value', false, 0]],
  add: [['a', 'value'], ['b', 'value', false, 0]],
  multiply: [['a', 'value'], ['b', 'value', false, 1]],
  mix: [['a', 'value'], ['b', 'value'], ['t', 'value', false, 0.5]],
  clamp: [['value', 'value'], ['lo', 'value', false, 0],
          ['hi', 'value', false, 1]],
  noise: [['input', 'value'], ['frequency', 'value', false, 1],
          ['seed', 'value', false, 0]],
  damp: [['value', 'value'], ['time', 'value'],
         ['halflife', 'value', false, 0.2]],
  edge: [['value', 'value'], ['threshold', 'value', false, 0.5]],
  combine: [['x', 'value', false, 0], ['y', 'value', false, 0],
            ['z', 'value', false, 0], ['w', 'value', false, 0]],
  split: [['value', 'value']],
  sequence: [['time', 'value']],
};
// Declared properties per type (name/kind/required/default/options) come
// from the runtime (drift_node_props ← core/NodeProps.h): creation forms
// and the inspector's defaults follow the engine, not editor hardcoding.
let nodePropsCache = null;
export function nodePropDefs(type) {
  try {
    nodePropsCache ??= wasm ? JSON.parse(wasm.nodeProps()) : null;
    return nodePropsCache?.[type] ?? [];
  } catch {
    return [];
  }
}
export const GRAPH_COLORS = { texture: '#6aa1ff', value: '#7fd490', event: '#e3c56d',
                       buffer: '#b48ade' };
export const G = { W: 170, HEAD: 24, ROW: 15, PAD: 7, COL: 250, GAP: 26, MARGIN: 40 };

// Node types the palette offers ('output' is exactly-one per scene, §9.6).
// Defaults keep a fresh node valid where possible (wave/sequence tick along
// immediately); nodes with required texture ports (transform, output-like)
// stay a draft until wired — that is the expected add-then-wire flow.
export const NODE_PALETTE = ['image', 'video', 'shader', 'compute', 'particles',
                      'sprites', 'trails', 'transform', 'fit', 'compositor',
                      'sequence', 'wave', 'noise', 'damp', 'remap', 'add',
                      'multiply', 'mix', 'clamp', 'edge', 'combine', 'split',
                      'parameter'];
export const NODE_DEFAULT_INPUTS = {
  particles: { time: '@time.delta' },
  wave: { input: '@time.seconds' },
  noise: { input: '@time.seconds' },
  damp: { value: 0, time: '@time.delta' },
  sequence: { time: '@time.seconds' },
  remap: { value: 0 },
  add: { a: 0 },
  multiply: { a: 0 },
  mix: { a: 0, b: 1 },
  clamp: { value: 0 },
  edge: { value: 0 },
  combine: { x: 0, y: 0 },
  split: { value: [0, 0] },
  compositor: { layers: [] },
};
