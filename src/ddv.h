/*
** ddv.h
** The 'dv' library: the runtime half of the syntax desugars.
**
** doc/Plan-2026-09.md section 2 names 'dv.defer', 'dv.isa', 'dv.spread'
** and '__slice' as the registry helpers the new forms lean on. This is
** where those live. 'dv.class' is not on that list and is here for the
** same reason the others are: what a class statement has to do at run
** time -- copy a parent's metamethods, install a constructor, build the
** two metatables an instance needs -- is thirty lines of C or two
** hundred instructions of emitted bytecode, and the C version is the one
** a reader can check. A form whose desugar needs to *decide* something at
** run time -- what a slice of this particular value means, whether this
** object is an instance of that class -- cannot decide it in the parser,
** and a helper is the alternative to a new opcode.
**
** Reached through '_ENV.dv', which is how every other desugar in this
** tree reaches what it needs: f-strings call '_ENV.tostring' and
** '_ENV.string.format', a regex literal calls '_ENV.regex.compile',
** 'defer' calls '_ENV.setmetatable'. The syntax proposals ask for a
** lookup outside '_ENV' so a program cannot shadow it; nothing here does
** that yet, and the consequence is the one 'defer' already documents --
** a program that replaces the global changes what the form means. Worth
** revisiting for all of them at once rather than for one.
**
** ------------------------------------------------------------- surface --
**
**   dv.slice(v, i, j)      the runtime half of 'v[i:j]' (proposals 4.4)
**   dv.class(name, parent) the table a 'class' statement declares (5.1)
**   dv.isa(obj, class)     is 'obj' one of these? (5.1)
**
** '__slice' is a metamethod name, not a function here: 'dv.slice' looks
** it up on the value's metatable and calls it when it is there. Strings
** and plain tables have no metatable to carry one, so they are handled
** directly -- which is what makes 's[1:3]' and '{1,2,3}[2:]' work
** without anyone installing anything.
*/

#ifndef ddv_h
#define ddv_h

#include "lua.h"

LUAMOD_API int luaopen_ddv (lua_State *L);

#endif
