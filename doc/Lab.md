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

3. **A REPL that runs at a breakpoint.** Once the debugger below can stop a program
   inside a hook, the hook is a place where the program's own frame is live and
   reachable. Evaluate there and locals, upvalues and the call stack are all in scope.
   This is the same machinery as §3 with a different front end, which is the argument
   for it: one mechanism, two features.

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

### 3.1 Call stack — essentially built

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

### 3.2 Variables — public API, with one thing to know

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

### 3.4 Pause and resume — the open question, and it is answerable

The claim to check first, because it decides the architecture: `doc/Messaging.md` §9.4
and `src/dv.h:359` say the budget hook **raises rather than yields**, because "a yield
from a hook puts a Lua frame under the yield with `CIST_HOOKYIELD` set and 10.7
refuses to hibernate that". That is a true statement about the *budget*. It is not a
statement that a hook cannot yield.

**A hook can yield, and this tree supports it.** `src/ldo.c:1008` (`lua_yieldk`) has an
explicit branch for it:

```c
if (isLua(ci)) {  /* inside a hook? */
  lua_assert(!isLuacode(ci));
  api_check(L, nresults == 0, "hooks cannot yield values");
  api_check(L, k == NULL, "hooks cannot continue after yielding");
}
```

and `luaG_traceexec` (`src/ldebug.c:971-976`) completes the mechanism: if the hook
left `L->status == LUA_YIELD`, it sets `CIST_HOOKYIELD` and throws `LUA_YIELD`. On the
next resume, `src/ldebug.c:952` sees the mark, clears it, and **does not call the hook
again at the same instruction** — so a resume from a breakpoint does not immediately
re-break on the same line. That is the behaviour you would otherwise have to build.

So the shape of a breakpoint is: the line hook matches, yields with zero values, and
control returns to whoever called `lua_resume` — the host, with the whole program
suspended exactly at the breakpoint and every frame intact.

Four consequences to design around:

1. **Zero values, no continuation.** The two `api_check`s above are hard requirements.
   So the breakpoint's payload — which line, which instance — cannot ride out on the
   yield; it goes in host-side state that the hook writes before yielding, the same
   way `dv_insn_hook` reaches the instance through the registry (`src/dv.c:205`).

2. **`dv_run` will not recognise it.** `dv_run` inspects what the program yielded and
   expects a wait-set; a hook yield produces nothing, and `src/dv.c:548` sets the
   error "the program yielded something that is not a wait-set". So the ABI needs a
   third outcome beside "finished" and "parked on queues" — a `DV_BREAK`, plus a
   `dv_continue` for resuming, since `dv_resume` is specified around answering a
   queue wait (`src/dv.c:636` refuses an instance that is not parked). This is the
   only genuinely new ABI surface any of this needs.

3. **A program stopped at a breakpoint cannot be hibernated**, and that is correct
   rather than a limitation to fix. `dshim.c:356` deliberately keeps `CIST_HOOKED` and
   `CIST_HOOKYIELD` out of its known-flags set, so `diluvium_shim_checkframes` refuses
   such a thread. A debugger session is not a state to persist.

4. **It costs the interrupt story a look.** Ctrl-C interrupting a runaway loop is
   asserted in both execution modes (`test/interrupt_check.sh`) and it also works
   through a hook. Two mechanisms in one hook slot is the §3.3 problem again.

An alternative worth pricing before committing: **an all-guest debugger.** The hook is
a Lua function; it serialises frames and locals with `debug.getinfo`/`getlocal` and
`msgpack`, pushes them to a queue, and blocks on `queue.wait` for the next command.
No new C, no ABI change, no hook-slot conflict with the budget (a Lua hook and a C
hook still share the slot — but the Lua one can call out to whatever the budget
needs). The debugger's protocol becomes ordinary messages, which is the same shape
everything else in this system uses, and `queue.wait` parking *is* snapshottable
where a hook yield is not. The cost is that `debug` must be available to the
instance — which it is today, and which
`doc/Messaging.md` §18.2 lists as something profile B wants to take away. **Read that
entry before choosing this path**: it is the last open forgery route, and a guest-side
debugger is an argument for narrowing the library rather than removing it.

---

## 4. What to settle first

In this order, because each answer constrains the next:

1. **Which binary carries the swarm layer** (§1). Everything else in `lab` sits on top
   of a host, and this decides whether `lab` is a target, a flag, or the default.
2. **Guest-side or host-side debugger** (§3.4). This is the fork. Guest-side needs no
   new ABI and keeps `debug` open for instances; host-side needs `DV_BREAK` and
   `dv_continue` and keeps the guest sandbox narrow.
3. **One hook slot, two users** (§3.3). Whichever way 2 goes, the budget must still
   fire, with a test that fails if it does not.
4. **What a REPL attaches to** (§2b). If the answer to 2 was host-side, option 3 —
   evaluate at a breakpoint with an `_ENV` proxy over `debug.getlocal` — gets a REPL
   nearly free, and is the reason to prefer it.

None of this needs `doc/Messaging.md`'s open defects fixed first. Profile A (§18.2) is
enough to build a lab on, and a lab does not hibernate.
