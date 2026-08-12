/*
** dtime.h
** The 'time' guest library: convert and format a moment, in UTC.
**
** The clock is a capability -- reading the current time is nondeterministic,
** so it goes through the 'host:time' connector and lands in the message log
** where a replay can reproduce it. This library does the *other* half, the
** part that is pure: given a moment as Unix epoch seconds, turn it into
** broken-down fields or an ISO 8601 string, and back. That arithmetic (civil
** dates from a day count, leap years, day of week) is a function of its input
** and nothing else, so it is a library and not a hostcall -- and it is exactly
** what the sandbox lost when 'os.date' and 'os.time' went with 'os'.
**
**   time.iso(sec)      -> "YYYY-MM-DDThh:mm:ssZ"    (UTC, seconds precision)
**   time.parse(str)    -> Unix epoch seconds        (ISO 8601, offset applied)
**   time.fields(sec)   -> {year,month,day,hour,min,sec,wday,yday}
**   time.of(fields)    -> Unix epoch seconds
**
** Seconds, not milliseconds, because seconds is the calendar unit os.time and
** a JWT's exp/iat already speak; 'host:time' answers in milliseconds, so a
** program divides by 1000 at the boundary. Everything is UTC: a time zone is
** host and locale state, and a sealed instance has neither, so there is one
** clock and it is the one every replay agrees on.
**
** On-top code: public Lua C API only, nothing in the core patch series.
*/

#ifndef dtime_h
#define dtime_h

#include "lua.h"

LUAMOD_API int luaopen_dtime (lua_State *L);

#endif
