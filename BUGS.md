# Known bugs

The working defect list, kept while development is in flight.

**What this file is not.** `doc/Messaging.md` §18 is the M0–M7 audit record — a
closed sweep with its evidence in `doc/audit/`, kept because the findings and
the refutations are both worth preserving. `CHANGELOG.yaml`'s `known_issues` is
per-release and user-facing, written when a release ships. This file is neither:
it is what is wrong *now*, in the tree, whether or not anyone has decided what
to do about it.

An entry earns a place here by being **reproduced**, not by being noticed. Where
something is suspected but unconfirmed it goes under "Unverified leads" and says
so, because §18's own calibration is the reason to be careful: of 67 raw
findings from that sweep, 35 survived scrutiny and 32 did not. A finding list
that does not separate the two is worth less than no list.

Each entry says what breaks, how to see it break, and what the fix would cost.

---

## Open — confirmed

### 1. A spawn whose program will not compile is reported as `faulted`

**Where.** `src/dvs.c:1098`, in `do_spawn`, on the `build()` failure path.

**What happens.** Every other rejection in `do_spawn` emits `denied` — bad
capabilities, a full instance table, no code in the request, the spawn rate
limit. A child whose source does not compile emits `faulted` instead, carrying
the handle of a slot that has already been released.

**Why it matters.** `faulted` is the event a supervisor restarts on. It is what
`doc/Guide.md`'s own restart example matches, and what the supervisor in
`test/dvs_check.c` matches. So a supervisor handed a program with a syntax error
spawns, hears `faulted`, restarts, and loops — forever, at one spawn per step,
burning a handle each time. Handles are never reused, so a long-lived system in
this state walks the `uint32` id space.

**Reproduced.** The canonical supervisor against a child containing
`queue.declare('work' {capacity = 4})` — a missing comma:

```
event 1: faulted|2      in 40 steps: 39 events, 39 faulted, 0 denied
event 2: faulted|3      alive at the end: 1 (the supervisor alone)
event 3: faulted|4
```

**The fix, and the decision it needs.** All three candidates are observable
changes to the `system/events` vocabulary, so the question underneath is whether
that vocabulary is a compatibility surface. `DVS_ABI_VERSION` is still 1 and the
swarm layer shipped in build 7, which suggests it is not yet.

- **(a) Emit `denied` on this path, with id 0.** Smallest change; makes the
  function internally consistent, since the property a supervisor acts on is
  "did a child come into existence" and on this path it did not. Costs the
  distinction between "the supervisor asked for too much" and "the code is
  broken", which survives only in the detail string. *Recommended.*
- **(b) Add a name — `unloadable` or similar.** Keeps the distinction, at the
  price of adding to a vocabulary that `doc/Guide.md` and §9.2 both enumerate as
  closed. Every existing supervisor would gain an event it has no case for,
  which is item 2 below.
- **(c) Leave it and document the trap.**

Either way the id should become 0, matching the other `denied` paths rather than
naming a released slot. And `doc/Guide.md`'s restart example needs a line
regardless: a supervisor that restarts on `denied` loops exactly as hard, so the
example should say that `denied` means the request itself was bad and retrying
the same code will not help.

---

### 2. `hibernated` is missing from the Guide's event table

**Where.** `src/dvs.c:1190` and `:1192` emit it. `doc/Messaging.md:896` lists it.
`doc/Guide.md:692`'s event table does not.

**What happens.** A program written from the Guide — which the README calls the
programmer's guide, and which is where a reader learns the event set — receives
an event with no row in the table it was written against.

**Fix.** One table row. No decision needed.

---

## Open — unverified leads

Noticed during a survey of the tree and **not reproduced**. Recorded so they are
not lost, and flagged so nobody acts on them as findings. Each needs confirming
before it is worth anyone's time.

- `dv_run` may leak the thread's registry reference when `lua_checkstack`
  fails — reachable only under allocation failure, i.e. exactly when a
  memory-budget test is running.
- `dv_resume` appears not to check that the handle it is given is a member of
  the current wait-set, so any ready or gone handle may fire.
- A throttled spawn makes `drain` return, abandoning the rest of that
  instance's lifecycle queue for the step — head-of-line blocking that would
  delay a `kill` or `hibernate` queued behind a throttled `spawn`.
- `dv_queue_info.direction` (`guest_read` / `guest_write`) is recorded at
  declare time and may not be enforced on any path.

---

## Performance shapes, measured and not currently biting

Recorded because each *looks* like a defect on reading, and measurement says
otherwise at present sizes. Anyone optimising these should have a measurement
first — and anyone scaling past these sizes should re-measure.

- **`kill_subtree` is quadratic in shape** — two `find` scans per slot inside a
  fixed-point loop, and it runs on every ordinary instance exit rather than only
  on an explicit kill. Measured flat, at 25.2 / 27.0 / 26.8 µs per agent across
  128 / 512 / 2048 agents, because the trees are shallow and the root sits in
  slot 0, so `find(parent)` hits on the first probe. **It would bite on deep
  hierarchies**, which no benchmark in the tree builds.
- **`dvs_step` walks the whole slot array three times** whether or not the slots
  are in use, so a table sized for growth costs per step. Real, and the size of
  the effect is memory-bandwidth-bound: with 256 agents fixed, 64× headroom cost
  2.22× per step on one machine and 1.56× on another. See `doc/Benchmarks.md`.
- **Resolving a handle is a linear scan** of the slot table, on every call that
  takes one. 0.06 µs each and irrelevant alone; quadratic for a host that walks
  its own roster, which is why the benchmark harness builds its roster once.

---

## Fixed, and owed a changelog entry

Fixed in the tree, recorded nowhere else yet. `doc/Hibernate.md`'s precedent is
that changelog entries belong to the release that ships the work, not to the
branch, so these are parked here until then.

### The counting allocator read a type tag as a size

`dv_alloc` in `src/dv.c`. Lua passes the object's type tag in the allocator's
`osize` argument when `ptr` is NULL (`luaM_malloc_`, `src/lmem.c`), and the code
subtracted it as though it were a size — so every fresh allocation undercharged
by a few bytes while the matching free credited back the full size, and the
counter drifted downward. After 400,000 allocations it read 216 bytes against
63,549 actually held.

The consequence was not only a wrong figure. `dv_usage`'s memory number, the
`status` event's `mem_kb`, and **the memory budget itself** are all that counter.
A program that allocated and freed enough could talk it back to zero and be
granted its whole budget again on top of what it already held: measured on the
unfixed allocator, a program with a 512 KB budget ran to completion holding
934,846 bytes.

`the_memory_counter_agrees_with_the_collector` in `test/dv_check.c` is the
regression test; it checks the instance's counter against
`collectgarbage("count")`, which is the one ground truth this ABI does not feed,
and it fails without the fix. The same work added `dv_memory`, since `dv_usage`
reports only the peak and in kilobytes — the right answer to "does this child
need more" and the wrong one to "what does an idle agent cost".

### Every program in `test/footprint.c` raised on its first line

They all opened with `queue.declare('inbox', …)`, and `inbox` is one of §6.6's
two reserved queues — it exists before the program starts, so declaring it
raises. The run status was discarded, so an instance that died on line one was
measured and printed as one parked on a wait, and §18.2's density figure was the
cost of a raise. `measure` now prints the run status so it cannot recur quietly,
and the option key is `capacity` rather than the `cap` the queue library silently
ignores.

**Any footprint figure published before build 9 should be treated as invalid.**
