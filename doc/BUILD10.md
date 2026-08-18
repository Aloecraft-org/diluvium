# 5.5.1_build10: assistant enablement, the additive half

**Status:** planned, 2026-08-18. Written as the plan before the build, the
way BUILD7 and BUILD8 were.

The target workload that motivates this build is an AI assistant deployed
as a swarm: a supervisor that answers inbound HTTP, calls LLM and other
REST APIs outbound, sends notifications, spawns bounded workers, and holds
a few credentials while doing it. Most of that already works. What this
build ships is the **additive** remainder — new config keys, new result
fields, new library surface, new tooling — chosen so that a deployment
that configures none of it behaves bit-for-bit as it did on build9.

The other half of assistant enablement is **behaviour-changing** and is
deliberately not here: hardening the rest plugin's egress, consuming the
plugin `wake` policy, converting `exec` to the deferred seam. Those are
documented in §6 with their reasons, exactly so this build cannot smuggle
them in. BUILD8's rule holds: a behaviour change to shipped machinery
belongs in its own build.

**Priority order is load-bearing.** Parts in order; if the clock runs out
it runs out at the bottom, and §5's optional parts are the first to drop.

---

## 1. Listener response headers

The listener's reply message grows an optional `headers` map:

```lua
{conn = t, status = 303, body = "", content_type = "text/plain",
 headers = { location = "/done", ["cache-control"] = "no-store" }}
```

Without it a guest can set only status, body and content type — no
redirects, no caching directives, no CORS — which is a hard wall for any
web-facing deployment. The design mirrors build7's request-header
allowlist, because the questions are the same and the answers should be:

- **The allowlist is config's, not traffic's.** A listen block gains
  `response_headers = { "location", "cache-control" }` — lowercase names,
  up to 8, same bounds as the request side. What a guest may emit is the
  operator's decision, stated once.
- **Host-owned names are refused at config load.** `content-length`,
  `connection`, `transfer-encoding` and `content-type` in the allowlist
  are a config error by name (content type already has its field, and the
  other three are the framing the host owns). A conflict caught at load
  is a typo; the same conflict at traffic time would be a fight over the
  wire format.
- **The value discipline is `content_type`'s, verbatim.** The guest is
  untrusted and these bytes are interpolated into the response head; a CR
  or LF is response splitting on the far side of the LB. A header whose
  value carries a control byte, whose name is not on the allowlist, or
  whose value is past the request side's 4096 bound is **dropped whole —
  never truncated, never "cleaned"** — and the response is otherwise
  answered. That is the `content_type` precedent (keep the safe default,
  here absence) rather than the 431 precedent, because inbound refusal
  protects the guest from a lying client, while here the client must be
  protected from a lying guest, and a refused response would hand a
  misbehaving guest a way to turn its own bug into an outage.

Mechanically: `dh_listener_cfg` reuses `DH_MAX_HDRS`/`DH_HDR_NAME_MAX`
for a second list; `respond_raw` stops assembling the head in a fixed
`snprintf` buffer and builds it into the connection's `dh_buf` (which
already owns growth and overflow guards); the reply pump learns which
listener owns the connection it found, because the allowlist is
per-listener while tokens are host-wide.

Tests, in `test/host_check.c` beside the request-header ones: an
allowlisted header appears on the wire; repeats of the scenario with a
CRLF value, a non-allowlisted name, and a host-owned name each leave the
wire clean; the config refusals (9 names, uppercase, host-owned) are
refused by name.

## 2. Rest plugin response headers

The mirror gap: a `rest/get` result today is `{status, content_type,
body}`, so a guest cannot see `Location`, `ETag`, or a rate-limit header.
Both implementations gain a `headers` map in the result — lowercased
names, repeats joined `", "`, entry count and value length bounded, with
`content_type` kept as the convenience field it is. The manifest's two
`result` schemas say so. No host code is involved: the host reads plugin
results opaquely.

This also makes the comment at the top of `rest_plugin.c` — which has
promised `-> {status, headers, body}` since the file was written — true
instead of aspirational.

## 3. `host.spawn` — the swarm half of build7 Part 1

Capabilities.md §8 is the recorded principle: the mechanism is not the
surface. Build7 applied it to hostcalls; nothing has applied it to the
lifecycle, and even this repository's own tests still spawn a child by
looking up `system/lifecycle` by magic name, pushing a raw op table, and
concatenating the child's source into a string literal.

v1 is deliberately small:

```lua
local child = host.spawn{ code = src, caps = {...}, budget = {...} }
child:push(msg)          -- convenience for queue.push to the child's inbox
host.children()          -- the handles this instance has spawned
```

plus one convention: a spawned child's exit is announced on a
well-known queue the library declares, so a supervisor stops inventing
its own heartbeat. The library owns the lifecycle queue name, the op
shape, and the correlation; the queues stay the substrate.

Not in v1, and shaped so they slot in without changing call sites:
restart/backoff helpers (`host.supervise`), spawn by **code ref** —
prototypes are already content-addressed by the hash of their stripped
dump, so "spawn the chunk named X" has machinery to lean on and kills
code-as-string-concatenation — and the `await` endgame, which collapses
the library's internals exactly as build7 Part 1 planned.

## 4. Vault tooling — secrets off the floor

A `script/` tool that takes name→value pairs (from env or a file, never
argv) and emits a compiled secure-function vault: `local get =
loadfile("vault.luac")(); get("api_key")`. The at-rest claim is the
secure-function claim and no more — the ROADMAP's own words: obfuscation,
not encryption; Kubernetes-secrets parity, honestly stated.

Two facts go in the tool's documentation because the design is only as
good as its caveats:

- **Snapshots are the leak that matters.** A secret fetched into a
  retained local rides every snapshot in the clear. The pattern is fetch
  per use; the better pattern is a secret that never enters the sandbox
  at all (`crypto.key_env` today, rest credential injection in build11),
  with the vault for the residue guest logic must actually see.
- **A guest-side vault is all-or-nothing.** Whoever can call it holds
  every key. Per-key attenuation through spawns is the host-side vault
  connector, recorded as the v2 direction, not built here.

The test plants a secret, mints the vault, and asserts `strings` and a
sweep of all 256 single-byte xor keys cannot find it — the shape
`test_secure_dump.lua` already established — and asserts the taint pass
covers literals inside the vault's table constructor rather than assuming
it.

## 5. Assistant example, changelog; optional parts

`host/` gains an assistant-shaped example deployment wiring listener +
rest + vault + spawn with honest bounds (a raised `call_timeout_ms`
because an LLM call is slow, the response-header allowlist, a child
budget) — the artifact that says what this release is for. CHANGELOG.yaml
gains the `5.5.1_build10` entry.

Optional, first to drop, in order: a **stats connector**
(`host:stats/system` from `getrusage`/`statvfs` over already-granted
scopes only; `host:stats/swarm` from numbers the host already holds — the
`env`/`log` family BUILD7 §5 recorded), and an **smtp plugin**
(`smtp/send`, wake `error`, server and credentials from env as
`crypto.key_env` does; plugin-only, so it can trail the release without a
host version bump).

---

## 6. What build10 is NOT building (so we don't lie about it)

- **Rest egress allowlist + credential injection.** The highest-value
  security item for the assistant workload: today `host:rest/*` reaches
  any URL with any headers, so the API key lives inside the sandbox and a
  prompt-injected guest can exfiltrate it. It is build11's headline, not
  a build10 rider, because it needs the plugin-config delivery decision
  (env vars vs. a per-plugin block in the deployment file) made once and
  deliberately — that shape outlives any one plugin.
- **Consuming the plugin `wake` policy.** Declared, validated, not read
  (BUILD8 gate 7.6, still red). It is a branch in the hibernation wake
  path — behaviour change, own build — but every plugin shipped widens
  exposure to the gap, so it belongs early in build11.
- **`exec` on the deferred seam.** Already recorded in its own words:
  "belongs in its own build."
- **Streaming, either direction.** SSE out of the listener and streamed
  plugin responses stay out; the `final` flag on the wire and the
  one-request listener shape keep both possible later. An assistant loop
  works whole-message today.
- **`imap`.** Parked with its sketch in `plugins/README.md`; check JMAP
  before building it.
- **Aloelite.** A plugin over its Mount API, two backends (direct file /
  manager HTTP); versions with aloelite, so it lives in that repository
  and rides no diluvium build.

## 7. Acceptance gates

1. `make host_check` green, including the new listener tests, on a tree
   with nothing new configured — and the pre-existing tests unchanged,
   which is the back-compat claim made checkable.
2. Every injection test verified failing against a deliberately broken
   variant before landing, the discipline the scramble rework set.
3. The wire shapes appear in `host/types/host.lua` and `doc/Host.md` in
   the same commit as the code; a surface the editor cannot see is not
   finished.
4. `host.spawn`'s example and tests use no raw lifecycle push — the code
   we ship models the pattern, per build7's rule.
5. The changelog entry exists before the tag does.
