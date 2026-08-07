# Diluvium Roadmap

Scope: the standalone runtime and compiler in this repository. Host
embedding, capability plumbing and anything downstream of them are tracked
elsewhere and deliberately kept out of this file.

This document exists so the state of the fork is readable from the tree
rather than reconstructed each time. Update it in the same commit as the
work it describes.

## Verified state

Against `v5.5.1_rc1` (Lua 5.5.1 fork point `7579fc9`), suite green at
36 passed / 0 failed / 6 skipped locally. CI last verified the tree at
`dba073e` (run 31144117057, 34 tests) on linux-x86_64 and macos-arm64;
`defer` landed after that and is verified on the run for its own commit.

### Language

| Feature | State |
| :--- | :--- |
| String interpolation `$"...{expr}..."` | done |
| Null coalescing `??` | done, short-circuits, no dedicated opcode |
| Secure (obfuscated) functions `~function` | done |
| Compound assignment `+=` etc. | done (no `~=`; see below) |
| Safe navigation `?.` / `?[` | done |
| `switch` statement | done |
| `match` (switch as an expression) | not started |
| `defer` | done (`with` not started) |
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
| 5 | Bytecode format byte not bumped | fixed (`LUAC_FORMAT` 0x44) |
| 6 | F-string nesting and escapes | fixed |
| 7 | `$` fallthrough created `$`-prefixed identifiers | fixed |
| 8 | `luai_verifycode` empty | **open** |
| 9 | String hash seed nondeterministic | fixed |

Finding 8 is stock Lua behaviour, and only becomes a hard requirement once
compiled chunks are accepted as untrusted input. It is not a blocker for
the standalone runtime.

### Core patch series

14 files, enforced by `script/patch_series.sh check` in CI. Doctrine is
that this stays as small as it can be: anything expressible as a library
or an embedder file does not belong in `src/`.

## Next

Nothing from the original list is outstanding. Remaining, unscheduled:

- **`match`** -- switch in expression position.
- **`with`** -- the other half of the `defer`/`with` pairing.
- **Literal suffix registry** (`1.23d`), gated on decQuad semantics.
- The analyzer work below.

`match` -- switch in expression position -- is deliberately separate from
the statement form and unscheduled; the statement carries the README
promise on its own.

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

## Open

- Contract calling convention, kernel framing, libm embedding — carried
  from the handoff, all still open.
- Float reduction order, needed before any vector work.
- `luai_verifycode`, per finding 8 above.

## Known non-code issues

- **Readline is GPLv3 and is statically linked into the released
  binaries**, which makes those artifacts a combined work under GPLv3
  rather than the Apache-2.0 the repository advertises. Source
  availability is already satisfied on both sides; what is missing is
  disclosure. This resolves itself when line editing moves host-side and
  readline leaves the build — until then the release notes should say so.
