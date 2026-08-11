# Hibernation: the close-out record

This file was the working brief for finishing hibernation — the last block of the
M0–M7 audit, profile C. The block is closed: every item below is fixed, with a
named test that fails when its fix is removed, and hibernation is **on by
default**. `doc/Messaging.md` §10 is still the design and §18 carries the audit
summary with the strikethroughs; `doc/audit/M0-M7.md` holds the evidence per
finding. This file keeps what the brief learned, because most of it was paid for.

## What closed, and what each item turned out to be

| | Was | Turned out |
|---|---|---|
| **0** | `u2.funcidx` dropped; memory corruption on wake-then-error | Reconstructed at restore; `a_restored_program_can_raise` is the test that had never existed |
| **5** | Fuzzer mutants refused by the digest in front of the field checks | Re-seals the digest; the depth it gained found S2 |
| **S2** | Two mutants abort instead of refusing | The *smallest* item, not the least understood: `settbc` let a crafted slot list reach the raise in `luaF_newtbcupval`, which escapes the lock convention — one **extra unlock**, the counter at -1, not the leaked lock the audit predicted. A closability pre-check refuses it; the `KNOWN` set is empty |
| **1** | Count hook never re-armed; budget laundered by a wake | `dv_restore` arms it (the ordering is forced by `dv_set_budget` refusing a started instance), and the header carries `insn_used` — a budget belongs to the program, not one residency |
| `old_errfunc` | Restored errors correct but bare, no traceback | The record carries the thread's handler slot and each pcall frame's saved one, in slot units; validation refuses rather than asserts, which is S2's lesson applied ahead of time |
| **25** | Swarm snapshots unstamped both ways | `dvs_set_host_identity`; all four stamp quadrants asserted by moving the identity under the cache |
| **14** | Nested coroutines captured, precondition 4 unenforced | Refused by name, and the design's other commitment is held to by test: drop the coroutine and the same program snapshots |
| **12** | Endpoint references fail their own identity test after restore | The metatable is the permanent `dendpoint.refmt`; `bind` adopts an endpoint queue no token claims, which is the one route by which the host's drain path returns after a wake |

The format cost was paid once, as this file directed: `DILUVIUM_SNAP_FORMAT` 2,
`DS_THREAD_VERSION` 2, and the permanents-fingerprint change from `dendpoint.refmt`
all land in the same release, so existing snapshots are invalidated exactly once.

With the block closed: `dvs_allow_hibernation` is an opt-out (its dvs.h comment was
struck, as planned, rather than edited), §18.2's profile C stopped being a profile,
and `dv_restore` carries the reasoning where §18.3 asked for an ABI gate or a
reason — the gate was wanted while a corruption path was reachable under a switch
that said "off"; the path is closed, and refusal-by-validation is the contract.

Left deliberately undone here, for release time: the changelog entry and the
`stable: true` / `latest` fields in `CHANGELOG.yaml` (§18.3 — this block is what
*makes them available*, and they belong to the release that ships it), and the
**Known issues** rewrite that goes with them.

## The traps, kept because they will spring again

**A symptom can depend on the build.** Finding 0 reproduced three different ways:
the assertions build aborted, the release build **hung forever**, and the ASan
build finished with a *wrong error* and no complaint. The sanitizer was the
configuration that did *not* reproduce it. "Clean under asan+ubsan" is worth less
in the snapshot layer than anywhere else in this tree.

**Reading is not running.** Finding 0's mechanism was documented correctly by the
audit and still needed a live backtrace to be believed, because the predicted
crash was actually a livelock. S2 sprang it a third time: the audit read the
mechanism as a leaked lock, and the live counter read `-1` — an extra unlock, the
opposite defect at the same site. The fix was the same either way; the lesson is
not about the fix.

**Do not put a newly honest check on the release path.** `make verify_wasm` had
never executed successfully in its life; adding it to `build.yml` blocked the
5.5.1_build4 release on its first run. A check that has never passed anywhere does
not gate anything until it has passed once. The fuzzer's `KNOWN` set was the same
lesson applied ahead of time, and it emptied on schedule.

**A green test can certify the wrong half of its own claim.** The one
hibernate+budget test in the tree read "the budget survives hibernation" and
asserted the *number* read back while cached, never waking anything — so the
budget could read as preserved while enforcement was lost with the hook and the
counter with the bytes. When a claim has an enforcement half and a bookkeeping
half, the test must run the enforcement half.

## The instruments, which outlive the block

**`a_restored_program_can_raise` (`test/dv_check.c`)** resumes a restored thread
into an error, which nothing in the tree had ever done — the entire reason a
memory-corruption defect sat green through ten audits, a fuzzer and two
sanitizers. It now also asserts the traceback, and
`a_restored_pcall_still_guards_and_still_hands_back` covers the handler chain
through a restored pcall. If you change `dshim.c`'s frame rebuild or `dsnap.c`'s
thread record, these are the tests that tell you.

**The snapshot fuzzer (`script/fuzz_snapshot.py`)** re-seals the payload digest
after mutating, so a mutant is refused by the field checks rather than by the
digest sitting in front of them. Its `KNOWN` set is empty and must stay empty;
adding an entry needs a better reason than a red run.

## Still open, and honestly not part of this block

The vararg `funcidx` discrepancy: the finding-0 reconstruction's "agree by
construction" claim is measurably false for vararg pcall callees — organic
`u2.funcidx` names the pre-call slot while the reconstruction names the post-move
one, off by `(totalargs+1)*sizeof(StackValue)`, because `buildhiddenargs` moves
`ci->func.p` after `lua_pcallk` saved the offset. It is benign on every tested
path (`finishpcallk` closes and places the error one slot higher than organic,
which the driver tolerates), and the errfunc work did not lean on the claim — a
handler slot is caller-chosen and is serialized, not derived. But anyone extending
the frame rebuild should re-derive this rather than trust the comment at the
reconstruction site.
