# web/ — Diluvium in the browser

A wasm build a page drives from JavaScript: run dlua, compile a chunk to
bytecode, run the analyzer. One `.wasm` plus one small ES module, no
framework and no server.

This is **not** `diluvium_wasi.wasm` (the command module for wasmtime) and
**not** the swarm. It is the language, the compiler and the analyzer, sealed,
with real `wasi-libc` underneath — so numbers format correctly, the allocator
is real and grows (no fixed heap), and a Lua error is caught rather than
trapping the module.

```
diluvium.js                  the loader + API (this is the CDN module)
demo.html                    a page exercising eval / compile / analyze
diluvium_browser.wasm        built by `make build_browser` (gitignored)
```

## Build

```sh
make build_browser           # -> web/diluvium_browser.wasm
```

It builds with the LLVM the distro ships, **not** the wasi-sdk container the
other wasm targets use, so it needs no podman. On Debian/Ubuntu:

```sh
apt-get install clang-20 lld-20 libclang-rt-20-dev-wasm32 libc++abi-20-dev-wasm32
# and a wasm32-wasi sysroot at /usr/lib/wasm32-wasi + /usr/include/wasm32-wasi
```

Override the toolchain paths on the `make` line if yours differ:
`BROWSER_CC`, `BROWSER_LD`, `BROWSER_SYSROOT`, `BROWSER_LIBC`, `BROWSER_RT`.

The one file that is not Diluvium's own is `src/wasm_browser_sjlj.c`, vendored
verbatim from wasi-libc (MIT): the three functions that make `setjmp`/`longjmp`
— and so `pcall` — lower onto the wasm exception-handling proposal instead of
trapping. The wasi-sdk's wasi-libc already ships these; Debian's does not.

## Use

```js
import { loadDiluvium } from "./diluvium.js";
const dl = await loadDiluvium("./diluvium_browser.wasm");

dl.version();                       // "0.15.0"
dl.eval('print($"hi {1+1}")');      // { ok: true, output: "hi 2\n" }
dl.compile("return 1 + 1");         // Uint8Array of bytecode, or null on error
dl.analyze("local x = 1 return x"); // the analysis report as an object, or null
dl.reset();                         // drop the eval session; next eval is fresh
```

`eval` runs with REPL semantics (a bare expression prints its value) and
returns what the program wrote to output. Output crosses `fd_write`, which the
loader captures; every other system call a sealed instance never reaches is
stubbed to fail. That handful of stubs is the whole "WASI shim" — see the top
of `diluvium.js`.

## Try it

```sh
make build_browser
python3 -m http.server -d web 8000      # then open http://127.0.0.1:8000/demo.html
```

Opening `demo.html` from `file://` will not work: a module `fetch`ing a
`.wasm` needs an origin. Any static server does.
