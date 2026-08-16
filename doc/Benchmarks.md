# Swarm benchmarks: what a swarm costs, and how it was measured

Two binaries, neither of them in the test suite:

```
make swarm_bench                       # the swarm layer, on its own
make swarm_bench ARGS="--json --scale 4"
make host_bench                        # the same JWT work through the generic host
```

`swarm_bench` measures the swarm layer against a single-threaded host of its
own: agents per gibibyte, spawn and wake rates, what a message across a queue
costs, and what happens when more agents exist than fit in memory at once.
`host_bench` measures one thing the first cannot — a real hostcall round trip —
by running the same JWT workload through `host/`'s crypto connector.

## What the numbers mean, before any of them

**Counts and byte figures are comparable. Times are advisory.** This is
`script/bench.lua`'s doctrine and it applies here for the same reason: a shared
runner varies by more than most regressions worth catching, so wall-clock alone
cannot tell a slower build from a busier machine. Bytes and instruction counts
are deterministic and reproduce anywhere.

**Nothing is asserted.** Both binaries print; neither fails on a number.
`test/footprint.c` says why in its own header — a figure baked into an assertion
becomes a check that fails when someone adds a library rather than when
something is wrong. What they *do* assert is progress: every scenario runs under
a wall-clock deadline and exits non-zero when it stops making any.

That guard is not decoration. `doc/Hibernate.md` records that the worst defect
this subsystem ever had presented three different ways depending on the build —
an abort under assertions, a **hang forever** in release, and a wrong answer
under the sanitizers. A benchmark without a deadline turns that into a CI job
that never finishes and a mystery. It has to fail, and say what it was waiting
for.

**Instruction counts need `--count`.** `dv_usage` counts nothing at all unless a
non-zero instruction budget armed the count hook, so `--count` sets one large
enough never to bind. Arming it costs a registry lookup every thousand
instructions: **times from a counted run are not comparable with times from an
ordinary one.** Run it separately when you want the deterministic figure.

**Neither is in CI, and neither is under the sanitizers.** Timing under
AddressSanitizer measures AddressSanitizer. And a check that has never passed
anywhere does not gate anything until it has passed somewhere — which is the
lesson `make verify_wasm` taught by blocking the 5.5.1_build4 release on its
first run.

## What is measured

| Scenario | The question |
|---|---|
| `density` | What does an agent cost, resident and hibernated? |
| `spawn` | How fast can a supervisor bring agents up, and tear them down? |
| `queue` | What does one message across a queue cost, by payload size? |
| `jwt` | What can an agent get done — HS256 mint and verify, in Diluvium? |
| `churn` | What happens when more agents exist than fit in memory? |
| `step` | What does an idle swarm cost per step? |
| `roster` | What does asking the swarm about an instance cost? |

`host_bench` adds one: the same JWT work through `crypto/jwt_sign` and
`crypto/jwt_verify`, swept across batch sizes.

## Reference figures

Taken at `--scale 1` on one x86-64 Linux machine, release build. **They are
here to be reproduced and argued with, not quoted as a specification** — the
byte figures should reproduce anywhere, and the times should not.

### An agent costs 73 KB awake and 1.4 KB asleep

```
resident_bytes_per_agent                    73392 B
cached_bytes_per_agent                       1430 B
resident_over_cached                        51.32 x
agents_per_GiB_resident                     14630
agents_per_GiB_cached                      750868
slot_table_bytes_per_slot                 1632.25 B
supervisor_bytes                           363792 B
rss_bytes_per_agent                         89648 B
hibernate_us_each                          284.03 us
wake_us_each                               535.07 us
```

This is `dvs.h`'s own claim about the snapshot cache, measured: *"a swarm's cost
at rest should be a buffer per idle agent and not an interpreter per idle
agent."* It is, and the ratio is fifty to one for the smallest agent that is
still an agent — an inbox and a loop that reads it.

Three things the figure does not include, all reported separately:

- **The slot table**, which is `max_instances` × 1632 bytes, `calloc`'d up front
  and paid whether the slots are used or not. A swarm sized at 100,000
  instances reserves 156 MB before a single program loads.
- **The interpreter's own overhead outside the guest heap.** `rss_bytes_per_agent`
  is around 90 KB against the guest heap's 73 KB; the difference is the
  allocator and the process.
- **The supervisor.** It holds one copy of the worker's source per queued spawn
  request, so a supervisor that queues 512 spawns of a 3.7 KB program is holding
  1.9 MB of program text. `supervisor_bytes` reports it.

Waking costs about 1.9× hibernating, which is the shape to expect: `dv_restore`
is validating untrusted input and doing three separate jobs about it, where
`dv_snapshot` is writing bytes out. (`dv_snapshot`'s size enquiry is a full
serialisation that is then thrown away, so hibernating pays the serialiser
twice — that is in the 284 µs above.)

### Spawning is ~400 µs an agent, and the program's size shows

```
small_us_per_spawn                         394.01 us   (233-byte worker)
large_us_per_spawn                         553.31 us   (3657-byte worker)
small_steps                                     9      (512 agents, rate 64)
rate8_steps                                    65      (the same, at the default rate of 8)
small_subtree_kill_us_each                  36.81 us
```

A spawn request carries the child's whole source — that is what `9.1` means by
code arriving as a message — and the layer reads the request through a cursor
that reopens at byte 0 for each field. So request size has a steeper cost curve
than it looks, and `DVS_MAX_REQUEST_BYTES` (32 KB) is the ceiling. Past a few
kilobytes, send a reference to code rather than the code.

`rate8` is the same spawn at the default `spawns_per_step` of 8: 65 steps
instead of 9, and **nothing is lost** — the rate limit defers rather than
denying, so the burst arrives over the following steps in order.

The subtree kill figure matters more than it looks, because `kill_subtree` runs
on **every ordinary instance exit**, not only on an explicit kill. Any swarm
whose agents come and go pays it per agent.

### A message costs about 3 µs

```
p16_roundtrips_per_s                    374156 /s     2.67 us
p256_roundtrips_per_s                   241295 /s     4.14 us
p4096_roundtrips_per_s                  102027 /s     9.80 us   797 MiB/s
```

A round trip is a host push in, a guest `queue.wait`, a guest push out, and a
host drain. Everything crossing a queue is msgpack-encoded and decoded even when
it never leaves the instance — that is deliberate, it is what gives queues copy
semantics and makes them snapshot-ready — so this is partly a codec measurement.
That is what a queue costs here.

### ~2,000 HS256 tokens a second across the swarm, in Diluvium

```
tokens_per_s                              2013.94 /s   (64 agents)
tokens_per_s_per_agent                      31.47 /s
us_per_token                               496.54 us
insns_per_token                          112656.49        (--count)
resumes_per_token                               1
minted / verified / failed        4224 / 4224 / 0
```

`failed` is reported because a throughput figure for work that silently did not
succeed is not a throughput figure: the worker verifies every token it mints and
the harness reads the count back.

**The aggregate is flat and the per-agent figure is not.** Doubling the agents
from 32 to 64 left `tokens_per_s` where it was and halved `tokens_per_s_per_agent`,
because the swarm layer has no scheduler and this host is single-threaded: one
`dv_resume` per instance per step, one step at a time, on one thread. Agents buy
concurrency and isolation here, not parallelism. Parallelism is a host's to
provide — the vtable is where it would go, and `dvs.h` says as much — and no host
in this tree provides it yet.

One token is a mint and a full verify: four SHA-256 compressions of the signing
input, plus base64url and JSON both ways, all in Diluvium. **There is no crypto
a sealed guest can call** — `src/dhash.c`'s SHA-256 is reachable from the
snapshot layer and the host's crypto connector and from nowhere a program can
reach — so this is the interpreter's number, and 112,625 instructions per token
is the deterministic form of it.

### The same work through the host connector is ~19× faster

```
batch1_tokens_per_s                       34211 /s    129 host turns
batch8_tokens_per_s                       50554 /s     17 host turns
batch32_tokens_per_s                      51741 /s      5 host turns
```

(16 agents, 64 tokens each, all 1024 verified, none failed.)

Here the HMAC is C and is not the cost. What the sweep measures is the round
trip: **the guest is driven exactly once per host turn**, so a program that asks
for one token and waits for it spends a whole turn per call however cheap the
call is. Batching collapses 65 turns into 3.

Two numbers, two different quantities. Quoting either as "how fast Diluvium does
JWTs" without saying which one would be a lie of omission.

Batch 32 buys almost nothing over batch 8, and that is the second lesson: the
pump stops draining an instance when its reply queue's length plus its
outstanding calls reach that queue's capacity, so past a point the batch is
bounded by the queue the guest declared rather than by the number it asked for.
A worker that batches must size `host/replies` to match — the `host` library
declares it at 16 on first use, which is the real ceiling on any batch bigger
than that.

### Oversubscription: the thrash curve

```
                     all resident   half resident   eighth resident
ops_per_s                  25041           2045             1467
hit_rate                    1.00           0.49             0.13
us_per_op                  39.93         488.95           681.78
wakes                          0            520              894
us_per_wake                    0         962.86           780.92
steps                        165            627              952
cached_bytes_each              0        1436.95          1437.72
wake_buffer_accepted          16             16               16
wake_buffer_refused_of_64     48             48               48
```

Work goes to a uniformly random agent, which is the worst case for any cache and
the honest one to quote — a workload with any locality does better, and a
benchmark that assumed locality would be measuring its own assumption.

The shape: **halving residency costs about 12×**, and going from half to an
eighth costs only another 1.5×. The cliff is at the point where a typical
message finds its agent swapped out; past that, you are already paying a wake
per message and it cannot get much worse.

Two things worth knowing before designing around this:

- **`query` does not wake anything.** `{op = 'query', id = child}` is answered
  from the slot and never restores an instance, so a supervisor polling its
  children's status does not thrash them. A *message* does — and only when the
  agent asked to be woken with `wake_on_message`.
- **The wake buffer holds 16.** A cached agent takes messages into a bounded
  host buffer and a full one is refused with `DVS_LIMIT` rather than grown. The
  benchmark measures this directly: of 64 pushes at one sleeping agent without
  an intervening step, 16 are accepted and 48 refused. That is `6.2`'s
  backpressure being visible, and it is the one number here that is a hard
  constant rather than a measurement.

### An idle swarm is not free, and pays for room it is not using

```
                                  us_per_step
tight_table       (256 agents, 257 slots)      187.95
table_8x          (256 agents, 2049 slots)     219.42
table_64x         (256 agents, 16385 slots)    417.01
quarter_agents    (64 agents, 257 slots)        26.15
```

Nothing is happening in any of these: every agent is parked on an empty queue
and the step does no work at all.

The three `table` rows hold the agents fixed and vary only `max_instances`.
`dvs_step` walks the whole slot array three times whether or not the slots are
in use, so **a swarm sized with 64× headroom costs 2.4× per step for the room it
is not using.** Size `max_instances` to what you expect, not to what you might
someday want.

### Asking about an instance is a linear scan

```
us_per_full_walk                            15.89 us   (256 agents)
us_per_lookup                                0.06 us
```

Every call that takes a handle — `dvs_instance`, `dvs_push`, `dvs_resident`,
`dvs_budget`, `dvs_caps` — resolves it by scanning the slot table. One lookup is
cheap; a host walking its own roster of N agents is quadratic. That is why the
roster in `swarm_bench` is built once and outside every timed region, and it is
worth doing the same in any host that keeps one.

## Reproducing

```
make swarm_bench ARGS="--scale 1"                # the reference run
make swarm_bench ARGS="--json --scale 1" > a.json
make swarm_bench ARGS="--count --only jwt"       # deterministic instruction counts
make swarm_bench ARGS="--only churn --seed 7"    # a different random order
make host_bench  ARGS="--scale 1"
```

`--scale F` multiplies every size. `--seed N` fixes the churn scenario's random
order, so a run reproduces exactly. `--deadline SEC` is the per-scenario
progress deadline; raise it on a slow machine rather than assuming a stall.

Report the machine. A timing figure without one is not a figure.

## What these found

The first thing the density instrument did was disagree with the collector, and
that turned out to be a real defect rather than a measurement error: the
counting allocator was treating Lua's *type tag* as a size on every fresh
allocation — Lua passes the tag in the allocator's `osize` argument when `ptr`
is `NULL` — so the counter drifted downward against the truth by a few bytes per
allocation. After 400,000 allocations it read 216 bytes against 63,549 actually
held.

That is not only a wrong number. A memory budget is enforced through that
counter, so a program that allocated and freed enough could talk it back down to
nothing and be granted its whole budget again on top of everything it was
already holding. Measured on the unfixed allocator: a program with a 512 KB
budget finished holding **934,846 bytes**, with its own counter reading 345.

Fixed in `dv_alloc`, with `the_memory_counter_agrees_with_the_collector` in
`test/dv_check.c` as the regression test — it checks the instance's counter
against `collectgarbage("count")`, which is Lua counting the same bytes for its
own reasons and is the one ground truth this ABI does not feed.

The same work added `dv_memory`, because `dv_usage` reports only the peak, in
kilobytes. That is the right answer to a supervisor's question — does this child
need a larger budget — and the wrong answer to a host's, which is what a swarm
costs at rest. An idle agent's peak is whatever it touched on the way to being
idle.
