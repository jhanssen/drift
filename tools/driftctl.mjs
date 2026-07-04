#!/usr/bin/env node
// Control client for a running drift (--listen PORT). No dependencies —
// speaks just enough RFC 6455 for the ControlServer protocol (protocol v0:
// describe / set; see src/platform/linux/ControlServer.h).
//
// usage: driftctl.mjs [--port N] describe
//        driftctl.mjs [--port N] set NAME VALUE   (VALUE: number or x,y[,z,w])
//        driftctl.mjs [--port N] pause | resume
//        driftctl.mjs [--port N] seek SECONDS
//        driftctl.mjs [--port N] reload           (re-read scene from disk)
//        driftctl.mjs [--port N] watch            (print events)

import net from 'node:net';
import crypto from 'node:crypto';

const args = process.argv.slice(2);
let port = 7681;
if (args[0] === '--port') {
  args.shift();
  port = Number(args.shift());
}
const command = args.shift();

function usage() {
  console.error('usage: driftctl.mjs [--port N] describe | set NAME VALUE | ' +
                'pause | resume | seek SECONDS | reload | watch');
  process.exit(2);
}

let request;
if (command === 'describe' || command === 'reload') {
  request = { id: 1, method: command };
} else if (command === 'set') {
  const name = args.shift();
  const raw = args.shift();
  if (!name || raw === undefined) usage();
  const parts = raw.split(',').map(Number);
  if (parts.some(Number.isNaN)) usage();
  const value = parts.length === 1 ? parts[0] : parts;
  request = { id: 1, method: 'set', params: { name, value } };
} else if (command === 'pause' || command === 'resume') {
  request = { id: 1, method: 'pause',
              params: { paused: command === 'pause' } };
} else if (command === 'seek') {
  const time = Number(args.shift());
  if (Number.isNaN(time)) usage();
  request = { id: 1, method: 'seek', params: { time } };
} else if (command !== 'watch') {
  usage();
}

function encodeFrame(payload) {
  const data = Buffer.from(payload);
  const mask = crypto.randomBytes(4);
  let header;
  if (data.length < 126) {
    header = Buffer.from([0x81, 0x80 | data.length]);
  } else {
    header = Buffer.alloc(4);
    header[0] = 0x81;
    header[1] = 0x80 | 126;
    header.writeUInt16BE(data.length, 2);
  }
  for (let i = 0; i < data.length; i++) data[i] ^= mask[i % 4];
  return Buffer.concat([header, mask, data]);
}

// Returns [payload, bytesConsumed] or null if incomplete.
function decodeFrame(buf) {
  if (buf.length < 2) return null;
  let len = buf[1] & 0x7f;
  let pos = 2;
  if (len === 126) {
    if (buf.length < 4) return null;
    len = buf.readUInt16BE(2);
    pos = 4;
  } else if (len === 127) {
    if (buf.length < 10) return null;
    len = Number(buf.readBigUInt64BE(2));
    pos = 10;
  }
  if (buf.length < pos + len) return null;
  return [buf.subarray(pos, pos + len).toString(), pos + len];
}

const socket = net.connect(port, '127.0.0.1');
const key = crypto.randomBytes(16).toString('base64');
let upgraded = false;
let buffer = Buffer.alloc(0);

socket.on('connect', () => {
  socket.write(
    `GET / HTTP/1.1\r\nHost: 127.0.0.1:${port}\r\n` +
    'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
    `Sec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`);
});

socket.on('data', (chunk) => {
  buffer = Buffer.concat([buffer, chunk]);
  if (!upgraded) {
    const end = buffer.indexOf('\r\n\r\n');
    if (end < 0) return;
    const head = buffer.subarray(0, end).toString();
    buffer = buffer.subarray(end + 4);
    if (!head.startsWith('HTTP/1.1 101')) {
      console.error(head.split('\r\n')[0]);
      process.exit(1);
    }
    upgraded = true;
    if (request) socket.write(encodeFrame(JSON.stringify(request)));
  }
  for (;;) {
    const frame = decodeFrame(buffer);
    if (!frame) return;
    buffer = buffer.subarray(frame[1]);
    const message = JSON.parse(frame[0]);
    console.log(JSON.stringify(message, null, 2));
    if (request && message.id === request.id) {
      socket.end();
      process.exit(message.error ? 1 : 0);
    }
  }
});

socket.on('error', (err) => {
  console.error(`driftctl: ${err.message}`);
  process.exit(1);
});
