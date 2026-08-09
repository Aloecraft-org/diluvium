/*
** $Id: lundump.h $
** load precompiled Lua chunks
** See Copyright Notice in lua.h
*/

#ifndef lundump_h
#define lundump_h

#include <limits.h>

#include "llimits.h"
#include "lobject.h"
#include "lverify.h"
#include "lzio.h"


/*
** Diluvium: stock Lua leaves this hook empty, so an instruction's
** operands were never checked against the prototype that owns them.
** src/lverify.c implements it; see the note there.
*/
#define luai_verifycode(L,f)	luaU_verify(L,f)


/* data to catch conversion errors */
#define LUAC_DATA	"\x19\x93\r\n\x1a\n"

#define LUAC_INT	-0x5678
#define LUAC_INST	0x12345678
#define LUAC_NUM	cast_num(-370.5)

/*
** Encode major-minor version in one byte, one nibble for each
*/
#define LUAC_VERSION  (((LUA_VERSION_NUM / 100) * 16) + LUA_VERSION_NUM % 100)

/*
** Diluvium bytecode is not loadable by stock Lua (extra is_encrypted
** byte per function, optional XOR-scrambled code/constants), so it
** carries its own format byte: stock Lua rejects Diluvium chunks at the
** header instead of misparsing them, and vice versa.
**
** 0x44 ('D' for Diluvium) was the first such format. 0x45 is the second:
** a written string's size field carries a scramble flag in its low bit,
** because 5.5's saved-string table means a string shared with a secure
** function has one stored copy whose position no longer says whether it
** should be hidden. 0x46 is the third: the scramble became a generated
** keystream rather than one repeated byte. That one is not a layout change
** at all, and it is the reason to state the rule as more than layout --
** a chunk scrambled under a different keystream still parses, and decodes
** to garbage silently. Bump this whenever the layout OR the encoding
** changes, so a stale chunk is refused rather than misread.
*/
#define LUAC_FORMAT	0x46	/* Diluvium format 3; stock Lua uses 0 */


/* load one chunk; from lundump.c */
LUAI_FUNC LClosure* luaU_undump (lua_State* L, ZIO* Z, Table *anchor,
                                 const char* name, int fixed);

/* dump one chunk; from ldump.c */
LUAI_FUNC int luaU_dump (lua_State* L, const Proto* f, lua_Writer w,
                         void* data, int strip);

#endif
