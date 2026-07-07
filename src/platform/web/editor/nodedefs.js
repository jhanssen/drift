// Node-type metadata the graph view draws and creates from. Ports and
// declared properties both come from the runtime (drift_node_ports,
// drift_node_props) instead of editor hardcoding.

import { wasm } from './preview.js';

// Port tables come from the runtime too (drift_node_ports ← the loader's
// own PortDef tables in SceneLoader.cpp), so a port added engine-side
// appears here without an editor edit. Inputs convert to
// [name, category, isArray, default, required] and outputs to
// [name, category], both in §9 declaration order (the first output is
// the default). Types whose
// ports are reflected or dynamic (shader/compute WGSL, §19.2 graph
// interfaces, sequence tracks) have no static entry — the editor resolves
// those from the document. Without a preview runtime the accessors return
// null and the graph degrades to bound-ports-only, like nodePropDefs.
let nodePortsCache = null;
function nodePorts() {
  if (!nodePortsCache && wasm) {
    // A failure sticks as empty tables: one loud console line instead of
    // re-marshalling the JSON out of the wasm heap on every accessor call.
    const cache = { inputs: {}, outputs: {}, implicit: {} };
    try {
      const decls = JSON.parse(wasm.nodePorts());
      for (const [type, decl] of Object.entries(decls)) {
        if (decl.inputs) {
          cache.inputs[type] = decl.inputs.map(
              (p) => [p.name, p.type, !!p.array, p.default, !!p.required]);
        }
        if (decl.outputs) {
          (decl.implicit ? cache.implicit : cache.outputs)[type] =
              decl.outputs.map((p) => [p.name, p.type]);
        }
      }
    } catch (err) {
      console.log(`editor: node ports: ${err}`);
    }
    nodePortsCache = cache;
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
export const TYPE_ARITY = { scalar: 1, vec2: 2, vec3: 3, vec4: 4 };

export const GRAPH_COLORS = { texture: '#6aa1ff', value: '#7fd490', event: '#e3c56d',
                       buffer: '#b48ade' };
export const G = { W: 170, HEAD: 24, ROW: 15, PAD: 7, COL: 250, GAP: 26, MARGIN: 40 };

// Node types the palette offers ('output' is exactly-one per scene, §9.6).
export const NODE_PALETTE = ['image', 'video', 'shader', 'compute', 'particles',
                      'sprites', 'trails', 'transform', 'fit', 'compositor',
                      'sequence', 'wave', 'noise', 'damp', 'remap', 'add',
                      'multiply', 'mix', 'clamp', 'edge', 'combine', 'split',
                      'parameter'];

// Creation-time input policy beyond mere validity: sources that should
// tick immediately wire to the clock, and a few defaults read better than
// zero. Everything else needed to keep a fresh node valid is derived from
// the port table below, so a required port added engine-side never leaves
// the palette creating drafts.
const NODE_DEFAULT_INPUTS = {
  particles: { time: '@time.delta' },
  wave: { input: '@time.seconds' },
  noise: { input: '@time.seconds' },
  damp: { time: '@time.delta' },
  sequence: { time: '@time.seconds' },
  mix: { b: 1 },
  split: { value: [0, 0] },
  combine: { x: 0, y: 0 },
};

// The inputs a freshly created node starts with: the policy table plus
// zero/empty fills for the remaining required value/array ports (§13).
// Required texture/event/buffer pins stay unwired — that is the expected
// add-then-wire flow.
export function defaultInputsFor(type) {
  const inputs = structuredClone(NODE_DEFAULT_INPUTS[type] ?? {});
  for (const [name, category, isArray, , required] of nodeInputs(type) ?? []) {
    if (!required || inputs[name] !== undefined) {
      continue;
    }
    if (isArray) {
      inputs[name] = [];
    } else if (category === 'value') {
      inputs[name] = 0;
    }
  }
  return Object.keys(inputs).length ? inputs : null;
}
