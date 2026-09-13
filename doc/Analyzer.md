# The Diluvium analyzer

`src/analyze.c` reads a compiled `Proto` and describes what it does, as
JSON. `diluvium_compiler -r` writes that report; `diluvium_generate_report`
is the same thing behind the C API, and the WASM builds export it.

This document is what it computes **today**, so the determinism verdict can
be reviewed as a design rather than discovered as a diff.

## Why bytecode and not source

The analyzer runs after compilation, over `Proto`. That has two
consequences worth stating plainly.

It sees through Diluvium's syntax for free. `switch`, `defer`, `with`,
compound assignment, safe navigation and f-strings all desugar to ordinary
instructions before it looks, so none of them needed analyzer work. A
front end that parsed source would have needed teaching about each one.

It also sees a secure function's contents. `~function` is obfuscated at
rest in the *dump*, not in memory, so the report can describe a function
whose source is unreadable in a text editor. That is the point of the
report: it is how you find out what a chunk you were handed will do.

## What it produces

Per function, in `FunctionInfo`:

| Field | What it is |
| :--- | :--- |
| `name` | source and line, e.g. `@file.lua:12` |
| `params`, `param_names`, `vararg` | the signature |
| `upvalues` | names, so `_ENV` capture is visible |
| `constants` | the constant table |
| `calls` | call sites, **with the callee resolved where possible** |
| `reads` | table-field reads, as `table.field` pairs |
| `closures` | where closures are created, and how many upvalues each takes |
| `returns_table`, `return_shape` | what the function returns |
| `children` | indices of nested prototypes |

Plus, for the chunk as a whole, `globals_after_load`: which globals a
chunk defines, split into functions and variables. That is the interface
question — what appears in `_ENV` once this runs.

## How it gets there

Three passes' worth of machinery, all in one walk:

**Instruction decoding.** `analyze_function` switches over every opcode.
This is the part a Lua version bump touches: the switches are exhaustive,
so a new or renumbered opcode has to be accounted for.

**Backward register tracing.** `find_reg_source(f, pc, reg)` walks
*backwards* from a program counter to find the instruction that last wrote
a register. `find_newtable_for_reg` is the same idea for table
constructors. This is what turns "call whatever is in R3" into "call
`string.format`", and it is the piece the determinism work reuses: it is a
small dataflow analysis already.

**Callee resolution.** `resolve_callee` combines the two: from a call
site, trace the register back to its `GETTABUP`/`GETFIELD`/`MOVE` origin
and name it. That is how `calls` ends up with real names instead of
register numbers, and it is why a taint pass can ask "does this function
call `math.random`" and get an answer.

**Return classification.** `classify_return` decides whether a function
returns a table, and what shape.

## Its limits

Worth knowing before designing on top of it:

- **Dynamic access is opaque.** `math[name]`, `load`, a metatable's
  `__index` — `resolve_callee` cannot name any of those. This is the main
  reason the determinism verdict must be *three*-valued: anything it
  cannot follow has to be `indeterminate`, never a false `deterministic`.
- **It is intraprocedural per function**, with a `children` list. There is
  a call graph in the report, but nothing walks it transitively yet. The
  determinism verdict needs that propagation added.
- **No control-flow graph.** Jumps are decoded but basic blocks are not
  built. That matters for the boundedness verdict, which needs to
  recognise loops.
- **It analyses one chunk.** `require` is a call like any other.

## What the determinism verdict adds

Specified in the handoff; recorded here against the code above.

1. **Taint sources.** Mark the calls that make a function
   nondeterministic: `math.random`, `os.*` (clock, date, time), `io.*`,
   host calls, and float-to-string formatting. Note `sqrt`, `floor`,
   `ceil`, `abs` and `fmod` are IEEE-exact and stay clean.

   **The float transcendentals are no longer on this list.** They were:
   `math.exp`, `log`, `pow`, `sin` and the rest were taint sources
   because the platform's libm answered them, and glibc, musl, Apple's,
   wasi-libc's and mingw's are five correct implementations that disagree
   in the last bit — measured at 1,695 of 32,000 sampled arguments
   against glibc, so about one call in twenty. Stage 1 of the numeric
   spec removed the reason: with the `numeric` feature these route
   through the embedded libm (`src/dlibm.c`, openlibm vendored under
   `dv_` prefixes), which is one implementation on every target. A
   contract that calls `math.exp` can carry a green verdict, which is
   what that stage was for.

   Two caveats the verdict has to carry when it is built:

   - The clean list is `exp log pow sin cos tan asin acos atan atan2
     sinh cosh tanh expm1 log1p`, and only under a build carrying the
     feature. `dv_features()` is how a tool asks; without it the
     platform's libm is back and so is the taint.
   - `math.fmod`, `math.modf`, `math.ldexp` and `math.frexp` were never
     tainted and are not routed, because they are exact.
2. **Propagation.** Walk the existing call graph so a function that calls
   a tainted function is itself tainted.
3. **Three values.** `deterministic`, `nondeterministic`, `indeterminate`
   — the third for anything dynamic, per the limits above.
4. **Runtime half.** Wrap the flagged stdlib entries at init so a
   per-state flag records what an execution *actually* touched. Static
   says *could*, runtime says *did*, and nothing is forbidden either way:
   the verdict is reported, not enforced.

   Half of this exists. `dv_numeric_touched_fast` (`dv.h`) is the
   per-instance, sticky, one-way flag for the numeric half: a kernel that
   runs at `DV_TIER_FAST` sets it, a host reads it, and nothing clears
   it. No fast backend exists yet — every portable kernel is reproducible
   tier by construction — so what is wired today is the path, proved end
   to end in `test/dv_check.c` through a debug-build-only hook. The
   remaining stdlib entries (`math.random`, `os.*`, `io.*`) need the same
   treatment and do not have it.

The **boundedness verdict** discussed alongside it — is a function's
execution statically bounded? — reuses the same walk but needs the
control-flow work above, since it has to find loops. A bounded function
needs no instruction-count hook at all, which makes it a throughput
property as much as a safety one.

## Where the work would go

All of it is on-top. `analyze.c` is already outside the core patch series,
and the runtime wrappers are ordinary C registered by the embedder. The
determinism verdict adds nothing to the 14 files in
`script/patch_series.sh` — worth confirming with `patch_series.sh check`
when the time comes.
