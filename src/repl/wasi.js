// A WASI preview1 shim, sized for a REPL in a page.
//
// Adapted from doc/repl-reference.html in the diluvium tree, which is the
// authoritative description of how to drive the WASM build. Keep the two in
// step: when the runtime's import list changes, that file changes first.
//
// libdiluvium_wasi.wasm imports the whole of wasi_snapshot_preview1 -- 45
// entries, because that is what wasi-libc pulls in -- but only the handful
// below are ever called. Everything else answers ENOSYS through the Proxy, so
// an unexpected call fails loudly instead of trapping on a missing import.

const ENOSYS = 52;
const EBADF = 8;

export function makeWasi(getMemory, onOutput) {
  const u8 = () => new Uint8Array(getMemory().buffer);
  const dv = () => new DataView(getMemory().buffer);
  const decoder = new TextDecoder();

  const wasi = new Proxy({}, {
    get: (target, name) => target[name] ?? (() => ENOSYS),
    has: () => true,
  });

  // The whole of output. print, io.write and Lua's error messages all land
  // in fd_write, so nothing else needs capturing.
  wasi.fd_write = (fd, iovs, iovsLen, nwritten) => {
    if (fd !== 1 && fd !== 2) return EBADF;
    const view = dv();
    const chunks = [];
    let written = 0;
    for (let i = 0; i < iovsLen; i++) {
      const ptr = view.getUint32(iovs + i * 8, true);
      const len = view.getUint32(iovs + i * 8 + 4, true);
      chunks.push(u8().slice(ptr, ptr + len));
      written += len;
    }
    const total = chunks.reduce((n, b) => n + b.length, 0);
    const flat = new Uint8Array(total);
    let o = 0;
    for (const b of chunks) { flat.set(b, o); o += b.length; }
    onOutput(decoder.decode(flat), fd);
    view.setUint32(nwritten, written, true);
    return 0;
  };

  // There is no stdin in a panel: the terminal feeds repl_eval directly.
  wasi.fd_read = (fd, iovs, iovsLen, nread) => {
    dv().setUint32(nread, 0, true);
    return 0;
  };
  wasi.fd_close = () => 0;
  wasi.fd_seek = () => ENOSYS;
  wasi.fd_fdstat_get = (fd, buf) => {
    const view = dv();
    view.setUint8(buf, 2);                     // character device
    view.setUint16(buf + 2, 0, true);
    view.setBigUint64(buf + 8, 0n, true);
    view.setBigUint64(buf + 16, 0n, true);
    return 0;
  };
  wasi.random_get = (buf, len) => {
    crypto.getRandomValues(u8().subarray(buf, buf + len));
    return 0;
  };
  wasi.clock_time_get = (id, prec, out) => {
    dv().setBigUint64(out, BigInt(Date.now()) * 1000000n, true);
    return 0;
  };
  wasi.environ_sizes_get = (c, s) => {
    const view = dv();
    view.setUint32(c, 0, true);
    view.setUint32(s, 0, true);
    return 0;
  };
  wasi.environ_get = () => 0;
  wasi.args_sizes_get = (c, s) => {
    const view = dv();
    view.setUint32(c, 0, true);
    view.setUint32(s, 0, true);
    return 0;
  };
  wasi.args_get = () => 0;
  wasi.proc_exit = (code) => { throw new Error('exit(' + code + ')'); };
  wasi.sched_yield = () => 0;

  return wasi;
}
