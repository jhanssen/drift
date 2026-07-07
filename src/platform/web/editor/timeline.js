import { wasm, wasmManifest, setWasmManifest } from './preview.js';
import { openQuickForm } from './ui.js';
import { activeManifest, refreshPanel } from './params.js';
import { sceneSource, setSceneSource, pushDocument,
         pushFromGraph } from './document.js';
import { seekBoth, fireBoth } from './transport.js';
import { graph, selectedNodeId, setSelectedNodeId } from './graph/state.js';
import { drawGraph } from './graph/draw.js';
import { renderInspector } from './graph/inspector.js';

// duration/loop are node properties (§9.9); edits push the whole document.
// Shrinking below existing keys/cues is a §13 load error — the push is
// rejected and the draft banner names the node until the times are fixed.
function editSequenceProps(seq, anchor) {
  openQuickForm(anchor, `${seq.id} properties`, [
    { key: 'duration', label: 'duration', value: String(seq.duration) },
    { key: 'loop', label: 'loop', options: ['on', 'off'],
      value: seq.loop ? 'on' : 'off' },
  ], (v) => {
    const duration = parseFloat(v.duration);
    if (!(duration > 0)) {
      return 'duration must be a number > 0 (seconds)';
    }
    const node = sceneSource?.nodes.find((n) => n.id === seq.id);
    if (!node) {
      return `node '${seq.id}' is not in the document`;
    }
    node.duration = duration;
    node.loop = v.loop === 'on';
    pushFromGraph();
    return null;
  });
}

// Track add/remove (the timeline owns tracks, §16.1). New tracks get a
// minimal valid shape (§13 requires one key/cue); shape it by editing.
function addTrack(seq, anchor) {
  openQuickForm(anchor, `add track to ${seq.id}`, [
    { key: 'name', label: 'name', value: '' },
    { key: 'kind', label: 'kind', options: ['value', 'event'] },
  ], (v) => {
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(v.name)) {
      return 'name must be an identifier (§3)';
    }
    if (seq.tracks.some((t) => t.name === v.name)) {
      return `track '${v.name}' already exists`;
    }
    if (v.kind === 'event') {
      seq.tracks.push({ name: v.name, kind: 'event', fires: [0] });
    } else {
      seq.tracks.push({ name: v.name, kind: 'value', type: 'scalar',
                        interpolate: 'hold', keys: [{ t: 0, value: 0 }] });
    }
    pushDocument().then(refreshPanel);
    return null;
  });
}

function removeTrack(seq, lane) {
  if (seq.tracks.length <= 1) {
    alert('a sequence needs at least one track (§13)');
    return;
  }
  const track = seq.tracks[lane];
  if (!confirm(`delete track '${track.name}'? (references to it will ` +
               'fail validation and the push will be rejected)')) {
    return;
  }
  seq.tracks.splice(lane, 1);
  pushDocument().then(() => {
    // A rejected push (something still references the track) leaves the
    // runtimes on the old scene; resync the panel to reality.
    if (wasm && wasm.errors()) {
      alert(`delete rejected:\n${wasm.errors()}`);
      setWasmManifest(JSON.parse(wasm.describe()));
      setSceneSource(JSON.parse(wasm.source()));
    }
    refreshPanel();
  });
}

// ---- timeline panel (§16.5 editor mapping; read-only in v0) --------------

const LANE_COLORS = ['#6aa1ff', '#7fd490', '#e3c56d', '#e0787d'];
const RULER_H = 18, LANE_H = 42, GUTTER_W = 96;
export const FLASH_MS = 300;
let timelines = []; // { seq, canvas }
// Fires are one-frame phenomena; flashes are how the UI shows them.
export const cueFlashes = new Map();  // seqId/track/cueIdx -> performance.now()
export const laneFlashes = new Map(); // node/port -> performance.now()

// The timeline shows one sequence at a time: the active one, synced from
// the graph selection (§16.1's one-panel-per-node, minus the stacking).
// With several sequence nodes a tab row switches between them.
export let activeSequenceId = null;

export function refreshTimeline() {
  const container = document.getElementById('timeline');
  const seqs = activeManifest()?.sequences ?? [];
  container.textContent = '';
  timelines = [];
  if (!seqs.length) {
    container.innerHTML =
        '<span class="dim">no sequence nodes in this scene</span>';
    return;
  }
  if (!seqs.some((s) => s.id === activeSequenceId)) {
    activeSequenceId = seqs[0].id;
  }
  if (seqs.length > 1) {
    const tabs = document.createElement('div');
    tabs.id = 'seqTabs';
    for (const s of seqs) {
      const tab = document.createElement('button');
      tab.textContent = s.id;
      tab.className = s.id === activeSequenceId ? 'active' : '';
      tab.onclick = () => {
        activeSequenceId = s.id;
        if (graph?.byId.has(s.id)) {
          setSelectedNodeId(s.id); // the tab and the graph agree on focus
        }
        renderInspector();
        refreshTimeline();
      };
      tabs.appendChild(tab);
    }
    container.appendChild(tabs);
  }
  // A selected-but-unwired sequence is pruned by the loader (§8) and has
  // no runtime timeline; say so instead of silently showing another one.
  const selected = graph?.byId.get(selectedNodeId);
  if (selected?.source?.type === 'sequence' &&
      !seqs.some((s) => s.id === selected.id)) {
    const note = document.createElement('div');
    note.className = 'dim';
    note.textContent = `sequence '${selected.id}' is not wired into the ` +
        'graph, so it never runs and has no timeline yet — wire one of ' +
        'its outputs first';
    container.appendChild(note);
  }
  for (const seq of seqs.filter((s) => s.id === activeSequenceId)) {
    const header = document.createElement('h3');
    header.textContent =
        `${seq.id} — ${seq.duration}s${seq.loop ? ' · loop' : ''} `;
    const buttonCss = 'font-size:10px;padding:1px 6px;margin-left:8px';
    const editBtn = document.createElement('button');
    editBtn.textContent = '✎';
    editBtn.title = 'duration / loop';
    editBtn.style.cssText = buttonCss;
    editBtn.onclick = (e) =>
        editSequenceProps(seq, e.target.getBoundingClientRect());
    header.appendChild(editBtn);
    const addBtn = document.createElement('button');
    addBtn.textContent = '+ track';
    addBtn.style.cssText = buttonCss;
    addBtn.onclick = (e) => addTrack(seq, e.target.getBoundingClientRect());
    header.appendChild(addBtn);
    const canvas = document.createElement('canvas');
    const tl = { seq, canvas, hits: [], laneRanges: [] };
    canvas.onmousedown = (e) => {
      const inBox = (b) => e.offsetX >= b.x0 && e.offsetX <= b.x1 &&
                           e.offsetY >= b.y0 && e.offsetY <= b.y1;
      const del = (tl.deleteButtons || []).find(inBox);
      if (del) {
        removeTrack(seq, del.lane);
        suppressClick = true;
        e.preventDefault();
        return;
      }
      const button = (tl.fireButtons || []).find(inBox);
      if (button) {
        fireBoth(seq.id, seq.tracks[button.lane].name);
        suppressClick = true;
        e.preventDefault();
        return;
      }
      const hit = tl.hits.find((h) => Math.abs(h.x - e.offsetX) < 7 &&
                                      Math.abs(h.y - e.offsetY) < 7);
      if (!hit) {
        return;
      }
      dragState = { tl, lane: hit.lane, keyIdx: hit.keyIdx, comp: hit.comp,
                    lastX: e.clientX, lastY: e.clientY, moved: false };
      suppressClick = true;
      e.preventDefault();
    };
    canvas.addEventListener('wheel', (e) => {
      // Zoom a lane's value scale around the value under the cursor;
      // over the name gutter the wheel still scrolls the strip.
      if (e.offsetX < GUTTER_W) {
        return;
      }
      const lane = Math.floor((e.offsetY - RULER_H) / LANE_H);
      if (lane < 0 || lane >= seq.tracks.length) {
        return;
      }
      e.preventDefault();
      const range = laneRangeMemory.get(
          `${activeManifest()?.scene}/${seq.id}/${seq.tracks[lane].name}`);
      const geom = tl.laneRanges[lane];
      if (!range || !geom) {
        return;
      }
      const span = range.vmax - range.vmin;
      const factor = e.deltaY > 0 ? 1.2 : 1 / 1.2;
      if ((span < 1e-6 && factor < 1) || (span > 1e6 && factor > 1)) {
        return;
      }
      const f = (geom.bottom - e.offsetY) / (geom.bottom - geom.top);
      const pivot = range.vmin + f * span;
      range.vmin = pivot + (range.vmin - pivot) * factor;
      range.vmax = pivot + (range.vmax - pivot) * factor;
    }, { passive: false });
    canvas.onclick = (e) => {
      if (suppressClick) {
        suppressClick = false;
        return;
      }
      const plotW = canvas.clientWidth - GUTTER_W;
      const t = ((e.offsetX - GUTTER_W) / plotW) * seq.duration;
      seekBoth(Math.min(Math.max(t, 0), seq.duration));
    };
    canvas.ondblclick = (e) => {
      const lane = Math.floor((e.offsetY - RULER_H) / LANE_H);
      if (lane < 0 || lane >= seq.tracks.length) {
        return;
      }
      const track = seq.tracks[lane];
      if (e.offsetX < GUTTER_W) {
        // Gutter: re-fit this lane's scale to its current keys.
        laneRangeMemory.delete(
            `${activeManifest()?.scene}/${seq.id}/${track.name}`);
        return;
      }
      // Plot area: insert a cue / a key on the current curve at that time.
      const plotW = canvas.clientWidth - GUTTER_W;
      const t = Math.min(Math.max(
          ((e.offsetX - GUTTER_W) / plotW) * seq.duration, 0), seq.duration);
      if (track.kind === 'event') {
        const cue = Math.min(t, seq.duration - 0.01);
        if (track.fires.some((f) => Math.abs(f - cue) < 0.02)) {
          return;
        }
        track.fires.push(+cue.toFixed(3));
        track.fires.sort((a, b) => a - b);
      } else {
        if (track.keys.some((k) => Math.abs(k.t - t) < 0.02)) {
          return;
        }
        const value = sampleTrack(track, t);
        track.keys.push({
          t: +t.toFixed(3),
          value: value.length === 1 ? value[0] : value,
        });
        track.keys.sort((a, b) => a.t - b.t);
      }
      pushDocument();
    };
    container.appendChild(header);
    container.appendChild(canvas);
    timelines.push(tl);
  }
  console.log(`editor: timeline: ${seqs.length} sequence(s)`);
}

// Track sampling mirrors core semantics (§9.9): hold/linear/smooth, first
// key's value before it, last key's after, no wrap interpolation.
function sampleTrack(track, t) {
  const keys = track.keys;
  let k = 0;
  while (k + 1 < keys.length && keys[k + 1].t <= t) {
    k++;
  }
  const a = [].concat(keys[k].value);
  if (track.interpolate === 'hold' || k + 1 >= keys.length ||
      t <= keys[k].t) {
    return a;
  }
  const b = [].concat(keys[k + 1].value);
  let f = (t - keys[k].t) / (keys[k + 1].t - keys[k].t);
  if (track.interpolate === 'smooth') {
    f = f * f * (3 - 2 * f);
  }
  return a.map((v, i) => v + (b[i] - v) * f);
}

// Key dragging: t moves between neighboring keys; the value follows the
// vertical axis using the lane range frozen at grab time (so the lane
// rescaling under the edit doesn't warp the mapping). Pushed on release.
let dragState = null;
let suppressClick = false;

// Lane scales are fitted ONCE from the authored keys and then left alone —
// any fit-to-data policy pins the data's max at the lane top, so dragging a
// key up would always snap it back to the same pixel. Edited values beyond
// the original range draw outside the lane band; double-click a lane to
// re-fit its scale. Keyed per scene/sequence/track.
const laneRangeMemory = new Map();

// Incremental deltas (not absolute cursor mapping): no jump on grab, and
// it composes with mid-drag lane zooming. Shift = 10x finer.
window.addEventListener('mousemove', (e) => {
  if (!dragState) {
    return;
  }
  const { tl, lane, keyIdx, comp } = dragState;
  const seq = tl.seq;
  const track = seq.tracks[lane];
  const geom = tl.laneRanges[lane];
  const fine = e.shiftKey ? 0.1 : 1;
  const dx = e.clientX - dragState.lastX;
  const dy = e.clientY - dragState.lastY;
  dragState.lastX = e.clientX;
  dragState.lastY = e.clientY;

  const plotW = tl.canvas.clientWidth - GUTTER_W;
  const dt = (dx / plotW) * seq.duration * fine;

  if (comp < 0) {
    // Event cue: time only, ascending, within [0, duration).
    const fires = track.fires;
    const lo = keyIdx > 0 ? fires[keyIdx - 1] + 0.01 : 0;
    const hi = keyIdx < fires.length - 1 ? fires[keyIdx + 1] - 0.01
                                         : seq.duration - 0.01;
    fires[keyIdx] = Math.min(Math.max(fires[keyIdx] + dt, lo),
                             Math.max(lo, hi));
    dragState.moved = true;
    return;
  }

  const keys = track.keys;
  const t = keys[keyIdx].t + dt;
  const lo = keyIdx > 0 ? keys[keyIdx - 1].t + 0.01 : 0;
  const hi = keyIdx < keys.length - 1 ? keys[keyIdx + 1].t - 0.01
                                      : seq.duration;
  keys[keyIdx].t = Math.min(Math.max(t, lo), Math.max(lo, hi));

  const dv = (-dy / (geom.bottom - geom.top)) *
             (geom.vmax - geom.vmin) * fine;
  if (Array.isArray(keys[keyIdx].value)) {
    keys[keyIdx].value[comp] += dv;
  } else {
    keys[keyIdx].value += dv;
  }
  dragState.moved = true;
});

window.addEventListener('mouseup', (e) => {
  if (dragState) {
    if (dragState.moved) {
      pushDocument();
    } else {
      // A click on a key (no movement): type the values instead.
      const { tl, lane, keyIdx, comp } = dragState;
      openKeyEditor(tl, lane, keyIdx, comp, e.clientX, e.clientY);
    }
    dragState = null;
  }
});

// ---- numeric key editor (click a key) -------------------------------------

const keyEdit = document.getElementById('keyEdit');
let keyEditState = null;

function openKeyEditor(tl, lane, keyIdx, comp, screenX, screenY) {
  const track = tl.seq.tracks[lane];
  const isCue = comp < 0;
  keyEditState = { tl, lane, keyIdx, comp };
  document.getElementById('keyEditT').value = isCue
      ? +track.fires[keyIdx].toFixed(3)
      : +track.keys[keyIdx].t.toFixed(3);
  const valueInput = document.getElementById('keyEditV');
  valueInput.parentElement.hidden = isCue; // cues have no value
  if (!isCue) {
    valueInput.value =
        +[].concat(track.keys[keyIdx].value)[comp].toFixed(4);
  }
  keyEdit.hidden = false;
  keyEdit.style.left =
      Math.min(screenX + 10, innerWidth - 200) + 'px';
  keyEdit.style.top = Math.max(screenY - 44, 4) + 'px';
  const focusInput = isCue ? document.getElementById('keyEditT') : valueInput;
  focusInput.focus();
  focusInput.select();
}

function closeKeyEditor(apply) {
  if (keyEditState && apply) {
    const { tl, lane, keyIdx, comp } = keyEditState;
    const track = tl.seq.tracks[lane];
    const t = parseFloat(document.getElementById('keyEditT').value);
    if (comp < 0) {
      if (!Number.isNaN(t)) {
        const fires = track.fires;
        const lo = keyIdx > 0 ? fires[keyIdx - 1] + 0.01 : 0;
        const hi = keyIdx < fires.length - 1 ? fires[keyIdx + 1] - 0.01
                                             : tl.seq.duration - 0.01;
        fires[keyIdx] = Math.min(Math.max(t, lo), Math.max(lo, hi));
      }
    } else {
      const keys = track.keys;
      const key = keys[keyIdx];
      const v = parseFloat(document.getElementById('keyEditV').value);
      if (!Number.isNaN(t)) {
        const lo = keyIdx > 0 ? keys[keyIdx - 1].t + 0.01 : 0;
        const hi = keyIdx < keys.length - 1 ? keys[keyIdx + 1].t - 0.01
                                            : tl.seq.duration;
        key.t = Math.min(Math.max(t, lo), Math.max(lo, hi));
      }
      if (!Number.isNaN(v)) {
        if (Array.isArray(key.value)) {
          key.value[comp] = v;
        } else {
          key.value = v;
        }
      }
    }
    pushDocument();
  }
  keyEditState = null;
  keyEdit.hidden = true;
}

document.getElementById('keyEditDel').onclick = () => {
  if (!keyEditState) {
    return;
  }
  const { tl, lane, keyIdx, comp } = keyEditState;
  const track = tl.seq.tracks[lane];
  const list = comp < 0 ? track.fires : track.keys;
  if (list.length <= 1) {
    console.log('editor: cannot delete the last key/cue (§13 requires one)');
    return;
  }
  list.splice(keyIdx, 1);
  keyEditState = null;
  keyEdit.hidden = true;
  pushDocument();
};

keyEdit.addEventListener('keydown', (e) => {
  if (e.key === 'Enter') {
    closeKeyEditor(true);
  } else if (e.key === 'Escape') {
    closeKeyEditor(false);
  }
});
// Clicking anywhere else dismisses (capture: fires before canvas handlers,
// so clicking another key closes this editor and opens that one).
window.addEventListener('mousedown', (e) => {
  if (keyEditState && !keyEdit.contains(e.target)) {
    closeKeyEditor(false);
  }
}, true);
document.getElementById('timeline').addEventListener('scroll', () => {
  if (keyEditState) {
    closeKeyEditor(false);
  }
});

function drawTimeline(tl, playTime) {
  const { seq, canvas } = tl;
  const dpr = devicePixelRatio || 1;
  const cssW = canvas.clientWidth || canvas.parentElement.clientWidth;
  const cssH = RULER_H + seq.tracks.length * LANE_H;
  canvas.style.height = cssH + 'px';
  if (canvas.width !== Math.round(cssW * dpr) ||
      canvas.height !== Math.round(cssH * dpr)) {
    canvas.width = Math.round(cssW * dpr);
    canvas.height = Math.round(cssH * dpr);
  }
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, cssW, cssH);

  const plotW = cssW - GUTTER_W;
  const xOf = (t) => GUTTER_W + (t / seq.duration) * plotW;

  // Ruler: a tick per second, labels as space allows.
  ctx.font = '10px system-ui';
  ctx.fillStyle = '#8a8f98';
  ctx.strokeStyle = '#2c3038';
  const labelEvery = Math.max(1, Math.ceil(seq.duration / (plotW / 42)));
  for (let s = 0; s <= seq.duration; s++) {
    const x = xOf(s);
    ctx.beginPath();
    ctx.moveTo(x, RULER_H - 5);
    ctx.lineTo(x, cssH);
    ctx.stroke();
    if (s % labelEvery === 0) {
      ctx.fillText(`${s}s`, x + 3, RULER_H - 7);
    }
  }

  tl.hits = [];
  tl.laneRanges = [];
  tl.fireButtons = [];
  tl.deleteButtons = [];
  const now = performance.now();
  seq.tracks.forEach((track, lane) => {
    const top = RULER_H + lane * LANE_H;
    const bottom = top + LANE_H - 8;
    const height = bottom - (top + 8);

    ctx.strokeStyle = '#2c3038';
    ctx.beginPath();
    ctx.moveTo(0, top + LANE_H);
    ctx.lineTo(cssW, top + LANE_H);
    ctx.stroke();
    ctx.fillStyle = '#d6d9de';
    ctx.font = '11px system-ui';
    ctx.fillText(track.name, 8, top + 14);
    ctx.fillStyle = '#8a8f98';
    ctx.font = '9px system-ui';
    ctx.fillText(track.kind === 'event' ? 'event' : track.interpolate,
                 8, top + 26);
    ctx.fillText('✕', GUTTER_W - 14, top + 14);
    tl.deleteButtons.push({ x0: GUTTER_W - 20, y0: top + 2,
                            x1: GUTTER_W - 4, y1: top + 18, lane });

    if (track.kind === 'event') {
      // Cue markers on a center line; drag moves them in time only. The ⚡
      // in the gutter is the "fire now" verb (§16.1); markers flash when
      // the playhead crosses them or a manual fire lands.
      const y = (top + 8 + bottom) / 2;
      tl.laneRanges[lane] = { vmin: 0, vmax: 1, top: top + 8, bottom };
      ctx.fillStyle = '#e3c56d';
      ctx.font = '12px system-ui';
      ctx.fillText('⚡', GUTTER_W - 22, y + 4);
      tl.fireButtons.push({ x0: GUTTER_W - 28, y0: y - 10,
                            x1: GUTTER_W - 8, y1: y + 10, lane });
      ctx.strokeStyle = '#2c3038';
      ctx.beginPath();
      ctx.moveTo(GUTTER_W, y);
      ctx.lineTo(cssW, y);
      ctx.stroke();
      const laneFlash =
          now - (laneFlashes.get(`${seq.id}/${track.name}`) || 0) < FLASH_MS;
      track.fires.forEach((cue, keyIdx) => {
        const flash = laneFlash ||
            now - (cueFlashes.get(`${seq.id}/${track.name}/${keyIdx}`) || 0) <
                FLASH_MS;
        const x = xOf(cue);
        const r = flash ? 8 : 5;
        ctx.fillStyle = flash ? '#ffffff' : '#e3c56d';
        ctx.beginPath();
        ctx.moveTo(x, y - r);
        ctx.lineTo(x + r, y);
        ctx.lineTo(x, y + r);
        ctx.lineTo(x - r, y);
        ctx.fill();
        tl.hits.push({ x, y, lane, keyIdx, comp: -1 });
        if (dragState && dragState.tl === tl && dragState.lane === lane &&
            dragState.keyIdx === keyIdx) {
          ctx.fillStyle = '#d6d9de';
          ctx.font = '10px system-ui';
          ctx.fillText(`${cue.toFixed(2)}s`, x + 8, y - 7);
        }
      });
      return;
    }

    // Stable lane scale: first fit wins (see laneRangeMemory above).
    const memoryKey = `${activeManifest()?.scene}/${seq.id}/${track.name}`;
    let range = laneRangeMemory.get(memoryKey);
    if (!range) {
      const flat = track.keys.flatMap((key) => [].concat(key.value));
      let lo = Math.min(...flat), hi = Math.max(...flat);
      if (lo === hi) {
        lo -= 0.5;
        hi += 0.5;
      }
      const pad = (hi - lo) * 0.12;
      range = { vmin: lo - pad, vmax: hi + pad };
      laneRangeMemory.set(memoryKey, range);
    }
    const { vmin, vmax } = range;
    tl.laneRanges[lane] = { vmin, vmax, top: top + 8, bottom };
    const yOf = (v) => bottom - ((v - vmin) / (vmax - vmin)) * height;

    // The scale itself, so identical shapes at different scales read
    // differently.
    ctx.fillStyle = '#8a8f98';
    ctx.font = '9px system-ui';
    ctx.textAlign = 'right';
    ctx.fillText((+vmax.toFixed(2)).toString(), cssW - 4, top + 14);
    ctx.fillText((+vmin.toFixed(2)).toString(), cssW - 4, bottom);
    ctx.textAlign = 'left';

    const arity = [].concat(track.keys[0].value).length;
    for (let c = 0; c < arity; c++) {
      ctx.strokeStyle = LANE_COLORS[c];
      ctx.beginPath();
      // Sampled polyline reproduces hold steps and smooth curves alike.
      const steps = Math.max(64, plotW / 3);
      for (let i = 0; i <= steps; i++) {
        const t = (i / steps) * seq.duration;
        const y = yOf(sampleTrack(track, t)[c]);
        i ? ctx.lineTo(xOf(t), y) : ctx.moveTo(xOf(t), y);
        if (track.interpolate === 'hold') {
          // Re-sample just past each step edge to keep risers vertical.
          const next = sampleTrack(track, Math.min(t + 1e-9, seq.duration));
          ctx.lineTo(xOf(t), yOf(next[c]));
        }
      }
      ctx.stroke();
      ctx.fillStyle = LANE_COLORS[c];
      track.keys.forEach((key, keyIdx) => {
        const value = [].concat(key.value)[c];
        const x = xOf(key.t), y = yOf(value);
        ctx.beginPath();
        ctx.moveTo(x, y - 4);
        ctx.lineTo(x + 4, y);
        ctx.lineTo(x, y + 4);
        ctx.lineTo(x - 4, y);
        ctx.fill();
        tl.hits.push({ x, y, lane, keyIdx, comp: c });
        if (dragState && dragState.tl === tl && dragState.lane === lane &&
            dragState.keyIdx === keyIdx && dragState.comp === c) {
          ctx.fillStyle = '#d6d9de';
          ctx.font = '10px system-ui';
          ctx.fillText(`${key.t.toFixed(2)}s · ${value.toFixed(2)}`,
                       x + 8, y - 7);
          ctx.fillStyle = LANE_COLORS[c];
        }
      });
    }
  });

  if (playTime !== null) {
    const local = seq.loop
        ? ((playTime % seq.duration) + seq.duration) % seq.duration
        : Math.min(Math.max(playTime, 0), seq.duration);
    ctx.strokeStyle = '#e0787d';
    ctx.beginPath();
    ctx.moveTo(xOf(local), 4);
    ctx.lineTo(xOf(local), cssH);
    ctx.stroke();
  }
}

const seqPrevLocal = new Map(); // seq id -> previous local time

export function tickTimelines() {
  const t = wasm && wasmManifest ? wasm.time() : null;
  document.getElementById('timeText').textContent =
      t !== null ? `${t.toFixed(2)}s` : '—';
  // Flash cues the playhead crossed (mirrors the §9.9 crossing rules) —
  // over every sequence, not just the visible timeline panel, so event
  // wires in the graph keep flashing for sequences whose panel is hidden.
  if (t !== null) {
    for (const seq of activeManifest()?.sequences ?? []) {
      const local = seq.loop
          ? ((t % seq.duration) + seq.duration) % seq.duration
          : Math.min(Math.max(t, 0), seq.duration);
      const prev = seqPrevLocal.get(seq.id);
      if (prev !== undefined && local !== prev) {
        const wrapped = local < prev;
        seq.tracks.forEach((track) => {
          if (track.kind !== 'event') {
            return;
          }
          track.fires.forEach((cue, cueIdx) => {
            const fired = wrapped ? cue > prev || cue <= local
                                  : cue > prev && cue <= local;
            if (fired) {
              cueFlashes.set(`${seq.id}/${track.name}/${cueIdx}`,
                             performance.now());
            }
          });
        });
      }
      seqPrevLocal.set(seq.id, local);
    }
  }
  for (const timeline of timelines) {
    drawTimeline(timeline, t);
  }
  if (document.body.classList.contains('graph-mode')) {
    drawGraph();
  }
  requestAnimationFrame(tickTimelines);
}

export function setActiveSequenceId(id) {
  activeSequenceId = id;
}
