// diluvium.js -- drive the Diluvium browser build from JavaScript.
//
// One small ES module, no dependencies, for a page or Node. It loads
// diluvium_browser.wasm (real wasi-libc underneath, so real numbers and a real
// allocator, and Lua errors caught rather than trapping) and hands back four
// calls:
//
//   dl.version()        -> "5.5.1_buildNN"
//   dl.eval(src)        -> { ok, output }   run dlua, collect what it printed
//   dl.compile(src)     -> Uint8Array | null   source -> bytecode chunk
//   dl.analyze(src)     -> object | null        the JSON analysis report
//   dl.reset()          -> void                 drop the eval session
//
// The module imports seventeen wasi_snapshot_preview1 calls; all but output are
// stubbed to fail, because a page instance reaches nothing but the language.
// This is the whole "WASI shim" -- a handful of stubs, not a runtime.

const ENOSYS = 52, ENOTCAPABLE = 76;

export async function loadDiluvium(source) {
  const bytes =
    source instanceof Uint8Array ? source :
    source instanceof ArrayBuffer ? new Uint8Array(source) :
    new Uint8Array(await (await fetch(source)).arrayBuffer());

  let mem;                       // set after instantiation
  let captured = "";             // stdout+stderr since the last reset
  const dec = new TextDecoder(), enc = new TextEncoder();
  const u8 = () => new Uint8Array(mem.buffer);
  const dv = () => new DataView(mem.buffer);

  const preview1 = new Proxy({}, { get: (_, name) => {
    if (name === "fd_write") return (fd, iovs, n, pWritten) => {
      const view = dv(); let total = 0;
      for (let i = 0; i < n; i++) {
        const ptr = view.getUint32(iovs + i * 8, true);
        const len = view.getUint32(iovs + i * 8 + 4, true);
        captured += dec.decode(u8().slice(ptr, ptr + len));
        total += len;
      }
      view.setUint32(pWritten, total, true);
      return 0;
    };
    if (name === "clock_time_get") return (id, prec, pOut) => {
      dv().setBigUint64(pOut, BigInt(Date.now()) * 1_000_000n, true); return 0;
    };
    if (name === "random_get") return (ptr, len) => {
      const b = u8().subarray(ptr, ptr + len);
      (globalThis.crypto?.getRandomValues?.bind(globalThis.crypto)
        || ((a) => { for (let i = 0; i < a.length; i++) a[i] = (Math.random() * 256) | 0; return a; }))(b);
      return 0;
    };
    if (name === "proc_exit") return (code) => { throw new Error("diluvium: proc_exit(" + code + ")"); };
    if (name === "environ_sizes_get") return (pc, pb) => { dv().setUint32(pc, 0, true); dv().setUint32(pb, 0, true); return 0; };
    if (name === "environ_get") return () => 0;
    // fd 3+ are scanned for preopened directories at startup; EBADF (8) is how
    // "no more preopens" is signalled, so the scan ends cleanly with none.
    if (name === "fd_prestat_get") return () => 8;
    // Everything else -- the filesystem calls a sealed instance never reaches --
    // reports "not capable" rather than pretending to succeed.
    return () => ENOTCAPABLE;
  }});

  const { instance } = await WebAssembly.instantiate(bytes, { wasi_snapshot_preview1: preview1 });
  const ex = instance.exports;
  mem = ex.memory;
  ex.__wasm_call_ctors?.();

  // --- marshalling: bytes in through the guest's own malloc, strings out by
  //     reading to the NUL and freeing what the guest returned. ------------
  const putBytes = (b) => {
    const p = ex.malloc(b.length || 1);
    u8().set(b, p);
    return { p, len: b.length };
  };
  const cstr = (p) => {
    if (!p) return null;
    const bytes = u8(); let end = p;
    while (bytes[end] !== 0) end++;
    return dec.decode(bytes.slice(p, end));
  };
  const run = (src, fn) => {
    const { p, len } = putBytes(enc.encode(src));
    try { return fn(p, len); } finally { ex.free(p); }
  };

  return {
    version: () => cstr(ex.dl_version()),

    reset: () => { captured = ""; ex.dl_reset(); },

    eval(src) {
      captured = "";
      const rc = run(src, (p, len) => ex.dl_eval(p, len));
      return { ok: rc === 0, output: captured };
    },

    compile(src) {
      const pLen = ex.malloc(4);
      try {
        const outPtr = run(src, (p, len) => ex.dl_compile(p, len, pLen));
        if (!outPtr) return null;
        const n = dv().getUint32(pLen, true);
        const out = u8().slice(outPtr, outPtr + n);
        ex.free(outPtr);
        return out;
      } finally { ex.free(pLen); }
    },

    analyze(src) {
      const outPtr = run(src, (p, len) => ex.dl_analyze(p, len));
      if (!outPtr) return null;
      const json = cstr(outPtr);
      ex.free(outPtr);
      try { return JSON.parse(json); } catch { return null; }
    },
  };
}
