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

- **Reserve a correlation token in the request encoding before the first hostcall ships.**
  See the section below, which corrects what this line used to say.
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

## The shape a hostcall should take, and the correction that produced it

**A hostcall needs no ABI at all.** It is a message on a queue the host drains, and an
answer on a queue the host pushes to. Everything below follows from that, and it was
arrived at by being wrong first, which is worth recording because the wrong version is
the intuitive one.

**The claim that was wrong.** A queue-shaped hostcall was described as the *cheap*
option, on the grounds that it "makes every hostcall a park, so `os.time` costs a full
host round-trip" — and that this argued for a synchronous C handler beside
`dv_set_endpoint_handler`, which could answer inline without parking.

That reasoning assumed the guest asks and then immediately needs the answer, which is
what a function call looks like and not what an actor does. Queues are unidirectional:
asking is a `queue.push`, which returns; the answer arrives later, like anything else on
a queue. **Nothing forces a block between the two.**

**Measured, against this tree, with no new mechanism.** A guest declares a request and a
reply queue, pushes `{call = 'time', tok = 1}`, does 500,500 iterations of arithmetic
*without parking*, and then parks once on `{inbox, reply}` — the park it was going to do
anyway, since an actor's loop ends by waiting on its inbox. The host drains the request
during that park and pushes the answer; the guest wakes on the reply queue with the
value. One park, which the program had already budgeted for, and zero round-trips added.

So the queue shape is not the cheap approximation of a hostcall. It is the right one:

- **It costs no ABI.** No new entry point, no handler registration, no new park machinery.
- **It gets replay for free.** Requests and answers are ordinary messages, so they are
  already in the log the replay claim above depends on. A synchronous C handler would
  have been a second path *out* of the log — the exact defect that
  `DV_FLAG_UNSAFE_STDLIB` exists to scaffold over.
- **It puts the blocking decision where it belongs.** A program that has other work does
  it; a program that has none parks. `queue.wait` already takes a list, so one park
  covers the inbox and every outstanding answer at once.
- **It makes backpressure visible.** A request queue is bounded like any other, so a host
  that stops draining is a full queue the program can see rather than a silent stall.
  Declare it `reject` rather than `block`, or asking becomes the park this whole section
  is about avoiding.

The one thing that genuinely forces a park is a *synchronous-looking API* — `local t =
os.time()` means "I need this now" and has nowhere to go but a wait. That is an argument
about what to name the guest-side call, not about the mechanism underneath it, and an
actor-shaped program sidesteps it by asking at the top of its loop and reading the answer
when it wakes.

### What must be reserved before the first hostcall ships

The deadline is real; what it applies to changed. **And it is now met:
`doc/Hostcall.md` is the reservation** — the request/reply encoding with the
correlation token as a required field, written before any host shipped a
handler. What follows is the reasoning that document rests on.

**A correlation token, in the request and echoed in the reply.** This is the item with the
deadline now. Under the queue shape a guest may have several requests outstanding — that
is the point of not blocking — and replies arrive on one queue in whatever order the host
answers. Without a token in the encoding from the first hostcall, matching a reply to its
request means either one-outstanding-at-a-time or a version break to add one. The probe
above carried `tok = 1` by hand; that field has to be part of the format, not a
convention.

**"Pending" is a synchronous-handler concern, and this shape does not have one.** The
earlier advice — reserve a `"pending"` status so that adding an async hostcall later is
not a version break — was written assuming a handler that returns a result inline. Under
the queue shape every hostcall is already asynchronous, and "the answer has not arrived"
is the ordinary state of an empty queue rather than a status code. Keep the status field
extensible regardless: a reply says what happened (`ok`, an error, a refusal), and the
set of those will grow.

**Metering still needs deciding, and is now easier to place.** If a hostcall is a message,
the natural charge is per message and per byte, which is a rule the queue layer is already
positioned to apply — rather than a separate accounting path only hostcalls use.
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
