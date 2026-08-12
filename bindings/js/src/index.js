// Embed a Diluvium program, in a browser tab or in Node.
//
// One package for both, over the same `.wasm`. There is no native path: a
// browser cannot have one, and maintaining two would mean two sets of bugs for a
// runtime whose whole pitch is that hosts are interchangeable.
//
// ## Why this file is shaped differently from the Rust wrapper
//
// The `dv_*` ABI is a C ABI, and a C ABI does not cross into WebAssembly
// unchanged. Three things differ, and they are the same three for *any*
// wasm-hosted binding -- including Rust over wasmtime:
//
//   Pointers are offsets. Every `const uint8_t *` is a u32 index into the
//   module's linear memory, not an address. Nothing can be passed by reference
//   from outside that memory.
//
//   The host must allocate inside the guest. To pass a name or a message in, the
//   bytes have to be *in* the module's memory first, which means calling its
//   exported `malloc`. The Makefile exports `malloc` and `free` from the wasm
//   build for exactly this.
//
//   Zero-copy stops at the memory boundary. `dv_queue_peek` still avoids a copy
//   inside the runtime, but reading the bytes from JavaScript copies them out of
//   linear memory. The peek/release pair is still worth using -- it avoids the
//   runtime's own copy -- but it is not the same guarantee the Rust wrapper has,
//   and pretending otherwise would be a lie about performance.
//
// ## Notification
//
// `dv_set_notify` takes a C function pointer, which in wasm is an index into the
// module's function table. Installing a JavaScript callback there means exporting
// a mutable table and reserving a slot, which is more machinery than the feature
// is worth right now: `queueInfo(id).len` answers the same question, and a host
// that has just resumed a program knows to look. Left out deliberately rather
// than half-built.

import { encode, decode } from "./msgpack.js";
import { wasiPreview1, WASI_MODULE } from "./wasi.js";

export { Ext, Float } from "./msgpack.js";
export { wasiPreview1, WasiExit, WASI_MODULE } from "./wasi.js";

export const ABI_VERSION = 1;

const DV_OK = 0;
const DV_QUEUE_FULL = 1;
const DV_QUEUE_DISABLED = 2;
const DV_QUEUE_EMPTY = 4;
const DV_IDLE = 6;
const DV_DONE = 7;
const DV_BUSY = 11;
const DV_QUEUE_DROPPED = 13;

/** What happened to a push. Full and disabled are answers, not failures. */
export const Accepted = Object.freeze({
  OK: "ok",
  DROPPED_OLDEST: "dropped_oldest",
  FULL: "full",
  DISABLED: "disabled",
});

export class DiluviumError extends Error {}
export class AbiMismatch extends DiluviumError {}
export class ProgramError extends DiluviumError {}
export class UnknownQueue extends DiluviumError {}
export class Busy extends DiluviumError {}

const DIRECTIONS = ["both", "guest_write", "guest_read"];
const ON_FULL = ["reject", "drop_oldest", "drop_newest", "block"];

// Field offsets, asked of the runtime rather than assumed.
//
// A hardcoded offset here is a bug that cannot be caught by testing on the
// machine that wrote it: wasm32 is ILP32, so every struct holding a pointer or a
// size_t is laid out differently there than on the LP64 host a developer would
// measure. `dv_layout` reports what the compiler that built the runtime actually
// produced.
const L = Object.freeze({
  CONFIG_SIZE: 0, CONFIG_ABI: 1, CONFIG_FLAGS: 2,
  QI_SIZE: 3, QI_CAPACITY: 4, QI_LEN: 5, QI_ENABLED: 6, QI_EXPORTED: 7,
  QI_DIRECTION: 8, QI_ON_FULL: 9,
  WS_SIZE: 10, WS_N: 11, WS_IDS: 12, WS_TIMEOUT: 13, WS_FOR_WRITE: 14,
  COUNT: 15,
});

function readLayout(mod) {
  const p = mod.exports.malloc(L.COUNT * 4);
  if (p === 0) throw new DiluviumError("out of memory reading the ABI layout");
  try {
    const n = mod.exports.dv_layout(p, L.COUNT);
    const view = new DataView(mod.exports.memory.buffer);
    const out = new Uint32Array(L.COUNT);
    for (let i = 0; i < n; i++) out[i] = view.getUint32(p + i * 4, true);
    return out;
  } finally {
    mod.exports.free(p);
  }
}

/**
 * A scope for allocations inside the guest's linear memory.
 *
 * Every string and every message has to live in the module's memory before the
 * ABI can see it, and every one of those has to come back. Doing that by hand at
 * each call site is how a wasm binding leaks; this makes the lifetime a block.
 */
class Scratch {
  constructor(mod) {
    this.mod = mod;
    this.owned = [];
  }

  alloc(n) {
    const p = this.mod.exports.malloc(n);
    if (p === 0) throw new DiluviumError("out of memory inside the instance");
    this.owned.push(p);
    return p;
  }

  bytes(src) {
    const p = this.alloc(src.length || 1);
    new Uint8Array(this.mod.exports.memory.buffer).set(src, p);
    return p;
  }

  cstring(s) {
    const b = new TextEncoder().encode(s);
    const p = this.alloc(b.length + 1);
    const mem = new Uint8Array(this.mod.exports.memory.buffer);
    mem.set(b, p);
    mem[p + b.length] = 0;
    return p;
  }

  free() {
    for (const p of this.owned) this.mod.exports.free(p);
    this.owned.length = 0;
  }
}

/** What a parked program is waiting for. */
export class Wait {
  constructor(ids, timeoutMs, forWrite) {
    /** @type {number[]} the queues it is watching */
    this.ids = ids;
    /** @type {number|null} milliseconds it asked for, or null for no limit */
    this.timeoutMs = timeoutMs;
    /** @type {boolean} true when it wants space, not a message: drain it */
    this.forWrite = forWrite;
  }
}

/** Where a program got to: `wait` is null once it has finished. */
export class Step {
  constructor(wait) {
    this.wait = wait;
  }
  get parked() {
    return this.wait !== null;
  }
  get done() {
    return this.wait === null;
  }
}

/**
 * One Diluvium program.
 *
 * Created through {@link Diluvium.load}, because instantiating WebAssembly is
 * asynchronous and a constructor cannot be.
 */
export class Instance {
  constructor(mod, ptr, layout) {
    this._mod = mod;
    this._ptr = ptr;
    this._L = layout;
    this._ws = mod.exports.malloc(layout[L.WS_SIZE]);
  }

  get _mem() {
    // Re-read every time: linear memory can be replaced when it grows, and a
    // cached view would then be pointed at a detached buffer.
    return new DataView(this._mod.exports.memory.buffer);
  }

  get _u8() {
    return new Uint8Array(this._mod.exports.memory.buffer);
  }

  _lastError() {
    const p = this._mod.exports.dv_last_error(this._ptr);
    if (p === 0) return "(no message)";
    const mem = this._u8;
    let end = p;
    while (mem[end] !== 0) end++;
    return new TextDecoder().decode(mem.subarray(p, end));
  }

  /** Free the instance. Idempotent. */
  close() {
    if (this._ptr !== 0) {
      this._mod.exports.dv_free(this._ptr);
      this._mod.exports.free(this._ws);
      this._ptr = 0;
    }
  }

  _live() {
    if (this._ptr === 0) throw new DiluviumError("this instance has been closed");
  }

  /**
   * Find a queue the program declared, or one of the `inbox`/`outbox` pair every
   * instance starts with. `null` when there is none -- which is also how you
   * learn the program you loaded is not the one you expected.
   */
  queue(name) {
    this._live();
    const s = new Scratch(this._mod);
    try {
      const id = this._mod.exports.dv_queue_lookup(this._ptr, s.cstring(name));
      return id === 0 ? null : id;
    } finally {
      s.free();
    }
  }

  /** Capacity, current length, and how the queue is configured. */
  queueInfo(id) {
    this._live();
    const s = new Scratch(this._mod);
    try {
      const lay = this._L;
      const out = s.alloc(lay[L.QI_SIZE]);
      if (this._mod.exports.dv_queue_state(this._ptr, id, out) !== DV_OK) {
        throw new UnknownQueue(`no such queue: ${id}`);
      }
      const m = this._mem;
      return {
        capacity: m.getUint32(out + lay[L.QI_CAPACITY], true),
        len: m.getUint32(out + lay[L.QI_LEN], true),
        enabled: m.getUint8(out + lay[L.QI_ENABLED]) !== 0,
        exported: m.getUint8(out + lay[L.QI_EXPORTED]) !== 0,
        direction: DIRECTIONS[m.getUint8(out + lay[L.QI_DIRECTION])] ?? "both",
        onFull: ON_FULL[m.getUint8(out + lay[L.QI_ON_FULL])] ?? "reject",
      };
    } finally {
      s.free();
    }
  }

  /** Send a message, encoding it to msgpack. Returns an {@link Accepted}. */
  push(id, value) {
    return this.pushRaw(id, encode(value));
  }

  /** Send msgpack bytes you already have. */
  pushRaw(id, payload) {
    this._live();
    const s = new Scratch(this._mod);
    try {
      const p = s.bytes(payload);
      const st = this._mod.exports.dv_queue_push(this._ptr, id, p, payload.length);
      switch (st) {
        case DV_OK: return Accepted.OK;
        case DV_QUEUE_DROPPED: return Accepted.DROPPED_OLDEST;
        case DV_QUEUE_FULL: return Accepted.FULL;
        case DV_QUEUE_DISABLED: return Accepted.DISABLED;
        default: throw new UnknownQueue(`no such queue: ${id}`);
      }
    } finally {
      s.free();
    }
  }

  /** Take the oldest message and decode it. `undefined` when the queue is empty. */
  pop(id) {
    const raw = this.popRaw(id);
    return raw === undefined ? undefined : decode(raw);
  }

  /**
   * Take the oldest message as raw msgpack. `undefined` when empty.
   *
   * Uses peek/release, which avoids the runtime's copy; the copy out of linear
   * memory is unavoidable from JavaScript. See the note at the top of this file.
   */
  popRaw(id) {
    this._live();
    const s = new Scratch(this._mod);
    try {
      const pp = s.alloc(4);
      const plen = s.alloc(4);
      const st = this._mod.exports.dv_queue_peek(this._ptr, id, pp, plen);
      if (st === DV_QUEUE_EMPTY) return undefined;
      if (st !== DV_OK) throw new UnknownQueue(`no such queue: ${id}`);
      const m = this._mem;
      const ptr = m.getUint32(pp, true);
      const len = m.getUint32(plen, true);
      const out = this._u8.slice(ptr, ptr + len);
      this._mod.exports.dv_queue_release(this._ptr, id);
      return out;
    } finally {
      s.free();
    }
  }

  /** Start the program, or continue it after a completed step. */
  run() {
    this._live();
    const st = this._mod.exports.dv_run(this._ptr, this._ws);
    return this._settle(st, true);
  }

  /** Answer a park: say which queue fired. */
  resume(fired) {
    this._live();
    const st = this._mod.exports.dv_resume(this._ptr, fired);
    return this._settle(st, false);
  }

  /** Answer a park by saying its timeout elapsed. */
  resumeTimeout() {
    this._live();
    const st = this._mod.exports.dv_resume(this._ptr, 0);
    return this._settle(st, false);
  }

  _settle(st, filled) {
    if (st === DV_DONE) return new Step(null);
    if (st === DV_IDLE) {
      // dv_resume has nowhere to put a wait-set, and a program looping on
      // queue.wait parks again at once, so read the current park back rather
      // than reporting a stale or empty one.
      if (!filled) this._mod.exports.dv_waitset_get(this._ptr, this._ws);
      const m = this._mem;
      const lay = this._L;
      const n = m.getUint32(this._ws + lay[L.WS_N], true);
      const ids = [];
      const base = this._ws + lay[L.WS_IDS];
      for (let i = 0; i < n; i++) ids.push(m.getUint32(base + i * 4, true));
      const timeout = m.getBigInt64(this._ws + lay[L.WS_TIMEOUT], true);
      const forWrite = m.getUint8(this._ws + lay[L.WS_FOR_WRITE]) !== 0;
      return new Step(new Wait(ids, timeout >= 0n ? Number(timeout) : null, forWrite));
    }
    if (st === DV_BUSY) throw new Busy(this._lastError());
    throw new ProgramError(this._lastError());
  }
}

/** Loading and version checks. */
export const Diluvium = {
  /**
   * Instantiate the runtime and load a program.
   *
   * `wasm` is a module, a compiled `WebAssembly.Module`, or bytes. `source` is
   * Lua text; `bytecode` is accepted only if you pass `{ allowBytecode: true }`,
   * because the loader refusing malformed bytecode is a smaller claim than
   * bytecode being safe.
   *
   * An instance is sealed: it has no `io`, `os` or `package`, and reaches
   * outside itself only by yielding a request its host answers. What is left is
   * the language, the queues, coroutines and the codec, so a program that needs
   * a clock or a file should be handed it through a queue.
   *
   * `unsafeStdlib` puts those libraries back. **Scaffolding, not a supported
   * configuration**: it costs the instruction budget its meaning, since a
   * subprocess started by `os.execute` costs no VM instructions, and it costs
   * replayability, since inputs then arrive somewhere other than the message
   * log. It is also what the WASI shim's `clock_time_get` is for -- `os.time`
   * and `os.clock` reach it, and without this flag neither exists.
   *
   * `unsafeDebug` opens the whole `debug` library rather than the narrowed one,
   * which restores the endpoint-forgery route and lets the program switch off
   * its own instruction budget. Only for programs you wrote.
   */
  async load(wasm, source, {
    name = "=(program)",
    allowBytecode = false,
    unsafeStdlib = false,
    unsafeDebug = false,
    wasiWrite = undefined,
  } = {}) {
    const mod = await instantiate(wasm, { wasiWrite });
    const library = mod.exports.dv_abi_version();
    if (library !== ABI_VERSION) {
      throw new AbiMismatch(
        `the runtime is ABI version ${library}, this wrapper expects ${ABI_VERSION}`
      );
    }
    const layout = readLayout(mod);
    const s = new Scratch(mod);
    try {
      const cfg = s.alloc(layout[L.CONFIG_SIZE]);
      // Zero the whole struct: the reserved and memory_limit fields must not be
      // whatever malloc left behind.
      new Uint8Array(mod.exports.memory.buffer).fill(
        0, cfg, cfg + layout[L.CONFIG_SIZE]
      );
      const view = new DataView(mod.exports.memory.buffer);
      view.setUint32(cfg + layout[L.CONFIG_ABI], ABI_VERSION, true);
      // dv.h's flags: TEXT_ONLY 0x1, UNSAFE_DEBUG 0x2, UNSAFE_STDLIB 0x4.
      const flags =
        (allowBytecode ? 0 : 0x1) |
        (unsafeDebug ? 0x2 : 0) |
        (unsafeStdlib ? 0x4 : 0);
      view.setUint32(cfg + layout[L.CONFIG_FLAGS], flags, true);
      const ptr = mod.exports.dv_new(cfg);
      if (ptr === 0) throw new DiluviumError("could not create an instance");
      const inst = new Instance(mod, ptr, layout);
      const code =
        typeof source === "string" ? new TextEncoder().encode(source) : source;
      const cp = s.bytes(code);
      const st = mod.exports.dv_load(ptr, cp, code.length, s.cstring(name));
      if (st !== DV_OK) throw new ProgramError(inst._lastError());
      return inst;
    } finally {
      s.free();
    }
  },

  /** The ABI version this wrapper was built against. */
  abiVersion() {
    return ABI_VERSION;
  },
};

// ## The swarm host, across the boundary
//
// `dvs_new` takes a vtable of C function pointers, which JavaScript cannot
// make -- the same wall `dv_set_notify` hit above. The runtime's answer is
// `src/dvs_shim.c`: the trampolines live on the C side, declared as imports
// from module "env" (`js_host_create` / `js_host_destroy` / `js_host_drive`),
// and `dvsjs_new` is the constructor a wasm host calls instead of `dvs_new`.
// A module containing the shim cannot instantiate without those imports, so
// `instantiate` always supplies them, late-bound the way the WASI shim
// late-binds memory: install the real host with `setSwarmHost` after
// instantiation, before anything calls `dvs_step`. The defaults are honest
// rather than convenient -- `drive` without an installed host throws by name,
// because an instance silently dropped by a missing host is the kind of
// failure this tree refuses to ship.
//
// One caution that is the boundary's, not this file's: a host callback runs
// *inside* `dvs_step`, and a JavaScript exception thrown from it unwinds the
// wasm stack mid-step. Treat callbacks as code that must not throw; the
// throwing default exists to catch a missing host during development, not as
// an error-handling channel.
const swarmHosts = new WeakMap();

/** Install the swarm host for an instantiated module: an object with
 * `drive(id, instPtr, ctx)` (required; return truthy to keep the instance,
 * falsy when it is finished -- an undefined return throws, so a forgotten
 * `return` cannot silently kill instances), and optionally `create(id,
 * instPtr)` returning a numeric ctx, and `destroy(id, ctx)`. */
export function setSwarmHost(instance, host) {
  const ref = swarmHosts.get(instance);
  if (!ref) {
    throw new DiluviumError(
      "setSwarmHost: this instance was not created by instantiate(), so it has no env trampolines"
    );
  }
  ref.current = host;
}

/** Instantiate a Diluvium wasm module and return the raw WebAssembly
 * instance: exports (including `dvs_*` and `dvsjs_new` when the module is the
 * swarm build), linear memory, `malloc`/`free`. `Diluvium.load` is the
 * comfortable path for one instance; this is the layer a host -- lab's swarm
 * host in particular -- builds on, paired with `setSwarmHost`. Exported
 * because hiding it would just make hosts re-implement the WASI plumbing. */
export async function instantiate(wasm, { wasiWrite } = {}) {
  if (wasm && wasm.exports) return wasm; // already instantiated

  // The module is linked against the wasi-sdk's libc, so it imports
  // `wasi_snapshot_preview1` regardless of what any program does -- the imports
  // come from libc's startup and stdio. Instantiating with `{}` fails before a line
  // of Lua runs. See src/wasi.js for what is and is not provided.
  const module =
    wasm instanceof WebAssembly.Module
      ? wasm
      : await WebAssembly.compile(wasm instanceof Uint8Array ? wasm : new Uint8Array(wasm));

  const wasi = wasiPreview1(wasiWrite ? { write: wasiWrite } : {});
  wasi.complete(module);
  const hostRef = { current: null };
  const instance = await WebAssembly.instantiate(module, {
    [WASI_MODULE]: wasi.imports,
    // Supplied whether or not this module contains the swarm shim: extra
    // import modules are ignored, missing ones fail instantiation.
    env: {
      js_host_create: (ud, id, inst) => {
        const h = hostRef.current;
        return h && h.create ? h.create(id, inst) | 0 : 0;
      },
      js_host_destroy: (ud, id, ctx) => {
        const h = hostRef.current;
        if (h && h.destroy) h.destroy(id, ctx);
      },
      js_host_drive: (ud, id, inst, ctx) => {
        const h = hostRef.current;
        if (!h || typeof h.drive !== "function") {
          throw new DiluviumError(
            "no swarm host installed: call setSwarmHost(instance, host) before dvs_step drives anything"
          );
        }
        const keep = h.drive(id, inst, ctx);
        if (keep === undefined) {
          throw new DiluviumError(
            "swarm host drive() returned undefined; return truthy to keep the instance, falsy when it is finished"
          );
        }
        return keep ? 1 : 0;
      },
    },
  });
  wasi.attach(instance);
  swarmHosts.set(instance, hostRef);

  // The wasm build is not a WASI reactor, so its constructors have to be run by
  // hand -- the same thing doc/repl-reference.html records about run_lua. This is
  // also why `_start` is never called: the module is a command, but it is being
  // used as a library, and `_start` would run main and exit.
  if (typeof instance.exports.__wasm_call_ctors === "function") {
    instance.exports.__wasm_call_ctors();
  }
  return instance;
}
