/*
** drepl.h
** Diluvium REPL support.
**
** The intelligence a REPL needs -- telling an unfinished statement from a
** broken one, echoing the value of a bare expression, and completing an
** identifier -- lived inside lua.c as static helpers, where only the
** native interpreter could reach it. Anything else driving a Diluvium
** REPL (a browser terminal over the WASM build, an editor, a remote
** front end) had to reimplement it, and the only way to reimplement the
** first one is to match on the text of Lua's error messages.
**
** This is on-top code: it uses nothing but the public C API, adds nothing
** to the core patch series, and is compiled into the amalgamation so the
** interpreter and the WASM host share one implementation.
*/

#ifndef drepl_h
#define drepl_h

#include <stddef.h>

#include "lua.h"

/* Results of 'diluvium_repl_load'. */
#define DILUVIUM_REPL_OK          0  /* compiled; the chunk is on the stack */
#define DILUVIUM_REPL_INCOMPLETE  1  /* needs more input; stack unchanged */
#define DILUVIUM_REPL_ERROR       2  /* the message is on the stack */

/*
** Compile one REPL entry. A bare expression is compiled as though it were
** written 'return <it>;', so evaluating it yields its value the way a
** REPL is expected to; anything that is not an expression is compiled as
** written.
*/
LUA_API int diluvium_repl_load (lua_State *L, const char *code, size_t len,
                                const char *chunkname);

/*
** Push a table of completion candidates for the identifier ending at
** 'cursor' in 'buf', and return the offset at which the replacement
** starts -- so a front end replaces buf[offset..cursor) with a candidate.
**
** Resolution is entirely raw: no metamethod runs, so completing inside a
** table with an active __index has no side effects and cannot fail.
*/
LUA_API size_t diluvium_repl_complete (lua_State *L, const char *buf,
                                       size_t cursor);

#endif
