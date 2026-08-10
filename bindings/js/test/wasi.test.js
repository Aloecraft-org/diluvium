// The WASI preview-1 host, checked without the wasi-sdk.
//
// This exists because of a CI failure that lasted weeks and could not be seen
// here: `npm test` passed, and the only thing that ever instantiated a real module
// was a job needing a container running the pinned wasi-sdk. The wrapper
// instantiated with `{}` imports and died on
// `Import #0 module="wasi_snapshot_preview1"` before any Lua ran.
//
// The module below is assembled byte by byte rather than compiled from WAT,
// because this package has no dependencies and adding wabt to check the import
// object would be a strange trade. It is small: three imports, two exports, and no
// instruction that is not a constant or a call.

import { test } from "node:test";
import assert from "node:assert/strict";

import { wasiPreview1, WasiExit, WASI_MODULE } from "../src/wasi.js";

// -- a hand-assembled probe module --------------------------------------------

/** Unsigned LEB128, which is how wasm writes every length and index. */
const uleb = (n) => {
  const out = [];
  do {
    let b = n & 0x7f;
    n >>>= 7;
    if (n !== 0) b |= 0x80;
    out.push(b);
  } while (n !== 0);
  return out;
};

/** A section: id, then its byte length, then its contents. */
const section = (id, body) => [id, ...uleb(body.length), ...body];

/** A vector: a count, then the elements. */
const vec = (items) => [...uleb(items.length), ...items.flat()];

const str = (s) => [...uleb(s.length), ...[...s].map((c) => c.charCodeAt(0))];

const I32 = 0x7f;
const FUNC = 0x60;

function probeModule() {
  // (i32,i32,i32,i32)->i32 for fd_write, (i32,i32)->i32 for random_get,
  // ()->() for an import this file deliberately does not implement, ()->i32 for
  // the two exported probes.
  const types = section(1, vec([
    [FUNC, ...vec([[I32], [I32], [I32], [I32]]), ...vec([[I32]])],
    [FUNC, ...vec([[I32], [I32]]), ...vec([[I32]])],
    [FUNC, ...vec([]), ...vec([])],
    [FUNC, ...vec([]), ...vec([[I32]])],
  ]));

  // Imported functions come first in the index space: 0, 1, 2.
  const imports = section(2, vec([
    [...str(WASI_MODULE), ...str("fd_write"), 0x00, 0],
    [...str(WASI_MODULE), ...str("random_get"), 0x00, 1],
    // Not implemented in wasi.js on purpose: `complete()` has to fill it in, or
    // instantiation fails. This is the wasi-sdk-bump case, in miniature.
    [...str(WASI_MODULE), ...str("some_future_call"), 0x00, 2],
  ]));

  // Two local functions, indices 3 and 4, both of type 3.
  const funcs = section(3, vec([[3], [3]]));
  const memory = section(5, vec([[0x00, 1]])); // one page, no maximum
  const exports = section(7, vec([
    [...str("memory"), 0x02, 0],
    [...str("probe"), 0x00, 3],
    [...str("random"), 0x00, 4],
  ]));

  const i32c = (n) => [0x41, ...uleb(n)];
  const call = (f) => [0x10, ...uleb(f)];
  const END = 0x0b;

  // probe(): fd_write(1, iovs=0, iovs_len=1, nwritten=16)
  const probe = [0x00, ...i32c(1), ...i32c(0), ...i32c(1), ...i32c(16), ...call(0), END];
  // random(): random_get(buf=32, len=8)
  const random = [0x00, ...i32c(32), ...i32c(8), ...call(1), END];
  const code = section(10, vec([
    [...uleb(probe.length), ...probe],
    [...uleb(random.length), ...random],
  ]));

  return new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    ...types, ...imports, ...funcs, ...memory, ...exports, ...code,
  ]);
}

const bytes = probeModule();

/** Instantiate the probe through the shim, the way index.js does. */
async function withWasi(opts = {}) {
  const module = await WebAssembly.compile(bytes);
  const wasi = wasiPreview1(opts);
  const stubbed = wasi.complete(module);
  const instance = await WebAssembly.instantiate(module, {
    [WASI_MODULE]: wasi.imports,
  });
  wasi.attach(instance);
  return { instance, wasi, stubbed };
}

// -- the tests ----------------------------------------------------------------

test("the probe module is valid wasm, so a failure below is the shim's", async () => {
  await assert.doesNotReject(WebAssembly.compile(bytes));
});

test("a module importing wasi_snapshot_preview1 instantiates", async () => {
  // The whole CI failure, reduced: with `{}` this is the error users saw.
  const module = await WebAssembly.compile(bytes);
  await assert.rejects(
    WebAssembly.instantiate(module, {}),
    /wasi_snapshot_preview1/,
    "with no imports it must still fail, or this test proves nothing"
  );
  await assert.doesNotReject(withWasi(), "and with the shim it must not");
});

test("an import the shim does not implement is stubbed, not left missing", async () => {
  const { stubbed } = await withWasi();
  assert.deepEqual(stubbed, ["some_future_call"]);
});

test("fd_write reaches the host, with the bytes the guest wrote", async () => {
  const seen = [];
  const { instance } = await withWasi({ write: (fd, text) => seen.push([fd, text]) });
  const mem = new Uint8Array(instance.exports.memory.buffer);
  const view = new DataView(instance.exports.memory.buffer);
  // One iovec at 0 pointing at "hi there" placed at 64.
  const text = new TextEncoder().encode("hi there");
  mem.set(text, 64);
  view.setUint32(0, 64, true);
  view.setUint32(4, text.length, true);

  assert.equal(instance.exports.probe(), 0, "fd_write returns ESUCCESS");
  assert.deepEqual(seen, [[1, "hi there"]]);
  assert.equal(view.getUint32(16, true), text.length, "and reports what it wrote");
});

test("fd_write refuses a descriptor that does not exist", async () => {
  // Reaching a real fd_write with fd 3 needs a different module, so this calls the
  // import directly -- which is the same function object the guest gets.
  const { wasi } = await withWasi();
  assert.equal(wasi.imports.fd_write(3, 0, 1, 16), 8, "EBADF");
});

test("random_get fills the buffer it was given", async () => {
  const { instance } = await withWasi();
  const mem = new Uint8Array(instance.exports.memory.buffer);
  mem.fill(0, 32, 40);
  assert.equal(instance.exports.random(), 0, "random_get returns ESUCCESS");
  const filled = mem.subarray(32, 40);
  // A zero result is possible but overwhelmingly unlikely; what is being checked
  // is that the call wrote to the guest's memory at all.
  assert.ok(filled.some((b) => b !== 0), "and it is no longer all zeroes");
  // Nothing outside the range moved.
  assert.equal(mem[31], 0);
  assert.equal(mem[40], 0);
});

test("clock_time_get writes a plausible nanosecond epoch", async () => {
  const { instance, wasi } = await withWasi();
  const before = BigInt(Date.now()) * 1000000n;
  assert.equal(wasi.imports.clock_time_get(0, 0n, 128), 0);
  const got = new DataView(instance.exports.memory.buffer).getBigUint64(128, true);
  assert.ok(got >= before, "not earlier than the moment before the call");
  assert.ok(got < before + 60_000_000_000n, "and not a minute in the future");
});

test("there is no environment, no arguments, and no preopened directory", async () => {
  const { instance, wasi } = await withWasi();
  const v = new DataView(instance.exports.memory.buffer);
  assert.equal(wasi.imports.environ_sizes_get(200, 204), 0);
  assert.equal(v.getUint32(200, true), 0, "no environment variables");
  assert.equal(wasi.imports.args_sizes_get(208, 212), 0);
  assert.equal(v.getUint32(208, true), 0, "no arguments");
  // EBADF at index 0 is how libc learns the descriptor table is empty. A sandbox
  // that answered ESUCCESS here would be claiming a directory it cannot serve.
  assert.equal(wasi.imports.fd_prestat_get(0, 216), 8, "no preopened directories");
});

test("proc_exit throws rather than returning, so a caller can tell", async () => {
  const { wasi } = await withWasi();
  assert.throws(() => wasi.imports.proc_exit(3), WasiExit);
  try {
    wasi.imports.proc_exit(3);
  } catch (e) {
    assert.equal(e.code, 3);
  }
});

test("a call before attach says so instead of reading null", async () => {
  const wasi = wasiPreview1();
  assert.throws(() => wasi.imports.environ_sizes_get(0, 4), /attach/);
});

test("fd_write survives the guest's memory growing", async () => {
  // `memory.grow` detaches the old ArrayBuffer -- checked, not assumed: on this
  // engine `buffer.resizable` is false and the old view throws TypeError after a
  // grow. So a shim that cached a DataView would work until the guest's first
  // allocation past a page boundary and then fail on every call after it.
  //
  // The write *before* the grow is the load-bearing part. Without it a lazily
  // cached view is only ever created after the grow and the test passes with the
  // bug present, which is what it did until removing the mitigation turned nothing
  // red.
  const seen = [];
  const { instance } = await withWasi({ write: (fd, text) => seen.push(text) });
  const write = (s) => {
    const mem = new Uint8Array(instance.exports.memory.buffer);
    const view = new DataView(instance.exports.memory.buffer);
    const text = new TextEncoder().encode(s);
    mem.set(text, 64);
    view.setUint32(0, 64, true);
    view.setUint32(4, text.length, true);
    return instance.exports.probe();
  };
  assert.equal(write("before grow"), 0);
  instance.exports.memory.grow(1);
  assert.equal(write("after grow"), 0, "the call after a grow still works");
  assert.deepEqual(seen, ["before grow", "after grow"]);
});
