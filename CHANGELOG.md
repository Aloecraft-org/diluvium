# Changelog

All notable changes to Diluvium are recorded here.

Generated from `CHANGELOG.yaml`, which is the source of truth --
edit that file, then run `script/changelog.py generate`.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Note that tags carry suffixes (`_release`, `_build1`) because this
repository also holds upstream Lua's tags, and a bare `v5.4.7` is
Lua's rather than Diluvium's.

## [5.5.1_build2] - unreleased

`v5.5.1_build2` &middot; Lua 5.5.1 &middot; bytecode format `0x45`

A security release for the 5.5 line. Secure (`~`) functions did not
hide string constants that ordinary code in the same chunk also
used, so a shared literal was stored in the dump in the clear.

Only the 5.5 line is affected. Lua 5.5 introduced the saved-string
table this depends on, so 5.4.7 and earlier never had it.

### Added

- `test/test_secure_dump.lua`, covering the dump-level security
  property directly: shared literals, both sibling orders, both sides
  of the short-string boundary, debug names, stripped and unstripped,
  and back-references -- and that all of it still round-trips once a
  string is hidden away from the function that hid it.
- CI's obfuscation audit now shares a literal between a secure
  function and ordinary code, and checks the unstripped dump too.
  Its previous probe used markers only inside the secure function and
  so could not have caught this.

### Fixed

- The bytecode loader leaked memory on every malformed chunk it
  refused.

  `loadString` held a secure function's scratch buffer in a local
  across `loadVector`, which raises on a truncated chunk, so the
  buffer was never freed. A program refusing untrusted bytecode in a
  loop leaked without bound. Short strings now unscramble in a local
  buffer and long ones in the string's own storage, so there is no
  allocation to lose. Present since the 5.5.1 rebase.
- A corrupt string size is rejected by name instead of being acted on.

### Security

- Secure functions no longer leak strings that ordinary code shares.

  Lua 5.5 writes each distinct string in a chunk once and refers back
  to it by index. Scrambling decided by a function's position in the
  prototype tree stops being sound under that, because a string
  shared between a secure function and ordinary code has exactly one
  stored copy -- at whichever site was written first. A function
  dumps its own constants before recursing into its children, so a
  literal shared with the enclosing function was always stored
  unscrambled, whatever the source order:

      local tag = "license-key";
      ~function check() return "license-key" end

  `strings` found it. Among siblings it depended on which was dumped
  first, so the same source leaked or did not depending on where else
  a literal happened to appear. It round-tripped correctly either
  way, which is why nothing failed: the function was marked secure
  and the bytes simply were not there.

  The dump now walks the whole prototype tree before writing
  anything and marks every string a secure function will contribute;
  a marked string is scrambled wherever it is written. A consequence
  worth knowing: a literal shared with a secure function is hidden in
  the ordinary function's copy too. There is only the one copy, and
  the secure function decides.
- `LUAC_FORMAT` moves from 0x44 to 0x45.

  Because a string's secrecy is no longer implied by its position, a
  written string's size field now carries a scramble flag in its low
  bit. That is a format change, so the byte is bumped and stale
  chunks are refused rather than misread.

### Upgrading

Bytecode compiled by 5.5.1_build1 will not load in 5.5.1_build2, and
the reverse. Recompile any `.luac` produced by build1. Source is
unaffected, and nothing about the language changed.


## [5.5.1_build1] - 2026-08-07

`v5.5.1_build1` &middot; Lua 5.5.1 &middot; bytecode format `0x44`

First release of the 5.5 line, and the one that brings the language
work: `switch`, `defer`, `with`, safe navigation, compound
assignment and f-string format specifications, plus a rewritten
interactive interpreter that no longer depends on GNU readline.

Named `5.5.1_build1` rather than a release candidate to stay out of
upstream Lua's version space -- upstream will never ship that
suffix, so a Diluvium build cannot be mistaken for one.

### Added

- `switch` statement. The subject is evaluated once, a case can list
  several values, and there is no fallthrough.

  It is a contextual keyword, so existing code using `switch` as a
  name keeps working. The one consequence is that a subject cannot
  start with `(`, a string or a table constructor, because
  `switch (x)`, `switch "s"` and `switch {}` are all calls in stock
  Lua and have to keep meaning that.
- `defer` and `with`, for scope-bound cleanup.

  `defer stat` runs a statement however the block is left. `with
  NAME = expr do ... end` binds a to-be-closed local scoped to the
  block. Both desugar to Lua's to-be-closed variables, so ordering
  and unwinding are inherited rather than invented: they run in
  reverse declaration order on end of block, `break`, `goto`,
  `return`, an error, and `coroutine.close`.
- Safe navigation, `a?.b` and `a?[k]`.

  Once the value left of a `?` is nil the whole remaining chain is
  skipped -- nothing further is indexed or called -- and the result
  is nil. It tests nil, not falsiness, so `false?.x` still raises.
- Compound assignment: `+=`, `-=`, `*=`, `/=`, `//=`, `%=`, `^=`,
  `|=`, `&=`, `<<=`, `>>=`, `..=` and `??=`.

  The target's prefix is evaluated once, so `t[next_key()] += 1`
  calls `next_key` a single time. There is no `~=` form, since that
  already means "not equal".
- F-string format specifications, `$"{value::%.2f}"`.

  Everything after `::` is handed to `string.format`. It is `::` and
  not the `:` other languages use because `:` already introduces a
  method call, and `$"{obj:method()}"` has to keep meaning that.
- A built-in line editor: syntax highlighting as you type, tab
  completion through tables and metatable `__index`, and persistent
  history in `~/.diluvium_history`.

  `NO_COLOR`, `DILUVIUM_NO_COLOR` and `TERM=dumb` turn highlighting
  off. `diluvium -h` lists the options and `help()` explains what
  Diluvium adds to Lua.
- The REPL's intelligence is now shared with every front end.

  Telling unfinished input from broken input, echoing expressions and
  completing names live in `drepl.c` -- on-top code using only the
  public C API -- so the interpreter and the WASM host run one
  implementation. The WASM builds export `repl_eval`, `repl_complete`
  and `reset_lua`. Before this a browser front end could only tell
  "keep typing" from "that's wrong" by matching the text of Lua's
  error messages.
- `doc/repl-reference.html`, a working browser REPL and the reference for driving the WASM build.
- Build statistics. `script/build_stats.sh` records every artifact's
  size plus `text`/`data`/`bss` for the native ones, which is what
  says where growth came from rather than only that it happened.
  `BUILDSTATS.json` ships as a release asset, so every release is a
  permanent data point.
- `doc/Analyzer.md`, describing what the bytecode analyzer computes today and the four limits that shape it.

### Changed

- Rebased onto Lua 5.5.1, from 5.4.7.

  Diluvium's changes to the Lua sources are held to 14 files, checked
  against pristine upstream by `script/patch_series.sh` in CI, which
  is what makes a rebase of this size tractable.
- F-strings rewritten.

  Escape handling is now shared with plain strings, so `\x`, `\ddd`,
  `\u` and `\z` mean the same thing inside an f-string as outside --
  they were silently wrong before. Nesting works, interpolation
  resolves `tostring` through `_ENV` so a sandbox can control it, and
  a whole f-string compiles to a single N-ary `CONCAT` rather than a
  chain of pairwise ones.

### Removed

- GNU readline.

  It was GPLv3, which made the released binaries a combined work
  under GPLv3 rather than the Apache-2.0 the repository advertises,
  and statically linked it was larger than the whole runtime. The
  replacement is `dline.c`, written rather than vendored so the tree
  stays under one licence. The binary now links only libc and libm,
  and grew 13 KB for it.

### Fixed

- F-string nesting and escape sequences (handoff findings 6 and 7).
- `$` no longer falls through to create `$`-prefixed identifiers.

### Known issues

- Secure functions leak string constants that ordinary code in the
  same chunk also uses. Fixed in 5.5.1_build2; see that entry. A
  `~function` whose literals are unique to it is unaffected.

### Upgrading

Bytecode from the 5.4 line will not load: this is a different Lua
release with a different bytecode format. Recompile. Source is
backward compatible, as ever -- every new construct is a syntax
error in stock Lua and none of them takes a reserved word.


## [5.5.1_rc1] - 2026-08-06 (prerelease, tagged, not published)

`v5.5.1_rc1` &middot; Lua 5.5.1 &middot; bytecode format `0x44`

The rebase itself, tagged but never published as a release. Recorded
here because it is where the 5.5.1 base came from; the language work
landed afterwards, in 5.5.1_build1.

### Changed

- Rebased Diluvium onto Lua 5.5.1, inheriting upstream's changes since 5.4.7.
- Platform builds fixed for the new base.

### Fixed

- `files.lua` guards its Linux-only `/dev/full` sub-test, which is not portable to CI.


## [5.4.7] - 2026-08-05

`v5.4.7_release` &middot; Lua 5.4.7 &middot; bytecode format `0x44`

The first Diluvium release proper, carrying string interpolation,
null coalescing, secure functions and the bytecode analysis report,
and the round of correctness work that made them trustworthy.

### Added

- String interpolation, `$"Hello, {name}!"`.
- Null coalescing `??`, which short-circuits and needs no dedicated opcode.
- Secure functions, `~function`, whose contents are obfuscated at rest in the dump.
- A bytecode analysis report, `diluvium_compiler -r`, for reading what a compiled chunk will do.
- `script/patch_series.sh`: generates Diluvium's patch series against
  pristine upstream Lua and fails CI when a core file diverges
  without being allowlisted. This is what keeps the fork rebaseable.
- Reworked CI. Every workflow can be run by hand against any commit,
  and releases are opt-in -- a manual Release run defaults to
  `publish: false`, so it is a full rehearsal that leaves the
  Releases page untouched.

### Changed

- `LUAC_FORMAT` set to 0x44.

  Diluvium bytecode is not loadable by stock Lua, so it now carries
  its own format byte. Stock Lua rejects a Diluvium chunk at the
  header instead of misparsing it, and the reverse.
- The string hash seed is fixed, so `pairs` iteration order is deterministic across runs.

### Fixed

- `local ~function` silently created a global instead of a local.
- Obfuscation covered only about a quarter of a secure function: the scramble length was an instruction count where it needed to be a byte count.
- Closures nested inside a secure function were not obfuscated; `is_encrypted` now propagates to them.
- `??` evaluated both operands. It now compiles as a branch, and `OP_2Q` is gone.

### Known issues

- GNU readline is statically linked into these binaries and is
  GPLv3, which makes the published artifacts a combined work under
  GPLv3 rather than Apache-2.0. Source availability is satisfied on
  both sides; what is missing is disclosure, which is what this note
  is. Resolved from 5.5.1_build1, where readline left the build.


## [5.4.7-rc4] - 2026-01-21 (prerelease)

`v5.4.7_rc4` &middot; Lua 5.4.7 &middot; bytecode format `0x0`

The last of four January release candidates -- `v5.4.7_rc`,
`v5.4.7_rc2`, `v5.4.7_rc3` and `v5.4.7_rc4`, which differ from one
another only in build fixes and shipped no notes.

They are deliberately excluded from the release mirror. They predate
`SHA256SUMS.txt`, so nothing about them can be verified after the
fact, and they use the pre-WASI asset names, so current tooling
cannot load them. They are left published as history rather than as
something to download.

### Known issues

- Bytecode carries the stock Lua format byte (0) despite not being
  loadable by stock Lua, so a mismatched chunk is misparsed rather
  than refused. Fixed in 5.4.7.
- Secure functions obfuscate only part of their contents, and not nested closures at all. Fixed in 5.4.7.
