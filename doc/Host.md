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

The **generic host** (`host/`, `make build_host`) is the reference
implementation of this protocol: one binary that drives a deployment from a
supervisor program plus a typed `*.host.lua` configuration, so a deployment is
data, not C. `host/dhost.c` is the core (construction, the drive loop, the
roster, the hostcall pump); the listener, SQLite, crypto, fs and exec
connectors live beside it. Read it as the worked example of the duties below.

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
grammar as everything else — `dvs_holds` exists precisely so a host mediating
its own resources asks the same question the swarm asks about queues), run
the connector, push the reply with `tok` echoed verbatim. Every drained
request is answered — `ok`, `denied`, `error` or `malformed` — never dropped.
Connectors are all off by default; a deployment's configuration names the
ones it wires, and lab wires JavaScript functions where production wires
system calls. The correlation token is load-bearing from the very first
prototype: do not ship a handler without it.

The queue names are conventions this protocol fixes, so guests are portable
between hosts: a guest that makes hostcalls declares **`host/calls`**
(exported, `on_full = "reject"`) and waits on **`host/replies`**. These names,
and the token discipline, are what the build7 `host` guest library encapsulates
(`doc/BUILD7.md` §1) — they remain the protocol, but a program should reach
them through the `host` library (`host.sql.open("db").exec(...)`) and never
declare these queues by hand. A guest
that declares no `host/calls` makes no hostcalls and costs the pump nothing;
one that declares no reply queue has asked questions with nowhere to hear
answers, which becomes its own diagnostic. A hibernated instance's pending
calls sleep in its queues and are answered after it wakes — the pump reads
resident instances only, which is correct rather than lazy: the reply
belongs in the log of the residency that reads it.

**The replay boundary, for connectors with state.** A hostcall reply is a
message, so it is in the log, so **a replay replays logged replies — it does
not re-execute connectors.** For pure connectors (time, rng) the distinction
is invisible. For stateful ones it is the whole point and cuts both ways:
replaying a run does not double-apply its `sql/exec` writes, *and* the
database's current contents are not what the replayed queries would see —
the database is **outside the replay boundary**, an external system the log
happens to describe. A deployment that needs the store itself reproducible
layers that on top (its own idempotency keys, snapshots of the database
beside the log); the host protocol promises only that the *program's*
execution replays.

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

## Configuration — decided, typed, and enforced

A deployment is a `*.host.lua` file returning one table, typed by the
LuaCATS schema in `host/types/host.lua` (so an editor completes keys and
catches typos ahead of time) and annotated examples in
`host/example.host.lua`. The JS host takes the same shape as an object.

**This form is a way-station.** `doc/Capabilities.md` is the direction: the
separate config artifact gives way to one configuration shape an instance takes
at every depth — the host being the root's parent — with grants expressed as
capability / permission / scope and attenuation the only rule. Two nearer
changes landed in build7 (`doc/BUILD7.md`): connector config grants a **scope**
(the sql connector takes a directory and the program names its database within
it, `host.sql.open`) rather than naming an exact file, and the **`host` guest
library** is the surface a program actually uses, so the raw queue idiom below
is the *mechanism*, not what anyone should hand-write.

Lua's syntax without Lua's power, and the power is removed by construction
rather than convention: the host evaluates the file in an **empty
environment**, text mode only — there is nothing to call, so the file can
declare and cannot compute — and then refuses any key it does not know **by
name**, because an unknown key is a typo about to become a silent default.
One file, one context: the config is host property; guest programs are their
own `.lua` files and are never inlined into it.

The listener's message shapes, which are the other convention guests are
written against: a completed request arrives on the configured queue
(default `http_in`) as `{conn, method, path, body}` — plus a `headers` map
when the deployment allowlists request headers (build7; lowercase names,
always present once configured, so the shape is config's decision) — and a
response leaves on the reply queue (default `http_out`) as `{conn, status,
body, content_type?, headers?}` — `conn` echoed verbatim, the hostcall
token discipline applied to traffic. The reply's `headers` map (build10)
is gated by the listen block's `response_headers` allowlist the same way
the request side is gated by `headers`: lowercase names in config, the
host's own framing names refused at load, and a reply header that is not
allowlisted, carries a control byte, or exceeds the value bound is dropped
whole — never truncated — while the response still answers, since on this
path it is the client that must be protected from a lying guest. The port is topology and comes from this file, never
from a guest: a guest cannot read a socket, and a listener that hibernated
with its program would be host state pretending otherwise.

The C implementation is `host/` (`make build_host`, `make host_check`);
`test/host_check.c` drives every duty end to end, the listener over a real
socket.
