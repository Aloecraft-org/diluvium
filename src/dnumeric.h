/*
** dnumeric.h
** The 'array' guest library: typed arrays and portable numeric kernels.
**
** Stage 0 of doc/diluvium-numeric-spec.md, behind the 'numeric' build
** feature (C define DV_NUMERIC). Without the feature this file's
** translation unit compiles to nothing a program can reach: 'dlibs.c'
** does not register the library and 'diluvium_array_adopt' reports that
** it did not adopt, so a build without the feature is the build that came
** before it.
**
** Why a library and not a hostcall: an array is a value the program owns
** and computes with, in its own address space and against its own memory
** budget. Nothing here reaches outside the instance.
**
** What the tier means, in one line, because it is the whole point of the
** implementation being this specific: every kernel here is *reproducible*
** tier -- bit-identical on every target this runtime builds for -- and
** the numeric spec's section 4 is the list of things that had to be true
** for that, from the reduction order down to the compiler flags in
** doc/Plan-2026-09.md 3.5.
*/

#ifndef dnumeric_h
#define dnumeric_h

#include <stddef.h>
#include <stdint.h>

#include "lua.h"


/*
** Element types, in the order the ABI numbers them ('dv_array_adopt' in
** dv.h takes these values, and they are part of that contract).
*/
#define DVN_F64		0
#define DVN_I64		1
#define DVN_U8		2


LUAMOD_API int luaopen_dnumeric (lua_State *L);


/*
** Adopt a host-owned buffer as an 'array' on the stack, without copying.
**
** 'bytes' must hold exactly 'len' bytes, must be a whole number of
** elements for 'dtype', and must have been allocated so that 'free' can
** release it -- which is what the instance's allocator does underneath.
** On success this takes ownership and returns 0; the array's finaliser
** releases the buffer.
**
** Returns 1 without taking ownership and without pushing anything when
** the arguments do not describe an array, and 1 in every build that has
** no 'numeric' feature. 'dv_array_adopt' in dv.c is the caller and it is
** what turns a 1 into the documented string copy.
*/
LUA_API int diluvium_array_adopt (lua_State *L, int dtype, size_t len,
                                    void *bytes);


/*
** 'LUA_API' rather than 'LUAI_FUNC' on these three, following dshim.h:
** they cross translation units in the per-file build, which compiles
** each d*.c without LUA_CORE, and LUAI_FUNC is only defined there.
**
** The budget seam (doc/Plan-2026-09.md 3.4). Implemented in dv.c, which
** is the only file that can see 'dv_instance'; declared here because the
** kernels are the only callers and dv.h is a published ABI these two are
** deliberately not part of.
**
** 'diluvium_budget_open' returns a cookie for the instance this state
** belongs to, or NULL when there is none. 'diluvium_budget_charge' adds
** 'n' instructions and raises exactly as the instruction hook does when
** the budget is spent; with a NULL cookie it does nothing.
*/
LUA_API void *diluvium_budget_open (lua_State *L);
LUA_API void diluvium_budget_charge (lua_State *L, void *cookie,
                                       uint64_t n);


/*
** Record that something ran at DV_TIER_FAST, which is what
** 'dv_numeric_touched_fast' reports.
**
** Sticky and one-way: the flag is the audit trail's answer to "was this
** run reproducible", and a call that could clear it would answer that
** wrongly. No backend calls this yet -- the portable kernels are all
** reproducible tier by construction -- so the only caller today is the
** test hook in dnumeric.c, which exists so the wiring from a kernel to
** the host's question is proved before there is a kernel to prove it
** with. Implemented in dv.c, for the same reason as the two above.
*/
LUA_API void diluvium_numeric_touched (lua_State *L);


#endif
