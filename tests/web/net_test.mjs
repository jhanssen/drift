#!/usr/bin/env node
// §4.4 browser network backend, end to end: the real wasm bundle in
// headless Chrome (CDP over --remote-debugging-pipe, tests/web/cdp.mjs)
// against loopback assertion servers. The probe module
// (fixtures/netprobe.wat) reports everything it observes back through
// the network, so the servers' transcript is the assertion — no page-
// side observability needed, and the test proves the full loop:
// imports -> core -> fetch/WebSocket -> delivery shims -> external wake
// -> module read -> outbound send. Because the page and the servers sit
// on different ports, HTTP runs under genuine CORS (the §4.4 browser
// envelope): /nocors omits the ACAO header and must read as the offline
// face, while the request still reaches the wire (CORS blocks
// responses, not simple requests). A second, never-granted server must
// see no connection at all.
//
// The test project is written into MEMFS at runtime (Module.FS +
// drift_open), so the servers use ephemeral ports; they are rendered
// zero-padded to five digits everywhere (declared origins, grant
// record, the module's URLs) to satisfy §4.4's byte-exact origin match.
//
// Exit codes: 0 pass, 1 fail, 77 skip (no Chrome / no bundle) — wired
// to ctest's SKIP_RETURN_CODE.
//
// usage: net_test.mjs [--bundle DIR] [--chrome BIN] [--headful]
//                     [--timeout SECONDS]

import { createServer } from 'node:http';
import { createHash } from 'node:crypto';
import { readFileSync, existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname, extname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { Chrome } from './cdp.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const repo = resolve(here, '../..');

const args = process.argv.slice(2);
const opt = (name, def) => {
    const i = args.indexOf(name);
    return i >= 0 ? args[i + 1] : def;
};
const bundle = resolve(opt('--bundle',
                           process.env.DRIFT_WEB_BUNDLE ??
                               join(repo, 'build-web')));
const headful = args.includes('--headful');
const timeoutMs = Number(opt('--timeout', '60')) * 1000;

const chromeBin = opt('--chrome', process.env.CHROME) ?? (() => {
    for (const c of ['google-chrome', 'google-chrome-stable', 'chromium',
                     'chromium-browser']) {
        for (const dir of process.env.PATH.split(':')) {
            if (existsSync(join(dir, c))) {
                return join(dir, c);
            }
        }
    }
    return null;
})();

if (!chromeBin) {
    console.log('SKIP: no chrome/chromium on PATH (set --chrome or CHROME)');
    process.exit(77);
}
if (!existsSync(join(bundle, 'drift.html'))) {
    console.log(`SKIP: no web bundle at ${bundle} (emcmake build, or ` +
                '--bundle/DRIFT_WEB_BUNDLE)');
    process.exit(77);
}

// ---- servers ------------------------------------------------------------

const listen = (srv) => new Promise((ok) =>
    srv.listen(0, '127.0.0.1', () => ok(srv.address().port)));

// Static server for the bundle.
const mime = { '.html': 'text/html', '.js': 'text/javascript',
               '.wasm': 'application/wasm', '.css': 'text/css' };
const staticSrv = createServer((req, res) => {
    const path = join(bundle, req.url.split('?')[0].replace(/^\/+/, ''));
    try {
        const body = readFileSync(path);
        res.writeHead(200, { 'Content-Type':
            mime[extname(path)] ?? 'application/octet-stream' });
        res.end(body);
    } catch {
        res.writeHead(404);
        res.end();
    }
});

// The assertion server: HTTP routes + a WS echo peer, one transcript.
const transcript = [];
const wsLog = [];
let done; // resolves when the module's terminal /close lands
const terminal = new Promise((ok) => { done = ok; });

const DATA_BODY = 'hello from drift-web';
const apiSrv = createServer((req, res) => {
    const url = new URL(req.url, 'http://x');
    const entry = { method: req.method, path: url.pathname,
                    query: Object.fromEntries(url.searchParams),
                    contentType: req.headers['content-type'] ?? null,
                    body: '' };
    transcript.push(entry);
    let chunks = [];
    req.on('data', (d) => chunks.push(d));
    req.on('end', () => {
        entry.body = Buffer.concat(chunks).toString();
        const cors = { 'Access-Control-Allow-Origin': '*' };
        if (req.method === 'OPTIONS') { // PNA/preflight insurance
            res.writeHead(204, { ...cors,
                'Access-Control-Allow-Methods': 'GET, POST',
                'Access-Control-Allow-Headers': '*',
                'Access-Control-Allow-Private-Network': 'true' });
            return res.end();
        }
        switch (url.pathname) {
        case '/data':
            res.writeHead(200, cors);
            return res.end(DATA_BODY);
        case '/nocors': // no ACAO: the browser must refuse the response
            res.writeHead(200);
            return res.end('unreadable');
        case '/report':
            res.writeHead(200, cors);
            return res.end();
        case '/close':
            res.writeHead(200, cors);
            res.end();
            return done();
        default:
            res.writeHead(404, cors);
            return res.end();
        }
    });
});

// Server-side WebSocket: hand-rolled framing (client frames masked).
const wsAccept = (key) => createHash('sha1')
    .update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
const wsFrame = (op, payload = Buffer.alloc(0)) =>
    Buffer.concat([Buffer.from([0x80 | op, payload.length]), payload]);

apiSrv.on('upgrade', (req, socket) => {
    if (new URL(req.url, 'http://x').pathname !== '/ws') {
        return socket.destroy();
    }
    socket.write('HTTP/1.1 101 Switching Protocols\r\n' +
                 'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
                 `Sec-WebSocket-Accept: ` +
                 `${wsAccept(req.headers['sec-websocket-key'])}\r\n\r\n`);
    wsLog.push('open');
    let buf = Buffer.alloc(0);
    socket.on('data', (d) => {
        buf = Buffer.concat([buf, d]);
        for (;;) {
            if (buf.length < 2) {
                return;
            }
            const op = buf[0] & 0x0f;
            let len = buf[1] & 0x7f;
            let off = 2;
            if (len === 126) {
                if (buf.length < 4) {
                    return;
                }
                len = buf.readUInt16BE(2);
                off = 4;
            }
            const masked = (buf[1] & 0x80) !== 0;
            const need = off + (masked ? 4 : 0) + len;
            if (buf.length < need) {
                return;
            }
            const key = masked ? buf.subarray(off, off + 4) : null;
            const payload = Buffer.from(
                buf.subarray(off + (masked ? 4 : 0), need));
            if (key) {
                for (let i = 0; i < payload.length; i++) {
                    payload[i] ^= key[i % 4];
                }
            }
            buf = buf.subarray(need);
            if (op === 1 || op === 2) {
                const text = payload.toString();
                wsLog.push(`msg:${text}`);
                if (text === 'hello') {
                    socket.write(wsFrame(1, Buffer.from('m1')));
                } else if (text === 'e:m1') {
                    // The round trip is complete: close from our side —
                    // the module reports the close state via /close.
                    socket.write(wsFrame(8));
                }
            } else if (op === 8) {
                wsLog.push('close');
                socket.end();
                return;
            } else if (op === 9) {
                socket.write(wsFrame(10, payload));
            }
        }
    });
});

// The denied server: declared but never granted — nothing may ever
// connect. Counting TCP connections catches even a non-HTTP dial.
let deniedConnections = 0;
const deniedSrv = createServer((req, res) => {
    res.writeHead(200, { 'Access-Control-Allow-Origin': '*' });
    res.end('leaked');
});
deniedSrv.on('connection', () => ++deniedConnections);

const [staticPort, apiPort, deniedPort] =
    await Promise.all([staticSrv, apiSrv, deniedSrv].map(listen));

// ---- the test project (written into MEMFS by the page) -------------------

const pad = (p) => String(p).padStart(5, '0');
const origin = `http://127.0.0.1:${pad(apiPort)}`;
const wsOrigin = `ws://127.0.0.1:${pad(apiPort)}`;
const deniedOrigin = `http://127.0.0.1:${pad(deniedPort)}`;

const files = {
    '/probe/scene.json': JSON.stringify({
        version: 1,
        name: 'NetProbe',
        nodes: [
            // The probe's state drives a shader uniform: nodes
            // unreachable from the output are pruned, so the wire is
            // what keeps the module alive (and proves value outputs
            // cross a real browser scene).
            { id: 'probe', type: 'graph',
              graph: 'packages/test.netprobe/graphs/probe.json' },
            { id: 'bg', type: 'shader', shader: 'shaders/bg.wgsl',
              inputs: { state: '@probe' } },
            { id: 'out', type: 'output', inputs: { color: '@bg' } },
        ],
    }),
    '/probe/shaders/bg.wgsl':
        'struct Params { state: f32 }\n' +
        '@group(0) @binding(0) var<uniform> params: Params;\n' +
        '@fragment fn main(@location(0) uv: vec2f) -> @location(0) vec4f {\n' +
        '    return vec4f(uv, params.state * 0.1, 1.0);\n}\n',
    '/probe/packages/test.netprobe/manifest.json': JSON.stringify({
        name: 'test.netprobe',
        version: '1.0.0',
        description: 'browser network-backend probe (tests/web)',
        license: 'MIT',
    }),
    // The grant record: the api origins, NOT the denied one (§4.4
    // declared-but-ungranted must read as the offline face).
    '/probe/packages/test.netprobe/.installed.json': JSON.stringify({
        permissions: { network: { origins: [origin, wsOrigin] } },
    }),
    '/probe/packages/test.netprobe/graphs/probe.json': JSON.stringify({
        version: 1,
        name: 'NetProbe',
        inputs: {},
        outputs: { result: '@probe.state' },
        nodes: [
            { id: 'probe', type: 'module',
              module: 'modules/probe.wasm',
              interface: 'modules/probe.json' },
        ],
    }),
    '/probe/packages/test.netprobe/modules/probe.json': JSON.stringify({
        abi: 1,
        permissions: {
            network: { origins: [origin, wsOrigin, deniedOrigin] },
        },
        inputs: {
            denied: { type: 'scalar', default: deniedPort },
            port: { type: 'scalar', default: apiPort },
        },
        outputs: { state: { type: 'scalar' } },
    }),
};

const wasmB64 = readFileSync(join(here, 'fixtures/netprobe.wasm'))
    .toString('base64');

// ---- drive chrome ---------------------------------------------------------

const userDataDir = mkdtempSync(join(tmpdir(), 'drift-web-net-'));
const chrome = Chrome.launch(chromeBin, { headful, userDataDir });
const consoleLog = [];
chrome.onEvent((msg) => {
    if (msg.method === 'Runtime.consoleAPICalled') {
        consoleLog.push(msg.params.args
            .map((a) => a.value ?? a.description ?? '').join(' '));
    }
});

const failures = [];
const check = (cond, what) => {
    if (!cond) {
        failures.push(what);
    }
};
const sleep = (ms) => new Promise((ok) => setTimeout(ok, ms));
const deadline = Date.now() + timeoutMs;

try {
    const page = await chrome.page();
    await page.send('Runtime.enable', {});
    await page.send('Page.enable', {});
    await page.send('Page.navigate',
                    { url: `http://127.0.0.1:${staticPort}/drift.html` });

    // The app is up once the default scene loads (device + storage
    // preload both gated behind it).
    for (;;) {
        const desc = await page.eval(
            `typeof Module !== 'undefined' && Module.calledRun
                 ? Module.ccall('drift_describe', 'string', [], []) : null`);
        if (desc && JSON.parse(desc).scene) {
            break;
        }
        if (Date.now() > deadline) {
            throw new Error('app did not start (WebGPU device? see console)');
        }
        await sleep(200);
    }

    // Write the test project into MEMFS and open it. The first open
    // usually reports "still compiling" (async WebAssembly compile) —
    // retry until the compile cache has it.
    const inject = await page.eval(`(() => {
        try {
            const files = ${JSON.stringify(files)};
            for (const [p, c] of Object.entries(files)) {
                Module.FS.mkdirTree(p.slice(0, p.lastIndexOf('/')));
                Module.FS.writeFile(p, c);
            }
            const b = atob('${wasmB64}');
            const u = new Uint8Array(b.length);
            for (let i = 0; i < b.length; i++) u[i] = b.charCodeAt(i);
            Module.FS.writeFile(
                '/probe/packages/test.netprobe/modules/probe.wasm', u);
            return 'ok';
        } catch (e) { return 'inject: ' + e; }
    })()`);
    if (inject !== 'ok') {
        throw new Error(inject);
    }

    for (;;) {
        if (await page.eval(
                `Module.ccall('drift_open', 'number', ['string'], ['/probe'])`)) {
            break;
        }
        if (Date.now() > deadline) {
            const errs = await page.eval(
                `Module.ccall('drift_errors', 'string', [], [])`);
            throw new Error(`drift_open never succeeded: ${errs}`);
        }
        await sleep(150);
    }

    // The probe runs on external wakes from here; the terminal /close
    // report ends the test.
    await Promise.race([terminal, sleep(Math.max(0, deadline - Date.now()))
        .then(() => { throw new Error(
            'timed out waiting for the /close report'); })]);

    // ---- assertions -------------------------------------------------------
    const get = (path) => transcript.find(
        (e) => e.path === path && e.method === 'GET');
    check(get('/data'), 'GET /data reached the server');
    check(get('/nocors'),
          'GET /nocors reached the server (CORS blocks responses, ' +
          'not simple requests)');

    const report = transcript.find((e) => e.path === '/report');
    check(report, 'POST /report arrived');
    if (report) {
        check(report.method === 'POST', `/report was ${report.method}`);
        check(report.body === DATA_BODY,
              `/report body was ${JSON.stringify(report.body)}, expected ` +
              `the /data body (mailbox round trip)`);
        check(report.contentType === null,
              `/report carried Content-Type ${report.contentType} — ` +
              'browser POSTs must be header-bare (native parity)');
        check(report.query.d === '2',
              `leak state d=${report.query.d}, expected 2 (Failed): the ` +
              'ungranted origin must read as the offline face');
        check(report.query.c === '2',
              `nocors state c=${report.query.c}, expected 2 (Failed): a ` +
              'CORS-refused response must read as the offline face');
    }

    check(wsLog[0] === 'open' && wsLog[1] === 'msg:hello',
          `ws transcript starts ${JSON.stringify(wsLog.slice(0, 2))}, ` +
          'expected open + hello (the open-wake send)');
    check(wsLog.includes('msg:e:m1'),
          'the module echoed m1 back (message wake -> send)');

    const close = transcript.find((e) => e.path === '/close');
    check(close?.query.w === '3',
          `close report w=${close?.query.w}, expected 3 (Closed): a ` +
          'server-initiated close is a lifecycle wake, not an error');

    check(deniedConnections === 0,
          `the never-granted origin saw ${deniedConnections} connection(s)` +
          ' — policy must fail the handle before the wire');

    check(consoleLog.some((l) => l.includes('not granted')),
          'the ungranted origin surfaced as a load warning (soft-deny)');
} catch (e) {
    failures.push(e.message);
} finally {
    chrome.kill();
    staticSrv.close();
    apiSrv.close();
    deniedSrv.close();
    rmSync(userDataDir, { recursive: true, force: true });
}

if (failures.length) {
    console.error('FAIL: browser network backend');
    for (const f of failures) {
        console.error(`  - ${f}`);
    }
    console.error('\ntranscript:', JSON.stringify(transcript, null, 2));
    console.error('ws:', JSON.stringify(wsLog));
    console.error('\nconsole:');
    for (const l of consoleLog) {
        console.error(`  ${l}`);
    }
    process.exit(1);
}
console.log('PASS: browser network backend ' +
            `(${transcript.length} http entries, ws ${wsLog.length} events)`);
process.exit(0);
