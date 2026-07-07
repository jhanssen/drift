import { wasm, wasmManifest } from './preview.js';
import { live, liveManifest, liveSend } from './live.js';
import { sceneSource } from './document.js';
import { project, projectRoot, projectWriteThrough } from './project.js';
import { refreshGraph } from './graph/build.js';
import { refreshTimeline } from './timeline.js';

// ---- parameter panel ------------------------------------------------------

const widgets = new Map(); // name -> { update(values) }

export function activeManifest() {
  return liveManifest?.parameters ? liveManifest : wasmManifest;
}

// Fan an edit out to every attached runtime; remote-originated changes only
// update the preview and the widgets (the live app already applied them).
export function applyParam(name, values, { fromRemote = false } = {}) {
  currentValues.set(name, values);
  if (wasm) {
    wasm.set(name, values);
  }
  if (live && !fromRemote) {
    liveSend('set', { name, value: values.length === 1 ? values[0] : values })
        .catch((err) => console.log(`editor: set ${name}: ${err}`));
  }
  // The editor is an authoring tool: a slider edit is a document edit —
  // the declared default follows the value and persists like any other
  // change (debounced; the runtimes already have the value via set).
  // Consumer-side ephemeral overrides (§17.7) belong to the runtime's
  // own future settings surface, not here. fromRemote skips persistence
  // so two attached editors don't fight over the write.
  if ((project || live) && !fromRemote &&
      sceneSource?.parameters?.[name]) {
    const decl = sceneSource.parameters[name];
    decl.default = values.length === 1 ? values[0] : [...values];
    clearTimeout(paramPersistTimer);
    paramPersistTimer = setTimeout(() => {
      const json = JSON.stringify(sceneSource, null, 2);
      if (project) {
        wasm?.writeAsset(projectRoot(), 'scene.json', json);
        projectWriteThrough('scene.json', json + '\n');
      } else if (live) {
        liveSend('write-asset', { path: 'scene.json', contents: json + '\n' })
            .catch((err) => console.log(`editor: persist ${name}: ${err}`));
      }
    }, 400);
  }
  widgets.get(name)?.update(values);
}
let paramPersistTimer = null;

export const currentValues = new Map(); // name -> values, re-push after reload

export function refreshPanel() {
  refreshGraph();
  refreshTimeline();
  const manifest = activeManifest();
  const container = document.getElementById('params');
  const mismatch = document.getElementById('mismatch');
  mismatch.hidden = !(wasmManifest && liveManifest?.scene &&
                      wasmManifest.scene !== liveManifest.scene);
  if (!mismatch.hidden) {
    mismatch.textContent = `preview shows '${wasmManifest.scene}' but the ` +
                           `live scene is '${liveManifest.scene}'`;
  }
  container.textContent = '';
  widgets.clear();
  if (!manifest?.parameters?.length) {
    container.innerHTML = '<span style="color:var(--dim)">' +
        (manifest ? 'scene has no parameters' : 'none yet') + '</span>';
    return;
  }
  for (const param of manifest.parameters) {
    container.appendChild(buildWidget(param));
  }
}

function buildWidget(param) {
  const row = document.createElement('div');
  row.className = 'row';
  const label = document.createElement('label');
  label.textContent = param.label || param.name;
  label.title = param.name;
  row.appendChild(label);

  const values = [].concat(param.value);
  const isColor = param.hint === 'color' &&
                  (param.type === 'vec3' || param.type === 'vec4');

  if (param.type === 'scalar') {
    const min = param.min ?? 0;
    const max = param.max ?? 1;
    const slider = document.createElement('input');
    slider.type = 'range';
    slider.min = min;
    slider.max = max;
    slider.step = param.step || (max - min) / 1000;
    const number = document.createElement('input');
    number.type = 'number';
    number.step = slider.step;
    const push = (v) => applyParam(param.name, [Number(v)]);
    slider.oninput = () => push(slider.value);
    number.onchange = () => push(number.value);
    row.appendChild(slider);
    row.appendChild(number);
    widgets.set(param.name, {
      update([v]) { slider.value = v; number.value = +v.toFixed(4); },
    });
  } else if (isColor) {
    const color = document.createElement('input');
    color.type = 'color';
    const alpha = param.type === 'vec4' ? document.createElement('input') : null;
    if (alpha) {
      alpha.type = 'number';
      alpha.min = 0;
      alpha.max = 1;
      alpha.step = 0.01;
      alpha.title = 'alpha';
    }
    const current = () => {
      const rgb = [1, 3, 5].map(
          (i) => parseInt(color.value.slice(i, i + 2), 16) / 255);
      return alpha ? [...rgb, Number(alpha.value)] : rgb;
    };
    color.oninput = () => applyParam(param.name, current());
    if (alpha) {
      alpha.onchange = () => applyParam(param.name, current());
      row.appendChild(alpha);
    }
    row.insertBefore(color, alpha);
    widgets.set(param.name, {
      update(v) {
        color.value = '#' + v.slice(0, 3).map((c) =>
            Math.round(Math.max(0, Math.min(1, c)) * 255)
                .toString(16).padStart(2, '0')).join('');
        if (alpha) alpha.value = +(v[3] ?? 1).toFixed(2);
      },
    });
  } else {
    const arity = { vec2: 2, vec3: 3, vec4: 4 }[param.type] ?? values.length;
    const inputs = [];
    for (let i = 0; i < arity; ++i) {
      const input = document.createElement('input');
      input.type = 'number';
      input.step = param.step || 0.01;
      input.onchange = () =>
          applyParam(param.name, inputs.map((box) => Number(box.value)));
      inputs.push(input);
      row.appendChild(input);
    }
    widgets.set(param.name, {
      update(v) { inputs.forEach((box, i) => box.value = +v[i].toFixed(4)); },
    });
  }

  widgets.get(param.name).update(values);
  return row;
}
