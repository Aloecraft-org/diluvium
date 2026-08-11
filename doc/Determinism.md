# Determinism and replay: an open design

Salvaged. A parallel session designed a deterministic actor scheduler for Diluvium and
wrote a runnable prototype for it, on a branch that had forked before the messaging work
landed and did not know queues, endpoints or `libdiluvium-swarm` existed. Its own text
says "neither the C surface nor the Lua API described here exists in `src/` yet", which
was true when written and is now false for most of it.

The branch is gone. The *argument* is kept here, because it is about something
`doc/Messaging.md` deliberately does not cover, and nothing else in the tree records it.
The prototype is kept at `doc/attic/swarm-prototype.lua` and still runs:

```sh
./dist/diluvium_linux_x86_64 doc/attic/swarm-prototype.lua
# ...
# replay is byte-identical: the swarm is deterministic.
```

It is self-checking: it runs a small map-reduce swarm twice and asserts the delivery
traces are identical, failing loudly if not. 242 lines, no C.

## What Messaging.md leaves out, on purpose

§9.1.2 gives the swarm layer six jobs and a scheduler is not among them: "there is no
scheduler in here, and a host that wants one writes it". §8.3 gives the host the clock.
That is the right layering — but it means **nothing in the tree says what a good
scheduler would be**, and the answer is not obvious, because there is a property
available here that most actor systems cannot offer.

## The claim worth keeping

**A deterministic scheduler makes a whole swarm replayable, and that is a differentiator
rather than a nicety.** BEAM's scheduler is preemptive and timing-dependent, so an
Erlang system cannot be replayed; the mechanics that would make Diluvium's replayable
are small:

1. Every message gets a **sequence number when it enters the system** — never a
   wall-clock timestamp.
2. Each mailbox stays ordered by sequence number.
3. The scheduler's choice of what to run next is a **pure function of queue state**: the
   runnable actor whose oldest pending message has the globally lowest sequence number.
   Sequence numbers are unique, so the choice is total and no tie needs breaking with
   anything timing-dependent.
4. Replay is then exact — the same message log produces the same execution.

Two things this runtime already has make it cheaper here than elsewhere. Coroutines are
built in, so yield/resume exists and the scheduler is the only new machinery. And the
string hash seed is fixed, so even `pairs()` order is deterministic — the scheduler does
not rely on that, but it means actor code that iterates a table is not a replay hazard.

**The payoff that makes it worth the effort:** the analyzer's determinism verdict treats
a host call as a source of nondeterminism, so anything that calls out is `indeterminate`.
If the scheduler is deterministic given the message log, then send and receive are
*deterministic functions of that log* — a messaging system built this way subtracts
itself from the taint set, and the analyzer can then prove a whole *swarm* replayable
rather than only "each process given its inputs".

## The two open questions

**In-process or distributed.** In-process, the sequence number is one counter and
determinism is trivial. Distributed, it needs an ordering authority — a sequencer,
Lamport/vector clocks, or a consensus-ordered log — which is the real project and may
already exist in the surrounding stack. The trap named in the original, and worth
repeating: if *every* message including local ones must pass a global sequencer, the
per-message cost climbs until the granularity tax makes half the plausible programs
unrealistic. Two tiers with honestly different guarantees, under one API that looks
uniform, is the alternative.

**Must the whole swarm be replayable, or only each process given its message stream?**
This decides whether a global sequencer is required at all. It is a requirements
question, not an engineering one — and for Discofetch-shaped work, where a session is a
handler instance that lives and dies, per-process replay is plausibly enough.

## What it says about a future hostcall ABI

There is no hostcall ABI yet. When there is one, these fall out of the model rather than
needing separate design, and one of them is a decision that gets expensive to defer:

- **Reserve a "pending" status in the result encoding before the first hostcall ships.**
  If a hostcall can only ever answer "done", adding an async one later is a version
  break; if it can answer "pending + token", it never is. This is the cheap
  forward-compatibility move, and the only item here with a deadline.
- **A hostcall is synchronous from the guest's view and need not block the host.** The
  guest yields a request; the scheduler performs it and resumes with the result. If the
  host cannot answer immediately it leaves the actor blocked exactly as an empty receive
  does. The guest code is identical either way, and that indistinguishability is the
  whole async story.
- **Only blocking operations yield.** Receive and an async hostcall yield; send, spawn,
  subscribe and publish mutate scheduler state and return. Few, explicit yield points
  keep the common path cheap and the analyzer's job tractable.
- **Metering is part of the ABI.** A hostcall is not one VM instruction, so §9.4's
  instruction budget does not charge for it. Flat, per-argument-byte, or host-declared is
  an open choice — and it interacts with `defer`/`with`, since a suspended actor with a
  to-be-closed local in scope has cleanup the scheduler must honour on cancellation.
- **mpsc and pub/sub are different primitives.** Many-senders-one-consumer is the
  foundation; one-publisher-many-subscribers is a layer on top that sends one copy per
  subscriber. The prototype builds the second in four lines on the first. Bundling them
  produces a primitive that is wrong for both.

## How it maps onto what exists

The prototype's vocabulary and the tree's have converged without either knowing:

| Prototype | In the tree today |
|---|---|
| a mailbox | a bounded queue (`queue.declare`, §6) |
| an address | an endpoint reference (§7.3), resolved by the host |
| an actor | an instance — a `lua_State` with its own heap and budget |
| `spawn` | `{op = "spawn"}` on `system/lifecycle` (§9.1) |
| the scheduler | **the host's `drive` loop, and still unwritten** |

Its third open question — "coroutines in one state, or a `lua_State` per actor?" — **is
already answered by the shipped design**: `dv_new` is a state per actor, so a fault is
contained and messages are copied rather than shared. The prototype used coroutines in
one state for simplicity and said so.

So what remains genuinely open is the scheduler itself: sequence numbers, the ordering
rule, and whether the swarm or only each process must replay. `examples/discofetch`'s
host is the naive version of the same loop — it drives every instance once per step, in
id order, which is deterministic by accident rather than by design and says nothing
about what a good rule would be.
