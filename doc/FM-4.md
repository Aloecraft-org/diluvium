# FM-4, from this side: what closing it takes, measured

**Audience:** whoever decides the `CORE_PATCH_ALLOWLIST` question that
`5.5.1_build12p1`'s changelog and DRT's
[`doc/Failure-Modes.md`](https://github.com/Aloecraft-org/diluvium-drt/blob/main/doc/Failure-Modes.md)
both defer to this repository. Written here because this is where the fix
goes and where the evidence was collected. DRT's FM-4 entry is the
operational view and stays theirs.

Tree measured: `4debee4`, which carries build12p1's `dv.c` (hook left armed).
Everything below was run, not inferred; the probe and the two prototype
diffs are in `doc/attic/` so it can be re-run rather than trusted.

## The short version

- **FM-4 as DRT states it — a `pcall` loop that catches the budget error
  forever — is one of six ways a guest reaches the same place**, and three
  of the other five are worse than the named one. A one-line finalizer hangs
  the deployment with `dv_usage` reading `0` and `dv_exceeded` saying no.
  The same finalizer hangs `dv_free` of a program that *finished cleanly*.
  A one-line `xpcall` message handler hangs with the count frozen exactly at
  the limit. None of the three needs a loop. All measured in §1.
- **The fix the changelog named — "`pcall` refusing to catch, a core-file
  patch in `lbaselib.c`" — is the wrong file, and taken literally it closes
  one door of six.** The catch machinery is `ldo.c` (two sites) and the
  finalizer runner is `lgc.c` (two sites): about 20 lines of core plus ~30
  on-top in `dv.c` and `dshim.c`. The `pcall` half has a no-core-patch form
  (the technique that narrowed `debug`); the finalizer half has none, so
  `lgc.c` enters the allowlist either way. Recommendation: the
  `ldo.c`+`lgc.c` shape (§3).
- **Prototyped both shapes on this tree.** Every VM-side door returns
  `DV_ERROR` at *exactly* the budget, zero overrun, in single-digit
  milliseconds; `dv_free` is bounded; the five C suites pass except the one
  test that asserts the old overrun; clean under ASan and UBSan; twenty of
  the upstream conformance tests, including the ones for errors, coroutines,
  finalizers and to-be-closed variables, pass on a CLI built from it (§4).
- **One door stays open on any of this: a C library call that never returns
  to the VM** (`string.find` with a backtracking pattern, measured). No
  instruction executes, so no hook fires. Only a wall-clock bound in the host
  closes it. DRT's sized watchdog is still needed for that class, and the
  Rust `Instance` being `Send` gives it a cheaper move than aborting (§6).
- **Sizing:** about two days for the runtime change with tests and docs,
  plus half a day in DRT (pin, FM-4 entry, re-measure under `drt start`).
  Optional follow-on once the error is uncatchable: `dv_interrupt`, about a
  day including bindings, because it moves the ABI version (§6).
- **It splits into four increments that each ship on their own**, and the
  first three need no changes to any existing test. §9 has the layers, what
  each one closes, and the per-layer measurements.

---

## 1. The doors

**Method.** `doc/attic/fm4-probe.c`: one program per door, a fresh
instance, a 1,000,000-instruction budget, `dv_run`, then `dv_free`. A
`SIGALRM` after 10 s prints `dv_usage` and exits 124. Built with the test
flags (`ltests.h` assertions on, `-O1`), so the millisecond figures are
shape, not speed; the instruction counts are exact and build-independent.
`spin` is `function() while true do end end` throughout.

| door | program, under a 1,000,000 budget | unmodified tree |
|---|---|---|
| control | `while true do end` | `DV_ERROR` at 1,000,000, 6.5 ms |
| **pcall** (FM-4's program) | `while true do pcall(spin) end` | hang; 1,428,565,000 at 10 s |
| xpcall | `while true do xpcall(spin, function(e) return e end) end` | hang; 1,380,563,000 |
| sortpcall | `while true do pcall(table.sort, {3,2,1}, function(a,b) pcall(spin) return a<b end) end` | hang; 1,349,928,000 |
| load | `while true do load(spin) end` | hang; 433,212,000 |
| **xhandler** | `xpcall(spin, spin)` | hang; **frozen at 1,000,000**, `exceeded=1` |
| **gc_run** | `setmetatable({}, {__gc = spin}) collectgarbage()` | hang; **0, `exceeded=0`** |
| gc_remark | a `__gc` that re-marks itself, then `while true do collectgarbage() end` | hang; 0 |
| gc_alloc | the same `__gc`; the main loop only allocates | hang; 0 |
| **gc_free** | `KEEP = setmetatable({}, {__gc = spin})`, then return | **`DV_DONE` in 0 ms, then `dv_free` hangs**; 0 |
| **matcher** | `string.find(('a'):rep(300), '(.-)(.-)(.-)(.-)(.-)b')` | hang; **0** |
| coresume | `while true do coroutine.resume(coroutine.create(spin)) end` | `DV_ERROR` at 1,111,000, 7.4 ms |
| coclose | a fresh coroutine holding a spinning `__close`, `coroutine.close` in a loop | `DV_ERROR` at 1,066,000, 7.0 ms |

The bold rows are the finding. Read down the last column: the first block
is FM-4 as described (unbounded work, honestly counted); the second block
is unbounded work *not counted at all*; the last two rows are not doors.

### 1.1 Same-thread catches: pcall, xpcall, sortpcall, load

Every catch in Lua goes through `luaD_rawrunprotected` (`ldo.c:160`), and
its guest-reachable callers are a short list — this is the enumeration the
fix is built on, from `grep -n "luaD_rawrunprotected(\|luaD_pcall(" src/*.c`:

| catch site | reached by | closes with |
|---|---|---|
| `precover` (`ldo.c:957`) | every `pcall`/`xpcall` called from Lua inside `inst->co`: `lua_pcallk` takes its continuation branch when yieldable (`lapi.c`), records a `CIST_YPCALL` frame instead of a `setjmp`, and the error lands in `lua_resume`'s handler, which unrolls to that frame | one condition |
| `luaD_pcall` (`ldo.c:1083`) | every *non-yieldable* protected call — a `pcall` inside a `table.sort` comparator, a `string.gsub` callback, a metamethod called from C — and `luaD_protectedparser`, which is `load` with a reader function | one re-throw |
| `lua_resume` (`ldo.c:968`) | `coroutine.resume`, `coroutine.wrap` | nothing needed; §1.2 |
| `luaE_resetthread` | `coroutine.close` | nothing needed; §1.2 |
| `GCTM` (`lgc.c:968`) | every `__gc` finalizer | §1.3 |

Per catch the guest buys `DV_HOOK_STEP` (1000) instructions; build12p1 made
`insn_used` count them honestly, which is what the numbers in the table are.
`sortpcall` is here because it is the door a fix to `precover` alone leaves
open: the inner `pcall` runs inside a C comparator, is not yieldable, and
takes the `luaD_pcall` path — measured, the hook always fires inside the
inner `spin` and never in the comparator, so the loop never sees an
uncaught raise.

### 1.2 Cross-thread catches are bounded today, and it is worth knowing why

`coroutine.resume` and `coroutine.close` catch on a *different thread* from
the one that spins. The hook count is per thread (`L->hookcount`, reset on
the thread whose hook fires) while the budget is per instance. The child's
hook fires, raises, the catch returns to the parent, and the parent then
runs its own instructions with its own count — which the child's firings
never reset — so within 1000 of its own instructions the parent's hook
fires outside any catch. 1,111,000 is the budget plus ~110 child firings at
roughly ten parent instructions each.

Not a door, and nothing below changes it. Belt-and-braces on-top wrappers
that raise instead of returning `false, msg` once past budget are about
twenty lines and are optional; the bound does not depend on them.

### 1.3 Hooks off: xhandler and the four finalizer doors

These are the ones that need no loop, and they are three distinct
mechanisms with one shape:

- **`luaD_hook` clears `allowhook` for the duration of a hook** (`ldo.c:459`).
  `luaL_error` *inside the hook* runs the innermost `xpcall`'s message
  handler before it throws (`luaG_errormsg`, `ldebug.c:840`) — inside that
  window. A handler that spins runs unhooked. `insn_used` freezes at the
  limit, which is the reading build12p1 fixed for the *catch* case and
  which is back here on a different route.
- **`GCTM` clears `allowhook` around every finalizer** (`lgc.c:980`, "stop
  debug hooks during GC metamethod"). A `__gc` that spins runs unhooked, on
  whatever thread triggered the collection. The count never advances,
  `exceeded` is never set: the instance reads as idle while it burns a core.
- **At `lua_close`, finalizers run on the main state**, which has never had
  a hook: `dv_run` arms `inst->co` only. So `dv_free` of *any* instance whose
  program left a spinning `__gc` behind hangs the host — budget or not,
  exceeded or not, after a clean `DV_DONE`. That is FM-4 through the exit:
  a guest that has already stopped hangs whoever frees it, and in a swarm
  that is the supervisor's `kill`.

`gc_remark` and `gc_alloc` matter for the fix rather than for the finding:
a finalizer that re-marks its object (`setmetatable(o, mt)` inside `__gc`)
is finalized again on the next cycle, and if the main loop is short enough
the shared count always reaches zero *inside* the finalizer. That is the
pcall door wearing the collector's own protected call, and §2 says what it
forces.

### 1.4 C loops: the matcher

`string.find(('a'):rep(300), '(.-)(.-)(.-)(.-)(.-)b')` is one C call that
backtracks roughly `300^5/120` times. No VM instruction executes, no hook
fires, nothing is counted, nothing raises. Lua's matcher bounds *recursion*
(`MAXCCALLS`) and not steps; it is the standard sandbox complaint and the
only stdlib call in this class here — `table.sort`, `table.concat`,
`string.rep` are bounded by the memory budget, `json`/`msgpack` decoding is
linear. Nothing in this document closes it, and nothing inside the VM can:
only preemption from outside does, and §6 is about that.

---

## 2. What the fix is made of

Two facts make it small, and two on-top pieces make it complete.

**(a) "Uncatchable" is a flag consulted at three catch sites, not a new
error status.** `precover` finds no recover point when it is set;
`luaD_pcall` re-throws after its own cleanup; `GCTM` re-throws instead of
warning. `lua_resume` and `luaE_resetthread` need nothing (§1.2). A new
`LUA_ERR*` status would surface in every `lua_pcall` caller and every
binding; a flag surfaces nowhere.

**(b) A count hook can fire inside a finalizer safely.** The reason upstream
stops hooks there is debuggers: a line hook at an arbitrary point in an
arbitrary thread, and a hook that yields inside the collector, would be
catastrophic. The budget's hook never yields (§9.4, `dv.h`), and a guest
cannot install any other hook (`debug.sethook` is refused, `dlibs.c:132`).
So `GCTM` stops *debug* hooks — any mask beyond `LUA_MASKCOUNT` — and lets
a count-only hook through. A `DV_FLAG_UNSAFE_DEBUG` program with a line
hook gets today's behaviour, and can switch its budget off anyway.

**(c) The hook must raise without running a message handler.** That is the
`xhandler` door, and it is the reason `luaL_error` is the wrong call from
the hook: `lua_error` goes through `luaG_errormsg` and the handler. The
prototype adds `diluvium_shim_throw` — `luaD_throw(L, LUA_ERRRUN)` with the
lock taken as `lua_error` takes it — in `dshim.c`, which is the one file
that may read the internals. The hook builds the message and the traceback
itself, at the throw point. That is an improvement on what the handler
produced: today's message is `instruction budget of 1000000 exceeded` with
no position at all (`luaL_where(L, 1)` from a hook names the caller, which
for a spin in the main chunk is C); the prototype's is
`door:1: instruction budget of 1000000 exceeded` followed by a traceback
that names the spinning frame, the refusing `pcall`, `table.sort` and the
main chunk — see §4.

**(d) `dv_free` arms the hook on the main state before `lua_close`**, so
close-time finalizers are budgeted like any other code. At that level there
is no handler above `GCTM`, so an error is swallowed as today and each
finalizer is bounded by the hook; `luaC_checkfinalizer` refuses re-marking
while closing (`GCSTPCLS`), so the total is bounded by the objects that
exist.

**(e) The flag.** A registry boolean under a static light-userdata key —
the key's *address* is the key — created in `dv_new` so that setting it
later never allocates (the hook may fire with the memory budget spent), set
only by the instruction hook, and read from core with `luaH_get` on a
stack-built `TValue`: no API call, no lock, no allocation. The memory budget
does *not* set it: `dv.h` documents that an allocation past the cap fails
"as an ordinary out-of-memory error the program can even catch", a limit
rather than an execution, and that stays true. In the real change the key
is declared in `lua.h` (allowlisted; Diluvium's branding already lives
there) and defined in `ldo.c`.

**The re-throw's three guards**, all load-bearing: after the catcher's own
cleanup, so each level unwinds itself and the tbc variables close; never
when `L->errorJmp == NULL`, the outermost level, where `luaD_throw` would
reach the panic function and `abort()` — that is `lua_close`; and never from
a frame marked `CIST_FIN`, the finalizer's own protected call, which `GCTM`
decides for itself with the same handler check.

**A trap the conformance suite found, recorded so it is not rediscovered.**
The prototype's first reader interned the key as a string
(`luaS_newliteral`). In a plain state that string does not exist, so the
read *allocated* — inside the collector's error path — and `locals.lua`'s
simulated allocation failures threw `LUA_ERRMEM` out of `GCTM` with no
handler above it: `PANIC: unprotected error in call to Lua API (not enough
memory)`. The light-userdata key exists for exactly that reason, and the
`errorJmp` check comes *before* the flag read for the same one.

---

## 3. Two shapes, and which

Both were prototyped, both measured identically at the guest level, both
pass the same suites. They differ only in where the refusal for the
*same-thread* catches lives; the finalizer half is the same in both.

**A — core: `ldo.c` + `lgc.c`.** The four sites above. `doc/attic/fm4-prototype.diff`,
188 lines across `dshim.c`, `dshim.h`, `dv.c`, `ldo.c`, `lgc.c`; the core
part is about twenty lines.

**B — on-top for the catches: `dlibs.c` + `lgc.c`.** `pcall` and `xpcall`
reimplemented in `dlibs.c` exactly as `lbaselib.c` writes them, with a
continuation that re-raises once the flag is set; `load` wrapped so a
reader's `nil, msg` is re-raised. Installed at `diluvium_openguestlibs`, the
way `debug` is narrowed, so the CLI's states are untouched. Reimplementation
rather than wrapping, because a wrapper adds a C frame per level and halves
the nesting depth a `pcall` recursion gets before "C stack overflow".
`doc/attic/fm4-prototype-ontop.diff`, 249 lines.

| | A (`ldo.c` + `lgc.c`) | B (`dlibs.c` + `lgc.c`) |
|---|---|---|
| allowlist entries added | `ldo.c`, `lgc.c` (16 → 18) | `lgc.c` (16 → 17) |
| core lines | ~20 | ~10 |
| on-top lines | ~30 | ~130 |
| covers a future C-level `lua_pcall` around a guest callback | yes: `luaD_pcall` is the choke point | no: a list of wrappers to keep complete |
| `pcall`'s identity, frame shape, continuation | stock | a Diluvium continuation with a new registry name; a snapshot parked inside a `pcall` on the new build does not restore on an older one |
| the permanents fingerprint | unmoved | unmoved (names, `dsnap.c:302`) |
| §3.1 "patching core files: do none of this" | crossed twice | crossed once |

**Recommendation: A.** `lgc.c` moves regardless — there is no on-top way to
stop `GCTM` clearing `allowhook`, and the `gc_*` doors are the worst of the
set — so the doctrine line is crossed for the finalizer half whatever
happens. Once it is, ten more core lines buy the one true choke point, keep
`pcall` byte-for-byte stock, keep snapshots crossing in both directions, and
leave nothing on-top that a future library could forget to route through.
`doc/Messaging.md` §3.1 names `ldo.c` first among the files never to touch;
this is the case it also provides for — "if an implementation path appears
to require editing a core file not already on the allowlist, stop and raise
it" — raised, with the measurement attached.

B is the fallback if `ldo.c` is refused. It is complete for what a guest can
reach *today* (§1.1's table is the whole list, and the on-top libraries make
no protected calls on guest functions: `grep lua_pcall src/d*.c` finds only
the snapshot layer and the driver), and it costs a continuation name.

---

## 4. What the prototypes measured

Same probe, same budget, same flags. Both shapes give the same numbers; A's
are shown.

| door | unmodified tree | prototype |
|---|---|---|
| control | `DV_ERROR` at 1,000,000, 6.5 ms | same, 6.8 ms |
| pcall | hang, 1,428,565,000 at 10 s | **`DV_ERROR` at 1,000,000, 6.6 ms** |
| xpcall | hang | `DV_ERROR` at 1,000,000, 6.7 ms |
| sortpcall | hang | `DV_ERROR` at 1,000,000, 6.4 ms |
| load | hang | `DV_ERROR` at 1,000,000, 6.4 ms |
| xhandler | hang, frozen at the limit | `DV_ERROR` at 1,000,000, 6.5 ms |
| gc_run | hang, 0 | `DV_ERROR` at 1,000,000, 7.6 ms |
| gc_remark | hang, 0 | `DV_ERROR` at 1,000,000, 6.9 ms |
| gc_alloc | hang, 0 | `DV_ERROR` at 1,000,000, 6.5 ms |
| gc_free | `dv_free` hangs | `DV_DONE`, then **`dv_free` returns in 6.3 ms** |
| coresume, coclose | bounded, 1,111,000 / 1,066,000 | unchanged |
| matcher | hang, 0 | **hang, 0** — see §6 |

Zero overrun on every same-thread door: the count stops at the limit
because the first raise is the last. That is the reading build12p1's
changelog told supervisors to distrust (`insn_used` exactly at the limit was
the escape's signature), and it is honest again here because `dv_run` has
returned `DV_ERROR` beside it. A count *above* the limit is now only
possible through §1.2's cross-thread catches, and small.

**The error a host receives**, `FM4_FULL=1`, door `sortpcall`:

```
door:1: instruction budget of 1000000 exceeded
stack traceback:
	door:1: in function <door:1>
	[C]: in global 'pcall'
	door:1: in function <door:1>
	[C]: in function 'table.sort'
	[C]: in global 'pcall'
	door:1: in main chunk
	[C]: in ?
```

**Existing suites**, built against the prototype tree with the Makefile's
own flags:

| suite | result |
|---|---|
| `dv_check` | 257 checks, **1 failed**: `usage_keeps_counting_past_the_budget` |
| `dsnap_check` | 160, 0 failed |
| `dvs_check` | 139, 0 failed |
| `dtask_check` | 25, 0 failed |
| `dshim_check` | 102, 0 failed |

The one failure is expected and is a test to rewrite, not a regression: it
asserts `insn_used > limit` after a guest `pcall`, because on build12p1
"strictly greater" was what distinguished a hook that kept counting from
one that had switched itself off. With nothing to catch, the program stops
at the limit and the property it guards holds by a stronger route. Its
sibling `a_budget_survives_a_guest_pcall` passes as written and should
tighten to `== limit`.

**Sanitizers.** The doors and `dv_check` under `-fsanitize=address,undefined`
(the `sanitize_checks` flags): same results, no reports. The throw out of
`GCTM` is the part of this that deserved it.

**Conformance.** A debug CLI built from the prototype passes `gc`, `errors`,
`coroutine`, `closure`, `locals`, `calls`, `events`, `constructs`, `db`,
`api`, `nextvar`, `sort`, `strings`, `tpack`, `utf8`, `vararg`, `goto`,
`literals`, `math`, `bitwise` — twenty of the upstream suite, chosen for
touching pcall, coroutines, finalizers, to-be-closed variables, hooks and
the debug library. The CLI is unaffected by construction: the flag never
exists outside an instance, and the `GCTM` change only lets a *count-only*
hook through, which stock `lua.c` never installs.

---

## 5. What the real change touches

Runtime, shape A:

- `src/ldo.c`: the reader, the `precover` condition, the `luaD_pcall`
  re-throw with its three guards. `src/lgc.c`: the key's definition, the
  `allowhook` condition, the `GCTM` re-throw. `src/lua.h`: the key's
  declaration. `script/patch_series.sh`: two allowlist lines, with reasons —
  something like `ldo.c  past-budget errors are uncatchable: precover finds
  no recover point, luaD_pcall re-throws (doc/FM-4.md)` and `lgc.c  a count
  hook fires inside finalizers, and GCTM re-throws past the budget (same)`.
- `src/dshim.c`, `src/dshim.h`: `diluvium_shim_throw`.
- `src/dv.c`: the hook (flag, message with position, traceback, shim throw),
  `dv_new` (the flag, false), `dv_free` (arm the main state). One line the
  prototype does not have and the change should: `settle` reports
  `DV_ERROR` if the flag is set when a step ends `LUA_OK`, so that
  "instruction budget exceeded ⇒ `DV_ERROR`" is true by construction rather
  than by there being no path left that breaks it. `dv.h`'s 9.4 block says
  the error cannot be caught, that `dv_free` is bounded by the budget, and
  that `dv_usage` reads exactly the limit at the stop.
- `test/dv_check.c`: rewrite `usage_keeps_counting_past_the_budget` and
  tighten `a_budget_survives_a_guest_pcall`; add the doors, each shown red
  on the unmodified tree first (the probe is how); add the negatives — a
  budgeted instance *under* budget still catches an ordinary error with
  `pcall`, still gets `nil, msg` from `load`, still warns rather than
  raises on an error in `__gc`; a parked `pcall` still snapshots and
  restores (`dsnap_check` has it, and passed).
- CI lanes already there: `sanitize_checks`, `snap_fuzz`,
  `fingerprint_check.sh` (unmoved: names), `interrupt_check.sh`, the full
  `run_tests.sh`, and `patch_series.sh check` with the two new entries.
- Docs: `doc/Messaging.md` §9.4 ("raise, never yield" gains "and nothing
  catches it") and the §18 row, which closes; `doc/Guide.md:592` ("a
  catchable-looking error" is no longer the right description);
  `doc/Lab.md` §3.3 (a debugger's line hook is still stopped in finalizers —
  the condition is a count-only mask); the Rust `Config::budget` comment;
  `doc/DRT.md` one line.
- `CHANGELOG.yaml`: fixed — the catch loop, the message handler, the
  finalizer, `dv_free`; changed — the message carries position and
  traceback, `load` past budget raises, `insn_used` stops at the limit;
  known — C loops (§6). Guest-visible semantics move and no format does, so
  this is `5.5.1_build13`, not a patch on 12. Snapshots cross both ways.
- Bindings: `bindings/rust/diluvium/tests/limits.rs` gains the catch-loop
  case; JS and Python are optional, nothing in their surface changes.

DRT, afterwards: move the pin; FM-4 becomes "closed upstream in build13 for
VM-bound guests, open for C loops"; re-measure with `drt start` and the
root-that-spawns-it program the entry describes; keep the watchdog sizing,
now scoped to §6's class.

**Sizing.** A: a day for the runtime change including the settle line and
`dv.h`, half a day for tests (the doors are written; the negatives and the
rewrite are the work), half a day for docs and the changelog. DRT half a
day. B adds about a day: the reimplementation, the continuation
registration, and a `dsnap_check` case for a program parked inside the
Diluvium `pcall`.

---

## 6. What stays open, and whose it is

**C loops need a wall-clock bound, and only the host has a clock.** Three
things a host can do, none of them here:

1. *Abort the process on a stalled step* — DRT's own sizing in
   `doc/Next.md`. Crude, correct, takes the other tenants with it.
2. *Abandon the thread.* The Rust `Instance` is `Send` and `!Sync`, so a
   step can run on a worker with a deadline; on expiry the swarm drops its
   handle, never joins, and leaks the instance. The process stays up, other
   tenants keep stepping, one core burns until the next restart, and the
   metrics have to say so. Much cheaper than (1) for a deployment with more
   than one tenant per process, and entirely a DRT change.
3. *Cap the matcher.* A step counter in `lstrlib.c`'s `match`, PCRE-style,
   raising "pattern too complex" — a core patch (a further allowlist entry),
   about ten lines, and it narrows the class rather than closing it.
   Defence in depth if wanted; not a substitute for (1) or (2).

**`dv_interrupt` becomes cheap once the error is uncatchable.** §18's
objection to it — "the error it raised would be catchable too, so the same
loop absorbs it on a different clock" — is answered by this change. The
shape: a `volatile` flag in `dv_instance`, set from any thread (the one
function in `dv.h` that may be called off-thread, and `dv.h` must say so);
the hook checks it on every firing, treats it exactly as exceeded, and
throws. Latency is at most `DV_HOOK_STEP` instructions plus whatever C call
is in progress — so it kills every VM-bound hang and none of §6's C loops.
It is an ABI addition, so `DV_ABI_VERSION` moves and every binding follows;
about a day. It gives DRT a wall-clock kill for one instance instead of a
process abort, which is what `wall_ms` actually wants. Separate change,
after this one.

---

## 7. Decisions for the owner

1. **The allowlist:** `ldo.c` and `lgc.c` (A, recommended) or `lgc.c` only (B).
2. **The memory budget stays catchable.** Recommended: yes, as documented;
   the flag is the instruction hook's alone. A memory-only budget was never
   a time bound and this does not make it one.
3. **The message shape:** position and traceback built in the hook.
   Recommended: yes — today's has neither.
4. **`settle`'s belt-and-braces line.** Recommended: yes.
5. **Cross-thread wrappers** for `coroutine.resume`/`close`. Optional; the
   bound is measured without them.
6. **Ship as build13 now** rather than waiting for `dv_interrupt`.
   Recommended: yes; the interrupt moves the ABI and this does not.

---

## 8. Reproducing it

```sh
make _build_step0
gcc -DLUA_USER_H='"ltests.h"' -O1 -g -DLUA_USE_LINUX -DMAKE_LIB -I.data \
    -o dist/fm4_probe doc/attic/fm4-probe.c .data/onelua.c -lm -ldl
for d in control pcall xpcall sortpcall load xhandler gc_run gc_remark \
         gc_alloc gc_free matcher coresume coclose; do dist/fm4_probe $d 10; done
```

Then, on a branch, `patch -p1 < doc/attic/fm4-prototype.diff` (or the
`-ontop` one), `make _build_step0` again, and the same loop against the
patched `.data`. `FM4_FULL=1` prints the whole error. The five C suites are
`make dv_check dsnap_check dvs_check dtask_check dshim_check`; expect the
one `dv_check` failure named in §4 until that test is rewritten.

---

## 9. Building it in increments

§5 is the whole change as one landing. It does not have to be one: the fix
separates along the *mechanism* boundaries of §1, and the layers were
measured separately rather than merely sketched. Four increments, each of
which compiles, passes the suites, ships on its own, and needs at most one
decision.

The measurements below come from staging the prototype into layers on this
tree and running every door, all five C suites, twenty conformance tests and
the sanitizers at each layer. Two things fell out that a plan written from
reading would have got wrong, and they are why the layers are drawn here
rather than one row up or down:

- **Increments 1 to 3 need no changes to any existing test.** All five
  suites report their full counts with zero failures through increment 3
  (`dv_check` 257, `dsnap_check` 160, `dvs_check` 139, `dtask_check` 25,
  `dshim_check` 102). The one rewrite §4 names arrives only with
  increment 4, because only then does the program stop *at* the limit.
- **The finalizer half does not close in one line.** `gc_run` and `gc_free`
  do, but `gc_remark` and `gc_alloc` do not: a finalizer that re-marks its
  own object is finalized again on every cycle, and the shared instruction
  count always reaches zero *inside* it, so `GCTM`'s own protected call
  absorbs every raise and the main loop never sees an uncaught one. Same
  shape as `sortpcall`. Those two need increment 4's re-throw, and a plan
  that promised them at increment 2 would have been wrong.

### The increments

| # | lands | core lines | allowlist | doors it closes |
|---|---|---|---|---|
| 1 | the doors as a CI gate | 0 | none | none — it makes the class visible |
| 2 | the finalizer half | 1 (`lgc.c`) | +`lgc.c` | `gc_run`, `gc_free` |
| 3 | the raise itself | 0 | none | `xhandler` |
| 4 | uncatchable | ~20 (`ldo.c`, `lgc.c`) | +`ldo.c` | `pcall`, `xpcall`, `sortpcall`, `load`, `gc_remark`, `gc_alloc`, `memcatch` |
| 5 | release, docs, DRT | 0 | none | none |

**1 — the doors become a gate.** `doc/attic/fm4-probe.c` gets a Makefile
target beside `dv_check`, and `doc/attic/fm4-budget-check.sh` becomes
`test/budget_check.sh` with a step in `test.yml`. One subprocess per door
under a timeout, each verdict compared against a table of what is closed
*today* — so a door that reopens fails as `REGRESSED`, and a door that has
just been closed fails as `CLOSED -- update the table`. That second
direction is what makes every later increment's evidence come from CI
rather than from a claim in a commit message.

Verified: green on the unmodified tree, all fourteen doors as tabled; red
against each of the three prototype layers, naming exactly the rows that
moved; exit 2 when the probe is missing, which is `patch_series.sh`'s own
convention for a guard that cannot check.

The timeout's failure mode is asymmetric and safe: a closed door returns in
about 10 ms, so no plausible timeout calls one a hang, and a loaded runner
only costs time on the rows that are meant to hang. Nothing in `src/`
changes, and no decision is attached — so this can land while the allowlist
conversation is still open.

**2 — the finalizer half.** The one-line condition in `GCTM` (only *debug*
hooks stop for a finalizer; a count-only mask is a budget) plus `dv_free`
arming the hook on the main state before `lua_close`. Both halves are
needed for either door: at close time the hook was never armed, and inside a
finalizer it was never allowed to fire.

Closes the two doors that report **zero** usage and `dv_exceeded` false —
including the one where a program that has already finished cleanly hangs
whoever frees it, which in a swarm is the supervisor's `kill`. Gate: five
suites green with no test changes, twenty conformance tests green, ASan and
UBSan clean on the two doors and on `dv_check`. The `luaE_warnerror` per
bounded finalization is silent by default (5.5 warnings are off until
`@on`), so this adds no output to a deployment's log.

Decision: `lgc.c` on the allowlist, with a reason line that stands on its
own — a count hook is a budget rather than a debugger, and debug hooks are
still stopped there. `patch_series.sh check` cannot be run from a shallow
clone (it exits 2 by design), so CI's `fetch-depth: 0` job is where that
entry is actually proved.

**3 — the raise itself.** `diluvium_shim_throw` in `dshim.c`, the hook
building its own message and traceback at the throw point, and `settle`
refusing to report `DV_DONE` for an instance whose budget is spent. Costs
**no allowlist entry at all**: `dshim.c`, `dshim.h` and `dv.c` have no
upstream counterpart, so the patch guard does not cover them.

Closes `xhandler`, the door that needs no loop and freezes the count exactly
at the limit. Upgrades `gc_run` from a bounded `DV_DONE` to `DV_ERROR`, and
gives every budget error a position and a traceback where today it has
neither. `settle`'s own message is plain — `instruction budget exceeded` —
because a guest that caught the real error discarded it; increment 4 is what
makes the traceback survive. Gate: as increment 2, all green, no test
changes.

Increments 2 and 3 are order-independent. Take 3 first if the allowlist
conversation needs time, since it needs no decision; take 2 first if it does
not, since it closes more.

**4 — uncatchable.** The flag, `precover`'s condition, `luaD_pcall`'s
re-throw with its three guards, and `GCTM`'s re-throw. This is the increment
FM-4 is named for, and the seven doors it closes are every VM-side one left.

It does not subdivide further, and the reason is worth recording: a flag no
reader consults is dead code, a reader with no writer is a no-op, and
`precover` and `luaD_pcall` must land together because `sortpcall` is
exactly the door that either one alone leaves open. Gate: the `dv_check`
rewrite of §4, the negatives (an instance *under* budget still catches with
`pcall`, `load` still returns `nil, msg`, an error in `__gc` still warns),
the full `run_tests.sh`, and the patch guard with both entries.

Decision: `ldo.c` on the allowlist, or the on-top `dlibs.c` variant of §3
— also prototyped, and complete for what a guest can reach today. By this
point the doctrine line has already been crossed once, by an increment that
is merged and reviewed rather than by an argument in this document.

**5 — release, docs, DRT.** The changelog entry, the doc reconciliation
§5 lists, the Rust binding's test, then DRT's pin move, FM-4 rewritten to
"closed upstream for VM-bound guests, open for C loops", and a re-measure
under `drt start` with the root-that-spawns-it program its entry describes.

If increments 2 and 3 ship together as one release and increment 4 as the
next, DRT pins twice and FM-4's headline closes at the second. One release
for all three is equally coherent if the allowlist answer arrives early —
that is the only thing the release shape turns on.

### Where each layer leaves the doors

Measured, one column per increment. `error` is `DV_ERROR` from `dv_run`,
`done` a clean finish with a bounded `dv_free`, `hang` still open at 10 s.

| door | today | +1 | +2 | +3 | +4 |
|---|---|---|---|---|---|
| control | error | error | error | error | error |
| pcall | hang | hang | hang | hang | **error** |
| xpcall | hang | hang | hang | hang | **error** |
| sortpcall | hang | hang | hang | hang | **error** |
| load | hang | hang | hang | hang | **error** |
| memcatch | hang | hang | hang | hang | **error** |
| xhandler | hang | hang | hang | **error** | error |
| gc_run | hang | hang | **done** | **error** | error |
| gc_free | hang | hang | **done** | done | done |
| gc_remark | hang | hang | hang | hang | **error** |
| gc_alloc | hang | hang | hang | hang | **error** |
| coresume | error | error | error | error | error |
| coclose | error | error | error | error | error |
| matcher | hang | hang | hang | hang | hang — §6 |
| existing suites | green | green | green | green | one rewrite |

### Beyond the four

Neither of these belongs in the sequence above, and both get easier once it
is done. `dv_interrupt` (§6) is an ABI addition, so it moves
`DV_ABI_VERSION` and every binding follows; about a day, and §18's standing
objection to it is answered by increment 4 rather than by anything in it.
The C-loop class (§6) is host-side: DRT's sized watchdog, or the cheaper
move of abandoning a thread, with an optional matcher step cap in the core
as defence in depth.
