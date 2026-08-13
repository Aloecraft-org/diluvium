# The capability model: direction

**Status:** design direction, written 2026-08-13. **Not built** — this records
where the capability and host-configuration system is going, so build7 and the
capability-testing pass have a target and later work does not re-litigate it.
The `.host.lua` config that ships in build5/build6 is the thing this replaces.

The near-term work (build7, `doc/BUILD7.md`) implements *capabilities* against
the current config plumbing; this document is the *expression* model those
capabilities will eventually be granted through. The two are deliberately
separable — see §3.

---

## 1. Three axes, currently mashed into one

Today "wiring a connector" collapses three distinct things into one config edit.
Pulling them apart is the whole model:

- **Capability** — what a host *can* do: `fs`, `net`, `exec`, `sql`, `clock`,
  `random`. Host-level, the menu. A host declares its menu explicitly and once;
  restricting a program can never shrink it. (The failure mode this fixes: a
  per-program edit silently dropping a capability the host could offer, leaving
  a runtime nobody remembers how to fully turn back on.)
- **Permission** — grant or deny of a capability, **per instance**, attenuated:
  a child's grants are always a subset of its parent's. This already works
  (`dvs_holds`, attenuation at spawn); it does not change.
- **Scope** — what a permission *applies to*. A tagged union each capability
  validates, not free polymorphism: `fs` reads a path, `net` a CIDR, a listener
  a port range, a sensor a 1–9 range. Scope may carry a range that says what
  even makes sense (hours between sunrise and sunset; solar activity 1–9).

So a grant is `effect (grant/deny) × capability × scope`. "Scope" replaces the
awkward per-resource "binding" idea, and it generalises: a directory, an
absolute path, a CIDR, a bearer token, an LDAP group, a device are all scopes.
(The word *provision* is avoided on purpose — it is reserved for cluster nodes.)

**Defining a capability includes defining its scope-type.** Implementing a new
host, or a new capability into a host, starts by declaring what scopes qualify
it — which also makes each capability self-describing to the analysis report and
the editor.

## 2. The keystone: one config shape at every depth

An instance is a closure — one sealed state, its capabilities bound at creation,
reachable only through queues. The unlock is that **an instance takes the same
configuration whether it is the root or ten generations deep**, and the host is
simply *the root's parent*. Then:

- Host-config and spawn-request are the **same object**.
- Attenuation is the **only** rule in the system: a child's config must fit
  inside its parent's, checked identically whether the parent is the host or
  another instance. The host defines only the root's ceiling; from there it is
  programs granting subsets down the tree.
- A program can carry its own capability/budget/scope needs inline (LuaCATS-typed
  for editor support), so there is no separate `.host.lua` artifact. In profile
  A — trusted programs you wrote — a program declaring its own needs is fine, and
  the operator ceiling above the root is an *optional override* for the day you
  run a program you did not write.

The one thing that genuinely stays host-side is **resource wiring** — but as a
*scope*, not an application detail. The host grants a **place**: a directory for
`fs`, a directory (or path) the sql connector may open databases within, the
signing key. The program then **names the resource it wants inside that scope** —
which database file, which path — and the connector resolves that name against
the granted scope and refuses anything that escapes it. The config must not carry
the application's own filename; that a program is locked to one absolute file
from launch (build5/6's `path = "example.db"`) is the concrete wrong this fixes,
and naming the scope instead of the file is also how a program gets *more than
one* database. Those are machine facts (the same program on two boxes needs two
directories) and a program declaring its own key path is a footgun even when
trusted. So: caps and quantitative limits go in the program, resource *scopes*
stay with the host, and the program names its resources within them. (build7
Part 2 is the first cut of this for `sql` and `fs`.)

## 3. Mechanics vs. expression — why build7 is not throwaway

The *mechanics* of a capability — the `fs` path jail and size cap, the `exec`
executor and its timeout, the listener header allowlist — do not change when the
*expression* model above lands. build7 builds the mechanics on the current
plumbing; the depth-uniform / scope-grammar rework later changes how a grant is
*said* and reuses the same guts. So `fs`, `exec` and headers built now survive
the redesign; the config-model rework is a separate, larger build (see
`doc/BUILD7.md` §4 for what stays out of build7).

## 4. Stance: auditable, not restrictive

The goal is the most *useful* secure runtime, and the way to get there is making
capability configuration easy and obvious, not making it a fight:

- **Narrow by default, trivially wide.** Locked out of the box; `grant fs
  scope=*` is one line; scope is optional with a sane default. Mandatory scope on
  every grant is friction, and friction is what gets hacked around — a bypassed
  guarantee is worth less than an honest wide grant.
- **Warn, don't wall.** Wide grants shout in the logs; they are not blocked.
- **Lead with visibility.** Because caps are inline data the analyzer already
  reads, it can enumerate — statically, before anything runs — every capability
  set a program could request of its whole spawn tree. That static capability
  audit is the pitch: *you can see and prove exactly what any program and its
  descendants can touch.* "Auditable capabilities" wins the security conversation
  that "sandbox" invites and loses.
- **Keep the convenience verbs.** Even with `fs`+scope as the model, `sql/query`
  stays — general capabilities underneath, ergonomic verbs on top. From a
  permission view the config should not care whether a path holds a SQLite file
  or a text file; `sql` is a verb layered on `fs`, not its own universe.

## 5. `exec`, and the one category that is not a sandbox

Most capabilities map cleanly onto scoped resource access, the way WASI's
fine-grained `fs`/`net` control does. `exec` does not: it hands control to a
**native subprocess outside every sandbox**, which no scope reaches. It is easy
to implement (a naive pass-through) and honest to offer, but two things are true
and must be said in its docs:

- **The instruction budget cannot bound it** — a subprocess runs outside the VM
  and costs ~zero VM instructions, so `exec` is bounded by a **wall-clock timeout
  and an output cap**, host-side, not by the budget. (This is the documented
  reason `os` is sealed.)
- **Granting `exec` is leaving the sandbox.** Enable it freely; do not let the
  docs claim a guarantee it voids.

Replay is unaffected: an `exec` result arrives as a connector reply in the
message log, so a replay replays the logged output rather than re-running it —
the same property as `time` and `sql`.

## 6. Deferred: runtime code-path gating

The instance (the closure) is the enforcement boundary, so:

- "The same agent with different capabilities" is **two instances** of one
  program — cheap, works today, and a branch choosing a different `caps` table in
  a spawn is fully visible to the analyzer.
- A parent marking child caps required/optional (or a child declaring them) is a
  **spawn-request field** — small, config-shape only.
- Per-frame / per-code-path capability enforcement *inside* one instance is
  **new machinery** and is deferred; where wanted before it exists, it is a
  guest-side convention (the program checks a flag it was handed), not a runtime
  guarantee. This must not block the model above.

## 7. Roadmap-adjacent: release vs. debug reports, and signing

Tracked here so it is not lost; **not build7.** The analysis report needs to be
the first thing reached for when a program misbehaves, which means it needs
configurations — debug and release to start — and report signing:

- A **release** report requires a signing key, and signing requires the
  permissions to be **reiterated explicitly** at least once — interactively
  (here is the list, y/n per grant) or via a grants file the tool makes easy to
  produce ("copy this into a grants file and pass it as a parameter to automate
  signing in a secure environment"). Some permissions may be explicit by default;
  the point is that a *release* posture is one a human consciously listed.
- Example shape: `diluvium lab` in read-only mode because it is showing you
  source but running signed bytecode.

Design carried from the analyzer discussion: an instruction budget is
deterministic and replayable; wall-clock time is not. Anything that must replay
bit-exactly is bounded by the former and never the latter — which is exactly why
`exec` (§5) sits outside the replay/budget guarantees and says so.

## 8. Ergonomics: the mechanism is not the surface

Capability testing made this undeniable, so it goes here as a first-class
principle: **the mechanism is not the surface a programmer should touch.** The
hostcall boundary — a request pushed onto a queue, a reply awaited on another —
exists for real reasons (sealing, capability gating, replay) and is not
negotiable. But requiring a program to *hand-roll* that queue pair, size it,
correlate tokens, and know the magic queue names is a failure, not a feature. It
takes hours of study to write "create a table, write a value, read it back," and
that voids the accessibility claim the project rests on.

- **A `host` guest library** owns the queue plumbing so a program writes
  `host.sql.open("kv.sqlite").exec(...)` and never sees a queue or a token. It
  is the first thing
  build7 builds (Part 1), and every example and capability test is written
  against it, so the code we ship *models* the good pattern instead of teaching
  the raw one.
- **The queues stay the substrate, not the interface.** They remain the flexible
  thing that can put anything at either end; they are simply no longer what a
  program reaches for. Queues were never meant to be the *only* hostcall boundary.
- **The endgame is a natural `await`.** Because we own the language, the honest
  fix for async-looking hostcalls is language-level: an `await`-style keyword the
  guest library's internals collapse into, so eventually not even the library
  hand-rolls push/wait. That is a language-design effort (roadmap), and Part 1 is
  deliberately shaped so it slots in without changing call sites.
- **Tooling counts as surface.** Guest globals (`queue`, `json`, `host`, …) ship
  LuaCATS type defs so an editor stops flagging them as unknown. A surface the
  linter can't see is not finished.
