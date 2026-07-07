import { wasm, wasmManifest, setWasmManifest } from './preview.js';
import { setStatus } from './ui.js';
import { applyParam, currentValues, refreshPanel } from './params.js';
import { fetchSource } from './document.js';
import { setPaused, previewSeek } from './transport.js';

// ---- live runtime (ControlServer protocol v0) -----------------------------

export let live = null;          // WebSocket while connected
export let liveManifest = null;
let nextId = 1;
let syncTimer = null;
const pending = new Map();

// Preview adopts the live scene clock (always a forward snap for a fresh
// page; if the preview somehow leads — e.g. the live app was occluded and
// paused — drift_seek reloads the preview instance, honoring §9.7).
export async function syncClock(force = false) {
  if (!live || !wasm || !wasmManifest) {
    return;
  }
  try {
    const { seconds } = await liveSend('time');
    const drift = wasm.time() - seconds;
    if (!force && Math.abs(drift) < 0.12) {
      return;
    }
    if (wasm.seek(seconds)) {
      for (const [name, values] of currentValues) {
        wasm.set(name, values); // reload reset them to scene defaults
      }
    }
    console.log(`editor: preview clock -> live ` +
                `(${seconds.toFixed(2)}s, drift ${drift.toFixed(2)}s)`);
  } catch (err) {
    console.log(`editor: clock sync: ${err}`);
  }
}

export function liveSend(method, params) {
  return new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, { resolve, reject });
    live.send(JSON.stringify(params ? { id, method, params }
                                    : { id, method }));
  });
}

export function connectLive(port) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}`);
  setStatus('liveStatus', 'live: connecting…');
  ws.onopen = async () => {
    live = ws;
    document.getElementById('connectBtn').textContent = 'Disconnect';
    try {
      liveManifest = await liveSend('describe');
      document.getElementById('revertBtn').disabled = false;
      setStatus('liveStatus', `live: ${liveManifest.scene ?? 'no scene'}`, 'ok');
      console.log(`editor: live scene '${liveManifest.scene}'`);
      // Preview mirrors the wallpaper's current state: values and clock.
      if (liveManifest.parameters) {
        for (const p of liveManifest.parameters) {
          applyParam(p.name, [].concat(p.value), { fromRemote: true });
        }
      }
      refreshPanel();
      await fetchSource();
      await syncClock(/*force=*/true);
      syncTimer = setInterval(syncClock, 2000);
    } catch (err) {
      setStatus('liveStatus', `live: ${err}`, 'bad');
    }
  };
  ws.onmessage = (msg) => {
    const data = JSON.parse(msg.data);
    if (data.id !== undefined && pending.has(data.id)) {
      const { resolve, reject } = pending.get(data.id);
      pending.delete(data.id);
      data.error !== undefined ? reject(data.error) : resolve(data.result);
      return;
    }
    if (data.event === 'parameter') {
      // Someone else (driftctl, another editor) changed it.
      applyParam(data.name, [].concat(data.value), { fromRemote: true });
    } else if (data.event === 'transport') {
      setPaused(data.paused, { fromRemote: true });
      if (wasm && Math.abs(wasm.time() - data.seconds) > 0.12) {
        previewSeek(data.seconds);
      }
    } else if (data.event === 'reload') {
      (async () => {
        liveManifest = await liveSend('describe');
        refreshPanel();
        await fetchSource();
        await syncClock(/*force=*/true);
      })();
    }
  };
  ws.onclose = () => {
    if (live === ws) {
      live = null;
      liveManifest = null;
      clearInterval(syncTimer);
      syncTimer = null;
      document.getElementById('connectBtn').textContent = 'Connect';
      setStatus('liveStatus', 'live: not connected');
      console.log('editor: live disconnected');
      // The panel falls back to the preview manifest — re-describe it
      // first, or the widgets rebuild from values captured at load and
      // silently revert every parameter set during the live session.
      if (wasm) {
        try {
          const manifest = JSON.parse(wasm.describe());
          if (manifest.scene) {
            setWasmManifest(manifest);
          }
        } catch { /* no preview scene; the panel shows none-yet */ }
      }
      refreshPanel();
    }
  };
  ws.onerror = () => setStatus('liveStatus', 'live: connection failed', 'bad');
}

document.getElementById('connectBtn').onclick = () => {
  if (live) {
    live.close();
  } else {
    connectLive(document.getElementById('port').value);
  }
};
