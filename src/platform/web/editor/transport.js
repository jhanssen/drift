import { wasm } from './preview.js';
import { live, liveSend } from './live.js';
import { currentValues } from './params.js';
import { laneFlashes } from './timeline.js';

// ---- transport ------------------------------------------------------------

let paused = false;

// Seeks the preview only; drift_seek reloads on backward, which resets
// parameters to scene defaults — re-push what the panel currently shows.
export function previewSeek(seconds) {
  if (!wasm) {
    return;
  }
  if (wasm.seek(seconds)) {
    for (const [name, values] of currentValues) {
      wasm.set(name, values);
    }
  }
}

export function setPaused(value, { fromRemote = false } = {}) {
  paused = value;
  document.getElementById('playBtn').textContent = paused ? '▶' : '⏸';
  if (wasm) {
    wasm.pause(paused ? 1 : 0);
  }
  if (live && !fromRemote) {
    liveSend('pause', { paused }).catch((err) =>
        console.log(`editor: pause: ${err}`));
  }
}

export function seekBoth(seconds) {
  previewSeek(seconds);
  if (live) {
    liveSend('seek', { time: seconds }).catch((err) =>
        console.log(`editor: seek: ${err}`));
  }
}

// "Fire now" (§16.1): inject a one-frame event fire into both runtimes.
export function fireBoth(node, port) {
  if (wasm) {
    wasm.fire(node, port);
  }
  if (live) {
    liveSend('fire', { node, port }).catch((err) =>
        console.log(`editor: fire: ${err}`));
  }
  laneFlashes.set(`${node}/${port}`, performance.now());
}

document.getElementById('playBtn').onclick = () => setPaused(!paused);
