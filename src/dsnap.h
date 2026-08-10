/*
** dsnap.h
** Snapshots: the runtime fingerprint and the header.
**
** doc/Messaging.md 10.6 puts a header before every snapshot whose whole job is
** to refuse a snapshot that does not belong here, and 10.5 requires the same
** identity in the hash domain that content-addresses a Proto. Both come from
** one value, computed once per process: the runtime fingerprint.
**
** How the fingerprint is computed is the part worth reading. It is not a list
** of '#define's -- it is the SHA-256 of a fixed canary chunk's stripped dump.
** That was chosen after measuring: the same source compiled by the debug and
** release builds of this very tree produces different bytecode, 20 instructions
** against 12, because ltests.h sets MAXINDEXRK to 1 and that is a codegen knob.
** Version and LUAC_FORMAT are identical across those two builds, so a header
** carrying only those would accept a snapshot whose Proto hashes can never
** match, and the failure would surface deep inside restore as a missing Proto
** rather than at the header as a refusal.
**
** Hashing a real dump is self-calibrating: it covers every knob that can change
** codegen, including ones added later, which an enumerated list cannot. The dump
** header itself carries the number sizes, endianness and format byte, so those
** come along for free.
*/

#ifndef dsnap_h
#define dsnap_h

#include <stddef.h>

#include "lua.h"

#include "dhash.h"
#include "dmsgpack.h"


/* Snapshot format version. Bumped when the stream layout changes. */
#define DILUVIUM_SNAP_FORMAT	1


/*
** The runtime fingerprint, as hex. 'out' takes DILUVIUM_SHA256_HEX bytes.
**
** Computed on first call and cached in the registry, because it compiles and
** dumps a chunk and a snapshot-heavy host should not pay for that twice.
*/
LUA_API void diluvium_snap_fingerprint (lua_State *L, char *out);


/*
** What a header carries, and what a restore checks it against.
**
** 'host' is 10.10's host identity stamp: an opaque string the host supplies,
** compared byte for byte on restore. NULL means "do not stamp", and a snapshot
** with no stamp restores anywhere -- which is the right default for a single
** process moving its own state around, and the wrong one for anything that
** accepts a snapshot from elsewhere. The host chooses.
**
** 'capabilities' is 10.6's capability set. It is a string here rather than a
** structure because the capability system is M7 and inventing its shape now
** would be guessing; what matters and is implemented is that the field exists,
** travels in the header, and is compared. Today every caller passes NULL and
** the comparison is between two empty sets, which is a true statement about a
** runtime with no capability system rather than a placeholder that will be
** forgotten.
*/
typedef struct diluvium_snap_opts {
  const char *host;                    /* host identity stamp, or NULL */
  const char *capabilities;            /* capability set, or NULL */
} diluvium_snap_opts;


/*
** Write a header for the current runtime into the buffer at 'out'.
**
** Returns the number of bytes written, or 0 if 'cap' is too small. The header is
** msgpack, so a reader that is not this runtime can still see why it was
** refused -- which matters, because "refused" with no detail is the worst
** possible diagnostic for a snapshot that took an hour of work to produce.
*/
LUA_API size_t diluvium_snap_header (lua_State *L,
                                     const diluvium_snap_opts *opts,
                                     char *out, size_t cap);


/*
** Check a header against this runtime.
**
** Returns 0 on acceptance, having set '*used' to the header's length so the
** caller knows where the payload starts. On refusal returns a DILUVIUM_SNAP_*
** code below and pushes nothing; call 'diluvium_snap_why' for a sentence.
**
** Deliberately not raising: a host restoring an untrusted snapshot needs a
** status it can report, and 10.10 calls restore untrusted input. Raising would
** make every caller wrap this in a protected call to learn what every caller
** needs to know.
*/
#define DILUVIUM_SNAP_ACCEPT		0
#define DILUVIUM_SNAP_BAD_HEADER	1  /* not a snapshot header at all */
#define DILUVIUM_SNAP_FORMAT_MISMATCH	2
#define DILUVIUM_SNAP_RUNTIME_MISMATCH	3  /* different build or codegen */
#define DILUVIUM_SNAP_PERMANENTS_MISMATCH 4
#define DILUVIUM_SNAP_CAPABILITY_MISMATCH 5
#define DILUVIUM_SNAP_HOST_MISMATCH	6

LUA_API int diluvium_snap_checkheader (lua_State *L,
                                       const diluvium_snap_opts *opts,
                                       const char *s, size_t len,
                                       size_t *used);

/* A sentence naming a refusal, for the diagnostic a person actually reads. */
LUA_API const char *diluvium_snap_why (int code);


/*
** The permanents table (10.4).
**
** Every C function reachable from the program has to be in it -- not just
** hostcalls. 'print', 'table.insert' and the rest are C closures and traversal
** hits them immediately, and a C function cannot be serialized: its code is in
** the host binary. So it is *named* instead, and the name is resolved back on
** restore.
**
** '_G' and the standard library tables are in it too, and that is the part worth
** stating. They are tables, so an encoder would happily serialize them -- and
** would then drag the entire global environment into every snapshot, including
** every C function in it, which would fail anyway. Naming them instead means a
** closure's '_ENV' upvalue costs one short ext object.
**
** Built once per state, on first use. Idempotent.
*/
LUA_API void diluvium_snap_permanents (lua_State *L);


/*
** Register a function's prototype so a snapshot can reference it by hash instead
** of carrying its bytecode (10.5).
**
** This is the whole of the "a swarm of generated one-off agents sharing a common
** library inlines only the unique part" story: whatever a host pre-registers is
** referenced, everything else is inlined. A host that registers nothing gets
** self-contained snapshots, which is the right default.
**
** The function must be a Lua closure. Returns 1, or 0 if it is not.
*/
LUA_API int diluvium_snap_register (lua_State *L, int idx);

/* How many prototypes are registered. For a host reporting its own state. */
LUA_API int diluvium_snap_registered (lua_State *L);


/*
** The hooks the codec's snapshot mode needs, filled in for this runtime.
**
** Handed to 'diluvium_msgpack_encode_graph' and '..._decode_graph'. Valid for as
** long as 'L' is; the state it needs lives in the registry, not in the struct.
*/
LUA_API const diluvium_snap_hooks *diluvium_snap_hooks_for (lua_State *L);


/*
** The queue name list from a header, pushed as an array of strings.
**
** 10.8: handles do not survive, and restore re-declares queues by name. So this
** is not informational -- it is what the restore needs in order to rebuild
** anything the program can address.
*/
LUA_API int diluvium_snap_headerqueues (lua_State *L, const char *s,
                                        size_t len);

#endif
