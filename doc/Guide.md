# The Diluvium programmer's guide

Everything else in `doc/` is about *building* Diluvium. This is about *using* it.

Two people read this. One writes **programs** — Lua that runs inside an instance,
holds queues, and parks. The other writes a **host** — C, Rust, Python or JavaScript
that creates instances, moves bytes between them, and owns the clock. Parts 1–5 are
for the first, 6–7 for the second, and 8–9 for both.

Every code sample here was run against this tree before being written down, and the
API shapes come from the source rather than from the design document — where the two
disagree, the source wins and `doc/Messaging.md` §17 records the correction. If a
sample does not work for you, that is a bug in this file; there is nothing aspirational
in it.

```
diluvium script.lua          # ordinary Lua, plus the language in part 1
diluvium --task script.lua    # the coroutine-hosted driver: the program may park
diluvium -i                   # REPL
diluvium_compiler -r f.lua    # static analysis report as JSON, to luac.out
```

**Contents**

1. [The language](#1-the-language)
2. [Messages](#2-messages-msgpack)
3. [Queues](#3-queues)
4. [The shape of a program](#4-the-shape-of-a-program)
5. [Endpoints](#5-endpoints)
6. [Embedding one instance](#6-embedding-one-instance)
7. [Running a swarm](#7-running-a-swarm)
8. [Idioms and traps](#8-idioms-and-traps)
9. [What not to rely on yet](#9-what-not-to-rely-on-yet)

---

## 1. The language

Diluvium is Lua 5.5. Every Lua program is a Diluvium program. What follows is what
it adds, and all of it is optional.

The additions that read as keywords — `switch`, `case`, `default`, `defer`, `with` —
are **contextual**: they are still ordinary names, so `local switch = 1` and
`t.defer = 2` and `local function with() end` all still compile. That follows the
precedent 5.5 set with its own `global`. The cost is that `switch (x)`, `switch "s"`
and `switch {}` stay *function calls*, because they always were.

### String interpolation

```lua
local name, pi = "world", 3.14159
print($"hello {name}, pi is {pi::%.2f}")   --> hello world, pi is 3.14
```

The `$` prefix is required. `{expr}` takes any expression; `{expr::spec}` passes the
value through `string.format` with that spec, so `%.2f`, `%5d`, `%q` all work.

### Null coalescing and safe navigation

```lua
local t = {}
print(t.missing ?? "fallback")     --> fallback
print(t?.a?.b, t?["a"])            --> nil   nil
```

`??` short-circuits: the right side is not evaluated when the left is non-nil. `?.`
and `?[` stop at the first nil instead of raising, which is the difference between a
missing optional field and a bug.

### Compound assignment

```lua
local n = 1
n += 4
n *= 3      --> 15
```

`+= -= *= /= //= %= ^= ..= &= |=` and the shifts. There is deliberately **no `~=`**,
because `~=` already means "not equal".

### `switch`

```lua
local function classify (x)
  switch x do
    case 1 then return "one"
    case 2, 3 then return "a few"
    default return "many"
  end
end
```

One `case` may list several values. `default` takes **no `then`** — a wart, but the
real syntax. Cases do not fall through, and the subject is evaluated once and frozen
at entry, so a `case` expression with a side effect cannot change what is being
matched. Nothing runs when no case matches and there is no `default`.

### `defer` and `with`

```lua
local function f ()
  defer print("second")
  print("first")
end

with fh = io.open("/dev/null", "r") do
  -- fh is closed at the end of the block, on any exit path
end
```

`defer stat` runs `stat` when the enclosing block ends, however it ends — return,
error, or falling off the end. It desugars to a to-be-closed local, so it survives a
`coroutine.yield` in between and runs at the right time rather than at yield time.
`with name = expr do ... end` binds a to-be-closed local directly, for a value with a
`__close`. Several bindings may be separated by commas.

### Secure functions

```lua
local ~function hidden (s) return "secret:" .. s end
print(hidden("x"))   --> secret:x
```

A `~function` has its strings and constants obfuscated in the compiled chunk, so
`strings` on a dump finds nothing. **This is obfuscation, not encryption** — recovering
the strings takes reading `ldump.c` and implementing the keystream, which is trivial
for anyone who wants to. It raises the cost from one shell command to an afternoon.
Do not put a credential in one.

---

## 2. Messages (msgpack)

Everything that crosses a queue is msgpack. The `msgpack` library is the only encoder
involved, and it round-trips a Lua value graph.

```lua
local bytes = msgpack.encode({id = 7, tags = {"a", "b"}})
local back  = msgpack.decode(bytes)
```

Integers stay integers and floats stay floats across a round trip — that is a
property the queue layer depends on, not a nicety.

### The one trap that will get you

**An empty table encodes as a map, not an array.**

```lua
('%02x'):format(msgpack.encode({}):byte(1))         --> 80   (map, 0 pairs)
('%02x'):format(msgpack.encode({1,2,3}):byte(1))    --> 93   (array, 3 items)
```

A table is an array if it has at least one element and its keys are exactly
`1..n`; otherwise it is a map. So `{}` is a map, and a list you happened to empty
changes shape on the wire. If the far side cares, say which you meant:

```lua
msgpack.encode(msgpack.as_array({}))   -- 0x90, an empty array
msgpack.encode(msgpack.as_map({1,2}))  -- 0x82, a map with keys 1 and 2
```

This has bitten this project more than once, including a capability list that was
refused as malformed because `caps = {}` arrived as a map.

### Ext codes

```lua
msgpack.ext(0x20, "payload")   -- fine: 0x10..0x7F is yours
msgpack.ext(0x02, "payload")   -- error: "ext code 2 is reserved"
```

Codes below `0x10` are reserved by the registry in §5.5 — decimals, endpoint
references, prototypes, backreferences, closures, threads. They are refused in **both**
directions, including for a wrapper you mutate after construction, so a program cannot
produce bytes that only the runtime is supposed to mean something by.

---

## 3. Queues

A queue is a bounded ring of msgpack messages with a name. Handles are integers,
private to the instance that made them.

```lua
local q = queue.declare('work', {capacity = 2, on_full = 'reject'})
queue.push(q, {n = 1})     --> true   "ok"
queue.push(q, 'two')       --> true   "ok"
queue.push(q, 'three')     --> false  "full"
queue.len(q), queue.capacity(q)          --> 2  2
queue.pop(q)               --> {n=1}  "ok"   (a table, decoded)
queue.pop(q)               --> 'two'  "ok"
queue.pop(q)               --> nil    "empty"
```

### `queue.declare(name, opts?)`

| Option | Values | Default |
|---|---|---|
| `capacity` | a positive integer | `64` |
| `on_full` | `"reject"`, `"drop_oldest"`, `"drop_newest"`, `"block"` | `"reject"` |
| `exported` | `true` to let the host read and write it | `false` |
| `direction` | `"both"`, `"guest_write"`, `"guest_read"` | `"both"` |

Bounded is the point: a full queue makes backpressure visible instead of absorbing it
as memory growth. `on_full = "block"` parks the *sender* until space appears, which
means only a program running under a host that can park may use it.

Declaring a name twice is an error rather than a silent reconfigure — re-declaring is
`destroy` then `declare`, and saying so beats a call that sometimes creates and
sometimes mutates.

### The rest of the API

```lua
queue.lookup('work')       -- the handle, or nil if there is no such queue
queue.push(q, value)       -- true,"ok" | false,"full"|"disabled"|"gone"
                           --            | true,"dropped_oldest"
queue.pop(q)               -- value,"ok" | nil,"empty"
queue.wait(list, ms?)      -- id, value, why   (yields; see below)
queue.len(q)               -- messages waiting
queue.capacity(q)          -- the bound
queue.state(q)             -- "enabled" | ...
queue.disable(q)           -- pushes now fail with "disabled"
queue.enable(q)
queue.destroy(q)           -- lookup returns nil afterwards
queue.info(q)              -- name, capacity, len, on_full, direction,
                           -- exported, endpoint
```

`inbox` and `outbox` **already exist** in every instance, declared exported, so the
zero-configuration case needs no code at all. Use `queue.lookup`, not `queue.declare`,
or you will get "already declared" — which is how this was found while writing
`examples/discofetch`.

### `queue.wait`

```lua
local id, msg, why = queue.wait({inbox, control}, 5000)
```

Returns **three** values: which handle fired, the message (already decoded and taken
off the queue), and why.

| `why` | Meaning | `id` | `msg` |
|---|---|---|---|
| `"ok"` | a message arrived | the handle | the value |
| `"timeout"` | the timeout elapsed | `nil` | `nil` |
| `"closed"` | a waited-on queue went away | the handle | `nil` |

The timeout is in **milliseconds**, and omitting it means no timeout. There is no
timer in the runtime — the host owns the clock and answers the wait, so a host may
answer a five-second timeout whenever it likes. At most 32 handles; wanting more is a
routing problem, and routing belongs in a program.

The message is *taken* by the wait, not left to be popped: that way a message can
never be reported ready and then lost to another waiter.

---

## 4. The shape of a program

A Diluvium program is a coroutine that stops when it has nothing to do. There are no
threads, no scheduler and no callbacks. The whole idiom is:

```lua
-- 1. declare what you own
local inbox = queue.lookup('inbox')
local log   = queue.declare('log', {capacity = 32, exported = true})

-- 2. set up
local seen = 0

-- 3. loop: park, wake, act
while true do
  local id, msg, why = queue.wait({inbox})
  if why == 'closed' then break end
  seen = seen + 1
  queue.push(log, ('handled %d: %s'):format(seen, tostring(msg.kind)))
end
return 0
```

Run it with `diluvium --task prog.lua`. Without `--task` the program runs on the main
thread, which cannot park, and `queue.wait` will raise rather than yield.

When that program is parked it costs about **42 KB** and no CPU (`make footprint`).
That is the number to plan capacity with: a thousand idle sessions is ~41 MB.

Three things a program cannot do, and each is deliberate:

- **It cannot push into another program's queue.** Only a host can (`dvs_push`), or an
  endpoint reference can. Part 5 and part 7 are the two ways round it.
- **It cannot learn its own instance id.** Ids belong to the swarm layer. A program
  identifies itself by something it already knows — a name it was given.
- **It cannot limit itself.** A runaway loop never yields, so nothing cooperative can
  stop it. Limits live outside, in the host's budget (part 6).

---

## 5. Endpoints

An endpoint is a queue handle for something that is not in this state. You receive an
opaque *reference* in a message and bind it:

```lua
local id, ref = queue.wait({inbox})
local peer    = endpoint.bind(ref, 'peer')     -- an ordinary queue handle
queue.push(peer, {hello = true})               -- push, len, wait: all unchanged
```

That is the whole trick: a sender cannot tell an endpoint from a local queue, and does
not need to. Where the bytes actually go is the host's business.

```lua
endpoint.status(handle)       -- "live" | "closed"; errors on a non-endpoint handle
endpoint.is_endpoint(handle)  -- true/false. Takes a QUEUE HANDLE, not a reference
```

**You cannot construct a reference.** There is no constructor anywhere, `msgpack.decode`
of hand-made ext `0x02` bytes gives you an inert opaque value that `bind` refuses, and
those bytes cannot be written back out to be laundered through the trusted delivery
path. A reference arrives in a message or you do not have one.

A reference *can* be passed on in a message — a router handing a handler its
destination — as of `v5.5.1_build3`. Before that it silently became a plain
one-element array on the wire.

See §9: while the `debug` library is open to guests, a program holding one real
reference can still forge another, so this is a structuring device and not yet a
security boundary.

---

## 6. Embedding one instance

`dv.h` is self-contained: no `lua.h`, no internal types. The contract tests are written
against it alone, which is how that stays true.

```c
#include "dv.h"

dv_instance *inst = dv_new(NULL);
dv_set_budget(inst, 5000000, 4096);           /* instructions, memory in KB */
dv_load(inst, (const uint8_t *)src, len, "=agent");

dv_waitset ws;
memset(&ws, 0, sizeof(ws));
dv_status st = dv_run(inst, &ws);              /* runs until it parks or ends */

while (st == DV_IDLE) {                        /* parked on ws.ids[0..ws.n-1] */
  dv_queue_id q = dv_queue_lookup(inst, "inbox");
  dv_queue_push(inst, q, msgpack_bytes, n);    /* the host encodes */
  st = dv_resume(inst, q);                     /* answer the wait */
}

dv_free(inst);
```

The status is the whole protocol: `DV_OK`/`DV_DONE` finished, `DV_IDLE` parked and
waiting, `DV_ERROR` failed with `dv_last_error(inst)`. `DV_BUSY` means you called
`dv_run` on something already started.

Set a budget **before** `dv_run`; setting one on a running instance is refused, because
a budget that changed mid-flight would make "exceeded" mean nothing. Exceeding it stops
the program with a catchable-looking error and sets `dv_exceeded(inst)`; the count hook
raises rather than yields, so a program cannot ride through it.

`dv_usage`'s instruction count is exact only to the hook's granularity, which is
1000 instructions — so a program that does very little honestly reports `0 insns`. The
memory figure is a high-water mark in KB and is the one to plan capacity with.

The rest of the surface, by job:

| Job | Calls |
|---|---|
| Lifecycle | `dv_new` `dv_load` `dv_run` `dv_resume` `dv_free` |
| Queues | `dv_queue_lookup` `dv_queue_state` `dv_queue_push` `dv_queue_pop` `dv_queue_peek` `dv_queue_release` |
| Waiting | `dv_waitset_get` |
| Limits | `dv_set_budget` `dv_usage` `dv_exceeded` |
| Endpoints | `dv_set_endpoint_handler` `dv_endpoint_allow` `dv_endpoint_queue` `dv_endpoint_close` |
| Snapshots | `dv_snapshot` `dv_restore` `dv_register_code` |
| Misc | `dv_abi_version` `dv_status_name` `dv_last_error` `dv_set_notify` `dv_layout` |

`dv_queue_peek` plus `dv_queue_release` is the zero-copy read: peek gives you a pointer
into the queue, release consumes it. Read the bytes before releasing, and copy anything
you keep — that pointer dies with the message.

To resolve endpoint references, either install a callback with
`dv_set_endpoint_handler`, or pre-authorise specific bytes with `dv_endpoint_allow(inst,
ref, len, token)` and then read the resulting queue with `dv_endpoint_queue(inst,
token)`. Pre-authorising is the only shape available to a host reaching the ABI through
wasm.

Bindings for Rust, Python and JavaScript ship in `bindings/`; the JavaScript one
carries a portable WASI preview-1 host, so a `.wasm` build runs under Node with no
native dependency.

---

## 7. Running a swarm

`libdiluvium-swarm` (`dvs.h`) adds an instance table, one parent link per instance,
capabilities, budget enforcement, and a snapshot cache. It is a separate library: an
app embedding a single sandbox links none of it.

**There is no supervisor type.** A supervisor is a program holding the `"lifecycle"`
capability. Restart policy, backoff, naming, discovery, routing and topology are all
ordinary programs — which, for a system whose programs are generated at runtime, is the
entire point.

### The host

The swarm cannot spawn anything by itself, because spawning means something different
in every environment. You supply three functions:

```c
dvs_host host = {0};
host.create  = my_create;    /* handed a made, loaded, budgeted instance */
host.destroy = my_destroy;
host.drive   = my_drive;     /* advance one step; return 1 to keep it */

dvs_swarm *sw = dvs_new(&host, 64 /* max instances */, 4 /* spawns per step */);
dvs_root(sw, src, len, caps, ncaps, insns, mem_kb, &root);
while (dvs_step(sw) > 0) { /* ... */ }
```

A single-threaded `drive` is one `dv_run` or `dv_resume` — that is a legitimate host,
not a stub. Copy `host_drive` from `examples/discofetch/swarmd.c` or
`test/dvs_check.c`.

**Return something non-NULL from `create` if you want `destroy` to fire at all**:
`release()` guards the destroy callback on `ctx != NULL`, so a host whose `create`
returns `NULL` is never told an instance went away.

### The lifecycle protocol

A program with the capability pushes to `system/lifecycle` and reads `system/events`.
Four operations:

```lua
queue.push(sys, {
  op    = 'spawn',
  code  = source_string,
  caps  = { 'queue:work/*' },                       -- must be an attenuation
  budget = { instructions = 5e6, memory_kb = 512 }, -- nested, as shown
  wake_on_message = true,                           -- optional
})
queue.push(sys, {op = 'kill',      id = child})   -- kills the subtree
queue.push(sys, {op = 'query',     id = child})   -- answered with a 'status' event
queue.push(sys, {op = 'hibernate', id = child})   -- see §9; off by default
```

And the events that come back, on the **parent's** `system/events`:

| `event` | Means | `detail` |
|---|---|---|
| `spawned` | the child exists; `id` is its handle | — |
| `exited` | it finished cleanly | sometimes `"killed"` |
| `faulted` | it raised | the error, with a traceback |
| `exceeded` | it hit its budget | which limit |
| `denied` | the request was refused | the capability, or the reason |
| `throttled` | the spawn rate limit; the request stays queued | — |
| `status` | the answer to `query` | `alive insns=N mem_kb=N` |

`faulted`, `exceeded` and `exited` are three events rather than one "it stopped",
because a supervisor's response to each is different.

A fault detail arrives with a traceback in it, so it has newlines — flatten it before
logging:

```lua
local function oneline (s) return (tostring(s):gsub('%s*\n.*', '')) end
```

### Capabilities

A trailing `*` is a prefix wildcard. A bare `"*"` is a **literal name**, deliberately:
otherwise a parent holding `"**"` could grant a child `"*"` and the child would end up
holding everything the parent did not.

Attenuation admits no exceptions. `caps` on a spawn must be implied by what the parent
holds, or the spawn is denied and nothing is created:

```
parent holds  queue:*            child may get  queue:client/alice     ✓
parent holds  queue:client/*     child may get  queue:*                ✗ denied
```

`"lifecycle"` is the one name the swarm layer itself checks, and it checks it at
*drain* time rather than at declare time — a program without it may declare
`system/lifecycle` and write to it all it likes, and nothing will ever read it. There
is no error to catch and work around.

### A worked example

`examples/discofetch/` is a supervisor, a coordinator, and one handler instance per
client, with a Dockerfile. It demonstrates in ~350 lines of C and three small Lua
programs: per-client isolation, a budget stopping a runaway, a spawn denied for asking
too much, and a restart policy written in Lua. Start there.

---

## 8. Idioms and traps

Collected from things that actually went wrong.

**Generate a program per unit of work.** A coordinator cannot send its handler a setup
message, so it writes the handler's parameters into the handler's *source* and spawns
that. Use `%q` for every value you interpolate:

```lua
src = template:gsub('@CLIENT@', function () return ('%q'):format(name) end)
```

`%q` produces a Lua-safe quoted literal. Without it, a client name containing a quote
or a newline becomes code. This is the difference between code generation and an
injection hole.

**Code is data.** Hand a program its source over a queue like any other string. The
host in `examples/discofetch` does exactly that rather than pasting Lua into Lua.

**A queue must exist before anything can be pushed to it**, and it exists once the
program that declares it has run. So a host cannot push into an instance it has not
driven yet. Drive first, then push.

**Handles are never reused.** A restarted child is a *different instance* with a new
id. A host that caches the first id keeps pushing at a dead handle and gets
`DVS_GONE`, silently, forever.

**A `spawned` event arrives before the child runs an instruction**, so a
`name → id` mapping built from it is always in place before that child's first
message. Rely on that ordering; it is the only one you get.

**Prefer relative timing to absolute steps.** The number of steps a handshake takes is
not a contract.

**Don't skip a msgpack header byte to read a string.** fixstr has a one-byte header,
`str8` has two. "Skip the first byte" works until a string passes 31 characters. Use
the token cursor (`diluvium_mp_open`/`diluvium_mp_read`) in C, or decode properly.

**One `lua_sethook` slot.** If you install your own hook you replace the instruction
budget's count hook and silently disable the budget. Dispatch to both, or don't.

---

## 9. What not to rely on yet

As of `v5.5.1_build3`. The full list with its reasoning is `doc/Messaging.md` §18;
this is what it means for code you are writing now.

**Hibernation is off, and should stay off.** `dvs_hibernate` refuses unless a host calls
`dvs_allow_hibernation`. The thread record does not carry `u2.funcidx`, so an error
raised in a *restored* program unwinds from the stack base and writes the error object
over the driver's own function slot — memory corruption, reached by the ordinary
wake-then-error path. **Keep agent state at the application level and spawn a fresh
instance per unit of work.** It costs little: `dv_new` plus `dv_load` of a small chunk is
comparable to `dv_new` plus `dv_restore` of a value graph, and at 42 KB resident, ten
thousand idle instances is about 410 MB.

Behind the same switch: a woken instance's instruction budget is not re-armed, the
swarm layer stamps no host identity on its own snapshots, and endpoints do not survive
a snapshot.

**The capability layer is not a security boundary.** Attenuation, subtree kill and
budgets all work and are tested — but `debug.getmetatable` reaches an endpoint
reference's private metatable and `debug.getregistry` reaches everything else, so a
program holding one real reference can mint one to any peer name it can guess. **Treat
every program you load as trusted**: written or templated by you, not generated by an
untrusted party and not accepting arbitrary Lua from a user. This is what §18.2 calls
profile A, and it is the release's supported configuration.

**Two smaller edges.** Rebinding an endpoint token whose queue was destroyed returns
the destroyed handle and poisons the token permanently — do not `destroy` a bound
endpoint queue and re-bind it. And §10.7's precondition that a snapshot capture involve
a single thread is documented but unenforced: nested coroutines are captured rather
than refused.

**No decimals.** Ext `0x01` is reserved for decQuad and unimplemented, so `1.23d`-style
literals do not exist and money is not a solved problem here yet. Numbers are Lua
numbers: 64-bit integers and doubles.
