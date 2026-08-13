/*
** dhostlib.h
** The 'host' guest library: hostcalls without the hand-rolled queue pair.
**
** A connector call used to take five lines of ceremony -- declare
** 'host/calls' and 'host/replies', invent a token, push {tok, call, args},
** wait, correlate -- repeated in every program that touched the host. The
** ceremony was never the design; the queues are the *substrate* (flexible,
** loggable, replayable) and this library is the *surface*. It owns the queue
** pair, sizes it, allocates tokens, correlates replies, and turns a non-ok
** status into an error with the connector's own sentence in it:
**
**   host.sql.open(name)             -> db, the named database within the
**                                      deployment's granted scope
**   db.exec(sql, ...params)         -> {changes, rowid}     raises on non-ok
**   db.query(sql, ...params)        -> {cols, rows}         raises on non-ok
**   db.try_exec / db.try_query      -> value,'ok' | nil,status,detail
**   host.crypto.hash / hmac (data)  -> hex string
**   host.crypto.random(nbytes?)     -> hex string
**   host.crypto.jwt_sign(claims, ttl?) -> token
**   host.crypto.jwt_verify(token)   -> {valid, claims?|reason?}  (never raises
**                                      on an invalid token: that is an answer)
**   host.time()                     -> wall-clock milliseconds
**   host.call(name, args?)          -> value                raises on non-ok
**   host.try(name, args?)           -> value,'ok' | nil,status,detail
**
** The library is a thin C shim around an embedded Lua chunk, because all it
** does is orchestrate 'queue.*' -- there is nothing here Lua cannot say, and
** a Lua body is the seam a future 'await' keyword drops into: the call sites
** stay synchronous-looking, the internals change, no program does. It is a
** permanent (dsnap.c DS_MODULES), so adding it moves the permanents
** fingerprint -- the build6 -> build7 snapshot boundary.
**
** On-top code: public Lua C API only, nothing in the core patch series.
*/

#ifndef dhostlib_h
#define dhostlib_h

#include "lua.h"

LUAMOD_API int luaopen_dhostlib (lua_State *L);

#endif
