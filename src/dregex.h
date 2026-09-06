/*
** dregex.h
** The 'regex' guest library: compiled regular expressions, and the value a
** `...` literal evaluates to.
**
** Why a library and not more of 'string'. Lua patterns are not regular
** expressions and are not trying to be: they have no alternation, no grouping
** of a quantified subexpression, and their '%b' and '%f' items are not regular
** at all. Code that meets the outside world -- a log line, a header, a route,
** a config value somebody else wrote -- arrives with a regular expression
** already written for it, and rewriting that into Lua patterns is where the
** bugs are. So this adds the notation rather than extending the old one, and
** 'string.find' keeps meaning exactly what it meant. Neither is deprecated:
** Lua patterns stay the right tool for '%b()' and for anchored scanning, and
** they are smaller and faster for a fixed literal.
**
** Why this engine. A backtracking matcher -- PCRE, Perl, Python, Java, and Lua
** patterns themselves -- can take exponential time on inputs that look
** ordinary, and that is not a performance footnote here for two reasons that
** are specific to this runtime rather than general good taste:
**
**   The instruction budget cannot see it. doc/Messaging.md 9.4 charges VM
**   instructions through a count hook, and a match runs entirely inside one C
**   call: an agent that spends four minutes inside a matcher spends zero
**   instructions and reports zero. A budget a feature can step around silently
**   is the defect the M0-M7 audit found twice; adding a third would be a
**   choice, not an accident.
**
**   Replay depends on time not mattering, but a swarm that stalls is not
**   replayable in any useful sense either. doc/Determinism.md's model is that
**   the scheduler is a pure function of queue state, and a matcher whose cost
**   depends on the shape rather than the size of a message is exactly the kind
**   of input-dependent stall that model has no account of.
**
** So the engine is a Thompson NFA simulated with Pike's submatch tracking --
** the construction RE2 and Go's 'regexp' use. Every match is O(len * program)
** with no backtracking and no recursion over the subject, so '(a*)*b' against
** thirty a's costs thirty steps rather than a billion. The price is paid in
** notation and is stated rather than hidden: no backreferences and no
** lookaround, because neither is a regular language and neither can be had at
** this bound. dregex.c refuses them by name and says why.
**
** Priority (leftmost-first) semantics are Perl's, not POSIX's: alternation
** prefers its left branch, '*' and '+' are greedy, '*?' and '+?' are lazy. A
** pattern that matches under PCRE, minus the two constructs above, matches the
** same text here.
**
** Byte-oriented, like every other string operation in Lua: '.' is one byte,
** and the class shorthands are ASCII and locale-independent by construction
** (dregex.c computes them from ranges, never from <ctype.h>, which is a
** locale-dependent input and therefore a determinism hazard). A literal
** non-ASCII character in a pattern still works, because its UTF-8 bytes are
** matched as bytes -- what is not offered is '.' counting a codepoint or
** '\w' meaning a letter in another script.
**
** A compiled regex is a *table*, not a userdata, and that is a deliberate
** consequence of hibernation rather than a shortcut: dsnap.c refuses to
** capture a userdata (10.7 item 2), so an agent that parked while holding one
** could not be snapshotted. The compiled program is a byte string in the
** table, so the whole object is ordinary snapshot-able state. The metatable
** is named as a permanent ("dregex.mt") for the same reason the endpoint
** reference metatable is: identity has to survive a restore.
**
** On-top code: public Lua C API only, nothing in the core patch series. The
** `...` literal is two lexer cases and one expression form in lparser.c, and
** it desugars to a call to 'regex.compile' -- it does not reach into this
** file, so a build without this library still parses (and then fails at run
** time with an ordinary "attempt to index a nil value (global 'regex')").
*/

#ifndef dregex_h
#define dregex_h

#include "lua.h"

LUAMOD_API int luaopen_dregex (lua_State *L);


/*
** The metatable every compiled regex wears, for dsnap.c to name as a permanent.
**
** Same argument as 'diluvium_endpoint_pushrefmt': a regex object is recognised
** by rawequal against this exact table, so a snapshot that copied it by content
** would restore an object that fails its own identity test -- every method call
** on it would then report "not a compiled regex" for a value that plainly is
** one. Built on first ask, so it is valid before 'luaopen_dregex' runs.
*/
LUA_API void diluvium_regex_pushmt (lua_State *L);

#endif
