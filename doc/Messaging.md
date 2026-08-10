# Diluvium Messaging, Supervision, and Hibernation

Status: design, ready for implementation
Target: Diluvium 5.5 (current build3)

Scope note: this document covers host embedding and the capability plumbing
that reaches it, which `doc/ROADMAP.md` previously placed out of scope for
this repository. That boundary has moved; see the scope paragraph at the top
of the roadmap, which now points here.

---

## 1. Purpose

This document specifies the messaging and lifecycle foundation for Diluvium. It
covers five interlocking pieces:

1. An embedded msgpack codec, exposed to Lua and used internally as the canonical
   value-encoding format.
2. A queue subsystem: named, guest-declared, bounded message queues addressed by
   integer handle, usable both within a program and across the host boundary.
3. A delivery model with a deliberately weak guarantee, so that routing, discovery,
   broadcast, and retry live in Diluvium programs rather than in the runtime.
4. Supervision and lifecycle, so Diluvium programs can manage other Diluvium
   programs.
5. Hibernate and restore, so a program can snapshot itself at a point it nominates
   and be brought back later.

This work is independent of any particular application. It belongs in Diluvium.

### 1.1 Why this is the enabling piece

- Waiting becomes a yield rather than a block, which is the only form that works on
  the browser main thread and the only form compatible with hibernation.
- Every waiting capability routes through one chokepoint, so permissions, logging,
  and test mocking are uniform rather than per-capability.
- One wire format serves intra-program messaging, cross-boundary messaging, and
  snapshot encoding.
- Supervision becomes a capability rather than a runtime feature, so supervision
  policy is itself a rewritable Diluvium program.

### 1.2 Relationship to a future hostcall document

Section 8 states the rule that decides whether a capability is a direct call or a
queue protocol, and it is deliberately short. The capability inventory, the token
model, and the per-capability permission points are separate work, and when they
acquire real content they should move to their own document rather than growing
this one. That split is planned, not forgotten. It has not happened yet because a
one-page rule in its own file drifts from the document that owns the mechanism it
describes.

---

## 2. Non-goals

Explicitly out of scope:

- decQuad and decimal numerics. Section 5.5 reserves the ext code only.
- Request/response correlation, RPC helpers, retry, and broadcast fan-out. These
  are Lua-level libraries and specialized agents, not runtime features. See
  Section 7.4 for why this is a load-bearing decision rather than a deferral.
- Multi-consumer queues with acknowledgement and redelivery.
- Any package manager or dynamic module resolution beyond what already exists.
- Mid-execution hibernation below a C frame that carries no continuation. See
  Section 10.2 for what this does and does not exclude.
- Snapshot authentication. Deferred behind a checkable precondition; see 10.10.

---

## 3. Inherited constraints

Pre-existing project constraints. Not up for renegotiation during implementation.

| Constraint | Implication |
|---|---|
| Core Lua patches must stay separable from layered logic for upstream syncs | Parts 1 through 5 require zero *new* core files; see 3.1 for the two existing ones they touch. Part 6 requires reading core internal headers but still zero edits to existing core files. |
| Tiny compiled binary is a primary product claim | Everything is size-budgeted and the swarm layer is a separate library. See 3.2. |
| 100% backward compatibility with standard Lua | New surfaces are library tables, not syntax. No grammar changes. **And no removal of existing capability:** in particular, yielding across `pcall` is legal Lua and must remain legal. See 8.4. |
| Targets: mac, windows, arm64 (rpi), musl/portable, wasm | The C ABI and packaging must cover all five. |

### 3.1 Core patch policy, stated precisely

There is a distinction that matters here and it should be maintained deliberately:

- **Patching core files** means editing `ldo.c`, `lstate.h`, `lvm.c`, and so on.
  This creates merge conflicts on every upstream sync. **Do none of this.**
- **Depending on core internal headers** means a new file that includes
  `lstate.h` and reads fields from `CallInfo`, `UpVal`, and friends. This creates
  a compile break when upstream changes those structs, which is loud and
  localized rather than a silent merge hazard.

Parts 1 through 5 need neither. Every mechanism they use is in the public API:
`lua_yieldk`, `lua_callk`, `lua_pcallk`, `lua_isyieldable`, `lua_resume`,
`lua_status`, `lua_sethook`.

Two existing files are touched, both already on the allowlist in
`script/patch_series.sh`:

- `lua.c`, whose allowlist reason already records that REPL input handling moved
  out to `drepl.c`. The call driver follows that precedent: the mechanism is in
  `dtask.c` and `lua.c` keeps a delegation plus one option. Do not grow `lua.c`
  with a new mechanism.
- `onelua.c`, to include new layered sources in the amalgamation.

Part 6 needs the second kind of dependency. Confine it to a single new source
file that acts as an accessor shim. Nothing outside that file may include a core
internal header. When upstream moves a field, exactly one file fails to compile.

If an implementation path appears to require editing a core file not already on
the allowlist, stop and raise it.

### 3.2 Size budget

Measure and report stripped object size for each component on the musl and wasm
targets, via `script/build_stats.sh`, which already fails over a threshold. Wire a
gate per milestone so a budget overrun is refused rather than merely logged.

Working targets. Revised at M5 with real numbers, which is what this section
said would happen:

| Component | Target | Measured (linux-x86_64, `-O3`, stripped text) |
|---|---|---|
| msgpack codec | under 25 KB | 16.0 KB, snapshot mode included |
| queue subsystem | under **20 KB** | 17.6 KB — `dqueue.c` 15.0 + `dendpoint.c` 2.6 |
| instance C ABI | under 10 KB | 6.1 KB |
| hibernate | under 30 KB | 22.7 KB — `dshim.c`, `dhash.c`, `dsnap.c` |
| swarm layer | separate library, not counted | not built |

The queue target moved from 15 KB, and the reason is worth recording rather than
quietly adjusting: it was set against 6, which describes queues alone. What
arrived with them was the byte-level host path — `push_bytes`, `peek_bytes`,
`stat`, the notification hook — which 6 does not mention because it is 11's
concern, and endpoints, which 7 treats separately. Splitting `dendpoint.c` out of
`dqueue.c` was worth doing on its own merits and does not change the total, so the
honest move was to revise the number rather than shuffle bytes between files to
meet it.

The total is roughly 8% against the advertised ~1 MiB runtime. That is the reason
the codec is compiled in rather than vendored per host: one copy serves the Lua
surface, the queue encoding, and the snapshot encoder.

---

## 4. Architecture

### 4.0 The only boundary is the instance

Read this before anything else in the document, because it removes most of the
complexity the rest might otherwise seem to imply.

**There is exactly one isolation boundary in this system: the instance.** An
instance has its own heap, its own queues, its own capability set, its own budget,
and its own snapshot. Nothing else crosses it but msgpack bytes.

There is exactly one kind of program. Diluvium is Lua 5.5 with extensions; there is
no separate host language, no supervisor language, and no privileged program type.
"Supervisor", "coordinator", "handler", "router", and "agent" are all words for a
program that happens to hold a particular capability. None of them is a type, none
has a lifecycle of its own, and none appears anywhere in the runtime.

Because upvalues and object references cannot leave a `lua_State`, there is no rule
needed to prevent them crossing an instance boundary. It is structurally impossible
rather than enforced.

### 4.1 Layers

Four layers. Keeping them separate is what stops the runtime from growing a
scheduler.

| Layer | What it owns | Where it lives |
|---|---|---|
| Guest libraries | msgpack, queues, yield semantics, hibernate entry point | inside the Diluvium binary |
| Instance ABI | embedding one instance: load, run, push, pop | `dv_*` C ABI |
| Swarm layer | instance table, parentage, capability sets, lifecycle drain, budget enforcement, snapshot cache | separate optional library (`libdiluvium-swarm`), built on the instance ABI |
| Programs | everything else, including all supervision and routing behavior | Diluvium |

An app embedding a single scripting sandbox links only the first two and never
pays for the rest. A multi-instance host links the swarm layer as well.

The swarm layer cannot spawn anything by itself, because spawning means
something different in every environment: a wasm instance, a task, a worker, a
process, a job on another machine. The portable part is bookkeeping. The host
supplies a small vtable for creating, destroying, and driving execution contexts.

```
  Diluvium program (agent / supervisor / router)
        |  msgpack.*   queue.*   hibernate()
        v
  +-------------------------------------------+
  |  Layered Diluvium libraries (new)          |
  +-------------------------------------------+
        |  public Lua C API (+ accessor shim for hibernate)
        v
  +-------------------------------------------+
  |  Core Lua 5.5 (no edits)                   |
  +-------------------------------------------+
        |  dv_* instance ABI
        v
  +-------------------------------------------+
  |  Swarm layer (optional, separate lib)      |
  |  instance table, endpoints, budgets, cache |
  +-------------------------------------------+
        |  host vtable: create / destroy / drive
        v
  +-------------------------------------------+
  |  Host: tokio, wasmtime, JS, Python, C      |
  +-------------------------------------------+
```

### 4.2 The one seam the layer table understates

Endpoint references serialize as ext 0x02 (5.5), and resolving one means asking the
swarm layer's instance table. Taken naively that makes the codec depend on the
swarm layer, which would contradict the table above.

It does not have to. The codec takes an **optional resolver callback**. With no
resolver installed, ext 0x02 decodes to an opaque value carrying its raw bytes
rather than failing, and encoding an endpoint reference is an error naming the
missing resolver. The swarm layer installs a resolver; a single-instance embedder
never does and never links it.

State the seam as an injected interface, so it is designed rather than discovered.

---

## 5. Part 1: msgpack codec

### 5.1 Source

Derived from lua-cmsgpack 0.4.0 (`lua_cmsgpack.c`, as vendored by Redis).
**MIT licensed, not BSD** as this section first said; Apache-2.0 compatible
either way, and `NOTICE` records it.

The draft warned to expect real porting work because the file targets Lua 5.1.
It no longer does: upstream is version-guarded throughout (`#if
LUA_VERSION_NUM < 502` and `< 503`) and already branches on `lua_isinteger`, so
`luaL_register` does not appear, `lua_objlen` is behind a guard and
`lua_setfenv` is absent. The porting table below is kept for the record but
almost nothing in it applied.

What the work actually was, in descending order of size: the ext registry (5.5),
which upstream has no trace of; the buffer, the integer decoding and the float
width policy, all rewritten for reasons recorded in the file header; and the
array-versus-map rule, which upstream disagrees with. Landed as
`src/dmsgpack.c`, registered through `src/dlibs.c`.

### 5.2 Required porting changes

| Issue | Action |
|---|---|
| `luaL_register` | Replace with `luaL_newlib` |
| `lua_objlen` | Replace with `lua_rawlen` |
| `lua_setfenv` if present | Remove, use upvalue-based state |
| No integer subtype in 5.1 | Branch on `lua_isinteger`. Encode integers as msgpack int, floats as float64. Round-tripping an integer must yield an integer. |
| Recursion depth | Keep the existing depth cap. It is the only protection against cyclic input in the plain codec. |
| Cyclic tables | Must raise a clean Lua error, not crash or truncate. Add a test. |

Note that the hibernate encoder in Part 6 *does* handle cycles, via backreferences.
The plain `msgpack.encode` does not. These are two entry points into shared
encoding machinery, not two codecs.

### 5.3 Array vs map disambiguation

Specify this explicitly rather than inheriting Redis's behavior by accident.

**Rule:** a table encodes as a msgpack array if and only if it has at least one
element, its keys form a dense integer sequence `1..n`, and it has no other keys.
Every other table, including the empty table, encodes as a map.

Provide explicit overrides so programmers never have to reason about the heuristic
when shape matters:

```lua
msgpack.encode(msgpack.as_array(t))
msgpack.encode(msgpack.as_map(t))
```

Encode-time markers, not persistent types. Implement as a lightweight tagged
wrapper the encoder recognizes and unwraps.

### 5.4 Lua API

```lua
msgpack.encode(value)            -> string
msgpack.decode(str)              -> value
msgpack.decode(str, offset)      -> value, next_offset
msgpack.as_array(t)              -> tagged wrapper
msgpack.as_map(t)                -> tagged wrapper
msgpack.ext(code, data)          -> tagged wrapper
```

Errors are raised, not returned, consistent with the standard library.

`msgpack.ext` was not in the first draft. It is required to make 5.5 coherent:
that section says decoding an application-range code must surface its bytes to
the program, which is half a feature if the program cannot then produce one.
`code` must be in 0x10-0x7F, so a program cannot mint a reserved code. A decoded
ext arrives as the same wrapper shape, with the payload at `[1]` and the code at
`[3]`, which is what makes an ext value round-trip through a program that does
not understand it.

### 5.5 Ext code registry

| Code | Meaning | Status |
|---|---|---|
| 0x01 | Decimal value | Reserved for decQuad. Do not implement. |
| 0x02 | Endpoint reference | Implemented in Part 3. Requires a resolver; see 4.2. |
| 0x03 | Proto reference by hash | Implemented in Part 6, carried inside 0x06 rather than standing alone. |
| 0x04 | Backreference | Implemented in Part 6. |
| 0x05 | Persisted userdata | Implemented in Part 6. |
| 0x06 | Closure | Implemented in Part 6. |
| 0x07 | C function by permanent name | Implemented in Part 6. |
| 0x08 | Suspended thread | Implemented in Part 6. The draft's registry had no code for a thread; 10.3 requires one. |
| 0x09 - 0x0F | Diluvium core, unassigned | Reserved. |
| 0x10 - 0x7F | Application-defined | Free for host and program use. |

Decoding an unknown ext code in 0x00-0x0F must be a clean error naming the code.
Ext 0x01 specifically must say decQuad is not yet implemented rather than producing
a generic unknown-ext error.

Decoding an unknown code in the application range must surface the raw bytes and
code to the program rather than failing, so applications can define types without a
runtime change.

Codes 0x03 through 0x07 are only valid inside a snapshot stream. `msgpack.decode`
must reject them.

**0x04 is the codec's own**; the other four belong to the snapshot layer through a
hook seam, the same shape 4.2 uses for 0x02. Backreferences have to be the
codec's, because only the codec walks tables and the position a backreference
names is assigned by that walk.

**0x03 does not appear as a standalone object.** Its payload — an inline flag, a
32-byte hash, and the stripped dump when inlined — is carried inside every 0x06
closure record instead. A snapshot has no bare prototypes to point at: a
prototype is not a Lua value, so it could not take a position in the object graph
without inventing a pseudo-value for it, and nothing would be gained. The code
stays assigned and its payload layout is exactly what the table says, so a future
standalone use needs no renumbering.

**0x03 and 0x07 take no position in the object graph; 0x05 and 0x06 do.** A
prototype is not a value, and a permanent is a name for something both processes
already have — neither is part of the graph. Persisted userdata and closures are.
This asymmetry is part of the format rather than an implementation detail: give a
position to something the encoder did not, or withhold one it did, and every
backreference past the first such object silently resolves to the wrong object,
with no error anywhere. Nothing in the test file noticed when 0x07 was wrongly
given one, until a case was written whose backreference *crosses* a permanent.

### 5.6 Non-encodable values

Encoding a function, coroutine, or userdata without a hook is an error outside a
snapshot context. The error message must name the offending value's type and its
key path within the containing table.

This message is the main diagnostic a programmer gets when their state is not
serializable. It is worth making good.

---

## 6. Part 2: Queue subsystem

### 6.1 Model

- Queues are **declared by the guest program**, not the host. The host attaches to
  queues that exist; it does not create them.
- Queues are **volatile**. They do not outlive the program instance except through
  an explicit snapshot.
- Queues are **bounded**, always. There is no unbounded option.
- Queues are **dumb**: no designated producer or consumer, no acknowledgement, no
  redelivery. Either side may push and pop.
- The only guarantee is that `push` reports whether the message was accepted. See
  Part 3.
- Queues can be **enabled and disabled**, so a program going down rejects new
  messages cleanly rather than accepting messages it will drop.

### 6.2 Handles

`declare` and `lookup` return a dense integer handle. All subsequent operations
take the handle. No string matching on the hot path.

Handles are **runtime identity, not durable identity.** They are valid only for the
current instance incarnation. A program must not store a handle in state expected
to outlive the instance. On restore, handles are re-resolved by name (Section 10.8).

Document this prominently. Storing a handle and restoring it later is the obvious
mistake and it fails by silently addressing the wrong queue.

**Handles are therefore never reused within an instance.** A destroyed handle
stays destroyed, so using a stale one raises instead of hitting whichever queue
took its place.

One exception, and it is principled rather than convenient: `queue.wait` accepts
a **destroyed** handle and fires `"closed"` for it, while still raising for a
handle that was never issued. `push` and `pop` are operations *on* a queue and
have no way to express "it is gone"; `wait` waits *for* queues, 6.3 says it
resumes when a listed one is disabled or destroyed, and 6.4 gives that its own
status. Without the distinction a destroyed queue in a wait-set would abort a
wait that a listed sibling could have satisfied — and a typo would still be
silently accepted, which is why "never issued" stays an error. That converts the failure this section warns about from a wrong
answer somewhere else into an error at the call site. The cost is that a program
churning queues walks the handle space upward, which no real program does.

### 6.3 Lua API

```lua
queue.declare(name, opts)     -> id
queue.lookup(name)            -> id | nil
queue.destroy(id)

queue.push(id, value)         -> ok, status
queue.pop(id)                 -> value, status      -- never yields
queue.wait(ids, timeout_ms)   -> id, value, status  -- yields

queue.enable(id)
queue.disable(id)

queue.len(id)                 -> integer
queue.capacity(id)            -> integer
queue.state(id)               -> "enabled" | "disabled"
queue.info(id)                -> table
```

`queue.info` was not in the first draft and is needed to make `exported` and
`direction` more than write-only: both are recorded at declare time and nothing
could read them back, which made them untestable and invisible to a program
deciding how to treat a queue it did not declare. It returns `name`,
`capacity`, `on_full`, `exported`, `direction` and `len`.

A name must be a **string**, and a number is not coerced into one.
`luaL_checkstring` would accept `5` and declare a queue called `"5"`; since
handles are integers, a name that reads like a handle is a mistake worth making
impossible rather than merely unlikely.

`opts` fields:

| Field | Default | Meaning |
|---|---|---|
| `capacity` | 64 | Maximum queued messages. Must be > 0. |
| `on_full` | `"reject"` | One of `"reject"`, `"drop_oldest"`, `"drop_newest"`, `"block"`. |
| `exported` | `false` | Whether the host can see and attach to this queue. |
| `direction` | `"both"` | One of `"both"`, `"guest_write"`, `"guest_read"`. Recorded but not enforced in this milestone. Reserved so the wire format does not change when enforcement lands. |

`on_full = "block"` yields the caller until space is available or the queue is
disabled. All other policies return immediately.

`queue.pop` never yields. On an empty queue it returns `nil` and status `"empty"`.
Programs that want to wait use `queue.wait`.

`queue.wait` takes a list of handles and yields with a wait-set. It resumes when
any listed queue has a message, when the timeout elapses, or when a listed queue is
disabled or destroyed. Return the handle that fired.

### 6.4 Status values

String statuses at the Lua level. No numeric codes.

| Status | Meaning |
|---|---|
| `"ok"` | Accepted |
| `"full"` | At capacity, not accepted |
| `"disabled"` | Queue disabled, not accepted |
| `"empty"` | Nothing to pop |
| `"dropped_oldest"` | Accepted, oldest message evicted |
| `"timeout"` | `queue.wait` elapsed |
| `"closed"` | Queue destroyed while waiting |
| `"gone"` | Endpoint no longer resolvable. See Part 3. |

A full, disabled, or gone destination is a **normal outcome, not an error.** Do not
raise.

`on_full = "drop_newest"` has no status of its own, and does not need one: the
newest message is the one being pushed, so dropping it and rejecting it are the
same event. It reports `"full"`.

`"empty"` is also what distinguishes an absent message from a `nil` one, since
`nil` is encodable and can be pushed. A `pop` returning `nil, "ok"` delivered a
nil; `nil, "empty"` delivered nothing.

Disabling a queue stops it accepting, not delivering: a disabled queue can still
be drained of what it already holds. 6.1 wants a program going down to reject new
messages cleanly, not to abandon ones it already accepted.

### 6.5 Encoding

All values pushed to a queue are msgpack-encoded at push and decoded at pop,
including for queues that never cross the host boundary.

Deliberate, with a known cost. Reasons:

- Value semantics rather than reference sharing. A pushed table cannot be mutated
  by the sender afterward.
- Queue contents are already bytes when a snapshot is taken.
- One serializability rule for programmers, not two.

If profiling shows this matters for high-frequency internal queues (the
`proc_50ms` feeding `proc_500ms` pattern), a `local` queue kind that skips encoding
is an additive change. Do not build it now. Do record encode and decode timings in
the benchmark suite so the decision can be revisited with numbers.

### 6.6 Reserved names

`inbox` and `outbox` are reserved per-instance defaults, auto-declared at startup
with `exported = true`. A program may re-declare them with different options before
first use. (Confirmed; open question 4 is closed.)

Re-declaring means `destroy` then `declare`: `declare` refuses a name that
already exists rather than reconfiguring it. A call that sometimes creates and
sometimes mutates is worse than one that says which it did, and the new handle
that `declare` returns is the honest signal that the old one is stale.

Names are namespaced with `/`, for example `sensors/gps`. (Confirmed; open question
3 is closed.) The separator is structural only in this milestone; no hierarchical
semantics are implemented. Capability scoping over name patterns is future work and
must not be designed away.

---

## 7. Part 3: Delivery model and endpoints

This part exists because the naive design leaks routing policy into the runtime.
It is short, and it is the most important section in the document.

### 7.1 The problem

A destination may be resident in this process, hibernated to a snapshot cache, in
another worker, on another machine, on a build agent, or gone. If the queue layer
tries to know the difference, it grows a router, a discovery mechanism, a retry
policy, and a timeout policy, all in C, all in the binary, all wrong for somebody.

### 7.2 The guarantee

**`push` reports whether the message was accepted into the next hop. Nothing more.**

The next hop may be:

- a local queue owned by this instance
- a host-managed endpoint bound by the swarm layer
- the inbox of a relay agent that has taken responsibility for finding the
  destination

All three are indistinguishable to the sender, and all three have identical
semantics: accepted, or one of `"full"`, `"disabled"`, `"gone"`.

**`push` never waits on the destination's liveness.** It may yield on local
capacity when `on_full = "block"`, and that is the only case in which it waits at
all. A broadcaster fanning out to two hundred agents cannot be stalled by one slow
or absent destination.

There is no delivery confirmation, no ordering guarantee across endpoints, and no
retry. Those are application concerns.

### 7.3 Endpoints

```lua
endpoint.bind(ref, name)   -> id      -- resolve now, get a queue handle
endpoint.status(id)        -> "live" | "gone"
```

`ref` is an opaque instance reference obtained from the lifecycle protocol
(Part 5), never constructed by the guest. `bind` returns an ordinary queue handle,
so everything downstream is unchanged.

An endpoint handle refers to something whose lifetime the holder does not control.
It is a descriptor to a pipe whose far end can close. When the far end is gone,
push returns `"gone"` immediately. It never blocks and never raises.

Endpoint references serialize as msgpack ext 0x02 so they can be passed in
messages, which is how a coordinator hands a handler's address to a peer. See 4.2
for the resolver seam this implies.

### 7.4 Why routing lives in agents

Because the delivery guarantee is weak and uniform, every richer convention is
expressible as a specialized Diluvium program:

- **Broadcast.** One broadcaster agent holds write capability to every agent's
  `broadcast` queue; nobody else does. Fan-out is an ordinary loop of pushes, each
  of which may be rejected. Capability attenuation, not runtime support, is what
  prevents chaos.
- **Store and forward.** An agent that accepts a message, immediately answers
  "accepted", and then takes responsibility for locating the destination, waking a
  hibernated instance, or reaching across a network. The weak guarantee is exactly
  what makes this composable: the relay's answer means the same thing as a direct
  push's answer.
- **Discovery, retry, dead-lettering, priority.** Same pattern.

This is the payoff for keeping the core dumb. Conventions become programs that can
be inspected, replaced, and rewritten at runtime, rather than C that has to be
right for every topology at once.

**Do not add broadcast, multicast, retry, or delivery confirmation to the queue
layer.** If an application needs them, it writes an agent.

### 7.5 Host-managed endpoints

The swarm layer may bind an endpoint to something that is not a resident local
queue: a snapshot in the cache, a socket to another node, a worker channel. It does
this behind `endpoint.bind`, and the guest cannot tell.

The one requirement: **accepting a message must be O(1) and must not depend on the
destination being reachable.** If the swarm layer cannot accept in bounded time,
it must return `"gone"` and let an agent handle it. A host-side buffer for a
non-resident instance is legitimate as long as it is bounded and drains in FIFO
order ahead of live pushes on restore. A host-side buffer that grows without bound
or that waits on a network is not.

---

## 8. Part 4: Yield-aware hostcalls

### 8.1 The rule

**Direct hostcalls for operations that cannot wait. Queues for everything that
can.**

Pure operations (crypto, math, string utilities, encoding) stay direct C calls.
Anything with latency (network, sensors, LLM calls, timers, inter-instance
messaging) is a queue protocol.

Apply this to new capabilities from here on. There are no existing hostcalls in
this repository to retrofit, so the rule is prospective; when one is added that
violates it, note it for a later pass rather than bending the rule.

### 8.2 Coroutine hosting

Agents run inside a coroutine, always, entered via `lua_resume`. Lua will not
permit a yield from the main thread unless it was resumed, so this is structural
rather than stylistic.

**Coroutine hosting is a new entry path, not a conversion of the existing ones.**
This was drafted the other way and the draft was wrong; see 8.2.1. The driver is
`src/dtask.c`, on-top code using only the public C API, and `lua.c`,
`repl_eval` and `run_lua` keep stock semantics and are not touched.

There are therefore two front doors, deliberately:

| Door | Semantics | Held to |
|---|---|---|
| `lua.c` / the CLI | Stock Lua. Main thread, non-yieldable top level. | The upstream conformance suite |
| The `dv_*` ABI, via `dtask.c` | Coroutine-hosted. Yieldable top level, parks on queues. | This document |

`--task` on the CLI enters through the driver, so `diluvium --task foo.lua` runs
a program that can wait while `diluvium foo.lua` stays stock. It is a new path
rather than a conversion, which is what makes it conformance-safe, and it turns
the CLI into a reference host rather than only a script runner. It is also what
lets messaging semantics be tested from the ordinary `.lua` suite rather than a
C harness per feature, which is why it was built before M1.

`--task` is refused together with `-i`: what an interactive session should show
while a program is parked is a real question, and combining the two would answer
it by accident. When `queue.wait` lands, the wait-set drive loop belongs in
`docall`'s task branch -- that is the point at which this mode stops being an
entry and becomes a host.

"Task" rather than "agent" deliberately: 4.0 makes "agent" an application word for
a program holding a capability, so naming a runtime entry point after it would be
exactly the leak that section exists to prevent. The runtime words here are
instance and task.

### 8.2.1 Why not the existing entry paths

Running ordinary script and REPL chunks on a coroutine is observable from Lua.
Measured on this tree: inside a coroutine, `coroutine.running()` reports
`ismain=false` and `coroutine.isyieldable()` returns true. Four sites in the
suite assert the opposite, and all four are in the default runner table:

- `test/coroutine.lua:10-14` — `ismain` must be true; `isyieldable()` must be
  false; `pcall(coroutine.yield)` must fail. This is the top of the file.
- `test/coroutine.lua:159-163` — `coroutine.close(main)` must error naming "main".
- `test/errors.lua:399` — `coroutine.yield()` must report "outside a coroutine".
- `test/locals.lua:1177` — "attempt to yield from outside".

These are upstream Lua conformance tests asserting exactly what the constraint
table calls non-negotiable. Amending them would erase the conformance oracle in
the one area — yields across C boundaries — that `queue.wait` most needs one for.
More than a test problem: a library branching on `coroutine.isyieldable()` to
decide whether to yield would change behavior in ordinary scripts.

Note for later: `script/wasi_check.sh` drives the standalone binary, not
`repl_eval`, so the WASM REPL path is *not* conformance-tested and could be
converted when Lab is ready to answer what a parked prompt does. That is an M3
decision, not a constraint.

**Implementation constraints, established against this tree and each verified by
removing it and watching a test fail:**

1. **The inner protected call must be `lua_pcallk` with a non-NULL continuation,
   and it must execute inside a C function the resume is running.** `lua_pcallk`
   takes the continuation branch only when `k != NULL` *and* `yieldable(L)` is
   already true (`lapi.c:1095`). Otherwise it falls through to `luaD_pcall`,
   whose `f_call` uses `luaD_callnoyield`, and `nyci` sets the very bit
   `yieldable()` tests — making everything beneath it non-yieldable. A fresh
   thread that has never been resumed is not yieldable, so a driver that creates
   a thread and calls `lua_pcallk` on it directly falls into exactly that trap.

   This is why the acceptance criterion is that `coroutine.isyieldable()` returns
   **true** inside the driver. "Nothing yields and the suite is green" does not
   prove the work achieved anything; that assertion does. Substituting a plain
   `lua_pcall` fails it.

2. **All completion logic goes in the continuation, and an error status must be
   re-raised there.** A continuation runs on error as well as on yield: the raise
   makes `precover` find the `CIST_YPCALL` frame and unroll into it, calling the
   continuation with the error status — while `lua_pcallk` itself assigns
   `status = LUA_OK` unconditionally in that branch (`lapi.c:1112`) and the raise
   long-jumps clean out of the C frame. **The code after the call is unreachable
   on the error path.** A continuation that simply returns its results turns
   every error into a successful resume with exit status 0.

   This is the worst failure mode available here because it looks like a passing
   build, and it is subtle in a specific way: with the re-raise removed, tests
   asserting that the message and traceback are *present* still pass — the
   traceback comes back as a successful return value. Only a test asserting the
   *status* catches it.

3. Install the message handler on the thread, where the error is raised, so the
   traceback describes the failing frames rather than the driver. Frames the
   driver adds below the chunk do not shift `debug.getinfo` levels, which count
   downward from the running function (`ldebug.c:167`), so traceback shape is not
   frame-count sensitive.

4. `lua_checkstack` in both directions. A fresh thread has around 20 free slots,
   and `lua_xmove` does not grow its destination; past the limit it writes beyond
   the stack, which `api_check` catches only in a debug build. This is a
   memory-safety case, not a tidiness one.

5. Anchor the thread in the registry, not on the caller's stack. Callers do
   arithmetic against `lua_gettop`, and a GC step triggered inside the body would
   otherwise be free to collect the thread it is running on.

6. A fresh thread per call. A thread that has raised is dead and cannot be
   resumed, so a cache would have to detect that anyway.

7. Write the `LUA_YIELD` branch even while nothing parks, and make it loud, so an
   untaught caller fails rather than mistaking a suspended thread for a finished
   one.

8. Provide the SIGINT hook point — `lua.c`'s `globalL` is what `laction` installs
   a `lua_sethook` on, and if code runs on a thread while `globalL` names the main
   state, Ctrl-C silently stops interrupting anything. Nothing in the suite
   presses Ctrl-C. Since the CLI is not converted, this hazard is not live today
   and the hook is present but **untested**; that is worth knowing rather than
   assuming coverage.

### 8.3 Wait-set protocol

When a coroutine parks, the host receives the list of queue handles being waited on
and a timeout in milliseconds, or a sentinel for none.

The host owns the clock. There is no in-runtime timer. Timeouts are relative
durations sourced from a monotonic clock, never wall time.

The host resumes with the handle that fired, or with the timeout indication.

### 8.4 Non-yieldable contexts

Before yielding, check `lua_isyieldable`. If false, raise a clear Lua error naming
the situation.

**`pcall` is not on this list.** Yielding across `pcall` has been legal since Lua
5.2 and works in this tree: `luaB_pcall` goes through `lua_pcallk` with a
`finishpcall` continuation, and `yieldable()` counts non-yieldable C calls, which
that path does not increment. `test/coroutine.lua` exercises it. Adding such a
restriction would break the backward-compatibility constraint outright, because
`pcall(function() coroutine.yield() end)` is legal Lua today.

Genuinely non-yieldable:

- inside a `table.sort` comparator
- inside a `string.gsub` replacement function
- inside a `__gc` finalizer
- inside a metamethod invoked from C without a continuation
- on the main thread when not resumed

The message must be specific enough that a programmer knows to restructure.

**This list is not shared with hibernate.** The sets differ, and conflating them is
what produced the `pcall` error above. Hibernate's constraint is Section 10.2.

### 8.5 Permission checks

Capability checks happen **on entry, before the yield**, and the decision is
captured and honored on resume. Never re-check after resume: a token revoked while
the agent was parked would produce authorization that varies with scheduling.

The token model itself is separate work. The check signature is fixed now so the
call site does not move later:

```
(capability, resource, context) -> allow | deny
```

The token model stays entirely opaque behind it. Structure dispatch so this check
point exists at a single obvious location. (Confirmed; open question 5 is closed.)

---

## 9. Part 5: Lifecycle capability

### 9.1 Supervision is a capability, not a runtime feature

Diluvium does not get a `spawn()` primitive. It does not own a process table or a
scheduler. Instead, a program requests a spawn by writing to a reserved queue it
holds capability for:

```lua
local sys = queue.lookup("system/lifecycle")
queue.push(sys, {
  op    = "spawn",
  code  = handler_code_ref,
  caps  = { "queue:work/*", "queue:system/events" },
  budget = { instructions = 5e6, memory_kb = 512 },
  wake_on_message = true,
})
```

The swarm layer drains that queue and acts on it, calling `dv_new` on the
instance ABI. What "spawn" means is the host's business: an instance, a task, a
worker, a process, a job elsewhere. The requesting program is identical across all
of them.

There is no supervisor type. A program holding the lifecycle capability is what the
word describes, and nothing in the runtime distinguishes it from any other program.
Restart strategies, backoff, escalation, and topology are ordinary Diluvium
programs. For a system where programs are generated and rewritten at runtime,
having the supervision policy itself be rewritable is the point.

### 9.1.1 Delegation is recursive and needs no support

A program holding the lifecycle capability may grant it, attenuated or not, to a
child at spawn. That child then holds it and may do the same. Supervisors create
supervisors with no additional mechanism, because there is no type to instantiate:
delegation is recursive for the same reason attenuation is.

This makes "subtree" accurate rather than aspirational. Parentage is a single field
per instance, depth is unbounded, and subtree kill means what it says.

### 9.1.2 What the swarm layer owns, exhaustively

Six things. If something proposed for this layer does not reduce to one of them,
it belongs in a program instead.

1. An instance table, because endpoints resolve against it.
2. One parent field per instance, for subtree kill and attenuation checks.
3. The capability set per instance, for enforcement.
4. Draining `system/lifecycle` and calling the host vtable.
5. Enforcing per-instance budgets (9.4). It enforces the numbers; it does not
   decide them.
6. The snapshot cache and `wake_on_message` delivery (9.5), including the
   host-identity stamp of 10.10.

Not in this layer: restart policy, backoff, naming, discovery, topology, routing,
coordinator and handler roles, or anything describing how programs relate to each
other beyond parentage.

The line is mechanism versus policy. Enforcing a budget is mechanism. Choosing what
a child's budget should be, and what to do when it is exceeded, is a program.

### 9.1.3 An agent is an instance

"Agent" is an application word, not a runtime concept. As this system is designed,
the thing called an agent maps to one instance: its own heap, capability set,
budget, and snapshot.

The alternative, agents as coroutines sharing one instance, is cheaper per agent but
shares a heap, shares a fate, and cannot be snapshotted individually, which breaks
the swap-out-to-cache model in Section 9.5 entirely. Parts 5 through 7 assume
instances throughout.

### 9.2 Reserved system queues

| Name | Direction | Contents |
|---|---|---|
| `system/lifecycle` | guest writes | spawn, kill, query |
| `system/events` | guest reads | child exited, faulted, exceeded budget |

Monitor semantics only. **Do not implement Erlang-style bidirectional links.**
One-directional monitoring covers supervision; links can be layered later if
wanted.

### 9.3 Capability attenuation

**A supervisor must never grant a child more capability than it holds itself.**
Attenuation only, enforced by the swarm layer, no exceptions.

This is what makes a privilege hierarchy structural rather than conventional. An
agent that must submit a request to a reviewer does so because it holds no
capability for the target queue, not because it was asked politely.

### 9.4 Resource control

The swarm layer owns per-instance limits. A guest cannot meaningfully limit
itself, because a runaway loop never yields and nothing cooperative will stop it.

| Limit | Mechanism |
|---|---|
| Instruction budget | `lua_sethook` count hook, public API |
| Memory | the allocator, already pluggable |
| Total queued messages | computable exactly, since every queue is bounded |
| Wall clock | host side |

Use the count hook to **abort, not to schedule.** Raising an error from a hook is
fine; yielding from one reintroduces the C-frame problem and breaks hibernate.

### 9.5 Failure policy

| Situation | Default |
|---|---|
| Supervisor dies | Kill the subtree. Reparenting is harder to remove once depended on. |
| Spawn storm | Rate-limit the lifecycle capability. A self-rewriting system will produce a fork bomb eventually, as a bug rather than an attack. |
| Push to a dead instance | `"gone"`, immediately, never blocking |
| Push to a non-resident instance with `wake_on_message` | Accept into a bounded host buffer, return `"ok"`, restore asynchronously, drain the buffer ahead of live pushes |
| Push to a non-resident instance without `wake_on_message` | `"gone"` |

---

## 10. Part 6: Hibernate and restore

### 10.1 Shape

Hibernation is **self-initiated**. The program receives a request through an
ordinary queue and calls `hibernate()` at a point it chooses.

```lua
local resumed = hibernate(opts)   -- false when snapshotting, true when restored
```

Returns twice. On snapshot it returns `false`; on restore the call returns `true`
and execution continues from that point.

### 10.2 The hard limit, stated so it is not rediscovered

A frame belonging to a C function **that carries no continuation** cannot be
captured, because its working state lives on the machine's C stack. Therefore
hibernation is impossible below such a frame. This excludes hibernating from
inside a `table.sort` comparator, a `string.gsub` replacement, a `load` reader, a
metamethod invoked from C without a continuation, or a generic `for` driven by a C
iterator.

The distinction is continuations, not C-ness. A frame that yielded across `pcall`
carries its resumption state explicitly — `u.c.k`, `u.c.ctx`, `u.c.funcidx`,
`u.c.old_errfunc` — all plain data plus a C function pointer the permanents table
can already resolve. Hibernating across `pcall` is therefore feasible in
principle, unlike a comparator whose state is genuinely on the machine stack.

**It is out of scope for M6 anyway**, because restoring `old_errfunc` and the
error-handler chain correctly is fiddly and nothing needs it yet. The wording is
"C frames without continuations" so the door stays open and nobody later concludes
it was ruled out on principle.

**Implementation requirement:** before capturing, walk the `CallInfo` chain and
refuse if any frame other than the top one has `CIST_C` set and a null
continuation. Return a clean error naming the situation. Never capture partially.

Note the useful consequence: an agent parked on `queue.wait` has no C frame at all,
because waiting is a yield. Idle-on-inbox is the overwhelmingly common state at
scale, and it is capturable.

**"Other than the top one" is load-bearing, and was worth stating.** The top frame
of a suspended thread is by definition the one that called `lua_yieldk`, and a null
`k` there is the ordinary case, not a defect: `ldo.c`'s `resume` takes the poscall
path and hands the yielded values to that frame's caller, so there is no saved C
state to want. `coroutine.yield` is built exactly this way. A walk that checked
every frame uniformly would refuse every ordinary suspended coroutine — which is
what the first implementation did, and what `dshim_check.c` caught on its first
run.

**The list of excluded situations above is right but describes the wrong failure.**
A `table.sort` comparator or a `string.gsub` replacement cannot be *hibernated
from*, but it also cannot *yield* — `lua_call` routes through `luaD_callnoyield`,
which sets the bit `yieldable` tests, so the attempt raises "attempt to yield
across a C-call boundary" rather than parking. So there is no reachable suspended
thread with a continuation-less C frame below its yield. The precondition check is
therefore a tripwire on a VM invariant rather than a gate that fires in practice,
and the thing it guards against is a future host call that reaches Lua with
`lua_call` where it should have used `lua_callk` — which would make the shape
reachable for the first time, quietly. §14 was right to name this as the property
that will break during refactoring; it just breaks by becoming *possible*, not by
being mishandled.

That has a consequence for how it can be tested, since no test over a real thread
can distinguish the correct rule from an over-broad one. The rule is factored out
as `diluvium_shim_framecapturable(frame, is_innermost)`, a pure predicate over a
flattened frame, so it can be exercised with frames the test fills in itself.
Verified by mutation: widening the exemption to all C frames turns exactly one
check red, and it is that one.

### 10.3 What is captured

| Category | Treatment |
|---|---|
| Numbers, strings, booleans, nil | Plain msgpack |
| Tables | Msgpack map or array, with ext 0x04 backreferences for identity and cycles; metatable link and `__mode` preserved (see below) |
| Lua closures | Ext 0x06: a Proto reference plus upvalue references. Protos deduplicated by hash. |
| Upvalues | Serialized by identity. Two closures sharing an upvalue must still share it after a round trip. Use `lua_upvaluejoin` on restore. |
| C closures and functions | Ext 0x07: resolved by name through a permanents table, both directions |
| Full userdata | Ext 0x05, via a `__persist` metamethod that emits a reconstitution descriptor, never raw bytes |
| Light userdata | **Refuse.** A bare pointer with no type information and no hook. |
| Coroutine stack | Value stack, `CallInfo` chain with pc as a code-array offset, base and top as stack offsets, callstatus flags, vararg counts, `tbclist` for pending to-be-closed variables |
| Open upvalues | As stack slot indices, re-opened against the reconstructed stack |
| Queue contents | Already msgpack bytes. Serialized with the snapshot, since queues are guest-owned. |

Restrict v1 to a **single thread**. An agent driving nested coroutines is out of
scope; refuse with a clear message.

**How a thread is written.** Ext 0x08, which 5.5's registry had no code for -- an
omission rather than a decision, since 10.3 requires a thread to be captured. The
payload is the frame metadata only: sizes, one fixed record per frame, the
to-be-closed slots, and the names of any continuations. The stack *values* are not
in it, because a slot can hold a table that holds the thread; they arrive as SLOT
fixups, and a final BUILD fixup validates the frames against the filled stack and
constructs the chain. That order is forced -- a frame claims its function is at
slot N, and nothing can check that until slot N holds something.

**Live slots and reserved capacity are different numbers, and both travel.** A
frame's `ci->top` is `func + 1 + maxstacksize`, which sits *above* the live top by
however many registers the frame is not currently using. Validating a frame's top
against the live count instead of the capacity refused every real thread, which is
how this was found.

**`nextraargs` is only meaningful for a vararg prototype.** `luaT_adjustvarargs`
writes that field and nothing else does, so on a non-vararg frame it holds whatever
the last user of that recycled `CallInfo` left there. Capturing it unconditionally
meant capturing garbage, and the restore then failed its own range check on it.

**To-be-closed slots are marked after the whole graph is finished**, not with the
thread. Marking a slot requires its value to have a `__close` metamethod, and a
metatable arrives as a fixup of its own -- which for a `defer` object, a
self-referential `setmetatable(t, t)`, is queued *later* than the thread holding
it. So marking during the thread's own fixup found a table with no metatable and
refused it. Anything whose precondition is "the graph is complete" belongs in the
finish pass rather than in an ordering assumption.

**How metatables and upvalues travel, and why not inline.** Neither is part of its
owner's contents, and writing either inline would put it in the position numbering
at a place the decoder cannot predict — the decoder has to create the owner before
it can know whether anything followed. So both go in one trailing *fixup* section:
the root value, then tagged fixups, then a nil. Three tags: `META` (position,
metatable), `UPVAL` (position, index, value) and `UPJOIN` (position, index, source
position, source index).

A nil terminator rather than a length prefix because the list grows while it is
being written — a metatable may have a metatable, which is ordinary inheritance —
so its length is unknown when a header would have to be emitted, and buffering the
section to find out would copy the largest part of the graph once per level.

Bare pairs rather than a two-element array per entry, and this one was found by a
failing test rather than by design: an array is a *table* on the wire, and the
decoder gives every table it creates a position. A wrapper therefore takes a
position the encoder never assigned, and every position after the first metatable
is off by one. The symptom was the first metatable restoring correctly and the
rest not.

**Upvalue identity is `UPJOIN`.** Two closures over one variable are written as
one `UPVAL` and one `UPJOIN`, and the decoder replays the second with
`lua_upvaluejoin`. The encoder claims an upvalue for the first closure that
reaches it and drains closures in order, so an `UPJOIN` always follows the `UPVAL`
it refers to. This is the one requirement in 10.3 that no amount of copying values
can satisfy, and the only test that can tell the difference is one that mutates
through one closure and reads through the other — comparing values passes even
when each closure got a private copy.

**Metatables are applied after contents**, which is the only order available: the
owner must exist before anything can reference it, and its contents are read on
the way past. For a weak table that means entries were inserted while it was still
strong and `__mode` takes effect afterwards. The collector reads `__mode` from the
metatable at each traversal rather than caching it at insert time, so the weakness
does apply — but this is the one place in a restore whose result depends on
collector behaviour rather than on the format, and it is worth knowing before
something is built on top of it.

**Userdata is refused, and why ext 0x05 is not implemented.** The natural design
is the one Pluto and eris use: `__persist` returns a reconstructor closure, which
this format already knows how to carry — its prototype is content-addressed, its
upvalues become fixups, and whatever it closed over travels as graph values. It
founders on ordering. The userdata needs its position *before* the reconstructor is
written, so a reference to it from the reconstructor's own upvalues resolves; but on
the way back the reconstructor cannot be *called* until the whole graph exists, and
by then any table that referenced the userdata has already been handed whatever
placeholder stood at that position. The program would find a placeholder where its
file handle should be, and find it silently.

Fixing that needs either a patch-up pass over every reference to the userdata or an
ext that can carry a nested graph value. Until then a userdata is refused with a
message naming `__persist`, which is what 10.7 item 2 asks for.

**Shape wrappers are refused.** `msgpack.as_array(t)`, `as_map` and `ext` are
encoding directives, not program state, and their hidden metatable is shared
runtime furniture that must not enter a snapshot's object graph. Silently
unwrapping would restore a plain table where the program left a wrapper, which is
a difference the program can see, so this is an error with a key path.

**Depth is capped at 150 in snapshot mode, not 16.** The plain codec's cap of 16 is
its cycle guard and the snapshot encoder does not need one, so the only thing left
to bound is C recursion. 16 would refuse a linked list of twenty nodes, which
would be absurd. 150 sits below Lua's own `LUAI_MAXCCALLS`, so a graph that would
overflow the C stack is refused here first, with a message that says so.

### 10.4 The permanents table

Every C function reachable from the program must be in it, not just hostcalls.
`print`, `table.insert`, `string.format`, and the rest of the standard library are
all C closures and traversal hits them immediately.

Build it by walking the base libraries at init. The result must be identical on
save and restore, which means the same runtime build and module set. Include a
fingerprint of it in the snapshot header.

**Continuations need naming too, and the draft did not say so.** A suspended
agent's chain has C frames in it -- dtask.c's driver and `queue.wait` both yield
with a continuation saved -- and a continuation is a bare function pointer with
nowhere to be written down. So it is named exactly as a C function is, in a fixed
array rather than a table keyed by the pointer, because casting a function pointer
to `void *` is not something C promises. Registered next to the `lua_yieldk` call
that installs it, which is an ordering that cannot be got wrong.

`pcall`'s continuation is the awkward one: `finishpcall` is static in `lbaselib.c`,
which is not on the patch-series allowlist, and its address is unreachable any
other way. So it is *discovered* -- park a canary thread inside a `pcall` and read
the continuation off the frame the shim reports. The same trick as the fingerprint
canary, and honest for the same reason: it names what this build actually uses
rather than what a list assumes.

**Two things the permanents table must name that no guest can reach.** dtask.c's
driver body, which is the outermost frame's function on every parked agent, and
`queue.wait`'s light-userdata park marker. Neither is exposed to the guest, so
neither is found by walking the libraries; both are reached explicitly. Without
them, the one shape hibernation is built for cannot be captured at all.

**`_G` and the library tables are in it too**, and that is the part the draft did
not say. They are *tables*, so an encoder will happily serialize them — and then a
snapshot of one closure that calls one global drags the entire global environment
in, every C function in it included, and fails on the first of those. Naming them
instead costs one short ext object for a closure's `_ENV` upvalue. The permanents
check therefore runs on tables as well as functions, and it runs *before* the
object graph gives anything a position.

**Naming has to be order-independent.** A value reachable by two paths — `string`
is both `_G.string` and `package.loaded.string` — must get the same name in every
process, and first-path-wins only settles that if the order of paths is settled. So
the modules are walked in a fixed list rather than by iterating `package.loaded`,
and the fingerprint is computed over the *sorted* name list, so it does not depend
on iteration order even though this tree's iteration order is deterministic. That
costs one sort per state and removes a dependency; a fingerprint that varied
silently would turn every restore into a refusal — safe, but useless.

**A C function that is not in the set is a refusal, and says so.** The message
names the permanents table and says a host registering its own C functions must
name them, because that is the cause every time and the generic "cannot encode a
function value" sends the reader looking in the wrong place.

### 10.5 Content-addressed code

Do not choose between carrying bytecode and not carrying it. Reference by hash any
Proto already present in the target runtime's registry; inline the ones that are
not. A swarm of generated one-off agents sharing a common library then inlines only
the unique part.

Hashing rules:

- Hash over the **stripped** dump. Line numbers and source names change with
  whitespace edits, and a comment reflow must not invalidate every cached agent.
  Carry unstripped debug info alongside if wanted.
- Include runtime identity in the hash domain: Diluvium version and build, opcode
  set, number configuration, `LUAC_FORMAT`, and the permanents fingerprint.
- Restore requires an exact match. Mismatch is a clean refusal, never a
  best-effort load.

**And the hash is verified against the bytes, not trusted.** An inlined prototype
whose bytes hash to something other than the name it carries is refused. That is
not the same check as the loader's: `lverify.c` rejects malformed bytecode, and
would reject most tampering anyway, which is exactly why this needed its own test
— the first version flipped a byte of the instruction stream, the loader caught it,
and the case stayed green with the hash verification removed. Flipping a byte of
the *hash* is the case that isolates it: the bytecode loads perfectly, and what is
wrong is that the record claims a name its bytes do not own. Since the reference
case hands that name out to whoever asks for it later, accepting one would let a
snapshot install code under someone else's name.

**Dedup happens twice, for two reasons.** A prototype already in the target
runtime's registry is referenced — that is 10.5's point, and what a host
pre-registers for a shared library. A prototype already *inlined earlier in the
same stream* is also referenced, which is what makes a swarm of ten agents over
one generated function cost one copy and nine hashes rather than ten copies.

**This depends on dump determinism**, which this tree already has and should keep
deliberately: the string hash seed is fixed (`luaconf.h` defines `luai_makeseed`
as a constant, so the `lauxlib.c` fallback is dead code), `test_determinism.lua`
asserts identical iteration order within and across processes, and string dedup
order is write order.

**Measured, before anything was built on it.** Three processes of one build
produce byte-identical stripped dumps of the same source. That is a test rather
than an assumption now — `test/fingerprint_check.sh`, whose first check is exactly
this, since the fingerprint *is* the hash of a stripped dump.

**And it turned up the correction in 10.6.** Two *different builds* of this same
tree do not agree: debug emits 20 instructions where release emits 12. The cause
is deliberate and reproducible rather than nondeterminism — `src/ltests.h` sets
`MAXINDEXRK` to 1, which is an `lcode.c` codegen knob — but it means "identical
source produces identical dumps" holds per build, not per version.

**It also depends on the secure-function scramble being deterministic.** See
10.9. If the obfuscation ever acquires a per-dump nonce, content addressing breaks;
that trade is refused in 10.9 for exactly this reason.

### 10.6 Snapshot header

Every snapshot carries, before any payload:

| Field | Purpose |
|---|---|
| Format version | Refuse on mismatch |
| Runtime identity | Version, build, opcode set, number config, `LUAC_FORMAT` |
| Permanents fingerprint | Refuse on mismatch |
| Capability set in effect | Refuse restore under a different one |
| Queue name list | For handle re-resolution |
| Host identity stamp | See 10.10 |

**Runtime identity cannot be a list of constants**, and this was corrected after
measuring rather than reasoning. The list above — version, build, opcode set,
number config, `LUAC_FORMAT` — reads as sufficient and is not: the debug and
release builds of this tree agree on every one of those (`diluvium (lua)
5.5.1|85|70` from both) and disagree on the bytecode they emit. A header carrying
only that list would accept a snapshot whose Proto hashes can never match, and
10.5's "exact match" would then fail deep inside restore as a missing Proto
instead of at the header as a refusal.

So the runtime identity is **the SHA-256 of a fixed canary chunk's stripped
dump**. That is self-calibrating: it covers every codegen knob, including ones
added after this was written, which an enumerated list cannot. The dump header
carries the number sizes, endianness and format byte, so those come along for
free; the version string is hashed in alongside so two builds with identical
codegen and different versions are still told apart. Computed once and cached
per state, since it compiles and dumps a chunk.

`test/fingerprint_check.sh` holds the property, and it needs two builds of the
tree — which is why it is a shell script and not part of `dsnap_check.c`. Its
third check asserts the thing that made the correction necessary: that version
and `LUAC_FORMAT` really are identical across the pair, so a reader does not have
to take the paragraph above on trust.

**The header is plain msgpack, not the graph format.** A reader that refuses the
payload must still be able to read the header, including a reader in another
language whose only msgpack is the ordinary kind. Otherwise "refused" arrives
with no detail, which is the worst possible diagnostic for a snapshot that took
an hour of work to produce.

**Two fields are implemented ahead of their content, deliberately.** The
permanents fingerprint hashes the permanents set, which is empty until the next
milestone builds it; the capability set is a string because the capability system
is M7 and inventing its shape now would be guessing. Both travel in the header
and are compared today, so the field, the comparison and the refusal are all
exercised before the content arrives — and when the permanents table is built,
every snapshot taken before it is correctly refused rather than silently
mismatched.

**The host stamp is asymmetric, and that is the point.** An unstamped snapshot
restores anywhere, because a process moving its own state has nothing to check
against. A stamped one must match. And a host that supplies a stamp refuses a
snapshot without one — otherwise stamping would be advisory, and an unstamped
snapshot from anywhere would pass the very check the host added to stop that.

**Refusals are returned, not raised.** 10.10 calls restore untrusted input, so the
first thing that touches those bytes must be unable to take the host down — and a
host needs a status it can report, not an error it has to catch to learn what it
already needed to know. Bytes that are not msgpack at all are the common case for
a file that is not a snapshot, and the codec raises on those; the check runs the
decode under protection.

**Capability enforcement on restore is not optional.** A snapshot taken under
privileged capabilities must not be restorable into a context that should not have
them. For a system with a privilege hierarchy this check is doing security work,
and it is the check that makes the hierarchy structural rather than conventional.

### 10.7 Preconditions on the program

Checked at hibernate time, refused if violated:

1. No C frames without continuations below the call site (10.2). Implemented as
   `diluvium_shim_capturable`, which also refuses a thread that is not suspended
   and a thread parked inside a debug hook — see below.
2. No live host resources in the reachable graph. No open sockets, file handles, or
   sessions. Because hibernation is self-initiated, the program is in a position to
   release them first. Verify by rejecting any userdata lacking a `__persist` hook.
3. No light userdata **that the runtime has not named**. The blanket version of
   this was too absolute, and the runtime's own wait protocol proved it:
   `queue.wait` pushes a light-userdata sentinel onto a thread's stack before
   yielding, so a parked agent -- the thing hibernation exists for -- always has
   one. A light userdata the runtime owns can be a permanent like anything else,
   and the name resolves to the same sentinel in the new process. Only an unnamed
   one is impossible, which is why the permanents check runs before this refusal
   rather than after it.
4. Single thread.

Two more the draft did not list, both found by writing the check:

5. **The thread must actually be suspended.** A running thread has state on the C
   stack that nothing has written down; a thread that resumed another (`normal`
   status) is in the same position one level up, and that is the case a swarm hits
   for real — a supervisor parked inside a resume of a child. Both look identical
   from the internals: status `LUA_OK` with a non-empty `CallInfo` chain. Which
   means the frame count is part of the test and not a shortcut around it.

   One case is deliberately *not* refused: a thread with an empty call chain. The
   main thread sitting at the C host boundary and a thread that has never started
   are indistinguishable — both are empty — so refusing self-capture belongs to the
   snapshot layer, which knows which thread it was called on. Capturing an empty
   thread is meaningless but harmless.

6. **Not inside a debug hook.** A C hook can park a thread: `lua_yieldk` from a
   hook returns rather than throwing, and `luaG_traceexec` then sets
   `CIST_HOOKYIELD` and throws. The result is genuinely suspended, so the
   suspended test passes it, and it is refused on its own grounds — the thread is
   parked mid-instruction with `savedpc` past the instruction it has not executed.
   Catching this needs the frame walk rather than an `allowhook` test, because
   `luaD_hook` restores `allowhook` and clears `CIST_HOOKED` before the yield
   propagates. A *Lua* hook cannot reach this state at all: `ldblib.c`'s `hookf`
   uses `lua_call`, so the yield errors instead of parking.

### 10.8 Handle re-resolution

Queue handles do not survive. On restore, re-declare queues by name from the header
and re-resolve handles. Any handle value stored in program state is stale and must
not be silently reused. If the analyzer can detect a handle stored in a table that
reaches the snapshot, warn.

### 10.9 Secure functions in snapshots

`~function` exists so a function's constants and variable names cannot be read at
rest. A snapshot walks the closure graph and writes Protos, so a naive
implementation is a straightforward bypass of that feature. Section 10.6's
capability check does not cover this: it gates *restore*, not *confidentiality*.

Two rules, and both are needed because they cover different things.

**1. Route Proto encoding through the real dump path.** Do not write a parallel
Proto encoder. Section 10.5 already hashes stripped dumps, so using the actual dump
machinery means `taintSecureStrings` and the per-string scramble flag come along for
free. A parallel encoder is precisely how this class of bug happens a second time —
the string-taint fix exists because scrambling decided at one site did not hold at
another.

Rule 1 is satisfied structurally rather than by discipline: prototype encoding
*is* `lua_dump`, so there is no parallel encoder that could drift. The test that
this actually holds is the one from `test_secure_dump.lua` pointed at a snapshot
instead of a dump — put a distinctive literal inside a `~function`, snapshot it,
and search the whole stream for the literal. It is not there, and the function
still returns it after a round trip, which matters because scrambling achieved by
breaking the function would pass the first half alone.

**2. State on the record that a snapshot is as sensitive as a memory dump.**
Scrambling covers Protos, not live data. A secure function's constants may be
hidden while its runtime values, its upvalues, and its queue contents sit in the
clear in the same stream. Rule 1 alone would give a false assurance, and a
guarantee believed to be broader than it is repeats the mistake this project has
already documented once.

**Determinism requirement on the obfuscation.** Content-addressed Protos require
identical source to produce identical bytes, so the scramble must be deterministic.
A per-dump nonce is therefore refused: it would trade a small obfuscation gain for
the entire snapshot dedup story. A position-varying but deterministic keystream is
compatible and does not affect this section. Whoever changes the obfuscation must
re-read this paragraph first.

### 10.10 Restore is untrusted input: three separate jobs

`dv_restore` reconstructs a `CallInfo` chain, stack offsets, pc offsets, upvalue
slot indices, and `tbclist` directly into `lua_State` internals. Inline Protos at
least pass through the loader and reach `lverify.c`; the stack reconstruction
reaches nothing. The measured baseline for why this matters is in the roadmap: one
flipped byte in a dump crashed the release interpreter about 7% of the time.

Three jobs, deliberately named separately so that deferring one does not silently
defer the others.

**Validate — required in M6, justified as robustness.** A malformed snapshot that
crashes an interpreter inside a swarm gives a crash with no provenance and no
repro, which is expensive regardless of whether anyone is attacking. Minimum
scope: pc offsets within the code array; base and top within stack bounds and
monotonic across frames; upvalue slot indices in range; `tbclist` indices in range
and ordered; `callstatus` a valid flag combination; frame functions actually
functions; vararg counts consistent; backreference indices in range with no forward
references; type tags valid. A structure-aware fuzzer is an acceptance criterion,
not follow-up; `script/fuzz_struct.py` is the template.

**Authenticate — deferred, behind a checkable precondition.** The condition is not
"no sensitive data", it is **"snapshots never leave the host."** While
`wake_on_message` pulls from a local cache on the same machine, the threat model is
file corruption and authentication buys little. The moment a spawn target is
another node, authentication becomes required. So the precondition is recorded
here rather than omitted:

> Snapshots are not authenticated. Restore must only ever be fed bytes produced on
> the same host. Crossing that boundary requires authentication first.

And it is made **checkable rather than merely documented**: the swarm layer stamps
each snapshot with a host-instance identifier and refuses foreign ones. That is
roughly twenty lines, it is not authentication and must not be described as such,
and it converts the precondition from prose into something that fails loudly when
someone crosses it.

**Capability-check — required, per 10.6.** Authentication does not cover this. A
legitimately produced, correctly stamped snapshot from a privileged instance is
exactly the thing 10.6 refuses to restore into a lower-privileged context.

The claim this work supports is **"a malformed snapshot is refused."** Never
"snapshots are safe."

### 10.11 Effort note

This is a bounded piece of work because of what has been excluded. The stack walk
reads fields rather than restructuring the call mechanism, and the whole category
of problems around arbitrary threads, hooks, and C continuations is excluded by
construction rather than handled. Expect a few hundred lines in the accessor shim
plus the serializer, not a general-purpose persistence engine.

---

## 11. Part 7: C ABI

### 11.1 Instance ABI principles

- Bytes in, bytes out. No Lua types cross the ABI.
- Narrow and stable. Every host binding is thin because this is small.
- Symbol prefix `dv_`. Settled.

### 11.2 Instance ABI surface

```c
/* version */
uint32_t     dv_abi_version(void);

/* lifecycle */
dv_instance* dv_new(const dv_config* cfg);
void         dv_free(dv_instance* inst);
dv_status    dv_load(dv_instance* inst, const uint8_t* code, size_t len,
                          const char* name);
const char*  dv_last_error(dv_instance* inst);
const char*  dv_status_name(dv_status s);

/* queues */
dv_queue_id  dv_queue_lookup(dv_instance* inst, const char* name);
dv_status    dv_queue_state(dv_instance* inst, dv_queue_id id, dv_queue_info* out);
dv_status    dv_queue_push(dv_instance* inst, dv_queue_id id,
                           const uint8_t* msgpack, size_t len);
dv_status    dv_queue_pop(dv_instance* inst, dv_queue_id id,
                          uint8_t* buf, size_t cap, size_t* out_len);
dv_status    dv_queue_peek(dv_instance* inst, dv_queue_id id,
                           const uint8_t** ptr, size_t* out_len);
void         dv_queue_release(dv_instance* inst, dv_queue_id id);

/* scheduling */
dv_status    dv_run(dv_instance* inst, dv_waitset* out_waitset);
dv_status    dv_resume(dv_instance* inst, dv_queue_id fired);
dv_status    dv_waitset_get(dv_instance* inst, dv_waitset* out);

/* snapshots */
dv_status    dv_snapshot(dv_instance* inst, const char* host,
                         uint8_t* out, size_t cap, size_t* len);
dv_status    dv_restore(dv_instance* inst, const char* host,
                         const uint8_t* snap, size_t len);
dv_status    dv_register_code(dv_instance* inst, const uint8_t* code,
                         size_t len, const char* name);
```

`dv_snapshot` writes into a caller-supplied buffer rather than returning an
allocation, so it matches every other call on this surface and a wasm host needs no
free. Pass `out == NULL` for a size enquiry. The `host` argument is 10.10's identity
stamp, and it appears on both calls because the check is symmetric: an unstamped
snapshot restores anywhere, a stamped one only under the same string, and a host
that supplies one refuses a snapshot without it.

`dv_restore` needs a *fresh* instance and says so; afterwards the instance is parked
exactly as the snapshot's was, so the next call is `dv_waitset_get` and then
`dv_resume` — not `dv_run`, because the program is continuing rather than starting.

`dv_register_code` is 10.5 from a host's side: register the chunk a swarm shares and
every agent's snapshot carries a hash in place of its code. A host that registers
nothing gets self-contained snapshots, which is the right default.

```c
/* notification */
void         dv_set_notify(dv_instance* inst,
                           void (*cb)(void* ud, dv_queue_id id), void* ud);
```

`dv_queue_peek` returns a borrowed pointer so hosts can avoid a copy on the hot
path; `dv_queue_release` pops it. `dv_queue_pop` remains for simple bindings.

**The peek contract, stated tightly enough to be implementable.** The pointer aims
into a Lua string owned by the guest heap, so "valid until the next call" is not
sufficient on its own — the implementation must anchor the value in the registry
between `peek` and `release`, and the header must forbid `dv_run` and `dv_resume`
in between. Any Lua execution may collect an unanchored string.

`dv_set_notify` avoids polling when the guest pushes to an exported queue. The
callback fires synchronously on the calling thread during `dv_run` and must not
re-enter the ABI.

### 11.3 Status codes

`DV_OK`, `DV_QUEUE_FULL`, `DV_QUEUE_DISABLED`, `DV_QUEUE_UNKNOWN`,
`DV_QUEUE_EMPTY`, `DV_QUEUE_GONE`, `DV_IDLE`, `DV_DONE`, `DV_ERROR`,
`DV_ABI_MISMATCH`, `DV_SNAPSHOT_MISMATCH`, `DV_BUSY`, `DV_BUFFER_TOO_SMALL`.

`dv_run` returns `DV_IDLE` with a populated wait-set when the guest parks,
`DV_DONE` on completion, `DV_ERROR` on a Lua error.

**`dv_waitset_get` was missing from the first draft, and the gap is worth
recording.** `dv_resume` can also return `DV_IDLE` — a program looping on
`queue.wait` parks again immediately, which is the normal shape rather than an
edge case — and its signature has nowhere to put a wait-set. Without this, a
host would have to guess, and a zeroed struct reads as "parked on no queues at
all", which is the kind of wrong that looks like it works. Found while writing
the Rust wrapper, which is the argument for writing a binding in the same
milestone as the ABI rather than after it.

`dv_load` also takes a `name`, since without one every traceback says
`(dv_load)` and a host with several programs cannot tell them apart.
`dv_last_error` returns the message and traceback for the last `DV_ERROR`;
`dv_status_name` gives a status a printable name, for logs and for binding error
types.

`DV_QUEUE_DROPPED` joins 11.3's list: `on_full = "drop_oldest"` accepted the
message *and* evicted one, which is neither `DV_OK` nor a refusal.

### 11.4 Threading contract

**One instance, one thread. The host must not call the ABI for a given instance
from more than one thread concurrently.** State this in the header, the docs, and
every binding. Each binding enforces it in its own idiom: the Rust wrapper is
`!Sync`, the Python wrapper documents it, the JS wrapper is single-threaded by
construction.

### 11.5 Swarm layer ABI

The multi-instance runtime layer is a separate library named **swarm**
(`libdiluvium-swarm`), with symbol prefix `dvs_` (confirmed; open question 1 is
closed). It owns the six items listed in 9.1.2 and requires a host-supplied
vtable:

```c
typedef struct {
  void* (*create)(void* ud, const dv_spawn_req* req);
  void  (*destroy)(void* ud, void* ctx);
  int   (*drive)(void* ud, void* ctx);
} dv_host_vtable;
```

Everything above that vtable is portable C. Everything below is the host's.

### 11.6 Version negotiation

Every binding calls `dv_abi_version()` at init and refuses on mismatch. The version
covers the ABI surface, the ext code registry, and the snapshot format. A wrapper
newer than the library it was handed must fail loudly rather than misdecode.

---

## 12. Part 8: Host bindings and packaging

No host ever writes a shim. Every target ships prebuilt.

### 12.1 Artifact naming

One archive per target triple, published to the existing release mirror under the
stable `/release/latest/` paths:

```
diluvium-<version>-<target-triple>.{a,so,dylib,dll,wasm}
diluvium-swarm-<version>-<target-triple>.{a,so,dylib,dll,wasm}
diluvium-<version>-headers.tar.gz
```

The npm, crates, and PyPI packaging scripts all fetch from the mirror. No package
gets its own retrieval logic.

### 12.2 Rust (crates.io)

A `-sys` crate carrying prebuilt static libraries per triple with a
build-from-source fallback, plus a safe wrapper.

The wrapper presents queues in `tokio::sync::mpsc` shape so `select!` works with no
adaptation. Use `rmp-serde` so host code gets `Deserialize` on messages. Rust is one
of the two demo targets and gets the most polish.

**Field names cross the boundary, and every binding must agree.** `rmp-serde`'s
default `to_vec` encodes a struct as an *array of field values*, so a Rust
`Order { id, item, qty }` arrives in Lua as `{1, "widget", 2}` — coupling every
guest to the declaration order of a Rust struct, invisibly, until somebody moves
a field. The wrapper uses `to_vec_named`. Found by a guest reading `order.qty`
as nil, which is the cheapest possible way to find it and the reason the binding
belongs in the same milestone as the ABI.

The same rule binds the JS and Python wrappers: a struct, object or dataclass
crosses as a msgpack **map**, keyed by name.

### 12.3 JavaScript (npm)

The `.wasm` plus a TypeScript wrapper. One package serving browser and Node; the
browser build uses the existing embedded WASI shim.

```js
const inst = await Diluvium.load(bytecode);
inst.queue('outbox').onMessage(msg => { /* decoded object */ });
inst.queue('inbox').push({ hello: 'world' });
```

Bundle a small msgpack encoder rather than taking a dependency. A few KB of codec
keeps the size story intact and avoids a transitive-dependency argument on a
package whose pitch is smallness.

### 12.4 Python (PyPI)

cffi against the same ABI. Wheels for manylinux, musllinux, macOS universal2, and
win_amd64.

### 12.5 C and C++

Header plus per-triple archives in the release mirror. This is also the honest
answer for Go, C#, Java, and anything else: document the ABI, do not promise
bindings that will not be maintained.

### 12.6 Portability demo

Build one demo as an acceptance artifact: the same unmodified Diluvium bytecode
running in a browser tab exchanging messages with JavaScript, and under tokio
exchanging messages with a socket, with nothing different but the host wrapper.

Short enough to read on one screen. This is the concrete form of the
hostcall-portability claim.

---

## 13. Milestones

Each independently mergeable. Report stripped size deltas at each.

**M0: the coroutine-hosted call driver** — done.
`src/dtask.c` and `dtask.h`: `diluvium_task_call` with `lua_pcall`'s stack
contract, `lua_pcallk` plus continuation inside a resumed C body, error re-raised
from the continuation, `lua_checkstack` both directions, registry-anchored fresh
thread per call, loud `LUA_YIELD` branch, SIGINT hook point. Nothing existing
converted, so the conformance suite is untouched.

Accepted on: `coroutine.isyieldable()` true inside the driver and the calling
state still non-yieldable; errors reporting with a traceback naming the failing
frame; 64 arguments and 64 results crossing intact; the caller's stack left as
`lua_pcall` leaves it; a top-level yield reported rather than swallowed. Tests in
`test/dtask_check.c`, run by `make dtask_check`. Each of the three mitigations
that can fail silently was verified by removing it and watching a named test fail
— re-raise removed gives 3 failures, plain `lua_pcall` gives 5, dropped
`lua_checkstack` aborts under `api_check`.

Sequenced first because the entry shape is the part that is painful to retrofit,
and separated from M3 because "what does the prompt do while parked" is a UX
question that deserves a considered answer rather than a same-day one.

**M0b: `--task` on the CLI** — done.
`collectargs` learns one long option; `docall` branches to the driver; `globalL`
follows the running thread through the driver's hook. Refused with `-i`. Error
output is byte-identical between modes, checked by hand against the same failing
script.

Accepted on: `coroutine.isyieldable()` false by default and true under `--task`;
`--task -i` refused; and `test/interrupt_check.sh` asserting Ctrl-C interrupts a
runaway loop in **both** modes. That last one closes the gap M0 left open --
removing the `diluvium_task_sethook` call leaves the `--task` case running until
the harness kills it, which is exactly the silent regression the hook exists to
prevent, and it now has a test that catches it.

**M1: msgpack codec** — done.
`src/dmsgpack.c` plus `src/dlibs.c` for registration, since `linit.c` cannot
host a new library without editing a core file that is not allowlisted and
changing the meaning of every `LUA_<lib>K` mask.

Accepted on `test/test_msgpack.lua`, 139 checks: round-trip corpus; integers
surviving as integers including `math.mininteger`; both sides of every integer
width cutoff asserted **as wire bytes** rather than only round-tripped, since a
codec wrong in both directions round-trips perfectly and interoperates with
nothing; floats always float64; the empty table as a map; forced shapes; a
cyclic table raising with "cycle" in the message; a non-encodable value naming
its type and its key path (`a.b[1]`); the decode offset chain; every reserved
ext code failing by name, with 0x01 saying decQuad specifically; ext 0x02
decoding opaquely with no resolver installed; and every single byte plus every
truncation of a valid encoding refused without a crash.

Stripped size 13.0 KB of text at `-O3` on linux-x86_64, against a 25 KB budget.
Not yet measured on musl or wasm, which 3.2 asks for.

**M2: queue subsystem, local only** — done.
`src/dqueue.c`. Declare, lookup, destroy, push, pop, enable, disable, len,
capacity, state, info. `on_full` of `reject`, `drop_oldest` and `drop_newest`;
`"block"` is **refused at declare time** with a message saying it needs the
wait-set protocol, rather than silently treated as `reject` — which would hand a
program the opposite of the backpressure it asked for and look like it worked.

State lives in registry-anchored Lua tables rather than C structs. The C version
is faster per operation and adds a place to leak on each of destroy, error and
state close; since 6.5 already accepts a msgpack encode per push, table accesses
beside it are noise. The benchmark will say if that stops being true.

Accepted on `test/test_queue.lua`, 287 checks: one case per row of 6.4; that a
full, disabled or empty queue never raises; that a pushed table cannot be
mutated by the sender afterwards, nested tables included; that a bounded queue
holds FIFO order across twenty ring wraps; that `pop` returns rather than parking
inside a coroutine where a yield would be legal; that a stale handle raises; and
the `proc_10ms`/`proc_50ms`/`proc_500ms` chain end to end, polled, since
`queue.wait` is M3.

Stripped size 5.9 KB of text at `-O3` on linux-x86_64 against a 15 KB budget.
msgpack is now 13.8 KB against 25 KB. Neither measured on musl or wasm yet.

One finding worth keeping: the chain test first reported `sum(1..40)` where
`sum(1..50)` was expected. The queue was right and the test had under-sized its
middle stage, which rejected the last two of ten pushes. The under-sized case is
now pinned deliberately as its own test, since "a stage that cannot keep up does
not grow, and the sender is told which messages did not make it" is the other
half of *bounded, always*.

**M3: yield-aware layer** — done, apart from the REPL half.
`queue.wait`, `on_full = "block"`, the wait-set protocol, the yieldability guard
with 8.4's diagnostics, and the drive loop in `docall`'s `--task` branch — which
is the point at which the CLI stopped being an entry and became a host.

Accepted on `test/test_wait.lua`, 58 checks, run under `--task`: a satisfiable
wait answering without parking; a wait picking whichever listed queue is ready,
with readiness beating a closed sibling; a zero timeout polling; a finite timeout
elapsing with wall clock actually passing, since the host owns the clock; a
disabled queue delivering what it holds and only then reporting `"closed"`; a
destroyed queue firing `"closed"` rather than raising; **a wait inside `pcall`
and `xpcall` succeeding**; each genuinely non-yieldable context refusing by name;
and a VM-dispatched metamethod parking happily, which is the half that keeps the
previous item honest.

Deadlock is reported rather than hung. Under a single-threaded CLI over local
queues, nothing can write while the program is parked, so an indefinite wait can
never be satisfied — the host says so and the program exits non-zero. That is the
CLI's judgement and not the runtime's: 8.3 puts the clock, and therefore this
call, on the host side. Checked in subprocesses, since the failure mode being
tested is "hangs forever".

`queue.wait` takes at most 32 handles. A program waiting on more is describing a
routing problem, and 7.4 says routing belongs in a program.

Still open, deliberately: `repl_eval`, the parked-prompt question and the Lab
adjustment. Nothing about them is blocked — `wasi_check.sh` drives the standalone
binary, so `repl_eval` is not conformance-tested and can be converted whenever
Lab is ready to answer what a prompt shows while a program waits.

Sizes at `-O3` on linux-x86_64: queues 10.4 KB against 15 KB, msgpack 13.8 KB
against 25 KB, driver 1.7 KB.

**M4: instance C ABI plus reference host** — done.
`src/dv.h` and `src/dv.c`, plus `bindings/rust/` (a `diluvium-sys` crate that
builds the amalgamation from source, and a safe `diluvium` wrapper).

`dv_run` and `dv_resume` are steps, not loops. The CLI's driver loops because a
CLI can afford to block; a host with an event loop of its own cannot, so the ABI
returns the moment the program parks. Both drive the same body via
`diluvium_task_pushbody`, so the subtleties about continuations and
non-yieldable protected calls live in one file rather than two.

Accepted on `test/dv_check.c` (73 checks, written against `dv.h` alone — which
is also a check that the header suffices on its own) and 16 Rust tests plus a
doctest and an example host. Between them: the version refused at `dv_new`;
errors crossing with a traceback that names the failing frame; a guest queue
invisible until the program declares it; every row of 6.4 from the host side;
`DV_BUFFER_TOO_SMALL` leaving the message in place so a host can size a buffer
and retry; the peek/release zero-copy path with its registry anchor; export
notifications firing for exported queues and not for private ones; parking,
resuming, and a park for *space* reporting `for_write` so a host knows to drain
rather than feed; `DV_BUSY` when a parked program is run instead of resumed; and
a host answering a 5-second timeout in microseconds, because it owns the clock.

The Rust wrapper is `Send` but not `Sync`, which is 11.4 stated in the type
system rather than in a comment.

Sizes at `-O3` on linux-x86_64: ABI **5.5 KB** against a 10 KB budget.

Worth watching: queues are now **13.6 KB** against a 15 KB budget, having grown
from 10.4 with the byte-level host paths. M5 adds endpoints to the same file, so
the budget will bind there rather than later. Splitting the host-facing half of
`dqueue.c` into its own translation unit is the obvious move if it does, and it
costs nothing to defer until the number says so. msgpack is 13.5 KB against 25;
the driver 1.7 KB.

**M5: endpoints and delivery model** — done.
`src/dendpoint.c`, its own file both because it is a different concern from a
buffer and because `dqueue.c` had reached its budget.

An endpoint is an ordinary bounded queue with one extra property: its far end can
close, and once it has, a push answers `"gone"` immediately. **Liveness is a flag
the host maintains, not a callback the runtime makes** — the host is the only
thing that knows when a far end died, and a call in the push path would put
whatever it does inside the cheapest operation in the system. That is what keeps
7.5's requirement literal: accepting is O(1) and never depends on the destination
being reachable.

Ext 0x02 gets the resolver seam 4.2 promised, and the seam is real rather than
notional: with the endpoint library loaded a reference in a message arrives as an
opaque reference object; without it the same bytes decode to an ext value, so an
embedder linking only the codec sees no error. Both halves are tested, in
different places — the resolved path in `test_msgpack.lua`, the opaque fallback in
`bindings/js`, whose codec has no resolver at all.

A reference cannot be forged. There is no constructor anywhere, its metatable is
hidden, and a lookalike is refused by name — so 7.3's "never constructed by the
guest" is structural rather than a convention.

Accepted on: a push to a closed endpoint answering `"gone"` immediately, from both
the guest and the host side, without raising; `endpoint.status` reporting live
then gone; a refused bind raising, which is deliberately *different* from a push
to a dead endpoint — the program asked for one specific destination and did not
get it; and **a relay forwarding between three instances with no runtime support
for routing at all.** In that test nothing in the runtime knows the other
instances exist: the sender pushes to an endpoint, the host moves bytes, the relay
is an ordinary program holding two handles, and a hop count proves each message
really went through it. That is 7.4 demonstrated instead of asserted.

`dv_endpoint_allow` is new, and was found by writing the wasmtime binding: the
bind handler was a C function pointer, and in wasm a function pointer is an index
into the module's function table, so there is no way to hand one in from outside.
A host can now pre-authorise a reference instead, mapping bytes to a token up
front. It is the better shape for every host — a host almost always knows what its
own references mean — and the callback remains for one that wants to resolve
lazily. Registered references are consulted first, so the two compose.

Not done: `"gone"` for a non-resident instance with `wake_on_message`, which is
9.5 and belongs to the swarm layer.

**M6: hibernate** — done, except userdata `__persist`, which is refused by name
and whose absence is explained in 10.3.
Accessor shim, precondition checks, value graph with backreferences, closures and
upvalue identity, permanents table, content-addressed Protos through the real dump
path, snapshot header, validation pass and structure-aware fuzzer, host-identity
stamp, `dv_snapshot` and `dv_restore`.
Accept when: an agent parked on `queue.wait` round-trips and resumes; shared
upvalues remain shared; a mismatched runtime identity refuses cleanly; a hibernate
attempted below a continuation-less C frame refuses with a named error; a snapshot
taken with a live `defer` in scope round-trips; a foreign host stamp is refused;
the fuzzer reports no crashes. **All met.**

*Done so far.* `src/dshim.c` and `src/dshim.h` — 1.5 KB of the size budget, and
the only file in the tree that reads Lua's internal headers, which is the whole
point of 3.1's second clause. It reports frames, stack slots, upvalue open/closed
state, proto identity, the secure-function flag, and capturability. Nothing but
plain data crosses the interface, so `dsnap.c` can be written without internal
headers even though it is the thing that needs the information.

Every internal-structure assertion the draft made was checked at source level
before a line was written, per 17's procedure, and all held: `CallInfo`'s field
names and union layout, `CIST_C` at bit 15, `upisopen`, `StkIdRel` being a union,
`tbclist`'s emptiness convention, `lua_closethread`'s signature, and `Proto`'s
`is_encrypted`. What did *not* hold were two things about the design's own logic
rather than the tree — see 10.2 and 10.7 — and both were caught by the tests
rather than by re-reading.

`test/dshim_check.c`, 82 checks. Two things it defends, which fail differently: an
upstream *rename* breaks the compile, loudly, and that is what confining the
dependency buys; an upstream change of *meaning* under the same field names would
not, so the checks assert relations that only hold if the reads mean what they
claim — frame counts against known call depths, `func_index` against the function
actually in that slot, an open upvalue's slot against the live value in it. The
capturability half runs nine different suspended shapes (bare yield, `pcall`,
`xpcall`, `__index`, `__add`, generic `for`, nested coroutine, pending `<close>`)
and four uncapturable ones.

Verified by mutation, as with M2 and M3: seven deliberate breakages, each one
turning a named check red, listed at the head of the test file. Two of them
initially turned *nothing* red, which is the reason 10.2's factored predicate and
10.7's items 5 and 6 exist — the first pass had a check that refused every
ordinary coroutine and a check that would have accepted a running thread.

Also fixed in passing: `src/makefile`'s object list, which had never been updated
past M0, so `make build_platform` had been failing to link since M1. The
amalgamation path the test suite uses was unaffected, which is why it went
unnoticed.

*Then the value graph and the header.* `src/dhash.c` (SHA-256), the snapshot mode
in `src/dmsgpack.c`, and `src/dsnap.c` (fingerprint and header). 4.5 KB on top of
the shim; the codec came out 0.2 KB *smaller* than the figure recorded at M5,
because the path-recording changes that made a 150-deep cap possible also removed
work from the common path.

SHA-256 rather than a fast hash because 10.5 makes a Proto's hash a security
boundary: a collision is a snapshot naming one function and being handed another.
`test/dhash_check.c` checks it against the published FIPS 180-4 vectors and
against Python's hashlib for every length from 0 to 69 — one shot, a byte at a
time, and split at every point — because "it round-trips" proves nothing about a
hash. 252 checks. Two of the first expectations written were digests that had not
actually been computed; the four published vectors passed and those two did not,
which is the right way round for that mistake to happen.

The graph work is in the codec rather than beside it, because identity tracking is
inseparable from the table walk. `msgpack.encode` still refuses a cycle, which is
correct there — a queue message with a cycle has no agreed meaning for whoever
receives it — so snapshot mode is a separate entry point rather than a flag.

`test/dsnap_check.c`, 71 checks, and the shape of them is the point: a graph
serializer fails *quietly*. A round trip that silently unshares two references to
one table passes any test written as "encode, decode, compare contents". So every
case asserts an identity relation — that two paths reach the same table, that a
cycle closes on itself, that one class table is still the metatable of all three
instances — rather than comparing values.

Verified by mutation, five breakages each turning a named case red. One of them
originally produced a *hang* rather than a failure, because the metatable worklist
re-queues forever if the identity map is inconsistent; the encoder now checks that
invariant, and the same mutation reports eight named failures instead of nothing.
That guard exists because of what the mutation run said, not because it was
foreseen.

Two additions the milestone list did not mention, both found by needing them:
`diluvium_queue_next` (nothing could enumerate queues, and 10.6 puts their names
in the header) and `diluvium_msgpack_decode_n` (the C decode entry point did not
report bytes consumed, and a header followed by a payload is exactly the case that
needs it).

*Then permanents, prototypes and closures.* 9.3 KB across `dhash.c` and `dsnap.c`,
plus 2.7 KB of snapshot mode in the codec. `dsnap_check.c` is 112 checks.

Four assumptions were probed against the tree before any of it was written, per
17's procedure, and all four held: a *nested* closure can be dumped and reloaded
(95 bytes stripped); `lua_load` forces upvalue 1 to the globals table, so every
upvalue must be overwritten from the graph rather than trusted; `lua_upvaluejoin`
genuinely restores sharing, verified by mutating through one closure and reading
through the other; and two closures of one prototype dump identically, which is
what content addressing needs.

The fixup section from the previous slice was generalised rather than joined by a
third mechanism: metatables and upvalues are both things that are not part of their
owner's contents, so they are both tagged fixups. `UPJOIN` is the whole of 10.3's
upvalue identity.

Permanents cover `_G` and the library tables, not only C functions, and 10.4 has
been corrected to say so — a snapshot of one closure calling one global would
otherwise drag the entire global environment in. Measured: such a snapshot is 41
bytes.

Verified by mutation, seven breakages. **Two initially turned nothing red**, and
both gaps were real:

Giving ext 0x07 a position on decode broke nothing, because noticing needs a
backreference that *crosses* a permanent — and then it silently resolves to the
wrong object rather than failing. `a_permanent_does_not_shift_positions` is that
case.

Removing the inlined-prototype hash verification broke nothing, because the
tampering case flipped a byte of bytecode and the loader caught it. Worse, both
tampering cases turned out to be testing the *reference* path, because an earlier
test had registered the same prototype — one of them passing on a "not present in
this runtime" refusal unrelated to its own claim. Both now use distinct function
bodies and assert the record inlines its prototype before tampering with it.

*Then thread capture, which is the milestone's point.* `dsnap_check.c` is 140
checks and the acceptance criterion 14 names now passes: an agent parked on
`queue.wait`, through dtask.c's real driver, round-trips, wakes with a message
pushed into its queue *by name*, and finishes. So does the `defer` case 14 calls
the highest-coverage single test available. 20.1 KB of the 30 KB budget.

Four assumptions were probed against the tree first and all held; see the closure
entry above. What did not hold were four things about the *design*, each corrected
in 10.3, 10.4 or 10.7 and each found by a failing test rather than by reading:

Live slots and reserved capacity are different numbers. A frame's `ci->top` sits
above the live top by its unused registers, so validating against the live count
refused every real thread.

`nextraargs` is uninitialised on a non-vararg frame — `luaT_adjustvarargs` is the
only writer — so capturing it unconditionally captured garbage from a recycled
`CallInfo`.

To-be-closed slots cannot be marked with their thread. A `defer` object's
`__close` lives on a metatable that arrives as a later fixup, so marking during
the thread's own fixup found a table with no metatable. That is what the finish
pass is for.

Light userdata cannot be refused outright. `queue.wait` puts a sentinel on the
stack of every parked agent, so 10.7's blanket refusal would have refused exactly
the shape hibernation exists for. Named light userdata is now allowed and the
permanents check runs first.

Verified by mutation, five breakages. `nyield` initially broke nothing: every
thread case resumed to completion, and none asked a *restored* thread what it was
waiting for — which is the first thing a host does.
`a_restored_agent_reports_its_waitset` is that case, and it is the real host
workflow rather than a contrivance. Two mutations (a wrong pc, a dropped
continuation) abort rather than reporting a named failure; the property is covered
in that nothing passes, but the diagnostic is a crash, and validation cannot
distinguish "pc in range but wrong" from a correct one.

Two bugs the amalgamation hid, both caught by `make build_platform` and not by the
suite: `LUAI_MAXSTACK` is defined in `ldo.c` rather than any header (the debug
build sees it only because `ltests.h` redefines it), and `luaC_objbarrier` needed
`lgc.h`. The platform build belongs in the sweep for that reason.

*Then the ABI and the fuzzer.* `dv_snapshot`, `dv_restore` and
`dv_register_code`; `test/dv_check.c` is 128 checks and `script/fuzz_snapshot.py`
runs 430 mutants with no crashes. 22.7 KB of the 30 KB budget.

A whole-instance snapshot carries the parked thread *and the queue subsystem's
state*, which turned out to simplify 10.8 rather than implement it: the state is a
plain table of numbers, strings and tables, so restoring it verbatim brings the
queues, their contents and the program's own handles back unchanged. Handles do not
go stale after all. 10.8's re-declare-by-name step is still needed for the case it
was written for — moving one program's state into an instance that already has
queues — and `diluvium_queue_setstate` refuses that rather than merging two
numbering spaces.

Three bugs of one kind, and the kind is the lesson: **naming must not depend on
history.** The permanents fingerprint travels in the header, so a set that grew
lazily made a snapshot from a started instance unreadable by a fresh one. The
driver's continuation, registered where it was installed, was missing in a process
that had only ever *loaded* a snapshot. Both were registered eagerly instead. Two
instances in one process hid the second one completely, because the continuation
table is process-wide; the fuzzer, which loads in a separate process, found it on
its first run.

Encoding was also *registering* the prototypes it inlined, so a second snapshot of
the same program referenced them, silently stopped being self-contained, and could
not be restored anywhere else. Found because a size enquiry disagreed with the
snapshot that followed it. Prototype dedup is now per stream, on both sides.

The fuzzer's 13 crashes split into two kinds, which is why 10.10 now has two
layers. Three were real validation gaps — `nyield` unchecked, and a frame's `is_c`
and `nresults` allowed to disagree with the `callstatus` the restore actually
writes, so a C frame could claim to be Lua and have the VM read `u.l.savedpc` out
of the wrong union member. The rest are not checkable at all: whether a pc is the
*right* offset inside its own prototype is not a question any local check can
answer. So the header now carries a SHA-256 of the payload. Measured: 4 crashes
with neither layer, 3 with the field checks, 0 with both. The digest is integrity
and not authentication — it catches corruption, and the field checks are what
stands between a deliberately rewritten snapshot and the interpreter.

Not done: userdata `__persist` (ext 0x05). Refused by name, with a message saying
so, which is what 10.7 item 2 specifies — and 10.3 records why the obvious design
does not work, because that is worth more than a half-implementation that breaks
when a userdata is referenced twice. The header's capability field is still
compared against an empty set until M7.

**M7: swarm layer**
Instance table, `system/lifecycle` and `system/events`, attenuation, budgets,
orphan policy, rate limits, snapshot cache with `wake_on_message`.
Accept when: a supervisor spawns and restarts children; a child cannot be granted
capability the supervisor lacks; a message to a swapped-out instance wakes it and
arrives in order ahead of live pushes.

**M4b: Python and JavaScript bindings** — Python done, JS partly.
`bindings/python` (cffi in API mode, 17 tests) and `bindings/js` (a bundled
msgpack codec, 15 tests). `bindings/README.md` records the five wrapper decisions
every binding must copy rather than rediscover, since each was found the hard way
in the first one.

`dv_layout` is new and exists for wasm: a binding reaching the ABI through
WebAssembly has no `offsetof`, and wasm32 is ILP32 — so every struct holding a
pointer or a `size_t` is laid out differently there than on the LP64 machine a
developer would measure it on. That is a bug no local test can catch, so the
runtime reports its own layout instead.

The JS codec is checked against vectors generated **by** `src/dmsgpack.c`, so
cross-implementation agreement on the wire format is a measured fact rather than
two readings of the same spec. The JS *wasm wrapper* is unverified: building
`diluvium.wasm` needs the wasi-sdk in a container, which was unavailable, so CI's
`js-binding` job is the first place it runs. Stated in `bindings/README.md`
rather than implied.

**M8: packaging**
Rust and JS first, then Python wheels and the header archive.
Accept when: the portability demo runs in both environments from published
packages, not local builds.

---

## 14. Test plan

Lean. Cover semantics and boundaries, not permutations.

- **msgpack round-trip corpus.** One file covering each type, integer vs float,
  nested tables, forced array and map, empty table, deep nesting at the cap, cyclic
  input.
- **Queue semantics table** (`test/test_queue.lua`). One test per row of 6.4.
  These are the rows that will regress. Plus the model rather than the surface:
  value-not-reference semantics, FIFO across ring wraps, `pop` not parking, and
  a stale handle raising.
- **Delivery semantics.** Push to full, disabled, and gone destinations. Assert
  none of them raise and none of them block.
- **Driver contract** (`test/dtask_check.c`, `make dtask_check`). Yieldability
  inside the driver and non-yieldability outside it; error status, message and
  traceback across the state hop; argument and result counts past a fresh
  thread's free slots; the caller's stack unchanged; a top-level yield reported.
  In C because the driver has no guest binding by design.
- **Interrupt** (`test/interrupt_check.sh`, `make interrupt_check`). SIGINT to a
  runaway loop, in both execution modes. Shell rather than Lua because it needs
  a subprocess and a signal. The `--task` case is the one that regresses
  silently, since the handler still runs and still looks like it worked.
- **Instance ABI** (`test/dv_check.c`, `make dv_check`). Written against `dv.h`
  with no access to the runtime's internals, so it also checks the header is
  sufficient for a host. Runs wherever the suite runs, including where no Rust
  toolchain exists.
- **Yield/resume, per binding** (`bindings/rust/diluvium/tests/host.rs`). Push
  in, program wakes, program pushes out, host receives — with the host's own
  types on one side and Lua tables on the other.
- **Non-yieldable rejection** (`test/test_wait.lua`). One test per context in
  8.4, plus one asserting a yield inside `pcall` **succeeds**, and one asserting
  a VM-dispatched metamethod parks — the pair is what keeps the boundary from
  drifting in either direction.
- **Deadlock, not hang.** An indefinite park under a host that cannot satisfy it
  reports and exits. Run as a subprocess, since the failure being tested is a
  program that never returns.
- **No-C-frame assertion** (`test/dshim_check.c`, `agent_parked_on_wait_is_capturable`).
  Park an agent on `queue.wait` through the real driver and verify the CallInfo
  chain has no continuation-less `CIST_C` frame below the wait. This property is
  what makes M6 possible and it will silently break during refactoring. The test
  says which frames carry continuations and not only that the verdict is OK, so a
  regression names the frame that lost one. Worth recording: the wait path turns
  out not to need the innermost-frame exemption at all — `dq_wait` yields with
  `dq_wait_k` saved, so it would be capturable under the strictest reading of the
  rule.
- **Hibernate round-trip** (`test/dsnap_check.c`). Parked agent, shared upvalues,
  queue contents, handle re-resolution, refusal cases from 10.7, and a live
  `defer` in scope. The `defer` case is the highest-coverage single test available
  here: `defer` desugars to a to-be-closed local holding a self-referential
  `setmetatable(t, t)`, so one test exercises `tbclist` capture, closure
  serialization, and ext 0x04 backreferences at once. Both pass.

  Two more earned their place by catching something nothing else did.
  `an_open_upvalue_stays_open` resumes the restored coroutine so that it *writes*
  to a captured local and then asks the closure what it sees — a test that only
  reads passes even when each side got a private copy.
  `a_restored_agent_reports_its_waitset` asks the restored agent what it is waiting
  for before resuming it, which is the first thing a real host does and the only
  thing that depends on `u2.nyield` being carried.
- **Dump determinism.** Identical source produces byte-identical stripped dumps
  across processes. Precondition for 10.5.
- **Snapshot validation** (`script/fuzz_snapshot.py`, `make snap_fuzz`).
  Structure-aware mutation of a snapshot; assert refusal, never a crash. Plus a
  foreign host stamp refused. 430 mutants, 0 crashes. It runs the restore as a
  *subprocess*, which is not incidental twice over: a crash cannot be asserted from
  inside the process it happens in, and loading in a fresh process is what caught a
  continuation name that a same-process test could never have missed.
- **Attenuation.** A supervisor attempting to over-grant is refused.
- **Cross-host portability.** Identical bytecode under two hosts, identical output.

Benchmarks, reported not asserted: encode and decode throughput by message size,
push and pop cost for local queues, snapshot size and restore latency for a
representative agent. These inform the 6.5 revisit and the cache sizing in M7.

---

## 15. Open questions

1. **Snapshot cache storage.** File layout and eviction policy for M7. Not
   designed here.

Closed since the first draft: the swarm prefix is `dvs_` (11.5); default `on_full`
is `"reject"` (6.3); the queue name separator is `/`, structural only (6.6);
`inbox` and `outbox` are auto-declared (6.6); the capability check signature is
`(capability, resource, context) -> allow | deny` with the token model opaque
(8.5). Also settled earlier: the instance ABI prefix is `dv_`, the multi-instance
layer is named swarm, and an agent is one instance.

---

## 16. Decisions already made

Settled during design. Do not relitigate during implementation.

- msgpack, not protobuf. Programs are generated and rewritten at runtime, so schema
  compilation is a poor fit, and one codec serves wire and disk.
- Queues are guest-declared and guest-owned, not host-owned. The host is more likely
  to swap the program than the reverse.
- Queues are volatile, with enable and disable so a program going down rejects
  cleanly rather than accepting messages it will drop.
- Integer handles, not string lookup per operation.
- Dumb queues: no designated producer or consumer, no acknowledgement. Direction
  flags recorded but unenforced.
- The only guarantee is that a message was accepted into the next hop.
- Bounded always.
- Waiting is a yield, never a block.
- Routing, broadcast, retry, discovery, and store-and-forward are agents, not
  runtime features.
- Supervision is a capability expressed as a queue protocol, not a runtime
  primitive. Diluvium never owns a process table.
- There is one kind of program and one isolation boundary, the instance.
  "Supervisor", "coordinator", "handler" and "agent" are roles a program plays by
  holding a capability, never types.
- An agent is one instance, not a coroutine in a shared instance.
- Capability grants attenuate only, and the lifecycle capability delegates
  recursively with no additional mechanism.
- Hibernation is self-initiated, refuses below a continuation-less C frame, and
  restores only against an exact runtime and bytecode match.
- Yielding across `pcall` is legal Lua and stays legal. The yield-blocking and
  hibernate-blocking context sets are different sets and are documented
  separately.
- Snapshot Proto encoding goes through the real dump path, never a parallel
  encoder.
- The secure-function scramble stays deterministic, because content-addressed
  Protos depend on it.

---

## 17. Corrections against the first draft

Recorded because the reasons generalize, and because this document is the handoff
artifact between sessions that do not share context.

- **`pcall` was listed as non-yieldable.** It is not, and has not been since Lua
  5.2. The error came from generalizing backward from the hibernate constraint to
  the yield constraint via a shared diagnostic; the two sets are different. M3's
  acceptance criterion was inverted as a result and now asserts the opposite.
- **Hibernation's limit was stated as "any C frame."** It is any C frame *without
  a continuation*.
- **Hibernate bypassed secure functions.** Section 10.9 is new.
- **Restore's threat model was undifferentiated.** Section 10.10 separates
  validate, authenticate, and capability-check so that deferring one does not
  silently defer the others.
- **The codec was described as layer-independent while requiring the swarm
  layer's instance table.** Section 4.2 makes the dependency an injected
  interface.
- **The peek contract ignored the collector.** 11.2 now requires a registry
  anchor.
- **§8.2 said to convert the existing entry paths.** It cannot be done: top-level
  yieldability is observable from Lua and four upstream conformance tests assert
  the opposite, so coroutine hosting has to be a new door. This was the *second*
  correction of the same kind as the `pcall` one, found the same way — checking
  the claim against the tree instead of against an abstract Lua — which is why
  the lesson below is stated as a procedure rather than an observation.
- **The continuation was described as being for yields.** It runs on errors too,
  and the post-call code is unreachable on that path, so a naive continuation
  swallows every error into a successful exit. See 8.2 item 2.
- **The msgpack source was called BSD.** It is MIT. And the porting table in
  5.2 described a version of that file from years ago: upstream is already
  version-guarded and already uses `lua_isinteger`, so the warning to "expect
  real porting work" pointed at the wrong work. The real work was the ext
  registry, which upstream has none of.
- **§10.2's exclusion list described the wrong failure.** A comparator or a `gsub`
  replacement cannot hibernate, but it also cannot yield, so the shape the
  precondition refuses is not reachable from Lua at all. The check is a tripwire on
  a VM invariant, not a gate — and because of that, no test over a real thread can
  tell the correct rule from an over-broad one, which is why the rule is factored
  out as a pure predicate. See 10.2.
- **§10.7 listed four preconditions; there are six.** "Suspended" and "not inside a
  hook" were missing, and both are reachable states with distinct causes. The
  `normal`-status case — a supervisor parked inside a resume of a child — is the one
  a swarm hits routinely.
- **§10.6's runtime identity was a list of constants.** Version, opcode set,
  number config and `LUAC_FORMAT` are all identical between this tree's debug and
  release builds, which emit different bytecode — so that list would have accepted
  snapshots whose Proto hashes can never match, failing deep inside restore rather
  than at the header. Replaced with the hash of a canary chunk's stripped dump,
  which is self-calibrating. Found by measuring dump determinism before building
  on it, which is the procedure below working as intended for once rather than
  after the fact.
- **§10.4 named only C functions.** `_G` and the library tables are tables, so an
  encoder serializes them happily — and a snapshot of one closure that calls one
  global then drags the whole global environment in and fails on the first C
  function it meets. The permanents check has to run on tables, and before the
  object graph assigns positions.
- **§5.5 implied ext 0x03 stands alone.** It cannot: a prototype is not a Lua
  value, so it cannot take a position in the object graph. Its payload is carried
  inside 0x06.
- **A test can pass for the wrong reason, and mutation is what finds it.** Two
  prototype-tampering cases were silently exercising the reference path instead of
  the inline path, because an earlier test in the same file had registered the same
  prototype; one of them was passing on a refusal that had nothing to do with what
  it claimed to check. Removing the mitigation and watching *nothing* go red is the
  only thing that surfaced it. That has now happened three times in this milestone
  — the innermost-frame exemption, ext 0x07's position, and this — which is enough
  to say the rule plainly: a green test is evidence only after its mitigation has
  been removed once.
- **§10.7 refused all light userdata; the runtime's own wait protocol contains
  one.** `queue.wait` pushes a light-userdata sentinel onto a parked thread's
  stack, so the blanket refusal would have refused exactly the shape hibernation
  exists for. Named light userdata is fine; the permanents check runs first.
- **§10.4 did not mention continuations.** A suspended agent's chain has C frames
  with continuations saved, and a continuation is a bare pointer. `pcall`'s is
  static in a core file that is not on the allowlist, so it is discovered by
  watching a canary rather than named from a list.
- **§5.5 had no ext code for a thread**, which 10.3 requires. 0x08.
- **The amalgamation hides missing includes.** `LUAI_MAXSTACK` lives in `ldo.c`,
  not a header, and the debug build compiles anyway because `ltests.h` redefines
  it; `luaC_objbarrier` needed `lgc.h`. Both failed only in `make build_platform`,
  which is now part of the sweep.
- **Naming must not depend on history.** Three bugs of one shape: the permanents
  set, the driver's continuation, and the prototype registry all grew as a side
  effect of what an instance happened to have done. The permanents *fingerprint*
  travels in the header, so a set that grew lazily refused a snapshot from a
  started instance; a continuation registered where it is installed is missing in a
  process that only loads; and a prototype registered by the act of inlining it
  made the next snapshot reference code no other runtime had. All three are now
  eager or per-stream.
- **§10.8's handle re-resolution is unnecessary for a whole-instance snapshot.**
  The queue subsystem's state is a plain table, so restoring it verbatim brings
  handles back unchanged rather than stale. The step is still needed for the case
  10.8 was written for, and merging two numbering spaces is refused.
- **§10.10 needs two layers, not one.** Field validation cannot answer whether a
  frame's pc is the *right* in-range offset, so the header carries a digest of the
  payload. Measured against the fuzzer: 4 crashes with neither, 3 with field checks
  alone, 0 with both. The digest is integrity, not authentication.
- **The general lesson.** The first draft was written against an abstract Lua 5.5
  rather than against this tree, which is what produced both the `pcall` error and
  the secure-function gap. Assertions about core internals — `lua_upvaluejoin`,
  `CIST_C`, `tbclist`, the dump header, `LUAC_FORMAT` — should be checked at
  source level before the milestone that depends on them, not trusted because the
  surrounding reasoning holds together. Those five were checked while writing this
  revision and hold; the next set should get the same treatment.
