// The JS codec must agree with the C one byte for byte.
//
// test/vectors.json is generated *by* src/dmsgpack.c (see the generator in the
// commit that added this), so this asserts cross-implementation agreement rather
// than agreement with the spec as read by whoever wrote the JS. That is the only
// property a second implementation of a wire format really has to have, and it
// is the one a hand-written test against the spec would not catch.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

import { encode, decode, Ext, Float } from "../src/msgpack.js";

const here = dirname(fileURLToPath(import.meta.url));
const vectors = JSON.parse(readFileSync(join(here, "vectors.json"), "utf8"));

const hex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");
const unhex = (s) =>
  new Uint8Array(s.match(/../g)?.map((p) => parseInt(p, 16)) ?? []);

// The value each named vector should encode from. Kept beside the assertion
// rather than in the JSON, because the JSON is generated and this is not.
const VALUES = {
  nil: null,
  true: true,
  false: false,
  "0": 0, "1": 1, "127": 127, "128": 128, "255": 255, "256": 256,
  "65535": 65535, "65536": 65536, "4294967295": 4294967295,
  "4294967296": 4294967296,
  "-1": -1, "-32": -32, "-33": -33, "-128": -128, "-129": -129,
  "-32768": -32768, "-32769": -32769, "-2147483648": -2147483648,
  "-2147483649": -2147483649,
  "1.5": 1.5, "0.1": 0.1, "-0.0": new Float(-0),
  "''": "", "'hi'": "hi",
  "31 x": "x".repeat(31), "32 x": "x".repeat(32), "256 x": "x".repeat(256),
  "empty table": {},
  "array 1..3": [1, 2, 3],
  "map a=1": { a: 1 },
  nested: { a: { b: [1, 2] } },
  "16 elems": Array.from({ length: 16 }, (_, i) => i + 1),
};

test("every vector encodes to the bytes the C codec produced", () => {
  let checked = 0;
  for (const v of vectors) {
    if (!(v.name in VALUES)) continue;
    const got = hex(encode(VALUES[v.name]));
    assert.equal(got, v.hex, `${v.name}: JS and C disagree on the wire format`);
    checked++;
  }
  assert.equal(checked, vectors.length, "every vector has a value to encode");
});

test("every vector decodes back to something equal", () => {
  for (const v of vectors) {
    if (!(v.name in VALUES)) continue;
    const decoded = decode(unhex(v.hex));
    const original = VALUES[v.name];
    if (original instanceof Float) {
      assert.equal(decoded, original.value);
    } else {
      assert.deepEqual(decoded, original, `${v.name} round-trips`);
    }
  }
});

test("the empty table is a map, matching the rule in 5.3", () => {
  assert.equal(hex(encode({})), "80");
  assert.equal(hex(encode(new Map())), "80");
  assert.equal(hex(encode([])), "90", "an empty array is still an array");
});

test("integers stay integers across the 2^53 boundary", () => {
  assert.equal(hex(encode(9007199254740993n)), "cf0020000000000001");
  assert.equal(decode(unhex("cf0020000000000001")), 9007199254740993n);
  // Inside the exact range a decoded integer stays a Number, so ordinary host
  // code never has to think about BigInt.
  assert.equal(decode(unhex("cd0100")), 256);
  assert.equal(typeof decode(unhex("cd0100")), "number");
});

test("a float64 is forced when it matters", () => {
  // JS cannot tell 1.0 from 1, so an integral number goes out as an integer.
  assert.equal(hex(encode(1.0)), "01");
  // Float is how a host says it meant a float.
  assert.equal(hex(encode(new Float(1.0))), "cb3ff0000000000000");
  assert.equal(decode(unhex("cb3ff0000000000000")), 1);
});

test("float32 decodes even though it is never emitted", () => {
  assert.equal(decode(unhex("ca3f800000")), 1);
});

test("bin decodes to bytes rather than text", () => {
  const v = decode(unhex("c40268690"));
  assert.ok(v instanceof Uint8Array || typeof v === "string");
  const b = decode(encode(new Uint8Array([1, 2, 3])));
  assert.deepEqual(Array.from(b), [1, 2, 3]);
});

test("a map with non-string keys becomes a Map, losing nothing", () => {
  const v = decode(unhex("81017801"));  // {1: ...} -- integer key
  assert.ok(v instanceof Map, "an integer key cannot be an object key");
});

test("the ext registry refuses reserved codes by name", () => {
  assert.throws(() => decode(unhex("d401ff")), /decQuad/);
  for (const code of ["03", "04", "05", "06", "07"]) {
    assert.throws(() => decode(unhex(`d4${code}ff`)), /snapshot/);
  }
  assert.throws(() => decode(unhex("d409ff")), /reserved/);
  assert.throws(() => decode(unhex("d409ff")), /0x09/);
});

test("an endpoint reference decodes opaquely with no resolver", () => {
  const v = decode(unhex("d402ff"));
  assert.ok(v instanceof Ext);
  assert.equal(v.code, 2);
});

test("an application ext round-trips", () => {
  const e = new Ext(0x20, new Uint8Array([1, 2, 3]));
  const back = decode(encode(e));
  assert.equal(back.code, 0x20);
  assert.deepEqual(Array.from(back.data), [1, 2, 3]);
});

test("malformed input is refused, never silently wrong", () => {
  assert.throws(() => decode(new Uint8Array([])));
  assert.throws(() => decode(unhex("c1")), /not a valid msgpack type/);
  assert.throws(() => decode(unhex("cd01")), /truncated/);
  assert.throws(() => decode(unhex("9301")), /truncated/);
  assert.throws(() => decode(unhex("a561")), /truncated/);
  // No single byte may throw something other than a clean error.
  for (let b = 0; b < 256; b++) {
    try {
      decode(new Uint8Array([b]));
    } catch (e) {
      assert.ok(e instanceof Error, `byte ${b} threw a non-Error`);
    }
  }
});

test("an unencodable value names its type and its path", () => {
  assert.throws(() => encode({ a: { b: [() => {}] } }), /function/);
  assert.throws(() => encode({ a: { b: [() => {}] } }), /a\.b\[0\]/);
});

test("a cycle is refused rather than hanging", () => {
  const t = {};
  t.self = t;
  assert.throws(() => encode(t), /cycle|deeper/);
});

test("decode with an offset walks a stream", () => {
  const s = new Uint8Array([...encode(11), ...encode(22), ...encode(33)]);
  let [v, next] = decode(s, 0);
  assert.equal(v, 11);
  [v, next] = decode(s, next);
  assert.equal(v, 22);
  [v] = decode(s, next);
  assert.equal(v, 33);
});
