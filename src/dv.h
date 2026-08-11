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

/*
** Open the whole 'debug' library in the instance, rather than the narrowed one.
**
** An instance gets 'debug' with twelve of its sixteen functions replaced by
** refusals that say why, because the library reaches past every abstraction the
** rest of this ABI is built out of: 'getregistry' hands back the metatable that
** makes an endpoint reference unforgeable, 'getmetatable' walks past
** '__metatable', and 'sethook' takes the one hook slot that 'dv_set_budget'
** enforces a budget through -- so a program could switch its own budget off in
** one line. 'getinfo', 'getlocal', 'gethook' and 'traceback' stay: a program
** may read its own frames.
**
** Set this only where the program is one you wrote or generated yourself. It
** does not merely restore convenience -- it puts back the three escapes above,
** so with it set the capability layer is a way of structuring a program rather
** than a boundary around one.
**
** (The refusal messages name this constant, in src/dlibs.c; they are the only
** other place it appears.)
*/
#define DV_FLAG_UNSAFE_DEBUG	0x2u

/*
** Put `io`, `os` and `package` back into the instance, with `dofile` and
** `loadfile`. **Scaffolding, not a supported configuration.**
**
** By default an instance has none of them, and reaches outside itself only by
** yielding a request its host answers -- which today means the queues. That is
** the design's one boundary, and this flag is a second one that predates it.
**
** Set it only for a program that predates the sealed default and that you
** wrote yourself. Three things stop being true when you do:
**
**   The instruction budget stops meaning anything. 9.4 charges VM instructions;
**   a subprocess started by `os.execute` costs none of them, so a program can
**   burn an hour of machine time while `dv_usage` reports a few thousand.
**
**   Replay stops working. doc/Determinism.md's claim is that a swarm is
**   replayable because every input arrives through the message log. `os.time`
**   and `io.read` do not, and they do not cross the seam the analyzer watches,
**   so the swarm is not replayable and nothing says so.
**
**   The instance stops being a boundary. `os.execute` reaches the machine; a
**   program that can start a process has no need of anything else this ABI
**   restricts.
**
** The intended end state is that this flag has no users, because a program that
** needs the time asks for it and the host answers -- which is also what makes
** that answer fakeable, and therefore what makes replay work. See
** doc/Determinism.md.
**
** A snapshot does not cross this flag. The permanents fingerprint covers the
** names in the module tables, so a sealed instance and an unsealed one
** disagree, and 'dv_restore' refuses with the permanents-set message. That is
** the right answer: a program captured holding `io.open` cannot wake somewhere
** there is none.
*/
#define DV_FLAG_UNSAFE_STDLIB	0x4u

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
**
** Naming a handle that is neither ready nor gone does nothing: the program stays
** parked and this returns DV_IDLE, so the call may simply be retried. It used to
** synthesise a timeout, which was a lie a program could not detect -- 6.3 defines
** "timeout" as 'queue.wait' having elapsed, and a program that passed no timeout
** would be told one had. Passing 0 remains the only way to say the timeout elapsed.
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


/* ------------------------------------------------------------- endpoints -- */

/*
** An endpoint is a queue handle whose far end somebody else owns.
**
** The guest never builds a reference: it receives one in a message as msgpack
** ext 0x02 and hands it to 'endpoint.bind'. What the bytes mean is entirely
** yours -- an index into your instance table, a socket address, a name to look
** up later. The runtime carries them and does not read them.
**
** The reason this is so thin is 7.4: because a push only ever reports whether a
** message was accepted into the next hop, every richer convention -- broadcast,
** store-and-forward, retry, discovery -- is expressible as an ordinary Diluvium
** program. Adding any of them here would freeze one topology into the binary.
*/

/*
** Answer a guest that is binding a reference. Set '*token' to whatever you want
** to identify the endpoint by and return 1, or return 0 to refuse.
**
** A refused bind raises in the guest, because it asked for one specific
** destination and did not get it. That is different from a push to a
** bound-but-dead endpoint, which is the ordinary DV_QUEUE_GONE.
*/
void dv_set_endpoint_handler (dv_instance *inst,
                              int (*bind)(void *ud, const uint8_t *ref,
                                          size_t len, uint32_t *token),
                              void *ud);

/*
** The queue handle a token was bound to, or 0 if nothing bound it.
**
** This is how you find the buffer to drain: an endpoint is a real bounded local
** queue, so 'dv_queue_pop' on this handle is how messages leave the instance.
** The buffer being bounded is the point -- 7.5 requires accepting a message to
** be O(1) and never to wait, so a host-side buffer that grew without limit or
** waited on a network would break the guarantee rather than extend it.
*/
/*
** Pre-authorise a reference, mapping bytes to a token, with no callback.
**
** Prefer this. A host almost always knows what its own references mean, so
** saying it up front is simpler than answering a question later -- and for a host
** reaching this ABI through WebAssembly it is the only option, because a C
** function pointer there is an index into the module's function table and
** installing one from outside means exporting a mutable table and reserving a
** slot. Found while writing the wasmtime binding, which could not use the
** callback at all.
**
** Registered references are consulted before any handler set by
** 'dv_set_endpoint_handler', so the two compose.
*/
void dv_endpoint_allow (dv_instance *inst, const uint8_t *ref, size_t len,
                        uint32_t token);

dv_queue_id dv_endpoint_queue (dv_instance *inst, uint32_t token);

/*
** Say that an endpoint's far end has closed. Pushes to it answer DV_QUEUE_GONE
** from then on, immediately.
**
** Once gone it stays gone. An endpoint names one particular far end; something
** new at the same address is a new endpoint, and letting a handle revive would
** make "gone" mean "not right now" -- a weaker promise that a program could not
** act on.
*/
dv_status dv_endpoint_close (dv_instance *inst, dv_queue_id id);


/* ---------------------------------------------------------------- layout -- */

/*
** Report the size and field offsets of the structs above.
**
** This exists for wasm. A binding that reaches the ABI through WebAssembly has
** no 'offsetof': it reads fields out of linear memory at offsets it worked out
** somehow, and wasm32 is ILP32 -- so every struct holding a pointer or a
** 'size_t' is laid out differently there than on the LP64 host where a
** developer would have measured it. Hardcoding those numbers is a bug that
** cannot be caught by testing on the machine that wrote them.
**
** 'out' is filled with DV_LAYOUT_COUNT values, in the order of the DV_LAYOUT_*
** indices. 'n' is how many the caller has room for; fewer than
** DV_LAYOUT_COUNT is not an error, so a newer runtime can add entries without
** breaking an older binding.
**
** Returns the number of values written.
*/
#define DV_LAYOUT_CONFIG_SIZE		0
#define DV_LAYOUT_CONFIG_ABI		1
#define DV_LAYOUT_CONFIG_FLAGS		2
#define DV_LAYOUT_QUEUE_INFO_SIZE	3
#define DV_LAYOUT_QUEUE_INFO_CAPACITY	4
#define DV_LAYOUT_QUEUE_INFO_LEN	5
#define DV_LAYOUT_QUEUE_INFO_ENABLED	6
#define DV_LAYOUT_QUEUE_INFO_EXPORTED	7
#define DV_LAYOUT_QUEUE_INFO_DIRECTION	8
#define DV_LAYOUT_QUEUE_INFO_ON_FULL	9
#define DV_LAYOUT_WAITSET_SIZE		10
#define DV_LAYOUT_WAITSET_N		11
#define DV_LAYOUT_WAITSET_IDS		12
#define DV_LAYOUT_WAITSET_TIMEOUT	13
#define DV_LAYOUT_WAITSET_FOR_WRITE	14
#define DV_LAYOUT_COUNT			15

/*
** Per-instance limits (9.4).
**
** A guest cannot meaningfully limit itself: a runaway loop never yields, and
** nothing cooperative will stop it. So the limits live out here, and 9.4 is
** emphatic about what the instruction budget is for -- **abort, not schedule**.
** The count hook raises an error; it does not yield, because yielding from a hook
** puts a Lua frame under the yield with CIST_HOOKYIELD set and 10.7 refuses to
** hibernate that. Budgeting by yielding would quietly cost hibernation.
**
** 'instructions' is a VM instruction count, 0 for no limit. 'memory_kb' caps the
** instance's heap, 0 for no limit; an allocation past it fails, which Lua reports
** as an ordinary out-of-memory error the program can even catch.
**
** Set before 'dv_run'. Setting a limit on a running instance is refused, because
** a budget that changed mid-flight would make "exceeded" mean nothing.
*/
dv_status dv_set_budget (dv_instance *inst, uint64_t instructions,
                         uint64_t memory_kb);

/*
** What the instance has spent. Either pointer may be NULL.
**
** 'instructions' is exact to the hook's granularity, which is why the granularity
** is small; 'memory_kb' is the high-water mark rather than the current figure,
** because a supervisor deciding whether a child needs more wants the peak.
*/
dv_status dv_usage (dv_instance *inst, uint64_t *instructions,
                    uint64_t *memory_kb);

/* Did this instance stop because it ran out of budget? */
int dv_exceeded (dv_instance *inst);


/*
** Hibernate and wake an instance.
**
** 'dv_snapshot' writes the instance's whole state -- the parked program, its call
** chain, its reachable values, and every queue with its contents -- into 'out'.
** The instance must be *parked*, which is the only state in which all of it is
** written down: doc/Messaging.md 10.2 explains why, and 10.7 lists the checks.
** Pass out == NULL to ask only for the size.
**
** 'host' is 10.10's identity stamp, or NULL for none. A snapshot with no stamp
** restores anywhere; a stamped one restores only under the same string; and a
** host that supplies one refuses a snapshot without it, or stamping would be
** advisory.
**
** Returns DV_OK with '*len' set, DV_BUFFER_TOO_SMALL when 'cap' is short (with
** '*len' set to what is needed), or DV_ERROR with 'dv_last_error' saying why.
*/
dv_status dv_snapshot (dv_instance *inst, const char *host,
                       uint8_t *out, size_t cap, size_t *len);

/*
** Restore into a *fresh* instance -- one with nothing loaded and no queues.
**
** After DV_OK the instance is parked exactly as the snapshot's was, so the next
** call is 'dv_waitset_get' to learn what it is waiting for and then 'dv_resume'.
** Not 'dv_run': the program is not starting, it is continuing.
**
** Refuses rather than raising, and refuses on *any* input. 10.10 calls a snapshot
** untrusted: this reconstructs a CallInfo chain and stack offsets straight into
** the interpreter, so every field is range-checked before anything is written and
** a malformed snapshot cannot leave a half-built instance. 'dv_last_error' says
** which field, when it can.
**
** DV_SNAPSHOT_MISMATCH means the header was refused: a different runtime build, a
** different permanents or capability set, or the wrong host stamp. DV_ERROR means
** the header matched and the payload did not survive reading.
**
** A budget spans residencies. The snapshot carries the instructions consumed so
** far, restore continues the count from there, and the hook that enforces the
** limit goes back on -- so a woken instance is exactly as budgeted as it was
** when it parked. Set the budget with 'dv_set_budget' *before* restoring, the
** same order a load uses; the other order does not exist, because
** 'dv_set_budget' refuses an instance that has started and a restored instance
** has.
*/
dv_status dv_restore (dv_instance *inst, const char *host,
                      const uint8_t *s, size_t len);

/*
** Reference a prototype by hash instead of carrying its bytecode (10.5).
**
** Register the chunk a swarm shares and every snapshot of an agent using it
** carries a 32-byte hash in place of its code. A host that registers nothing gets
** self-contained snapshots, which is the right default. The bytes are a compiled
** or source chunk, same as 'dv_load'.
*/
dv_status dv_register_code (dv_instance *inst, const uint8_t *code, size_t len,
                            const char *name);

uint32_t dv_layout (uint32_t *out, size_t n);


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
