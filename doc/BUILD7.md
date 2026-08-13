# 5.5.1_build7: file I/O through the boundary, and request headers

**Status:** plan, written 2026-08-13. Target: **under an hour, host-side only.**
**Open on purpose:** the sections below close the two gaps we already know.
Capability testing comes next and sets the rest; §4 is the slot for what it
finds. Do not build past §1–§2 until that conversation happens.

---

## 0. Why this build, stated plainly

File I/O is the textbook motivation for the hostcall boundary — a sealed guest
cannot open a file, so it *asks* the host, which mediates by capability, confines
the request, and logs the answer for replay. We built the connectors *around*
that shape — `time`, `crypto`, `sql`, `listen` — and never built the file
connector itself. So today a guest's only persistence is the `sql` connector
against **one operator-pinned database** (`connectors.sql.path`, chosen in the
`.host.lua` config, invisible to the guest): a narrow SQL interface, not file
I/O. That was specified early and slipped because it wasn't written down.

The second gap is smaller and already filed: the listener packs
`{conn, method, path, body}` with **no headers**, so `Authorization` never
reaches a guest (`doc/LAB-PLAN.md` upstream ask #2, worked around in the
discofetch notebook with a `?auth=`/`body.auth` stopgap).

This build closes both.

## 1. The enabling property that makes an hour realistic

**Both changes are host-side (`host/`), not runtime.** Neither adds a guest
global, a C function reachable from a program, or a permanent — so the
**permanents fingerprint does not move**, and a build6 snapshot restores on
build7 unchanged. `DILUVIUM_SNAP_FORMAT`, `DS_THREAD_VERSION` and `LUAC_FORMAT`
all stay put. No snapshot-format work, no fingerprint churn, no
version-boundary upgrading notes — the opposite of build6, which moved the
fingerprint when `bytes`/`json`/`time` joined the permanents set.

Both also follow patterns already in the tree, which is the other half of the
hour: the file connector is `host/dhost_sql.c` with a different verb, and the
headers change is a handful of lines in `host/dhost_http.c` where the headers
are *already parsed*.

---

## 2. In scope

### 2.1 The file connector: `host:fs/*` — the headline

A new `host/dhost_fs.c`, structured exactly like `dhost_sql.c` (config parse →
`dh_fs_open`/`dh_fs_close` → a `conn_fs` gated by `dvs_holds`). Calls, kept
minimal on purpose:

```
fs/read  {path}                 -> the file's bytes (a string)     host:fs/read
fs/write {path, data, append?}  -> {bytes = n}                     host:fs/write
```

`fs/stat`, `fs/list`, `fs/remove`, `mkdir`, directory ops — **deferred to §4.**
Read and write are the two that unblock everything and cost the least to confine.

**Config** (`connectors.fs` in the `.host.lua`, typed in `host/types/host.lua`):

```lua
fs = {
  root     = "/var/lib/app/files",  -- required: the jail; every path resolves under it
  mode     = "read",                -- "read" (default) leaves fs/write unwired, like sql
  max_bytes = 1048576,              -- refuse a read or write past this; the row-cap analogue
}
```

**Confinement is the load-bearing part**, and it is `sql`'s authorizer lesson in
a different domain: the write-gate answers "does it change data," the jail
answers "does it reach outside the one place it is allowed." Non-negotiable:

- Resolve the guest's `path` against `root`, then **`realpath` the result and
  require `root` as a prefix** — this refuses `..` traversal *and* a symlink
  that points outside. A path that escapes is `DENIED`, not clamped.
- Refuse an embedded NUL (the `sql` connector's lesson: a NUL ends the string
  early for one API and not another).
- Refuse an absolute guest path, or resolve it strictly under `root` — pick one
  and say which; refusing is simpler.
- Cap both directions at `max_bytes` (a hostile `fs/read` of a huge file is a
  memory DoS; a hostile write fills the disk).
- `fs/write` exists only when `mode = "readwrite"`, so a grant of `host:fs/read`
  is exactly and only that.

**Replay**, stated in a code comment as `sql` does: an `fs/read` reply is in the
message log, so a replay replays the bytes — it does not re-read the disk. An
`fs/write` is a side effect *outside* the replay boundary, exactly like
`sql/exec`; the pump's existing discipline (never drain a request you cannot
answer, never re-run a stateful connector on retry) already covers it, so no new
machinery.

**Lab (JS host):** a mock `fsConnector(root)` over an in-memory or
IndexedDB-backed map, same jail and same `max_bytes`, per `doc/LAB-PLAN.md`
§W3 — a guest must not tell the hosts apart. Honest ceiling, same as `sql`: the
JS mock enforces the *contract*, and its jail is a string check rather than a
kernel one.

**Test:** a `host_check.c` case in the established shape — a guest reads a
seeded file, writes and reads back, a `..` escape is denied, a write under a
`read` mode is denied, an oversized read is refused.

### 2.2 HTTP request headers

The listener already parses every header (`try_parse` walks the `phr_header`
array for `Content-Length`). Forward an **allowlisted** subset into the message:

```
{conn, method, path, body, headers = { authorization = "...", ["content-type"] = "..." }}
```

- **Config:** `connectors.listen.headers = {"authorization", "content-type"}` —
  an allowlist of lower-cased names. Default empty; a guest sees a header only
  because the deployment named it. Everything else is dropped — arbitrary
  header pass-through is a smuggling and DoS surface, and the guest's view
  should be the minimum it asked for.
- **Bound** the count forwarded and each value's length (a header the LB should
  have capped is still the LB's job, but the host does not hand a guest an
  unbounded map).
- Names lower-cased so a guest matches on one spelling.
- **Lab:** the JS listener mock carries the same `headers` field.
- **Test:** the injection/smuggling `host_check` case gains an assertion that a
  non-allowlisted header does not appear and an allowlisted one does.

---

## 3. The hour

| Step | Mirrors | ~ |
|---|---|---|
| `dhost_fs.c` + config block + register + types | `dhost_sql.c`, its config parse, `host/types/host.lua` | 30m |
| listener `headers` (allowlist in `try_parse`/`deliver`, config) | `dhost_http.c` | 15m |
| `host_check` cases (fs round-trip + confinement; a headers assertion) | existing host_check | 5m |
| build7 changelog entry + `VERSION`/`.technoproj` bump; `consistency` green | build6 flip | 5m |

The changelog entry is short and says the quiet part: **host-side only,
snapshot-compatible with build6, no fingerprint move, `LUAC_FORMAT` unchanged.**

---

## 4. Deliberately open — for after capability testing

Capability testing is the next conversation and it decides everything below.
Listed so they stay decisions rather than drift, **not** so they ship in build7:

- **`fs` breadth:** `stat` / `list` / `remove` / `mkdir` / `append`-only /
  atomic replace? One `root` jail, or several named mounts? A per-guest subtree
  keyed off capability (`host:fs/read:sessions/*`)?
- **Is `fs` even the right shape, or does `sql` + blob columns cover the real
  need?** Capability testing answers this before a line of `dhost_fs.c` is
  written — do not build it speculatively.
- **Response headers:** should a guest set arbitrary response headers, or is the
  `content_type` field enough? (Same allowlist reasoning, other direction.)
- **The rest of the textbook-hostcall audit:** environment variables? outbound
  HTTP as a connector (the counterpart to the inbound listener)? a `log`
  connector vs. the exported-queue pattern? subprocess — **never**, by the same
  argument that seals `os`. Capability testing is exactly the exercise that
  turns this list into a spec.

---

## 5. What build7 does not touch

No runtime, no ABI, no snapshot format, no guest sandbox surface (no new
globals; `io`/`os`/`package` stay sealed — file access arrives *only* through
`host:fs/*`, which is the whole point). `LUAC_FORMAT` stays `0x46`. A build6
snapshot restores on build7. The only new surface is two host-side connectors'
worth of config and calls.
