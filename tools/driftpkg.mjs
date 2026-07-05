#!/usr/bin/env node
// Package manager for drift node packages (SCENE_FORMAT.md §20). No
// dependencies. A repository is a directory or static HTTP host with an
// index.json (§20.4); the store is where the runtime resolves
// packages/<name>/… references from (§20.2).
//
// usage: driftpkg.mjs init PATH                  (new .sceneproject skeleton)
//        driftpkg.mjs index [REPO-DIR]           (regenerate index.json)
//        driftpkg.mjs install NAME[@PIN] [--repo DIR|URL] [--store DIR]
//        driftpkg.mjs list [--store DIR]
//
// Defaults: REPO-DIR/--repo = the library/ directory next to this tool's
// repository checkout; --store = $DRIFT_PACKAGE_PATH's first entry, else
// $XDG_DATA_HOME/drift/packages, else ~/.local/share/drift/packages.

import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const defaultRepo = path.join(here, '..', 'library');

function usage() {
  console.error('usage: driftpkg.mjs init PATH | index [REPO-DIR] | ' +
                'install NAME[@PIN] [--repo DIR|URL] [--store DIR] | ' +
                'list [--store DIR]');
  process.exit(2);
}

// The same starter the editor's New… writes (editor.html TEMPLATE_*):
// a sequence-driven glow with one parameter, ready to run and edit.
const TEMPLATE_SCENE = (name) => JSON.stringify({
  version: 1,
  name,
  parameters: {
    tint: { type: 'vec3', default: [0.4, 0.6, 1.0], hint: 'color',
            label: 'Tint' },
  },
  nodes: [
    { id: 'seq', type: 'sequence', duration: 8, loop: true,
      tracks: [
        { name: 'level', kind: 'value', type: 'scalar',
          interpolate: 'smooth',
          keys: [{ t: 0, value: 0.2 }, { t: 4, value: 1 },
                 { t: 8, value: 0.2 }] },
      ],
      inputs: { time: '@time.seconds' } },
    { id: 'main', type: 'shader', shader: 'shaders/main.wgsl',
      inputs: { level: '@seq.level', tint: '$tint' } },
    { id: 'out', type: 'output', inputs: { color: '@main' } },
  ],
}, null, 2) + '\n';

const TEMPLATE_WGSL = `struct Params {
    level: f32,
    tint: vec3<f32>,
}
@group(0) @binding(0) var<uniform> params: Params;

@fragment fn main(@location(0) uv: vec2f) -> @location(0) vec4f {
    let d = distance(uv, vec2f(0.5, 0.5));
    let glow = params.level * exp(-d * d * 8.0);
    return vec4f(params.tint * glow, 1.0);
}
`;

function cmdInit(target) {
  if (fs.existsSync(target) && fs.readdirSync(target).length > 0) {
    throw new Error(`${target} exists and is not empty`);
  }
  const name = path.basename(target).replace(/\.sceneproject$/, '') ||
               'Untitled';
  fs.mkdirSync(path.join(target, 'shaders'), { recursive: true });
  fs.mkdirSync(path.join(target, 'assets'), { recursive: true });
  fs.writeFileSync(path.join(target, 'scene.json'), TEMPLATE_SCENE(name));
  fs.writeFileSync(path.join(target, 'shaders', 'main.wgsl'),
                   TEMPLATE_WGSL);
  console.log(`created ${target}`);
  console.log(`run it:   drift ${target}`);
  console.log('edit it:  open the folder in the editor (Open…), or run');
  console.log(`          drift ${target} --listen 7681 and connect`);
}

function defaultStore() {
  const env = process.env.DRIFT_PACKAGE_PATH;
  if (env) return env.split(':').filter(Boolean)[0];
  const xdg = process.env.XDG_DATA_HOME;
  if (xdg) return path.join(xdg, 'drift', 'packages');
  return path.join(process.env.HOME || '.', '.local', 'share', 'drift',
                   'packages');
}

function sha256(data) {
  return crypto.createHash('sha256').update(data).digest('hex');
}

// §20.4: tree hash = sha256 over "<sha256>  <path>\n" per file, sorted.
function treeHash(files) {
  const lines = files
    .slice()
    .sort((a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0))
    .map((f) => `${f.sha256}  ${f.path}\n`)
    .join('');
  return sha256(lines);
}

function versionSegments(v) {
  if (!/^[0-9]+(\.[0-9]+)*$/.test(v)) return null;
  return v.split('.').map(Number);
}

function versionCompare(a, b) {
  const n = Math.max(a.length, b.length);
  for (let i = 0; i < n; ++i) {
    const x = a[i] ?? 0;
    const y = b[i] ?? 0;
    if (x !== y) return x < y ? -1 : 1;
  }
  return 0;
}

function matchesPin(version, pin) {
  return pin.length <= version.length &&
         pin.every((seg, i) => seg === version[i]);
}

function listFiles(dir, prefix = '') {
  const out = [];
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const rel = prefix ? `${prefix}/${entry.name}` : entry.name;
    if (entry.isDirectory()) {
      out.push(...listFiles(path.join(dir, entry.name), rel));
    } else if (entry.isFile() && !entry.name.startsWith('.')) {
      out.push(rel);
    }
  }
  return out;
}

function readManifest(text, context) {
  let manifest;
  try {
    manifest = JSON.parse(text);
  } catch (e) {
    throw new Error(`${context}: manifest.json: ${e.message}`);
  }
  if (typeof manifest.name !== 'string' ||
      !/^[a-z0-9-]+$/.test(manifest.name)) {
    throw new Error(`${context}: manifest 'name' must be [a-z0-9-]+`);
  }
  if (typeof manifest.version !== 'string' ||
      !versionSegments(manifest.version)) {
    throw new Error(`${context}: manifest 'version' must be dotted integers`);
  }
  return manifest;
}

// --- repository access: directory or http(s) base URL ------------------

function repoFetcher(repo) {
  if (/^https?:\/\//.test(repo)) {
    const base = repo.endsWith('/') ? repo : repo + '/';
    return async (rel) => {
      const res = await fetch(base + rel);
      if (!res.ok) throw new Error(`GET ${base + rel}: ${res.status}`);
      return Buffer.from(await res.arrayBuffer());
    };
  }
  return async (rel) => fs.readFileSync(path.join(repo, rel));
}

// --- commands -----------------------------------------------------------

function cmdIndex(repoDir) {
  const packages = [];
  for (const entry of fs.readdirSync(repoDir, { withFileTypes: true })) {
    if (!entry.isDirectory()) continue;
    const pkgDir = path.join(repoDir, entry.name);
    const manifestPath = path.join(pkgDir, 'manifest.json');
    if (!fs.existsSync(manifestPath)) continue;
    const manifest = readManifest(fs.readFileSync(manifestPath, 'utf8'),
                                  entry.name);
    if (manifest.name !== entry.name) {
      throw new Error(`${entry.name}: manifest 'name' is '${manifest.name}'`);
    }
    const files = listFiles(pkgDir).sort().map((rel) => {
      const data = fs.readFileSync(path.join(pkgDir, rel));
      return { path: rel, sha256: sha256(data), size: data.length };
    });
    const pkg = {
      name: manifest.name,
      version: manifest.version,
      files,
      hash: treeHash(files),
    };
    if (typeof manifest.description === 'string') {
      pkg.description = manifest.description;
    }
    packages.push(pkg);
  }
  packages.sort((a, b) => (a.name < b.name ? -1 : 1));
  const index = { version: 1, packages };
  fs.writeFileSync(path.join(repoDir, 'index.json'),
                   JSON.stringify(index, null, 2) + '\n');
  console.log(`indexed ${packages.length} package(s) in ${repoDir}`);
}

async function cmdInstall(spec, repo, store) {
  const at = spec.indexOf('@');
  const name = at === -1 ? spec : spec.slice(0, at);
  const pinText = at === -1 ? null : spec.slice(at + 1);
  const pin = pinText === null ? null : versionSegments(pinText);
  if (!/^[a-z0-9-]+$/.test(name) || (pinText !== null && !pin)) usage();

  const get = repoFetcher(repo);
  const index = JSON.parse((await get('index.json')).toString('utf8'));
  if (index.version !== 1) {
    throw new Error(`unsupported repository index version ${index.version}`);
  }
  let best = null;
  for (const pkg of index.packages ?? []) {
    if (pkg.name !== name) continue;
    const segs = versionSegments(pkg.version);
    if (!segs || (pin && !matchesPin(segs, pin))) continue;
    if (!best || versionCompare(segs, versionSegments(best.version)) > 0) {
      best = pkg;
    }
  }
  if (!best) {
    throw new Error(`repository has no '${name}'` +
                    (pinText ? ` matching pin '${pinText}'` : ''));
  }

  // Fetch and verify every file, then the tree hash, into a staging dir
  // beside the final location so a failed install leaves nothing behind.
  const dest = path.join(store, name, best.version);
  const staging = dest + '.staging';
  fs.rmSync(staging, { recursive: true, force: true });
  const fetched = [];
  for (const file of best.files) {
    if (file.path.split('/').includes('..') ||
        path.isAbsolute(file.path)) {
      throw new Error(`index entry escapes the package: ${file.path}`);
    }
    const data = await get(`${name}/${file.path}`);
    const digest = sha256(data);
    if (digest !== file.sha256) {
      throw new Error(`${file.path}: sha256 mismatch (${digest})`);
    }
    const target = path.join(staging, file.path);
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.writeFileSync(target, data);
    fetched.push({ path: file.path, sha256: digest });
  }
  if (treeHash(fetched) !== best.hash) {
    throw new Error('package tree hash mismatch');
  }
  const manifest = readManifest(
    fs.readFileSync(path.join(staging, 'manifest.json'), 'utf8'), name);
  if (manifest.name !== name || manifest.version !== best.version) {
    throw new Error('manifest does not match the index entry');
  }
  fs.writeFileSync(path.join(staging, '.installed.json'),
                   JSON.stringify({ repository: String(repo),
                                    hash: best.hash }, null, 2) + '\n');
  fs.rmSync(dest, { recursive: true, force: true });
  fs.mkdirSync(path.dirname(dest), { recursive: true });
  fs.renameSync(staging, dest);
  console.log(`installed ${name} ${best.version} -> ${dest}`);
}

function cmdList(store) {
  if (!fs.existsSync(store)) {
    console.log(`(empty store: ${store})`);
    return;
  }
  for (const name of fs.readdirSync(store).sort()) {
    const dir = path.join(store, name);
    if (!fs.statSync(dir).isDirectory()) continue;
    const versions = fs.readdirSync(dir).filter((v) => versionSegments(v));
    versions.sort((a, b) =>
      versionCompare(versionSegments(a), versionSegments(b)));
    for (const v of versions) {
      console.log(`${name} ${v}`);
    }
  }
}

// --- argument handling ----------------------------------------------------

const args = process.argv.slice(2);
const command = args.shift();
let repo = defaultRepo;
let store = defaultStore();
const positional = [];
while (args.length) {
  const a = args.shift();
  if (a === '--repo') repo = args.shift() ?? usage();
  else if (a === '--store') store = args.shift() ?? usage();
  else positional.push(a);
}

try {
  if (command === 'init') {
    if (positional.length !== 1) usage();
    cmdInit(positional[0]);
  } else if (command === 'index') {
    cmdIndex(positional[0] ?? defaultRepo);
  } else if (command === 'install') {
    if (positional.length !== 1) usage();
    await cmdInstall(positional[0], repo, store);
  } else if (command === 'list') {
    cmdList(store);
  } else {
    usage();
  }
} catch (e) {
  console.error(`driftpkg: ${e.message}`);
  process.exit(1);
}
