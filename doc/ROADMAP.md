# Diluvium Roadmap

Scope: the standalone runtime and compiler in this repository. Host
embedding, capability plumbing and anything downstream of them are tracked
elsewhere and deliberately kept out of this file.

This document exists so the state of the fork is readable from the tree
rather than reconstructed each time. Update it in the same commit as the
work it describes.

## Verified state

Against `v5.5.1_rc1` (Lua 5.5.1 fork point `7579fc9`), suite green at
34 passed / 0 failed / 6 skipped.

### Language

| Feature | State |
| :--- | :--- |
| String interpolation `$"...{expr}..."` | done |
| Null coalescing `??` | done, short-circuits, no dedicated opcode |
| Secure (obfuscated) functions `~function` | done |
| Compound assignment `+=` etc. | done (no `~=`; see below) |
| Safe navigation `?.` / `?[` | not started |
| `switch` statement | done |
| `match` (switch as an expression) | not started |
| `defer` / `with` | not started |
| F-string format specs `{x:%.2f}` | not started |
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

Ordered. Each item is independently shippable.

1. **`defer`.** Desugars to a to-be-closed local with a `__close` wrapper,
   so unwind ordering is inherited rather than implemented.
3. **Safe navigation `?.` / `?[`.** Cheap now that `??` compiles as a
   branch; same test-nil-and-skip shape.
4. **F-string format specs.** `{x:%.2f}` mapping to `string.format`.

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
