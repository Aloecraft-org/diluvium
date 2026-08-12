/*
** dbytes.h
** The 'bytes' guest library: base64, base64url and hex, both directions.
**
** Pure computation, and that is the whole reason it is a library rather than a
** hostcall. A connector exists for the things only the host can do -- the
** clock, the database, a signature under a key the guest must not hold -- and
** every one of them is nondeterministic or reaches outside the instance, so it
** goes through the message log where a replay can reproduce it. Encoding bytes
** as hex is neither: it is a function of its input and nothing else, so it
** belongs *inside* the sandbox, where a program calls it directly and no grant
** is involved. A guest could already write these in Lua; this is the same
** answer, correct and fast, and without the fifteen lines of table-driven
** base64 every program would otherwise carry.
**
** The sealed instance has no other way to reach them: 'msgpack' is the wire
** format the queues already speak, but a JWT segment, an HTTP body, a nonce
** printed for a human -- the edges where a program meets something that is not
** another Diluvium queue -- want text encodings, and there was no base64 or hex
** in a sandbox that has no 'io' and no C module loader to bring one in.
**
** On-top code: public Lua C API only, nothing in the core patch series.
*/

#ifndef dbytes_h
#define dbytes_h

#include "lua.h"

LUAMOD_API int luaopen_dbytes (lua_State *L);

#endif
