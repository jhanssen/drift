// ---- graph view: pan/zoom, node dragging, add/delete, asset drag-drop ------

import { wasm, Module } from '../preview.js';
import { openQuickForm } from '../ui.js';
import { sceneSource, pushFromGraph } from '../document.js';
import { project, projectRoot, projectWriteThrough } from '../project.js';
import { G, NODE_PALETTE, NODE_DEFAULT_INPUTS,
         nodePropDefs } from '../nodedefs.js';
import { graph, graphView, graphDrag, setGraphDrag, wireDrag, setWireDrag,
         pinPress, setPinPress, selectedIds, setSelectedIds, selectedNodeId,
         setSelectedNodeId, activeDoc, inSubgraph, graphWorldOf, pinAt,
         setGraphStatus, updateGraphChrome } from './state.js';
import { parseConnection } from './model.js';
import { startWireDrag, applyWireDrop, openPinEditor,
         unbindPort } from './wiring.js';
import { enterSubgraph } from './subgraphs.js';
import { renderInspector } from './inspector.js';

const graphCanvas = document.getElementById('graphCanvas');

export function fitGraphView() {
  const cw = graphCanvas.clientWidth, ch = graphCanvas.clientHeight;
  if (!graph?.nodes.length || !cw || !ch) {
    return;
  }
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
  for (const n of graph.nodes) {
    x0 = Math.min(x0, n.x);
    y0 = Math.min(y0, n.y);
    x1 = Math.max(x1, n.x + n.w);
    y1 = Math.max(y1, n.y + n.h);
  }
  x0 -= 24; y0 -= 24; x1 += 24; y1 += 24;
  graphView.scale = Math.min(cw / (x1 - x0), ch / (y1 - y0), 1.2);
  graphView.x = -x0 + (cw / graphView.scale - (x1 - x0)) / 2;
  graphView.y = -y0 + (ch / graphView.scale - (y1 - y0)) / 2;
}

graphCanvas.onmousedown = (e) => {
  const { x: wx, y: wy } = graphWorldOf(e.clientX, e.clientY);
  const pin = pinAt(wx, wy);
  if (pin) {
    setPinPress(pin.side === 'in' ? { pin, cx: e.clientX, cy: e.clientY }
                                  : null);
    setWireDrag(startWireDrag(pin));
    if (wireDrag) {
      wireDrag.cursor = { x: wx, y: wy };
      e.preventDefault();
      return;
    }
  }
  const hit = [...(graph?.nodes ?? [])].reverse().find((n) =>
      wx >= n.x && wx <= n.x + n.w && wy >= n.y && wy <= n.y + n.h);
  if (hit) {
    if (e.shiftKey) {
      // Shift toggles membership in the multi-selection (§19.5's
      // make-subgraph source).
      if (selectedIds.has(hit.id)) {
        selectedIds.delete(hit.id);
        if (selectedNodeId === hit.id) {
          setSelectedNodeId([...selectedIds].pop() ?? null);
        }
      } else {
        selectedIds.add(hit.id);
        setSelectedNodeId(hit.id);
      }
    } else {
      if (!selectedIds.has(hit.id)) {
        setSelectedIds(new Set([hit.id])); // clicking a member keeps the group
      }
      setSelectedNodeId(hit.id);
    }
    renderInspector();
    updateGraphChrome();
    // Dragging any selected node moves the whole selection.
    const group = selectedIds.has(hit.id)
        ? [...selectedIds].map((id) => graph.byId.get(id)).filter(Boolean)
        : [hit];
    setGraphDrag({ kind: 'node', node: hit, nodes: group,
                   lastX: e.clientX, lastY: e.clientY, moved: false });
  } else {
    setGraphDrag({ kind: 'pan', lastX: e.clientX, lastY: e.clientY,
                   moved: false });
  }
  e.preventDefault();
};

window.addEventListener('mousemove', (e) => {
  if (wireDrag) {
    wireDrag.cursor = graphWorldOf(e.clientX, e.clientY);
    return;
  }
  if (!graphDrag) {
    return;
  }
  const dx = (e.clientX - graphDrag.lastX) / graphView.scale;
  const dy = (e.clientY - graphDrag.lastY) / graphView.scale;
  graphDrag.lastX = e.clientX;
  graphDrag.lastY = e.clientY;
  if (graphDrag.kind === 'node') {
    for (const node of graphDrag.nodes ?? [graphDrag.node]) {
      node.x += dx;
      node.y += dy;
    }
  } else {
    graphView.x += dx;
    graphView.y += dy;
  }
  graphDrag.moved = true;
});

window.addEventListener('mouseup', (e) => {
  if (pinPress) {
    const press = pinPress;
    setPinPress(null);
    if (wireDrag &&
        Math.hypot(e.clientX - press.cx, e.clientY - press.cy) < 4) {
      // A click, not a drag: edit the pin's value in place.
      setWireDrag(null);
      openPinEditor(press.pin, e);
      return;
    }
  }
  if (wireDrag) {
    const w = graphWorldOf(e.clientX, e.clientY);
    applyWireDrop(wireDrag, pinAt(w.x, w.y));
    setWireDrag(null);
    return;
  }
  if (!graphDrag) {
    return;
  }
  if (graphDrag.kind === 'node' && graphDrag.moved && activeDoc()) {
    // Dropped positions → the §2.1 editor block (of whichever document is
    // in view). Editor-only metadata, so the document is not pushed;
    // Save… and the next real edit carry it.
    for (const node of graphDrag.nodes ?? [graphDrag.node]) {
      node.x = Math.round(node.x);
      node.y = Math.round(node.y);
      ((activeDoc().editor ??= {}).positions ??= {})[node.id] =
          [node.x, node.y];
    }
  } else if (graphDrag.kind === 'pan' && !graphDrag.moved) {
    setSelectedNodeId(null); // background click clears the selection
    selectedIds.clear();
    renderInspector();
    updateGraphChrome();
  }
  setGraphDrag(null);
});

export function freeNodeId(type) {
  const taken = new Set(activeDoc().nodes.map((n) => n.id));
  for (let i = 1; ; i++) {
    const id = i === 1 ? type : `${type}${i}`;
    if (!taken.has(id)) {
      return id;
    }
  }
}

function placeAtViewCenter(key) {
  const cx = graphCanvas.clientWidth / 2 / graphView.scale - graphView.x;
  const cy = graphCanvas.clientHeight / 2 / graphView.scale - graphView.y;
  ((activeDoc().editor ??= {}).positions ??= {})[key] =
      [Math.round(cx - G.W / 2), Math.round(cy - 30)];
}

// Parameters are declarations (§6), not nodes, but they are created and
// placed from the same palette; the graph draws them as source nodes.
// Parse "x, y…" against a declared arity; a single number splats.
export function parseParamValue(text, arity) {
  const parts = text.split(',').map((s) => Number(s.trim()));
  if (!parts.length || parts.some(Number.isNaN) ||
      (parts.length !== 1 && parts.length !== arity)) {
    return undefined;
  }
  return arity === 1 ? parts[0]
       : parts.length === 1 ? Array(arity).fill(parts[0]) : parts;
}

function addParameter(anchor) {
  openQuickForm(anchor, 'add parameter', [
    { key: 'name', label: 'name', value: '' },
    { key: 'type', label: 'type',
      options: ['scalar', 'vec2', 'vec3', 'vec4'] },
    { key: 'default', label: 'default', value: '0' },
    { key: 'min', label: 'min', value: '0' },
    { key: 'max', label: 'max', value: '1' },
    { note: 'min/max bound the panel control (scalars); vector defaults ' +
            'are "x, y…"' },
  ], (v) => {
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(v.name)) {
      return 'name must be an identifier (§3)';
    }
    if (sceneSource.parameters?.[v.name]) {
      return `parameter '${v.name}' already exists`;
    }
    const arity = { scalar: 1, vec2: 2, vec3: 3, vec4: 4 }[v.type];
    const def = parseParamValue(v.default, arity);
    if (def === undefined) {
      return `default: expected ${arity} number(s)`;
    }
    const decl = { type: v.type, default: def };
    if (arity === 1) {
      const min = Number(v.min), max = Number(v.max);
      if (Number.isNaN(min) || Number.isNaN(max) || !(max > min)) {
        return 'min/max must be numbers with max > min';
      }
      decl.min = min;
      decl.max = max;
    }
    (sceneSource.parameters ??= {})[v.name] = decl;
    placeAtViewCenter('$' + v.name);
    setSelectedNodeId('$' + v.name);
    pushFromGraph();
    return null;
  });
}

function addNodeOfType(type, anchor, graphPath) {
  const doc = activeDoc();
  if (!doc?.nodes) {
    return;
  }
  anchor ??= document.getElementById('addNodeBtn').getBoundingClientRect();
  if (type === 'parameter') {
    addParameter(anchor);
    return;
  }
  // Creation parameters come from the runtime's property metadata:
  // structured ("json") kinds are edited by richer tooling, and a graph
  // instance's file is already chosen in the palette.
  const defs = nodePropDefs(type).filter((p) =>
      p.kind !== 'json' && !(graphPath && p.name === 'graph'));
  const fields = defs.map((p) => {
    if (p.options) {
      return { key: p.name, label: p.name, options: p.options,
               value: p.default };
    }
    if (p.kind === 'bool') {
      return { key: p.name, label: p.name, options: ['true', 'false'],
               value: String(p.default ?? true) };
    }
    const value = p.kind === 'asset'
        ? (p.name === 'shader' ? 'shaders/'
                               : p.name === 'src' ? 'assets/' : '')
        : p.default !== undefined ? String(p.default)
        : type === 'sequence' && p.name === 'duration' ? '8' : '';
    return { key: p.name, label: p.name, value };
  });
  const baseId = graphPath
      ? graphPath.split('/').pop().replace(/\.json$/, '')
            .replace(/[^A-Za-z0-9_]/g, '_')
      : type;
  fields.push({ key: 'id', label: 'id', value: freeNodeId(baseId) });
  openQuickForm(anchor, `add ${graphPath ?? type}`, fields, (v) => {
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(v.id) || v.id === 'time' ||
        v.id === 'mouse' || doc.nodes.some((n) => n.id === v.id)) {
      return 'invalid, reserved, or duplicate id (§3)';
    }
    const node = { id: v.id, type };
    if (graphPath) {
      node.graph = graphPath; // §19.3 instance
    }
    for (const p of defs) {
      const text = (v[p.name] ?? '').trim();
      if (!text) {
        if (p.required) {
          return `'${p.name}' is required`;
        }
        continue;
      }
      let value = text;
      if (p.kind === 'bool') {
        value = text === 'true';
      } else if (p.kind === 'int') {
        value = parseInt(text, 10);
        if (!(value > 0) || String(value) !== text) {
          return `'${p.name}' must be a positive integer`;
        }
      } else if (p.kind === 'number') {
        value = parseFloat(text);
        if (!Number.isFinite(value)) {
          return `'${p.name}' must be a number`;
        }
      }
      if (p.default !== undefined && value === p.default) {
        continue; // stays on the default; keep the document minimal
      }
      node[p.name] = value;
    }
    if (type === 'sequence') {
      node.tracks = [{ name: 'level', kind: 'value', type: 'scalar',
                       interpolate: 'linear', keys: [{ t: 0, value: 0 }] }];
    }
    if (NODE_DEFAULT_INPUTS[type]) {
      node.inputs = structuredClone(NODE_DEFAULT_INPUTS[type]);
    }
    doc.nodes.push(node);
    placeAtViewCenter(v.id);
    setSelectedNodeId(v.id);
    pushFromGraph();
    return null;
  });
}

function removeNodeFromDoc(doc, id) {
  const refNode = (value) => parseConnection(value)?.node ?? null;
  doc.nodes = doc.nodes.filter((n) => n.id !== id);
  for (const other of doc.nodes) {
    for (const [name, value] of Object.entries(other.inputs ?? {})) {
      if (Array.isArray(value)) {
        const kept = value.filter((el) => refNode(el) !== id);
        if (kept.length !== value.length) {
          other.inputs[name] = kept;
        }
      } else if (refNode(value) === id) {
        unbindPort(other.id, other.inputs, name);
      }
    }
  }
  delete doc.editor?.positions?.[id];
}

export function deleteSelectedNode() {
  const doc = activeDoc();
  const targets = (selectedIds.size > 1 ? [...selectedIds] : [selectedNodeId])
      .map((id) => graph?.byId.get(id))
      .filter((n) => n && !n.implicit && !n.boundary &&
                     !(n.param && selectedIds.size > 1));
  if (!targets.length || !doc) {
    return;
  }
  const what = targets.length > 1
      ? `${targets.length} nodes`
      : `${targets[0].param ? 'parameter' : 'node'} '${targets[0].id}'`;
  if (!confirm(`delete ${what}? wires to it are removed; a required port ` +
               'left unbound keeps the scene a draft until rewired')) {
    return;
  }
  for (const node of targets) {
    if (node.param) {
      delete doc.parameters[node.param];
      const ref = '$' + node.param;
      for (const other of doc.nodes) {
        for (const [name, value] of Object.entries(other.inputs ?? {})) {
          if (Array.isArray(value)) {
            const kept = value.filter((el) => el !== ref);
            if (kept.length !== value.length) {
              other.inputs[name] = kept;
            }
          } else if (value === ref) {
            unbindPort(other.id, other.inputs, name);
          }
        }
      }
      delete doc.editor?.positions?.[node.id];
      continue;
    }
    removeNodeFromDoc(doc, node.id);
  }
  setSelectedNodeId(null);
  selectedIds.clear();
  pushFromGraph();
}

const nodeMenu = document.getElementById('nodeMenu');
// Rebuilt on open: the graphs/ entries (§19) depend on the project, and a
// graph file cannot declare parameters (§19.1).
function rebuildNodeMenu() {
  nodeMenu.textContent = '';
  const items = NODE_PALETTE
      .filter((type) => !(inSubgraph() && type === 'parameter'))
      .map((type) => ({ label: type, type }));
  try {
    for (const path of JSON.parse(wasm?.graphs() ?? '[]')) {
      // §20.6: package graphs list beside the project's own; the label
      // collapses "packages/sway/graphs/sway.json" to "⬡ sway".
      const pkg = path.match(/^packages\/([a-z0-9-]+)\/graphs\/(.+)\.json$/);
      items.push({
        label: pkg ? '⬡ ' + (pkg[2] === pkg[1] ? pkg[1]
                                               : `${pkg[1]}/${pkg[2]}`)
                   : '▣ ' + path.replace(/^graphs\//, '')
                                .replace(/\.json$/, ''),
        type: 'graph', graphPath: path,
      });
    }
  } catch { /* no runtime, no graph entries */ }
  for (const item of items) {
    const button = document.createElement('button');
    button.textContent = item.label;
    button.onclick = (e) => {
      const anchor = e.target.getBoundingClientRect();
      nodeMenu.hidden = true;
      addNodeOfType(item.type, anchor, item.graphPath);
    };
    nodeMenu.appendChild(button);
  }
}
document.getElementById('addNodeBtn').onclick = (e) => {
  const rect = e.target.getBoundingClientRect();
  rebuildNodeMenu();
  nodeMenu.style.left = rect.left + 'px';
  nodeMenu.style.top = rect.bottom + 4 + 'px';
  nodeMenu.hidden = !nodeMenu.hidden;
};
window.addEventListener('mousedown', (e) => {
  if (!nodeMenu.hidden && !nodeMenu.contains(e.target) &&
      e.target.id !== 'addNodeBtn') {
    nodeMenu.hidden = true;
  }
}, true);

window.addEventListener('keydown', (e) => {
  if ((e.key === 'Delete' || e.key === 'Backspace') &&
      document.body.classList.contains('graph-mode') && selectedNodeId &&
      !/^(INPUT|TEXTAREA|SELECT)$/.test(document.activeElement?.tagName)) {
    e.preventDefault();
    deleteSelectedNode();
  }
});

graphCanvas.addEventListener('wheel', (e) => {
  e.preventDefault();
  const next = Math.min(Math.max(
      graphView.scale * (e.deltaY > 0 ? 1 / 1.15 : 1.15), 0.2), 3);
  // Keep the world point under the cursor fixed while zooming.
  const wx = e.offsetX / graphView.scale - graphView.x;
  const wy = e.offsetY / graphView.scale - graphView.y;
  graphView.scale = next;
  graphView.x = e.offsetX / next - wx;
  graphView.y = e.offsetY / next - wy;
}, { passive: false });

// Double- or middle-click on a graph instance drills into the subgraph
// (§19.5); double-click elsewhere refits the view.
function nodeAtClient(clientX, clientY) {
  const { x, y } = graphWorldOf(clientX, clientY);
  return [...(graph?.nodes ?? [])].reverse().find((n) =>
      x >= n.x && x <= n.x + n.w && y >= n.y && y <= n.y + n.h);
}

graphCanvas.ondblclick = (e) => {
  const hit = nodeAtClient(e.clientX, e.clientY);
  if (hit?.source?.type === 'graph' &&
      typeof hit.source.graph === 'string') {
    enterSubgraph(hit.source.graph, hit.id);
    return;
  }
  fitGraphView();
};
graphCanvas.addEventListener('auxclick', (e) => {
  if (e.button !== 1) {
    return;
  }
  e.preventDefault();
  const hit = nodeAtClient(e.clientX, e.clientY);
  if (hit?.source?.type === 'graph' &&
      typeof hit.source.graph === 'string') {
    enterSubgraph(hit.source.graph, hit.id);
  }
});

// ---- asset drag-drop (§5.4): a dropped file lands in the project (working
// copy + write-through) and becomes a node at the drop point.

const assetKindOf = (file) => {
  if (file.type.startsWith('image/')) {
    return { dir: 'assets', type: 'image', prop: 'src' };
  }
  if (file.type === 'video/mp4' || /\.mp4$/i.test(file.name)) {
    return { dir: 'assets', type: 'video', prop: 'src' };
  }
  if (/\.wgsl$/i.test(file.name)) {
    return { dir: 'shaders', type: 'shader', prop: 'shader' };
  }
  return null;
};

graphCanvas.addEventListener('dragover', (e) => {
  if ([...e.dataTransfer.items].some((i) => i.kind === 'file')) {
    e.preventDefault();
  }
});
graphCanvas.addEventListener('drop', async (e) => {
  e.preventDefault();
  if (!wasm) {
    return;
  }
  if (!project) {
    setGraphStatus('open a project to import files — bundled scenes have ' +
                   'nowhere durable to put them');
    return;
  }
  if (inSubgraph()) {
    setGraphStatus('leave the subgraph view to import files');
    return;
  }
  const world = graphWorldOf(e.clientX, e.clientY);
  const doc = activeDoc();
  let offset = 0;
  let added = false;
  for (const file of e.dataTransfer.files) {
    const kind = assetKindOf(file);
    if (!kind) {
      setGraphStatus(`${file.name}: images, mp4 video, or .wgsl only ` +
                     '(remux other video with ffmpeg -c copy)');
      continue;
    }
    const base = file.name.replace(/[^A-Za-z0-9._-]+/g, '_');
    const rel = `${kind.dir}/${base}`;
    const data = new Uint8Array(await file.arrayBuffer());
    if (data.length > 128 << 20) {
      setGraphStatus(`${file.name}: large file — the preview keeps a copy ` +
                     'in memory');
    }
    const full = `${projectRoot()}/${rel}`;
    Module.FS.mkdirTree(full.slice(0, full.lastIndexOf('/')));
    Module.FS.writeFile(full, data);
    projectWriteThrough(rel, data);
    let stem = base.replace(/\.[^.]*$/, '').replace(/[^A-Za-z0-9_]/g, '_')
                   .replace(/^([0-9])/, '_$1') || kind.type;
    if (stem === 'time' || stem === 'mouse') {
      stem = '_' + stem;
    }
    const id = freeNodeId(stem);
    doc.nodes.push({ id, type: kind.type, [kind.prop]: rel });
    ((doc.editor ??= {}).positions ??= {})[id] =
        [Math.round(world.x - G.W / 2 + offset), Math.round(world.y + offset)];
    offset += 26;
    setSelectedNodeId(id);
    added = true;
  }
  if (added) {
    await pushFromGraph();
  }
});
