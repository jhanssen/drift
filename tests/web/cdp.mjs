// Minimal Chrome DevTools Protocol client over --remote-debugging-pipe:
// NUL-delimited JSON on the child's fds 3 (commands in) and 4 (events/
// replies out). Pipe mode needs no debug port and no WebSocket client,
// so the harness stays dependency-free and can't race another Chrome.
import { spawn } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

export class Chrome {
    #child; #buf = ''; #id = 0; #pending = new Map(); #listeners = [];

    constructor(child) {
        this.#child = child;
        child.stdio[4].setEncoding('utf8');
        child.stdio[4].on('data', (d) => this.#onData(d));
    }

    // Extra flags welcome; the caller owns userDataDir cleanup. Unset, it
    // falls back to a fresh temp dir — interpolating undefined would make
    // Chrome dump a profile into a literal ./undefined in the cwd.
    static launch(binary, { headful = false, userDataDir, flags = [] } = {}) {
        userDataDir ??= mkdtempSync(join(tmpdir(), 'drift-cdp-'));
        const args = [
            '--remote-debugging-pipe',
            '--no-first-run',
            '--no-default-browser-check',
            '--disable-background-networking',
            `--user-data-dir=${userDataDir}`,
            '--enable-unsafe-webgpu',
            '--enable-features=Vulkan',
            ...(headful ? [] : ['--headless=new']),
            ...flags,
            'about:blank',
        ];
        const child = spawn(binary, args, {
            stdio: ['ignore', 'ignore', 'pipe', 'pipe', 'pipe'],
        });
        return new Chrome(child);
    }

    #onData(d) {
        this.#buf += d;
        for (;;) {
            const nul = this.#buf.indexOf('\0');
            if (nul < 0) {
                return;
            }
            const msg = JSON.parse(this.#buf.slice(0, nul));
            this.#buf = this.#buf.slice(nul + 1);
            if (msg.id !== undefined && this.#pending.has(msg.id)) {
                const { resolve, reject } = this.#pending.get(msg.id);
                this.#pending.delete(msg.id);
                if (msg.error) {
                    reject(new Error(`${msg.error.message}`));
                } else {
                    resolve(msg.result);
                }
            } else if (msg.method) {
                for (const l of this.#listeners) {
                    l(msg);
                }
            }
        }
    }

    send(method, params = {}, sessionId) {
        const id = ++this.#id;
        const msg = { id, method, params };
        if (sessionId) {
            msg.sessionId = sessionId;
        }
        return new Promise((resolve, reject) => {
            this.#pending.set(id, { resolve, reject });
            this.#child.stdio[3].write(JSON.stringify(msg) + '\0');
        });
    }

    onEvent(fn) {
        this.#listeners.push(fn);
    }

    // Attaches to the boot about:blank page; returns a session facade.
    async page() {
        const { targetInfos } = await this.send('Target.getTargets');
        const page = targetInfos.find((t) => t.type === 'page');
        if (!page) {
            throw new Error('no page target');
        }
        const { sessionId } = await this.send('Target.attachToTarget', {
            targetId: page.targetId,
            flatten: true,
        });
        const chrome = this;
        return {
            sessionId,
            send: (method, params) => chrome.send(method, params, sessionId),
            // Evaluates in the page; returns the value, throws on JS
            // exceptions. Promises are awaited.
            async eval(expression) {
                const r = await chrome.send('Runtime.evaluate', {
                    expression,
                    awaitPromise: true,
                    returnByValue: true,
                }, sessionId);
                if (r.exceptionDetails) {
                    const e = r.exceptionDetails;
                    throw new Error(`page: ${e.exception?.description ??
                                            e.text}`);
                }
                return r.result?.value;
            },
        };
    }

    kill() {
        this.#child.kill('SIGKILL');
    }
}
