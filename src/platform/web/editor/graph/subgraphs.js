// ---- subgraphs (§19): the drill-in view stack and its edits ----------------

import { wasm, setWasmManifest } from '../preview.js';
import { live, liveSend } from '../live.js';
import { openQuickForm } from '../ui.js';
import { currentValues, refreshPanel } from '../params.js';
import { sceneSource, pushFromGraph } from '../document.js';
import { projectRoot, projectWriteThrough } from '../project.js';
import { implicitOutputs } from '../nodedefs.js';
import { viewStack, selectedIds, setSelectedIds, setSelectedNodeId,
         setGraphNeedsFit, setGraphStatus, viewReadOnly, updateGraphChrome,
         graph, inSubgraph } from './state.js';
import { graphDocs, graphDoc, resolveDocPath, activeDocRoot, pkgRootOf,
         parseConnection, refOf, mapRefs, specInputsOf,
         outputsOf } from './model.js';
import { refreshGraph } from './build.js';
import { freeNodeId } from './interact.js';

export function enterSubgraph(path, viaInstance) {
  if (!wasm) {
    setGraphStatus('the subgraph view needs the preview runtime');
    return;
  }
  // A nested instance inside a package references its file
  // package-relative (§20.1); canonicalize so reads and the read-only
  // check see the packages/ form.
  path = resolveDocPath(path, activeDocRoot());
  graphDocs.delete(path); // fresh read — the file may have changed
  const doc = graphDoc(path);
  if (!doc) {
    setGraphStatus(`cannot read ${path}`);
    return;
  }
  viewStack.push({ path, doc, viaInstance });
  setSelectedNodeId(null);
  selectedIds.clear();
  setGraphNeedsFit(true);
  refreshGraph();
  updateGraphChrome();
}

export function leaveSubgraph() {
  viewStack.pop();
  setSelectedNodeId(null);
  selectedIds.clear();
  setGraphNeedsFit(true);
  refreshGraph();
  updateGraphChrome();
}

// Writes a project file everywhere: the preview's MEMFS and, when a live
// runtime is attached, its project directory (the write-asset verb).
// Returns the live error, if any.
export async function writeProjectFile(path, contents) {
  wasm?.writeAsset(projectRoot(), path, contents);
  projectWriteThrough(path, contents);
  graphDocs.delete(path);
  if (live) {
    try {
      await liveSend('write-asset', { path, contents });
    } catch (err) {
      console.log(`editor: write-asset: ${err}`);
      return String(err);
    }
  }
  return null;
}

// Writes the edited graph file and reloads the scene (both runtimes) so
// flattening picks the change up.
export async function commitSubgraph() {
  if (viewReadOnly()) {
    // Snap back whatever local mutation led here.
    const top = viewStack[viewStack.length - 1];
    graphDocs.delete(top.path);
    top.doc = graphDoc(top.path);
    refreshGraph();
    setGraphStatus('package graphs are read-only (§20.6)');
    return;
  }
  const view = viewStack[viewStack.length - 1];
  const liveError = await writeProjectFile(
      view.path, JSON.stringify(view.doc, null, 2) + '\n');
  let wasmError = null;
  if (wasm.load(JSON.stringify(sceneSource))) {
    for (const [name, values] of currentValues) {
      wasm.set(name, values);
    }
    const manifest = JSON.parse(wasm.describe());
    if (manifest.scene) {
      setWasmManifest(manifest);
    }
  } else {
    wasmError = (wasm.errors() || 'load failed').split('\n')[0];
  }
  if (live && !liveError) {
    try {
      await liveSend('load', { scene: JSON.stringify(sceneSource, null, 2) });
    } catch (err) {
      console.log(`editor: live load after ${view.path}: ${err}`);
    }
  }
  setGraphStatus(wasmError ? `draft — ${wasmError}`
                           : liveError ? `live: ${liveError}` : null);
  refreshPanel();
}

// ---- §19.5: make-subgraph (collapse a selection) and flatten (inline) ------

// The declared §19.2 type for a boundary-crossing wire into node.port:
// category pins it for texture/event/buffer; value ports fall back to the
// parameter's declared type or the default literal's shape.
function boundaryType(node, port, rawValue) {
  const spec = specInputsOf(node.source)?.find(([n]) => n === port);
  const category = spec?.[1];
  if (category === 'texture' || category === 'event' ||
      category === 'buffer') {
    return category;
  }
  if (typeof rawValue === 'string' && rawValue[0] === '$') {
    return sceneSource.parameters?.[rawValue.slice(1)]?.type ?? 'scalar';
  }
  const def = spec?.[3];
  return Array.isArray(def) ? `vec${def.length}` : 'scalar';
}

function makeSubgraphFromSelection(anchor) {
  if (inSubgraph() || !wasm || !sceneSource) {
    return;
  }
  const picked = [...selectedIds].map((id) => graph?.byId.get(id))
      .filter((n) => n && !n.implicit && !n.param && !n.boundary &&
                     n.source.type !== 'output');
  if (picked.length < 2 || picked.length !== selectedIds.size) {
    setGraphStatus('make subgraph: select two or more regular nodes ' +
                   '(the output node stays in the scene)');
    return;
  }
  const ids = new Set(picked.map((n) => n.id));
  // The selection must be connected through its own wires (undirected).
  const adjacency = new Map([...ids].map((id) => [id, []]));
  for (const edge of graph.edges) {
    if (ids.has(edge.from) && ids.has(edge.to)) {
      adjacency.get(edge.from).push(edge.to);
      adjacency.get(edge.to).push(edge.from);
    }
  }
  const seen = new Set([picked[0].id]);
  const queue = [picked[0].id];
  while (queue.length) {
    for (const next of adjacency.get(queue.pop())) {
      if (!seen.has(next)) {
        seen.add(next);
        queue.push(next);
      }
    }
  }
  if (seen.size !== ids.size) {
    setGraphStatus('make subgraph: the selection must be connected');
    return;
  }
  // §19.2 binds have no array-element form, so an outside wire INTO a
  // selected array port (compositor layers) cannot become an interface
  // input. Outgoing array consumption is fine — the outside element just
  // rewires to the instance.
  const arrayCrossesIn = (owner) =>
      Object.values(owner.inputs ?? {}).some((value) =>
          Array.isArray(value) && value.some((el) => {
            const conn = parseConnection(el);
            return conn && !ids.has(conn.node) &&
                   !implicitOutputs(conn.node);
          }));
  if (picked.some((n) => arrayCrossesIn(n.source))) {
    setGraphStatus('make subgraph: an outside wire into a selected array ' +
                   'port (layers) is not supported yet');
    return;
  }
  setGraphStatus(null);

  openQuickForm(anchor, 'make subgraph', [
    { key: 'name', label: 'file name', value: 'block' },
  ], (v) => {
    if (!/^[A-Za-z_][A-Za-z0-9_-]*$/.test(v.name)) {
      return 'a simple file name (letters, digits, _ or -)';
    }
    const path = `graphs/${v.name}.json`;
    if (wasm.readAsset(path)) {
      return `${path} already exists`;
    }

    const file = { version: 1, name: v.name, inputs: {}, outputs: {},
                   nodes: [] };
    const instId = freeNodeId(v.name.replace(/-/g, '_'));
    const instInputs = {};
    const usedNames = new Set();
    const exportName = (want) => {
      let name = want.replace(/[^A-Za-z0-9_]/g, '_');
      if (!/^[A-Za-z_]/.test(name)) {
        name = '_' + name;
      }
      while (usedNames.has(name)) {
        name += '_';
      }
      usedNames.add(name);
      return name;
    };
    const paramInputs = new Map(); // $param -> exported input name

    // Inner copies; boundary-crossing and $param bindings become the
    // interface (incoming wires -> declared inputs).
    for (const node of picked) {
      const src = structuredClone(node.source);
      for (const [port, value] of Object.entries(src.inputs ?? {})) {
        const conn = parseConnection(value);
        if (conn && !ids.has(conn.node) && !implicitOutputs(conn.node)) {
          const name = exportName(port);
          file.inputs[name] = { type: boundaryType(node, port, value),
                                bind: `@${src.id}.${port}` };
          instInputs[name] = value; // keeps previous/object form
          delete src.inputs[port];
        } else if (typeof value === 'string' && value[0] === '$') {
          const param = value.slice(1);
          if (!paramInputs.has(param)) {
            const name = exportName(param);
            paramInputs.set(param, name);
            file.inputs[name] = { type: boundaryType(node, port, value),
                                  bind: [] };
            instInputs[name] = value;
          }
          const decl = file.inputs[paramInputs.get(param)];
          decl.bind = [].concat(decl.bind, `@${src.id}.${port}`);
          delete src.inputs[port];
        }
      }
      file.nodes.push(src);
      const pos = sceneSource.editor?.positions?.[src.id];
      if (pos) {
        ((file.editor ??= {}).positions ??= {})[src.id] = pos;
      }
    }

    // Outgoing wires -> declared outputs; consumers rewire to the instance.
    const exports = new Map(); // 'node.port' -> exported name
    for (const other of sceneSource.nodes) {
      if (ids.has(other.id)) {
        continue;
      }
      for (const [port, value] of Object.entries(other.inputs ?? {})) {
        other.inputs[port] = mapRefs(value, (conn) => {
          if (!ids.has(conn.node)) {
            return null;
          }
          const producer = graph.byId.get(conn.node);
          const fromPort =
              conn.port ?? outputsOf(producer.source)[0]?.[0] ?? 'result';
          const key = `${conn.node}.${fromPort}`;
          if (!exports.has(key)) {
            const name = exports.size ? exportName(`${conn.node}_${fromPort}`)
                                      : 'result';
            exports.set(key, name);
            file.outputs[name] = `@${conn.node}.${fromPort}`;
          }
          return refOf(instId, exports.get(key), conn.previous);
        });
      }
    }
    if (!exports.size) {
      // Nothing outside consumes the selection: export the sink so the
      // file still has its required 'result' (§19.2).
      const consumedInside = new Set(graph.edges
          .filter((e) => ids.has(e.from) && ids.has(e.to))
          .map((e) => e.from));
      const sink = picked.find((n) => !consumedInside.has(n.id)) ?? picked[0];
      file.outputs.result =
          `@${sink.id}.${outputsOf(sink.source)[0]?.[0] ?? 'result'}`;
    }

    // Swap the selection for the instance.
    const at = sceneSource.nodes.findIndex((n) => ids.has(n.id));
    sceneSource.nodes = sceneSource.nodes.filter((n) => !ids.has(n.id));
    const inst = { id: instId, type: 'graph', graph: path };
    if (Object.keys(instInputs).length) {
      inst.inputs = instInputs;
    }
    sceneSource.nodes.splice(Math.max(at, 0), 0, inst);
    const positions = (sceneSource.editor ??= {}).positions ??= {};
    positions[instId] = [
      Math.round(picked.reduce((s, n) => s + n.x, 0) / picked.length),
      Math.round(picked.reduce((s, n) => s + n.y, 0) / picked.length),
    ];
    for (const id of ids) {
      delete positions[id];
    }

    writeProjectFile(path, JSON.stringify(file, null, 2) + '\n');
    setSelectedIds(new Set([instId]));
    setSelectedNodeId(instId);
    pushFromGraph();
    return null;
  });
}

// The inverse: inline the instance this subgraph view was entered through
// back into the scene, keeping the inlined nodes selected.
function flattenInstance() {
  const view = viewStack[0];
  if (viewStack.length !== 1 || !view?.viaInstance || !sceneSource) {
    return;
  }
  const inst = sceneSource.nodes.find((n) => n.id === view.viaInstance);
  if (!inst) {
    return;
  }
  const doc = view.doc;
  const taken = new Set(sceneSource.nodes.map((n) => n.id));
  taken.delete(inst.id);
  const rename = new Map();
  for (const inner of doc.nodes ?? []) {
    let id = `${inst.id}_${inner.id}`;
    for (let i = 2; taken.has(id); ++i) {
      id = `${inst.id}_${inner.id}${i}`;
    }
    taken.add(id);
    rename.set(inner.id, id);
  }
  const copies = (doc.nodes ?? []).map((inner) => {
    const copy = structuredClone(inner);
    copy.id = rename.get(inner.id);
    // §20.1: a package's inner paths are package-relative; inlined into
    // the scene they must carry the package root (the loader's rewrite).
    const base = pkgRootOf(view.path);
    for (const key of ['shader', 'src', 'graph']) {
      if (typeof copy[key] === 'string') {
        copy[key] = resolveDocPath(copy[key], base);
      }
    }
    for (const [port, value] of Object.entries(copy.inputs ?? {})) {
      copy.inputs[port] = mapRefs(value, (conn) =>
          rename.has(conn.node)
              ? refOf(rename.get(conn.node), conn.port, conn.previous)
              : null);
    }
    return copy;
  });
  // Interface inputs land on their bound ports; declared defaults fill the
  // unbound ones so behavior is unchanged (§19.3).
  for (const [name, decl] of Object.entries(doc.inputs ?? {})) {
    const value = inst.inputs?.[name] ?? decl?.default;
    if (value === undefined) {
      continue;
    }
    for (const bind of [].concat(decl?.bind ?? [])) {
      const conn = typeof bind === 'string' ? parseConnection(bind) : null;
      const target =
          conn && copies.find((c) => c.id === rename.get(conn.node));
      if (target) {
        (target.inputs ??= {})[conn.port] = structuredClone(value);
      }
    }
  }
  // Outer consumers of the instance follow the output mapping.
  const exportsMap = new Map(Object.entries(doc.outputs ?? {}).map(
      ([name, ref]) => {
        const conn = parseConnection(ref);
        return [name, conn ? { node: rename.get(conn.node),
                               port: conn.port ?? null } : null];
      }));
  for (const other of sceneSource.nodes) {
    if (other.id === inst.id) {
      continue;
    }
    for (const [port, value] of Object.entries(other.inputs ?? {})) {
      other.inputs[port] = mapRefs(value, (conn) => {
        if (conn.node !== inst.id) {
          return null;
        }
        const exp = exportsMap.get(conn.port ?? 'result');
        return exp ? refOf(exp.node, exp.port, conn.previous) : null;
      });
    }
  }
  const at = sceneSource.nodes.indexOf(inst);
  sceneSource.nodes.splice(at, 1, ...copies);
  const positions = (sceneSource.editor ??= {}).positions ??= {};
  const base = positions[inst.id] ?? [60, 60];
  delete positions[inst.id];
  const filePos = doc.editor?.positions ?? {};
  const minX = Math.min(...Object.values(filePos).map((p) => p[0]), 0);
  const minY = Math.min(...Object.values(filePos).map((p) => p[1]), 0);
  copies.forEach((copy, i) => {
    const pos = filePos[(doc.nodes ?? [])[i]?.id];
    positions[copy.id] = pos
        ? [base[0] + pos[0] - minX, base[1] + pos[1] - minY]
        : [base[0] + (i % 3) * 180, base[1] + Math.floor(i / 3) * 90];
  });
  viewStack.length = 0;
  setSelectedIds(new Set(copies.map((c) => c.id)));
  setSelectedNodeId(copies[0]?.id ?? null);
  setGraphNeedsFit(true);
  setGraphStatus(null);
  pushFromGraph();
  updateGraphChrome();
}

document.getElementById('makeSubBtn').onclick = (e) =>
    makeSubgraphFromSelection(e.target.getBoundingClientRect());
document.getElementById('flattenBtn').onclick = flattenInstance;

document.getElementById('backBtn').onclick = leaveSubgraph;
