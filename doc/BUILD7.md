# 5.5.1_build7: a capability grab-bag

**Status:** plan, written 2026-08-13, revised as a grab-bag.
**Shape:** host-side capability *mechanics*, snapshot-compatible, triaged after
capability testing — we build the guts, then decide what actually ships once the
container testing pass tells us what we need.

This build implements *capabilities*; it does **not** touch how grants are
*expressed*. The expression model — capability / permission / scope, one config
shape at every depth, the end of `.host.lua` — is `doc/Capabilities.md`, a
separate and larger effort. The guts built here (a jail, an executor, a header
allowlist) survive that redesign unchanged, which is why building them now on the
current plumbing is not throwaway.

---

## 1. The property that makes this cheap

Everything below is **host-side** (`host/`), not runtime. Nothing adds a guest
global, a C function a program can reach, or a permanent — so the **permanents
fingerprint does not move**, and a build6 snapshot restores on build7 unchanged.
`DILUVIUM_SNAP_FORMAT`, `DS_THREAD_VERSION` and `LUAC_FORMAT` all stay put. No
snapshot-format work, no upgrading notes — the opposite of build6.

Each item also mirrors a pattern already in the tree, which is the other half of
the speed: the file connector is `dhost_sql.c` with a different verb, `exec` is a
naive subprocess wrapped in a timeout, and the header change is a few lines where
the headers are already parsed.

---

## 2. The grab-bag

Capability testing decides which of these ship and in what shape. They are listed
because we have already identified them; none is committed until the testing pass
weighs in.

### 2.1 `fs` — file I/O, the connector we shipped every neighbour of but not

`host/dhost_fs.c`, structured like `dhost_sql.c`. Minimal calls:

```
fs/read  {path}                 -> the bytes           host:fs/read
fs/write {path, data, append?}  -> {bytes = n}         host:fs/write
```

Confinement is the load-bearing part — the SQL authorizer lesson in a new domain:

- Resolve `path` under a configured `root`, **`realpath` the result and require
  `root` as a prefix** (kills `..` traversal and symlink escape). Escape is
  `DENIED`, not clamped.
- Refuse an embedded NUL; cap both directions at `max_bytes` (a hostile read is a
  memory DoS, a hostile write fills the disk).
- `fs/write` exists only when `mode = "readwrite"`, so `host:fs/read` is exactly
  and only read.

Replay: `fs/read` reply is logged and replayed; `fs/write` is an `sql/exec`-style
side effect the pump's existing discipline already covers. From a permission
view this is the *general* capability — the config must not care whether the path
holds a SQLite file or a text file (`doc/Capabilities.md` §4).

### 2.2 `exec` — the honest escape hatch

`host/dhost_exec.c`. A naive subprocess pass-through is easy; the whole surface
is *bounding* it, because the instruction budget cannot (a subprocess runs
outside the VM). So:

```
exec/run {argv = {...}, stdin?, timeout_ms?, cwd?} -> {status, stdout, stderr}
```

- **Wall-clock timeout** (host-configured max, per-call ≤ it) and an **output
  cap** — a runaway child is killed at the deadline, not counted against a budget
  it escapes.
- No shell by default: `argv` is a vector, not a string, so there is no shell
  metacharacter surface unless the deployment explicitly wants `sh -c`.
- Replay: the result arrives as a connector reply, logged and replayed like any
  other — `exec` does not break replay, only the budget.
- In the lab (JS host), the executor evaluates JavaScript rather than spawning a
  shell — same `exec` capability, different executor, "can't tell hosts apart"
  intact.
- **Docs must say it plainly:** granting `exec` is leaving the sandbox
  (`doc/Capabilities.md` §5).

### 2.3 Request headers — an allowlisted map on the listener

The listener already parses every header; forward an allowlisted subset into the
message:

```
{conn, method, path, body, headers = { authorization = "...", ["content-type"] = "..." }}
```

Config `connectors.listen.headers = {"authorization", "content-type"}` — a
lower-cased allowlist, empty by default; a guest sees a header only because the
deployment named it. Bound the count and each value's length. Fixes
`doc/LAB-PLAN.md` upstream ask #2 (the discofetch API's `Authorization`).

### 2.4 Candidates surfaced but not specified

Named so capability testing can pick them up, not built blind:

- **`net`** — outbound HTTP/TCP as a connector (the counterpart to the inbound
  listener), scoped by CIDR/host.
- **`env`** — read allowlisted environment variables.
- **`log`** — a host-side log sink vs. the existing exported-queue pattern.
- Anything the container testing pass throws up.

*(System time is already `host:time`. Nothing to do.)*

### 2.5 Distribution — the installer ships the CLI, not the host

Surfaced by the capability-testing pass and worth writing down before it is lost:
`curl -fsSL https://diluvium.aloecraft.org/start | sh` installs **only the
`diluvium` CLI** — the unsealed interpreter. The **generic host**
(`diluvium-host`: connectors, listener, the driven swarm — everything the
capability model actually lives in) is not a release asset and not on the
installer's menu. A newcomer who follows the website, then reaches for `sql` or
a swarm, gets a runtime that cannot do either and no signal as to why. That is a
real onboarding cliff, not just a test-harness inconvenience.

The static-musl host already builds (`make build_host_musl`, `host/Dockerfile.musl`)
and is exactly the fetch1 deployment artifact — so the gap is purely
*packaging*, not engineering:

- **Publish `diluvium-host`** alongside the interpreter in `release.yml` (the
  static-musl and glibc builds), with its own `SHA256SUMS` line.
- **A flag on the installer** — `--host` / `--swarm` / `--full` — that also drops
  `diluvium-host` (and a starter `deploy.host.lua`), so the website's one command
  can yield the full runtime when asked. Default stays the lean CLI.
- Until then the capability tests build the host from source in-image, which is
  correct for a test but must not be what a *user* has to do.

**Defer, but streamline soon.** No runtime or snapshot impact — pure release
plumbing — so it can land in build7 or slip a build without cost. The point of
recording it here is that the fix is packaging, the artifact already exists, and
the current silent divergence is the thing to close.

---

## 3. The hour, honestly

`fs` + `headers` is close to the original one-hour build. `exec` roughly doubles
it (the timeout/kill/output-cap plumbing, and the shape decision). So build7 is
now **a few hours of mechanics**, not one — and that is the whole cost, because
none of it touches the runtime or the snapshot format.

| Step | Mirrors | ~ |
|---|---|---|
| `dhost_fs.c` + config + register + types | `dhost_sql.c` | 40m |
| `dhost_exec.c` + timeout/kill/output-cap | new | 60m |
| listener `headers` allowlist + config | `dhost_http.c` | 15m |
| `host_check` cases (fs jail, exec timeout, headers) | existing host_check | 20m |
| changelog build7 (host-side, snapshot-compatible) + version bump | build6 flip | 10m |

---

## 4. Firmly out of build7

The expression model (`doc/Capabilities.md`) in full: capability/permission/scope
grammar, one config shape at every depth, config-in-the-program, the end of
`.host.lua`. Also report signing / release-vs-debug reports, and runtime
intra-instance code-path gating. Those change how grants are *said* and are a
larger, separate build — build7 changes only what the host *can do*, on the
current plumbing.

---

## 5. What build7 does not touch

No runtime, ABI, snapshot format, or guest sandbox surface — `io`/`os`/`package`
stay sealed; file and process access arrive *only* through `host:fs/*` and
`host:exec/*`, which is the point. `LUAC_FORMAT` stays `0x46`. A build6 snapshot
restores on build7.
