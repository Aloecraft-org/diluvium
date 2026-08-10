/*
** dvs.h
** The swarm layer: libdiluvium-swarm, symbol prefix 'dvs_'.
**
** doc/Messaging.md 4.1 makes this a separate optional library on purpose. An app
** embedding a single scripting sandbox links the runtime and the instance ABI and
** never pays for any of this; a multi-instance host links this as well.
**
** 9.1.2 lists exactly six things it owns, and the list is exhaustive by design --
** if something proposed for this layer does not reduce to one of them, it belongs
** in a program:
**
**   1. An instance table, because endpoints resolve against it.
**   2. One parent field per instance, for subtree kill and attenuation.
**   3. The capability set per instance, for enforcement.
**   4. Draining 'system/lifecycle' and calling the host vtable.
**   5. Enforcing per-instance budgets. It enforces the numbers; it does not
**      decide them.
**   6. The snapshot cache and 'wake_on_message' delivery.
**
** Not here: restart policy, backoff, naming, discovery, topology, routing, or
** anything describing how programs relate to each other beyond parentage. The line
** is mechanism against policy -- enforcing a budget is mechanism, deciding what a
** child's budget should be is a program.
**
** There is no supervisor type, and that is 9.1's central claim rather than an
** omission. A program holding the lifecycle capability is what the word describes;
** nothing here distinguishes it from any other program. Restart strategies,
** backoff and escalation are ordinary Diluvium programs, which for a system whose
** programs are generated and rewritten at runtime is the entire point.
**
** This file includes 'dv.h' and nothing else from the tree. No 'lua.h', no
** internal headers, no codec beyond the token cursor the instance ABI exposes.
** That is the layer boundary, and it is checkable: the moment this file needs a
** 'lua_State', something has been put in the wrong layer.
*/

#ifndef dvs_h
#define dvs_h

#include <stddef.h>
#include <stdint.h>

#include "dv.h"


#define DVS_ABI_VERSION	1u

uint32_t dvs_abi_version (void);


typedef struct dvs_swarm dvs_swarm;

/* An instance handle. 0 is never valid, and a handle is never reused. */
typedef uint32_t dvs_id;


typedef enum dvs_status {
  DVS_OK = 0,
  DVS_ERROR,             /* 'dvs_last_error' has the message */
  DVS_UNKNOWN,           /* no such instance */
  DVS_DENIED,            /* a capability the requester does not hold */
  DVS_LIMIT,             /* a rate limit or a table full */
  DVS_GONE               /* the instance is dead */
} dvs_status;

const char *dvs_status_name (dvs_status s);


/*
** The host vtable (11.5). Everything above it is portable C; everything below is
** the host's.
**
** The swarm layer cannot spawn anything by itself, because spawning means
** something different in every environment -- a wasm instance, a task, a worker, a
** process, a job on another machine. The portable part is bookkeeping.
**
** 'create' is handed an instance that is already made, loaded and budgeted, and
** returns whatever the host wants to associate with it -- a task handle, a thread,
** nothing at all. 'drive' advances that context by one step and reports whether
** the instance is still alive. 'destroy' tears it down.
**
** A single-threaded host can implement 'create' as a no-op and 'drive' as one
** 'dv_run' or 'dv_resume'; that is what the tests do, and it is a legitimate host
** rather than a stub.
*/
typedef struct dvs_host {
  void *(*create) (void *ud, dvs_id id, dv_instance *inst);
  void  (*destroy) (void *ud, dvs_id id, void *ctx);
  /* 1 to keep going, 0 when the instance has finished or failed. */
  int   (*drive) (void *ud, dvs_id id, dv_instance *inst, void *ctx);
  void *ud;
} dvs_host;


/*
** Create a swarm. 'host' is copied, so the caller need not keep it.
**
** 'max_instances' bounds the table; 'spawns_per_step' is 9.5's rate limit on the
** lifecycle capability, which exists because "a self-rewriting system will produce
** a fork bomb eventually, as a bug rather than an attack". Zero means the built-in
** defaults rather than no limit -- an unbounded default is the wrong shape for
** something whose failure mode is unbounded.
*/
dvs_swarm *dvs_new (const dvs_host *host, uint32_t max_instances,
                    uint32_t spawns_per_step);

void dvs_free (dvs_swarm *sw);

/* The message from the last DVS_ERROR. Owned by the swarm, valid until the next
   call on it. */
const char *dvs_last_error (dvs_swarm *sw);


/*
** Put a program in as the root of the swarm.
**
** The root has no parent, so its capabilities are whatever the host grants and are
** the ceiling for everything below -- 9.3's attenuation makes every descendant's
** set a subset of this one. A host that grants the root nothing gets a swarm that
** can never spawn, which is a legitimate configuration and not a mistake.
*/
dvs_status dvs_root (dvs_swarm *sw, const char *code, size_t code_len,
                     const char *const *caps, size_t ncaps,
                     uint64_t instructions, uint64_t memory_kb, dvs_id *out);


/*
** Run the swarm one step: drain every instance's 'system/lifecycle', act on it,
** then drive each resident instance once.
**
** One step rather than a loop, for the same reason 'dv_run' is a step: there is no
** scheduler in here, and a host that wants one writes it. Returns the number of
** instances still alive, so a caller's loop condition is obvious.
*/
int dvs_step (dvs_swarm *sw);

/* How many instances are alive. */
int dvs_alive (dvs_swarm *sw);

/* The instance behind a handle, or NULL. For a host that wants to push directly. */
dv_instance *dvs_instance (dvs_swarm *sw, dvs_id id);

/* Its parent, or 0 for the root or an unknown handle. */
dvs_id dvs_parent (dvs_swarm *sw, dvs_id id);

/*
** Kill an instance and everything below it (9.5).
**
** Subtree, and the default is not negotiable in this layer: "reparenting is harder
** to remove once depended on". A program that wants an orphan to survive arranges
** for it to be a child of something that will outlive it -- which is a topology
** decision, and topology is a program.
*/
dvs_status dvs_kill (dvs_swarm *sw, dvs_id id);


/* ---------------------------------------------------- the snapshot cache -- */

/*
** 9.1.2 item 6. An instance that is not resident has no 'dv_instance' behind it at
** all -- only its bytes -- which is the point: a swarm's cost at rest should be a
** buffer per idle agent and not an interpreter per idle agent.
**
** How an instance comes to be non-resident. 10.1 makes hibernation self-initiated,
** and it stays that way here: a program pushes '{op = "hibernate"}' to
** 'system/lifecycle' and then parks, and the swarm swaps it out on the next drain.
** Nothing swaps an instance out behind its back, because 'dv_snapshot' requires a
** parked instance and only the program knows when it is at a point it is willing to
** stop at.
**
** This is not yet 10.1's 'hibernate()' returning twice -- that is a guest function
** M6 did not build, and the difference is visible: a program here parks with
** 'queue.wait' and continues from the wait when it comes back, rather than
** continuing from a call that returns 'true'. For the idle-on-inbox case 10.2 calls
** "the overwhelmingly common state at scale" the two are the same thing; for a
** program that wants to hibernate mid-computation they are not.
*/

/*
** Hibernation is OFF by default, and that is a release decision rather than a
** default anyone should keep.
**
** 18.1 records a defect in the snapshot layer that this layer cannot work around:
** the thread record drops 'u2.funcidx', so an error raised in a *restored* program
** unwinds from the stack base -- closing every to-be-closed slot and open upvalue in
** the thread, and, with no such slots, writing the error object over the driver's own
** function slot. That is memory corruption, not a wrong answer, and it is reached by
** the ordinary wake-then-error path.
**
** So a swarm refuses to hibernate anything unless a caller asks for it explicitly.
** The point is to make a known defect *unreachable* rather than documented: a
** deployment that keeps agent state at the application level and spawns fresh
** instances never enters that path, and 18.2's profile C is what turning this on
** commits to.
**
** Passing 1 is how the tests keep the machinery exercised, so that the capability
** does not rot while it is switched off. Do not pass 1 in production until 18.1's
** entry is struck out.
*/
void dvs_allow_hibernation (dvs_swarm *sw, int allow);

/* Swap an instance out to the cache. It must be parked; a running or already
   non-resident instance is refused rather than forced. Refused outright unless
   'dvs_allow_hibernation' was called -- see above. */
dvs_status dvs_hibernate (dvs_swarm *sw, dvs_id id);

/* Bring one back. 'dvs_step' does this by itself when a message arrives for a
   'wake_on_message' instance; a host may also ask directly. */
dvs_status dvs_wake (dvs_swarm *sw, dvs_id id);

/* 1 when there is a live instance behind the handle, 0 when it is cached or gone. */
int dvs_resident (dvs_swarm *sw, dvs_id id);

/* How large the instance's cached snapshot is, or 0 when it is resident. */
size_t dvs_cached_size (dvs_swarm *sw, dvs_id id);


/*
** Push a message into an instance's queue by name -- the host's side of 8.4's
** delivery table, and the only call that knows what to do about a non-resident
** destination:
**
**   resident                          -> the queue, DVS_OK
**   dead or unknown                   -> DVS_GONE, immediately, never blocking
**   cached, with 'wake_on_message'    -> a bounded host buffer, DVS_OK; the
**                                        instance is restored on the next step and
**                                        the buffer drains ahead of live pushes
**   cached, without 'wake_on_message' -> DVS_GONE
**
** The buffer is bounded, and a full one is DVS_LIMIT rather than a larger buffer:
** 6.2's bounded queues exist so that backpressure is visible, and an unbounded
** wake buffer would be the one place in the system where it was not.
*/
dvs_status dvs_push (dvs_swarm *sw, dvs_id id, const char *queue,
                     const void *msg, size_t len);


/* ------------------------------------------------------ asking about an id -- */

/*
** The budget an instance was given, as opposed to what it has used.
**
** This exists because the instance ABI has no getter: 'dv_set_budget' sets one,
** 'dv_usage' reports consumption, and 'dv_exceeded' reports the verdict, but
** nothing reads back the limit. A host sizing its own context around an instance
** -- reserving memory for it, deciding whether waking a cached one is affordable --
** needs the number it was configured with, and the swarm is the only thing holding
** it.
**
** A query rather than an argument to 'create'. The host vtable was already
** corrected once (11.5) and its signature is right for the reasons given there; the
** information was simply missing, and a host wants it at moments other than
** creation anyway. Either output pointer may be NULL.
*/
dvs_status dvs_budget (dvs_swarm *sw, dvs_id id, uint64_t *instructions,
                       uint64_t *memory_kb);

/*
** Copy an instance's capability names into 'out', returning how many there are.
**
** Writes at most 'max' of them and returns the true count, so a caller can size a
** buffer by asking with max == 0. The pointers are the swarm's and stay valid until
** that instance dies.
**
** For a host that logs or audits. 'dvs_holds' answers a yes-or-no question, which
** is the right shape for enforcement and the wrong one for "write down what this
** agent was allowed to do" -- and 9.3's attenuation is only auditable if the set
** can be read out.
*/
size_t dvs_caps (dvs_swarm *sw, dvs_id id, const char **out, size_t max);


/*
** Does 'id' hold 'cap'?
**
** Exposed because a host mediating something of its own -- a network reach, a file
** path -- needs the same answer the swarm uses for queues, and reimplementing the
** pattern match would be a second policy.
*/
int dvs_holds (dvs_swarm *sw, dvs_id id, const char *cap);

/*
** Would granting 'cap' to a child of 'parent' be an attenuation?
**
** 9.3 admits no exceptions: a supervisor must never grant a child more than it
** holds itself. This is that test, and it is the one the spawn path uses.
*/
int dvs_may_grant (dvs_swarm *sw, dvs_id parent, const char *cap);

#endif
