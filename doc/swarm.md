# Swarm messaging and the hostcall ABI

Status: **design and a runnable prototype**, nothing shipped. This document
exists so the shape of the concurrency model is argued before it is built,
and `doc/swarm-prototype.lua` is a working sketch of the core so the
argument is not purely on paper. Neither the C surface nor the Lua API
described here exists in `src/` yet.

## Why these two questions are one question

The swarm — many Diluvium processes passing messages — and the hostcall
ABI look like separate features. They are not. `send` and `recv` *are*
hostcalls, the archetypal ones, and the scheduler that lets `recv()` block
and resume *is* the async hostcall model. The hardest single decision in
the hostcall ABI — can a hostcall yield? — is answered by what `recv()`
needs, and the answer is **yes, by yielding the coroutine to a scheduler,
with no continuation in the ABI itself.**

So the design order is inverted from how it sounds. We do not build a
hostcall layer and then messaging on top; we design the messaging, and the
hostcall ABI falls out of it. The prototype demonstrates this directly:
`recv()` and a host function call ride the *same* yield-and-resume path
through the scheduler, and an actor cannot tell whether the scheduler
answered it immediately or much later. That indistinguishability is the
whole async story.

Prerequisite, now met: a host that accepts guest-supplied bytecode is
exactly the untrusted-bytecode threat model, so none of this was safe to
build until the load-time verifier landed (`doc/ROADMAP.md`). It has.

## The model: deterministic actors

A process is a `lua_State` (or, in the prototype, a coroutine) with a
**mailbox**. Anyone can send to it; it consumes. That is the actor model,
and it is the proven shape for a swarm — Erlang/BEAM is the reference. The
one thing Diluvium adds, and the thing that makes it worth building rather
than linking libuv and calling it a day, is that the scheduler is
**deterministic**. BEAM's is preemptive and timing-dependent; a BEAM
system cannot be replayed. A Diluvium swarm can, and that is the same
promise the runtime already makes for a single process, extended to many.

### mpsc and pub/sub are different primitives

They were bundled in the original framing and they should be pulled apart,
because one cannot be built cleanly by pretending it is the other. Rust's
Tokio keeps them as distinct channel types for this reason.

- **mpsc** — many senders, one consumer. Fan-*in*. A mailbox, an actor
  inbox, a work queue. This is the foundation.
- **pub/sub** — one publisher, many subscribers. Fan-*out*. Broadcast,
  events, topics. This is a *layer on top of* mpsc: a topic fans a message
  out by sending one copy, mpsc-style, to each subscriber. The prototype
  builds it in four lines on top of `send`.

A swarm wants both: directed messages to a known process (mpsc), and
topic broadcasts nobody addresses individually (pub/sub).

### What makes it deterministic

The mechanics are small and they are the entire value proposition:

1. Every message gets a **sequence number at the moment it enters the
   system** — never a wall-clock timestamp.
2. Each mailbox stays ordered by sequence number.
3. The scheduler's choice of what to run next is a **pure function of
   queue state** — the runnable actor whose oldest pending message has the
   globally lowest sequence number. Sequence numbers are unique, so the
   choice is total: there are no ties to break with anything
   timing-dependent.
4. Replay is therefore exact: the same message log produces the same
   execution, byte for byte. Wall-clock time never enters the scheduler.

Two properties the runtime already has make this cheaper than it would be
elsewhere. Coroutines are built in, so the yield/resume primitive exists;
the scheduler is the only new machinery. And the string hash seed is fixed
(`luai_makeseed`), so even `pairs()` iteration order is deterministic —
the scheduler does not rely on that (it orders by sequence number, which
is robust regardless), but it means actor code that happens to iterate a
table does not become a replay hazard.

### The payoff: it removes itself as a taint source

The analyzer's determinism verdict (specified in `doc/ROADMAP.md`, not yet
built) lists **host calls** as a source of nondeterminism — a function
that calls out is `indeterminate`. That is the conservative default. But
if the scheduler is deterministic given the message log, then `send` and
`recv` are *not* nondeterministic: they are deterministic functions of
that log. A messaging system built this way subtracts itself from the
taint set, and the analyzer can then prove a whole *swarm* program
deterministic — not merely "each process is deterministic given its
inputs," but "the swarm is replayable." That is the differentiator made
real, and it is the reason to spend the effort here rather than adopt an
existing actor library.

## The pivotal fork: in-process or distributed

This is the single decision that splits the design, and it is genuinely
open. The prototype deliberately implements only the first tier, because
the first tier is where the model is proven and the second is where the
hard, possibly-already-solved problem lives.

- **In-process** — many actors in one host process, sharing memory
  (many coroutines in one `lua_State`, or many `lua_State`s under one
  host). The sequence number is a single atomic counter. Determinism is
  *trivial*. This is where "Rust mpsc" lives literally, and it is a few
  hundred lines. The prototype is this tier.

- **Distributed** — actors across machines, the actual "swarm." Now the
  sequence number needs an **ordering authority**: a sequencer, or
  Lamport/vector clocks, or a consensus-ordered log. This is the real
  project, and it is the same shape as consensus ordering — so it may
  already exist somewhere in the surrounding stack. If it does, that is
  the piece to reuse rather than reinvent; if it does not, it is the bulk
  of the distributed work and should be scoped as such.

The trap to avoid is the Solana one: if *every* message, including local
in-process ones, must pass through a global sequencer to get uniform
determinism, the per-message cost climbs until the granularity tax makes
half the plausible programs unrealistic. The alternative is two tiers with
honestly different guarantees — local messages ordered by the cheap atomic
counter, remote messages ordered by the sequencer — under one API that
*looks* uniform (`send` to an address, wherever it lives). Which way to go
depends entirely on whether the **whole swarm** must be replayable or only
**each process given its message stream**. That is the second open
question, and it is a requirements question, not an engineering one.

## The hostcall ABI, derived

Read off the model rather than designed separately:

- **A hostcall is synchronous from the guest's view and need not block the
  host.** The guest yields a request; the scheduler performs it and
  resumes with the result. If the host can answer immediately it does; if
  it cannot (real I/O), it leaves the actor blocked exactly as an empty
  `recv` does, and resumes it when the completion arrives — carrying a
  sequence number, so the completion takes its deterministic place in the
  order. The guest code is identical in both cases. The prototype's
  `swarm.call` shows the immediate path; the blocked path is the same
  mechanism with a later resume.

- **Only blocking operations yield.** `recv` and an async hostcall yield;
  `send`, `spawn`, `subscribe`, `publish` are ordinary calls that mutate
  scheduler state and return. This keeps the common path cheap and the
  yield points few and explicit — which is also what keeps the analyzer's
  job tractable.

- **Reserve a "pending" status in the result encoding now**, even though
  nothing returns it yet. If a hostcall can only ever answer "done",
  adding an async one later is a version break; if it can answer "pending
  + token", it never is. This is the one cheap forward-compatibility move
  that should be made before the first hostcall ships, whichever tier is
  built.

- **The host surface is a capability set.** A hostcall names a host
  function; the host registers which names exist and hands a given guest
  only the ones it is allowed to reach. A channel or a topic handle is the
  same kind of capability. This is why the messaging and the hostcall
  surface want the same handle discipline.

- Still to decide, and out of scope for the prototype: the error model
  across the boundary (a raised Lua error versus a returned status pair),
  and **metering** — a hostcall is not one VM instruction, so the
  instruction-count budget does not charge for it, and the charge (flat,
  per-argument-byte, or host-declared) is part of the ABI. Both interact
  with the to-be-closed rules already in the language: a suspended actor
  with `defer`/`with` in scope has cleanup semantics that the scheduler
  has to honour on cancellation.

## What the prototype shows, and what it does not

`doc/swarm-prototype.lua` runs on the standard binary
(`./dist/diluvium_debug doc/swarm-prototype.lua`) and is self-checking: it
runs a small map-reduce swarm twice and asserts the delivery traces are
byte-identical, failing loudly if not.

It demonstrates: mailboxes with sequence-numbered messages; a scheduler
that delivers in global sequence order; `recv` as a yield; pub/sub built
on `send`; a hostcall (`swarm.call`) riding the identical yield/resume
path as `recv`; and determinism, proved by replay rather than asserted.

It does **not** implement: the distributed tier or any ordering authority
(it is one atomic counter); real `lua_State`-per-actor isolation (it uses
coroutines in one state, which is the right shape but not the isolation a
real swarm wants); preemption or instruction budgeting (actors are trusted
to yield); the `defer`/`with` cancellation semantics; or any of the ABI
decisions still marked open above. It is a model, not a foundation to
build production on — the point is to make the shape concrete enough to
argue with.

## Open decisions

1. **In-process or distributed** — the pivotal fork above. Everything
   downstream depends on it.
2. **Must the whole swarm be replayable, or only each process given its
   message stream?** This decides whether a global sequencer is required
   (and its tax paid) or whether two tiers with different guarantees are
   acceptable.
3. Actor identity and isolation: coroutines in one state (cheap, shared
   heap, a fault takes the state down) versus a `lua_State` per actor
   (isolated, a fault is contained, more memory, cross-state message
   copying). The swarm framing argues for the latter; the prototype uses
   the former for simplicity.
