# 5.5.1_build7: stop cementing bad patterns

**Status:** built, 2026-08-13. Written as the plan the same day; Parts 1-4
landed in 5.5.1_build7 (the `host` guest library refined to
`host.sql.open(name)` + handle calls during the Part 4 rewrites, exactly as
§1 said the caps should drive it), and §5 remains the honest list of what
was *not* built. Supersedes the earlier "capability grab-bag" framing
(which is folded into Part 3 below).

The capability-testing pass did its job: it made the guest-facing surface's
problems impossible to ignore. The tests pass — Diluvium does ~95% of what
discofetch needs today — but writing *create a table, write a value, read it
back* currently takes a separate config file, a supervisor indirection, a
hand-rolled queue pair, token correlation, and magic `sql/exec` op-strings. That
directly undermines the accessibility pitch the whole project rests on.

**Build 7's real job is to stop cementing bad patterns:** make the code we write
*today* model the practices we want, fill the connector gaps we can now, and
honestly *document* the bigger changes rather than faking them.

**Priority order is load-bearing.** Do the parts in order. If the clock runs
out, it runs out at the bottom, and the acceptance gates (§6) keep whatever
landed honest and shippable. The capability tests (discofetch,
`capability_testing/`) are the benchmark throughout: a passing suite *rewritten
to the new surface* is the proof the ergonomics actually improved.

---

## 1. Make hostcalls humane — the `host` guest library

The single highest-value change, because it changes what all future code looks
like. Today a program reaches a connector by hand:

```lua
local calls   = queue.declare('host/calls',   { cap = 8, exported = true })
local replies = queue.declare('host/replies', { cap = 8 })
local function ask(req) queue.push(calls, req); local _, m = queue.wait({replies}, 5000); return m end
local m = ask({ tok = 1, call = 'sql/exec', args = { sql = 'INSERT ...', params = {a, b} } })
```

Build a `host` guest library, baked into the runtime like `json`/`bytes`/`time`,
that owns all of that — the queue pair, the `{cap=…}` sizing, the token
correlation, the hardcoded `host/calls`/`host/replies` names, the
request/reply shape. The program becomes:

```lua
host.sql.exec("INSERT INTO kv VALUES (?, ?)", a, b)
local rows = host.sql.query("SELECT v FROM kv WHERE k = ?", "demo").rows
local tok  = host.crypto.jwt_sign({ sub = "u1" }, 60)
```

Target API (refine against the rewritten caps; readability is the acceptance
test, not this table):

| call | returns / raises |
|---|---|
| `host.sql.exec(sql, ...params)` | `{changes, rowid}`; raises on denied/error/malformed |
| `host.sql.query(sql, ...params)` | `{cols, rows}`; raises on not-ok |
| `host.crypto.hash(data)` / `.hmac(data)` | hex string |
| `host.crypto.random(nbytes)` | hex string |
| `host.crypto.jwt_sign(claims, ttl?)` / `.jwt_verify(token)` | token / `{valid, claims?, reason?}` |
| `host.time()` | ms |

Design notes, non-negotiable:

- **It raises on a non-`ok` status by default**, with the connector's `detail`
  in the message — that is the readable default. Provide a pcall-friendly escape
  for programs that *expect* a denial (e.g. `host.sql.try_exec(...)` returning
  `value, status`), so the gate/denial path stays expressible.
- **It multiplexes.** One internal queue pair, tokens allocated internally, many
  logical calls — the "one pair of queues" rigidity was the *exposure*, not the
  mechanism. Declare the queues lazily on first use.
- **It is the seam a future `await` keyword drops into.** Keep the call sites
  synchronous-looking so that when the language grows real async syntax (Part 5),
  the library's internals change and no program does. Do not design anything that
  a keyword would later have to fight.
- **Implementation:** prefer pure Lua preloaded as a permanent if the runtime can
  bake a Lua chunk in as a named global (it only orchestrates `queue.*`); fall
  back to C if not. Either way it is a **new permanent → the permanents
  fingerprint moves** (see §7). Register it at all the sites `json` uses
  (`dlibs.c`, `onelua.c`, `src/makefile` `AUX_O`, `dsnap.c` `DS_MODULES`) and add
  it to `script/check_source_lists.py`'s expectations; `test/dsnap_check.c`
  gains a permanents case.

**Ship LuaCATS type defs for the guest globals** in the same part —
`queue`, `msgpack`, `endpoint`, `json`, `bytes`, `time`, and the new `host` —
as a `types/guest.lua` (mirror `host/types/host.lua`, which only covers the
*config*). Today an editor flags `json`/`queue` as unknown globals; that is a
pure tooling gap and it is cheap to close.

---

## 2. Config grants a *scope*, not application details

Today the sql connector config names an exact file — `path = "example.db"` — and
resolves it against the host's cwd. That is wrong twice: the program is locked to
one absolute file from launch, and the config is carrying an *application* detail
(which database) that belongs to the program.

Change the model so **config grants a place and the program names its resource
inside it**:

- The sql connector config grants a **scope** — a directory (or a single path).
  The program opens the DB it wants *within* that scope; the connector decides
  how to resolve a program-supplied name against the granted scope, and refuses
  anything that escapes it (the `realpath`-under-root discipline from the old fs
  spec). **Multiple databases fall out for free** — name more than one.
- **Path resolution stops being ambiguous.** There is no cwd-vs-config-file
  guessing left, because the program never supplies an absolute path and the
  scope is explicit. (Whatever the supervisor-file resolution ends up being,
  make it explicit and documented — the point the review raised was the
  *ambiguity*, not a specific base directory.)
- **Every option has a safe default, is omittable, and preallocates nothing.**
  A newcomer on a generic host must not have to read a wall of documentation to
  learn whether an option is safe to drop. Document each in the schema.
- **Rename the liars:** `max_rows` → `max_result_rows` (it is a per-*query*
  result cap, not per-transaction or per-database). Split `mode`'s two jobs — it
  currently means *both* the file open-mode *and* whether `sql/exec` is wired at
  all; separate the access grant from the open-mode.

This is the near edge of the `doc/Capabilities.md` scope/permission model:
capability = the connector, scope = the granted directory, program = names its
file. It also makes `fs` (Part 3) sane, since `fs` wants exactly the same
directory-scope discipline.

---

## 3. Fill the connector gaps we can (host-side, cheap now the pattern is set)

All host-side; none of these alone moves the fingerprint (Part 1 already did).
Each mirrors an existing connector.

### 3.1 `fs` — file I/O

`host/dhost_fs.c`, structured like `dhost_sql.c`, using Part 2's scope model:

```
fs/read  {path}                 -> the bytes           host:fs/read
fs/write {path, data, append?}  -> {bytes = n}         host:fs/write
```

- Resolve `path` under the granted scope, **`realpath` and require the scope as
  a prefix** (kills `..` and symlink escape). Escape is `DENIED`, not clamped.
- Refuse an embedded NUL; cap both directions at a configured max (a hostile read
  is a memory DoS, a hostile write fills the disk).
- `fs/write` exists only when the scope was granted read-write.
- Exposed through the `host` library as `host.fs.read/write`.

### 3.2 `exec` — the honest escape hatch

`host/dhost_exec.c`. Naive subprocess pass-through is easy; the surface is
*bounding* it, because the instruction budget can't (a subprocess runs outside
the VM):

```
exec/run {argv = {...}, stdin?, timeout_ms?, cwd?} -> {status, stdout, stderr}
```

- **Wall-clock timeout** (host-configured max, per-call ≤ it) and an **output
  cap**; a runaway child is killed at the deadline.
- No shell by default: `argv` is a vector, not a string.
- Replay: the result is a connector reply, logged and replayed like any other.
- **Docs say it plainly:** granting `exec` is leaving the sandbox
  (`doc/Capabilities.md` §5).

### 3.3 Request headers on the listener

The listener already parses every header; forward an allowlisted subset:

```
{conn, method, path, body, headers = { authorization = "...", ["content-type"] = "..." }}
```

Config names a lower-cased allowlist, empty by default; bound the count and each
value's length. Fixes the discofetch API's `Authorization` workaround
(`doc/LAB-PLAN.md`).

---

## 4. Rewrite the capability tests to the new surface — keep them the benchmark

In the discofetch repo (`capability_testing/`, branch
`claude/discofetch-assessment-ja6xis`):

- Rewrite cap2–cap5 to use `host.*` and the scoped config, so a passing suite is
  itself the proof the ergonomics improved. The programs should read like
  ordinary programs; if they don't, Part 1's API is wrong and should change.
- Add **cap6 (fs)** once 3.1 lands — same shape as cap2, exercising
  `host.fs.read/write` within a granted directory.
- The caps are validated against the real host binary the way the existing ones
  were (drive `diluvium-host`, assert results out of the shared SQLite). Keep
  them the dashboard: the human judges this whole build by *do the caps pass on
  the new surface, and do the programs read cleanly.*

---

## 5. Document what we are NOT building yet (so we don't lie about it)

Recorded on the roadmap, built later, never pretended:

- **Natural async syntax / an `await`-style keyword.** The real answer to
  "queues shouldn't be the only hostcall boundary." Queues stay the flexible
  substrate; the language grows a natural way to *await* a hostcall so the guest
  library (Part 1) can shed even its internal push/wait. Because we own the
  language, this is a real option, not a wish — but it is a language-design
  effort, not this build. Part 1 is deliberately the first step toward it.
- **Inter-instance messaging through the swarm** (cap4 gap): the swarm never
  mints or delivers endpoint references to guests, so siblings coordinate through
  a shared connector today. Wiring the endpoint resolver through the swarm is
  real work.
- **Listener routing to a non-root instance** (cap5 gap): `http_in` lands on the
  root; an API can't be served by a spawned child yet.
- **`net`** — outbound HTTP/TCP connector; among other things, what a guest-side
  endpoint tester (cap5) needs, and what any deployment calling a REST API
  wants. Its scope-type is the point of building it: a grant names **which
  hosts may be reached** (`doc/Capabilities.md` §1 — `net` takes a CIDR, and
  a domain allowlist for the HTTP verb on top), so outbound reach is a stated,
  auditable part of a deployment rather than a property of whatever the
  platform happens to make reachable. Until it exists, a deployment that needs
  an outbound request reaches for `exec` and an HTTP client — which works, and
  is a wider grant than the one being asked for. That gap is the argument for
  the connector, and it is why `net` ranks above the rest of this list.
- **Installer ships the host** (`--host`/`--full`) + publishes `diluvium-host` as
  a release asset — the distribution gap that makes `curl … | sh` yield a runtime
  that can't do half of what the docs describe.
- **`env`**, **`log`** — smaller connector candidates.

---

## 6. Acceptance gates for the run (so it cannot drift)

Hard gates. Nothing merges past one that is red.

1. **The existing test suite stays green** (`make test`, `make host_check`, the
   fuzzers). Snapshot-format work is done **once** and carries a build6→build7
   upgrade note; the drift guard (`script/check_source_lists.py`) passes.
2. **The capability tests pass, rewritten to the new surface**, validated against
   the real host binary. This is the primary human-facing signal.
3. **Every doc claim matches the code.** No aspirational documentation — if it
   isn't built, it's in §5, not described as done.
4. **The changelog build7 entry** is written and consistent with VERSION,
   `.technoproj`, `lua.h`, `LUAC_FORMAT` (see `script/changelog.py`), matching
   how build6 was cut.

---

## 7. Snapshot boundary — done once, deliberately

Part 1 adds a guest global, so the **permanents fingerprint moves** — build 7 is
a snapshot-boundary build, exactly as build6 was when it added
`json`/`bytes`/`time`. That path is proven; follow it:

- Because we cross the boundary anyway, land **every** guest-surface change in
  this one build so we only cross it once.
- `DILUVIUM_SNAP_FORMAT`, `DS_THREAD_VERSION`, `LUAC_FORMAT` change only if a
  format actually changes — adding a permanent moves the *fingerprint*, not
  necessarily those constants. Check against build6's changes and do the minimum.
- Ship the build6→build7 note: a build6 snapshot does **not** restore on build7
  (fingerprint moved); say so where build6's note lives.

Parts 2 and 3 are host-side and do **not** move the fingerprint on their own.
