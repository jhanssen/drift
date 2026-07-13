// ---- node preview drill-in (double-click a texture node) -------------------
// A preview is a viewStack entry like a subgraph view — same back button,
// same breadcrumb — whose pane covers the graph canvas while it is on top.
// The image is the runtime's own output for that node (drift_preview_*):
// re-tapped whenever the node's texture revision changes, so video and
// animated shaders play live.

import { wasm } from '../preview.js';
import { viewStack, activeDoc, updateGraphChrome, setSelectedNodeId,
         selectedIds, setGraphStatus, setSyncAuxPane } from './state.js';
import { outputsOf } from './model.js';

const REVISION_POLL_MS = 100;

let pane = null;    // { root, canvas, caption }
let polling = null; // { runtimeId, revision, timer, inFlight }

export function previewableNode(node) {
  return outputsOf(node.source).some(([, category]) => category === 'texture');
}

// The runtime id of a node shown in the current view: flattening prefixes
// subgraph-inner ids with their instance chain (instance/inner). Preview
// entries never nest, so only subgraph entries contribute. null = the view
// was not entered through an instance (nothing runs under this path).
export function runtimeIdOf(nodeId) {
  const parts = [];
  for (const view of viewStack) {
    if (view.kind === 'preview') {
      continue;
    }
    if (!view.viaInstance) {
      return null;
    }
    parts.push(view.viaInstance);
  }
  parts.push(nodeId);
  return parts.join('/');
}

export function enterNodePreview(node) {
  if (!wasm) {
    setGraphStatus('node previews need the preview runtime');
    return;
  }
  const runtimeId = runtimeIdOf(node.id);
  if (runtimeId === null) {
    setGraphStatus('no running instance for this graph view');
    return;
  }
  const top = viewStack[viewStack.length - 1];
  // doc/path mirror the view beneath so stack readers (activeDoc,
  // viewReadOnly) keep working; kind drives the chrome and this pane.
  viewStack.push({ kind: 'preview', nodeId: node.id, runtimeId,
                   doc: activeDoc(), path: top?.path ?? '' });
  setSelectedNodeId(null);
  selectedIds.clear();
  updateGraphChrome();
}

function ensurePane() {
  if (pane) {
    return pane;
  }
  const root = document.createElement('div');
  root.id = 'nodePreviewPane';
  root.hidden = true;
  const canvas = document.createElement('canvas');
  const caption = document.createElement('div');
  caption.className = 'caption';
  root.appendChild(canvas);
  root.appendChild(caption);
  document.getElementById('viewStack').appendChild(root);
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && !root.hidden) {
      document.getElementById('backBtn').click();
    }
  });
  pane = { root, canvas, caption };
  return pane;
}

// One tap; re-arms on revision change. A -1 revision (node not running or
// no texture yet) keeps polling so the preview appears once the graph
// first evaluates the node.
async function refresh() {
  const p = polling;
  if (!p || p.inFlight) {
    return;
  }
  const revision = wasm.nodeRevision(p.runtimeId);
  if (revision === p.revision) {
    return;
  }
  p.inFlight = true;
  const rect = pane.root.getBoundingClientRect();
  const scale = window.devicePixelRatio || 1;
  const image = await wasm.nodePreview(
      p.runtimeId, Math.max(64, Math.round(rect.width * scale)),
      Math.max(64, Math.round((rect.height - 28) * scale)));
  if (polling !== p) {
    return; // pane closed or retargeted while tapping
  }
  p.inFlight = false;
  if (!image) {
    pane.caption.textContent = revision < 0
        ? `${p.runtimeId} — not running in the preview scene`
        : `${p.runtimeId} — no output yet`;
    p.revision = revision;
    return;
  }
  pane.canvas.width = image.width;
  pane.canvas.height = image.height;
  pane.canvas.getContext('2d').putImageData(image, 0, 0);
  pane.caption.textContent =
      `${p.runtimeId} · ${image.width}×${image.height} · live`;
  p.revision = revision;
}

// Chrome hook: shows/hides the pane to match the stack top and starts or
// stops the revision polling with it.
function syncPane() {
  const top = viewStack[viewStack.length - 1];
  const show = document.body.classList.contains('graph-mode') &&
               top?.kind === 'preview';
  const { root, canvas, caption } = ensurePane();
  root.hidden = !show;
  if (!show) {
    if (polling) {
      clearInterval(polling.timer);
      polling = null;
    }
    return;
  }
  if (polling?.runtimeId === top.runtimeId) {
    return;
  }
  if (polling) {
    clearInterval(polling.timer);
  }
  canvas.width = canvas.height = 0;
  caption.textContent = `${top.runtimeId} · …`;
  polling = { runtimeId: top.runtimeId, revision: NaN, inFlight: false,
              timer: setInterval(refresh, REVISION_POLL_MS) };
  refresh();
}

setSyncAuxPane(syncPane);

// ---- inspector thumbnail ----------------------------------------------------
// A small always-there preview while a texture node is selected; same tap,
// lighter cadence. Remounted (and the old poll stopped) on every inspector
// render; mount(null) stops it outright.

const THUMB_POLL_MS = 200;
const THUMB_MAX_W = 208, THUMB_MAX_H = 128;

let thumb = null; // { runtimeId, revision, timer, canvas, inFlight }

export function mountInspectorThumb(container, node) {
  if (thumb) {
    clearInterval(thumb.timer);
    thumb = null;
  }
  if (!container || !node || !wasm || !previewableNode(node)) {
    return;
  }
  const runtimeId = runtimeIdOf(node.id);
  if (runtimeId === null) {
    return;
  }
  const canvas = document.createElement('canvas');
  canvas.className = 'nodeThumb';
  canvas.title = 'live node output (double-click the node for a large view)';
  container.appendChild(canvas);
  const t = { runtimeId, revision: NaN, canvas, inFlight: false, timer: 0 };
  const refreshThumb = async () => {
    if (thumb !== t || t.inFlight || !canvas.isConnected) {
      return;
    }
    const revision = wasm.nodeRevision(t.runtimeId);
    if (revision === t.revision) {
      return;
    }
    t.inFlight = true;
    const image = await wasm.nodePreview(t.runtimeId, THUMB_MAX_W,
                                         THUMB_MAX_H);
    t.inFlight = false;
    if (thumb !== t || !image) {
      return;
    }
    canvas.width = image.width;
    canvas.height = image.height;
    canvas.getContext('2d').putImageData(image, 0, 0);
    t.revision = revision;
  };
  t.timer = setInterval(refreshThumb, THUMB_POLL_MS);
  thumb = t;
  refreshThumb();
}
