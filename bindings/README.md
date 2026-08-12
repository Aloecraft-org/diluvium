# Host bindings

The `dv_*` ABI in `src/dv.h` is small enough that a binding is mostly
transcription. What took thought was the *wrapper* decisions, and they generalise
— a new binding should copy these rather than rediscover them:

| Decision | Why |
| :--- | :--- |
| **Field names cross the wire.** A struct, object or dataclass becomes a msgpack **map**, keyed by name. | rmp-serde's default encodes a struct as an *array of field values*, which couples every guest to the declaration order of a host type, invisibly, until a field moves. Found by a Lua guest reading `order.qty` as nil. |
| **Check the version before anything else**, and refuse a mismatch. | A wrapper that guessed could misread a message rather than failing. `dv_new` also checks, so both ends do. |
| **Read the wait-set back after a resume.** | `dv_resume` can return `DV_IDLE` — a program looping on `queue.wait` parks again immediately — and its signature has nowhere to put one. A zeroed struct reads as "parked on no queues", which is wrong in the way that looks like working. Use `dv_waitset_get`. |
| **One instance, one thread**, said in the local idiom. | Rust: `Send` but not `Sync`. Python: the owning thread is recorded and a call from another is reported. JS: single-threaded by construction. |
| **Never hardcode a struct offset.** Call `dv_layout`. | wasm32 is ILP32, so anything holding a pointer or `size_t` is laid out differently there than on the LP64 machine a developer would measure it on. That bug cannot be caught by testing locally. |

## What is here

| Binding | State |
| :--- | :--- |
| `rust/` | Complete. `diluvium-sys` (raw FFI, builds the amalgamation) and `diluvium` (safe wrapper, `rmp-serde`). 16 tests, a doctest, an example host. |
| `python/` | Complete. cffi in API mode, so a version mismatch fails at build time rather than at the first call. 17 tests. |
| `js/` | Complete, and now verified end to end. Codec cross-checked against the C implementation (15 tests), WASI host (11 tests), and the wrapper against a real `diluvium.wasm` in CI. Loading a real module still needs the wasi-sdk in a container, so that last part runs only there. See below. |
| `rust/diluvium-wasmtime/` | Complete, and now verified end to end: engine configuration (3 tests) plus the sandboxed example against a real `.wasm` in CI. Needs the container for that last part. Gives containment and fuel metering; see below. |

## The JS wrapper's gap, stated plainly

`js/src/msgpack.js` is tested hard, and against vectors generated *by*
`src/dmsgpack.c` rather than against the spec as read by whoever wrote the
JavaScript — so cross-implementation agreement on the wire format is a checked
fact.

`js/src/index.js` was written against `dv.h` and the wasm build's exports, but
`make build_wasm` needs a container running the pinned wasi-sdk, so the only place
a real module is ever loaded is the CI job `js-binding`.

**What that cost, and what was done about it.** The wrapper instantiated the module
with no imports at all, and the wasm is linked against the wasi-sdk's libc, so it
imports `wasi_snapshot_preview1` whether or not any program touches a file -- the
imports come from libc's own startup and stdio. Every CI run for weeks failed with
`Import #0 module="wasi_snapshot_preview1": module is not an object or function`,
before a line of Lua ran, and nothing a developer could run locally would have said
so.

`js/src/wasi.js` is now a minimal preview-1 host: a clock, randomness, writes to
stdout and stderr, and refusals for everything else. `node:wasi` was not used
because this wrapper is meant to run in a browser too. Two decisions worth knowing:

- The stubs are **synthesized from the module's own import list**, via
  `WebAssembly.Module.imports()`. A wasm module must have every declared import
  satisfied or instantiation fails outright, and which ones libc declares depends on
  the sysroot and the link flags -- so a wasi-sdk bump adds a call that answers
  ENOSYS instead of breaking the wrapper. A fixed list is how this breaks again on
  someone else's build.
- The `DataView` is rebuilt on every call rather than cached, because `memory.grow`
  detaches the old `ArrayBuffer` and a cached view then throws on every access. The
  test that covers this had to be written carefully: a lazily cached view is only
  created after the grow unless something writes *before* it, and the first version
  passed with the bug present.

`js/test/wasi.test.js` proves the import object is complete and behaves; whether the
exports the wrapper reads are the ones the runtime provides is a question only `make
build_wasm` answers -- **and it has now answered it**. The integration test loads a
real `diluvium_wasi.wasm`, runs a program, reads `inbox`'s capacity through
`dv_layout`, and carries an error across with its traceback. So this file has moved
from reviewed code to working code, which it had never been before.

What remains true is narrower and worth keeping: nothing here can be run without a
container, so a regression in the wrapper's struct reads surfaces in CI and nowhere
else. That is why the WASI host got its own tests rather than being left to the
integration step -- one of the two failure modes now has local cover, and the other
does not.

Two things were done to shrink what can be wrong in it: every struct offset comes
from `dv_layout` instead of a constant, and every allocation inside the guest's
memory goes through one `Scratch` scope rather than being freed by hand at each
call site.

### The swarm module

`dist/diluvium_swarm_wasi.wasm` is the swarm layer's artifact, and it is a
*separate* module on purpose: `dvs_new` takes C function pointers a JavaScript
host cannot make, so `src/dvs_shim.c` puts the trampolines on the C side as
mandatory `env` imports (`js_host_create` / `js_host_destroy` /
`js_host_drive`) and exports `dvsjs_new` in `dvs_new`'s place. Mandatory means
mandatory — linked into `diluvium_wasi.wasm` those imports would break
`wasmtime diluvium_wasi.wasm` and every other pure-WASI consumer, which is why
they live in their own artifact. The wrapper's `instantiate()` supplies the
`env` trampolines always (they are inert for modules that don't import them);
`setSwarmHost(instance, host)` installs the real host — `drive(id, inst, ctx)`
required, `create`/`destroy` optional — and the default `drive` throws by
name, so a forgotten host is a message rather than a silently dropped
instance. `js/test/swarm.integration.mjs` is the proof-of-life for all of it,
and is deliberately not in CI until it has passed once somewhere (the
`verify_wasm` lesson): run it by hand against a built module first.

## WASI under wasmtime

`rust/diluvium-wasmtime`. Rust, but **not the `diluvium` crate** — a third
binding, and the reason is the same as the JS one. Linking the static library gives you addresses; hosting the
`.wasm` in wasmtime gives you u32 offsets into a linear memory you have to
allocate inside. The pointer discipline is completely different, so one crate
serving both would collapse to whichever surface is weaker.

The public API can and should be identical, so host code moves between them
unchanged. What differs is underneath: `Memory::read`/`write` instead of
dereferencing, the module's exported `malloc` to pass anything in, and a
`Func`-backed table slot if notification is wanted.

Why choose it over linking natively: **containment and fuel**. A statically linked
Diluvium bug is a bug in your process; a wasm one is confined to the module's
linear memory. And wasmtime's fuel metering does what §9.4's instruction budget
does but from *outside* the guest — §9.4 uses `lua_sethook`, which runs inside the
thing it limits, so a program cannot outlast fuel or spin somewhere a hook never
fires. `Instance::set_fuel` is that, and the example demonstrates it stopping
`while true do end`.

The cost is speed, and a copy per message that the native binding does not make.

**The engine has to enable the exception-handling proposal, and that is not
optional.** The wasi-sdk lowers `setjmp`/`longjmp` onto EH instructions (`-mllvm
-wasm-enable-sjlj` in the Makefile) and Lua's error handling is built on `longjmp`,
so `throw` and `try_table` are in every `diluvium.wasm` whether or not a program
ever raises an error. The crate was pinned to wasmtime 27, which has no way to
enable EH at all -- the feature is not plumbed, so there is no flag to set -- and
every CI run for weeks failed with `exceptions proposal not enabled (at offset
0xd60)`. It is now wasmtime 43, with `config.wasm_exceptions(true)` set explicitly
even though that is the default, because the default is gated behind wasmtime's `gc`
feature and this crate builds with `default-features = false`. Stating it makes
dropping the feature a compile error rather than a runtime one, which is the
difference between a mistake caught in a second and one that took weeks.

43 rather than the newest, and the reason is the CI job: it uses whatever stable Rust
the runner ships rather than pinning a toolchain, and wasmtime 47's own MSRV is
1.94.0 — the current stable, with no headroom. 43 asks for 1.91.0 and has the same
exception-handling support, which is all this crate needs from a recent wasmtime. The
crate now declares `rust-version` so cargo's resolver enforces that rather than
leaving it to whoever bumps the dependency next.

The feature to ask for is **`gc-null`**, not `gc`: `gc` alone compiles and then
fails at `Engine::new` with "none of the collectors are available", and a guest that
only needs EH never allocates a GC object, so the collector that cannot collect is
the correct one.

`diluvium-wasmtime/tests/engine.rs` covers this with a hand-written module
containing a `throw`, which needs no container -- the point being that the property
the crate depends on is now checked somewhere a developer will see it.

Writing it turned up a real gap in the ABI, which is now `dv_endpoint_allow`: the
endpoint bind handler was a C function pointer, and in wasm a function pointer is
an index into the module's function table — there is no way to hand one in from
outside. So a host can now pre-authorise a reference instead, mapping bytes to a
token up front. That is the better shape for every host, not only wasm ones: a
host almost always knows what its own references mean, and saying it up front
needs no call out at all.
