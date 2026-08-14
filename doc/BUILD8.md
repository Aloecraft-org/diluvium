# 5.5.1_build8: nothing blocks the shared thread

**Status:** in progress, 2026-08-14. Written against the tree at `fd73209`
(5.5.1_build7). Every claim below about existing code carries a `file:line`;
they were checked, not remembered, because the whole shape of this build turns
on how much is already here.

**Landed so far:** Part 1 in full (the deferred reply, the ledger, the
reply-queue accounting fix, the single sleep point) and Part 2 in full (the
manifest, the `plugins` config block, socketpair spawn over fd 3, framing,
the three error classes, `max_inflight`). `make host_check` is 54 checks / 0
failed, including a real child process over a real socketpair, and the rest of
the suite is unchanged: dvs 139, dsnap 159, dhash 252, dshim 102, dv 245,
dtask 25, and the Lua suite at 51 passed / 3 skipped.

**Not yet built, and not pretended:** Part 3 (generated wrappers — `host.call`
carries the release without them), Part 4 (the wake policies are *declared* in
the manifest and *validated* at load, and nothing consumes them yet), and Part
5 (no REST plugin exists; `test/plugin_echo.c` is an echo fixture). Gates 7.6
and 7.9 are consequently still red, and §7 is the list that keeps that
honest.

Build 7 made a hostcall *readable*. Build 8 makes it *concurrent*, and then
makes it *extensible* — a capability can live in another program, on the far
side of a queue, and a hundred agents can each be mid-call without any one of
them stalling the others.

Two features, in this order, because the second depends on the first:

1. **Deferred hostcalls.** A connector that cannot answer now says so, and the
   host goes back to driving the swarm. Today it answers inline and the whole
   process waits.
2. **The plugin channel.** Host-side capabilities become external programs
   speaking a framed msgpack protocol over a dedicated channel, described by a
   manifest. New capability on the other side of a queue stops requiring a new
   Diluvium release.

The ordering is not aesthetic. A plugin call is *by definition* a call that
takes time, so shipping the plugin channel on top of a synchronous pump would
ship the exact stall the channel exists to avoid.

---

## 0. What already exists — read this before planning any of it

The handoff this plan came from described build 8 as a scheduler rewrite:
a new `dv_hc_submit` hostcall, a Lua-side yield, a readiness-driven loop
replacing a round-robin one, a pending table holding `luaL_ref` registry
references to parked coroutines. **Almost none of that is needed.** Checking
each claim against the tree collapsed the build to roughly a third of its
described size, and the third that survives is all in one place.

### 0.1 Guest-side parking already works

`src/dhostlib.c`'s `roundtrip` pushes the request and then parks:

```lua
local sent, why = queue.push(calls, { tok = t, call = call, args = args })
...
local _, m, st = queue.wait({ replies }, waitms)
```

`queue.push` returns normally; `queue.wait` yields through `lua_yieldk` with a
registered continuation (`dq_wait_k`, `src/dqueue.c:681`, `:782`). That *is*
the submit-plus-yield shape §4.1 of the handoff proposes, spelled in the
vocabulary the tree already has. `dv_hc_submit` would be a second name for
`queue.push`.

The scheduler half is equally present. `host_drive` (`host/dhost.c:855`) asks
the instance for its wait-set, delivers to the round-robin next queue that has
something, and — when nothing is ready — returns `1` at `host/dhost.c:878`
with the comment *"parked; nothing to deliver yet"*. The swarm moves to the
next instance. A parked instance does not block another one **today**.

This is not incidental. It is the correction `doc/Messaging.md` §17 records
against its own first draft: *"A hostcall was reasoned about as a function
call, so it looked like it had to block... Queues are unidirectional: asking
is a `queue.push`, which returns, and the answer arrives later like any other
message."* Build 8 is the first build to actually lean on that.

### 0.2 A generic call already reaches any connector

`src/dhostlib.c:241-242`:

```lua
call = function(name, args) return ask(name, args) end,
try  = function(name, args) return try(name, args) end,
```

So `host.call("rest/get", { url = u })` reaches a connector registered under
the prefix `rest` with **no guest-side change at all**. Generated wrappers
(§3) are sugar over this, not the mechanism. That is a deliberate de-risking:
the plugin channel's acceptance does not depend on codegen landing.

The capability gate needs no new grammar either. `answer` builds `"host:" +
call` and checks `dvs_holds` (`host/dhost.c:983-985`), so `rest/get` is gated
by `host:rest/get` and attenuates through spawns like everything else.

### 0.3 The blocking is one call, in one place

`answer` invokes the connector synchronously and immediately writes the reply
(`host/dhost.c:1011-1030`):

```c
st = conn->fn(conn->ud, id, call, args, argslen, &value, detail, sizeof(detail));
dh_map(reply, 3);
```

`dh_call_fn` (`host/dhost.h:170`) has no way to say *"later."* That is the
entire defect. `host/dhost_exec.c` is where it bites: a correct non-blocking
`poll` loop over the child's pipes (`host/dhost_exec.c:245-263`) that runs
**inside** the connector call, so the host thread sits in it until the child
exits or the deadline fires. The schema documents this honestly today
(`host/types/host.lua:112-114`): *"the host answers hostcalls synchronously: a
running child stalls every guest and the listener until it exits or hits the
deadline."*

Build 8 deletes that sentence by making it false.

### 0.4 The permanents fingerprint does not have to move

`ds_perm_walk` (`src/dsnap.c:188-206`) names only C functions:

```c
if (lua_type(L, -2) == LUA_TSTRING && lua_iscfunction(L, -1)) {
```

and the fingerprint is SHA-256 over the sorted **name** list
(`src/dsnap.c:296-340`). The `host` library is an embedded Lua chunk
(`src/dhostlib.c:36`), so its functions are Lua closures and contribute no
names; only the `host` table itself is a named permanent, and it is already in
`DS_MODULES` (`src/dsnap.c:154`).

**Therefore: build 8 is not a snapshot-boundary build, provided every new
guest-visible function is a Lua closure inside the existing `host` table.** A
build7 snapshot restores on build8. This is a real constraint with a real
payoff, and §8 states it as a rule rather than a hope.

### 0.5 There is no leak surface to design

The handoff's pending table holds a `luaL_ref` per parked coroutine, and warns
that the ref *"is the whole lifetime and leak surface."* In this tree it does
not exist: the host lives outside the sandbox and addresses instances by
`dvs_id`, not by pointer. A pending entry is `(dvs_id, tok)` — two integers.
If the instance is gone, `dvs_instance(h->sw, id)` returns `NULL` and the
delivery is dropped. Nothing is anchored, so nothing leaks.

The parked coroutine is kept alive by its instance, and the instance by the
swarm, exactly as it is today for a guest parked on any other queue.

### 0.6 What is actually left

| Handoff said | Tree says | Build 8 does |
|---|---|---|
| New `dv_hc_submit` hostcall | `queue.push` is it | nothing |
| Lua-side yield wrapper | `queue.wait` is it | nothing |
| Readiness-driven scheduler | `host_drive` is it | nothing |
| Pending table with `luaL_ref` | ids, not refs | a two-integer table, host-side |
| Instance teardown drops refs | no refs exist | sweep by id in `host_destroy` |
| Argument marshalling out of the Lua stack | the pump already copies to `dh_buf` | keep the discipline |
| Snapshot boundary | closures are unnamed | **do not cross it** |
| Connector may answer later | `dh_call_fn` cannot | **`DH_CALL_PENDING`** |
| Plugin channel | nothing | **all of it** |

Everything in bold is Part 1 and Part 2. Everything else is already paid for.

---

## 1. Part 1 — the deferred reply

The gate for the whole build. No transport, no framing, no subprocess: just the
seam that lets a connector answer later, proven against a deliberately slow
in-process capability. Part 2 is built on a contract that is already green.

### 1.1 `DH_CALL_PENDING`

`dh_call_status` (`host/dhost.h:164`) gains a fourth value:

```c
typedef enum dh_call_status {
  DH_CALL_OK = 0,
  DH_CALL_ERROR,
  DH_CALL_DENIED,
  DH_CALL_PENDING       /* taken; exactly one dh_reply is owed */
} dh_call_status;
```

A connector returning `DH_CALL_PENDING` has taken ownership of `(id, tok)` and
**owes exactly one delivery**. `answer` writes no reply bytes; `pump_instance`
pushes nothing (it already guards on `reply.len > 0`, `host/dhost.c:1067`).
The request is still consumed, so the calls queue cannot wedge.

The debt is the contract. One reply, never zero, never two — and the pending
table is what makes that checkable rather than merely intended.

### 1.2 The pending table

Central, in `dh_host`, not per-connector. Two reasons: the sweep on instance
death has to happen somewhere that knows about instances, and one table is
auditable where five are not.

```c
typedef struct dh_pending {
  dvs_id id;
  int64_t tok;
  size_t conn;                /* index into h->conns: who owes this */
  void *ud;                   /* the connector's own handle for the call */
  int64_t deadline_ms;        /* host-side backstop; 0 = none */
  struct dh_pending *next;
} dh_pending;
```

```c
/* Answer a call a connector took as PENDING. Idempotent-hostile on purpose:
   a second delivery for the same (id, tok) is a connector bug and is
   refused, not silently accepted. */
int  dh_reply (dh_host *h, dvs_id id, int64_t tok, dh_call_status st,
               const unsigned char *value, size_t vlen, const char *detail);
/* How many replies this instance is owed. */
size_t dh_pending_count (dh_host *h, dvs_id id);
```

Three lifetime rules, each with a named failure it prevents:

- **Instance teardown sweeps.** `host_destroy` (`host/dhost.c:835`) already
  fires when an instance goes away; it drops that instance's pending entries
  and calls the owning connector's new optional `cancel` hook so a plugin-side
  computation can be abandoned. Without this, a dead instance's entries
  accumulate for the life of the host.
- **A reply for an unknown `(id, tok)` is discarded without error.** That is
  the normal case after a teardown or a timeout, not a fault.
- **`dvs_id` reuse must not alias.** If the swarm recycles ids, a stale reply
  can land on a new instance holding the same id. Confirm `dvs_id` is
  monotonic; if it is not, the pending key gains a generation counter. *This
  is the one open verification in Part 1 and it is checked before the seam
  ships, not after.*

### 1.3 Reply-queue accounting — the correctness bug this introduces

`pump_instance` refuses to drain a request it cannot answer
(`host/dhost.c:1055-1061`): if the replies queue is full, the request stays put.
The comment explains why — *"every drained request is answered"* is kept by not
draining until there is room, which also stops a stateful connector being re-run.

Deferral breaks that accounting. A pending call has been drained but has produced
no reply yet, so the queue looks emptier than it is. Drain twenty pending calls
into a sixteen-deep replies queue and four replies have nowhere to land; the
guest waits out its timeout for an answer the host already computed.

The fix is one term:

```c
if (info.len + dh_pending_count(h, sc->id) >= info.capacity)
  break;
```

This is §5.4's flow control appearing at the *queue* rather than at the pipe,
and it is the more fundamental of the two. `max_inflight` bounds what the
plugin sees; this bounds what the guest can be owed. Both are needed and they
are not the same bound.

### 1.4 One poll point

The main loop (`host/dhost_main.c:56-74`) sleeps in `dh_http_poll` when there is
a listener and in `nanosleep` when there is not. A plugin has fds that must be
in whatever the process sleeps on, and "call both in turn" is not available:
each takes a timeout, so the first to sleep delays the second.

`dh_http_poll` (`host/dhost_http.c:447-502`) already builds a `pollfd` array
each turn and dispatches `revents` afterward. Split it along the seam it
already has:

```c
typedef struct dh_pollset dh_pollset;      /* growable pollfd + owner tags */

void dh_http_arm   (dh_host *h, dh_pollset *ps);   /* contribute fds */
void dh_http_fire  (dh_host *h, dh_pollset *ps);   /* handle revents */
void dh_plug_arm   (dh_host *h, dh_pollset *ps);
void dh_plug_fire  (dh_host *h, dh_pollset *ps);
```

The main loop arms both, calls `poll` **once**, fires both. The `nanosleep`
branch disappears: with nothing armed, `poll(NULL, 0, timeout)` is the same
sleep with one fewer code path.

This is the largest refactor in Part 1 and it is worth its cost. Build 8's
whole claim is *nothing blocks the shared thread*; that claim is only
checkable if there is one place the thread sleeps.

### 1.5 The proof: `slow`, a deliberately slow in-process capability

Before any subprocess exists, a test-only connector that defers every call and
answers it N milliseconds later off the host clock. It has no transport, no
framing, and no child process, so what it tests is exactly the seam and nothing
else.

The acceptance test: **N instances, each calling `slow` with a stagger, and the
assertion that total wall time tracks the slowest call rather than their sum**,
plus a check that the host's own turn count keeps advancing while all N are
parked. On today's tree that test cannot pass. That is the point of writing it
first.

---

## 2. Part 2 — the plugin channel

### 2.1 The manifest: `<capability>.plugin.json`

One self-contained file per plugin, named for the capability it provides:
`rest.plugin.json`, `aloecrypt.plugin.json`. The `.plugin.json` double
extension follows `*.host.lua`'s precedent — the middle word says what kind of
document it is, the last says how to parse it — and it makes a directory of
plugins greppable and a stray `.json` unambiguous.

```json
{
  "schema": 1,
  "plugin": {
    "name": "rest",
    "exec": "/usr/local/libexec/diluvium-rest-plugin",
    "checksum": "sha256:9f2c…",
    "transport": "socketpair",
    "max_inflight": 8
  },
  "capabilities": [
    {
      "name": "get",
      "wake": "reissue",
      "errors": ["timeout", "dns", "tls", "status"],
      "args":   { "type": "object", "properties": { "url": { "type": "string" } } },
      "result": { "type": "object", "properties": { "status": { "type": "integer" } } }
    }
  ]
}
```

`plugin.name` plus `capabilities[].name` compose the call name (`rest/get`),
which composes the capability (`host:rest/get`). One naming rule, all the way
down.

**Parsing costs nothing new.** The host already keeps a `lua_State` for its own
configuration (`host/dhost.c:392-403`: `luaL_newstate`, `luaL_loadfilex` in text
mode, an empty `_ENV`), and the tree already ships a strict, bounded JSON
decoder as the `json` guest library (`src/djson.c`) — depth-bounded, control
bytes refused, trailing bytes refused, which are exactly the properties you want
for a file read at startup. The manifest reader is `json.decode` into that
state, then the same `cfg_known_keys` / `cfg_str` / `cfg_num` walk
(`host/dhost.c:186-258`) with the same refuse-unknown-keys-by-name discipline.

**No JSON parser is vendored and no schema validator is embedded.** That is
§6.1's first constraint made literal: the runtime reads the flat metadata
(`name`, `wake`, `errors`, `max_inflight`, `exec`, `checksum`) and **skips
`args` and `result` entirely**. Validation stays plugin-side and tooling-side.
The second constraint — no external `$ref` — falls out for free, because
`json.decode` fetches nothing.

The third constraint, binary encoding, is stated once here and not reinvented
per plugin: **msgpack `bin` on the wire, base64-in-string in the schema**. The
codec reads `bin` and `str` into the same token (`src/dmsgpack.h:118`), so the
wire is honest and only the *description* of it needs the convention.

### 2.2 Wiring: the `plugins` table

Sibling to `connectors` in `*.host.lua`, because a plugin is a capability the
deployment grants and that is what `connectors` means:

```lua
plugins = {
  rest = { manifest = "rest.plugin.json", max_inflight = 8 },
},
```

`manifest` resolves relative to the config file. `max_inflight` here overrides
the manifest's, so an operator can throttle a plugin they did not write without
editing its manifest — the manifest states what the plugin can take, the
deployment states what this deployment will send.

`dh_config` gains a `dh_plugin_cfg plugins[DH_MAX_PLUGINS]` and `nplugins`;
`top_keys` (`host/dhost.c:365`) gains `"plugins"`. `DH_MAX_CONNECTORS`
(`host/dhost.h:175`) rises from 8, since every plugin claims a connector slot
alongside the five built-ins.

### 2.3 Transport

| Target | Binding |
|---|---|
| glibc / musl / macOS | `socketpair(AF_UNIX, SOCK_STREAM)`, plugin inherits fd 3 |
| Lab (browser) | Worker plus `postMessage` |
| WASI / wasmtime | see §5.3 — honest constraints, not a promise |

**Not stdin/stdout.** A stray `print`, a library log line, a panic trace or a
JVM warning desyncs the framing permanently and unrecoverably. stdout and
stderr stay free for plugin logging, which is also what makes a plugin
debuggable by running it by hand.

The inherited-fd binding has a property worth naming: the channel has no
filesystem path and no port, so nothing else can connect to it. Parentage is
structural, not negotiated, and there is no authentication step because there
is no one else who could be on the other end.

`host/dhost_exec.c:195-232` is the worked example for the spawn and should be
lifted rather than rewritten: `fork`, `setpgid(0,0)` so a kill reaches the whole
tree, `signal(SIGPIPE, SIG_DFL)` because the host's ignore is process state a
child should not inherit, and the close-everything-above-the-standard-three loop
— amended for build 8 to keep fd 3, which *is* the channel.

The one deliberate difference: **`execv` with an absolute path, never
`execvp`**. `dhost_exec.c` uses `execvp` because a guest naming `git` should
find `git`; a plugin path comes from a manifest an operator wrote, and a PATH
lookup there is an injection surface with no upside (§6.1).

### 2.4 Framing

Length-prefixed msgpack. A `uint32` big-endian byte count, then that many bytes
of one msgpack map. Every frame carries:

- `id` — request correlation, a **host-global counter**, and deliberately not
  the guest's `tok`. The plan this build came from proposed reusing `tok` so
  that no translation table could drift; that is wrong, and the tree says why:
  a `tok` is chosen by the guest and is unique only within its instance, so two
  instances collide on their first call. The map from wire id back to
  `(instance, tok)` is the plugin's in-flight table.
- `target` — the capability name (`"get"`; the plugin knows its own prefix).
- `version` — frame format version, `1`, present from v1 precisely because a
  format that gains a version byte later cannot be told from one that never had
  one.

Reply frames additionally carry:

- `final` (bool) — always `true` in build 8. Reserved so that token streaming
  does not need a breaking change. Ship the flag, not the feature.
- `error` (nullable) — §2.5.

The host needs no new codec. `dh_buf` already carries msgpack emitters
(`host/dhost.h:44-57`) and `diluvium_mp_*` is a read cursor over bytes with no
`lua_State` in sight (`src/dmsgpack.h:140-160`), which is what lets the plugin
channel be framed entirely outside the sandbox.

### 2.5 Error classes

Not error strings. Three classes, distinguishable by the guest, because the
retry decision differs for each:

- `transport` — the channel or the plugin process failed. Retryable, and the
  host may be able to restart the plugin.
- `plugin` — the plugin itself errored. Not retryable without a fix.
- `capability` — the underlying service returned an error (a 401 from Auth0, a
  503 from an upstream API). Retryable or not depending on the service, which
  is why the capability declares its own enums in the manifest (`errors`) and
  the class is only the outer container.

These map onto the existing reply status vocabulary rather than extending it:
all three arrive as `status = "error"` with the class in `detail`'s structured
form, so `host.try` keeps working unchanged and no guest needs to learn a
fourth status.

### 2.6 Flow control

`max_inflight` per plugin. Without it: forty parked instances call the same
plugin, the host writes forty request frames, a plugin with a simple
read-work-reply loop handles them serially, the pipe buffer fills at roughly
64KB, and the host's `write` blocks — which is precisely the failure this
build exists to remove, reintroduced at the last possible moment.

With it, excess requests wait in the host's own queue, where waiting is free
because those instances are parked anyway.

Two disciplines, not one:
- The channel fd is **non-blocking**, and a partial write is retained and
  finished on the next `POLLOUT`. `max_inflight` is a bound, not a guarantee;
  a non-blocking fd is the guarantee.
- Plugin authors remain responsible for expected load. The manifest field is a
  floor under a bad plugin, not a substitute for designing a good one.

### 2.7 Security posture

Explicit and deliberate: **there is no plugin authentication in build 8.**

Containment comes from the plugin being a narrow program — fixed endpoint,
parses a frame, does one job — rather than from certifying it. An attacker who
can swap the plugin binary on the box can equally swap `diluvium-host`, so
authentication buys little against the attacker who actually exists.

Three things are still required, because each costs one line:

1. **`execv` from an absolute path.** No shell, no PATH. §2.3.
2. **Checksum recorded in the manifest, logged at startup, not enforced.**
   Forensics now; enforcement becomes a one-line change later.
3. **`--insecure-plugins` opt-in**, with a startup warning. The flag enables
   insecurity rather than enabling checking, so the default is correct on the
   day someone forgets to pass anything.

Lab is unaffected: a browser module involves no exec at all and its
authenticity is the page's problem. SRI is available later if it matters.

---

## 3. Part 3 — generated wrappers

Ergonomics, explicitly off the critical path (§0.2: `host.call` already works).

**Generation timing: load time, all capabilities in the manifest.** Lazy
generation buys nothing here — there is no existing lazy-codegen anything in
this tree, every guest library registration is eager, and laziness would add a
cache-invalidation axis and a first-call latency spike at plugin counts this
build will never reach. Load-time is also the tighter contract: a manifest that
names a malformed capability fails at startup rather than at 3am on first use.

The generated form is uniform, which is what makes this codegen worth doing at
all — the only per-capability datum is the name:

```lua
host.rest.get = function(args, waitms) return host.call("rest/get", args, waitms) end
```

Two consequences follow, and they are the whole design:

- **`host.call`/`host.try` gain an optional third argument** (`waitms`), a
  one-line change to `src/dhostlib.c:241-242`. A REST call should not inherit
  the library's 10-second default by accident, and `exec` already demonstrates
  the pattern (`src/dhostlib.c:218-226`).
- **Wrappers are Lua closures and are never registered as permanents.** This
  is the §8 rule and it is what keeps the fingerprint identical across
  deployments that wire different plugins. A C function here would make the
  fingerprint depend on the manifest, and two hosts with different plugin sets
  could no longer exchange snapshots at all.

The open mechanism is delivery: host-side generated source has to reach a
guest's globals, and no host→guest code-injection path exists today. The
candidate is a **prelude chunk** set on the swarm at `dh_host_open` and run in
each instance after libraries open and before the program starts — new
machinery in `src/dvs.c`, which is Diluvium's own file, so §"on-top only"
holds. It must also run on **restore**, because `host` is a named permanent and
resolves to the restoring runtime's fresh copy, which has no wrappers until
something puts them there.

If the prelude turns out to be hairy, Part 3 is dropped and `host.call` carries
the release. That is why it is Part 3 and not Part 1.

---

## 4. Part 4 — hibernate and the wake policy

A reply to a hibernated instance takes the existing wake-on-message path:
restore the snapshot, then deliver. No new mechanism.

What is new is that the host-side pending state is **not** in the snapshot — it
cannot be, it lives outside the sandbox — so a wake has to decide what to do
about a call that was in flight. The snapshot carries the parked closure as
content (§0.4), so the guest still believes it is waiting. The manifest decides,
per capability:

- `reissue` — idempotent calls. The host re-submits and the guest never knows.
- `cached` — the result completed before hibernation and is replayed.
- `error` — anything else. The guest's `queue.wait` returns a `transport` error
  and the program handles it like any other failure.

This is the field that turns *"you cannot hibernate mid-hostcall"* from a
limitation into a declared per-capability decision. It is also why `wake` is a
required manifest field rather than an optional one: the default would be wrong
for something.

**Status: declared, not acted on.** The enum is in `dh_plugin_cap.wake`, the
manifest loader requires it and refuses anything else by name, and
`host/rest.plugin.json` shows both interesting answers (`get` is `reissue`,
`post` is `error`, and the manifest says why beside each). No code reads the
field yet. Recording the decision at the point the plugin author is thinking
about it is worth doing first — the field is the hard part; consuming it is a
branch in the wake path — but until that branch exists, gate 7.6 is red and
this section describes a plan, not a behavior.

---

## 5. Part 5 — the REST plugin

The first plugin, and the reason for the build. There is no outbound HTTP
capability in the tree today — `host/dhost_http.c` is the inbound listener only
(`host/types/host.lua:40-47` lists `time`, `listen`, `sql`, `crypto`, `fs`,
`exec`), and `net` is exactly what `doc/BUILD7.md` §5 deferred. Build 8 closes
it as a plugin rather than as a connector, which is the whole thesis: **the host
binary never learns to speak HTTP.**

```
rest/get    {url, headers?, timeout_ms?}         -> {status, headers, body}
rest/post   {url, headers?, body, timeout_ms?}   -> {status, headers, body}
```

### 5.1 musl / native

A standalone program, execed from an absolute path with fd 3 as the channel.
Written in whatever is convenient — the point of the protocol is that the host
does not care. It links its own TLS; the host links none, which is why
`build_host_musl`'s fully-static link stays clean.

### 5.2 Lab (browser)

No subprocess. A Worker running `fetch()` behind `postMessage`, speaking the
same frame bodies. `bindings/js/` already holds a msgpack implementation
(`bindings/js/src/msgpack.js`) and the WASI glue (`bindings/js/src/wasi.js`),
so the JS side reuses the codec it already ships.

Two facts to carry into that work rather than discover in it:

- **Lab is not in this repository.** `doc/Lab.md` is a design brief and says so
  itself; there is no `lab/` directory, no Worker and no `postMessage` anywhere
  in the tree. What *is* here is the seam: `src/dvs_shim.c` declares
  `js_host_create` / `js_host_destroy` / `js_host_drive` as `env` imports and
  exports `dvsjs_new`, and `bindings/js/src/index.js` supplies that `env`.
- **That seam has never executed.** `bindings/js/test/swarm.integration.mjs`
  says as much in its own header, and CI runs only the instance integration
  test. Running it once is a prerequisite for a Lab plugin, not a follow-up.

And the reachable-URL sets will differ: a browser adds CORS, no control of the
`Host` header, and no client certificates. Both bindings implement one manifest
at the frame level; say so in the manifest rather than discovering it.

That the fixture proving this — `test/plugin_echo.c` — includes **nothing** from
this tree is the assertion that the manifest is a protocol and not a C header.
If a plugin ever needs a Diluvium header, that has stopped being true.

### 5.3 WASI — optional, and honestly bounded

Nice to have, blocking nothing. The build already targets WASI
(`Makefile:98-125`, via a digest-pinned `wasi-sdk` container), but **WASI
preview 1 has no subprocess spawn**, so an exec-based plugin cannot exist under
it. The options, in order of honesty:

1. A **preopened fd** supplied by the embedder, with the plugin run outside the
   sandbox by whoever launched wasmtime. Works today, moves the launch problem
   out of the module.
2. **wasi-sockets** / the component model, where the "plugin" is a component
   import. This is what §5.1's table means by *component import*, and it is a
   preview-2 story.
3. Nothing — the WASI build ships without plugins and says so.

Option 1 is what to attempt if there is time. Option 3 is the default and is
not a failure.

---

## 6. What we are NOT building

Named deferrals with reasons, not omissions.

| Deferred | Reason |
|---|---|
| `await` assertion syntax | Depends on the analyzer computing may-park verdicts, which consumes the manifest build 8 creates. Earliest build 9. |
| Streaming responses | Request/response is sufficient. The `final` flag ships now so the wire format does not break later. |
| Cancel frame | Only affects terminated instances, where the cost is a leaked plugin-side computation, not incorrect behavior. The `cancel` hook (§1.2) is the host-side half; the wire half waits. |
| Plugin authentication | Deliberate. §2.7. |
| Parallel instance execution | Separate project. Deferral delivers most of the practical win and is required for parallelism anyway. |
| Rust host | The submit/reply boundary defined here is the seam a Rust host would occupy. Nothing forecloses it. |
| Converting `exec` to the deferred seam | Part 1 makes it possible; doing it is a behavior change to a shipped connector and belongs in its own build. §7 gate 5 keeps it honest in the meantime. |

### Build 7 leftovers — in or out

From `doc/BUILD7.md` §5, each marked rather than left ambiguous:

| Leftover | Build 8? |
|---|---|
| Natural async / `await` keyword | **out** — already a named non-goal; build 8 is its second step |
| Inter-instance endpoint delivery | **out** — orthogonal |
| Listener routing to a non-root instance | **out** — orthogonal |
| `net` — outbound HTTP/TCP | **in, as a plugin** — §5. Strike it from §5 of BUILD7 rather than build it twice |
| Installer ships the host | **out** — packaging |
| `env`, `log` | **out** — unrelated connectors |

### Roadmap items this closes indirectly

- `doc/Capabilities.md` §8's *"the endgame is a natural `await`"* — build 8 is
  the shape that keyword slots into. On track, not re-specified.
- `doc/Messaging.md` §17's hostcall-blocking correction — build 8 is the first
  build to exercise it against a genuinely slow backend.

---

## 7. Acceptance gates

Hard. Nothing merges past a red one.

1. **The existing suite stays green.** `make test`, `make host_check`, the
   fuzzers, and the drift guard `script/check_source_lists.py`.
2. **A build7 snapshot restores on build8.** The fingerprint did not move; §8
   says why, and this gate is what proves the rule was followed.
3. **N parked instances against `slow`** (§1.5): no instance blocks another,
   and the host thread never blocks on a write. Written **before** the
   implementation.
4. **Pending-table leak check.** Instances terminated mid-call leave no entry;
   asserted by count, not by inspection.
5. **`exec` still behaves exactly as it does today.** Part 1 changes the seam,
   not the connector; the existing `exec_is_bounded_and_shell_free` case
   (`test/host_check.c:744`) passes unchanged.
6. **Hibernate with a call pending**, woken, across all three wake policies.
7. **A hostcall inside a `table.sort` comparator produces the legible error**,
   not a raw C-call boundary message. `dq_cannot_park` (`src/dqueue.c:465-471`)
   already names the contexts; the gate is that a hostcall reaches it and says
   so.
8. **`dv_restore` validation and fuzzing** — moved here from follow-up work, per
   the handoff's §8.3.
9. **A REST fixture the swarm can actually use**, on musl and in Lab, built
   before the implementation rather than after.
10. **Changelog build8 entry** written and consistent (`script/changelog.py`
    validate/consistency/check), mirroring how build7 was cut.

---

## 8. The snapshot boundary — deliberately not crossed

Build 7 crossed it because it added a guest global. **Build 8 does not, and
that is a constraint to hold rather than an accident to enjoy.**

The rule, from §0.4:

> Every guest-visible function build 8 adds is a **Lua closure inside the
> existing `host` table**. No new guest global. No new C function reachable
> from a guest table. No new named permanent.

Because `ds_perm_walk` names only C functions (`src/dsnap.c:194`) and the
fingerprint hashes the sorted name list (`src/dsnap.c:296`), that rule keeps
the fingerprint byte-identical — which matters more than a version bump would
suggest. If generated wrappers were named permanents, the fingerprint would
depend on **which plugins a deployment wires**, and two hosts with different
plugin sets could never exchange a snapshot. The rule is not a convenience; it
is what keeps a snapshot a property of the runtime rather than of the
deployment.

### 8.1 The sharp edge of that rule

The stability is real and so is its cost, and the cost is worth stating
plainly because it is silent. Since the fingerprint does not cover the `host`
table's *shape*, a snapshot taken on a deployment that wires a plugin will
**restore without complaint** on one that does not. `ds_hook_permanent` writes
the name `host`, and the decoder resolves that name against the *restoring*
runtime's own table (`src/dsnap.c:1113-1136`), erroring only if the name is
absent — which it never is. Anything the guest stashed below the top level
(`local get = host.rest.get`) is not a permanent, so it is content-copied: the
restored instance can hold one deployment's wrapper beside another's `host`
table, with no diagnostic anywhere.

Three options were weighed, and the choice is deliberate:

1. **Regenerate on restore** — what §3 plans. Keeps snapshots exchangeable
   between deployments, which is the whole reason for the rule.
2. **Name a per-deployment digest** via `diluvium_snap_addpermanent`
   (`src/dsnap.c:1779`, worked example at `src/dv.c:290-291`). Turns the silent
   divergence into a loud refusal — and makes the fingerprint a property of the
   *deployment* rather than of the runtime, which kills gate 7.2 and stops a
   build7 snapshot restoring at all.
3. **Ship no wrappers.** `host.call` already reaches any connector; zero
   snapshot exposure.

**Take (1).** What makes it safe rather than merely convenient is §3's
uniformity: the only per-capability datum in a generated wrapper is a name
string, so a stale wrapper calling `host.call("rest/get")` on a host that does
not wire `rest` is *denied by `dvs_holds`* — refused by name, never silently
misrouted. Option (2) stays available as a one-line upgrade if that ever stops
being true, which is why the manifest's checksum is logged from day one.

If an implementation path appears to require a new permanent, stop and raise it.
The same applies to the harder constraint above it: **no core-lua patches in
build 8.** Everything here is `lua_resume`, `lua_status`, `luaL_ref` and the
public C API, in Diluvium's own `d*.c` files. Upstream sync is preserved.

---

## 9. Carried-forward corrections — already applied

The handoff carried three items from an earlier source-level review. Checked
against the tree, **two are already fixed** and the third is already scheduled.
Recorded here so build 8 does not claim credit for work that shipped:

1. **`pcall` is yieldable.** Already correct in `doc/Messaging.md` §8.4 and in
   the code comment at `src/dqueue.c:459-462`, and listed as a first-draft
   correction in §17. Build 8 rests on it and needs to change nothing.
2. **Secure-function scrambling and snapshots.** Already covered by
   `doc/Messaging.md` §10.9, including the determinism requirement that
   content-addressed Protos depend on. Build 8 makes parking inside a secure
   function more common but introduces no new interaction.
3. **`dv_restore` validation and fuzzing.** Moved to an acceptance gate (§7.8),
   which is what the handoff asked for.
