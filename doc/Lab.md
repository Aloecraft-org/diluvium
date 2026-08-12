# `diluvium lab`: a design brief

Three capabilities are wanted, and none of them exists yet:

1. **A supervisor that spawns child programs**, driven from the command line.
2. **A REPL** — one that can be pointed at a live instance, not only at a fresh state.
3. **A debugger**: variables, call stack, breakpoints, and resume.

This document exists so a session can decide *how*, with the repository open, rather
than rediscovering the same five facts. It is not a plan and it does not claim any of
these are easy. Where something is genuinely open, it says which way the evidence
points and what would settle it.

**Updated for 5.5.1_build5's preparation, and lab's role grew while this file
slept.** `doc/Host.md` now defines the host protocol, and lab is its *JavaScript
implementation* — the host-outside-module strategy over the new
`diluvium_swarm_wasi.wasm`, with the swarm ABI exported, the host vtable crossing
the boundary as `env` imports, and `setSwarmHost`/`instantiate` in the JS binding
as the seam. That recasts capability 1 below from "needs a host" to "the host
exists as a protocol; lab implements it in JS", adds a fourth capability this
brief predates — **hostcall connectors** (`doc/Hostcall.md`): mock in JS what
production wires natively, same guest either way, worked end to end for SQLite
in §1a below — and gives the panel and the
notebook-to-agents composer a contract to sit on. §1's wasm half is rewritten
below to record what was actually built, including where this file's prediction
was wrong. The debugger sections (§2b, §3) stand: nothing about hooks, yields or
the one-slot problem moved.

Everything asserted here about the core was checked against this tree at the line
cited. Where a claim is inherited from `doc/Messaging.md` rather than checked, it says
so — that document's §17 exists because the first draft of it was written against an
abstract Lua 5.5 and got five things wrong.

**Reading is not running, and this file has now had both.** The first draft of §3.4 was
verified at source level only, and when it was later verified by *execution* one of its
two options turned out to be impossible. Where a claim below has been run, it says so
and shows the output. Where it has only been read, it says that too — treat those as
the ones most likely to be wrong.

---

## 0. What exists to build on

| Piece | Where | What it gives you |
|---|---|---|
| The coroutine-hosted driver | `src/dtask.c` | Runs a chunk on a coroutine so it can yield. `--task` is its CLI entry. |
| The instance ABI | `src/dv.h`, `src/dv.c` | `dv_new`/`dv_load`/`dv_run`/`dv_resume`, queues by name, budgets, snapshot. One sandboxed program, no Lua header needed. |
| The swarm layer | `src/dvs.h`, `src/dvs.c` | Instance table, parentage, capability attenuation, subtree kill, spawn rate limit, snapshot cache. **A supervisor is a program**, not a type. |
| REPL support | `src/drepl.h`, `src/drepl.c` | `diluvium_repl_load` (ok / incomplete / error, and expression-first so `1 + 1` prints) and `diluvium_repl_complete`. Front-end-agnostic. |
| Line editing | `src/dline.c` | The CLI's own editor, driving `diluvium_repl_complete`. |
| The internals shim | `src/dshim.h`, `src/dshim.c` | Call frames flattened to plain data — `func_index`, `pc`, `callstatus`, continuations — plus stack slots and the to-be-closed chain. **The one file allowed to read Lua's internal headers.** |
| Argument parsing | `src/lua.c:427`, `:437` | `has_task` and `collectargs`. `--task` is currently the only long option, and the code says so in a comment that a second one invalidates. |

Two things that do *not* exist and are easy to assume: there is no `lab` anywhere in
the tree (the string appears in no source file), and there is no debugger, no
breakpoint machinery, and no protocol for talking to a running instance.

---

## 1. A supervisor that spawns children

**This is built. It needs a host and a command, not a feature.**

`dvs.h` is the whole mechanism: `dvs_new`, `dvs_root`, `dvs_step`, `dvs_kill`,
`dvs_push`. A program pushes `{op = "spawn", code = ..., caps = {...}, budget = {...}}`
to its `system/lifecycle` queue and reads `system/events` for what happened.
Capabilities attenuate — a child can never be granted more than its parent holds —
subtree kill is the default, and spawns are rate-limited per step.

What is missing is a **host**: `dvs_host` has three function pointers, and the swarm
layer cannot spawn anything by itself because spawning means something different in
every environment. A single-threaded host implements `create` as a no-op and `drive`
as one `dv_run` or `dv_resume`. That is not a stub — `test/dvs_check.c` is exactly
that host, and 114 checks run through it. **Copy it.** `host_create`, `host_destroy`
and `host_drive` at `test/dvs_check.c:102` are about forty lines together.

So `diluvium lab` is, at its smallest:

```
dvs_new(&host, max, rate) → dvs_root(code) → while (dvs_step(sw)) { }
```

plus printing `system/events` as it drains, which is what makes it a lab rather than
a batch runner.

### The one real decision: which binary

`dvs.c` is deliberately not in the amalgamation. `.data/onelua.c` does not include it,
and `make build_swarm_lib` compiles it alone and then checks at the *linker* that no
`lua` symbol appears in `dvs.o` — that is how §4.1's layer boundary is enforced, and
it is a stronger check than compiling without `lua.h`.

Linking `dvs.o` into the CLI does not break that check. But it does mean the default
`diluvium` binary carries the swarm layer, which §4.1 says an app embedding a single
sandbox should never pay for. Three options, in the order I would consider them:

1. **A separate `diluvium-lab` binary** linking `libdiluvium-swarm.a`. Honest about
   the boundary, adds a target and an artifact, and fits whatever full-versus-slim
   split the packaging story lands on.
2. **In the main binary, behind a compile flag** (`-DDILUVIUM_LAB`). One artifact,
   and the flag is a place for the boundary to erode.
3. **In the main binary unconditionally.** Simplest, and it makes §4.1's claim about
   the CLI false. If you choose this, correct §4.1 in the same commit rather than
   leaving the document ahead of the tree.

**Since `doc/Host.md`, option 1 has a name and a bigger job.** The plan of record
is a *generic host* — one binary implementing the host protocol from
configuration plus a supervisor program, native and compiled to wasm32-wasi,
replacing the bespoke per-deployment C host each deployment used to hand-write,
rather than adding a lab-only sibling. So the question here is no longer "which binary
carries the swarm layer for lab" but "is lab's native form anything other than
the generic host with a REPL and a renderer attached" — and the current answer
is no: build the generic host second, after its protocol has been exercised by
lab's JS implementation, and let `diluvium lab`'s native story be that binary.

### The same decision, again, for wasm — decided, built, and predicted wrong

What this section used to say: the browser build had no swarm layer by
accident, "a wasm host cannot supply `dvs_host`'s three function pointers
from JavaScript", *so the host has to be C, compiled in*, behind a
three-function door (`swarm_start`/`swarm_step`/`swarm_next_event`).

The first half was right and is fixed: `dvs.c` is now in both wasm
archives, and `diluvium_swarm_wasi.wasm` is the separate artifact §12.1
anticipated — separate for a harder reason than the layer boundary, as it
turned out, because the shim's `env` imports are *mandatory*, and linked
into `diluvium_wasi.wasm` they would have broken `wasmtime
diluvium_wasi.wasm` and every other pure-WASI consumer.

The second half was wrong in the way worth recording. The premise held —
a JS host indeed cannot make a C function pointer — but "the host has to
be C" did not follow. `src/dvs_shim.c` inverts it: the *trampolines* are
C, twenty lines, declared as imports from module `"env"`
(`js_host_create` / `js_host_destroy` / `js_host_drive`, the
`_diluvium_write` pattern), and `dvsjs_new` stands in for `dvs_new`. The
host — the actual duties: drive, roster, pump — is JavaScript, supplied
at instantiation and installed with `setSwarmHost`. So the decision this
section asked for came out *neither* of the ways it offered: not a C host
compiled in, not a swarm-less browser build, but the host protocol
(`doc/Host.md`) implemented in JS over a C seam. The dedicated door this
section sketched was never built and is not needed; the swarm ABI is
exported whole (`--export-all`), and `bindings/js/test/swarm.integration.mjs`
is the proof-of-life — deliberately not in CI until it has passed once,
per the `verify_wasm` lesson.

**There is a consumer waiting.** `diluvium-lab` renders `system/events`
records in §9.2's exact shape — `event`, `id`, `detail` — and on
`build3` it feeds that renderer from a real `queue.declare`/`push`/`pop`
loop. What it could not do was make anything spawn; with the swarm module
and `setSwarmHost`, now it can. The panel's queries are `doc/Host.md`
duty 3, with the one agreed stub: a hibernated instance shows its budget
and cached size but no usage figure, until the swarm API learns to read
the count from the snapshot header.

### Where the command goes

`collectargs` (`src/lua.c:437`) switches on `argv[i][1]`, and its `-` case carries a
comment saying `--task` is the only long option — so a second long option means
editing that comment as well as the code. A *subcommand* (`diluvium lab`, no dashes)
is a different shape: `collectargs` treats the first non-`-` argument as the script
name (`src/lua.c:450`), so `lab` would be taken as a filename. Decide deliberately
between `--lab` (fits the existing parser) and `lab` (fits `diluvium serve`, which the
same discussions want) — and if it is the second, the dispatch has to happen before
`collectargs`, not inside it.

---

## 1a. Connectors in the JS host — SQLite, worked

**Read, not run: the wasm module this needs is built in the wasi-sdk
container, so none of the code below has executed here. The C connector it
mirrors (`host/dhost_sql.c`) has, under `make host_check`, and the
confinement table names exactly where the JS side cannot follow it.**

`doc/Hostcall.md`'s fourth capability is a connector: the guest pushes
`{tok, call, args}` to an exported `host/calls` queue and waits on
`host/replies`; the host drains the first, answers, and pushes
`{tok, status, value|detail}` to the second. `host/dhost.c` is the reference
pump (`pump_instance` + `answer`); a JS host is the same seven steps in
JavaScript, over the swarm module's exported `dv_*`/`dvs_*` surface. Nothing
in the guest can tell the two hosts apart, which is the acceptance test.

### The pump, in JS

The swarm host installed with `setSwarmHost` already keeps a roster — it
must, because `create`/`destroy` are where a host learns which instances
exist (§1, and `host_create` in `dhost.c`). Give each turn of the lab loop a
second pass, after `dvs_step`, that pumps every rostered instance:

```js
const CALLS = "host/calls", REPLIES = "host/replies";
const roster = new Set();               // filled by create/destroy callbacks

// A guest inst pointer -> queue helpers. The malloc/peek/release dance is
// index.js's popRaw/pushRaw, unwrapped for a swarm-provided pointer.
function queueId(ex, inst, name) {
  const b = new TextEncoder().encode(name);
  const p = ex.malloc(b.length + 1);
  new Uint8Array(ex.memory.buffer, p, b.length + 1).set([...b, 0]);
  try { return ex.dv_queue_lookup(inst, p); } finally { ex.free(p); }
}

function pump(ex, sw, id, connectors) {
  const inst = ex.dvs_instance(sw, id);
  if (inst === 0) return;               // hibernated: its calls wait with it
  const calls = queueId(ex, inst, CALLS);
  if (calls === 0) return;              // this program declares no calls queue
  const replies = queueId(ex, inst, REPLIES);
  for (;;) {
    // Backpressure first: never drain a request we cannot answer, or a
    // stateful connector re-runs on the retry (dhost.c's pump_instance).
    if (replies !== 0 && queueFull(ex, inst, replies)) break;
    const req = peekRaw(ex, inst, calls);   // decode() of the peeked bytes
    if (req === undefined) break;
    const reply = answer(ex, sw, id, req, connectors);
    releaseRaw(ex, inst, calls);
    if (replies !== 0) pushBytes(ex, inst, replies, encode(reply));
  }
}
```

`answer` is the routing and the gate, and it is the part a prototype must not
shortcut, because it is the whole security model:

```js
function answer(ex, sw, id, req, connectors) {
  const { tok, call, args } = req;
  if (typeof tok !== "number" || typeof call !== "string")
    return { tok, status: "malformed", detail: "tok and call are required" };
  // The capability check is the runtime's, not the host's: "host:" + call,
  // against this instance's grant. A connector never sees a call the caller
  // does not hold -- attenuation through spawns is already applied.
  if (!holds(ex, sw, id, "host:" + call))
    return { tok, status: "denied", detail: `does not hold host:${call}` };
  const conn = connectors[call.split("/")[0]];   // route on the prefix
  if (!conn) return { tok, status: "denied", detail: `no connector for ${call}` };
  try {
    return { tok, status: "ok", value: conn(call, args) };
  } catch (e) {
    return { tok, status: "error", detail: String(e.message ?? e) };
  }
}

function holds(ex, sw, id, cap) {
  const b = new TextEncoder().encode(cap);
  const p = ex.malloc(b.length + 1);
  new Uint8Array(ex.memory.buffer, p, b.length + 1).set([...b, 0]);
  try { return ex.dvs_holds(sw, id, p) === 1; } finally { ex.free(p); }
}
```

Two rules from `doc/Hostcall.md` that a prototype gets wrong by omission:
every drained request is answered (denied, error and malformed included — a
dropped request is invisible backpressure), and the correlation `tok` is
echoed on every reply from the very first connector, JS mocks included.

### `sqlConnector(db)` — the factory

A connector is just the `(call, args) => value` function `answer` routes to.
For SQLite in Node, `db` is a `node:sqlite` `DatabaseSync`; in the browser it
is a `sql.js` `Database`. The factory closes over it and answers the two
calls `host:sql/*` splits into:

```js
// db.prepare(sql) -> stmt with .all(...params) / .run(...params); adjust the
// two method names if your driver differs (node:sqlite and better-sqlite3
// match; sql.js uses db.exec / stmt.getAsObject and needs a thin adapter).
export function sqlConnector(db, { readwrite = false, maxRows = 1024 } = {}) {
  return (call, args = {}) => {
    const isExec = call === "sql/exec";
    if (call !== "sql/query" && !isExec)
      throw new Error(`the sql connector answers sql/query and sql/exec`);
    if (isExec && !readwrite)
      throw new Error(`this database is read-only; sql/exec is not wired`);

    const sql = args.sql;
    if (typeof sql !== "string") throw new Error("args.sql must be a string");
    if (sql.includes("\0")) throw new Error("the statement has an embedded NUL");
    guardOneStatement(sql);             // see the checklist: text-level here
    const params = args.params ?? [];

    const stmt = db.prepare(sql);
    if (!isExec && stmt.reader === false) // node:sqlite exposes .reader; else
      throw new Error("sql/query is for reads; this writes (sql/exec)");
    // param count must match exactly -- too few silently NULL-binds the rest,
    // the same truncation the row cap refuses (dhost_sql.c bind_params).
    guardParamCount(stmt, params);

    if (isExec) {
      const r = stmt.run(...params);
      return { changes: Number(r.changes), rowid: Number(r.lastInsertRowid) };
    }
    const rows = stmt.all(...params);
    if (rows.length > maxRows)
      throw new Error(`result passed the ${maxRows}-row cap; page with LIMIT`);
    const cols = rows.length ? Object.keys(rows[0]) : columnNames(stmt);
    return { cols, rows: rows.map((r) => cols.map((c) => r[c])) };
  };
}
```

Wire it once the DB is open:
`pump(ex, sw, rootId, { sql: sqlConnector(db, { readwrite: false }) })`.

### The confinement checklist — and the one line the JS host cannot copy

`host/dhost_sql.c` earns its confinement from three SQLite primitives that
`node:sqlite` and `sql.js` **do not expose**. That is the prototype's real
ceiling, and the reason the JS SQL path is "fine for prototyping" and not
for production. Each row is a thing to verify, not assume:

| Confinement | How C does it | JS host |
|---|---|---|
| No ATTACH / DETACH | `sqlite3_set_authorizer` DENY + `SQLITE_LIMIT_ATTACHED = 0` | **No authorizer in the JS drivers.** Text-gate: reject a statement whose first keyword is `ATTACH`/`DETACH`. Weaker — the C comment explains why a regex is the wrong tool, and it is; this is the floor, not the target. |
| No BEGIN / COMMIT / SAVEPOINT (no cross-call transaction on a shared handle) | authorizer DENY on `SQLITE_TRANSACTION`/`SAVEPOINT` | Same text-gate. Also give **each guest its own connection** if you can, so a leaked transaction cannot span guests. |
| No PRAGMA | authorizer DENY on `SQLITE_PRAGMA` | Text-gate `PRAGMA`. (Some are connection-state changes — `writable_schema`, `foreign_keys` — so this is not cosmetic.) |
| query vs exec split | `sqlite3_stmt_readonly` after prepare — SQLite's own classification | `stmt.reader` on `node:sqlite` is the honest equivalent; if your driver lacks it, the split degrades to the text-gate and is weaker. |
| One statement per call | `sqlite3_prepare_v2` returns a `tail`; refuse non-whitespace after it | The JS drivers prepare **one** statement and ignore the rest — so you must reject trailing text yourself, or a second statement rides in unauthorized and silently unrun. |
| Extension loading off | `sqlite3_db_config(ENABLE_LOAD_EXTENSION, 0)` | Off by default in both drivers; do not turn it on. |
| Read-only means read-only | `sqlite3_open_v2(..., SQLITE_OPEN_READONLY)` | Open the file read-only (`node:sqlite`: `{ readOnly: true }`). `sql.js` runs from a byte image with no file to protect, so "read-only" there is only the query/exec split — note the difference. |
| Row cap refuses, never truncates | count while stepping, refuse past `max_rows` | `rows.length > maxRows` **after** `.all()` — the whole result already materialized in memory. For a real cap, append `LIMIT maxRows+1` and check, or step a cursor; `.all()` past a hostile `max_rows` is a memory DoS the C host does not have. |

The takeaway a lab session needs before it starts: **the JS SQL connector
enforces the same *contract* as the C one — same two calls, same reply
shapes, same capability names — but not the same *confinement*, because the
authorizer has no JavaScript.** Build to the contract so the guest cannot
tell, gate the escapes as well as the text allows, give each guest its own
connection, and do not point it at a database that matters until the native
generic host (`host/dhost_sql.c`, already built) is what runs in production.

---

## 2. A REPL

Two very different features share the word, and conflating them is the main hazard
here.

### 2a. A REPL over a fresh state — already shipped

`src/lua.c` already has one (`-i`), built on `diluvium_repl_load`. Nothing to do.

### 2b. A REPL *inside a live instance* — a real design question

This is what makes a lab: attach to an agent that is parked on its inbox and evaluate
an expression in its world. The problem is not compiling the line; it is *when* it
runs and *what it can see*.

The runtime has no notion of "run this now, out of band". A parked instance is
suspended inside `queue.wait`, and the only way to make it run is `dv_resume`, which
resumes the program's own continuation. There is no seam for injecting a chunk.

Three mechanisms, and the third is the one I would build:

1. **`lua_State` surgery from the host.** Push a chunk onto the instance's `L` and
   call it while the guest coroutine is suspended. It works — `inst->L` and `inst->co`
   are separate states and the guest is parked on `co` — but the evaluated chunk sees
   globals and not the parked frame's locals, which is most of what someone wants a
   REPL for. It also needs a new `dv_` entry point and it bypasses the instruction
   budget, which is armed on `co` only (`src/dv.c:596`).

2. **A REPL agent in the program.** A supervisor spawns a child whose whole job is to
   read source from a queue, `load` it, run it, and push the result back. Zero new C.
   It sees only its *own* state, so it is a REPL *next to* the agent rather than
   inside it — useful for poking at the swarm, useless for inspecting a stuck handler.

3. **A REPL that runs at a breakpoint.** The hook is the one place where the program's
   own frame is live and reachable, so an expression evaluated there sees locals,
   upvalues and the call stack. §3.4 confirms by execution that stopping there works
   and that the frame is readable, which makes this the strongest option — but it is
   not as free as the first draft of this file claimed, and the reason is worth knowing
   before you commit to it.

   **Evaluation has to happen inside the hook, and the hook has already returned by the
   time the host has a command.** The break arrives as a yield, so control is outside
   the coroutine; and a suspended coroutine cannot be called into — `lua_resume` is the
   only door. So "evaluate this expression at the current breakpoint" cannot be a call
   that reaches in. It has to be: store the pending source in instance state, resume in
   a mode that re-enters the hook immediately, let the hook compile and run it against
   the live frame, stash the result, and yield again.

   Re-entering immediately is the fiddly part, because `CIST_HOOKYIELD` deliberately
   suppresses a second call at the same instruction — that is what stops a breakpoint
   looping, and here it works against you. A `LUA_MASKCOUNT` hook with a count of 1
   fires on the next instruction regardless, which is the usual way round it. **Not yet
   run**; unlike the rest of §3.4 this is reasoning, and it is the first thing to
   prototype if this path is chosen.

A note on the trade-off in option 3, so it is chosen rather than discovered: a chunk
compiled at a breakpoint cannot see the parked frame's locals *by name* without help,
because a compiled chunk resolves names to globals and upvalues, not to another
frame's registers. The standard trick is to compile the line as a function and set its
`_ENV` to a proxy table whose `__index`/`__newindex` walk `debug.getlocal` and
`debug.getupvalue` on the target level. That is ordinary Lua and needs no C at all —
which makes it a strong argument for building the debugger's protocol in the guest.

---

## 3. A debugger

Four capabilities, in increasing order of difficulty. The honest headline: **three of
the four are close to free, and the fourth — pause and resume — is possible in this
tree, which `doc/Messaging.md` §9.4 can be read as denying.** That reading is about
the *budget* hook and does not generalize; see §3.3.

### 3.1 Call stack — essentially built, and demonstrated in §3.4

`dshim.h` already flattens a coroutine's call chain to plain data: frame count, and
per frame `is_c`, `has_k`, `func_index`, `top_index`, `pc`, `callstatus`,
`nresults`, `is_vararg`, `nextraargs`, and the continuation pointer. `pc` is an offset
into the prototype's code, which is what you need for a line number. It exists because
hibernation needs it, and a debugger wants the same information.

What it does not give you is a *line* or a *name*, and it should not: that is
`lua_getinfo`, public API, which works on another thread through
`lua_getstack(co, level, &ar)`. So the call stack is:

- names and lines: `lua_getstack` + `lua_getinfo` on `inst->co` — public API, no shim.
- anything the public API cannot answer (whether a frame is a `CIST_YPCALL` driver
  frame, whether a C frame has a continuation): `diluvium_shim_frame`.

Note the two count in opposite directions. `diluvium_shim_frame` takes `i` from 0 at
the *outermost* frame (`src/dshim.h:83`); `lua_getstack` takes level 0 as the
*innermost*. Getting this wrong produces a plausible stack in the wrong order, so
write the conversion once and test it against a known chain.

### 3.2 Variables — public API, demonstrated in §3.4, with two things to know

`lua_getlocal`, `lua_setlocal`, `lua_getupvalue`, `lua_setupvalue` all take an
explicit `lua_State *`, so they work on a suspended coroutine from the host. Negative
indices to `lua_getlocal` give varargs. `debug.getinfo(f, "u")` gives the upvalue
count.

The thing to know: **a suspended coroutine's frames are readable, but only its
`savedpc` is meaningful, not its live registers-in-flight.** A frame suspended in the
middle of an expression has intermediate values above its named locals, and
`lua_getlocal` will report a temporary under a name that has gone out of scope — this
is the same hazard `dshim.c` documents for `nextraargs` (`src/dshim.h:53`), where a
recycled `CallInfo` field held whatever the last user left there. Do not present a
value as a named local unless `lua_getlocal` returned a name for it.

`diluvium_shim_pushslot` reads a raw stack slot onto another state's stack, which is
the escape hatch when you want the whole activation record rather than the named part.

And the second thing: `lua_getlocal` returns the VM's internal locals alongside the
program's, named `(for state)`, `(temporary)` and so on. §3.4's transcript shows three
of them around one loop variable. Filter names beginning with `(` in the ABI, not in
each front end.

### 3.3 Breakpoints — `lua_sethook` with `LUA_MASKLINE`

`lua_sethook(co, hook, LUA_MASKLINE, 0)` fires the hook on every new line, and
`ar->currentline` after `lua_getinfo(L, "l", &ar)` gives it. A breakpoint set is a
lookup keyed by `(source, line)` and consulted in the hook — a hash set, and
`src/dhash.c` is already in the tree and self-contained.

The cost is real and should be stated up front: a line hook fires on **every line**,
so it is not something to leave armed in production. `doc/Messaging.md` §9.4's
instruction budget uses `LUA_MASKCOUNT` with a 1000-instruction step
(`DV_HOOK_STEP`, `src/dv.c:194`) precisely to keep the cost down. A debugger that is
only armed while debugging pays whatever it pays.

**One interaction to design around, not discover:** `src/dv.c:596` arms the count hook
for the instruction budget, and `lua_sethook` takes *one* hook and *one* mask per
thread. A debug hook installed naively **replaces the budget hook and silently
disables the budget** — the exact class of defect the audit in
`doc/audit/M0-M7.md` found twice (findings 1 and 2: a budget that is stored, reported
by an accessor, and enforced by nothing). Whatever is built here must either install
one hook with `LUA_MASKCOUNT | LUA_MASKLINE` that dispatches to both, or refuse to
attach a debugger to a budgeted instance and say why. Write the test that budgets an
instance, attaches a debugger, and asserts the budget still fires.

### 3.4 Pause and resume — verified by running it

The claim to check first, because it decides the architecture: `doc/Messaging.md` §9.4
and `src/dv.h:359` say the budget hook **raises rather than yields**, because "a yield
from a hook puts a Lua frame under the yield with `CIST_HOOKYIELD` set and 10.7
refuses to hibernate that". That is a true statement about the *budget*. It is not a
statement that a hook cannot yield.

**A C hook can yield, and a breakpoint therefore works. Run, not merely read.**
`src/ldo.c:1008` (`lua_yieldk`) has the branch:

```c
if (isLua(ci)) {  /* inside a hook? */
  lua_assert(!isLuacode(ci));
  api_check(L, nresults == 0, "hooks cannot yield values");
  api_check(L, k == NULL, "hooks cannot continue after yielding");
}
```

and `luaG_traceexec` (`src/ldebug.c:971-976`) completes it: if the hook left
`L->status == LUA_YIELD`, it sets `CIST_HOOKYIELD` and throws `LUA_YIELD`. On the next
resume, `src/ldebug.c:952` sees the mark, clears it, and does not call the hook again
at the same instruction.

A `LUA_MASKLINE` hook that calls `lua_yield(L, 0)` on a chosen line, against a
three-iteration loop, produced exactly this:

```
resume -> LUA_YIELD (nres=0)
    frame 0: [string "local acc = 0..."]:3 in main
    local 1: acc = 0
    local 2: (for state) = 2
    local 3: (for state) = 1
    local 4: i = 1
resume -> LUA_YIELD (nres=0)          <- acc = 1, i = 2
resume -> LUA_YIELD (nres=0)          <- acc = 3, i = 3
resume -> LUA_OK (nres=1)
final result: 6  (breaks hit: 3)
```

Four things that transcript settles, none of which a reading would have:

- The program **stops at the line, three times, once per iteration** — not once, and
  not forever. `CIST_HOOKYIELD` really does suppress the re-fire.
- The parked coroutine is **fully inspectable from outside**: `lua_getstack` +
  `lua_getinfo` give the source and line, and `lua_getlocal` gives locals *by name*
  with correct values at each stop (`acc` running 0, 1, 3 — the sum before adding `i`).
  So "variables" and "call stack" need no new machinery at all; they are public API
  against a suspended thread.
- **Resume is clean.** The program returned `6`, the right answer, after being stopped
  three times mid-loop.
- Internal locals appear alongside real ones as `(for state)`, `(temporary)` and so
  on. A debugger must filter names beginning with `(` or it will show the user the VM's
  bookkeeping.

**The all-guest debugger does not work, and this is the correction.** The first draft of
this file offered it as "an alternative worth pricing: no new C, no ABI change" — a
Lua hook that serialises frames to a queue and blocks for the next command. **A Lua
hook cannot yield.** Both routes out were run and both fail:

```
-- hook calls coroutine.yield()
resume -> false   attempt to yield across a C-call boundary

-- hook calls a blocking queue.wait, under --task
blocking queue.wait from inside a Lua hook -> ok=false
  err=queue: cannot wait on a queue here -- this code is not yieldable. That happens
  inside a table.sort comparator, a string.gsub replacement, a __gc finaliser, a
  metamethod called from C, or on the main thread when the program was not started by
  a host that resumes it (try --task).
```

The runtime's own diagnostic names the case. The privilege in `lua_yieldk` is for a
hook whose `CallInfo` is not Lua *code* — a C hook — and calling a Lua function as the
hook puts an ordinary Lua frame in the way. So a guest-side hook can **trace**: observe
lines, read its own locals with `debug.getlocal`, and push a record to a queue. It
cannot **pause**. That is a tracer, which is worth having and is about twenty lines of
Lua, but it is not a debugger and it does not remove the need for C.

**And as of `5.5.1_build4` a guest cannot even trace without being allowed to.**
`debug.sethook` is one of the twelve functions narrowed out of an instance, and the
reason given in `src/dlibs.c` is §3.3's hazard exactly: "a `lua_State` has one hook slot
and 9.4's instruction budget is in it, so setting a hook here would switch the budget
off". So the tracer needs `DV_FLAG_UNSAFE_DEBUG`, which is the right shape — a program
being traced is a program you own — but it means the tracer is a debugging *mode* rather
than something an agent can do to itself in production. The upside: §3.3's hazard is now
half-solved for free, since no guest can take the slot.

**And a host cannot install a hook at all through the public ABI.** `src/dv.h` contains
zero occurrences of `lua_State` — deliberately, since that self-containment is the
header's central claim and the contract tests exist to keep it true. There is no
accessor for `inst->co`, so `lua_sethook` is not reachable from outside `libdiluvium`.

That settles where this code lives: **inside the runtime library, alongside
`dv_snapshot`, not in a host on top of it.** The surface it needs, which is larger than
the first draft's "a `DV_BREAK` and a `dv_continue`":

| Needed | Why |
|---|---|
| `dv_break_at(inst, source, line)` / `dv_break_clear` | The host cannot reach `lua_sethook`, so arming is an ABI call. Keep the breakpoint set inside; `src/dhash.c` is already in the tree and self-contained. |
| `DV_BREAK` as a `dv_run`/`dv_resume` status | `dv_run` inspects what the program yielded and expects a wait-set; a hook yield produces nothing, so today `src/dv.c:548` reports "the program yielded something that is not a wait-set". |
| `dv_continue(inst)` | `dv_resume` is specified around answering a queue wait and refuses an instance that is not parked (`src/dv.c:636`). |
| `dv_frame_count` / `dv_frame_info` / `dv_local` | `dshim` already flattens frames, but locals need `lua_getlocal` against the coroutine, which no exported call reaches. Filter `(`-prefixed names here rather than in every front end. |
| Where the break happened | The yield carries nothing — `nresults == 0` and `k == NULL` are hard `api_check`s — so the hook must write the line into instance state before yielding, the way `dv_insn_hook` reaches the instance through the registry (`src/dv.c:205`). |

Two further consequences, both **read rather than run**, and flagged as such:

- **A program stopped at a breakpoint cannot be hibernated**, and that is right rather
  than a limitation to fix. `dshim.c:356` deliberately keeps `CIST_HOOKED` and
  `CIST_HOOKYIELD` out of its known-flags set, so `diluvium_shim_checkframes` refuses
  such a thread. A debugger session is not a state to persist. (Not exercised: it needs
  a snapshot attempted at a breakpoint, which needs the ABI above.)
- **One hook slot, still.** §3.3's problem does not go away: whatever arms the line
  hook must keep the budget's count hook firing, or budgets silently stop working.

## 4. What to settle first

In this order, because each answer constrains the next:

1. ~~**Which binary carries the swarm layer** (§1) — and, separately,
   **which wasm artifact does**.~~ **Both answered.** The wasm artifact is
   `diluvium_swarm_wasi.wasm`, deliberately separate, with the host in
   JavaScript over the `env`-import seam — see §1's correction for why the
   mechanism is not the one this file predicted. The native binary is the
   generic host of `doc/Host.md`, built second, once lab's JS host has
   exercised the protocol. What remains of this item is execution order,
   not a decision.

2. ~~**Guest-side or host-side debugger.**~~ **Settled by §3.4, and not by preference:**
   a Lua hook cannot yield, and `dv.h` exposes no `lua_State`, so the debugger lives
   *inside* `libdiluvium` alongside `dv_snapshot` and grows the ABI. The guest-side
   option survives only as a **tracer** — observe and report, never pause — which is
   twenty lines of Lua and worth having on its own.

3. **The breakpoint ABI**, as tabulated in §3.4: arming, a `DV_BREAK` status,
   `dv_continue`, and frame/local readers. Smaller than it looks, because the reading
   half is public Lua API against a suspended thread and already works.

4. **One hook slot, two users** (§3.3). The budget's count hook must still fire once a
   line hook is armed. Write the test that budgets an instance, attaches a debugger,
   and asserts the budget still fires — this is the same shape as the audit's finding
   that a budget could be stored, reported by an accessor, and enforced by nothing.

5. **Whether the REPL is the breakpoint's front end** (§2b option 3). Prototype the
   re-entry trick first; it is the only unproven step, and if it does not work the REPL
   and the debugger stop being one mechanism.

When this file said "none of this needs `doc/Messaging.md`'s open defects fixed
first — profile A is enough to build a lab on, and a lab does not hibernate", it
was hedging against a block that has since closed. Every audit finding is fixed,
hibernation is on by default, and a lab now *does* touch it: the swarm panel
shows hibernated instances (budget and cached size; usage stubbed per
`doc/Host.md` duty 3), and a lab that persists a session across a browser reload
would be the first real consumer of host-owned snapshot bytes. The debugger
half is unchanged by all of that — a program stopped at a breakpoint still
cannot hibernate, and that remains right rather than a limitation to fix.

Two contracts now bound what lab builds, and both were written before any lab
code exists so that lab cannot bake in their absence: `doc/Host.md` (the duties,
the roster-from-callbacks pattern, the panel queries) and `doc/Hostcall.md` (the
request/reply encoding — the correlation token is required from the *first*
prototyped connector, JS mocks included; a token-less prototype is the mistake
that document exists to prevent). The notebook-to-agents composer is lab
tooling with one runtime-imposed rule: agent functions must take their state as
a parameter and capture nothing, because a spawn ships source or bytecode, never
a closure.

## 5. How the claims here were checked

Two throwaway programs, both run against this tree:

- A C program installing a `LUA_MASKLINE` hook that calls `lua_yield(L, 0)` on one
  line of a three-iteration loop, driving it with `lua_resume` and inspecting the
  parked coroutine with `lua_getstack` / `lua_getinfo` / `lua_getlocal` between stops.
  Output is quoted in §3.4.
- A Lua program doing the same thing from the guest side, via `debug.sethook`, trying
  first `coroutine.yield` and then a blocking `queue.wait` under `--task`. Both refused;
  the errors are quoted in §3.4.

Neither is in the tree, because neither tests Diluvium — they test what Lua permits, and
the answers are now written down here. Anything in this file *not* marked as run is a
source-level reading, and §3.4 is the standing argument for why that is a weaker thing:
the all-guest debugger read perfectly well and does not work.
