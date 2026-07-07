// Node-type metadata the graph view draws and creates from. Ports and
// declared properties both come from the runtime (drift_node_ports,
// drift_node_props) instead of editor hardcoding.

import { wasm } from './preview.js';

// Port tables come from the runtime too (drift_node_ports ← the loader's
// own PortDef tables in SceneLoader.cpp), so a port added engine-side
// appears here without an editor edit. Inputs convert to
// [name, category, isArray, default] and outputs to [name, category] in
// §9 declaration order (the first output is the default). Types whose
// ports are reflected or dynamic (shader/compute WGSL, §19.2 graph
// interfaces, sequence tracks) have no static entry — the editor resolves
// those from the document. Without a preview runtime the accessors return
// null and the graph degrades to bound-ports-only, like nodePropDefs.
let nodePortsCache = null;
function nodePorts() {
  if (!nodePortsCache && wasm) {
    try {
      const decls = JSON.parse(wasm.nodePorts());
      nodePortsCache = { inputs: {}, outputs: {}, implicit: {} };
      for (const [type, decl] of Object.entries(decls)) {
        if (decl.inputs) {
          nodePortsCache.inputs[type] =
              decl.inputs.map((p) => [p.name, p.type, !!p.array, p.default]);
        }
        if (decl.outputs) {
          (decl.implicit ? nodePortsCache.implicit
                         : nodePortsCache.outputs)[type] =
              decl.outputs.map((p) => [p.name, p.type]);
        }
      }
    } catch {
      nodePortsCache = null;
    }
  }
  return nodePortsCache;
}
export const nodeInputs = (type) => nodePorts()?.inputs[type] ?? null;
export const nodeOutputs = (type) => nodePorts()?.outputs[type] ?? null;
// Implicit singletons (§9.7–9.8): time and mouse.
export const implicitOutputs = (id) => nodePorts()?.implicit[id] ?? null;

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
