// A minimal WASI preview-1 host, enough to instantiate `diluvium_wasi.wasm`.
//
// Why this file exists. The wasm the Makefile builds is linked against the
// wasi-sdk's libc, so it imports `wasi_snapshot_preview1` whether or not any
// program ever touches a file -- the imports come from libc's own startup and
// stdio, not from anything Diluvium asked for. A wrapper that instantiates with
// `{}` gets `Import #0 module="wasi_snapshot_preview1": module is not an object or
// function` before a single line of Lua runs, which is what CI reported for weeks.
//
// Why not `node:wasi`. This wrapper is meant to run in a browser as well, and
// `node:wasi` is not there. The set of calls that actually need real behaviour is
// small -- a clock, randomness, and writing to stdout/stderr -- so implementing
// those and refusing the rest is both portable and a smaller thing to be wrong
// about than a filesystem emulation nobody wants.
//
// Why the stubs are synthesized from the module's own import list. A wasm module
// must have *every* declared import satisfied or instantiation fails outright, and
// which ones libc declares depends on the sysroot and the link flags. Enumerating
// `WebAssembly.Module.imports()` and filling the gaps means a wasi-sdk bump adds a
// call that returns ENOSYS instead of breaking the wrapper. Guessing a fixed list
// is how this breaks again on someone else's build.
//
// What this is not: a sandbox escape hatch. Nothing here opens a path, reads an
// environment variable, or takes an argument. `path_open` answers ENOSYS -- which
// is not a policy this file invented, it is that there is nothing mounted to open.

/** WASI errno values this file uses. */
const ESUCCESS = 0;
const EBADF = 8;
const ENOSYS = 52;

/** The preview-1 module name, which is also what the error message names. */
export const WASI_MODULE = "wasi_snapshot_preview1";

const CLOCK_REALTIME = 0;

/**
 * A preview-1 import object.
 *
 * Returns `{ imports, attach }`. `attach(instance)` must be called once the
 * instance exists, because every call here needs its memory and the memory is an
 * export -- the classic wasm chicken-and-egg, and the reason this is two steps
 * rather than one.
 *
 * `write` is called with (fd, text) for stdout and stderr; the default sends them
 * to the console. A host that wants a program's `print` output as data passes its
 * own.
 */
export function wasiPreview1({ write = defaultWrite } = {}) {
  let memory = null;
  const view = () => {
    if (memory === null) {
      throw new Error("wasi: attach(instance) was not called before a wasi call");
    }
    // Re-made each time on purpose: the guest's memory can grow, and a DataView
    // over a detached ArrayBuffer throws on every access.
    return new DataView(memory.buffer);
  };
  const bytes = () => new Uint8Array(memory.buffer);

  const imports = {
    /** libc's exit path. Throwing is the only way to stop the guest from here. */
    proc_exit(code) {
      throw new WasiExit(code);
    },

    /**
     * Gather-write. Only stdout and stderr are real; anything else is EBADF,
     * because there are no other descriptors.
     */
    fd_write(fd, iovs, iovsLen, nwrittenPtr) {
      if (fd !== 1 && fd !== 2) return EBADF;
      const v = view();
      const mem = bytes();
      let written = 0;
      const parts = [];
      for (let i = 0; i < iovsLen; i++) {
        const base = v.getUint32(iovs + i * 8, true);
        const len = v.getUint32(iovs + i * 8 + 4, true);
        if (len > 0) parts.push(mem.subarray(base, base + len));
        written += len;
      }
      if (parts.length > 0) {
        const joined = new Uint8Array(written);
        let at = 0;
        for (const p of parts) {
          joined.set(p, at);
          at += p.length;
        }
        write(fd, new TextDecoder().decode(joined));
      }
      v.setUint32(nwrittenPtr, written, true);
      return ESUCCESS;
    },

    /** Closing stdout is a no-op rather than an error; libc does it at exit. */
    fd_close(fd) {
      return fd === 1 || fd === 2 ? ESUCCESS : EBADF;
    },

    /** Nothing here is seekable. */
    fd_seek() {
      return ENOSYS;
    },

    /**
     * A clock, which is one of the two calls the guest genuinely needs -- Lua's
     * `os.time` and `os.clock` reach it.
     */
    clock_time_get(id, _precision, out) {
      // Milliseconds to nanoseconds. `Date.now` is the portable floor here; a
      // browser's `performance.now` is monotonic but has no epoch, and preview-1's
      // realtime clock is defined against one.
      const ns = BigInt(Math.round(Date.now())) * 1000000n;
      if (id !== CLOCK_REALTIME) {
        // Monotonic and the process clocks: relative is all they promise, so a
        // realtime reading satisfies the contract without pretending to more
        // resolution than there is.
        view().setBigUint64(out, ns, true);
        return ESUCCESS;
      }
      view().setBigUint64(out, ns, true);
      return ESUCCESS;
    },

    clock_res_get(_id, out) {
      view().setBigUint64(out, 1000000n, true);
      return ESUCCESS;
    },

    /** The other call the guest needs: `math.randomseed` with no argument. */
    random_get(buf, len) {
      const target = bytes().subarray(buf, buf + len);
      const c = globalThis.crypto;
      if (c && typeof c.getRandomValues === "function") {
        // getRandomValues refuses more than 65536 bytes at once.
        for (let at = 0; at < len; at += 65536) {
          c.getRandomValues(target.subarray(at, Math.min(at + 65536, len)));
        }
        return ESUCCESS;
      }
      // No crypto is a refusal, not a fallback to Math.random: a caller that asked
      // for randomness and silently got something predictable is worse off than one
      // that was told no.
      return ENOSYS;
    },

    /** No arguments and no environment, and both have to say so rather than fail. */
    args_sizes_get(argcPtr, sizePtr) {
      const v = view();
      v.setUint32(argcPtr, 0, true);
      v.setUint32(sizePtr, 0, true);
      return ESUCCESS;
    },
    args_get() {
      return ESUCCESS;
    },
    environ_sizes_get(countPtr, sizePtr) {
      const v = view();
      v.setUint32(countPtr, 0, true);
      v.setUint32(sizePtr, 0, true);
      return ESUCCESS;
    },
    environ_get() {
      return ESUCCESS;
    },

    /**
     * No preopened directories. libc walks these at startup to build its
     * descriptor table, so EBADF at index 0 is how it learns the table is empty --
     * a refusal it expects, not an error it reports.
     */
    fd_prestat_get() {
      return EBADF;
    },
    fd_prestat_dir_name() {
      return EBADF;
    },
  };

  return {
    imports,
    attach(instance) {
      memory = instance.exports.memory;
      if (!memory) throw new Error("wasi: the module exports no memory");
    },
    /**
     * Fill in every import the module declares that is not implemented above.
     *
     * This is what keeps a wasi-sdk bump from breaking instantiation: an
     * unimplemented call becomes ENOSYS, which libc handles, instead of a missing
     * import, which nothing handles. Returns the names it stubbed so a caller can
     * see what it is not providing.
     */
    complete(module) {
      const stubbed = [];
      for (const { module: m, name } of WebAssembly.Module.imports(module)) {
        if (m !== WASI_MODULE || name in imports) continue;
        imports[name] = () => ENOSYS;
        stubbed.push(name);
      }
      return stubbed;
    },
  };
}

/** Thrown by `proc_exit`, so a caller can tell an exit from a trap. */
export class WasiExit extends Error {
  constructor(code) {
    super(`the guest called proc_exit(${code})`);
    this.code = code;
  }
}

function defaultWrite(fd, text) {
  // No trailing-newline handling: the guest sets stdout unbuffered (see
  // wasm_stubs.c), so it writes exactly what it means to write and adding a
  // newline per call would split every `io.write`.
  if (fd === 2) {
    if (typeof process !== "undefined" && process.stderr) process.stderr.write(text);
    else console.error(text);
  } else if (typeof process !== "undefined" && process.stdout) {
    process.stdout.write(text);
  } else {
    console.log(text);
  }
}
