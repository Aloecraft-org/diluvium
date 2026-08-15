#!/usr/bin/env node
/*
** rest_plugin.mjs
** The rest plugin, in JavaScript, for the two places JavaScript hosts
** Diluvium: Node beside the native host, and a Worker inside Lab.
**
** One file, two transports, one protocol. That is the claim doc/BUILD8.md
** section 5.2 makes and this file is what makes it checkable: the frame
** bodies are byte-identical to the ones plugins/rest/rest_plugin.c writes,
** so the same manifest describes both and a deployment swaps one for the
** other without a guest noticing.
**
**   node rest_plugin.mjs          -- speaks frames on fd 3, like the C plugin.
**                                    Usable by dist/diluvium-host TODAY; the
**                                    manifest's exec points at this file
**                                    (it has a shebang and needs +x).
**
**   import inside a Worker        -- speaks the same frame bodies as
**                                    structured-clone Uint8Arrays over
**                                    postMessage. This is the Lab binding.
**
** It includes nothing from this repository, deliberately -- not even
** bindings/js/src/msgpack.js. A plugin is defined by the protocol.
**
** What differs from the C plugin, and belongs in a deployment's thinking
** rather than in a surprise: a browser cannot set the Host header, cannot
** present a client certificate, and is subject to CORS. The reachable URL
** set is smaller here. Under Node none of that applies.
*/

const VERSION = 1;
const CHANNEL_FD = 3;
const FRAME_MAX = 8 * 1024 * 1024;
const DEFAULT_MS = 15000;

/* ------------------------------------------------------------- msgpack --
** Only what a frame needs. Maps decode to plain objects, str to string,
** bin to Uint8Array -- which is the distinction that lets a body cross as
** bytes instead of as base64.
*/

class Reader {
  constructor (buf) { this.b = buf; this.i = 0; this.v = new DataView(buf.buffer, buf.byteOffset, buf.byteLength); }
  u8 () { return this.b[this.i++]; }
  read () {
    const c = this.u8();
    if (c <= 0x7f) return c;
    if (c >= 0xe0) return c - 256;
    if ((c & 0xe0) === 0xa0) return this.str(c & 0x1f);
    if ((c & 0xf0) === 0x80) return this.map(c & 0x0f);
    if ((c & 0xf0) === 0x90) return this.arr(c & 0x0f);
    switch (c) {
      case 0xc0: return null;
      case 0xc2: return false;
      case 0xc3: return true;
      case 0xc4: return this.bin(this.n(1));
      case 0xc5: return this.bin(this.n(2));
      case 0xc6: return this.bin(this.n(4));
      case 0xca: { const v = this.v.getFloat32(this.i); this.i += 4; return v; }
      case 0xcb: { const v = this.v.getFloat64(this.i); this.i += 8; return v; }
      case 0xcc: return this.n(1);
      case 0xcd: return this.n(2);
      case 0xce: return this.n(4);
      case 0xcf: { const v = this.v.getBigUint64(this.i); this.i += 8; return Number(v); }
      case 0xd0: { const v = this.v.getInt8(this.i); this.i += 1; return v; }
      case 0xd1: { const v = this.v.getInt16(this.i); this.i += 2; return v; }
      case 0xd2: { const v = this.v.getInt32(this.i); this.i += 4; return v; }
      case 0xd3: { const v = this.v.getBigInt64(this.i); this.i += 8; return Number(v); }
      case 0xd9: return this.str(this.n(1));
      case 0xda: return this.str(this.n(2));
      case 0xdb: return this.str(this.n(4));
      case 0xdc: return this.arr(this.n(2));
      case 0xdd: return this.arr(this.n(4));
      case 0xde: return this.map(this.n(2));
      case 0xdf: return this.map(this.n(4));
      default: throw new Error(`msgpack: byte 0x${c.toString(16)} is not one this reader knows`);
    }
  }
  n (w) {
    let v = 0;
    for (let k = 0; k < w; k++) v = v * 256 + this.b[this.i++];
    return v;
  }
  str (n) { const s = new TextDecoder().decode(this.b.subarray(this.i, this.i + n)); this.i += n; return s; }
  bin (n) { const s = this.b.slice(this.i, this.i + n); this.i += n; return s; }
  arr (n) { const a = []; for (let k = 0; k < n; k++) a.push(this.read()); return a; }
  map (n) { const o = {}; for (let k = 0; k < n; k++) { const key = this.read(); o[key] = this.read(); } return o; }
}

class Writer {
  constructor () { this.parts = []; this.len = 0; }
  raw (u) { this.parts.push(u); this.len += u.length; }
  byte (c) { this.raw(Uint8Array.of(c)); }
  be (v, w) { const u = new Uint8Array(w); for (let k = w - 1; k >= 0; k--) { u[k] = v & 0xff; v = Math.floor(v / 256); } this.raw(u); }
  nil () { this.byte(0xc0); }
  bool (b) { this.byte(b ? 0xc3 : 0xc2); }
  int (v) {
    if (Number.isInteger(v) && v >= 0 && v <= 0x7f) return this.byte(v);
    if (Number.isInteger(v) && v < 0 && v >= -32) return this.byte(0xe0 | (v + 32));
    if (Number.isInteger(v) && v >= 0) { this.byte(0xcf); return this.be(v, 8); }
    if (Number.isInteger(v)) { this.byte(0xd3); const u = new Uint8Array(8); new DataView(u.buffer).setBigInt64(0, BigInt(v)); return this.raw(u); }
    this.byte(0xcb); const u = new Uint8Array(8); new DataView(u.buffer).setFloat64(0, v); return this.raw(u);
  }
  str (s) {
    const u = new TextEncoder().encode(s);
    /* fixstr stops at 31; the next byte is nil, so overrunning it changes
       type rather than truncating. The C plugin had this bug once. */
    if (u.length < 32) this.byte(0xa0 | u.length);
    else if (u.length < 256) { this.byte(0xd9); this.be(u.length, 1); }
    else if (u.length < 65536) { this.byte(0xda); this.be(u.length, 2); }
    else { this.byte(0xdb); this.be(u.length, 4); }
    this.raw(u);
  }
  bin (u) {
    if (u.length < 256) { this.byte(0xc4); this.be(u.length, 1); }
    else if (u.length < 65536) { this.byte(0xc5); this.be(u.length, 2); }
    else { this.byte(0xc6); this.be(u.length, 4); }
    this.raw(u);
  }
  map (n) { if (n < 16) this.byte(0x80 | n); else { this.byte(0xde); this.be(n, 2); } }
  any (v) {
    if (v === null || v === undefined) return this.nil();
    if (typeof v === 'boolean') return this.bool(v);
    if (typeof v === 'number') return this.int(v);
    if (typeof v === 'string') return this.str(v);
    if (v instanceof Uint8Array) return this.bin(v);
    const keys = Object.keys(v);
    this.map(keys.length);
    for (const k of keys) { this.str(k); this.any(v[k]); }
  }
  bytes () {
    const out = new Uint8Array(this.len);
    let o = 0;
    for (const p of this.parts) { out.set(p, o); o += p.length; }
    return out;
  }
}

function encodeReply (id, value, error) {
  const w = new Writer();
  w.map(4);
  w.str('version'); w.int(VERSION);
  w.str('id');      w.int(id);
  w.str('final');   w.bool(true);
  if (error) { w.str('error'); w.any(error); }
  else { w.str('value'); w.any(value); }
  return w.bytes();
}

/* --------------------------------------------------------------- the work */

const ERR = { TRANSPORT: 'transport', PLUGIN: 'plugin', CAPABILITY: 'capability' };

function fail (cls, code, message) { return { class: cls, code, message }; }

async function handle (target, args) {
  if (target !== 'get' && target !== 'post')
    return { error: fail(ERR.PLUGIN, 'bad_target', "this plugin answers 'get' and 'post'") };
  const a = args || {};
  if (typeof a.url !== 'string' || a.url === '')
    return { error: fail(ERR.PLUGIN, 'bad_args', "the call needs a 'url' string") };

  let u;
  try { u = new URL(a.url); }
  catch { return { error: fail(ERR.PLUGIN, 'bad_args', 'the url did not parse') }; }
  if (u.protocol !== 'http:' && u.protocol !== 'https:')
    return { error: fail(ERR.PLUGIN, 'bad_args', 'the url must begin http:// or https://') };
  if (u.username || u.password)
    return { error: fail(ERR.PLUGIN, 'bad_args',
      'credentials in a url are refused; send an Authorization header instead') };

  const headers = {};
  for (const [k, v] of Object.entries(a.headers || {})) {
    if (typeof v !== 'string') continue;
    /* Refused, never stripped: silently changing what was sent is worse
       than refusing to send it. */
    if (/[\r\n]/.test(k) || /[\r\n]/.test(v) || k.includes(':'))
      return { error: fail(ERR.PLUGIN, 'bad_args',
        'a header name or value contained CR, LF or a colon; that is refused rather than stripped') };
    const lower = k.toLowerCase();
    if (lower === 'host' || lower === 'content-length' || lower === 'connection') continue;
    headers[k] = v;
  }

  const timeout = (Number.isInteger(a.timeout_ms) && a.timeout_ms > 0 && a.timeout_ms <= 120000)
    ? a.timeout_ms : DEFAULT_MS;
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), timeout);
  try {
    const init = { method: target === 'post' ? 'POST' : 'GET', headers, signal: ctl.signal, redirect: 'follow' };
    if (target === 'post') init.body = a.body instanceof Uint8Array ? a.body : new TextEncoder().encode(String(a.body ?? ''));
    const res = await fetch(u.toString(), init);
    const body = new Uint8Array(await res.arrayBuffer());
    /* A non-2xx is an ANSWER, not an error: the guest asked what the service
       said, and 404 is what it said. */
    return { value: { status: res.status, content_type: res.headers.get('content-type'), body } };
  }
  catch (e) {
    if (e && e.name === 'AbortError')
      return { error: fail(ERR.CAPABILITY, 'timeout', `no answer within ${timeout} ms`) };
    const msg = String(e && e.message || e);
    const code = /getaddrinfo|ENOTFOUND|dns/i.test(msg) ? 'dns'
               : /certificate|TLS|SSL/i.test(msg) ? 'tls'
               : 'timeout';
    return { error: fail(ERR.CAPABILITY, code, msg) };
  }
  finally { clearTimeout(timer); }
}

async function dispatch (frame) {
  let req;
  try { req = new Reader(frame).read(); }
  catch { return null; }
  if (!req || typeof req.id !== 'number') return null;   /* uncorrelatable */
  const out = await handle(req.target, req.args);
  return encodeReply(req.id, out.value, out.error);
}

/* ----------------------------------------------------------- transports */

/* Lab: structured-clone Uint8Arrays over postMessage. No length prefix --
   postMessage preserves message boundaries, which is the one thing a byte
   stream does not, so framing is the transport's job and not ours here. */
function serveWorker (scope) {
  scope.onmessage = async (ev) => {
    const frame = ev.data instanceof Uint8Array ? ev.data : new Uint8Array(ev.data);
    const reply = await dispatch(frame);
    if (reply) scope.postMessage(reply, [reply.buffer]);
  };
}

/* Node: length-prefixed frames on fd 3, byte-for-byte what the C host
   writes and reads. Requests are handled one at a time in arrival order,
   which is what max_inflight bounds on the other side. */
async function serveFd () {
  const fs = await import('node:fs');
  let buf = Buffer.alloc(0);
  let busy = Promise.resolve();

  const drain = () => {
    while (buf.length >= 4) {
      const flen = buf.readUInt32BE(0);
      if (flen === 0 || flen > FRAME_MAX) { process.exit(1); }
      if (buf.length < 4 + flen) break;
      const frame = new Uint8Array(buf.subarray(4, 4 + flen));
      buf = buf.subarray(4 + flen);
      busy = busy.then(async () => {
        const reply = await dispatch(frame);
        if (!reply) return;
        const hdr = Buffer.alloc(4);
        hdr.writeUInt32BE(reply.length, 0);
        await new Promise((res, rej) =>
          fs.write(CHANNEL_FD, Buffer.concat([hdr, Buffer.from(reply)]), (e) => e ? rej(e) : res()));
      }).catch(() => process.exit(1));
    }
  };

  const stream = fs.createReadStream(null, { fd: CHANNEL_FD, autoClose: false });
  stream.on('data', (chunk) => { buf = Buffer.concat([buf, chunk]); drain(); });
  stream.on('end', () => busy.then(() => process.exit(0)));
  stream.on('error', () => process.exit(1));
}

const inWorker = typeof self !== 'undefined' && typeof self.postMessage === 'function'
                 && typeof process === 'undefined';
if (inWorker) serveWorker(self);
else serveFd();

export { dispatch, handle, Reader, Writer, encodeReply, serveWorker };
