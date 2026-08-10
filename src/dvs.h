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
** What a spawn asked for, handed to the host so it can size its own context.
**
** The capability list is the *attenuated* one -- what the child will actually
** hold, after 9.3's check against the parent -- so a host that logs it is logging
** the truth rather than the request.
*/
typedef struct dvs_spawn {
  const char *code;             /* the program's source or bytecode */
  size_t code_len;
  const char *const *caps;      /* the child's capabilities */
  size_t ncaps;
  uint64_t instructions;        /* budget, 0 for none */
  uint64_t memory_kb;
  int wake_on_message;
} dvs_spawn;


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
