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
| `js/` | Codec complete and cross-checked against the C implementation (15 tests). **The wasm wrapper is unverified**: building `diluvium.wasm` needs the wasi-sdk, which was not available where it was written. See below. |
| `rust/diluvium-wasmtime/` | Written, compiles, **unverified** for the same reason: it needs a real `.wasm`. Gives containment and fuel metering; see below. |

## The JS wrapper's gap, stated plainly

`js/src/msgpack.js` is tested hard, and against vectors generated *by*
`src/dmsgpack.c` rather than against the spec as read by whoever wrote the
JavaScript — so cross-implementation agreement on the wire format is a checked
fact.

`js/src/index.js` is not tested at all. It was written against `dv.h` and the
wasm build's exports, but `make build_wasm` needs a container running the pinned
wasi-sdk, and that was unavailable. The CI job `js-binding` builds the wasm and
runs an integration test; until that has passed once, treat the file as reviewed
code rather than working code.

Two things were done to shrink what can be wrong in it: every struct offset comes
from `dv_layout` instead of a constant, and every allocation inside the guest's
memory goes through one `Scratch` scope rather than being freed by hand at each
call site.

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

Writing it turned up a real gap in the ABI, which is now `dv_endpoint_allow`: the
endpoint bind handler was a C function pointer, and in wasm a function pointer is
an index into the module's function table — there is no way to hand one in from
outside. So a host can now pre-authorise a reference instead, mapping bytes to a
token up front. That is the better shape for every host, not only wasm ones: a
host almost always knows what its own references mean, and saying it up front
needs no call out at all.
