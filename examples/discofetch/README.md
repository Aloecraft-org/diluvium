# A toy Discofetch swarm

A supervisor, a coordinator, and one handler instance per connected client, running
on Diluvium's swarm layer. Clients arrive, get matched, and their handlers are
killed — and at that point nothing anywhere remembers they were here.

It is a toy: there is no network, no real STUN, and the clients are a hardcoded
list. What is real is the machinery — spawning, capability attenuation, per-instance
budgets, subtree kill, and the host/program split.

```
docker build -f examples/discofetch/Dockerfile -t discofetch-toy .   # from the repo root
docker run --rm discofetch-toy

# or, without Docker:
make -C examples/discofetch run
```

## What you are looking at

```
                 ┌──────────────────────────────┐
   clients ─────▶│ host (swarmd.c)              │  moves bytes, prints logs,
                 │  no client data, no routing  │  owns the clock
                 └───────────┬──────────────────┘
                             │ dvs_step
                 ┌───────────▼──────────────────┐
                 │ supervisor  (root)           │  caps: lifecycle, queue:*
                 │  restart policy              │  ← a program, not a type
                 └───────────┬──────────────────┘
                             │ spawn
                 ┌───────────▼──────────────────┐
                 │ coordinator                  │  caps: lifecycle, queue:client/*
                 │  matchmaking                 │  holds: name, want, instance id
                 └──┬────────┬────────┬─────────┘
                    │ spawn  │        │
              ┌─────▼──┐ ┌───▼────┐ ┌─▼──────┐
              │ alice  │ │  bob   │ │ carol  │  caps: queue:client/<name>
              │handler │ │handler │ │handler │  holds: that client's candidates
              └────────┘ └────────┘ └────────┘  ← and nothing else, anywhere
```

Four files:

| File | What it is |
|---|---|
| `swarmd.c` | The host. ~350 lines, and deliberately stupid: it creates the swarm, steps it, moves bytes between queues, and prints. It knows nothing about clients or matching. |
| `supervisor.lua` | The root program. Spawns the coordinator, restarts it up to three times, gives up after that. |
| `coordinator.lua` | Matchmaking. Generates a handler program per client, spawns it, pairs clients, kills both handlers on a match. |
| `handler.lua` | A template. One client's session: gathers toy ICE candidates, reports ready, then parks. |

## The four things worth watching in the output

**One instance per client, and the state dies with it.**

```
  [2] alice wants chess (nat cone) -> spawning a handler
  [3] alice: gathered 3 candidates behind a cone nat
  [2] MATCH #1 on chess: alice <-> bob
  [2]   both handlers (3, 4) are killed; their state goes with them
  [host] instance 3 destroyed; its state is gone with it
```

Alice's candidates were locals in a Lua state that only her handler could reach.
They were not cleared or overwritten on the way out — the state they lived in
stopped existing. The host holds nothing about alice, and the coordinator holds
only her name and which instance was hers.

**A budget stops a client that will not stop itself.**

```
  [2] handler 8 (runaway) blew its budget and was stopped
      -- instruction budget of 500000 exceeded
```

The `runaway` handler enters `while true do n = n + 1 end`. Nothing cooperative can
stop that — it never yields — so the limit lives outside the guest, as an
instruction budget the coordinator set when it spawned the handler. The instance
dies, the coordinator is told, and every other handler is untouched.

**A grant can only narrow.**

```
  [2] a request was denied: queue:*
```

For the `greedy` client the coordinator deliberately asks for `queue:*`, which is
wider than the `queue:client/*` it holds itself. The swarm layer refuses the spawn
and names the capability it refused. Nothing is created: a denied spawn costs
nothing and leaves nothing behind. There is no override, no admin flag, and no way
for a supervisor to hand out authority it does not have.

**The supervisor's restart policy is ordinary Lua.**

Break `coordinator.lua` — a typo, a `nil` index — and watch the supervisor catch
`faulted`, respawn, and give up after three tries. That policy is nine lines in
`supervisor.lua`, and none of it is in the runtime. Which is the point: a restart
strategy you can rewrite at runtime is a program, and one compiled into a C library
is not.

## Things this example makes you notice

**A program cannot push into another program's queue.** Only the host can (`dvs_push`),
or an endpoint reference can. That is why the coordinator writes each client's name
*into its handler's source* rather than sending it a setup message. That is not a
workaround for a missing feature — it is §9.1's model, and for a system meant to
generate and rewrite its agents at runtime it is the ordinary case. Note the `%q` in
`handler_for()`: a client name goes into a Lua source literal, so quoting it is the
difference between an example and an injection hole.

**Code arrives as a message.** The host hands the supervisor the coordinator's source
on an ordinary queue, and the supervisor hands it to `spawn`. Nothing distinguishes a
string that happens to be a program from any other string.

**Handles are never reused, so a restarted coordinator is a different instance.** The
first version of `swarmd.c` cached the coordinator's id once and kept pushing at a
dead handle after a restart — `DVS_GONE`, forever, silently. The host now re-reads it
on every step.

**`inbox` and `outbox` already exist.** §6.6 reserves and exports them per instance, so
the zero-configuration case needs no code — and declaring one again is an error
rather than a silent reconfigure.

**`create` returning `NULL` means `destroy` never fires.** `dvs.h` presents a no-op
`create` as the normal single-threaded case, but `release()` in `dvs.c` guards the
destroy callback with `sl->ctx != NULL`. So a host that wants `destroy` as a
*lifecycle notification* rather than only as context cleanup has to return something
non-NULL. This host allocates a one-field struct for exactly that reason.

## What is not here, and what it would take

**No hibernation.** Every instance in this example is resident, which is release
5.5.1_build3's supported configuration — `doc/Messaging.md` §18.1 records a snapshot
defect that makes the wake-then-error path corrupt memory, so `dvs_hibernate` refuses
unless a host asks for it by name. At ~46 KB per parked instance (`make footprint`),
ten thousand idle sessions is about 449 MB, so for this shape of workload staying
resident is affordable and the swap-out is an optimisation rather than a requirement.

**No two-way traffic between handlers.** A match currently ends the session. To have
matched peers exchange candidates through the swarm, the host would hand each handler
an **endpoint reference** to the other and let them push directly — `endpoint.bind`
turns a reference into an ordinary queue handle, and a sender cannot tell an endpoint
from a local queue. References survive being forwarded in a message as of
`v5.5.1_build3`; before that a router could use an endpoint but not hand one on.

**No network.** The host's `ARRIVALS` table is where a socket would be. The host is
already the right shape for it: it owns the clock, so a real one would poll its
sockets and its swarm in the same loop.

**Trusted programs only.** The capability layer is a structuring device here, not a
security boundary — a program holding one endpoint reference can still forge another
through the `debug` library. Every program in this example is one we wrote, which is
what §18.2 calls profile A. Do not feed this user-supplied Lua.
