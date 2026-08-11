# `diluvium lab`: a design brief

Three capabilities are wanted, and none of them exists yet:

1. **A supervisor that spawns child programs**, driven from the command line.
2. **A REPL** — one that can be pointed at a live instance, not only at a fresh state.
3. **A debugger**: variables, call stack, breakpoints, and resume.

This document exists so a session can decide *how*, with the repository open, rather
than rediscovering the same five facts. It is not a plan and it does not claim any of
these are easy. Where something is genuinely open, it says which way the evidence
points and what would settle it.

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

### The same decision, again, for wasm

The three options above are about the *native* binary. The wasi target
needs the question asked separately, and it is the more urgent of the two
— because it has already been answered, by default, and nobody chose the
answer.

`libdiluvium_wasi.wasm` is linked from `onelua.o + wasm_stubs.o +
analyze.o + diluvium_api.o` (`Makefile:199`). `dvs.c` is in none of them.
So the browser build has no swarm layer, and that follows from this
section's first sentence rather than from anything anyone weighed.

It is worth deciding deliberately now, because the wasi artifact stopped
being a REPL toy at `5.5.1_build3`: it carries all 27 `dv_*` exports and
registers `queue`, `endpoint` and `msgpack` as guest globals. A browser
host can already run budgeted instances and drain queues. The swarm layer
is the *only* missing tier, and it is missing for a reason that is
nowhere written down.

**A wasm host cannot supply `dvs_host`'s three function pointers from
JavaScript.** In wasm a function pointer is a table index — the same
problem that produced `dv_endpoint_allow`, recorded under §13 with the
note that it "was found by writing the wasmtime binding". So the host has
to be C, compiled in, exactly as `test/dvs_check.c:102` already is.
Concretely, in `wasm_stubs.c` beside `init_lua` and `run_lua`:

```
swarm_start(const char *code)  -> dvs_new + dvs_root
swarm_step(void)               -> dvs_step; returns whether anything ran
swarm_next_event(void)         -> drain one system/events record as
                                  msgpack or JSON; the caller frees it
```

plus `dvs.c` on the wasi compile line. That is the same forty lines this
section already prices for the CLI, with a three-function door instead of
a command.

`doc/Messaging.md` §12.1 already plans a separate
`diluvium-swarm-<version>-<triple>.wasm`, which is option 1 in wasm form
and keeps §4.1's boundary intact. If that is the answer, say so here, so
that the browser build's silence stops reading as an oversight.

**There is a consumer waiting.** `diluvium-lab` renders `system/events`
records in §9.2's exact shape — `event`, `id`, `detail` — and on
`build3` it feeds that renderer from a real `queue.declare`/`push`/`pop`
loop. What it cannot do is make anything spawn. Whichever option is
chosen, the transport changes there and nothing else does.

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

1. **Which binary carries the swarm layer** (§1) — and, separately,
   **which wasm artifact does**. Same question, two platforms, and the
   wasm one has been answered by accident since before anyone asked it.
   Everything else in `lab` sits on top of a host, so this decides
   whether `lab` is a target, a flag, or the default — on each of the two
   platforms that now has a real consumer.

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

None of this needs `doc/Messaging.md`'s open defects fixed first. Profile A (§18.2) is
enough to build a lab on, and a lab does not hibernate.

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
