// ---- graph view: building the node/edge model and its layout ---------------

import { G, IMPLICIT_OUTPUTS } from '../nodedefs.js';
import { sceneSource } from '../document.js';
import { activeDoc, inSubgraph, viewStack, graph, setGraph,
         selectedIds, setSelectedIds, selectedNodeId, setSelectedNodeId,
         graphDrag, setGraphDrag, setWireDrag,
         updateGraphChrome } from './state.js';
import { specInputsOf, outputsOf, graphInterface, parseConnection,
         inputEntry, typeCategory, resolveDocPath, activeDocRoot,
         mapRefs } from './model.js';
import { renderInspector } from './inspector.js';

function buildGraph() {
  const doc = activeDoc();
  if (!doc?.nodes) {
    return null;
  }
  const nodes = [];
  const byId = new Map();
  const add = (source, implicit) => {
    const inputs = [];
    const pushBound = (name, value, category, forceArray) => {
      // An array-valued port (compositor.layers, §9.5) holds one reference
      // per element; a plain vector literal stays a single literal input.
      if (Array.isArray(value) &&
          (forceArray || value.length === 0 ||
           value.some((el) => typeof el !== 'number'))) {
        value.forEach((el, i) => {
          const entry = inputEntry(`${name}[${i}]`, el);
          entry.arrayOf = { base: name, index: i };
          entry.category ??= category;
          inputs.push(entry);
        });
        inputs.push({ name: `${name}[+]`, append: name, category });
      } else {
        const entry = inputEntry(name, value);
        entry.category ??= category;
        inputs.push(entry);
      }
    };
    // Spec'd ports first (spec order), whether bound or not; ports the spec
    // does not know about (draft docs, unreflectable shaders) follow, but
    // only where the document binds them.
    const seen = new Set();
    for (const [name, category, isArray, def] of
             (implicit ? [] : specInputsOf(source)) ?? []) {
      seen.add(name);
      const bound = source.inputs?.[name];
      if (bound !== undefined) {
        pushBound(name, bound, category, isArray);
      } else if (isArray) {
        inputs.push({ name: `${name}[+]`, append: name, category });
      } else {
        inputs.push({ name, unbound: true, category, def });
      }
    }
    for (const [name, value] of Object.entries(source.inputs ?? {})) {
      if (!seen.has(name)) {
        pushBound(name, value, undefined, false);
      }
    }
    const outputs = implicit ? IMPLICIT_OUTPUTS[source.id] : outputsOf(source);
    // Instances label with their block's name (palette markers: ⬡ =
    // package, ▣ = project graph) instead of the generic "graph".
    let typeLabel = null;
    if (!implicit && source.type === 'graph' &&
        typeof source.graph === 'string') {
      const path = resolveDocPath(source.graph, activeDocRoot());
      typeLabel = (path.startsWith('packages/') ? '⬡ ' : '▣ ') +
                  (graphInterface(path)?.name ?? source.graph);
    }
    const node = {
      id: source.id, type: implicit ? '(implicit)' : source.type,
      typeLabel,
      source, implicit: !!implicit, inputs, outputs, w: G.W,
      h: G.HEAD + Math.max(inputs.length, outputs.length, 1) * G.ROW + G.PAD,
      x: 0, y: 0,
    };
    nodes.push(node);
    byId.set(node.id, node);
    return node;
  };
  for (const source of doc.nodes) {
    if (source?.id && !byId.has(source.id)) {
      add(source, false);
    }
  }
  // Parameters draw as source nodes (Unreal-style): "$name" bindings become
  // visible wires, and a declared-but-unused parameter stays grabbable.
  // Graph ids are '$name' — a namespace no §3 node id can enter. Graph
  // files have no parameters (§19.1), so the subgraph view skips this.
  for (const [name, decl] of Object.entries(
           (inSubgraph() ? null : doc.parameters) ?? {})) {
    if (typeof decl !== 'object' || decl === null) {
      continue;
    }
    const { type: valueType, ...rest } = decl;
    const node = {
      id: '$' + name, param: name, decl, type: 'parameter',
      source: { id: '$' + name, type: 'parameter', valueType, ...rest },
      implicit: false, inputs: [],
      outputs: [[valueType ?? 'value', 'value']],
      w: G.W, h: G.HEAD + G.ROW + G.PAD, x: 0, y: 0,
    };
    nodes.push(node);
    byId.set(node.id, node);
  }
  const edges = [];
  for (const node of nodes.slice()) {
    for (const input of node.inputs) {
      if (input.param) {
        const from = byId.get('$' + input.param);
        if (from) {
          input.paramEdge = true; // the wire replaces the pin's text label
          input.category ??= 'value';
          edges.push({ from: from.id, fromPort: from.outputs[0][0],
                       to: node.id, toPort: input.name,
                       category: 'value', previous: false });
        }
        continue;
      }
      if (!input.conn) {
        continue;
      }
      let from = byId.get(input.conn.node);
      if (!from && IMPLICIT_OUTPUTS[input.conn.node]) {
        // Implicit singletons (§9.7–9.8) appear once something uses them.
        from = add({ id: input.conn.node }, true);
      }
      if (!from) {
        continue; // the runtimes validated the doc; stay lenient anyway
      }
      const fromPort = input.conn.port ?? from.outputs[0]?.[0];
      const found = from.outputs.find(([n]) => n === fromPort);
      const edge = { from: from.id, fromPort, to: node.id,
                     toPort: input.name,
                     category: found ? found[1] : 'value',
                     previous: input.conn.previous };
      input.category = edge.category;
      edges.push(edge);
    }
  }
  // Subgraph view (§19.5): the declared interface draws as two boundary
  // boxes, with its binds and output mappings as (read-only) wires.
  if (inSubgraph()) {
    const face = (id, inputs, outputs) => {
      const node = {
        id, type: 'interface', boundary: true,
        source: { id, type: 'interface' }, implicit: false,
        inputs, outputs, w: G.W,
        h: G.HEAD + Math.max(inputs.length, outputs.length, 1) * G.ROW +
            G.PAD,
        x: 0, y: 0,
      };
      nodes.push(node);
      byId.set(id, node);
      return node;
    };
    // The trailing '+' pins start new interface entries by dragging, like
    // the array append pins.
    const inFace = face('#inputs', [],
        [...Object.entries(doc.inputs ?? {}).map(([name, decl]) =>
             [name, typeCategory(decl?.type)]),
         ['+', 'value']]);
    const outFace = face('#outputs',
        [...Object.keys(doc.outputs ?? {}).map((name) =>
             ({ name, unbound: true, category: 'value' })),
         { name: '+', unbound: true, category: 'value' }],
        []);
    // §19.5: the boundary shows the wiring of the instance we entered
    // through — a declared port draws wired only when the parent doc
    // actually supplies (inputs) or consumes (outputs) it there.
    const parentDoc = viewStack.length > 1
        ? viewStack[viewStack.length - 2].doc : sceneSource;
    const inst = parentDoc?.nodes?.find(
        (n) => n.id === viewStack[viewStack.length - 1]?.viaInstance);
    inFace.dimPorts = new Set(['+']);
    for (const name of Object.keys(doc.inputs ?? {})) {
      if (inst && !(name in (inst.inputs ?? {}))) {
        inFace.dimPorts.add(name);
      }
    }
    const consumedInParent = (port) => {
      if (!inst) {
        return true; // unknown instance: keep the mapped look
      }
      for (const n of parentDoc.nodes ?? []) {
        if (n.id === inst.id) {
          continue;
        }
        let hit = false;
        for (const value of Object.values(n.inputs ?? {})) {
          mapRefs(value, (conn) => {
            if (conn.node === inst.id &&
                (conn.port ?? 'result') === port) {
              hit = true;
            }
            return null;
          });
        }
        if (hit) {
          return true;
        }
      }
      return false;
    };
    for (const [name, decl] of Object.entries(doc.inputs ?? {})) {
      const binds = Array.isArray(decl?.bind) ? decl.bind : [decl?.bind];
      for (const bind of binds) {
        const conn = typeof bind === 'string' ? parseConnection(bind) : null;
        const to = conn && byId.get(conn.node);
        if (to) {
          const entry = to.inputs.find((i) => i.name === conn.port);
          if (entry) {
            // The bind is always picked up from here (boundBind), but the
            // pin only draws wired when the interface input is itself fed
            // by the instance — a dim boundary row keeps its inner pins
            // dim, showing the declared default they run at.
            entry.boundBind = name;
            entry.category ??= typeCategory(decl?.type);
            if (!inFace.dimPorts.has(name)) {
              entry.boundFed = true;
            } else if (entry.def === undefined) {
              entry.def = decl?.default;
            }
          }
          edges.push({ from: inFace.id, fromPort: name, to: to.id,
                       toPort: conn.port, category: typeCategory(decl?.type),
                       previous: false });
        }
      }
    }
    for (const [name, ref] of Object.entries(doc.outputs ?? {})) {
      const conn = typeof ref === 'string' ? parseConnection(ref) : null;
      const from = conn && byId.get(conn.node);
      if (from) {
        const fromPort = conn.port ?? from.outputs[0]?.[0];
        const found = from.outputs.find(([n]) => n === fromPort);
        // A mapped output draws wired (filled pin, bright label, the
        // inner port's category) only when the parent consumes it there;
        // otherwise it stays an open slot.
        const entry = outFace.inputs.find((i) => i.name === name);
        if (entry) {
          entry.category = found ? found[1] : 'value';
          if (consumedInParent(name)) {
            entry.unbound = false;
            entry.conn = { node: from.id, port: fromPort,
                           previous: false };
          }
        }
        edges.push({ from: from.id, fromPort, to: '#outputs', toPort: name,
                     category: found ? found[1] : 'value', previous: false });
      }
    }
  }
  layoutGraph(nodes, byId, edges);
  return { nodes, byId, edges };
}

// Columns by longest path from the sources, previous edges ignored (with
// them the graph would not be a DAG, §10); stored §2.1 positions override.
function layoutGraph(nodes, byId, edges) {
  const incoming = new Map(nodes.map((n) => [n.id, []]));
  for (const edge of edges) {
    if (!edge.previous) {
      incoming.get(edge.to).push(edge.from);
    }
  }
  const rank = new Map();
  const rankOf = (id, visiting) => {
    if (rank.has(id)) {
      return rank.get(id);
    }
    if (visiting.has(id)) {
      return 0; // cycles never validate, but never recurse forever
    }
    visiting.add(id);
    const r = incoming.get(id).reduce(
        (best, from) => Math.max(best, rankOf(from, visiting) + 1), 0);
    visiting.delete(id);
    rank.set(id, r);
    return r;
  };
  const columns = [];
  for (const node of nodes) {
    (columns[rankOf(node.id, new Set())] ??= []).push(node);
  }
  columns.forEach((column, r) => {
    let y = G.MARGIN;
    for (const node of column) {
      node.x = G.MARGIN + r * G.COL;
      node.y = y;
      y += node.h + G.GAP;
    }
  });
  const positions = activeDoc()?.editor?.positions;
  for (const [id, pos] of Object.entries(positions ?? {})) {
    const node = byId.get(id);
    if (node && Array.isArray(pos) && pos.length === 2 &&
        pos.every((v) => typeof v === 'number')) {
      node.x = pos[0];
      node.y = pos[1];
    }
  }
}

export function refreshGraph() {
  if (graphDrag?.kind === 'node') {
    setGraphDrag(null); // the dragged node object just went stale
  }
  setWireDrag(null);
  setGraph(buildGraph());
  if (selectedNodeId && !graph?.byId.has(selectedNodeId)) {
    setSelectedNodeId(null);
  }
  setSelectedIds(
      new Set([...selectedIds].filter((id) => graph?.byId.has(id))));
  renderInspector();
  updateGraphChrome();
}
