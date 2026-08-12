/*
** djson.h
** The 'json' guest library: a Lua value to JSON text and back.
**
** It sits beside 'msgpack', not under 'bytes', because it is the same kind of
** thing msgpack is -- a whole-value serializer, recursive, with a depth bound
** and a type mapping -- where 'bytes' is flat transcoding. The division of
** labour: 'msgpack' is what crosses a queue (compact, the runtime's own wire
** format), 'json' is what a program meets at an edge that speaks text -- an
** HTTP body, a config, another service's API. Neither reaches outside the
** instance, so both are libraries and not hostcalls.
**
** Pure, so no capability. 'json.decode' reads bytes that may come from outside
** (an HTTP body a client sent), so it is strict and bounded: control bytes in
** a string are refused, nesting past a fixed depth is refused rather than
** recursed into, and trailing bytes after the value are refused.
**
** The one ambiguity Lua forces -- a table is both array and map -- is settled
** by a heuristic, documented on 'json.encode' in djson.c: a table whose keys
** are exactly 1..n is an array, a table whose keys are all strings is an
** object, an empty table is an object, and anything else has no JSON form and
** is an error. A program that needs the distinction where the heuristic cannot
** see it keeps that data in msgpack, which has explicit array/map markers.
**
** On-top code: public Lua C API only, nothing in the core patch series.
*/

#ifndef djson_h
#define djson_h

#include "lua.h"

LUAMOD_API int luaopen_djson (lua_State *L);

#endif
