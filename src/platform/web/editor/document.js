import { wasm, setWasmManifest } from './preview.js';
import { live, liveSend } from './live.js';
import { activeManifest, currentValues, refreshPanel } from './params.js';
import { project, projectRoot, projectWriteThrough } from './project.js';
import { viewStack, inSubgraph, setGraphStatus } from './graph/state.js';
import { graphDocs } from './graph/model.js';
import { priorBindings } from './graph/wiring.js';
import { refreshGraph } from './graph/build.js';
import { commitSubgraph } from './graph/subgraphs.js';

// ---- scene document (the editor owns it; runtimes consume) ----------------

export let sceneSource = null; // parsed scene.json of the active target

export async function fetchSource() {
  try {
    const text = live ? (await liveSend('source')).scene
                      : wasm ? wasm.source() : null;
    sceneSource = text ? JSON.parse(text) : null;
  } catch (err) {
    console.log(`editor: source: ${err}`);
    sceneSource = null;
  }
  priorBindings.clear(); // a fresh document invalidates remembered bindings
  viewStack.length = 0;  // ...and any drilled-in subgraph view (§19.5)
  graphDocs.clear();
  refreshGraph(); // callers refresh the panel before the source arrives
}

// Mirrors the edited timeline keys into the document, pushes it to every
// attached runtime (each keeps its old scene if validation fails), and
// re-applies parameter values to the preview (a load resets them).
export async function pushDocument() {
  const manifest = activeManifest();
  if (!sceneSource) {
    return;
  }
  // No manifest (dead preview, not connected) still pushes: wasm.load is
  // what can revive the preview, and its errors feed the draft status.
  for (const seq of manifest?.sequences ?? []) {
    const node = sceneSource.nodes?.find((n) => n.id === seq.id);
    if (!node) {
      continue;
    }
    node.tracks = seq.tracks.map((track) => track.kind === 'event'
      ? {
          ...node.tracks?.find((t) => t.name === track.name),
          name: track.name,
          kind: 'event',
          fires: track.fires.map((t) => +t.toFixed(3)),
        }
      : {
          ...node.tracks?.find((t) => t.name === track.name),
          name: track.name,
          kind: 'value',
          type: track.type,
          interpolate: track.interpolate,
          keys: track.keys.map((k) => ({
            t: +k.t.toFixed(3),
            value: Array.isArray(k.value)
                ? k.value.map((v) => +v.toFixed(4)) : +k.value.toFixed(4),
          })),
        });
  }
  const json = JSON.stringify(sceneSource, null, 2);
  // Provider projects persist continuously: the document lands in the
  // working copy (so reload sees it) and writes through to the handle.
  if (project && wasm) {
    wasm.writeAsset(projectRoot(), 'scene.json', json);
    projectWriteThrough('scene.json', json + '\n');
  }
  const result = { wasmError: null, liveError: null };
  if (wasm) {
    if (wasm.load(json)) {
      for (const [name, values] of currentValues) {
        wasm.set(name, values);
      }
    } else {
      result.wasmError = wasm.errors() || 'load failed';
    }
  }
  if (live) {
    try {
      await liveSend('load', { scene: json });
    } catch (err) {
      result.liveError = String(err);
      console.log(`editor: load rejected: ${err}`);
    }
  }
  console.log('editor: document pushed');
  const error = result.wasmError ?? result.liveError;
  setGraphStatus(error ? `draft — ${error.split('\n')[0]}` : null);
  return result;
}

export function setSceneSource(source) {
  sceneSource = source;
}

// Graph edits land here: push the document (which owns the draft status),
// then re-describe an accepting preview so the timeline and params follow
// along (a live accept re-describes itself via its reload event).
export async function pushFromGraph() {
  if (inSubgraph()) {
    return commitSubgraph(); // §19.5: edits write the graph file instead
  }
  const { wasmError } = await pushDocument() ?? {};
  if (wasm && !wasmError) {
    const manifest = JSON.parse(wasm.describe());
    if (manifest.scene) {
      setWasmManifest(manifest);
    }
  }
  refreshPanel();
}
