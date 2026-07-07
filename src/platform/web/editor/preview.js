// ---- wasm preview runtime (same artifacts as drift.html) ------------------

import { query, sceneName } from './config.js';
import { setStatus } from './ui.js';
import { populateScenes, createProject } from './project.js';
import { refreshPanel } from './params.js';
import { fetchSource } from './document.js';
import { syncClock } from './live.js';

export let wasm = null;          // { describe(), set(name, values) }
export let wasmManifest = null;  // parsed describe once the scene is up

export const Module = {
  canvas: document.getElementById('canvas'),
  arguments: [sceneName],
  onRuntimeInitialized() {
    const describe = Module.cwrap('drift_describe', 'string', []);
    const set = Module.cwrap('drift_set_parameter', 'number',
        ['string', 'number', 'number', 'number', 'number', 'number']);
    const time = Module.cwrap('drift_time', 'number', []);
    const seek = Module.cwrap('drift_seek', 'number', ['number']);
    const pause = Module.cwrap('drift_pause', null, ['number']);
    const reload = Module.cwrap('drift_reload', 'number', []);
    const source = Module.cwrap('drift_source', 'string', []);
    const load = Module.cwrap('drift_load', 'number', ['string']);
    const fire = Module.cwrap('drift_fire', 'number', ['string', 'string']);
    const errors = Module.cwrap('drift_errors', 'string', []);
    const writeAsset = Module.cwrap('drift_write_asset', 'number',
        ['string', 'string', 'string']);
    const readAsset = Module.cwrap('drift_read_asset', 'string', ['string']);
    const open = Module.cwrap('drift_open', 'number', ['string']);
    const reflect = Module.cwrap('drift_reflect', 'string', ['string']);
    const nodeProps = Module.cwrap('drift_node_props', 'string', []);
    const nodePorts = Module.cwrap('drift_node_ports', 'string', []);
    const scenes = Module.cwrap('drift_scenes', 'string', []);
    const graphs = Module.cwrap('drift_graphs', 'string', []);
    wasm = {
      describe,
      reflect,
      nodeProps,
      nodePorts,
      scenes,
      graphs,
      time,
      seek,
      pause,
      reload,
      source,
      load,
      fire,
      errors,
      writeAsset,
      readAsset,
      open,
      set(name, v) {
        return set(name, v[0] ?? 0, v[1] ?? 0, v[2] ?? 0, v[3] ?? 0, v.length);
      },
    };
    populateScenes(); // MEMFS is mounted; no need to wait for the scene
    // The scene loads after the async adapter/device dance; poll briefly.
    const started = Date.now();
    const timer = setInterval(() => {
      const info = JSON.parse(wasm.describe());
      if (info.scene) {
        clearInterval(timer);
        wasmManifest = info;
        setStatus('wasmStatus', `preview: ${info.scene}`, 'ok');
        console.log(`editor: wasm scene '${info.scene}'`);
        document.getElementById('newBtn').disabled = false;
        document.getElementById('openBtn').disabled = false;
        document.getElementById('saveBtn').disabled = false;
        document.getElementById('revertBtn').disabled = false;
        refreshPanel();
        fetchSource();
        // The live connection may have won the race; sync now rather than
        // waiting for the periodic check.
        syncClock(/*force=*/true);
        if (query.get('scratch')) {
          createProject('untitled', 'opfs');
        }
      } else if (wasm.errors()) {
        // The scene failed to load (e.g. an asset this browser cannot
        // read); the editor still works live-only.
        clearInterval(timer);
        const reason = wasm.errors().split('\n')[0];
        setStatus('wasmStatus', `preview: ${reason}`, 'warn');
        console.log(`editor: wasm preview unavailable: ${reason}`);
      } else if (Date.now() - started > 8000) {
        clearInterval(timer);
        setStatus('wasmStatus', 'preview: unavailable (WebGPU?)', 'bad');
        console.log('editor: wasm preview unavailable');
      }
    }, 200);
  },
};

// Cross-module writes go through a setter: imported bindings are read-only.
export function setWasmManifest(manifest) {
  wasmManifest = manifest;
}

// drift.js (a classic script) reads the global Module when it runs; inject
// it only now that the object exists — a static <script async> tag could
// execute before this module set it up.
window.Module = Module;
const runtimeScript = document.createElement('script');
runtimeScript.src = 'drift.js';
document.head.appendChild(runtimeScript);
