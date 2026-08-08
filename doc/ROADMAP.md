# Diluvium Roadmap

Scope: the standalone runtime and compiler in this repository. Host
embedding, capability plumbing and anything downstream of them are tracked
elsewhere and deliberately kept out of this file.

This document exists so the state of the fork is readable from the tree
rather than reconstructed each time. Update it in the same commit as the
work it describes.

## Verified state

Against `v5.5.1_build1` (Lua 5.5.1 fork point `7579fc9`), suite green at
39 passed / 0 failed / 6 skipped locally, on linux-x86_64 and macos-arm64
in CI.

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
| 8 | `luai_verifycode` empty | **open** |
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

- Contract calling convention, kernel framing, libm embedding — carried
  from the handoff, all still open.
- Float reduction order, needed before any vector work.
- `luai_verifycode`, per finding 8 above -- now measured, and the largest
  open item on the runtime.

## Known non-code issues

- **Readline was GPLv3 and statically linked into pre-5.5 released
  binaries**, which made those artifacts a combined work under GPLv3
  rather than the Apache-2.0 the repository advertises. Source
  availability was satisfied on both sides; disclosure was not. Resolved
  from `5.5.1_build1` on: `dline.c` replaced readline and the binary links
  nothing but libc and libm. The older release artifacts are still up and
  still affected.
