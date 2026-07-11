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
import readline from 'node:readline';
import { fileURLToPath } from 'node:url';

// The editor and driftpkg write the same starter project.
import { templateScene,
         TEMPLATE_WGSL } from '../src/platform/web/editor/templates.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const defaultRepo = path.join(here, '..', 'library');

function usage() {
  console.error('usage: driftpkg.mjs init PATH | ' +
                'index [REPO-DIR] [--first-party] | ' +
                'install NAME[@PIN]|--all [--repo DIR|URL] [--store DIR] ' +
                '[--yes] | ' +
                'grant PROJECT-DIR [--yes] | ' +
                'list [--store DIR]');
  process.exit(2);
}

// --- §4.4 permissions: derive from interface JSONs, prompt, record ------

// Mirrors core/Module.cpp's origin grammar: https:// or wss:// host
// [:port]; plain http/ws only for localhost/127.0.0.1/[::1]. No path,
// no wildcard.
function validOrigin(origin) {
  const m = origin.match(
      /^(https?|wss?):\/\/(\[[0-9a-f:.]+\]|[a-z0-9.-]+)(:[0-9]{1,5})?$/);
  if (!m) return false;
  if ((m[1] === 'http' || m[1] === 'ws') &&
      !['localhost', '127.0.0.1', '[::1]'].includes(m[2])) {
    return false;
  }
  return true;
}

// Reads one interface JSON's permission requests; throws on grammar
// violations so a bad package fails indexing/install loudly.
function interfacePermissions(text, context) {
  let doc;
  try {
    doc = JSON.parse(text);
  } catch (e) {
    throw new Error(`${context}: ${e.message}`);
  }
  const p = doc?.permissions;
  if (!p) return null;
  const out = {};
  if (p.storage) {
    const quota = p.storage.quota;
    if (!Number.isInteger(quota) || quota <= 0 || quota > 256 << 20) {
      throw new Error(`${context}: bad permissions.storage.quota`);
    }
    out.storage = { quota };
  }
  if (p.network) {
    const origins = p.network.origins;
    if (!Array.isArray(origins) || !origins.length ||
        !origins.every((o) => typeof o === 'string' && validOrigin(o))) {
      throw new Error(`${context}: bad permissions.network.origins`);
    }
    out.network = { origins: [...new Set(origins)].sort() };
  }
  return Object.keys(out).length ? out : null;
}

// The package-level view: the union of its modules' requests (quota is a
// per-module namespace, so max, not sum). Derived, never declared twice
// (§4.4) — this is what indexes embed, prompts show, and records grant.
function derivePermissions(pkgDir) {
  const modulesDir = path.join(pkgDir, 'modules');
  let quota = 0;
  const origins = new Set();
  if (fs.existsSync(modulesDir)) {
    for (const entry of fs.readdirSync(modulesDir).sort()) {
      if (!entry.endsWith('.json')) continue;
      const p = interfacePermissions(
          fs.readFileSync(path.join(modulesDir, entry), 'utf8'),
          `modules/${entry}`);
      if (!p) continue;
      quota = Math.max(quota, p.storage?.quota ?? 0);
      for (const o of p.network?.origins ?? []) origins.add(o);
    }
  }
  const out = {};
  if (quota) out.storage = { quota };
  if (origins.size) out.network = { origins: [...origins].sort() };
  return Object.keys(out).length ? out : null;
}

function permissionLines(perms) {
  const lines = [];
  for (const o of perms?.network?.origins ?? []) lines.push(`network: ${o}`);
  if (perms?.storage) {
    lines.push(`storage: ${perms.storage.quota} bytes`);
  }
  return lines;
}

// True when `want` is fully covered by `granted` (origins subset, quota
// within) — the silent-inherit test for upgrades.
function permissionsCovered(want, granted) {
  if ((want?.storage?.quota ?? 0) > (granted?.storage?.quota ?? 0)) {
    return false;
  }
  const have = new Set(granted?.network?.origins ?? []);
  return (want?.network?.origins ?? []).every((o) => have.has(o));
}

// Order-insensitive equality via mutual coverage.
const permissionsEqual = (a, b) =>
    permissionsCovered(a, b) && permissionsCovered(b, a);

// The newest installed version's granted set for a package, if any — an
// upgrade whose requests it covers inherits without a prompt.
function latestGrant(store, name) {
  const dir = path.join(store, name);
  if (!fs.existsSync(dir)) return null;
  let best = null;
  let bestSegs = null;
  for (const v of fs.readdirSync(dir)) {
    const segs = versionSegments(v);
    if (!segs) continue;
    let perms = null;
    try {
      perms = JSON.parse(fs.readFileSync(
          path.join(dir, v, '.installed.json'), 'utf8')).permissions ?? null;
    } catch {
      continue;
    }
    if (!perms) continue;
    if (!bestSegs || versionCompare(segs, bestSegs) > 0) {
      best = perms;
      bestSegs = segs;
    }
  }
  return best;
}

function grantsFile() {
  const state = process.env.XDG_STATE_HOME ||
                path.join(process.env.HOME ?? '.', '.local', 'state');
  return path.join(state, 'drift', 'grants.json');
}

async function confirm(question, yes) {
  if (yes) return true;
  if (!process.stdin.isTTY) {
    console.error('driftpkg: refusing to grant permissions without a ' +
                  'terminal; re-run with --yes to grant non-interactively');
    return false;
  }
  const rl = readline.createInterface({ input: process.stdin,
                                        output: process.stderr });
  const answer = await new Promise((r) => rl.question(question, r));
  rl.close();
  return /^y(es)?$/i.test(answer.trim());
}

function cmdInit(target) {
  if (fs.existsSync(target) && fs.readdirSync(target).length > 0) {
    throw new Error(`${target} exists and is not empty`);
  }
  const name = path.basename(target).replace(/\.sceneproject$/, '') ||
               'Untitled';
  fs.mkdirSync(path.join(target, 'shaders'), { recursive: true });
  fs.mkdirSync(path.join(target, 'assets'), { recursive: true });
  fs.writeFileSync(path.join(target, 'scene.json'), templateScene(name));
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
  // §20.1: one or two dot-separated segments; bare names are reserved
  // for the first-party library.
  if (typeof manifest.name !== 'string' ||
      !/^[a-z0-9-]+(\.[a-z0-9-]+)?$/.test(manifest.name)) {
    throw new Error(
        `${context}: manifest 'name' must be [a-z0-9-]+ or ` +
        `publisher.name (§20.1)`);
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
    // Hash verification catches corruption, not an in-transit rewrite of
    // the index itself — remote repos must be HTTPS; plain http is for
    // local development only.
    const { protocol, hostname } = new URL(base);
    if (protocol === 'http:' &&
        !['localhost', '127.0.0.1', '[::1]', '::1'].includes(hostname)) {
      throw new Error(
          `refusing plain http for non-localhost repo '${repo}' — use https`);
    }
    return async (rel) => {
      const res = await fetch(base + rel);
      if (!res.ok) throw new Error(`GET ${base + rel}: ${res.status}`);
      return Buffer.from(await res.arrayBuffer());
    };
  }
  return async (rel) => fs.readFileSync(path.join(repo, rel));
}

// --- commands -----------------------------------------------------------

function cmdIndex(repoDir, firstParty) {
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
    // §4.4: capability requests ride the index so consent is visible
    // before download; install re-derives and cross-checks.
    const perms = derivePermissions(pkgDir);
    if (perms) {
      pkg.permissions = perms;
    }
    packages.push(pkg);
  }
  packages.sort((a, b) => (a.name < b.name ? -1 : 1));
  // §20.1: bare names are reserved for the first-party library. The flag
  // is a self-declaration — it guards against accidental squatting, not
  // attackers (that is what package signing is reserved for).
  if (!firstParty) {
    for (const pkg of packages) {
      if (!pkg.name.includes('.')) {
        console.warn(`warning: '${pkg.name}' is a bare name, reserved for ` +
                     `the first-party library; community packages should ` +
                     `be publisher.name (§20.1)`);
      }
    }
  }
  const index = { version: 1, packages };
  if (firstParty) {
    index.firstParty = true;
  }
  fs.writeFileSync(path.join(repoDir, 'index.json'),
                   JSON.stringify(index, null, 2) + '\n');
  console.log(`indexed ${packages.length} package(s) in ${repoDir}`);
}

async function cmdInstall(spec, repo, store, yes) {
  const get = repoFetcher(repo);
  const index = JSON.parse((await get('index.json')).toString('utf8'));
  if (index.version !== 1) {
    throw new Error(`unsupported repository index version ${index.version}`);
  }

  // install --all: every package in the repository, latest versions —
  // the one-command bootstrap for the examples that consume library/.
  if (spec === '--all') {
    const names = [...new Set((index.packages ?? []).map((p) => p.name))];
    for (const name of names.sort()) {
      await installOne(name, null, null, index, get, repo, store, yes);
    }
    return;
  }

  const at = spec.indexOf('@');
  const name = at === -1 ? spec : spec.slice(0, at);
  const pinText = at === -1 ? null : spec.slice(at + 1);
  const pin = pinText === null ? null : versionSegments(pinText);
  if (!/^[a-z0-9-]+(\.[a-z0-9-]+)?$/.test(name) ||
      (pinText !== null && !pin)) {
    usage();
  }
  await installOne(name, pin, pinText, index, get, repo, store, yes);
}

async function installOne(name, pin, pinText, index, get, repo, store, yes) {
  // §20.1: bare names are reserved for the first-party library.
  if (!name.includes('.') && !index.firstParty) {
    console.warn(`warning: installing bare-named '${name}' from a ` +
                 `repository that does not declare itself first-party ` +
                 `(§20.1)`);
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

  // §4.4 consent. Requests are re-derived from the fetched bytes; an
  // index that claims something else lies about its payload. An upgrade
  // covered by an existing grant inherits silently; anything else
  // prompts, and declining aborts the install.
  const perms = derivePermissions(staging);
  if (best.permissions !== undefined &&
      !permissionsEqual(perms, best.permissions)) {
    fs.rmSync(staging, { recursive: true, force: true });
    throw new Error(`index misstates '${name}' permission requests`);
  }
  if (perms) {
    const prior = latestGrant(store, name);
    if (permissionsCovered(perms, prior)) {
      console.error(`'${name}': permissions already granted; carried over`);
    } else {
      console.error(`'${name}' requests:`);
      for (const line of permissionLines(perms)) {
        console.error(`  ${line}`);
      }
      if (prior) {
        console.error('  (expands the previously granted set)');
      }
      if (!(await confirm('Allow? [y/N] ', yes))) {
        fs.rmSync(staging, { recursive: true, force: true });
        throw new Error(`permissions not granted for '${name}'`);
      }
    }
  }

  const record = { repository: String(repo), hash: best.hash };
  if (perms) {
    record.permissions = perms; // the record is the policy (§4.4)
  }
  fs.writeFileSync(path.join(staging, '.installed.json'),
                   JSON.stringify(record, null, 2) + '\n');
  fs.rmSync(dest, { recursive: true, force: true });
  fs.mkdirSync(path.dirname(dest), { recursive: true });
  fs.renameSync(staging, dest);
  console.log(`installed ${name} ${best.version} -> ${dest}`);
}

// §4.4 grants for a project run directly (drift <project>): union the
// project's own modules/ with any vendored packages', prompt, and record
// in the state-dir grants file keyed by the project's real path — where
// the native runtime reads it (platform/ProjectGrants.h).
async function cmdGrant(project, yes) {
  const root = fs.realpathSync(project);
  const sources = [root];
  const pkgsDir = path.join(root, 'packages');
  if (fs.existsSync(pkgsDir)) {
    for (const entry of fs.readdirSync(pkgsDir)) {
      sources.push(path.join(pkgsDir, entry));
    }
  }
  let quota = 0;
  const origins = new Set();
  for (const source of sources) {
    const p = derivePermissions(source);
    if (!p) continue;
    quota = Math.max(quota, p.storage?.quota ?? 0);
    for (const o of p.network?.origins ?? []) origins.add(o);
  }
  const perms = {};
  if (quota) perms.storage = { quota };
  if (origins.size) perms.network = { origins: [...origins].sort() };
  if (!Object.keys(perms).length) {
    console.log(`no capability requests in ${root}`);
    return;
  }
  console.error(`project '${root}' requests:`);
  for (const line of permissionLines(perms)) {
    console.error(`  ${line}`);
  }
  if (!(await confirm('Allow? [y/N] ', yes))) {
    throw new Error('permissions not granted');
  }
  const file = grantsFile();
  fs.mkdirSync(path.dirname(file), { recursive: true });
  let all = {};
  try {
    all = JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch { /* first grant */ }
  all[root] = { permissions: perms };
  fs.writeFileSync(file, JSON.stringify(all, null, 2) + '\n');
  console.log(`granted; recorded in ${file}`);
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
let firstParty = false;
let yes = false;
while (args.length) {
  const a = args.shift();
  if (a === '--repo') repo = args.shift() ?? usage();
  else if (a === '--store') store = args.shift() ?? usage();
  else if (a === '--first-party') firstParty = true;
  else if (a === '--yes') yes = true;
  else positional.push(a);
}

try {
  if (command === 'init') {
    if (positional.length !== 1) usage();
    cmdInit(positional[0]);
  } else if (command === 'index') {
    cmdIndex(positional[0] ?? defaultRepo, firstParty);
  } else if (command === 'install') {
    if (positional.length !== 1) usage();
    await cmdInstall(positional[0], repo, store, yes);
  } else if (command === 'grant') {
    if (positional.length !== 1) usage();
    await cmdGrant(positional[0], yes);
  } else if (command === 'list') {
    cmdList(store);
  } else {
    usage();
  }
} catch (e) {
  console.error(`driftpkg: ${e.message}`);
  process.exit(1);
}
