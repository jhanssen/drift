// The document model: reference grammar, per-node port reflection, and the
// §19 graph-file interface cache — shared by build, wiring, and subgraphs.

import { wasm } from '../preview.js';
import { nodeInputs, nodeOutputs } from '../nodedefs.js';
import { viewStack } from './state.js';

// §20.1: paths inside a package's graph file resolve against the package
// root — mirror the loader's rewrite when reading, reflecting, or
// flattening package content.
export const pkgRootOf = (path) =>
    (typeof path === 'string' &&
     path.match(/^packages\/[a-z0-9-]+(?:\.[a-z0-9-]+)?\//)?.[0]) || '';
export const activeDocRoot = () =>
    viewStack.length ? pkgRootOf(viewStack[viewStack.length - 1].path) : '';
export const resolveDocPath = (p, base) =>
    base && typeof p === 'string' && !p.startsWith('packages/') ? base + p
                                                                : p;

// path -> parsed graph file (null while unreadable); dropped whenever the
// underlying file may have changed.
export const graphDocs = new Map();

export function graphDoc(path) {
  if (!graphDocs.has(path)) {
    let doc = null;
    try {
      const text = wasm?.readAsset(path);
      doc = text ? JSON.parse(text) : null;
    } catch (err) {
      console.log(`editor: graph file ${path}: ${err}`);
    }
    graphDocs.set(path, doc);
  }
  return graphDocs.get(path);
}

export const typeCategory = (t) =>
    t === 'texture' || t === 'event' || t === 'buffer' ? t : 'value';

// The declared interface (§19.2) as editor pin lists; null while the file
// is unreadable. The visiting set keeps a (never-valid) instantiation
// cycle from hanging the page.
const ifaceVisiting = new Set();

export function graphInterface(path) {
  if (ifaceVisiting.has(path)) {
    return null;
  }
  ifaceVisiting.add(path);
  try {
    const doc = graphDoc(path);
    if (!doc) {
      return null;
    }
    const inputs = Object.entries(doc.inputs ?? {}).map(([name, decl]) => [
      name, typeCategory(decl?.type), false, decl?.default,
    ]);
    const outputs = Object.entries(doc.outputs ?? {}).map(([name, ref]) => {
      const conn = parseConnection(ref);
      const inner = doc.nodes?.find((n) => n.id === conn?.node);
      const ports = inner ? outputsOf(inner, pkgRootOf(path)) : [];
      const port = ports.find(
          ([n]) => n === (conn?.port ?? ports[0]?.[0]));
      return [name, port ? port[1] : 'value'];
    });
    outputs.sort((a, b) => (b[0] === 'result') - (a[0] === 'result'));
    return { inputs, outputs, name: doc.name ?? path };
  } finally {
    ifaceVisiting.delete(path);
  }
}

export function specInputsOf(source, base = activeDocRoot()) {
  if (source.type === 'graph') {
    // §19.3: pins are the graph file's declared interface.
    return graphInterface(resolveDocPath(source.graph, base))?.inputs ??
           null;
  }
  if (source.type === 'shader' || source.type === 'compute') {
    // The WGSL is the port declaration (§9.10/§18.2); the wasm runtime
    // reflects it. Without a preview runtime only bound ports get pins.
    return reflectPorts(source, false, base)?.map((p) =>
        [p.name, reflectCategory(p), false]) ?? null;
  }
  if (source.type === 'module') {
    return moduleInterface(source, base)?.inputs ?? null;
  }
  return nodeInputs(source.type);
}

// §4.5: a module node's pins are its interface JSON, in the same
// lexicographic order the runtime lays the I/O block out in. Value inputs
// without a declared default are required, like the loader enforces.
function moduleInterface(source, base = activeDocRoot()) {
  if (!wasm || typeof source.interface !== 'string') {
    return null;
  }
  try {
    const text = wasm.readAsset(resolveDocPath(source.interface, base));
    const doc = text ? JSON.parse(text) : null;
    if (!doc) {
      return null;
    }
    const sorted = (obj) => Object.entries(obj ?? {})
        .sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0));
    return {
      inputs: sorted(doc.inputs).map(([name, d]) => [
        name, typeCategory(d?.type), false, d?.default,
        d?.type !== 'event' && d?.default === undefined,
      ]),
      outputs: sorted(doc.outputs).map(([name, d]) =>
          [name, typeCategory(d?.type)]),
    };
  } catch (err) {
    console.log(`editor: module interface ${source.interface}: ${err}`);
    return null;
  }
}

const reflectCategory = (p) =>
    p.type === 'texture' ? 'texture' : p.type === 'buffer' ? 'buffer' : 'value';

function reflectPorts(source, dir, base = activeDocRoot()) {
  if (!wasm || typeof source.shader !== 'string') {
    return null;
  }
  try {
    const reflected =
        JSON.parse(wasm.reflect(resolveDocPath(source.shader, base)));
    return reflected.ports?.filter((p) => (p.dir === 'out') === dir) ?? null;
  } catch {
    return null;
  }
}

export function outputsOf(node, base = activeDocRoot()) {
  if (node.type === 'graph') {
    return graphInterface(resolveDocPath(node.graph, base))?.outputs ??
           [['result', 'value']];
  }
  if (node.type === 'sequence') {
    return (node.tracks ?? []).filter((t) => t?.name).map((t) =>
        [t.name, t.kind === 'event' ? 'event' : 'value']);
  }
  if (node.type === 'compute') {
    // §18.2: outputs are the read_write storage bindings, reflected.
    return reflectPorts(node, true, base)?.map((p) =>
        [p.name, reflectCategory(p)]) ?? [];
  }
  if (node.type === 'module') {
    return moduleInterface(node, base)?.outputs ?? [];
  }
  return nodeOutputs(node.type) ?? [['result', 'value']];
}

// Reference grammar (§7): "@node", "@node.port", { node, port, previous }.
export function parseConnection(value) {
  if (typeof value === 'string' && value[0] === '@') {
    const dot = value.indexOf('.');
    return { node: dot < 0 ? value.slice(1) : value.slice(1, dot),
             port: dot < 0 ? null : value.slice(dot + 1), previous: false };
  }
  if (value && typeof value === 'object' && !Array.isArray(value) &&
      typeof value.node === 'string') {
    return { node: value.node, port: value.port ?? null,
             previous: !!value.previous };
  }
  return null;
}

export function inputEntry(name, value) {
  const conn = parseConnection(value);
  if (conn) {
    return { name, conn };
  }
  if (typeof value === 'string' && value[0] === '$') {
    return { name, param: value.slice(1) };
  }
  return { name, literal: value };
}

// Always the explicit "@node.port" form: unambiguous, and stable if the
// producer's default output ever changes (e.g. sequence track reorder).
export const refFor = (nodeId, port) => `@${nodeId}.${port}`;

// Rewrites every connection in a raw input value through mapNode; a null
// return keeps the original.
export function mapRefs(value, mapNode) {
  if (Array.isArray(value) && value.some((el) => typeof el !== 'number')) {
    return value.map((el) => mapRefs(el, mapNode));
  }
  const conn = parseConnection(value);
  return (conn && mapNode(conn)) ?? value;
}

export const refOf = (node, port, previous) => previous
    ? { node, ...(port ? { port } : {}), previous: true }
    : port ? `@${node}.${port}` : `@${node}`;
