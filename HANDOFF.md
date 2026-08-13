# Build 7 handoff

**Status: done.** Parts 1-4 of `doc/BUILD7.md` landed in 5.5.1_build7 (on
`claude/build7-capability-tests-co6io2`, not the assessment branch the
table below names) and the capability suite (cap2-cap6) passes rewritten
to the `host.*` surface; §5 of the plan holds what was deliberately not
built. Everything below this line describes the tree as the build STARTED
-- the raw idiom, the `path`/`mode`/`max_rows` config, "no headers today"
-- and is kept as the record of the operational facts the build was made
from. The current truths live in `doc/Guide.md`, `doc/Host.md`,
`host/types/host.lua` and the changelog.

You are picking up an in-flight effort. This file is your starting point; the
full spec is **`doc/BUILD7.md`**. Read that first, then this for the operational
facts. Follow your own harness's git/commit conventions; the only fixed rules are
the branches and the acceptance gates below.

## Mission, in one line

Build 7 = **stop cementing bad patterns.** Make the code we write today model the
practices we want (a `host` guest library so programs stop hand-rolling queues;
config that grants a *scope* not a filename), fill the connector gaps we can
(`fs`, `exec`, listener headers), and *document* the bigger changes rather than
faking them. Do the five Parts of `doc/BUILD7.md` **in priority order.**

## The two repos and how they relate

| repo | branch | role |
|---|---|---|
| **diluvium** (this repo) | `claude/diluvium-hibernate-assessment-x7q26j` | the runtime + generic host; where Parts 1–3 happen |
| **discofetch** | `claude/discofetch-assessment-ja6xis` | the app; holds `capability_testing/`, the **benchmark** (Part 4) |

You need both. discofetch's `capability_testing/` drives the real host binary
and is how the human judges this build — a passing suite *rewritten to the new
surface* (Part 4) is the definition of success, not a green internal test alone.

## The benchmark (read this before you change anything)

`capability_testing/` (discofetch) has five slices — cap1 environment, cap2
sqlite+json, cap3 crypto+jwt, cap4 swarm, cap5 ports+daemons — each a container
that runs `diluvium-host` and asserts a result out of a shared SQLite file. They
currently pass. Each cap's program is written in the **raw** hostcall idiom
(declare `host/calls`/`host/replies`, hand-roll `ask()`, push `{tok, call,
args}`). **That rawness is exactly what build7 removes.** Part 4 rewrites them to
`host.*`; if the rewrite doesn't read like an ordinary program, Part 1's API is
wrong and should change. Run them with `capability_testing/run.sh <cap>` (or
`all`); first run builds the static host from a diluvium checkout via
`lib/build-host.sh` (`DILUVIUM_REPO=/path/to/diluvium`).

## Acceptance gates (nothing merges past a red one)

1. Existing test suite green — see `.github/workflows/test.yml` for the canonical
   sequence; `make host_check` for the generic host; the drift guard
   `script/check_source_lists.py` must pass (it fails if `onelua.c` and the
   makefile's `AUX_O` disagree).
2. Capability tests pass **rewritten to the new surface**, validated against the
   real host binary.
3. Every doc claim matches the code — no aspirational docs. Unbuilt work lives in
   `doc/BUILD7.md` §5, not described as done.
4. Changelog build7 entry written and consistent (see `script/changelog.py`
   validate/consistency/check; mirror how build6 was cut).

## Technical facts you'll need (so you don't rediscover them)

**Guest libraries — the four registration sites.** A guest global (`json`,
`bytes`, `time`, `endpoint`) is registered in FOUR places that must agree:
`src/dlibs.c`, `src/onelua.c` (the amalgamation `#include`s), `src/makefile`
(`AUX_O` per-file object list), and `src/dsnap.c` (`DS_MODULES`, the permanents
list). Plus the drift guard `script/check_source_lists.py`. `DS_MODULES` today
ends `... "endpoint", "bytes", "json", "time", NULL`. **Adding `host` here is
what moves the permanents fingerprint** (Part 1 → §7 of BUILD7). `build6` did
exactly this when it added json/bytes/time — copy that changeset's shape.

**Snapshot constants:** `DILUVIUM_SNAP_FORMAT` 2, `DS_THREAD_VERSION` 2,
`LUAC_FORMAT` `0x46`. Adding a permanent moves the *fingerprint*, not necessarily
these — change a constant only if a format actually changes. `test/dsnap_check.c`
has the permanents test to extend.

**Hostcall protocol** (`doc/Hostcall.md`, `doc/Host.md`): guest pushes
`{tok, call, args}` to exported queue `host/calls`, reads `{tok, status,
value|detail}` off `host/replies`, correlating by `tok`. `status` ∈
`ok`/`denied`/`error`/`malformed`. The `host` library (Part 1) encapsulates all
of this. The canonical raw idiom + the gate/denial/confinement regression tests
live in `test/host_check.c` (in-process C harness with embedded guest programs) —
read it; it is the worked example and the host regression suite.

**Connector call shapes** (headers of each file document them):
- `sql` (`host/dhost_sql.c`): `sql/query {sql, params}` → `{cols, rows}`;
  `sql/exec {sql, params}` → `{changes, rowid}`. One statement per call. `query`
  refuses writes; `exec` wired only when config mode is readwrite. `max_rows` is a
  per-**query** result cap (rename → `max_result_rows` in Part 2).
- `crypto` (`host/dhost_crypto.c`): `crypto/random {bytes}` → **hex** string;
  `crypto/hash {data}` / `crypto/hmac {data}` → hex; `crypto/jwt_sign {claims,
  ttl}` → token; `crypto/jwt_verify {token}` → `{valid, claims?, reason?}`.
  Config: `key`/`key_env`/`key_file`, `default_ttl`. Key never leaves the host.
- `listener` (`host/dhost_http.c`): `{conn, method, path, body}` on `http_in`;
  `{conn, status, body, content_type?}` on `http_out`. **No `headers` today**
  (Part 3.3 adds an allowlist). Lands on the **root** instance only.

**Config seam** (`host/dhost.c`): everything downstream reads the `dh_config`
struct, not the file — `dh_config_load` is the only place that parses the
`*.host.lua`. Schema/LuaCATS is `host/types/host.lua` (covers the *config* only;
Part 1 adds `types/guest.lua` for the guest globals). `supervisor` and the sql
`path` are currently `fopen`/opened relative to the host's **cwd** — the
ambiguity Part 2 removes by granting a scope.

**Swarm** (`src/dvs.c`): spawn by pushing `{op='spawn', code=<lua source
string>, caps, budget, wake_on_message}` to `system/lifecycle` (needs the
`lifecycle` cap); events on `system/events` (`spawned`/`exited`/`faulted`/
`exceeded`/`denied`/`throttled`, each `{event, id, detail}`). **No inter-instance
endpoint delivery** — that gap is documented (§5), not yours to close in build7.

**Build/run the host:** `make build_host` → `dist/diluvium-host` (glibc);
`make build_host_musl` → `dist/diluvium-host-musl` (static, for Alpine/cap tests,
via `host/build-musl.sh`). Invoke as `diluvium-host <deployment.host.lua>`; a
host with no listener drains and exits when its swarm is done, a host **with** a
listener is a daemon and does not self-exit (cap5 accounts for this).

## Decisions already made — do not relitigate

`doc/Capabilities.md` is the design authority: capability/permission/scope,
depth-uniform config, auditable-not-restrictive, `exec` is the honest escape
hatch, and (new §8) the ergonomics principle — the queue mechanism is not the
surface, the `host` library is, queues stay the substrate, a natural `await`
keyword is the endgame. Profile A = trusted programs (the capability layer is
structuring, not a security boundary against code you wrote). "Provision" is
reserved for cluster nodes — don't use it.

## Suggested order of attack

1. **Part 1** — `host` library + `types/guest.lua` + `dsnap_check` permanents +
   the four registration sites + drift guard. Cross the snapshot boundary once
   (§7); write the build6→7 note.
2. **Part 4 (cap2/cap3 only, early)** — rewrite the two simplest caps to `host.*`
   immediately, as the live acceptance test for Part 1's API. Iterate the API
   until they read like ordinary programs.
3. **Part 2** — sql config grants a scope; safe defaults; rename `max_rows`/split
   `mode`. Update cap2/cap3 configs.
4. **Part 3** — `fs`, then `exec`, then listener headers. Add cap6 (fs).
5. **Part 4 (rest)** — cap4/cap5 to the new surface.
6. **Docs + changelog** — reconcile every doc to what shipped; write the build7
   changelog entry; move anything unfinished into §5.

Keep the human's dashboard honest: after each Part, the capability tests should
still pass (or be updated in lockstep), because that suite is how they judge this
whole run without reading the diff.
