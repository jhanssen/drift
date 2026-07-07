import { G, GRAPH_COLORS } from '../nodedefs.js';
import { FLASH_MS, cueFlashes, laneFlashes } from '../timeline.js';
import { graph, graphView, selectedNodeId, selectedIds, graphDraftIds,
         graphStatusText, graphNeedsFit, setGraphNeedsFit, wireDrag,
         pinY, inputPinPos, outputPinPos } from './state.js';
import { fitGraphView } from './interact.js';
import { graphCanvas } from './canvas.js';

// ---- graph view: drawing ---------------------------------------------------

// Event wires reuse the timeline's flash bookkeeping: manual fires land in
// laneFlashes (node/port), playhead crossings in cueFlashes (node/track/cue).
function edgeFlashed(edge, now) {
  if (now - (laneFlashes.get(`${edge.from}/${edge.fromPort}`) || 0) <
      FLASH_MS) {
    return true;
  }
  const prefix = `${edge.from}/${edge.fromPort}/`;
  for (const [key, at] of cueFlashes) {
    if (now - at < FLASH_MS && key.startsWith(prefix)) {
      return true;
    }
  }
  return false;
}

export function drawGraph() {
  const dpr = devicePixelRatio || 1;
  const cw = graphCanvas.clientWidth, ch = graphCanvas.clientHeight;
  if (!cw || !ch) {
    return;
  }
  if (graphCanvas.width !== Math.round(cw * dpr) ||
      graphCanvas.height !== Math.round(ch * dpr)) {
    graphCanvas.width = Math.round(cw * dpr);
    graphCanvas.height = Math.round(ch * dpr);
  }
  if (graphNeedsFit && graph) {
    fitGraphView();
    setGraphNeedsFit(false);
  }
  const ctx = graphCanvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, cw, ch);
  if (!graph) {
    ctx.fillStyle = '#8a8f98';
    ctx.font = '12px system-ui';
    ctx.textAlign = 'center';
    ctx.fillText('no scene document', cw / 2, ch / 2);
    ctx.textAlign = 'left';
    return;
  }
  const s = graphView.scale;
  ctx.setTransform(dpr * s, 0, 0, dpr * s,
                   dpr * s * graphView.x, dpr * s * graphView.y);

  const now = performance.now();
  for (const edge of graph.edges) {
    const a = outputPinPos(graph.byId.get(edge.from), edge.fromPort);
    const b = inputPinPos(graph.byId.get(edge.to), edge.toPort);
    const flash = edge.category === 'event' && edgeFlashed(edge, now);
    ctx.strokeStyle = flash ? '#ffffff' : GRAPH_COLORS[edge.category];
    ctx.lineWidth = (edge.category === 'texture' ? 2 : 1.4) + (flash ? 1 : 0);
    ctx.setLineDash(edge.previous ? [5, 4] : []);
    const dx = Math.max(48, Math.abs(b.x - a.x) * 0.5);
    ctx.beginPath();
    ctx.moveTo(a.x, a.y);
    ctx.bezierCurveTo(a.x + dx, a.y, b.x - dx, b.y, b.x, b.y);
    ctx.stroke();
  }
  ctx.setLineDash([]);
  ctx.lineWidth = 1;

  const shortLit = (v) => {
    const text = JSON.stringify(v);
    return text.length > 12 ? text.slice(0, 11) + '…' : text;
  };
  // Output pins follow the same contract as inputs: filled only when a
  // wire actually leaves the port in this view.
  const wiredOut = new Set(
      graph.edges.map((e) => `${e.from}\n${e.fromPort}`));
  for (const node of graph.nodes) {
    const selected = node.id === selectedNodeId || selectedIds.has(node.id);
    const draft = graphDraftIds.has(node.id);
    ctx.beginPath();
    ctx.roundRect(node.x, node.y, node.w, node.h, node.param ? 12 : 6);
    ctx.fillStyle = '#1e2128';
    ctx.fill();
    ctx.strokeStyle = draft ? '#e3c56d' : selected ? '#6aa1ff' : '#2c3038';
    ctx.lineWidth = selected || draft ? 1.5 : 1;
    ctx.stroke();
    ctx.lineWidth = 1;

    ctx.save();
    ctx.beginPath();
    ctx.rect(node.x + 1, node.y + 1, node.w - 2, node.h - 2);
    ctx.clip();
    ctx.fillStyle = node.param ? '#7fd490'
                  : node.implicit ? '#8a8f98' : '#d6d9de';
    ctx.font = '11px system-ui';
    ctx.fillText(node.id, node.x + 8, node.y + 15);
    ctx.fillStyle = '#8a8f98';
    ctx.font = '9px system-ui';
    ctx.textAlign = 'right';
    ctx.fillText(node.typeLabel ?? node.type, node.x + node.w - 8,
                 node.y + 15);
    ctx.textAlign = 'left';
    ctx.strokeStyle = '#2c3038';
    ctx.beginPath();
    ctx.moveTo(node.x, node.y + G.HEAD - 3);
    ctx.lineTo(node.x + node.w, node.y + G.HEAD - 3);
    ctx.stroke();

    node.inputs.forEach((input, i) => {
      const y = pinY(node, i);
      ctx.fillStyle = (input.unbound && !input.boundFed) || input.append
          ? '#8a8f98' : '#c6cad1';
      ctx.fillText(input.name, node.x + 8, y + 3);
      const binding = input.param ? (input.paramEdge ? null : '$' + input.param)
                    : input.literal !== undefined ? shortLit(input.literal)
                    : input.unbound && !input.boundFed &&
                      input.def !== undefined
                        ? shortLit(input.def)
                    : null;
      if (binding) {
        ctx.fillStyle = '#8a8f98';
        ctx.fillText(binding,
                     node.x + 10 + ctx.measureText(input.name).width, y + 3);
      }
    });
    if (node.param) {
      // The one row shows the declared default; the panel owns live values.
      ctx.fillStyle = '#8a8f98';
      ctx.fillText(shortLit(node.decl.default), node.x + 8, pinY(node, 0) + 3);
    }
    ctx.textAlign = 'right';
    node.outputs.forEach(([name, category], i) => {
      ctx.fillStyle = node.dimPorts?.has(name) ? '#8a8f98'
                                               : GRAPH_COLORS[category];
      ctx.fillText(name, node.x + node.w - 8, pinY(node, i) + 3);
    });
    ctx.textAlign = 'left';
    ctx.restore();

    // Pins: filled = a wire terminates or leaves here, hollow = wireable
    // but carrying a literal, a parameter, or nothing; hollow rings take
    // the port's category color when the spec knows it.
    node.inputs.forEach((input, i) => {
      const y = pinY(node, i);
      ctx.beginPath();
      ctx.arc(node.x, y, 3, 0, Math.PI * 2);
      if (input.conn || input.paramEdge || input.boundFed) {
        ctx.fillStyle = GRAPH_COLORS[input.category ?? 'value'];
        ctx.fill();
      } else {
        ctx.strokeStyle = GRAPH_COLORS[input.category] ?? '#8a8f98';
        ctx.stroke();
      }
    });
    node.outputs.forEach(([name, category], i) => {
      ctx.beginPath();
      ctx.arc(node.x + node.w, pinY(node, i), 3, 0, Math.PI * 2);
      if (node.dimPorts?.has(name) ||
          !wiredOut.has(`${node.id}\n${name}`)) {
        ctx.strokeStyle = GRAPH_COLORS[category] ?? '#8a8f98';
        ctx.stroke();
      } else {
        ctx.fillStyle = GRAPH_COLORS[category];
        ctx.fill();
      }
    });
  }

  if (graphStatusText) {
    // Screen-space banner: the chip in the tab bar is easy to miss, and a
    // draft's most surprising symptom is "my edit changed nothing".
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.font = '11px system-ui';
    const note = graphStatusText.startsWith('draft')
        ? `⚠ ${graphStatusText} — runtimes keep playing the last valid scene`
        : `⚠ ${graphStatusText}`;
    const w = Math.min(ctx.measureText(note).width + 20, cw - 20);
    ctx.beginPath();
    ctx.roundRect(10, 10, w, 24, 5);
    ctx.fillStyle = 'rgba(30, 33, 40, .92)';
    ctx.fill();
    ctx.strokeStyle = '#e3c56d';
    ctx.stroke();
    ctx.save();
    ctx.clip();
    ctx.fillStyle = '#e3c56d';
    ctx.fillText(note, 20, 26);
    ctx.restore();
    ctx.setTransform(dpr * s, 0, 0, dpr * s,
                     dpr * s * graphView.x, dpr * s * graphView.y);
  }

  if (wireDrag?.cursor) {
    // Ghost wire while dragging: from the producer pin (or the seeking
    // input pin) to the cursor.
    const from = wireDrag.from && graph.byId.get(wireDrag.from.node);
    const a = wireDrag.seek === 'in'
        ? (from ? outputPinPos(from, wireDrag.from.port) : wireDrag.cursor)
        : inputPinPos(wireDrag.to.node, wireDrag.to.entry.name);
    const b = wireDrag.cursor;
    ctx.strokeStyle = '#d6d9de';
    ctx.lineWidth = 1.4;
    ctx.setLineDash([4, 3]);
    const dx = Math.max(48, Math.abs(b.x - a.x) * 0.5);
    const dir = wireDrag.seek === 'in' ? 1 : -1;
    ctx.beginPath();
    ctx.moveTo(a.x, a.y);
    ctx.bezierCurveTo(a.x + dir * dx, a.y, b.x - dir * dx, b.y, b.x, b.y);
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.lineWidth = 1;
  }
}
