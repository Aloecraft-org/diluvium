# Diluvium Roadmap

Scope: the standalone runtime and compiler in this repository. Host
embedding and capability plumbing were previously tracked elsewhere and
kept out of this file; that boundary has moved. They now live in
`doc/Messaging.md`, which specifies the msgpack codec, queues, the
delivery model, the lifecycle capability, hibernation, the `dv_*`
instance ABI and packaging. This file stays the record of the fork's
language, compiler and runtime state; anything downstream of the ABI
belongs there.

This document exists so the state of the fork is readable from the tree
rather than reconstructed each time. Update it in the same commit as the
work it describes.

## Verified state

Against `v5.5.1_build2` (Lua 5.5.1 fork point `7579fc9`), suite green at
39 passed / 0 failed / 6 skipped on linux-x86_64 and macos-arm64, and
again under ASan and UBSan with no report.

`5.5.1_build1` is the first release of the 5.5 line and is named to stay
out of upstream Lua's version space: upstream will never ship a
`5.5.1_build1`, so a Diluvium build can never be mistaken for one, and
`_rcN` is left free for whatever upstream does with it.

### Language

| Feature | State |
| :--- | :--- |
| String interpolation `$"...{expr}..."` | done |
| Null coalescing `??` | done, short-circuits, no dedicated opcode |
| Secure (obfuscated) functions `~function` | done |
| Compound assignment `+=` etc. | done (no `~=`; see below) |
| Safe navigation `?.` / `?[` | done |
| `switch` statement | done |
| `match` (switch as an expression) | **dropped** -- see below |
| `defer` / `with` | done |
| F-string format specs `{x::%.2f}` | done |
| Literal suffix registry (`1.23d`) | not started; gated on decQuad semantics |

### Review findings

The nine findings from the handoff review, re-tested against the tree
rather than taken from the log:

| # | Finding | State |
| :--- | :--- | :--- |
| 1 | `local ~function` silently created a global | fixed |
| 2 | `??` evaluated both operands | fixed |
| 3 | Obfuscation covered a quarter of the code (byte count) | fixed |
| 4 | Nested closures inside secure functions not obfuscated | fixed |
| 5 | Bytecode format byte not bumped | fixed (`LUAC_FORMAT` 0x45) |
| 6 | F-string nesting and escapes | fixed |
| 7 | `$` fallthrough created `$`-prefixed identifiers | fixed |
| 8 | `luai_verifycode` empty | fixed in `_build3` (see "The verifier, as built") |
| 9 | String hash seed nondeterministic | fixed |

Finding 8 is stock Lua behaviour, but the assessment above that it "only
becomes a hard requirement once compiled chunks are accepted as untrusted
input" understated it, and `script/fuzz_exec.py` is why.

Mutate one byte of a small dump and run the result: **about 7% of mutants
crash the release interpreter** -- segmentation faults and
`munmap_chunk(): invalid pointer`, not merely errors. Measured at 36 of
500 on one seed and 21 of 300 on another, on `diluvium_linux_x86_64`
where the debug assertions are compiled out.

The reason `script/fuzz_undump.lua` reports clean is that it never
executes what it loads. Corrupting an instruction's operand usually
leaves a chunk that still loads, because nothing checks operands against
the prototype that owns them; the damage appears only when the VM runs
it. The byte-stream fuzzer counts those as passes. Both fuzzers are worth
having, and neither substitutes for the other.

This is a memory-safety property, not a robustness nicety, and it sits
directly under what the fork advertises -- secure functions and the
analysis report exist so a chunk you did not compile can be handed to you
and inspected. A verifier is the fix: check each instruction's register,
constant, upvalue and prototype indices against the prototype's own
limits, and each jump target against its code length, at load time.
`fuzz_exec.py --allowed 0` is the test for it.

**Landing in 5.5.1_build3**, and partly landed: see "The verifier, as
built" below for what it does and the one crash class it does not close.

build2 shipped without it, deliberately and
on the record: the exposure is documented in the README and in build2's
`known_issues`, the complete mitigation (`load(bytes, name, "t")`) already
exists, and the normal paths are clean under ASan and UBSan. What made
that acceptable is saying so rather than shipping quietly. When the
verifier lands it needs structure-aware mutation to test it -- corrupting
operands specifically rather than random bytes, since random mutation
measures an accident rate and not an attack rate -- and the claim it
supports is "malformed bytecode is refused rather than crashing", never
"bytecode is safe". Lua 5.1's fuller checker still had escapes, and a
verifier believed to do more than it does would repeat this release's
other mistake.

### The verifier, as built

`src/lverify.c`, called from the `luai_verifycode` hook stock Lua leaves
empty. It is a **new** file rather than a patch to `lundump.c`, so the
core patch series stays at 14: `lundump.h` gains the one-line hook
definition it already had a slot for, and nothing else in `src/` moves.

Measured on the same seeds, verifier off then on, 300 mutants each:

| Seed | Before | After |
| :--- | ---: | ---: |
| 1 | 21 | 0 |
| 2 | 18 | 2 |
| 3 | 18 | 1 |
| 7 | 21 | 2 |

78 abnormal terminations out of 1200 down to 5 (and 3 out of 1500 on a
further seed). The suite is at 40 passed / 0 failed / 6 skipped.

**The suite could not have caught a bug in this file, and did not.**
`luai_verifycode` is called from `luaU_undump` and from nowhere else, so
it only runs when a *binary* chunk is loaded — and almost every test
feeds source. The first version of this verifier rejected
`for i = 1, 10 do end` on reload, and the whole suite passed anyway.
`test/test_verify.lua` exists for that reason and is shaped around it:
everything in it goes through `string.dump` and back, because that is
the only path that reaches the code under test. It round-trips every
construct the compiler emits — each loop form one variable at a time,
since the operand bound is a function of the variable count and an
off-by-one only shows at the exact frame size — plus every suite file's
own compiled form, stripped and unstripped, as a free corpus of real
compiler output.

It loads mutants and deliberately runs none of them: loading is where
the verifier works and is safe in-process, while running a mutant that
survives the verifier takes the suite down with it. That is
`fuzz_exec.py`'s job and the reason it forks. The refusal *rate* is
pinned too — the loader alone refuses 19.2% of single-byte mutants and
the loader plus the verifier refuses 32.9%, so a floor of 25% fails if
`luai_verifycode` is ever quietly emptied again.

What it checks is bounds and structure: every register, constant,
upvalue and prototype index against the owning prototype's own limits;
every jump target against the code length, and never onto an
`OP_EXTRAARG`; the register *window* of the instructions that name a
range, not merely the base; the `EXTRAARG` and `MMBIN` adjacency rules
in both directions; that a function ends in a return, so execution
cannot run off the end; and a nested prototype's upvalue descriptors
against the enclosing function's registers and upvalue list, which is
the one bound that belongs to a different prototype than the one being
read.

Four of those deserve recording individually.

The `C` operand of `OP_RETURN` and `OP_TAILCALL` is not a count but a
signal (`lcode.c`, `luaK_finish`); the VM subtracts it from `ci->func`,
so an arbitrary value walks the frame pointer backwards — a write.
Checking the count is not enough on its own, because
`ci->u.l.nextraargs` is the other half of that subtraction and is
written only by `buildhiddenargs`, on the `PF_VAHID` path; the flag has
to be checked with it or the value is whatever the `CallInfo` was last
used for.

`OP_MMBIN` and its two variants read the instruction *before* them and
take that instruction's `A` as the register the metamethod result is
written into, so the metamethod instructions carry a predecessor rule
and not only an operand rule.

A test that branches does not dispatch the following instruction:
`donextjump` reads the raw word and takes `GETARG_sJ` of it whatever
opcode it carries. Requiring an `OP_JMP` there is what makes that
jump's own target check the real bound — without it a 25-bit signed
offset is read out of, say, an `OP_LOADK` and the interpreter leaves the
code array.

`f->flag` is loaded raw and `luaT_adjustvarargs` only `lua_assert`s
which of the two vararg shapes it is looking at, so in a release build a
bad flag silently picks a branch and builds a frame the prologue never
prepared. The flag, the presence of `OP_VARARGPREP` at pc 0, and the
opcodes that depend on either (`OP_VARARG`'s two forms, `OP_GETVARG`)
all have to agree.

Beyond the instruction stream, four more checks close faults that have
nothing to do with operands:

- **Line info must cover the code, or be absent.** `loadDebug` reads the
  line-info length independently of the code length, and `ldebug.c`
  guards only against a `NULL` `lineinfo`, never a short one — so a chunk
  whose line info is shorter than its code reads out of bounds on the
  first traceback, error or line hook. The check is `sizelineinfo == 0 ||
  sizelineinfo == sizecode`, and the zero is load-bearing: a stripped
  chunk loaded the ordinary way has `lineinfo == NULL`, but loaded into a
  fixed buffer (the C API's `"B"` mode) it is a **non-NULL zero-length
  view** into the caller's memory. An earlier form of this check tested
  `== sizecode` and would have refused every stripped chunk loaded in
  fixed mode — an adversarial reviewer found that before it shipped, and
  it is the reason the check keys on the length and not the pointer.
- **Absolute line entries in range and ordered**, since `getbaseline`
  binary-searches them by pc, plus a density floor
  (`(sizecode-1)/MAXIWTHABS <= sizeabslineinfo`) because its first guess,
  `pc/MAXIWTHABS - 1`, is taken on faith to be a valid index. The floor
  uses the same `MAXIWTHABS` the runtime does — a chunk compiled under a
  different one and loaded here is already outside what the format
  promises, and stock `getbaseline` says as much.
- **`NEWTABLE` hash size.** The VM turns `vB` into a size with
  `1u << (vB - 1)`; `vB` is a 6-bit field, so a value past 32 shifts a
  32-bit word by 32 or more, which is undefined and which UBSan flags.
  The bound is 32, not 31, because `1u << 31` is defined and the compiler
  can legally emit exactly 32.
- **`is_encrypted` is 0 or 1**, rejected in the loader. This one closes
  no memory-safety fault — the reviewer confirmed a stray value behaves
  as 1, since every path that reads it is a truthiness test — but it is a
  malformed encoding, and the loader refuses those by name for the same
  reason it rejects unknown flag bits.

**One structural check was tried and deliberately dropped: IT/OT
pairing.** `lcode.c` asserts `luaP_isOT(code[pc-1]) == luaP_isIT(code[pc])`
— the invariant that would bound the zero-count `CALL`/`RETURN`/`SETLIST`,
which inherit `L->top` from a top-producing predecessor. But it asserts
it *before* `luaK_finish`'s own peephole rewrites `RETURN0` into `RETURN`
on every vararg or `<close>` function, which changes `isIT`, so the
*dumped* code does not satisfy the invariant. Enforcing it at load time
would refuse every vararg and every to-be-closed function. It belongs in
the runtime bucket with `SETLIST` below, not here — the load-time half is
genuinely not expressible without replaying `finish`.

### Fuzzing the verifier: accident rate and attack rate

`script/fuzz_exec.py` flips random bytes and measures how often that
crashes the interpreter. That is an accident rate: most single-byte
mutations land in string or debug data and never reach an operand check.
`script/fuzz_struct.py` is the attack-rate companion the roadmap asked
for. It locates the code array in a real dump — anchored on the 4-byte
alignment `dumpCode` gives it, the `OP_VARARGPREP` that opens every main
chunk, and the return that closes every function — and sets each
instruction operand out of range for the prototype that owns it, which is
exactly what the verifier checks. If it cannot find that anchor it aborts
rather than fuzzing nothing, because a silent zero-coverage pass is what a
format change would otherwise cause.

Measured off versus on, one corpus, 469 operand mutations: **111 crashes
become 0.** `--allowed 0`, and it stays there — this is the gate that says
the operand crash class is closed. Both fuzzers now run in CI at
`--allowed 0`.

### The one type assumption, and how it was closed

`OP_SETLIST` was the single opcode in the dispatch loop that read a
register as a table without first testing that it is one:

```c
Table *h = hvalue(s2v(ra));   /* no ttistable */
```

Every sibling — `GETFIELD`, `GETTABLE`, `SELF` — reaches `ttistable`
through `luaV_fastget` first; `SETLIST` was the anomaly, and `ltm.c`'s
vararg-table path (`luaT_getvarargs`) had the same one. Corrupt the
`NEWTABLE` that set `R[A]` up, or the register it targets, and `h->asize`
dereferenced whatever the slot held — the last release crash class, and
the three `gdb`-traced `fuzz_exec` mutants.

This is a **dataflow** property — was `R[A]` last written by a `NEWTABLE`
on every path that reaches here — not a bounds one, so the load-time
verifier cannot settle it: "there is a `NEWTABLE` textually before this
`SETLIST`" is not "`R[A]` is a table here" without a control-flow graph
to rule out a jump over it, and building that graph is the aspirational
work below.

So the fix is a **runtime type-test in the VM**, one `ttistable` at each
of the two sites, raising a catchable error instead of dereferencing a
wild pointer. It is sound and complete by construction — it checks the
real value at the moment it is known — with no false-rejection surface,
and it is one tag compare next to the `last > h->asize` compare already
there. The framing that makes it the right call rather than a reluctant
patch: it does not *add* a check, it *removes an inconsistency* where one
opcode skipped the test its siblings all make. That is the argued
exception `patch_series.sh` exists to surface, and it takes the core
series from 14 files to 16 — `lvm.c` and `ltm.c`, both newly allowlisted
with that reason. With it, `fuzz_exec.py --allowed 0` passes.

What remains at `SETLIST` is not memory safety: two debug-build
`lua_assert`s a corrupt operand can still trip — a live `CLOSE` with a
nonzero `B` (the parser emits dead ones with `B == 1`, so the verifier
cannot reject it), and the `SETLIST` preallocation assert when a corrupt
`vC` forces a resize. Both are release-safe — the resize path is
self-consistent in its (possibly wrapped) `last`, and `CLOSE` ignores `B`
— which the release fuzzers confirm by staying at zero on exactly those
mutations. They are invariants worth an assertion, not holes.

### Aspirational: a dataflow pass, shared with boundedness

The verifier is a bounds-and-structure checker by design, and the
`SETLIST` type-test above is the one place that was not enough. The
principled version — a control-flow graph and an abstract interpretation
that tracks, per register per program point, whether a value is
known-table — would let the verifier *prove* the `SETLIST` invariant
statically rather than the VM enforcing it dynamically, and would make
the vararg-table check the same kind of static fact.

It is recorded as aspirational rather than scheduled because it is not
single-use: the **boundedness verdict** in the Analyzer section below
needs exactly this machinery — basic blocks and a fixpoint over loop
structure. Building the CFG once serves both, which is the argument for
doing it deliberately and well rather than bolting a narrow
`SETLIST`-only tracer onto the verifier now. The VM type-test remains
correct and cheap even after the analyzer can also prove it, so this is
additive, not a replacement — belt and suspenders, the right posture for
memory safety.

One thing the verifier got wrong about itself, worth recording because
of the shape rather than the size. Every opmode macro — `testAMode`,
`testMMMode`, `getOpMode` — indexes `luaP_opmodes`, which has one entry
per opcode (85) and is read with a 7-bit field that encodes 128 values.
Validating an opcode before using its own opmode is not enough, because
several checks look at a *neighbouring* instruction: the metamethod
fallback after an arithmetic opcode, the owner of an `EXTRAARG`. A
corrupt neighbour was read out of that table for exactly as long as it
took its own turn to come around — a verifier with the bug it exists to
catch. ASan found it and nothing else did; the debug build's own
assertions did not, and neither did the fuzzer, because reading 42 bytes
past a static array rarely faults.

The fix is structural rather than local: `verifyproto` sweeps every
opcode for range in a pass of its own before any per-instruction check
runs, so neighbour lookups are safe by construction, and the
metamethod-family test is a range compare rather than a table read.
Anything added later that inspects another instruction inherits that
guarantee instead of having to remember it.

One loose end, recorded because it is genuinely unresolved rather than
because it is understood. Walking every single-byte mutation of the
*debug* build's own dump leaves three assertion failures: two are the
`SETLIST` type assumption above, and the third is `ci_func`'s
`ttisLclosure` at `lvm.c:1216`. The release build loads that same
mutant, runs it, and returns the correct answer, so it is not
reproducible as a release-mode crash and may be an artefact of what
`ltests.h` changes. It is either a debug-only artefact or a silent
invariant violation that happens not to fault; deciding which needs
more than the time it has had. Note `test_verify.lua` cannot hit it —
that test loads mutants and runs none.

### Secure functions and the saved-string table

Found after the review, and worth recording because of what it says about
the shape of the feature rather than only the bug.

5.5 added a saved-string table to the dump: each distinct string is
written once and referred to by index afterwards. Scrambling decided by
position in the proto tree -- which is how the instruction stream still
works -- silently stops being sound under that, because a string shared
between a secure function and ordinary code has exactly **one** stored
copy, at whichever site was written first. A function dumps its own
constants before recursing into its children, so a literal shared with
the enclosing function was always stored in the clear:

```lua
local tag = "license-key";
~function check() return "license-key" end   -- strings(1) found it anyway
```

Among siblings it depended on which was dumped first, which is a worse
property still: the same source leaked or did not depending on where else
a literal happened to appear. It round-tripped perfectly either way, so
nothing failed -- the function was marked secure and the bytes simply
were not there.

Deduplicating per `(string, encrypted)` pair does not fix it. The
plaintext copy has already been written by then; a second, scrambled copy
alongside it leaves the first one exactly where `strings` will find it.
The decision has to be made **before** anything is written, so
`taintSecureStrings` walks the whole proto tree first and marks every
string any secure function will contribute. A marked string is scrambled
wherever it is written, including from ordinary code.

That in turn means position no longer tells the loader what to do, so a
written string's size field now carries a scramble flag in its low bit
(`LUAC_FORMAT` 0x45). Cost is under a byte per distinct string, and
dedup is kept. Consequence worth knowing: a literal shared with a secure
function is hidden in the *ordinary* function's copy too -- there is only
the one copy, and it is the secure function that decides.

The general lesson for the next Lua rebase: any upstream change to how
the dump stores things has to be re-checked against secure functions,
because the security property is about the encoding and not only about
the values.

### The scramble itself, finally looked at

Both rounds above were about *where* the scramble is applied. Neither
touched what it is, and it was a single repeated byte from the day the
feature was written -- `git log -S 0xBE` lands on the original commit and
nothing after it. That passed every test, including the CI audit, because
every test asked whether `strings` finds the constants. It does not. `tr`
does, in one pass, which is about one step above the text editor the
README says this defends against.

It is now a generated keystream (`LUAC_FORMAT` 0x46), seeded from the
block length so blocks do not share a prefix, deterministic and
self-inverse. The tests changed shape more than the code did:
`test_secure_dump.lua` sweeps all 256 single-byte keys instead of checking
the one in `ldump.c`, and the CI audit does the same over real `luac`
output -- a property of the encoding rather than a fact about a constant,
so simplifying the scramble back cannot pass again by accident. Both were
verified failing on the previous scheme before being committed.

Two constraints now bind anyone editing this, and both are recorded in
`ldump.c` beside the code: it must stay **deterministic**, because
`doc/Messaging.md` content-addresses prototypes by the hash of their
stripped dump and a per-dump nonce would break that while round-tripping
perfectly, and it must stay **self-inverse**, because `lundump.c` holds
the same function under another name.

The honest claim is unchanged in kind and stronger in degree: this is
obfuscation, not encryption. Recovering a secure function's strings takes
reading these public sources and implementing the keystream. That is
trivial for anyone who wants to, and no longer a one-liner for anyone who
does not.

### `~function` after an expression

`~` is also the binary xor operator, and Lua expressions span newlines,
so a `~function` statement directly after an expression-ending statement
is absorbed into it:

```lua
local tag = "x"
~function probe() end        -- parsed as: "x" ~ function probe() ...
```

The result is the confusing error `'(' expected near 'probe'` rather than
a silent miscompile, since a named function is not an expression. A `;`
after the preceding statement resolves it. This is inherited from Lua's
grammar rather than introduced by the `~` prefix, and it is the same
class of problem as the `switch (x)` lookahead -- worth knowing before
any further sigil-prefixed syntax is added.

### Core patch series

14 files, enforced by `script/patch_series.sh check` in CI. Doctrine is
that this stays as small as it can be: anything expressible as a library
or an embedder file does not belong in `src/`.

### Testing

`test_secure_dump.lua` covers the dump-level security property directly:
that a secure function's strings are absent from the bytes, across shared
literals, both sibling orders, both sides of the short-string boundary,
debug names, stripped and unstripped, and back-references; and that all of
it still round-trips once a string is hidden away from the function that
hid it. CI's obfuscation audit checks the same property from outside,
through `luac`, and now uses a probe that shares a literal with ordinary
code -- the shape the old probe could not have caught.

Each feature has its own test, and `test_interop.lua` is where they meet.
It covers the two things a single-feature test cannot: **combinations**
(an f-string with a format spec, built from a safe-navigation chain and a
`??` default, inside a `switch` case, inside a `defer`, inside a secure
function) and **backward compatibility** -- every contextual keyword used
as a local, a field, a constructor key, a function name, a method name and
a call, plus the three call shapes (`switch (x)`, `switch "s"`,
`switch {}`) the lookahead deliberately excludes.

That second half is the one to extend when a contextual keyword is added:
the loop is data-driven, so a new keyword is one entry.

## Next

The first item next session is **Diluvium Lab integration**, and it is
mostly a subtraction problem. Lab was started before `drepl.c` and
`dline.c` existed, so it has its own console, its own completion and its
own notion of "is this input finished". At least three of those are now
duplicated by runtime code that is better placed to answer -- completion
walks real tables and metatable `__index`, and `repl_eval` reports
unfinished input as a status rather than as error-message text a front end
has to pattern-match. The work is to find each place Lab reimplements
something the runtime now exports, decide which side owns it, and delete
the other. `doc/repl-reference.html` is the contract to align against: it
is a working REPL over exactly those exports.

The blocking prerequisite is a **dev-branch release** -- being able to
pull an artifact built from a development branch into Lab for testing,
without cutting a public release. The Build workflow already builds any
commit by SHA and uploads to the run; what is missing is a stable place
for Lab to fetch from, which is a mirror at minimum.

Also unscheduled:

- **Literal suffix registry** (`1.23d`). The value is not decQuad on its
  own -- it is being able to say explicitly what a literal means and have
  the compiler hold you to it, rather than inferring from spelling. Design
  the registry around that: a suffix is a named claim about a literal's
  type and precision, decimal being the first entry, not the reason.
- **`match`** -- switch in expression position. Dropped for now; the
  statement form carries the README promise on its own.
- The analyzer work below.

Every new construct needs analyzer support and a test in `test/`, and must
be a syntax error in stock Lua.

### Contextual keywords

`switch` follows the precedent 5.5 set for its own `global`: it stays an
ordinary name, and a statement only parses as a switch when the token
after it cannot continue a call or an assignment. That deliberately
excludes `(`, a string and `{`, because `switch (x)`, `switch "s"` and
`switch {}` are all calls stock Lua accepts and they have to keep their
meaning. Inside a switch body, `case` and `default` are keywords.

Any future contextual keyword needs the same treatment, plus a `luaC_fix`
in `luaX_init`: recognition is pointer equality against an interned
string, so an unfixed name can be collected and re-created, and the
comparison then fails depending on when the collector ran.

### defer

`defer stat` desugars to an anonymous to-be-closed local holding
`setmetatable(t, t)`, where `t.__close` is the deferred statement compiled
as a parameterless function. One rule covers both forms, because
`do ... end` is itself a statement. Ordering and unwinding are inherited
from Lua: to-be-closed variables close in reverse declaration order on
every exit -- end of block, `break`, `goto`, `return`, an error, and
`coroutine.close` on a suspended coroutine.

Cost is one table and one closure per defer executed, in eight
instructions. The value has to carry `__close`, and generated code can
only reach a metatable through `_ENV`, so this calls `_ENV.setmetatable`
and lets a single table be its own metatable.

The cheaper alternative -- give the *function* type a metatable at state
creation, making a bare closure closable and dropping this to one
allocation -- was measured and does work, but it changes what
`getmetatable` returns for every function, makes `<close>` silently legal
on any function where it previously errored, and ties compiled bytecode to
state setup so a chunk would fail on a state that had not installed it.
Going through `_ENV` keeps the whole feature inside `lparser.c`.

Inherited semantics worth knowing: an error raised by deferred code while
another error is already unwinding *replaces* the in-flight error. That is
Lua's to-be-closed rule, not a Diluvium choice, and `test_defer.lua`
asserts it so it stays known.

### Safe navigation

`a?.b` and `a?[k]` short-circuit the *whole* remaining chain, as in C# and
JavaScript: once the value to the left of a `?` is nil, nothing further in
the chain is indexed or called, and the result is nil. Each `?` adds a
jump to one exit list; they all land on a `LOADNIL` into the register the
finished chain occupies. Three extra instructions, and only the guarded
head is tested rather than every step.

It tests nil, not falsiness — `false?.x` still raises, matching `??`,
which also short-circuits on nil alone. No lexer change: `?.` already
lexes as `?` then `.`, and `suffixedexp` looks ahead for `.` or `[` before
claiming the `?`. As with compound assignment, that means a space between
them is accepted too.

Because the nil path has to leave a value behind, such a chain is forced
into a register, so it is neither an assignable variable nor a bare call:
`a?.b = 1` is rejected, and `exprstat` takes a flag so `a?.b()` is still
accepted as a statement.

The one shared codegen addition is `luaK_skipifnil` in `lcode.c` — the
same EQK-against-nil test `??` uses in `luaK_infix`, with the opposite
sense, since `condjump` and `nilK` are static there.

### F-string format specs

`$"{value::%.2f}"` compiles to `string.format(spec, value)` instead of
`tostring(value)`. The spec is read raw up to the closing `}`, since it is
a `string.format` directive rather than Lua source.

It is `::` and not the `:` every other language uses, because `:` already
introduces a method call and `$"{obj:method()}"` has to keep meaning that.
Choosing between the two would need a token of lookahead, and taking that
lookahead consumes the very characters the spec is made of -- so `::`,
which cannot start a method call, avoids the problem instead of fighting
it. No change to `suffixedexp` and no new lexer state.

Which function to call is only known after the expression has been read,
so its register is reserved up front and filled in afterwards. The
function is built at the top of the stack and moved down, not emitted
straight into that slot: indexing a global by a constant whose index does
not fit an operand needs a scratch register, and at the top that lands
above the arguments rather than on one. That costs one `MOVE` per
interpolation, which is noise next to the `GETTABUP` and `CALL` beside it,
and is worth a single code path with no dead work.

The debug build sets `MAXINDEXRK` to 1 (see `ltests.h`), which forces that
scratch register on every constant past the second. It caught this; a
release build would only have shown it in a function with enough
constants. Anything that emits code into a reserved slot should be tested
under the debug binary for that reason.

### Compound assignment

Recognised in `exprstat` from the ordinary binary token plus `=`, rather
than lexed as a token per operator, so the whole feature costs one file
and no new tokens. The trade is that a space between the two is also
accepted (`x + = 1`) — not valid Lua either way, and hard to reject
without the tokens, since Lua's lexing is whitespace-insensitive
throughout.

There is no `~=` form: it is already "not equal", so bitwise xor has no
compound spelling. `??=` exists and short-circuits.

## Numeric types and portable bytecode

A chunk compiled by any Diluvium build loads on any other. Stock Lua does
not promise that and does not need to; Diluvium does, for the same reason
`luai_makeseed` fixes the string hash seed -- a contract runtime whose
nodes disagree about what an integer is has no consensus to reach, and a
compiler you have to re-run per target is one you cannot ship artifacts
from.

The format already carried most of the way. 5.5 writes integers, sizes and
counts as varints, so a chunk's *body* does not depend on how wide an
integer is; `lua_Number` is the one field still written raw. And the header
records `sizeof(int)`, `sizeof(Instruction)`, `sizeof(lua_Integer)` and
`sizeof(lua_Number)`, each followed by a sentinel read back raw -- so a
width, a byte order or a float format that disagrees is *refused* at load
rather than misread. That half matters more than the portability: the
failure mode was never silent corruption.

What was missing was the builds agreeing with each other. `luaconf.h`
infers the numeric types from the platform, and one inference is a trap.
`LUA_C89_NUMBERS` is on whenever `LUA_USE_C89` is set and the platform is
not Windows, and it takes `long` for the integer -- but `LUA_USE_C89` is a
statement about the *library* a target has, and the browser build sets it
for the wasm shim's sake. Two unrelated decisions on one switch, so
`wasm32-unknown-unknown` was a 32-bit-integer build:

| | every other target | `wasm32-unknown-unknown` |
| :--- | :--- | :--- |
| `math.maxinteger` | 9223372036854775807 | 2147483647 |
| `3000000000 + 1` | `3000000001`, an integer | `3000000001.0`, a float |
| `math.tointeger(2^40)` | 1099511627776 | nil |

Not only a narrower range: the subtype changes, which reaches `math.type`,
`//`, `%`, `%d` and table keys. Nothing shipped was affected -- the WASI
builds never set the flag, `site/` loads `libdiluvium_wasi.wasm`, and the
browser target is advisory and unreleased -- so it was a trap armed for
whoever shipped that target first.

`luaconf.h` now pins `long long` and `double` regardless, with
`DILUVIUM_NUMBERS_UNPINNED` as the deliberate way back out. It costs
nothing: both types exist on every target, wasm included, where `i64` is a
core value type and not an emulated pair. `ldump.c` asserts the two sizes
at compile time, so a build flag cannot move them again.

Three checks, each covering what the others cannot:

- `make dump_check` -- the header a dump produces, byte for byte, and a
  mutated header being refused. Runs wherever CI runs.
- `test/dump_cross_check.sh` -- that no build flag moves the numeric types
  any more, read from the *macro* rather than from a `sizeof`, because on
  an LP64 host `long` is 64 bits and the bug is invisible; and a genuinely
  mismatched build getting a refusal rather than a misread.
- `test/fingerprint_check.sh` -- already there, and the reminder that
  agreeing about numbers is not agreeing about *codegen*: the debug and
  release builds of this tree compile the same source to different
  bytecode, which is why a snapshot's runtime identity is a hash and not a
  field list.

### What this does not buy

Portable bytecode is a property of the artifact, not of the run. The same
chunk on two machines can still produce two answers, and the analyzer's
determinism verdict remains the thing that says whether it will:

- **Host calls.** Anything reaching out -- clock, filesystem, network,
  entropy -- is nondeterministic by construction, and the analyzer already
  treats it that way. `doc/Messaging.md` 8.3 gives the host the clock on
  purpose. `doc/Determinism.md` is about making the *scheduler* replayable
  given a message log, which is a third guarantee again, distinct from both
  of these.
- **libm.** IEEE-754 specifies `+ - * /` and `sqrt` to be correctly
  rounded, which is why ordinary arithmetic is bit-identical everywhere --
  `0.1 + 0.2` and `1e16 + 2.0` agree across platforms. It does not specify
  the transcendentals, and implementations differ. Same pinned chunk, no
  host call anywhere near it:

  ```
  math.sin(1e22)    glibc   -0.85220084976718879
                    msvcrt   0.46261304076460175
  ```

  Not a last-bit difference: argument reduction for large inputs is where
  libms diverge outright. `sin`, `cos`, `tan`, `exp`, `log` and `^` are all
  in this class, and a decimal library would not change it -- that is a
  question about *precision*, this is one about which implementation
  answered.
- **Addresses.** `tostring` of a table or function prints a pointer, so it
  differs between runs as much as between platforms.
- **The collector.** `collectgarbage("count")`, and anything measured
  against GC progress.

Settled, and worth not re-litigating: `pairs` order over string keys, which
`luai_makeseed` fixed by pinning the hash seed to `"DILU"`.

## CLI and REPL

The intelligence a REPL needs -- unfinished versus broken input,
expression echo, completion -- is in `drepl.c`, on-top code using only the
public C API, compiled into the amalgamation so the interpreter and the
WASM host share one implementation. Before that it was static inside
`lua.c`, and a browser front end could only tell "keep typing" from
"that's wrong" by matching the text of Lua's error messages.

Line editing is `dline.c`, written rather than vendored so the repository
stays under one licence. GNU readline is gone: it was GPLv3 and, linked
statically, larger than the whole runtime. The binary now links only libm
and libc, and gained 13 KB. It highlights as you type, with Diluvium's own
syntax in its own colour; `NO_COLOR`, `DILUVIUM_NO_COLOR` and `TERM=dumb`
all turn that off.

`wasm_stubs.c` exports `repl_eval`, `repl_complete` and `reset_lua` over
the same persistent state `run_lua` uses. `doc/repl-reference.html` is a
working browser REPL against those, verified in Chromium against a real
released artifact, and it records the three things about the WASM build
that are not guessable from outside: which artifact to load, that
`__wasm_call_ctors` must be called because the module is not a WASI
reactor, and why `run_lua` alone cannot drive a REPL.

Not done: `-r` on the main binary. Emitting the analysis report there
needs `analyze.c` in the amalgamation, since the debug build is a single
translation unit that does not link it. `luac` still has it.

## A cross-platform prompt

`bindings/rust/diluvium-repl` is a spike: `ego_cli`'s line editing driving
`drepl.c`'s REPL intelligence. It exists to answer two questions before
anything is committed to, and it answers both.

**Does the seam fit?** Better than expected: it needs no adapter at all.
`ego_cli::extend::Completion` is `{ start, end, candidates }`, and
`diluvium_repl_complete` returns the offset a replacement starts at with the
candidates on the stack -- so the completer is one call and a range. An
earlier draft added keywords and sorted the result on top, which was both
redundant and wrong: `drepl.c` already offers the keywords, already gates
them to a bare word, and already sorts, so the draft offered `for` as a
completion of `string.fo`. The test caught it and the fix was deletion.

Highlighting ports across as a classifier, `dline.c`'s `classify` arm for
arm, including the Diluvium syntax it colours separately (`$"`, `??`, `?.`,
`?[`, `~function`). One thing does not port: `dline.c` classifies and
reassembles by byte, which is safe there because it writes bytes back out.
Reassembling by byte in Rust turns `héllo` into `hÃ©llo`, and the invariant
`ego_cli` asks of a highlighter -- same printable characters, escapes added
-- is what caught it.

**What does Ctrl+C do?** It resolves itself, which was not obvious in
advance. `Session::read_line` sets raw mode on entry and restores it before
returning, the error path included, so evaluation always happens in cooked
mode: a real SIGINT is delivered during a chunk, where `lua.c`'s hook
interrupts it, and Lua's own `print` reaches a terminal whose line
discipline is on. At the prompt, in raw mode, Ctrl+C is a key press and
arrives as `ReadOutcome::Interrupted`. Both meanings land where they belong
without either side arranging it.

**Does it actually run anywhere but here?** Three of the four targets, and
run rather than only built -- the same fourteen tests, and the prompt itself
driven through a pipe on each:

| target | how | state |
| :--- | :--- | :--- |
| `x86_64-unknown-linux-gnu` | natively | 14/14, prompt runs |
| `x86_64-pc-windows-gnu` | mingw-w64, under wine | 14/14, prompt runs |
| `wasm32-wasip2` | wasi-sdk 27, under wasmtime | 14/14, prompt runs |
| `wasm32-unknown-unknown` | wasi-sdk 27 | builds; see below |

Windows is the one that matters most, because it is the platform Diluvium
has shipped to since `build.yml` grew its MSYS2 job and never had an editor
on: `dline.c` needs a termios and degrades to `fgets` there. This is arrow
keys, word motions, history and Tab completion on Windows, from the same
source as everywhere else.

WASI has no raw mode -- a component cannot reach the host's termios -- so
`Session` takes its line-at-a-time path there and the conveniences are
absent by construction, which is the platform's answer and not this
crate's.

**What does it cost?** Stripped, at the crate's release profile
(`opt-level = "z"`, LTO, `panic = "abort"`):

| | C interpreter | `dv-repl` |
| :--- | ---: | ---: |
| linux-gnu | 518 KB (`-O2`) | 928 KB |
| windows-gnu | -- | 851 KB |
| wasm | 572 KB (`-O2`), 340 KB (`-Oz`) | 759 KB |

Between 1.3x and 2.7x the C binary depending on which C build you compare
against, for the whole runtime plus an editor that works on three
platforms. An earlier estimate in this discussion said 4.5x, extrapolated
from `ego_cli`'s demo binary; that was wrong, because cross-crate LTO
removes most of what the demo's dependency graph carries.

There is no async runtime on native. `ego_cli`'s `runtime` feature is off
here, which takes `term::platform()` to `BlockingNative` -- crossterm's
blocking `read` and `std::io` for writes, so no future ever pends and
`futures_executor::block_on` drives the whole session. The native
dependency tree is 37 crates. That also answers a worry from when this was
still a proposal: an interpreter running `--task` already has a
cooperative scheduler, and there is no second one in the process now. WASI
keeps the feature, because `CookedStdio` is behind it.

**Two writers, one file descriptor.** Lua's `print` and `io.write` go
through C stdio; the session writes through Rust. On a tty C stdio is line
buffered and the two interleave correctly by luck; on a pipe it is fully
buffered, so an `io.write` with no newline stayed in C's buffer until exit
and came out after everything Rust had written since. `State::eval` flushes
C's streams before returning, and a test spawns the real binary with a real
pipe to keep it that way. Worth knowing generally: anything that puts Rust
and the C core on the same descriptor has this shape.

### What it is not, and what is still open

It is not `src/lua.c`. No argument handling, no script running, no
`--task`, no `LUA_INIT`, no history file. Those are the second half of the
question -- whether Rust should own the whole CLI or only the interactive
front end -- and this spike deliberately does not answer it.

- **The browser.** Two findings, one good and one blocking.

  The good one: `diluvium-sys` now links **wasi-libc** into the browser
  build. The obvious reading of "libc from the embedder" was that a page
  supplies 56 functions; it does not have to. Most of what Lua asks for --
  `snprintf`, `strtod`, `strftime`, the string family -- is pure
  computation referencing no syscall, so the linker takes wasi-libc's
  implementations and leaves only seventeen `wasi_snapshot_preview1` calls.
  `diluvium-repl`'s `browser` module answers those in Rust (`fd_write` onto
  the terminal, so Lua's `print` arrives there; everything facing a
  filesystem returns `ENOTCAPABLE`, which is the truth for a sealed
  instance), and the module then imports **nothing** but wasm-bindgen's own
  glue. `--allow-undefined` is gone with it, so an undefined symbol in the
  browser build is a link error again rather than an import nobody notices.

  What was *not* the answer, and is worth recording because it looks like
  it should be: compiling `src/wasm_stubs_unknown.c` into the Rust build.
  That file is not a shim library, it is the JavaScript artifact's whole
  embedder, and its semantics are wrong for this consumer -- `sprintf`
  returns an empty string, `printf` prints nothing, `malloc` is a
  fixed-size bump allocator, `setjmp`/`longjmp` trap, and it defines its
  own `global_L` and exports a competing REPL. Right names, wrong meanings.

  The blocking one: **wasm-bindgen cannot post-process this module.** It
  fails with "`__instance_terminated` global required for catch wrappers".
  The cause is a genuine conflict rather than a misconfiguration:

  1. The core is compiled with the wasm exception-handling proposal, which
     is what makes Lua's `setjmp`/`longjmp` -- and therefore `pcall` --
     catch rather than trap. `bindings/rust/WASM-SPIKE.md` records that as
     a deliberate departure from the JavaScript artifact, which stubs
     `setjmp` and accepts that a Lua error kills the module.
  2. That leaves a `tag` section in the wasm. ego-cli's own browser
     modules have none, which is why they post-process fine and this does
     not -- the section is the whole difference.
  3. wasm-bindgen sees the tags, takes its exception-aware path for catch
     wrappers, and wants a global only a Rust build with exception-handling
     enabled emits.
  4. Catch wrappers cannot be avoided from this side. Removing the
     `Result<_, JsValue>` return, making `start` synchronous, and dropping
     `ego_platform` and web-sys from the browser build were each tried;
     `wasm_bindgen_futures` and `js_sys` generate them regardless.

  Three ways out, cheapest first. Try a newer wasm-bindgen: the pin is
  `=0.2.114` only because `ego_platform` pins it, and this may already be
  fixed. Failing that, build Rust itself with exception-handling
  (`-C target-feature=+exception-handling` over `-Z build-std`, so the
  global exists) -- nightly, but principled. The third is to give up wasm
  EH in the browser and take the trapping `setjmp`, which is what the
  JavaScript artifact does today and is wrong for a prompt: every Lua error
  would kill the module instead of printing a message.

  Everything else is ready. `run` is generic over the terminal,
  `XtermTerminal::attach` takes the page's xterm.js object, and the tests
  need no browser-specific code at all -- `MemTerminal` is the same on
  every target, so the moment the module post-processes, the same fourteen
  run in a browser. ego-cli's `c038187` is the CI job to copy: it installs
  Firefox and geckodriver explicitly and names them, because the runner
  otherwise picks whichever driver is first on PATH, and it documents why
  Chrome needs a driver no newer than 141.

- **`ego_platform`.** `ego_cli` is pinned by revision here, but its own
  manifest tracks `ego_platform`'s main branch, so the committed lockfile
  is the only thing holding that still. Before anything shipping depends on
  this, that crate wants a release rather than a branch.
- **MSVC.** `diluvium-sys` builds for `*-pc-windows-gnu` and refuses MSVC by
  name; a Windows binary from this crate inherits that. The artifact
  `build.yml` already ships is MSYS2/MINGW64, so this is the same ABI.

## Analyzer

The determinism verdict (three-valued: deterministic / nondeterministic /
indeterminate) is the headline property and is specified in the handoff.
Two additions discussed and not yet built:

- **Boundedness verdict.** Same call-graph pass, same three values: is a
  function's execution statically bounded? A bounded function needs no
  instruction-count hook at all, so this is a throughput property rather
  than only a safety one.
- **Checkpoint lint.** An unbounded loop containing no cooperative
  checkpoint is exactly the shape that will hit an execution budget;
  the analyzer already walks loop structure and can say so.

Design note carried from that discussion: an instruction budget is
deterministic and replayable, wall-clock time is not. Anything that must
replay bit-exactly can be bounded by the former and must not be bounded by
the latter.

Programmer assertions about determinism should be recorded in the report,
not silencing — a verdict that hides what was assumed is worth less than
one that lists it. LuaCATS annotations are the cheapest home for these
(comments, so stock-Lua-valid by construction, and analyzer-only); new
syntax only if demand justifies it.

## Build statistics

`script/build_stats.sh` records what a build produced -- every artifact's
size, and `text`/`data`/`bss` for the native ones, which is what says
*where* growth came from rather than only that it happened. `compare`
prints the delta between two records and can fail over a threshold, so a
regression can be refused rather than merely logged.

`BUILDINFO.txt` already carried the commit and a checksum per artifact;
this adds the sizes and timings it never had. The record ships as
`BUILDSTATS.json`, a release asset, so every release is a permanent data
point and history needs no branch to push to. The build job compares
against the previous release and puts the table in its summary.

Sizes are only comparable within a toolchain, so each record carries the
compiler version. The wasi-sdk is pinned by digest; the native runner
images are not, so an unexplained jump is usually one of those moving.

## Open

- **The M0–M7 audit's confirmed defects — all fixed.** 35 confirmed, 32 refuted;
  roughly 24 distinct. Listed with consequences in `doc/Messaging.md` section 18,
  whose three release profiles are all done: the last block was profile C —
  hibernation at scale — and `doc/Hibernate.md` is the record of closing it.
  Hibernation is on by default; what remains from that arc is release
  engineering (the changelog entry and `stable: true`, §18.3), not runtime work.
- Contract calling convention, kernel framing, libm embedding — carried
  from the handoff, all still open.
- Float reduction order, needed before any vector work.
- A dataflow pass over the code (control-flow graph plus a known-table
  lattice), which would let the verifier prove the `OP_SETLIST` invariant
  statically instead of the VM enforcing it at run time, and which the
  boundedness verdict needs anyway. Aspirational; see "Aspirational: a
  dataflow pass, shared with boundedness" above. `luai_verifycode` itself
  (finding 8) is done.

## Known non-code issues

- **Readline was GPLv3 and statically linked into pre-5.5 released
  binaries**, which made those artifacts a combined work under GPLv3
  rather than the Apache-2.0 the repository advertises. Source
  availability was satisfied on both sides; disclosure was not. Resolved
  from `5.5.1_build1` on: `dline.c` replaced readline and the binary links
  nothing but libc and libm. The older release artifacts are still up and
  still affected.
