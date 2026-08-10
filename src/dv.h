/*
** dv.h
** The Diluvium instance ABI.
**
** This is the whole of what a host needs to embed one Diluvium program. It is
** deliberately small: every binding -- Rust, JavaScript, Python, C -- is thin
** because there is little here to be thin about.
**
** Four rules, from doc/Messaging.md 11:
**
**   Bytes in, bytes out. No Lua type crosses this boundary. Messages are
**   msgpack, which is also what queues store internally, so nothing is
**   translated on the way through.
**
**   One instance, one thread. The host must not call any function here for a
**   given instance from more than one thread at a time. This is not a lock we
**   forgot to add; it is the contract. Bindings enforce it in their own idiom
**   -- the Rust wrapper is !Sync, the Python one documents it, the JS one is
**   single-threaded by construction.
**
**   The host drives. 'dv_run' returns when the program parks, hands over what
**   it is waiting for, and leaves the decision to the caller. There is no
**   scheduler and no clock in here.
**
**   Version first. Call 'dv_abi_version' before anything else and refuse a
**   mismatch. The version covers this surface, the msgpack ext registry and
**   the snapshot format together, because a wrapper that is newer than the
**   library it was handed must fail loudly rather than misread a message.
*/

#ifndef dv_h
#define dv_h

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif


/*
** Bumped for any change to this surface, to the ext registry, or to the
** snapshot format. The three move together on purpose: a host that can call
** every function correctly and still misdecode a message is worse off than one
** that refused to start.
*/
#define DV_ABI_VERSION	1u

uint32_t dv_abi_version (void);


/* ---------------------------------------------------------------- status -- */

typedef enum dv_status {
  DV_OK = 0,
  DV_QUEUE_FULL,
  DV_QUEUE_DISABLED,
  DV_QUEUE_UNKNOWN,
  DV_QUEUE_EMPTY,
  DV_QUEUE_GONE,
  DV_IDLE,              /* the program parked; the wait-set is filled in */
  DV_DONE,              /* the program ran to completion */
  DV_ERROR,             /* a Lua error; 'dv_last_error' has the message */
  DV_ABI_MISMATCH,
  DV_SNAPSHOT_MISMATCH,
  DV_BUSY,
  DV_BUFFER_TOO_SMALL,
  DV_QUEUE_DROPPED      /* accepted, and the oldest message was evicted */
} dv_status;

/*
** A human-readable name for a status, for logs and binding error types. Never
** NULL, and never the error *message* -- that is 'dv_last_error'.
*/
const char *dv_status_name (dv_status s);


/* -------------------------------------------------------------- instance -- */

typedef struct dv_instance dv_instance;

typedef uint32_t dv_queue_id;   /* 0 is never a valid handle */

typedef struct dv_config {
  uint32_t abi_version;   /* set to DV_ABI_VERSION; checked by 'dv_new' */
  uint32_t flags;
  size_t memory_limit;    /* reserved for the swarm layer; not enforced here */
  void *reserved;
} dv_config;

/*
** Refuse precompiled chunks in 'dv_load', accepting source only.
**
** Worth setting whenever the bytes did not come from your own compiler. The
** loader checks an instruction's operands against the prototype that owns them
** and refuses malformed chunks rather than crashing, but Lua 5.1 shipped a
** fuller checker than that one and still had escapes. Source-only removes the
** question instead of answering it.
*/
#define DV_FLAG_TEXT_ONLY	0x1u

/* Passing NULL for 'cfg' means the defaults, including the current version. */
dv_instance *dv_new (const dv_config *cfg);
void dv_free (dv_instance *inst);

/*
** Compile a program into the instance. 'name' may be NULL, and appears in
** error messages and tracebacks; give it something a human can act on.
*/
dv_status dv_load (dv_instance *inst, const uint8_t *code, size_t len,
                   const char *name);

/*
** The message from the last DV_ERROR on this instance, or NULL. Owned by the
** instance and valid until the next call on it.
*/
const char *dv_last_error (dv_instance *inst);


/* ---------------------------------------------------------------- queues -- */

typedef struct dv_queue_info {
  uint32_t capacity;
  uint32_t len;
  uint8_t enabled;
  uint8_t exported;
  uint8_t direction;      /* 0 both, 1 guest_write, 2 guest_read */
  uint8_t on_full;        /* 0 reject, 1 drop_oldest, 2 drop_newest, 3 block */
} dv_queue_info;

/*
** Queues are declared by the guest, never by the host (6.1). This finds one
** that already exists and returns 0 when there is none -- which is also how a
** host learns that the program it loaded is not the program it expected.
*/
dv_queue_id dv_queue_lookup (dv_instance *inst, const char *name);

dv_status dv_queue_state (dv_instance *inst, dv_queue_id id,
                          dv_queue_info *out);

/*
** Push msgpack bytes. Returns DV_OK, DV_QUEUE_DROPPED, DV_QUEUE_FULL,
** DV_QUEUE_DISABLED or DV_QUEUE_UNKNOWN -- and a full or disabled queue is a
** normal outcome, not an error (6.4).
**
** Accepting is O(1) and never waits on anything (7.5), so a queue declared
** 'on_full = "block"' behaves as 'reject' from this side: a host has no thread
** to park. Look at 'len' against 'capacity' if you want backpressure.
**
** The bytes are stored as given and not validated. A host that pushes
** malformed msgpack will have the guest raise when it pops; checking here would
** mean a full decode on every message to catch a case only a broken host can
** produce.
*/
dv_status dv_queue_push (dv_instance *inst, dv_queue_id id,
                         const uint8_t *msgpack, size_t len);

/*
** Copy the oldest message out and remove it. DV_BUFFER_TOO_SMALL leaves the
** message in place and sets '*out_len' to what it needs, so a host can size a
** buffer and try again rather than losing the message.
*/
dv_status dv_queue_pop (dv_instance *inst, dv_queue_id id,
                        uint8_t *buf, size_t cap, size_t *out_len);

/*
** Borrow the oldest message without copying, then release it.
**
** This pair exists so a host can read a message without a copy on the hot
** path; 'dv_queue_pop' remains for bindings that would rather not think about
** lifetimes.
**
** The pointer is valid until 'dv_queue_release', the next
** 'dv_queue_peek' on this instance, or 'dv_free'. **Do not call 'dv_run' or
** 'dv_resume' in between**: the program would be free to pop that very message,
** and while the bytes are anchored against collection, releasing afterwards
** would then drop a different message than the one you read.
**
** 'dv_queue_release' removes the message, so peek plus release together are a
** pop. Releasing without a peek removes the oldest message all the same.
*/
dv_status dv_queue_peek (dv_instance *inst, dv_queue_id id,
                         const uint8_t **ptr, size_t *out_len);
void dv_queue_release (dv_instance *inst, dv_queue_id id);


/* ------------------------------------------------------------ scheduling -- */

/* Handles per wait-set. Matches DILUVIUM_WAIT_MAX in the runtime. */
#define DV_WAIT_MAX	32

typedef struct dv_waitset {
  uint32_t n;
  dv_queue_id ids[DV_WAIT_MAX];
  int64_t timeout_ms;     /* negative means the program set no timeout */
  uint8_t for_write;      /* 1 when it is waiting for space, not a message */
} dv_waitset;

/*
** Run until the program parks, finishes, or fails.
**
**   DV_IDLE   parked. '*out_waitset' says what it is waiting for; answer with
**             'dv_resume'.
**   DV_DONE   finished. Calling again returns DV_DONE.
**   DV_ERROR  raised. 'dv_last_error' has the message and traceback.
**
** The first call starts the program; later ones continue it. A host loop is
** therefore: load, run, and while it returns DV_IDLE decide what fired and
** resume. Nothing here sleeps: an idle program with a timeout is the host's
** to time, which is what lets the same program run under a reactor, a browser
** event loop or a plain blocking loop.
*/
dv_status dv_run (dv_instance *inst, dv_waitset *out_waitset);

/*
** Answer a park. Pass the handle the host decided fired, or 0 to say the
** timeout elapsed. What that means for the program -- a message, or a queue
** that has gone away -- is worked out from the queue itself, so a host never
** has to model the difference.
**
** Returns what 'dv_run' returns, including DV_IDLE if the program parks again.
*/
dv_status dv_resume (dv_instance *inst, dv_queue_id fired);

/*
** What the program is waiting for right now.
**
** 'dv_run' fills a wait-set in as it returns DV_IDLE, but 'dv_resume' can
** return DV_IDLE too -- a program that parks again immediately, which is the
** normal shape of a loop around 'queue.wait' -- and its signature has nowhere
** to put one. This is how a host reads the current park at any time, so a
** resume that parks again is no different from the first one.
**
** DV_OK with the wait-set filled in when the program is parked, DV_BUSY when it
** is not.
*/
dv_status dv_waitset_get (dv_instance *inst, dv_waitset *out);


/* ---------------------------------------------------------- notification -- */

/*
** Called when the program pushes to an exported queue, so a host need not
** poll. Fires synchronously on the calling thread inside 'dv_run' or
** 'dv_resume', and **must not call back into this ABI** -- the instance is
** mid-step when it runs.
*/
void dv_set_notify (dv_instance *inst,
                    void (*cb)(void *ud, dv_queue_id id), void *ud);


#if defined(__cplusplus)
}
#endif

#endif
