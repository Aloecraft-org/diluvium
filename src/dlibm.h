/*
** dlibm.h
** The embedded libm: float transcendentals that agree on every target.
**
** Stage 1 of doc/diluvium-numeric-spec.md, behind the 'numeric' build
** feature. Two correct libms are allowed to disagree in the last bit of
** 'exp' and routinely do -- glibc, musl, Apple's, wasi-libc's and
** mingw's are five different implementations -- so a program whose
** answer depends on a transcendental has no cross-target answer at all
** while it calls the platform's. This is the one implementation they all
** get instead.
**
** The functions are openlibm's, vendored under src/libm/ and prefixed
** here. The prefix is not decoration (doc/Plan-2026-09.md 3.5): with the
** platform's names a compiler may substitute its own implementation or
** fold a constant call at compile time, and either would put the
** platform's answer back without anything saying so.
**
** What is NOT here, and why: 'sqrt', 'floor', 'ceil', 'fabs' and 'fmod'
** are correctly rounded by IEEE 754 itself. Every target already agrees
** about them, so they stay on the platform and a vendored copy could
** only match.
**
** ------------------------------------------------------------- surface --
**
** Entry points:
**   diluvium_libm_install   replaces the routed entries in an open
**                           'math' table; called once per state
**   dv_exp .. dv_log1p      the functions themselves, for C callers
**                           (the array library's stage 2 twiddles)
*/

#ifndef dlibm_h
#define dlibm_h

#include "lua.h"


/*
** Replace 'math.exp', 'math.log', 'math.pow' and the rest with entries
** that call the functions below.
**
** On-top, per the numeric spec: the stdlib's own table is edited after
** it is opened rather than lmathlib.c being patched, so nothing here
** touches the core patch series. The 'math' table must be on the stack
** at 'idx'.
**
** Without the 'numeric' feature this does nothing at all, and 'math'
** keeps the platform's functions -- which is what makes a build without
** the feature the build that came before it.
*/
LUA_API void diluvium_libm_install (lua_State *L, int idx);


#if defined(DV_NUMERIC)

/* The vendored set. Every one of these is bit-identical on every target
   this runtime builds for; that is the whole claim, and
   test/numeric/libm_vectors.lua is where it is checked. */
double dv_exp (double x);
double dv_log (double x);
double dv_log2 (double x);
double dv_log10 (double x);
double dv_pow (double x, double y);
double dv_sin (double x);
double dv_cos (double x);
double dv_tan (double x);
double dv_asin (double x);
double dv_acos (double x);
double dv_atan (double x);
double dv_atan2 (double y, double x);
double dv_sinh (double x);
double dv_cosh (double x);
double dv_tanh (double x);
double dv_expm1 (double x);
double dv_log1p (double x);

#endif

#endif
