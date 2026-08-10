// A small msgpack codec, bundled rather than depended on.
//
// §12.3: "Bundle a small msgpack encoder rather than taking a dependency. A few
// KB of codec keeps the size story intact and avoids a transitive-dependency
// argument on a package whose pitch is smallness."
//
// It agrees byte for byte with the C implementation in src/dmsgpack.c, and
// test/msgpack.test.js asserts that against vectors generated *by* the C
// implementation rather than against the spec as read by whoever wrote this.
// Cross-implementation agreement is the only property that matters here.
//
// ## What JavaScript cannot preserve, and why it is documented rather than fixed
//
// Lua distinguishes integers from floats; JavaScript does not. `1.0` and `1` are
// the same value here, so a Lua float that happens to be integral arrives as a
// JS number and goes back as a msgpack **integer**. Nothing in this file can
// change that, and pretending otherwise -- a wrapper type for floats, say --
// would cost every host author something to work around a case almost none of
// them have. Use a BigInt when you need an integer past 2^53; use the `Float`
// marker below when you specifically need a float64 on the wire.

/** Force a number onto the wire as float64, whatever its value. */
export class Float {
  constructor(value) {
    this.value = value;
  }
}

/** An ext value: a code in 0x10..0x7f and its raw bytes. */
export class Ext {
  constructor(code, data) {
    this.code = code;
    this.data = data;
  }
}

const MAX_DEPTH = 16;

class Writer {
  constructor() {
    this.buf = new Uint8Array(64);
    this.len = 0;
  }

  need(n) {
    if (this.len + n <= this.buf.length) return;
    let cap = this.buf.length * 2;
    while (cap < this.len + n) cap *= 2;
    const next = new Uint8Array(cap);
    next.set(this.buf.subarray(0, this.len));
    this.buf = next;
  }

  byte(b) {
    this.need(1);
    this.buf[this.len++] = b;
  }

  bytes(src) {
    this.need(src.length);
    this.buf.set(src, this.len);
    this.len += src.length;
  }

  // Big-endian, width bytes. BigInt throughout so 64-bit widths do not lose
  // their low bits to float arithmetic.
  be(value, width) {
    this.need(width);
    let v = BigInt(value);
    for (let i = width - 1; i >= 0; i--) {
      this.buf[this.len + i] = Number(v & 0xffn);
      v >>= 8n;
    }
    this.len += width;
  }

  result() {
    return this.buf.slice(0, this.len);
  }
}

const utf8 = new TextEncoder();
const utf8dec = new TextDecoder("utf-8", { fatal: false });

function writeInt(w, value) {
  const v = BigInt(value);
  if (v >= 0n) {
    if (v <= 127n) w.byte(Number(v));
    else if (v <= 0xffn) { w.byte(0xcc); w.be(v, 1); }
    else if (v <= 0xffffn) { w.byte(0xcd); w.be(v, 2); }
    else if (v <= 0xffffffffn) { w.byte(0xce); w.be(v, 4); }
    else { w.byte(0xcf); w.be(v, 8); }
  } else {
    if (v >= -32n) w.byte(0xe0 | Number(v + 32n));
    else if (v >= -128n) { w.byte(0xd0); w.be(v & 0xffn, 1); }
    else if (v >= -32768n) { w.byte(0xd1); w.be(v & 0xffffn, 2); }
    else if (v >= -2147483648n) { w.byte(0xd2); w.be(v & 0xffffffffn, 4); }
    else { w.byte(0xd3); w.be(v & 0xffffffffffffffffn, 8); }
  }
}

function writeFloat(w, value) {
  // Always float64, matching §5.2: the encoded width follows the type, not the
  // magnitude, so two equal floats always encode identically.
  w.byte(0xcb);
  const b = new Uint8Array(8);
  new DataView(b.buffer).setFloat64(0, value, false);
  w.bytes(b);
}

function writeStr(w, s) {
  const b = utf8.encode(s);
  if (b.length < 32) w.byte(0xa0 | b.length);
  else if (b.length <= 0xff) { w.byte(0xd9); w.be(b.length, 1); }
  else if (b.length <= 0xffff) { w.byte(0xda); w.be(b.length, 2); }
  else { w.byte(0xdb); w.be(b.length, 4); }
  w.bytes(b);
}

function writeBin(w, b) {
  if (b.length <= 0xff) { w.byte(0xc4); w.be(b.length, 1); }
  else if (b.length <= 0xffff) { w.byte(0xc5); w.be(b.length, 2); }
  else { w.byte(0xc6); w.be(b.length, 4); }
  w.bytes(b);
}

function writeArrayHeader(w, n) {
  if (n <= 15) w.byte(0x90 | n);
  else if (n <= 0xffff) { w.byte(0xdc); w.be(n, 2); }
  else { w.byte(0xdd); w.be(n, 4); }
}

function writeMapHeader(w, n) {
  if (n <= 15) w.byte(0x80 | n);
  else if (n <= 0xffff) { w.byte(0xde); w.be(n, 2); }
  else { w.byte(0xdf); w.be(n, 4); }
}

function writeExt(w, code, data) {
  const n = data.length;
  if (n === 1) w.byte(0xd4);
  else if (n === 2) w.byte(0xd5);
  else if (n === 4) w.byte(0xd6);
  else if (n === 8) w.byte(0xd7);
  else if (n === 16) w.byte(0xd8);
  else if (n <= 0xff) { w.byte(0xc7); w.be(n, 1); }
  else if (n <= 0xffff) { w.byte(0xc8); w.be(n, 2); }
  else { w.byte(0xc9); w.be(n, 4); }
  w.byte(code & 0xff);
  w.bytes(data);
}

function writeValue(w, v, depth, path) {
  if (depth > MAX_DEPTH) {
    throw new TypeError(
      `msgpack: nesting deeper than ${MAX_DEPTH}, or a cycle, at ${path || "the value itself"}`
    );
  }
  if (v === null || v === undefined) return w.byte(0xc0);
  switch (typeof v) {
    case "boolean":
      return w.byte(v ? 0xc3 : 0xc2);
    case "bigint":
      return writeInt(w, v);
    case "number":
      // No integer/float distinction in JS, so an integral number goes out as
      // an integer. See the note at the top of this file.
      if (Number.isInteger(v) && Object.is(v, Math.trunc(v)) && !Object.is(v, -0)) {
        return writeInt(w, v);
      }
      return writeFloat(w, v);
    case "string":
      return writeStr(w, v);
    default:
      break;
  }
  if (v instanceof Float) return writeFloat(w, v.value);
  if (v instanceof Ext) return writeExt(w, v.code, v.data);
  if (v instanceof Uint8Array) return writeBin(w, v);
  if (Array.isArray(v)) {
    writeArrayHeader(w, v.length);
    for (let i = 0; i < v.length; i++) {
      writeValue(w, v[i], depth + 1, `${path}[${i}]`);
    }
    return;
  }
  if (v instanceof Map) {
    writeMapHeader(w, v.size);
    for (const [k, val] of v) {
      writeValue(w, k, depth + 1, path);
      writeValue(w, val, depth + 1, path ? `${path}.${String(k)}` : String(k));
    }
    return;
  }
  if (typeof v === "object") {
    const keys = Object.keys(v);
    writeMapHeader(w, keys.length);
    for (const k of keys) {
      writeStr(w, k);
      writeValue(w, v[k], depth + 1, path ? `${path}.${k}` : k);
    }
    return;
  }
  throw new TypeError(
    `msgpack: cannot encode a ${typeof v} at ${path || "the value itself"}`
  );
}

/** Encode a value to msgpack. Throws on anything unencodable, naming the path. */
export function encode(value) {
  const w = new Writer();
  writeValue(w, value, 0, "");
  return w.result();
}

class Reader {
  constructor(bytes, offset) {
    this.b = bytes;
    this.i = offset || 0;
    this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  }

  need(n) {
    if (this.i + n > this.b.length) throw new RangeError("msgpack: truncated input");
  }

  u8() {
    this.need(1);
    return this.b[this.i++];
  }

  be(width, signed) {
    this.need(width);
    let v = 0n;
    for (let k = 0; k < width; k++) v = (v << 8n) | BigInt(this.b[this.i + k]);
    this.i += width;
    if (signed && width < 8) {
      const sign = 1n << BigInt(width * 8 - 1);
      if (v & sign) v -= sign << 1n;
    } else if (signed) {
      if (v & (1n << 63n)) v -= 1n << 64n;
    }
    // Stay a Number while it is exact; become a BigInt only when it must.
    return v >= -9007199254740991n && v <= 9007199254740991n ? Number(v) : v;
  }

  raw(n) {
    this.need(n);
    const out = this.b.subarray(this.i, this.i + n);
    this.i += n;
    return out;
  }

  str(n) {
    return utf8dec.decode(this.raw(n));
  }
}

function readValue(r, depth) {
  if (depth > MAX_DEPTH) throw new RangeError(`msgpack: nesting deeper than ${MAX_DEPTH}`);
  const t = r.u8();
  if (t <= 0x7f) return t;
  if (t >= 0xe0) return t - 256;
  if ((t & 0xe0) === 0xa0) return r.str(t & 0x1f);
  if ((t & 0xf0) === 0x90) return readArray(r, t & 0x0f, depth);
  if ((t & 0xf0) === 0x80) return readMap(r, t & 0x0f, depth);
  switch (t) {
    case 0xc0: return null;
    case 0xc2: return false;
    case 0xc3: return true;
    case 0xcc: return r.be(1, false);
    case 0xcd: return r.be(2, false);
    case 0xce: return r.be(4, false);
    case 0xcf: return r.be(8, false);
    case 0xd0: return r.be(1, true);
    case 0xd1: return r.be(2, true);
    case 0xd2: return r.be(4, true);
    case 0xd3: return r.be(8, true);
    case 0xca: { r.need(4); const v = r.view.getFloat32(r.i, false); r.i += 4; return v; }
    case 0xcb: { r.need(8); const v = r.view.getFloat64(r.i, false); r.i += 8; return v; }
    case 0xd9: return r.str(r.be(1, false));
    case 0xda: return r.str(r.be(2, false));
    case 0xdb: return r.str(r.be(4, false));
    // bin decodes to bytes, not text: a host that meant bytes should get bytes.
    case 0xc4: return r.raw(r.be(1, false)).slice();
    case 0xc5: return r.raw(r.be(2, false)).slice();
    case 0xc6: return r.raw(r.be(4, false)).slice();
    case 0xdc: return readArray(r, r.be(2, false), depth);
    case 0xdd: return readArray(r, r.be(4, false), depth);
    case 0xde: return readMap(r, r.be(2, false), depth);
    case 0xdf: return readMap(r, r.be(4, false), depth);
    case 0xd4: case 0xd5: case 0xd6: case 0xd7: case 0xd8:
      return readExt(r, 1 << (t - 0xd4));
    case 0xc7: return readExt(r, r.be(1, false));
    case 0xc8: return readExt(r, r.be(2, false));
    case 0xc9: return readExt(r, r.be(4, false));
    default:
      throw new TypeError(
        `msgpack: byte 0x${t.toString(16).padStart(2, "0")} is not a valid msgpack type`
      );
  }
}

function readExt(r, len) {
  const code = r.u8();
  const data = r.raw(len).slice();
  // The registry in §5.5. Reserved codes fail by name, because a generic
  // "unknown ext" tells a programmer nothing about which unfinished feature they
  // just met.
  if (code >= 0x10 && code <= 0x7f) return new Ext(code, data);
  const hex = `0x${code.toString(16).padStart(2, "0")}`;
  if (code === 0x01) {
    throw new TypeError("msgpack: ext 0x01 is a decimal value; decQuad is not implemented yet");
  }
  if (code === 0x02) return new Ext(code, data); // endpoint, opaque without a resolver
  if (code >= 0x03 && code <= 0x07) {
    throw new TypeError(`msgpack: ext ${hex} is only valid inside a snapshot stream`);
  }
  throw new TypeError(`msgpack: ext ${hex} is reserved for Diluvium and not assigned`);
}

function readArray(r, n, depth) {
  const out = new Array(n);
  for (let i = 0; i < n; i++) out[i] = readValue(r, depth + 1);
  return out;
}

function readMap(r, n, depth) {
  // A plain object when every key is a string, which is what a JS host wants,
  // and a Map otherwise, because Lua allows any key and dropping the others
  // would silently lose data.
  const entries = [];
  let allStrings = true;
  for (let i = 0; i < n; i++) {
    const k = readValue(r, depth + 1);
    const v = readValue(r, depth + 1);
    if (typeof k !== "string") allStrings = false;
    entries.push([k, v]);
  }
  if (!allStrings) return new Map(entries);
  const out = {};
  for (const [k, v] of entries) out[k] = v;
  return out;
}

/**
 * Decode one value. With `offset`, returns `[value, nextOffset]` so a stream of
 * messages can be walked; without it, just the value.
 */
export function decode(bytes, offset) {
  const r = new Reader(bytes, offset === undefined ? 0 : offset);
  const v = readValue(r, 0);
  return offset === undefined ? v : [v, r.i];
}
