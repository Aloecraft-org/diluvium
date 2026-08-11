# Finishing hibernation

The brief for the work that is left, written to be picked up cold. `doc/Messaging.md`
§10 is the design and §18.2 is why hibernation is optional at all; this is the
working document for closing it out.

**Read first:** §18.3's checklist, then the relevant entry in `doc/audit/M0-M7.md`,
including its **refuted** half, which exists so a session does not re-litigate
something already checked.

---

## Where this stands

`dvs_hibernate` refuses unless a host calls `dvs_allow_hibernation`, and that is
still the right default. What changed is *why*: the memory-corruption defect that
made the switch necessary is fixed.

| | |
|---|---|
| **Done** | **0** — `u2.funcidx` reconstructed on restore. **5** — the fuzzer reaches the field-validation layer. |
| **Open** | **1** budget re-arm · **12** endpoints across a snapshot · **14** precondition 4 · **25** host-identity stamp · **S2** two malformed snapshots abort |

The happy path already works and is tested: park, snapshot, free the original,
restore into a fresh instance, resume, finish. That has been true for a while. What
was missing was every path where something goes wrong.

## Two things that did not exist before, and now do

**`a_restored_program_can_raise` (`test/dv_check.c`).** Resumes a *restored* thread
into an error. Nothing in the tree had ever done that, which is the entire reason a
memory-corruption defect sat green through ten audits, a fuzzer and two sanitizers.
If you change anything in `dshim.c`'s frame rebuild or `dsnap.c`'s thread record,
this is the test that tells you.

**A snapshot fuzzer that reaches the validation layer** (`script/fuzz_snapshot.py`).
It re-seals the payload digest after mutating, so a mutant is refused by the field
checks rather than by the digest sitting in front of them. Before: 409 refused / 20
accepted / 0 crashed. After: **335 / 92 / 2**. The accepted count quadrupling is the
evidence that mutants reach depth they never reached.

Its `KNOWN` set holds S2's two failures so a *third* crash still fails the run. **That
set must shrink to empty.** Adding to it needs a better reason than a red run.

## Three traps this work has already sprung

**A symptom can depend on the build.** Finding 0 reproduced three different ways:
the assertions build aborted, the release build **hung forever**, and the ASan build
finished with a *wrong error* and no complaint. Note the direction — the sanitizer
was the configuration that did *not* reproduce it. "Clean under asan+ubsan" is worth
less here than anywhere else in this tree, and any claim resting on that phrase about
the snapshot path should be treated as unverified.

**Reading is not running.** Finding 0's mechanism was documented correctly by the
audit and still needed a live backtrace to be believed, because the predicted crash
was actually a livelock. Mark claims as run or read; `doc/Lab.md` §3.4 is the
cautionary tale and this was the second one.

**Do not put a newly honest check on the release path.** `make verify_wasm` had never
executed successfully in its life; adding it to `build.yml` blocked the 5.5.1_build4
release on its first run. A check that has never passed anywhere does not gate
anything until it has passed once. The fuzzer's `KNOWN` set is the same lesson
applied ahead of time.

## The remaining items, in the order to take them

### 1. S2 — two malformed snapshots abort instead of refusing

Start here: it is the only one where the *shape* of the work is still unknown, so it
is where the estimate can still move.

Both mutants hit `lapi.c:1115: lua_pcallk: Assertion '--plock == 0'`, on the way out
of the `lua_pcall` in `diluvium_snap_load` (dsnap.c:1798) that runs `ds_loadbody`
under protection. `plock` is `ltests.h`'s API lock-balance counter, so an error is
escaping a region between a `lua_lock` and its `lua_unlock`.

Bound the severity before spending time on it: `lua_lock` is `((void) 0)` in
`lapi.h` and only `ltests.h` makes it a counter, so a default build has nothing to
unbalance. The exposure is an embedder that defines `lua_lock` as a real mutex, where
the same input leaves it held. It is still a defect — §10.10 says refused, never
crashed — but it is a hang for threaded hosts rather than corruption for everyone.

Reproduce: `./dist/snap_harness --emit ref.snap`, flip one bit at payload offset 667
or 1072, re-seal the digest (`reseal()` in the fuzzer), `--load` it. Confirmed *not* a
regression from finding 0's fix — both abort with that fix present and reverted.

### 2. Finding 1 — re-arm the budget on wake, and carry `insn_used`

The count hook is armed in exactly one place, inside `dv_run`, and a woken instance
can never re-enter it because `dv_run` refuses a started instance. So this needs a
call inside `dv_restore` or a new `dv_` entry point. Without carrying `insn_used` a
budget becomes per-residency, which is a different promise from the one §9.4 makes.

**Take `old_errfunc` with it.** `insn_used` needs a new field in the snapshot, which
means a format bump, which invalidates existing snapshots — and `old_errfunc` needs
exactly the same bump. Doing them separately pays that cost twice. `old_errfunc`'s
symptom is visible today: a restored program's error is correct but carries no
traceback, because the record holds neither it nor `L->errfunc`, so dv's message
handler does not run at the throw point. §10.2 calls it out of scope; that was
written before the bump was inevitable.

### 3. Finding 25 — stamp host identity on the swarm's own snapshots

`dvs_hibernate` and `dvs_wake` pass `NULL` for the stamp both ways (dvs.c:688, 698),
so §10.10's "a foreign stamp is refused" is not true of the swarm's own snapshots.
The mechanism exists at the instance level; what is missing is somewhere for a swarm
to hold an identity. Expect a `dvs_set_host_identity`-shaped call, plumbed to both
sites — mirror `dvs_allow_hibernation`'s shape, which is the house pattern for a
host-set switch.

### 4. Finding 14 — enforce §10.7's precondition 4

**The design decision is settled**, and it is worth writing down because the audit
left it open. The convention is that a supervisor *tells* an instance to hibernate
and the instance cleans up and parks somewhere capturable — so precondition 4 becomes
a check a well-behaved program never trips, and enforcing it is right. There is no
good reason to want to capture a nested coroutine.

`ds_encodethread` currently captures any thread the value graph reaches (dsnap.c's
`LUA_TTHREAD` case). Refusing means distinguishing the instance's own captured thread
from any other; `DS_CAPTURED` and the existing "a thread cannot capture itself" check
are the machinery to build on. It is a behaviour change, so it needs a test that a
nested coroutine is refused *by name*, not merely that something fails.

If a **force-hibernate** grows later — drop what cannot be captured, so a supervisor
can swap an instance out regardless — note that "drop" is a data-loss decision
wearing a capture decision's clothes. A program woken with a coroutine missing is
silently wrong in a way it cannot detect. Whatever is built should let the instance
learn on wake that something was dropped, so the choice belongs to the program.

### 5. Finding 12 — an endpoint reference across a snapshot

The least understood of the five, and the one most likely to grow. A reference is a
table wearing a private metatable which is not a permanent, so a restored reference
wears a *copy* and fails the identity test. The resolver seam (§4.2) already reaches
the trusted restore path, which is where a rebuilt reference would have to be
re-registered. `bind` also makes a false statement when it fails on a restored
reference, and that is worth fixing whether or not the rest is.

## When it is done

Hibernation goes on by default when 1, 12, 14, 25 and S2 are closed and the fuzzer's
`KNOWN` set is empty. At that point `dvs.h`'s comment on `dvs_allow_hibernation`
should be struck rather than edited, and §18.2's profile C stops being a profile.

Two things to do in the same change, because they are the release's honesty rather
than its features:

**Gate `dv_snapshot`/`dv_restore` at the ABI, or note why not.** `dvs_allow_hibernation`
covers the swarm layer only; `dv_restore` is a public call with no gate, which is how
finding 0 stayed reachable by any host using the instance ABI directly. Until that is
resolved, "hibernation is off" is a true statement about one layer and a false one
about the stack.

**`stable: true` becomes available.** 5.5.1_build4 shipped `stable: false` for exactly
one reason — an ungated `dv_restore` with a known corruption path. Closing this block
is what makes the field true, and `latest` can move with it. See §18.3.
