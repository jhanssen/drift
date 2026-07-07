import { query, sceneName } from './config.js';
import { templateScene, TEMPLATE_WGSL } from './templates.js';
import { wasm, wasmManifest, setWasmManifest, Module } from './preview.js';
import { live, liveSend } from './live.js';
import { setStatus, openQuickForm } from './ui.js';
import { currentValues, refreshPanel } from './params.js';
import { sceneSource, setSceneSource } from './document.js';
import { viewStack, setGraphNeedsFit, setGraphStatus } from './graph/state.js';
import { graphDocs } from './graph/model.js';
import { priorBindings } from './graph/wiring.js';

// The picker fills from the bundle (drift_scenes) once the runtime is up;
// until then it shows just the active scene.
const sceneSelect = document.getElementById('sceneSelect');
{
  const option = document.createElement('option');
  option.value = option.textContent = sceneName;
  option.selected = true;
  sceneSelect.appendChild(option);
}
export function populateScenes() {
  try {
    const names = JSON.parse(wasm.scenes());
    sceneSelect.textContent = '';
    for (const name of names) {
      const option = document.createElement('option');
      option.value = option.textContent = name;
      option.selected = name === sceneName;
      sceneSelect.appendChild(option);
    }
    // Provider projects (§5.4) list below the bundled scenes.
    kvGet('recents').then((r) => populateRecents(r ?? [])).catch(() => {});
    kvGet('store').then((h) => { storeHandle ??= h ?? null; })
        .catch(() => {});
  } catch (err) {
    console.log(`editor: scenes: ${err}`);
  }
}
sceneSelect.onchange = async () => {
  const value = sceneSelect.value;
  if (value.startsWith('project:')) {
    // Recent project: reopen from its persisted handle (may prompt).
    const recents = (await kvGet('recents').catch(() => null)) ?? [];
    const entry = recents.find((r) => 'project:' + r.name === value);
    if (!entry || !await openProject(entry.handle, entry.kind, entry.name)) {
      sceneSelect.value =
          project ? 'project:' + project.name : sceneName;
    }
    return;
  }
  // Bundled scene: runtime scene switch = page reload. Projects persist
  // continuously, so there is nothing to lose by leaving one.
  query.set('scene', value);
  query.delete('scratch');
  location.search = query.toString();
};

// ---- projects: storage providers (DESIGN.md §5.4) ---------------------------

// The active provider-backed project; null = a bundled scene. The preview's
// MEMFS holds only the working copy — every write fans out to the project's
// directory handle (a real folder via the picker, or origin-private
// storage), so the folder IS the project, the same bytes the native
// runtime runs.
export let project = null; // { name, root, handle, kind: 'dir'|'opfs', template }
let storeHandle = null;    // §20 package store overlay (persisted handle)
let pendingProject = null; // open blocked on missing packages; retried by
                           // the Packages… button

// -- tiny IndexedDB kv: persists directory handles (localStorage cannot) --
function kvOpen() {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open('drift-editor', 1);
    req.onupgradeneeded = () => req.result.createObjectStore('kv');
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}
async function kvGet(key) {
  const db = await kvOpen();
  return new Promise((resolve, reject) => {
    const r = db.transaction('kv').objectStore('kv').get(key);
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
}
async function kvSet(key, value) {
  const db = await kvOpen();
  return new Promise((resolve, reject) => {
    const t = db.transaction('kv', 'readwrite');
    t.objectStore('kv').put(value, key);
    t.oncomplete = () => resolve();
    t.onerror = () => reject(t.error);
  });
}

// OPFS handles have no permission model; picked directories re-confirm
// once per session (a one-click prompt).
async function granted(handle, mode) {
  if (!handle.queryPermission) {
    return true;
  }
  if (await handle.queryPermission({ mode }) === 'granted') {
    return true;
  }
  return await handle.requestPermission({ mode }) === 'granted';
}

// -- mirroring between a directory handle and the preview working copy --
async function mirrorHandleToFS(dir, root, prefix = '') {
  for await (const entry of dir.values()) {
    if (entry.name.startsWith('.')) {
      continue; // .git and friends
    }
    const rel = prefix ? `${prefix}/${entry.name}` : entry.name;
    if (entry.kind === 'directory') {
      await mirrorHandleToFS(entry, root, rel);
    } else {
      const data =
          new Uint8Array(await (await entry.getFile()).arrayBuffer());
      const full = `${root}/${rel}`;
      Module.FS.mkdirTree(full.slice(0, full.lastIndexOf('/')));
      Module.FS.writeFile(full, data);
    }
  }
}
async function writeFileToHandle(dirHandle, rel, contents) {
  let dir = dirHandle;
  const parts = rel.split('/');
  for (const part of parts.slice(0, -1)) {
    dir = await dir.getDirectoryHandle(part, { create: true });
  }
  const file =
      await dir.getFileHandle(parts[parts.length - 1], { create: true });
  const writable = await file.createWritable();
  await writable.write(contents);
  await writable.close();
}
function listFSTree(root, prefix = '') {
  const out = [];
  for (const name of Module.FS.readdir(prefix ? `${root}/${prefix}`
                                              : root)) {
    if (name === '.' || name === '..') {
      continue;
    }
    const rel = prefix ? `${prefix}/${name}` : name;
    if (Module.FS.isDir(Module.FS.stat(`${root}/${rel}`).mode)) {
      out.push(...listFSTree(root, rel));
    } else {
      out.push(rel);
    }
  }
  return out;
}

// Fan a project-file write out to the handle. Failures surface but never
// block the working copy — the preview stays editable regardless.
export function projectWriteThrough(path, contents) {
  if (!project?.handle) {
    return;
  }
  writeFileToHandle(project.handle, path, contents).catch((err) => {
    console.log(`editor: project write ${path}: ${err}`);
    setStatus('wasmStatus', `project write failed: ${path}`, 'warn');
  });
}

// -- §20 store overlay: resolve missing packages from an attached store --
const verSegs = (v) =>
    /^[0-9]+(\.[0-9]+)*$/.test(v) ? v.split('.').map(Number) : null;
const verCmp = (a, b) => {
  for (let i = 0; i < Math.max(a.length, b.length); ++i) {
    const x = a[i] ?? 0, y = b[i] ?? 0;
    if (x !== y) {
      return x < y ? -1 : 1;
    }
  }
  return 0;
};
const verPin = (v, pin) =>
    pin.length <= v.length && pin.every((s, i) => s === v[i]);

const missingPackages = (errors) =>
    [...(errors ?? '').matchAll(
        /package '([a-z0-9-]+)'(?: \(pin '([^']+)'\))? is not installed/g)]
        .map((m) => ({ name: m[1], pin: m[2] ? verSegs(m[2]) : null }));

// Mirror the newest matching version of each missing package from the
// attached store into the working copy's store at /packages (§20.2).
async function overlayPackages(missing) {
  if (!storeHandle || !missing.length ||
      !await granted(storeHandle, 'read')) {
    return false;
  }
  let any = false;
  for (const { name, pin } of missing) {
    try {
      const pkgDir = await storeHandle.getDirectoryHandle(name);
      let best = null, bestDir = null;
      for await (const entry of pkgDir.values()) {
        const segs = entry.kind === 'directory' ? verSegs(entry.name)
                                                : null;
        if (!segs || (pin && !verPin(segs, pin))) {
          continue;
        }
        if (!best || verCmp(segs, best) > 0) {
          best = segs;
          bestDir = entry;
        }
      }
      if (bestDir) {
        await mirrorHandleToFS(bestDir,
                               `/packages/${name}/${best.join('.')}`);
        any = true;
      }
    } catch { /* not in this store; the load error stands */ }
  }
  return any;
}

// -- project lifecycle --
const slugify = (name) =>
    name.replace(/[^A-Za-z0-9_-]+/g, '-').replace(/^-+|-+$/g, '') ||
    'project';

function populateRecents(recents) {
  let group = sceneSelect.querySelector('#recentGroup');
  if (!group) {
    group = document.createElement('optgroup');
    group.id = 'recentGroup';
    group.label = 'projects';
    sceneSelect.appendChild(group);
  }
  group.textContent = '';
  for (const r of recents) {
    const option = document.createElement('option');
    option.value = 'project:' + r.name;
    option.textContent =
        `${r.name} (${r.kind === 'opfs' ? 'browser' : 'folder'})`;
    group.appendChild(option);
  }
}

async function rememberRecent(entry) {
  const recents = (await kvGet('recents').catch(() => null)) ?? [];
  const next = [entry,
                ...recents.filter((r) => r.name !== entry.name)].slice(0, 8);
  await kvSet('recents', next).catch(() => {});
  populateRecents(next);
}

// Adopt a directory handle as the active project: mirror into the working
// copy, open in the preview (resolving missing packages from the attached
// store, §5.4 failure-driven), remember it.
async function openProject(handle, kind, name, { template = false } = {}) {
  if (!wasm || !await granted(handle, 'readwrite')) {
    setStatus('wasmStatus', 'project access was not granted', 'warn');
    return false;
  }
  const root = '/projects/' + slugify(name);
  Module.FS.mkdirTree(root);
  await mirrorHandleToFS(handle, root);
  if (live) {
    live.close(); // provider projects are standalone
  }
  let opened = wasm.open(root);
  if (!opened) {
    const missing = missingPackages(wasm.errors());
    if (missing.length) {
      storeHandle ??= (await kvGet('store').catch(() => null)) ?? null;
      if (storeHandle && await overlayPackages(missing)) {
        opened = wasm.open(root);
      }
    }
    if (!opened && missing.length) {
      pendingProject = { handle, kind, name, template };
      document.getElementById('pkgBtn').hidden = false;
      setStatus('wasmStatus',
                `missing packages: ${missing.map((m) => m.name).join(', ')}` +
                ' — attach your store via Packages…', 'warn');
      return false;
    }
    if (!opened) {
      setStatus('wasmStatus',
                `open failed: ${(wasm.errors() || '').split('\n')[0]}`,
                'warn');
      return false;
    }
  }
  project = { name, root, handle, kind, template };
  currentValues.clear();
  setGraphStatus(null);
  priorBindings.clear();
  viewStack.length = 0;
  graphDocs.clear();
  setWasmManifest(JSON.parse(wasm.describe()));
  setSceneSource(JSON.parse(wasm.source()));
  setGraphNeedsFit(true); // a whole new graph deserves a fresh framing
  setStatus('wasmStatus',
            `preview: ${wasmManifest.scene} (${kind === 'opfs'
                ? 'browser project' : 'folder project'})`, 'ok');
  const shaderSection = document.getElementById('shaderSection');
  shaderSection.hidden = !template;
  if (template) {
    document.getElementById('wgsl').value =
        wasm.readAsset('shaders/main.wgsl');
  }
  await rememberRecent({ name, kind, handle, when: Date.now() });
  sceneSelect.value = 'project:' + name;
  refreshPanel();
  return true;
}

export async function createProject(name, kind) {
  try {
    let handle;
    if (kind === 'opfs') {
      const opfs = await navigator.storage.getDirectory();
      const parent = await opfs.getDirectoryHandle('projects',
                                                   { create: true });
      handle = await parent.getDirectoryHandle(slugify(name),
                                               { create: true });
    } else {
      // The picker chooses where; the project folder is created inside
      // the grant (the picker's own New Folder works too).
      const parent =
          await showDirectoryPicker({ id: 'projects', mode: 'readwrite' });
      handle = await parent.getDirectoryHandle(slugify(name),
                                               { create: true });
    }
    await writeFileToHandle(handle, 'scene.json', templateScene('Untitled'));
    await writeFileToHandle(handle, 'shaders/main.wgsl', TEMPLATE_WGSL);
    return await openProject(handle, kind, name, { template: true });
  } catch (err) {
    if (err.name !== 'AbortError') {
      console.log(`editor: new project: ${err}`);
    }
    return false;
  }
}

function applyShader() {
  if (!project?.template || !wasm) {
    return;
  }
  const src = document.getElementById('wgsl').value;
  wasm.writeAsset(projectRoot(), 'shaders/main.wgsl', src);
  projectWriteThrough('shaders/main.wgsl', src);
  // Reload the document so the shader re-reflects; ports may have changed.
  if (!wasm.load(JSON.stringify(sceneSource))) {
    setStatus('wgslStatus', wasm.errors() || 'load failed', 'bad');
    return;
  }
  for (const [name, values] of currentValues) {
    wasm.set(name, values);
  }
  setWasmManifest(JSON.parse(wasm.describe()));
  setStatus('wgslStatus', 'applied', 'ok');
  refreshPanel();
}

// Save As: copy the working copy (with the live document) into a new
// folder and switch to it — how a bundled scene or browser project
// becomes a real folder.
async function saveProjectAs() {
  if (!wasm || !sceneSource) {
    return;
  }
  try {
    const parent =
        await showDirectoryPicker({ id: 'projects', mode: 'readwrite' });
    const name = project?.name ?? sceneName;
    const handle = await parent.getDirectoryHandle(slugify(name),
                                                   { create: true });
    const root = projectRoot();
    wasm.writeAsset(root, 'scene.json',
                    JSON.stringify(sceneSource, null, 2) + '\n');
    for (const rel of listFSTree(root)) {
      await writeFileToHandle(handle, rel,
                              Module.FS.readFile(`${root}/${rel}`));
    }
    return await openProject(handle, 'dir', name,
                             { template: project?.template ?? false });
  } catch (err) {
    if (err.name !== 'AbortError') {
      console.log(`editor: save as: ${err}`);
    }
    return false;
  }
}

document.getElementById('newBtn').onclick = () => {
  const anchor = document.getElementById('newBtn').getBoundingClientRect();
  openQuickForm(anchor, 'new project', [
    { key: 'name', label: 'name', value: 'untitled' },
    { key: 'where', label: 'location',
      options: ['folder…', 'browser storage'] },
    { note: 'folder…: the picker chooses a parent (e.g. Documents/drift ' +
            '— browsers refuse top-level system folders); the project ' +
            'folder is created inside it' },
  ], (v) => {
    if (!v.name) {
      return 'a name is required';
    }
    createProject(v.name, v.where === 'browser storage' ? 'opfs' : 'dir');
    return null;
  });
};
document.getElementById('openBtn').onclick = async () => {
  try {
    const handle =
        await showDirectoryPicker({ id: 'projects', mode: 'readwrite' });
    await openProject(handle, 'dir', handle.name);
  } catch (err) {
    if (err.name !== 'AbortError') {
      console.log(`editor: open: ${err}`);
    }
  }
};
document.getElementById('saveBtn').onclick = saveProjectAs;
document.getElementById('pkgBtn').onclick = async () => {
  try {
    const handle =
        await showDirectoryPicker({ id: 'pkgstore', mode: 'read' });
    storeHandle = handle;
    await kvSet('store', handle).catch(() => {});
    if (pendingProject) {
      const p = pendingProject;
      pendingProject = null;
      await openProject(p.handle, p.kind, p.name,
                        { template: p.template });
    }
  } catch (err) {
    if (err.name !== 'AbortError') {
      console.log(`editor: packages: ${err}`);
    }
  }
};
document.getElementById('revertBtn').onclick = async () => {
  // Re-read from disk (or the bundle) everywhere — how hand-edits to
  // scene.json get picked up, and how pushed edits get discarded.
  setGraphStatus(null); // a draft is exactly what revert discards
  priorBindings.clear();
  if (wasm && wasmManifest && wasm.reload()) {
    currentValues.clear();
    setWasmManifest(JSON.parse(wasm.describe()));
    setSceneSource(JSON.parse(wasm.source()));
  }
  if (live) {
    try {
      await liveSend('reload'); // its reload event re-describes us
    } catch (err) {
      console.log(`editor: reload: ${err}`);
    }
  }
  refreshPanel();
};
document.getElementById('applyShaderBtn').onclick = applyShader;
document.getElementById('wgsl').addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) {
    e.preventDefault();
    applyShader();
  }
});

export const projectRoot = () =>
    project ? project.root : '/scenes/' + sceneName;
