// Loading and driving libdiluvium_wasi.wasm.
//
// Adapted from doc/repl-reference.html in the diluvium tree. Three things
// about the build are not guessable from outside and each one costs an
// afternoon:
//
//   1. It must be libdiluvium_wasi.wasm. diluvium_wasi.wasm is a command
//      module whose _start runs the interpreter against stdin, and
//      diluvium_compiler_wasi.wasm is luac.
//
//   2. __wasm_call_ctors() comes before anything else. The module exports
//      that rather than _initialize, so it is not a WASI reactor and nothing
//      initialises libc on your behalf.
//
//   3. Drive it with repl_eval, not run_lua. run_lua cannot distinguish
//      unfinished input from broken input, and does not echo the value of a
//      bare expression -- a front end built on it has to guess at
//      continuation prompts by matching the text of Lua's error messages,
//      and `1 + 1` prints nothing.

import { makeWasi } from './wasi.js';

export async function loadRuntime(url, onOutput) {
  let instance;
  const wasi = makeWasi(() => instance.exports.memory, onOutput);

  // instantiateStreaming needs application/wasm on the response. nginx has
  // shipped that mapping since 1.21.4; the fallback keeps a misconfigured
  // server from turning into a blank panel with a cryptic error.
  let src;
  const response = await fetch(url);
  if (!response.ok) throw new Error(`${response.status} fetching ${url}`);
  const imports = { wasi_snapshot_preview1: wasi };
  if ((response.headers.get('content-type') || '').includes('application/wasm')) {
    src = await WebAssembly.instantiateStreaming(response, imports);
  } else {
    src = await WebAssembly.instantiate(await response.arrayBuffer(), imports);
  }

  instance = src.instance;
  const x = instance.exports;

  x.__wasm_call_ctors?.();
  x.init_lua();

  const enc = new TextEncoder();
  const dec = new TextDecoder();

  const withCString = (s, fn) => {
    const bytes = enc.encode(s);
    const p = x.malloc(bytes.length + 1);
    if (!p) throw new Error('out of memory in the kernel');
    const mem = new Uint8Array(x.memory.buffer);
    mem.set(bytes, p);
    mem[p + bytes.length] = 0;
    try { return fn(p); } finally { x.free(p); }
  };

  const readCString = (p) => {
    const mem = new Uint8Array(x.memory.buffer);
    let end = p;
    while (mem[end] !== 0) end++;
    return dec.decode(mem.slice(p, end));
  };

  return {
    // Older builds (anything before 5.5.1) have run_lua but not repl_eval.
    // The mirror only carries builds that do, but say so rather than
    // silently behaving worse if that ever stops being true.
    hasRepl: typeof x.repl_eval === 'function',

    // 0 ok, 1 unfinished, 2 error
    eval: (code) => withCString(code, (p) => (
      x.repl_eval ? x.repl_eval(p) : (x.run_lua(p) === 0 ? 0 : 2)
    )),

    complete: (buf, cursor) => {
      if (!x.repl_complete) return { offset: cursor, items: [] };
      return withCString(buf, (p) => {
        const r = x.repl_complete(p, cursor);
        if (!r) return { offset: cursor, items: [] };
        const parts = readCString(r).split('\t');
        x.free(r);
        return { offset: parseInt(parts[0], 10), items: parts.slice(1) };
      });
    },

    reset: () => x.reset_lua?.(),
  };
}
