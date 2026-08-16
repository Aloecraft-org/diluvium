# Swarm benchmarks

This document describes the `swarm_bench` and `host_bench` targets: what each
scenario measures, the headline numbers from the reference run, and how to turn
them into a capacity estimate for your own hardware.

The numbers here are from a single reference machine and a single run. They are
meant to establish shape and order of magnitude, not to be quoted as guarantees.
Run the benchmarks yourself before planning around them; the section at the end
shows how.

## Running

```
make swarm_bench                              # all seven scenarios, human-readable
make swarm_bench ARGS="--json --scale 4 --seed 7"
make host_bench                               # JWT through the generic host, batch sweep
```

| flag | meaning |
|---|---|
| `--json` | machine-readable output: one JSON object with a `cases` map |
| `--scale F` | multiply every scenario's default sizes by F (default 1) |
| `--only NAME` | run one scenario |
| `--seed N` | RNG seed for churn traffic; fix it and a run reproduces exactly |
| `--count` | arm the instruction hook and report VM instructions per op |
| `--deadline S` | wall-clock cap per scenario before it is called stalled |

`--count` is a separate run by design. `dv_usage` counts nothing unless a
budget armed the count hook, and arming it slows the timed path by about 2.2× —
so counted and uncounted timings are not comparable. Use `--count` when you
want the deterministic instruction figure and a plain run when you want wall
time.

## What is being measured

`swarm_bench` drives the swarm layer with its own minimal single-threaded host.
It measures the runtime, not a deployment: no network connectors, no
persistence, and no scheduler beyond a fixed step loop. `host_bench` runs the
same JWT workload through the generic host's crypto connector, and exists to
show the cost of doing work in-language versus handing it to a connector. The
two targets report different quantities and are labelled as such.

Everything is single-threaded: one resume per instance per step. Agents buy
concurrency and isolation, not parallelism — adding agents does not add
throughput on this host, and adding cores does not speed the swarm (it can
speed up plugins and whatever sits in front of the host). Parallelism is a
host's to provide, through the `dvs_host` vtable, and no host in this tree
provides it yet.

**Counts and byte figures are comparable; times are advisory.** This is
`script/bench.lua`'s doctrine and it holds here for the same reason: a shared
runner varies by more than most regressions worth catching, so wall-clock alone
cannot tell a slower build from a busier machine. Bytes and instruction counts
are deterministic and reproduce anywhere.

**Nothing is asserted.** Both binaries print; neither fails on a number — a
figure baked into an assertion becomes a check that fails when someone adds a
library rather than when something is wrong (`test/footprint.c` states this for
itself). What they do assert is *progress*: every scenario runs under a
wall-clock deadline and exits non-zero when it stops making any. That guard is
not decoration — `doc/Hibernate.md` records that this subsystem's worst defect
presented as a hang in the release build and a wrong answer under the
sanitizers, and a benchmark without a deadline turns that into a job that never
finishes. Neither target runs in CI and neither runs under the sanitizers:
timing under AddressSanitizer measures AddressSanitizer.

## Scenarios

**density** — spawn N idle agents, each the smallest thing that is still an
agent: an inbox and a loop that reads it. Measures guest heap per resident
agent, snapshot bytes per hibernated agent, hibernate and wake latency, the
slot table's per-slot cost, and RSS beside all of it.

**spawn** — a supervisor brings N workers up through `system/lifecycle`
messages, timed end to end, at two program sizes and two rate limits; then the
whole subtree is killed from the top.

**queue** — one message in and its reply out: a host push, a guest
`queue.wait`, a guest push, a host drain, at 16 B, 256 B and 4 KB payloads.
The swarm layer has no guest-to-guest routing — delivery is through the host
(`dvs_push`) or endpoints — and this measures the host path.

**jwt** — each agent mints an HS256 token *and fully verifies it*, entirely
in-language. A sealed guest has no crypto it can call, so HMAC-SHA256 is
implemented in Diluvium and cross-checked byte-for-byte against Python's
`hmac`. Reports tokens per second and, with `--count`, VM instructions per
token. The workers verify their own output and the harness reads the failure
count back, because a throughput figure for work that silently failed is not a
throughput figure.

**churn** — the residency stress test. N agents share a host residency budget
smaller than N; the host evicts the least recently used agent whenever a wake
would exceed it. Traffic is uniformly random across agents, so every message
has roughly `budget / N` odds of finding its agent resident. A miss buffers
the message, wakes the agent on the next step, and evicts another to hold the
budget. Reports µs per op at several budget ratios. Uniform random is the
worst case for locality by construction; see the capacity section for why real
workloads look better.

**step** — the cost of one `dvs_step` with every agent parked and nothing to
do, against several table sizes. Measures the fixed overhead of scanning
slots.

**roster** — the cost of resolving instance handles, which is a linear scan of
the slot table on every call that takes one.

`host_bench` adds the batch sweep: the same tokens through `crypto/jwt_sign`
and `crypto/jwt_verify`, with the worker asking for 1, 8 or 32 at a time
before waiting.

## Reference numbers

Reference machine: 4-vCPU Intel Xeon @ 2.80 GHz, 16 GiB, Linux, gcc 13.3 at
`-O2`. Single run, `--scale 1`.

| quantity | value |
|---|---|
| resident memory per awake agent | ~73 KB guest heap (~90 KB RSS) |
| hibernated snapshot per agent | ~1.4 KB |
| resident / hibernated ratio | ~51× |
| agents per GiB, awake | ~14,600 |
| agents per GiB, hibernated | ~750,000 |
| slot table, per slot | 1,632 B, allocated up front, used or not |
| hibernate | ~284 µs |
| wake | ~535 µs |
| spawn (233 B program / 3.7 KB program) | ~394 µs / ~553 µs |
| subtree kill, per agent | ~37 µs |
| message round trip, 16 B | ~2.7 µs |
| payload throughput, 4 KB messages | ~797 MiB/s |
| HS256 in-language | ~2,000 tokens/s aggregate (~112,656 VM instructions each) |
| HS256 via crypto connector | ~51,700 tokens/s at batch 32 |
| churn, all agents resident | ~40 µs/op |
| churn, half resident | ~489 µs/op (hit rate 0.49) |
| churn, one-eighth resident | ~682 µs/op (hit rate 0.13) |
| idle step, table sized to fit | ~188 µs (256 agents) |
| idle step, 64× table headroom | ~417 µs — 2.4× for the unused room |

Aggregate throughput is flat as agent count doubles; the per-agent figure
halves. That is the single-threaded host, not the workload.

Three costs sit outside the per-agent figure, and they are the ones a capacity
plan gets wrong: the slot table (`max_instances` × 1,632 B — a swarm sized at
100,000 instances reserves 156 MB before a single program loads), the
process's overhead beyond the guest heap (RSS ≈ 90 KB against the heap's
73 KB), and the supervisor, which holds one copy of the worker's source per
queued spawn request.

## Reading the numbers

**Footprint.** Awake agents cost tens of kilobytes; parked agents cost about a
kilobyte and a half. The swarm layer keeps its snapshot cache in RAM, but the
snapshot itself is ordinary bytes — a host using the instance ABI
(`dv_snapshot`/`dv_restore`) directly can keep them in a database row, on
disk, or on another machine, subject to the identity stamping described in
`doc/Messaging.md` §10.10. For most workloads memory will not be the limit;
single-core speed will be.

**The residency cliff.** The churn curve is a hit-rate curve. An op that finds
its agent resident costs about 40 µs; an op that misses costs a wake plus an
eviction plus the work — roughly 20× more. Under uniform random traffic the
hit rate equals the residency ratio (measured: 0.49 at half, 0.13 at an
eighth), so cost climbs steeply as residency drops toward half and then
flattens: past the cliff you are already paying a wake per message and it
cannot get much worse. Two things follow:

- The residency budget is a memory knob, and it is the *host's* knob — the
  swarm layer has no residency cap of its own, only a table bound. The budget
  controls how much RAM you spend keeping agents awake; everything else parks
  at ~1.4 KB.
- Traffic pattern, not the ratio, decides your real cost. Uniform random has
  no locality and is the worst case. Session-shaped traffic — an agent is
  touched, then touched again shortly after, while idle agents get nothing —
  will hit residency far more often than the ratio suggests, and the LRU
  eviction the bench host already uses is exactly the policy that exploits it.

Two facts worth knowing before designing around hibernation: a `query` request
is answered from the instance table and never wakes anything, so a supervisor
polling its children does not thrash them — a *message* does, and only when
the agent asked to be woken. And the wake buffer holds 16: a sleeping agent
takes messages into a bounded host buffer, and a full one is refused with
`DVS_LIMIT` rather than grown (measured directly: 64 pushes at one sleeping
agent, 16 accepted, 48 refused).

**In-language versus connector.** HMAC in the guest is about 25× slower than
through the connector. That is expected, and it is the design guidance: keep
agents small and deterministic and hand anything heavy to a connector. The
connector figure has its own lesson — the guest is driven once per host turn,
so a program that asks for one token and waits spends a turn per call however
cheap the call is. Batching 8 calls collapsed 129 turns into 17; batch 32
bought almost nothing more, because the hostcall pump stops draining an
instance when its reply queue fills. A worker that batches must size
`host/replies` to match — the `host` library declares it at capacity 16 on
first use, which silently caps any larger batch.

**Idle step cost.** `dvs_step` walks every slot whether or not it is in use,
so overprovisioned tables cost per step even when nothing is happening. Size
`max_instances` close to expected concurrency until this is optimized.
Relatedly, resolving a handle is a linear scan of the same table — one lookup
is 0.06 µs and irrelevant, but a host that walks its own roster of N agents is
quadratic in N. Build the roster once, outside anything you are timing.

**Spawn size and rate.** A spawn request carries the child's whole source, and
the lifecycle drain re-parses the request per field, so program size has a
steeper cost curve than it looks; `DVS_MAX_REQUEST_BYTES` (32 KB) is the
ceiling, and past a few kilobytes it is better to send a reference to code
than the code. The spawn rate limit defers rather than denying — a burst over
the limit arrives over the following steps, in order, with nothing lost. And
the subtree-kill cost is paid on every ordinary agent exit, not only on an
explicit kill.

## Known limitations of the bench

- Single reference machine, single run. Expect variance, especially on shared
  or budget vCPUs.
- Churn traffic is uniform random, which gives the LRU eviction policy nothing
  to exploit. A locality scenario (Zipf-distributed traffic) is the obvious
  next addition and should show hit rates well above the residency ratio.
- The bench host is not the production host: no connectors, persistence, or
  scheduler are represented. `host_bench` covers one slice of the generic
  host — the crypto connector — and nothing else of it.
- Everything is one thread. A parallel host would change the throughput
  numbers and none of the byte figures.

## Estimating capacity on your hardware

Do not size a deployment from the reference numbers. Run `swarm_bench` on the
target machine, then:

1. `--scale` multiplies the defaults (churn's are 256 agents and 1,024 ops),
   so pick a multiple that brings the agent count near your intended fleet.
   Note churn's µs/op at full residency and at your expected hit rate.
2. Decide how many swarm ops one application request costs. A typical
   request-response through a coordinator is 3 to 5 ops — in, route, work,
   reply, out — plus a wake/evict pair when the target agent was parked.
3. Requests per second ≈ 1,000,000 / (µs per op × ops per request). Hold
   roughly 30% headroom for the OS, the front-end, and noisy neighbours on
   shared cores.
4. Memory: awake agents × ~73 KB, plus parked agents × ~1.4 KB, plus
   `max_instances` × 1,632 B for the table. Confirm all three from `density`
   on your box.

Worked example: on a budget shared vCPU roughly 4× slower than the reference
machine, all-resident µs/op lands near 160. At 4 ops per request that is about
1,500 requests/s before headroom, or about 1,000 with it. If the workload
misses residency often, use the half-resident figure instead and plan for a
few hundred. Either way, on that class of machine, memory for the agents
themselves is negligible; the ceiling is single-core speed.

## Correctness notes from the reference run

Two defects were found by these benchmarks and fixed before any of the numbers
above were taken:

- **The counting allocator misread Lua's object type tag as an allocation
  size** when `ptr` was NULL, so the memory counter drifted downward — after
  400,000 allocations it read 216 bytes against 63,549 actually held. Because
  the memory budget is enforced through that counter, allocation churn handed
  the budget back: measured on the unfixed allocator, a program with a 512 KB
  budget ran to completion holding 934,846 bytes. Fixed in `dv_alloc`;
  `the_memory_counter_agrees_with_the_collector` in `test/dv_check.c`
  cross-checks against `collectgarbage("count")` — the one ground truth this
  ABI does not feed — and fails without the fix. The same work added
  `dv_memory`, which reports current bytes held beside the peak `dv_usage`
  reports, because an idle agent's peak is whatever it touched on the way to
  being idle and cannot answer what the agent costs at rest.

- **Every program in `test/footprint.c` raised on its first line.** They all
  declared `inbox`, which is one of the two reserved queues and already exists
  when the program starts, so the declaration raises — and the run status was
  discarded, so an instance that died on line one was measured and printed as
  one parked on a wait. Fixed; `measure` now prints the status so it cannot
  recur quietly.

Any footprint figures published before this run should be considered invalid.
