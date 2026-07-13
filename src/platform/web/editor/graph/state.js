// Graph-view state shared by the build/wiring/interaction/drawing modules,
// plus the small helpers that read it from several of them. Mutable
// bindings are written through setters (imported bindings are read-only).

import { wasm } from '../preview.js';
import { sceneSource } from '../document.js';
import { G } from '../nodedefs.js';
import { graphCanvas } from './canvas.js';

// Drill-in navigation state (§19.5).
export const viewStack = []; // { path, doc, viaInstance }; mutated in place
export let selectedIds = new Set(); // multi-selection; selectedNodeId is primary
export let graph = null; // { nodes, byId, edges } rebuilt from sceneSource
export let selectedNodeId = null;
export let graphNeedsFit = true;

export function setGraph(value) { graph = value; }
export function setSelectedNodeId(value) { selectedNodeId = value; }
export function setSelectedIds(value) { selectedIds = value; }
export function setGraphNeedsFit(value) { graphNeedsFit = value; }

export const graphView = { x: 0, y: 0, scale: 1 }; // screen = (world + x/y) · scale
export let graphDrag = null; // { kind: 'node'|'pan', ... }
export let wireDrag = null;  // { seek: 'in'|'out', from?, to?, detach?, cursor }

export const graphWorldOf = (clientX, clientY) => {
  const rect = graphCanvas.getBoundingClientRect();
  return { x: (clientX - rect.left) / graphView.scale - graphView.x,
           y: (clientY - rect.top) / graphView.scale - graphView.y };
};

export function setGraphDrag(value) { graphDrag = value; }
export function setWireDrag(value) { wireDrag = value; }

// Drill-in navigation (§19.5): the canvas edits the top of the stack (the
// state at the top of this module); an empty stack is the scene document
// itself.
export const activeDoc = () =>
    viewStack.length ? viewStack[viewStack.length - 1].doc : sceneSource;
export const inSubgraph = () => viewStack.length > 0;

// The node-preview pane registers here so chrome updates can drive it
// without a module cycle (nodepreview.js imports this module).
let syncAuxPane = () => {};
export function setSyncAuxPane(fn) { syncAuxPane = fn; }

export function updateGraphChrome() {
  const showGraph = document.body.classList.contains('graph-mode');
  const view = viewStack[viewStack.length - 1];
  const inPreview = view?.kind === 'preview';
  document.getElementById('backBtn').hidden = !showGraph || !view;
  const crumb = document.getElementById('crumb');
  crumb.hidden = !showGraph || !view;
  crumb.textContent =
      view ? 'scene' + viewStack.map((v) =>
                 ' ▸ ' + (v.kind === 'preview' ? '👁 ' + v.nodeId : v.path))
                 .join('') +
             (!inPreview && viewReadOnly() ? ' (read-only)' : '') : '';
  document.getElementById('flattenBtn').hidden = !showGraph ||
      viewStack.length !== 1 || !view?.viaInstance || inPreview;
  document.getElementById('makeSubBtn').hidden =
      !showGraph || !!view || selectedIds.size < 2 || !wasm;
  if (inPreview) {
    // The pane covers the canvas; node edits have no visible target.
    document.getElementById('addNodeBtn').hidden = true;
    document.getElementById('graphHint').hidden = true;
  }
  syncAuxPane();
}

// §20.6: package graphs come from the store, not the project — the
// drill-in view shows them but every edit path is refused.
export const viewReadOnly = () =>
    viewStack.length > 0 &&
    viewStack[viewStack.length - 1].path.startsWith('packages/');

// ---- pin geometry (shared by drawing and hit tests) ------------------------

export const pinY = (node, index) => node.y + G.HEAD + index * G.ROW + G.ROW / 2;
export const inputPinPos = (node, name) => ({
  x: node.x,
  y: pinY(node, Math.max(node.inputs.findIndex((p) => p.name === name), 0)),
});
export const outputPinPos = (node, name) => ({
  x: node.x + node.w,
  y: pinY(node, Math.max(node.outputs.findIndex(([n]) => n === name), 0)),
});

export function pinAt(wx, wy) {
  const r = 8 / graphView.scale;
  const near = (px, py) => Math.hypot(wx - px, wy - py) < r;
  for (const node of [...(graph?.nodes ?? [])].reverse()) {
    for (let i = 0; i < node.inputs.length; i++) {
      if (near(node.x, pinY(node, i))) {
        return { node, side: 'in', input: node.inputs[i] };
      }
    }
    for (let i = 0; i < node.outputs.length; i++) {
      if (near(node.x + node.w, pinY(node, i))) {
        return { node, side: 'out', output: node.outputs[i] };
      }
    }
  }
  return null;
}

export let graphStatusText = null;   // mirrored as an in-canvas banner
export let graphDraftIds = new Set(); // nodes named in the error get a warn border

export function setGraphStatus(text) {
  graphStatusText = text ?? null;
  graphDraftIds = new Set(
      [...(text ?? '').matchAll(/node '([A-Za-z_][A-Za-z0-9_]*)'/g)]
          .map((m) => m[1]));
  document.body.classList.toggle('draft', !!text?.startsWith('draft'));
  const el = document.getElementById('graphStatus');
  el.textContent = text ?? '';
  el.className = text ? 'warn' : '';
  el.title = text ?? '';
}
