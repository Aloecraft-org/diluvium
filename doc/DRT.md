# DRT, and what the generic host still does that it does not

`diluvium-drt` ([Aloecraft-org/diluvium-drt](https://github.com/Aloecraft-org/diluvium-drt))
is the successor to the generic host in `host/`. This document says what that
means for this repository, and then answers the question a migration actually
turns on: **what did `diluvium-host` do that DRT does not do yet?**

Two things it is not. It is not a plan — DRT's roadmap is DRT's
(`doc/Next.md` there), and nothing here proposes work for it. And it is not a
recommendation to delete anything: `host/` is deprecated, not dead, and
`doc/Host.md` remains the normative protocol both implementations answer to.

## Why there is a successor

The generic host is a C binary that drives a deployment from a supervisor
program and a typed `*.host.lua` file. It works, and `doc/Host.md` is its
contract. But it sat on the wrong side of a seam: it is the *embedding
application*, which is the layer a deployment most wants to extend, and it was
the layer least able to be extended — a new connector meant new C in this
repository, against the runtime's own build.

DRT moves that layer into Rust over the instance ABI. It reaches the runtime
through `bindings/rust/` — the safe `diluvium` crate over `diluvium-sys`, which
compiles `src/onelua.c` from source — so the language core stays here and
everything a host does lives there. The practical consequence is the one
DRT's own capability-suite notes call the "two-binary cliff": `diluvium` came
from the installer, `diluvium-host` was built separately and shipped by
nobody. DRT is one binary that embeds the language, so installing it gets you
everything.

`drt start` reads a `diluvium-host` `.host.lua` **unchanged** — DRT's README
states this as a design commitment, and its `Listener` type carries a comment
saying the `http` scheme reproduces `dhost_http.c`'s contract with the same
field names and defaults so a config moves between the two by moving the file.
That commitment is what makes the table below a migration checklist rather
than a rewrite plan.

## What this repository still owns

The split is worth stating plainly, because it decides where a change belongs:

| stays here | moved to DRT |
|---|---|
| The language, the runtime, the compiler | The embedding application |
| The instance ABI (`src/dv.h`) and the hostcall encoding (`doc/Hostcall.md`) | Connectors, listeners, deployment config |
| The host *protocol* as a normative document (`doc/Host.md`) | The reference *implementation* of it |
| `src/dvs.c`, frozen — see below | The swarm, reimplemented in `drt-swarm` |
| `bindings/rust/` (`diluvium-sys`, `diluvium`, `diluvium-wasmtime`) | Everything that consumes them |

### `src/dvs.c` is a frozen reference, not a product

DRT's `SPEC.md` §2 records the decision: the C swarm layer "stays in diluvium,
frozen, as the differential-test reference until DRT's swarm passes
acceptance; then diluvium deletes it." `crates/drt-swarm` is the
reimplementation, `drt-caps` reproduces `dvs_holds`/`dvs_may_grant` semantics
with tests naming them, and `drt-bench` runs this repository's
`test/swarm_bench.c` scenarios against the port, diffing against a checked-in
`bench/c-swarm_bench-baseline.json`.

So `make build_swarm_lib` and `dvs_check` should keep running in `test.yml`,
and `libdiluvium-swarm` should **not** become a release artifact — publishing
it would commit us to supporting a layer already scheduled for removal, and
would invite a host to link what DRT replaced. `doc/Messaging.md` §12.1 said
otherwise until this was written; it now points here.

Acceptance, per DRT's `SPEC.md` §12, is the ported capability suite passing
against `drt start` plus the REPL/ssh-attach demo. DRT's README places that
still ahead, so the freeze holds for now.

---

## The gap analysis

**Method, so this can be re-run rather than trusted.** The host's surface was
enumerated from `host/*.c` — the connector table in `dh_config_load`'s
`conn_keys`, the top-level `top_keys`, and every `strcmp(call, ...)` in the
hostcall dispatchers. DRT's was enumerated from its connector crates'
`match call` arms and its config types, at commit `656dbe1` (v0.4.0,
2026-09-01). Everything below is from reading source on both sides; where a
claim is an inference rather than something the code states, it says so.

### Hostcalls

| host call | `host/` | DRT | notes |
|---|---|---|---|
| `time` | ✅ | ✅ | `connectors/time` |
| `time/monotonic` | ✅ | ✅ | |
| `fs/read`, `fs/write` | ✅ | ✅ | `connectors/fs`, jailed by scope |
| `fs/list`, `fs/remove` | ❌ | ✅ | DRT is a **superset** here |
| `sql/query`, `sql/exec` | ✅ | ✅ | `exec` gated on `access = "readwrite"` in both |
| `crypto/random`, `/hash`, `/hmac` | ✅ | ✅ | `connectors/crypto` |
| `crypto/jwt_sign`, `/jwt_verify` | ✅ | ✅ | |
| `crypto/turn_credential` | ✅ | ✅ | |
| **`exec/run`** | ✅ | ❌ | **the real gap — see below** |
| **`capabilities/list`** | ✅ | ❌ | designed, unbuilt — see below |
| `rest/get`, `rest/post` | plugin | ✅ | in-process connector; was an out-of-process plugin here |
| `ssh/exec` | ❌ | ✅ | new in DRT |

### Configuration and machinery

| host feature | DRT | notes |
|---|---|---|
| `connectors.{time,listen,sql,crypto,fs}` | ✅ | `ConnectorWiring`, scope-carrying |
| `connectors.exec` | ❌ | no connector exists |
| HTTP listener (`dhost_http.c`) | ✅ | `crates/drt/src/listen.rs`, deliberately field-compatible |
| `max_instances`, `spawns_per_step` | ✅ | `drt-swarm` |
| `hibernation` | ✅ | residency + snapshot store |
| `caps`, `budget`, `identity` | ✅ | `drt-config`, `drt-caps` |
| `visibility` | ❌ | see `capabilities/list` below |
| `plugins` (the out-of-process channel) | ❌ | tracked as a seam — see below |
| `supervisor` | deliberate | DRT drains the root's lifecycle queue itself; a supervisor program is optional, not required (SPEC §5) |

### The three that matter, in order

**1. `exec/run` — a genuine, untracked gap.**

`host/dhost_exec.c` calls itself "the honest escape hatch": `argv` handed to
`execvp` with no shell to escape from, a wall-clock deadline the config caps
(SIGKILL at expiry, sweeping the child's whole process group), an output cap
per stream, and a nonzero exit reported as `{status = n}` rather than an
error, on the shell's own convention including `127`.

DRT has **nothing local**. `ssh/exec` runs a command on a remote host; there
is no `std::process::Command` or `tokio::process` in any connector or crate
source. `host:exec` survives only as a capability *string* in two
`drt-caps`/`drt-config` tests, with nothing behind it.

What makes this worth flagging is that it appears in none of DRT's own
tracking — not `SPEC.md`, not the sized backlog in `doc/Next.md`, not the
"Not in this list" section that records deliberate refusals, and not the
capability-suite port table. Every other item below is written down somewhere
over there. This one is not, which is why it leads.

It is also the one gap that blocks a config from moving unchanged: a
`.host.lua` granting `host:exec` is accepted by `drt start`'s parser (`exec`
is a legal capability string) and will fail at the call, not at load. That
inference is from reading both sides rather than from running it — worth a
five-minute check against a real `drt` binary before anyone relies on it.

Whether DRT *should* have it is a separate question, and a fair one: exec is
the capability `doc/Capabilities.md` describes as leaving the sandbox, and
DRT may well have declined it on purpose. But nothing records that decision,
and an undocumented omission and a deliberate refusal look identical from
here.

**2. `capabilities/list` and `visibility` — designed, not built.**

The host answers `capabilities/list` with each entry marked granted or not,
which is what makes an auditing agent expressible, and `visibility` in the
config is how an entry is hidden from that menu. DRT's `SPEC.md` §5 keeps the
design in as many words — "Menu visibility vs. grants stays two questions
(`capabilities/list` marks each entry granted or not; public by default)" —
but the string appears nowhere in DRT's Rust. Designed and not yet
implemented, which is a smaller and more honest claim than "missing".

**3. Plugins — absent, tracked, and mostly obsoleted.**

`host/dhost_plugin.c` (963 lines) is the out-of-process capability channel,
and `DH_CALL_PENDING` exists so a slow plugin does not stall the shared
thread. DRT has no equivalent, and says so: `cap7_plugins` is "out of scope
until dynamic connector loading (SPEC.md §7, a seam)".

Most of what the channel actually carried is already gone, though. The
shipped plugin was `plugins/rest/rest_plugin.c`, and DRT reimplements `rest`
as a first-class in-process connector — with a scope, which the C plugin had
no way to take. So the gap is the *extension mechanism*, not the capability
it mostly delivered.

### Not gaps

- **`libdiluvium-swarm` as a release artifact.** Covered above: not building
  it is correct.
- **Token budgets.** DRT's `doc/Next.md` explains at length why this is not a
  sizing question: with a generic `rest` connector the response body lands in
  the guest, so the count is self-reported by the thing being budgeted.
  Never a host feature here either.
- **A supervisor type.** Both refuse it, in the same words.

## If you are moving a deployment

1. Config moves as-is — that is the design commitment, and the `http`
   listener was built to honour it.
2. Check for `host:exec` grants first. That is the one capability with
   nothing behind it in DRT today.
3. If you depend on `capabilities/list` for an auditing agent, it is not
   there yet.
4. If you load out-of-process plugins, only `rest` has an in-process
   equivalent.
5. `make build_host` still builds the C host, and this repository still
   ships it. It is deprecated, not withdrawn.
