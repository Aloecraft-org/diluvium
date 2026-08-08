/*
** $Id: lverify.h $
** Load-time bytecode verifier
** See Copyright Notice in lua.h
*/

#ifndef lverify_h
#define lverify_h

#include "lobject.h"


/*
** Diluvium: check a loaded prototype -- and every prototype nested
** inside it -- against its own limits, before anything executes it.
**
** Lua has shipped no bytecode verifier since 5.2, so an instruction's
** register, constant, upvalue and prototype indices were never checked
** against the prototype that owns them; corruption showed up as an
** out-of-bounds access at run time rather than as a refusal at load
** time. script/fuzz_exec.py measures that directly.
**
** On a malformed chunk this raises LUA_ERRSYNTAX with the same
** "bad binary format" wording the loader itself uses, so a caller
** cannot tell which half refused the chunk and does not have to care.
** It returns normally otherwise.
**
** What this does NOT do is prove a chunk is safe. It is a bounds and
** structure check, not an abstract interpreter: it does not track which
** registers hold live values, does not type-check a constant against
** the operation that consumes it beyond the cases the VM itself
** assumes, and cannot bound the operand window of the multiple-result
** instructions, whose extent is a run-time property of the stack top.
** Lua 5.1 shipped a checker that did more than this one and still had
** escapes.
*/
LUAI_FUNC void luaU_verify (lua_State *L, const Proto *f);


#endif
