# The host protocol

What a host *is*, and the duties any host performs — written so that two
implementations can exist and agree: the lab host in JavaScript (driving the
wasm build from outside the module) and the generic host in C (native, and
compiled to wasm32-wasi for engines like wasmtime). The acceptance test for
this document is behavioural: **a guest program must not be able to tell hosts
apart.** A program prototyped against lab's mock connectors runs unchanged
against the C host's real ones, because everything a host does reaches the
guest as messages and refusals, never as a distinguishable surface.

The vocabulary, because it has confused readers before: the **host** is the
embedding application — the code standing outside the sandbox boundary,
speaking the `dv_`/`dvs_` ABI. One OS process. An **instance** is one sandboxed
`lua_State` holding one program. The **swarm** is C bookkeeping mapping ids to
instances, parents, capabilities and budgets. The standalone `diluvium`
executable is none of these: it runs a script unsealed in its own state, and
the boundary machinery is never engaged.

`examples/discofetch/swarmd.c` is the reference implementation of this protocol
today, and is scheduled to dissolve into the generic host — a supervisor
program plus configuration — once that exists. It should not grow.

## The duties

**1. Construction.** Create the swarm (`dvs_new` — the vtable below, a table
bound, a spawn rate), give it an identity when the deployment has one
(`dvs_set_host_identity`), apply policy opt-outs (`dvs_allow_hibernation(sw, 0)`
for a resident-only deployment; `dvs_allow_unsafe_stdlib` never, outside
scaffolding), and start exactly one program: the root, via `dvs_root`, with the
capability ceiling and budget the configuration names. Everything else in the
swarm descends from the root by attenuation. A generic host takes all of this
from configuration; nothing in it is code.

**2. The drive loop.** Call `dvs_step(sw)` until `dvs_alive(sw)` says nothing
is left, or shutdown is requested. Each step drives every resident instance
once through the vtable's `drive` callback, which owns answering that
instance's park: inspect the wait-set, decide which queue fires, `dv_resume`.
The loop is single-threaded and cooperative by design — instances only ever
run inside `drive` — which is what makes the whole protocol implementable in a
browser without threads. Determinism note: driving in id order is deterministic
by accident, not by design; the scheduler question (`doc/Determinism.md`) is
deliberately still open, and a host should not advertise replay guarantees it
has not implemented.

**3. The roster.** The vtable's `create` callback fires for every instance
that comes to exist (spawn and wake alike), `destroy` for every one that stops
being resident (death and hibernate alike); the host's per-instance context
pointer rides between them. A host maintains its roster from these callbacks —
there is deliberately no enumeration API — and answers panel queries from it
plus the per-id calls: `dvs_parent`, `dvs_budget`, `dvs_caps`, `dvs_resident`,
`dvs_cached_size`, and via `dvs_instance` → `dv_usage` / `dv_exceeded` /
`dv_last_error` while resident. A hibernated instance's instruction count is
not yet reachable through the swarm API (the number sits in its snapshot
header); until an accessor exists, a panel shows a hibernated instance as
hibernated, with its budget and cached size, and no usage figure. That stub is
the agreed v1 behaviour, not an oversight.

**4. The queue pump.** Outbound: drain the exported queues the deployment
cares about (`dv_queue_pop` on the handles the host looked up by name).
Inbound: push external events — network requests, UI actions, test fixtures —
into the queues guests wait on (`dv_queue_push`, or `dvs_push` for a cached
instance, which buffers and optionally wakes). This is where lab's "mock" and
production's "real" become the same thing: a request object pushed into a
coordinator's inbox is indistinguishable to the guest whether a socket or a
button produced it.

**5. Hostcalls.** The encoding is `doc/Hostcall.md` and is the contract; the
host's half is: drain each guest's request queue, dispatch on `call` against
that guest's granted capabilities (`host:time`-style grants, same attenuating
grammar as everything else), run the connector, push the reply with `tok`
echoed verbatim. Every drained request is answered — `ok`, `denied`, `error`
or `malformed` — never dropped. Connectors are all off by default; a
deployment's configuration names the ones it wires, and lab wires JavaScript
functions where production wires system calls. The correlation token is load-
bearing from the very first prototype: do not ship a handler without it.

**6. Hibernation policy.** The mechanism is the runtime's; *when* is the
host's. `wake_on_message` delivery is already handled by the swarm layer; what
a host decides is when to `dvs_hibernate` idle instances, and optionally
whether to persist — the snapshot bytes are host-owned, and writing them to
disk and restoring after a process restart is supported and deliberately
conservative (same build fingerprint, same permanents, budgets re-supplied
before restore, ids fresh in the new generation: treat remembered ids as
volatile across generations). The swarm's own topology has no serializer yet;
a host that persists snapshots owns recording which bytes belonged to what.

**7. Shutdown.** Stop stepping, then `dvs_free`, which releases every slot.
A host with persistence hibernates what it wants to keep first. Nothing here
is graceful-by-magic: a program that should flush on shutdown should be told,
by a message, like everything else.

## What a host must not do

Reach around the boundary. No connector hands a guest a live object, a shared
buffer, or a callback — answers are messages, copied like all messages. No
host feature may depend on reading a guest's internals beyond the ABI's
queries. And no host should widen what the sandbox sealed: the JS host in
particular exposes JavaScript *only* as hostcall connectors, never as FFI —
the moment a JS function is callable without crossing the queue, sealing,
capabilities, metering and replay all have a hole in them at once.

## Configuration, sketched

What the generic host reads instead of being edited; the JS host takes the
same shape as an object. Indicative, not final:

```
supervisor   = "supervisor.lua"        -- the root program
max_instances = 64
spawns_per_step = 4
identity     = "prod-cluster-7"        -- optional; stamps snapshots
hibernation  = "on"                    -- or "off": policy, not capability
budget       = { instructions = 5e6, memory_kb = 512 }   -- the root's
caps         = { "lifecycle", "queue:*", "host:time" }   -- the ceiling
connectors   = { time = true }         -- everything absent is off
```
