import { openQuickForm, isIdentifier } from '../ui.js';
import { sceneSource, pushFromGraph } from '../document.js';
import { activeDoc, inSubgraph, graph, setGraphStatus } from './state.js';
import { parseConnection, refFor, specInputsOf, typeCategory } from './model.js';
import { graphCanvas } from './canvas.js';

// ---- graph view: wiring edits (§16.1: the graph owns *what*) ---------------

// Wires are edited by dragging pins: an output pin starts a new wire seeking
// an input; an empty input pin seeks an output; a wired input pin picks the
// wire up (its raw document value moves along, keeping `previous` and the
// reference form). Dropping a picked-up wire on nothing disconnects it. The
// runtimes stay the validation authority: an invalid document keeps the old
// scene running and the editor shows a draft status until the push takes.
export function startWireDrag(pin) {
  if (!activeDoc()) {
    return null;
  }
  if (pin.node.boundary) {
    // §19.5: dragging boundary pins edits the interface. '#inputs' output
    // pins seek an inner input port to bind; '#outputs' input pins pick up
    // (or, from '+', create) an export mapping.
    if (pin.node.id === '#inputs' && pin.side === 'out') {
      if (pin.output[0] === '+') {
        return { seek: 'in', bindNew: true,
                 from: { node: '#inputs', port: '+', category: null } };
      }
      return { seek: 'in', bindName: pin.output[0],
               from: { node: '#inputs', port: pin.output[0],
                       category: pin.output[1] } };
    }
    if (pin.node.id === '#outputs' && pin.side === 'in') {
      if (pin.input.name === '+') {
        return { seek: 'out', exportNew: true,
                 to: { node: pin.node, entry: pin.input } };
      }
      return { seek: 'out', exportName: pin.input.name,
               to: { node: pin.node, entry: pin.input } };
    }
    return null;
  }
  if (pin.side === 'out') {
    return { seek: 'in',
             from: { node: pin.node.id, port: pin.output[0],
                     category: pin.output[1],
                     param: pin.node.param ?? null } };
  }
  const input = pin.input;
  if (input.boundBind) {
    // Pick up an interface bind at the inner end: drop elsewhere re-binds,
    // drop on nothing removes it (the declaration goes with its last bind).
    return { seek: 'in', bindName: input.boundBind,
             detachBind: `@${pin.node.id}.${input.name}`,
             from: { node: '#inputs', port: input.boundBind,
                     category: input.category ?? 'value' } };
  }
  if (input.paramEdge) {
    const producer = graph.byId.get('$' + input.param);
    return { seek: 'in', raw: '$' + input.param,
             detach: { node: pin.node, entry: input },
             from: { node: producer.id, port: producer.outputs[0][0],
                     category: 'value', param: input.param } };
  }
  if (input.conn) {
    const raw = input.arrayOf
        ? pin.node.source.inputs[input.arrayOf.base][input.arrayOf.index]
        : pin.node.source.inputs[input.name];
    const producer = graph.byId.get(input.conn.node);
    return { seek: 'in', raw,
             detach: { node: pin.node, entry: input },
             from: producer ? {
               node: producer.id,
               port: input.conn.port ?? producer.outputs[0]?.[0],
               category: input.category ?? 'value',
             } : null };
  }
  return { seek: 'out', to: { node: pin.node, entry: input } };
}

// A wire dropped on a port replaces its literal/$param binding; remember
// what it clobbered so a later disconnect restores the original instead of
// leaving a required port unbound (an inexplicable draft — the port never
// looked empty to the author).
export const priorBindings = new Map(); // 'node/port' -> raw JSON value

export function unbindPort(nodeId, inputs, name) {
  const key = `${nodeId}/${name}`;
  const prior = priorBindings.get(key);
  priorBindings.delete(key);
  if (prior !== undefined) {
    inputs[name] = prior;
  } else {
    delete inputs[name];
  }
}

function writeInput(node, entry, rawValue) {
  const inputs = (node.source.inputs ??= {});
  if (entry.append) {
    (inputs[entry.append] ??= []).push(rawValue);
  } else if (entry.arrayOf) {
    inputs[entry.arrayOf.base][entry.arrayOf.index] = rawValue;
  } else {
    const old = inputs[entry.name];
    if (old !== undefined && !parseConnection(old)) {
      priorBindings.set(`${node.id}/${entry.name}`, old);
    }
    inputs[entry.name] = rawValue;
  }
}

function removeInput(node, entry) {
  const inputs = node.source.inputs;
  if (!inputs) {
    return;
  }
  if (entry.arrayOf) {
    inputs[entry.arrayOf.base].splice(entry.arrayOf.index, 1);
  } else {
    unbindPort(node.id, inputs, entry.name);
  }
}

// Click a pin to type its value in place: a number, an "x, y…" vector, a
// $parameter, an @node.port reference, or empty to fall back to the
// port's default. Wiring stays a drag; this is the literal/param path.
export function openPinEditor(pin, anchor) {
  const { node, input } = pin;
  if (node.boundary || node.implicit || node.param || input.append) {
    return;
  }
  if (input.boundBind) {
    setGraphStatus(`'${input.name}' is fed by the interface — edit the ` +
                   'bind at #inputs (§19.2)');
    return;
  }
  const raw = input.arrayOf
      ? node.source.inputs?.[input.arrayOf.base]?.[input.arrayOf.index]
      : node.source.inputs?.[input.name];
  const display = raw === undefined ? ''
      : typeof raw === 'string' ? raw
      : Array.isArray(raw) && raw.every((v) => typeof v === 'number')
          ? raw.join(', ')
      : JSON.stringify(raw);
  openQuickForm({ left: anchor.clientX, bottom: anchor.clientY },
                `${node.id} · ${input.name}`, [
    { key: 'value', label: 'value', value: display },
    { note: 'number, "x, y…" vector, $parameter, or @node.port — empty ' +
            'resets' + (input.def !== undefined
                ? ` to the default ${JSON.stringify(input.def)}` : '') },
  ], (v) => {
    const text = v.value.trim();
    if (text === display) {
      return null; // unchanged
    }
    if (!text) {
      // An explicit reset outranks the disconnect-restores-literal
      // memory: the port really goes back to its default.
      priorBindings.delete(`${node.id}/${input.name}`);
      removeInput(node, input);
      pushFromGraph();
      return null;
    }
    let value;
    if (text[0] === '$') {
      if (inSubgraph()) {
        return '$parameters are not allowed inside a graph file (§19.1)';
      }
      if (!sceneSource.parameters?.[text.slice(1)]) {
        return `unknown parameter '${text}'`;
      }
      value = text;
    } else if (text[0] === '@') {
      value = text; // the loader validates the reference (draft explains)
    } else if (text[0] === '[' || text[0] === '{') {
      try {
        value = JSON.parse(text);
      } catch {
        return 'unparseable JSON';
      }
    } else {
      const parts = text.split(',').map((s) => Number(s.trim()));
      if (!parts.length || parts.some(Number.isNaN)) {
        return 'expected a number, "x, y…", $parameter, or @node.port';
      }
      value = parts.length === 1 ? parts[0] : parts;
    }
    writeInput(node, input, value);
    pushFromGraph();
    return null;
  });
}

export function applyWireDrop(drag, pin) {
  const compatible = (a, b) => a == null || b == null || a === b;
  // §19.5 boundary edits write the interface tables, not node inputs.
  if (drag.bindName || drag.bindNew) {
    applyBindDrop(drag, pin);
    return;
  }
  if (drag.exportName || drag.exportNew) {
    applyExportDrop(drag, pin);
    return;
  }
  if (drag.seek === 'out') {
    if (!pin || pin.side !== 'out') {
      return; // dropped on nothing: an empty input stays empty
    }
    if (pin.node.id === drag.to.node.id) {
      setGraphStatus('refused: direct self-loop (needs previous: true)');
      return;
    }
    if (!compatible(pin.output[1], drag.to.entry.category)) {
      setGraphStatus(`refused: ${pin.output[1]} → ` +
                     `${drag.to.entry.category} port`);
      return;
    }
    writeInput(drag.to.node, drag.to.entry,
               pin.node.param ? '$' + pin.node.param
                              : refFor(pin.node.id, pin.output[0]));
    pushFromGraph();
    return;
  }
  // seek === 'in': a new wire from an output, or a picked-up existing one.
  const target = pin && pin.side === 'in' ? pin : null;
  if (!target) {
    if (drag.detach) {
      removeInput(drag.detach.node, drag.detach.entry);
      pushFromGraph();
    }
    return;
  }
  if (drag.detach && target.node.id === drag.detach.node.id &&
      target.input.name === drag.detach.entry.name) {
    return; // dropped back where it came from
  }
  if (drag.from && target.node.id === drag.from.node) {
    setGraphStatus('refused: direct self-loop (needs previous: true)');
    return;
  }
  if (drag.from &&
      !compatible(drag.from.category, target.input.category)) {
    setGraphStatus(`refused: ${drag.from.category} → ` +
                   `${target.input.category} port`);
    return;
  }
  const raw = drag.raw ?? (drag.from.param
      ? '$' + drag.from.param : refFor(drag.from.node, drag.from.port));
  const sameArray = drag.detach?.entry.arrayOf &&
      drag.detach.node.id === target.node.id &&
      (target.input.arrayOf?.base ?? target.input.append) ===
          drag.detach.entry.arrayOf.base;
  if (sameArray) {
    // Within one array port the gesture reads as reordering, not replacing.
    const arr = drag.detach.node.source.inputs[drag.detach.entry.arrayOf.base];
    const fromIdx = drag.detach.entry.arrayOf.index;
    arr.splice(fromIdx, 1);
    let toIdx = target.input.append ? arr.length : target.input.arrayOf.index;
    if (!target.input.append && target.input.arrayOf.index > fromIdx) {
      toIdx--;
    }
    arr.splice(toIdx, 0, raw);
  } else {
    if (drag.detach) {
      removeInput(drag.detach.node, drag.detach.entry);
    }
    writeInput(target.node, target.input, raw);
  }
  pushFromGraph();
}

// A bind wire dropped on an inner input port (§19.2 rules client-side;
// the loader stays the authority — a bad edit shows as a draft).
function applyBindDrop(drag, pin) {
  const compatible = (a, b) => a == null || b == null || a === b;
  const doc = activeDoc();
  const target = pin && pin.side === 'in' && !pin.node.boundary &&
      !pin.node.implicit && !pin.node.param ? pin : null;
  const targetRef = target
      ? `@${target.node.id}.${target.input.name}` : null;
  const writable = target && !target.input.arrayOf && !target.input.append &&
      target.node.source.inputs?.[target.input.name] === undefined &&
      !Object.entries(doc.inputs ?? {}).some(([name, decl]) =>
          name !== drag.bindName &&
          [].concat(decl?.bind ?? []).includes(targetRef));
  if (target && !writable) {
    setGraphStatus('refused: that port already has a writer (§19.2)');
    return;
  }
  if (drag.bindNew) {
    if (!target) {
      return; // dropped on nothing: no new input
    }
    const spec = specInputsOf(target.node.source)?.find(
        ([n]) => n === target.input.name);
    const guess = spec?.[1] === 'texture' || spec?.[1] === 'event' ||
            spec?.[1] === 'buffer'
        ? spec[1]
        : Array.isArray(spec?.[3]) ? `vec${spec[3].length}` : 'scalar';
    const rect = graphCanvas.getBoundingClientRect();
    openQuickForm({ left: rect.left + 24, bottom: rect.top + 24 },
                  'new interface input', [
      { key: 'name', label: 'name', value: target.input.name },
      { key: 'type', label: 'type',
        options: ['scalar', 'vec2', 'vec3', 'vec4', 'texture', 'event',
                  'buffer'],
        value: guess },
    ], (v) => {
      if (!isIdentifier(v.name)) {
        return 'name must be an identifier (§3)';
      }
      if (doc.inputs?.[v.name]) {
        return `input '${v.name}' already exists`;
      }
      (doc.inputs ??= {})[v.name] = { type: v.type, bind: targetRef };
      pushFromGraph();
      return null;
    });
    return;
  }
  const decl = doc.inputs?.[drag.bindName];
  if (!decl) {
    return;
  }
  const binds = [].concat(decl.bind ?? []);
  if (drag.detachBind) {
    const at = binds.indexOf(drag.detachBind);
    if (at >= 0) {
      binds.splice(at, 1);
    }
  }
  if (target) {
    if (!compatible(typeCategory(decl.type),
                    target.input.category ?? null)) {
      setGraphStatus(`refused: ${typeCategory(decl.type)} → ` +
                     `${target.input.category} port`);
      return;
    }
    if (!binds.includes(targetRef)) {
      binds.push(targetRef);
    }
  } else if (!drag.detachBind) {
    return; // a fresh bind dropped on nothing: nothing to do
  }
  if (binds.length) {
    decl.bind = binds.length === 1 ? binds[0] : binds;
  } else {
    delete doc.inputs[drag.bindName]; // its last bind left with the drag
  }
  pushFromGraph();
}

// An export mapping dropped on an inner output port (or on nothing:
// deletion, except 'result' which §19.2 requires).
function applyExportDrop(drag, pin) {
  const doc = activeDoc();
  const target = pin && pin.side === 'out' && !pin.node.boundary &&
      !pin.node.implicit && !pin.node.param ? pin : null;
  if (drag.exportNew) {
    if (!target) {
      return;
    }
    const ref = refFor(target.node.id, target.output[0]);
    const rect = graphCanvas.getBoundingClientRect();
    openQuickForm({ left: rect.left + 24, bottom: rect.top + 24 },
                  'new interface output', [
      { key: 'name', label: 'name', value: target.output[0] },
    ], (v) => {
      if (!isIdentifier(v.name)) {
        return 'name must be an identifier (§3)';
      }
      if (doc.outputs?.[v.name]) {
        return `output '${v.name}' already exists`;
      }
      (doc.outputs ??= {})[v.name] = ref;
      pushFromGraph();
      return null;
    });
    return;
  }
  if (!target) {
    if (drag.exportName === 'result') {
      setGraphStatus("'result' is required (§19.2) — retarget it instead");
      return;
    }
    delete doc.outputs[drag.exportName];
    pushFromGraph();
    return;
  }
  doc.outputs[drag.exportName] = refFor(target.node.id, target.output[0]);
  pushFromGraph();
}
