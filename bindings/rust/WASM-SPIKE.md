# Spike: linking the C core into Rust wasm builds

**Question.** Can the amalgamated core -- whose setjmp/longjmp the wasi-sdk
lowers onto the standard exception-handling proposal (`-mllvm
-wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false`) -- be linked by rust-lld
into rustc-emitted WebAssembly, on every target the Rust runtime plans to
ship? This was the one critical-path item that could have forced a design
change (same-module linking failing would have meant nested-module hosting
everywhere). Run 2026-08-26 against `onelua.c` at 5.5.1_build11, wasi-sdk-27
(clang 20.1.8), rustc 1.94.1, wasmtime crate 43.0.2.

**Answer: yes, on all three targets.** Measured, not inferred -- each row is
a thing that ran or validated, with the wrinkles recorded below.

| target | linked | ran under wasmtime | what ran |
|---|---|---|---|
| `wasm32-wasip1` | yes | yes, core module | abi check, `pcall(error(...))` caught -- the sjlj throw/catch path -- queue round-trip through a park, snapshot at a park, restore into a fresh instance, continue to completion |
| `wasm32-wasip2` | yes | yes, as a component | the same guest, componentized by rustc's wasip2 target, `wasi:cli/run` |
| `wasm32-unknown-unknown` | yes | validate-only (no embedder shim in the spike) | full interpreter in the binary (497 KB, tag section present), accepted by an EH-enabled engine at compile |

The wasip1 and wasip2 runs exercised the exception path *at runtime*: the
guest's first act is `pcall(function() error(...) end)` and asserting the
message came back, which under this lowering is a wasm `throw` caught by a
`try_table`. The unknown-unknown module was linked and validated but not
executed -- running it needs the embedder's libc shim (below), which is
engineering, not risk.

## The recipe

C side (per target; identical apart from `--target` and the browser's
excluded libraries):

```
wasi-sdk-27/bin/clang -O2 -std=c99 -c src/onelua.c -o onelua.o \
  -I src -DMAKE_LIB \
  -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false \
  -DL_tmpnam=32 -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS \
  -Wno-deprecated-declarations
llvm-ar rcs libdiluvium_wasip1.a onelua.o
```

Rust side: an ordinary crate whose build script links the archive plus three
wasi-sdk sysroot archives -- `libsetjmp.a` (defines `__wasm_setjmp`,
`__wasm_setjmp_test`, `__wasm_longjmp`, which the lowering emits calls to),
`libwasi-emulated-signal.a`, `libwasi-emulated-process-clocks.a`. Host side:
wasmtime with `config.wasm_exceptions(true)` -- stated explicitly for the
reason `diluvium-wasmtime`'s source records.

## The four wrinkles, so nobody re-finds them

1. **LLVM 20 is the floor.** `-wasm-use-legacy-eh=false` does not exist in
   clang 19 (wasi-sdk-25 fails with "unknown argument"; wasi-sdk-27 works).
   Same constraint the Makefile's pinned container digest encodes; a source
   build needs wasi-sdk >= 24 in practice, 27 verified.

2. **Never put the wasi-sdk sysroot lib directory on the Rust link search
   path.** rustc resolves its own self-contained `crt1-command.o` by search
   path, and the sysroot directory shadows it with wasi-sdk's -- whose newer
   wasi-libc weak-imports `__wasi_init_tp`, producing a module that
   instantiates nowhere ("unknown import: `env::__wasi_init_tp`"). Copy the
   three needed archives into your own directory and search only there.

3. **rustc's bundled wasi-libc is older than wasi-sdk-27's** and lacks the
   `tmpfile`, `tmpnam` and `system` stubs, so `liolib`/`loslib` references
   surface as `env::` imports. Three one-line `#[no_mangle]` Rust stubs with
   the honest no-OS-here C semantics (`NULL`, `NULL`, `system(NULL) == 0`)
   close it. A build that excludes `liolib`/`loslib` outright -- what sealed
   instances imply anyway -- would not need them.

4. **wasip2 componentization refuses the preview1 clocks archive.** Rust's
   wasip2 target turns the core module into a component, and
   `libwasi-emulated-process-clocks.a` (compiled for preview1) carries a
   `__wasi_clock_time_get` reference no p2 world satisfies -- the
   componentizer stops with "failed to resolve import". Drop that archive on
   wasip2 and define C's `clock()` from Rust (`std::time::Instant`, clock_t
   in nanoseconds as wasi-libc defines it). Nothing else in the archive set
   objects to crossing.

## The browser contract

For `wasm32-unknown-unknown` the core compiles against the wasi-sdk headers
plus `src/wasm-shim/`, and the libc surface arrives from the embedder at
instantiation (link with `--allow-undefined`; the references become `env::`
imports). With `liolib`/`loslib`/`loadlib` compiled in, that surface is 84
symbols -- string/ctype/math/stdio/locale/time plus `malloc`/`realloc`/
`free`/`calloc` -- which is precisely the role `src/wasm_stubs_unknown.c` and
the JS shim play in the existing browser artifact. A Rust browser embedder
supplies the same set from Rust (allocator over `GlobalAlloc`, the string and
math family, a formatter for `snprintf`/`vsnprintf`), or shrinks it by
excluding the OS-facing libraries.

One deliberate departure worth recording: the existing browser build stubs
`setjmp` via `src/wasm-shim/setjmp.h` and compiles **without** the sjlj
lowering. This spike compiled the browser object *with* it, and it links and
validates -- `libsetjmp.a` has no wasi imports (only `__stack_pointer`), and
the shipping browsers' EH support is the same proposal wasmtime gates behind
`wasm_exceptions`. So a Rust browser build can have real `pcall` semantics
rather than a stubbed setjmp; whether the existing JS artifact should follow
is that build's own decision.

## What this settles

The Rust runtime work (diluvium-drt) can assume same-module linking on every
target: native (already proven by `diluvium-sys`' build script), wasip1/
wasip2 under wasmtime, and the browser with an embedder shim. The
nested-module shape (`diluvium-wasmtime`) remains what it always was -- a
sandboxing tier and the eventual multi-version engine -- not a workaround
anything here forces.

## In the tree, not just in the spike

Everything above was first measured out of tree. It is now what
`diluvium-sys` does, because the crate could not previously cross-compile at
all -- and said so in the worst possible way.

**The false green this closed.** `build.rs` shelled out to a bare `cc` with
no `--target` and branched on `cfg!(target_os = ...)`, which in a build
script describes the **host**. So `cargo build --target wasm32-unknown-unknown`
compiled an x86-64 object, emitted `-DLUA_USE_LINUX` and `-lm`/`-ldl`, and
finished green -- a library crate is never linked, so nothing looked. Forcing
a link was the only way to see it:

```
rust-lld: warning: archive member 'onelua.o' is neither Wasm object file nor LLVM bitcode
rust-lld: error: unable to find library -lm
rust-lld: error: unable to find library -ldl
```

What replaced it: every decision reads `TARGET`; a wasm target with no
wasm-capable C toolchain is a hard error naming `WASI_SDK_PATH` rather than a
host object; the EH flags are probed once so an old clang gets one sentence
instead of a wall of unknown-argument noise; and the compiled object's magic
bytes are checked against the target before it is archived. That last check
is the one that would have caught the original bug by itself, and it is the
artifact being checked rather than a downstream symptom -- `tests/link.rs`
forces a link on every target as a second layer, but on
wasm32-unknown-unknown that layer is not sufficient alone: the target links
with `--allow-undefined`, so a symbol the archive failed to provide becomes
an `env::` import instead of an error.

**Two further operational facts**, found running the real suites rather than
the spike guest:

- `wasm_compat.c` carries the three libc stubs (wrinkle 3) as **weak**
  definitions, plus `clock()` for wasip2 (wrinkle 4), so a libc that has its
  own wins and nothing collides. That is what makes the archive safe to link
  against whichever libc a consumer brings.
- **Running a module that carries the interpreter needs the exceptions
  proposal turned on explicitly at the CLI**: `wasmtime run -W exceptions=y`.
  The embedded API's `config.wasm_exceptions(true)` is the same switch by
  another name. A test binary that happens to GC the EH code away runs
  without it, which makes the failure look intermittent -- it is not; it is
  whether the linker kept a `try_table`.

Measured after the fix, with `WASI_SDK_PATH` set: the `diluvium-sys` and
`diluvium` suites -- snapshots, budgets, endpoints and all -- run **39 tests
green under wasmtime on both wasip1 and wasip2**, and the browser target
compiles a genuine wasm object and links. `dv_layout`'s ILP32 assertion
passes there too, which is the first time that check has run on a target
where it could actually fail. Native is unchanged.

One test is `cfg`-gated off wasm: `an_instance_moves_between_threads` spawns
an OS thread, and wasm has none. The property it exercises (`Send`) is
compile-time everywhere.
