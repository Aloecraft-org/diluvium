/*
** dvlibm_private.h
** What the vendored openlibm sources need, and nothing else.
**
** Upstream reaches for four headers: cdefs-compat.h, openlibm_math.h,
** math_private.h and <float.h>. Between them they carry FreeBSD's
** compatibility shims, a per-architecture description of long double
** layouts, and a good deal of platform configuration. None of it is
** needed here -- every function vendored is binary64 -- and all of it is
** a way for one target to build differently from another, which is the
** one thing an embedded libm exists to prevent. So this replaces them.
**
** ------------------------------------------------------------- surface --
**
** Configurable values: none. Everything here is fixed by IEEE 754.
**
** What it provides, which is the whole list the vendored sources use:
**   EXTRACT_WORDS / INSERT_WORDS          the halves of a double
**   GET_HIGH_WORD / SET_HIGH_WORD         the upper half alone
**   GET_LOW_WORD / SET_LOW_WORD           the lower half alone
**   STRICT_ASSIGN                         round to the declared type
**   u_int32_t, int32_t                    the integer types they name
**   the platform's exact functions        sqrt, floor, fabs, copysign,
**                                         scalbn, fmod -- see below
**
** The word accessors go through 'memcpy' on a 'uint64_t' rather than
** through a union of a double and two 32-bit halves, which is how
** upstream spells them. The union spelling has to know the byte order
** and gets it from the per-architecture headers this file replaces;
** shifting a 'uint64_t' does not, so one spelling is right on every
** target. Compilers recognise the memcpy and emit the same instruction
** the union would.
**
** The exact functions stay on the platform, per the numeric spec's stage
** 1: 'sqrt', 'floor', 'ceil', 'fabs' and 'fmod' are correctly rounded by
** IEEE 754 itself, so every target already agrees about them and a
** vendored copy could only match. 'scalbn' and 'copysign' are exact for
** the same reason -- both only move bits. What is *not* exact, and is
** therefore vendored, is every transcendental: those are where two
** correct libms are allowed to differ, and do.
*/

#ifndef dvlibm_private_h
#define dvlibm_private_h

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

/* The names the vendored sources use for these. */
typedef uint32_t u_int32_t;

/*
** '__inline' comes from the cdefs-compat.h this file replaces, and one
** vendored function carries it. Defined away rather than mapped to
** 'inline': a C99 inline definition with no external one is a different
** thing from an ordinary definition, and in a single translation unit
** with file-scope statics it is the wrong thing -- the compiler warns
** that a static is reachable from an inline function, which is exactly
** what it is. The generated code is the same either way; the compiler
** inlines what it wants to.
*/
#define __inline

/*
** A double is exactly eight bytes in the same order as a uint64_t on
** every target this builds for. Checked rather than assumed: the
** vendored code reads a double's exponent field by shifting, and on a
** machine where that were untrue it would return confident nonsense.
*/
typedef char dvlibm_double_is_binary64[(sizeof(double) == 8) ? 1 : -1];

#define DVLIBM_TOBITS(u, d) \
  do { double dvlibm__d = (d); memcpy(&(u), &dvlibm__d, 8); } while (0)

#define DVLIBM_FROMBITS(d, u) \
  do { uint64_t dvlibm__u = (u); memcpy(&(d), &dvlibm__u, 8); } while (0)

#define EXTRACT_WORDS(ix0, ix1, d) \
  do { \
    uint64_t dvlibm__b; DVLIBM_TOBITS(dvlibm__b, d); \
    (ix0) = (uint32_t)(dvlibm__b >> 32); \
    (ix1) = (uint32_t)dvlibm__b; \
  } while (0)

#define GET_HIGH_WORD(i, d) \
  do { \
    uint64_t dvlibm__b; DVLIBM_TOBITS(dvlibm__b, d); \
    (i) = (uint32_t)(dvlibm__b >> 32); \
  } while (0)

#define GET_LOW_WORD(i, d) \
  do { \
    uint64_t dvlibm__b; DVLIBM_TOBITS(dvlibm__b, d); \
    (i) = (uint32_t)dvlibm__b; \
  } while (0)

#define INSERT_WORDS(d, ix0, ix1) \
  DVLIBM_FROMBITS(d, ((uint64_t)(uint32_t)(ix0) << 32) | (uint32_t)(ix1))

#define SET_HIGH_WORD(d, v) \
  do { \
    uint64_t dvlibm__b; DVLIBM_TOBITS(dvlibm__b, d); \
    dvlibm__b = (dvlibm__b & UINT64_C(0x00000000ffffffff)) | \
                ((uint64_t)(uint32_t)(v) << 32); \
    DVLIBM_FROMBITS(d, dvlibm__b); \
  } while (0)

#define SET_LOW_WORD(d, v) \
  do { \
    uint64_t dvlibm__b; DVLIBM_TOBITS(dvlibm__b, d); \
    dvlibm__b = (dvlibm__b & UINT64_C(0xffffffff00000000)) | \
                (uint32_t)(v); \
    DVLIBM_FROMBITS(d, dvlibm__b); \
  } while (0)

/*
** Round an intermediate to the declared type at the assignment.
**
** Upstream makes this conditional on whether the target evaluates in
** excess precision. Here it is unconditional and spelled with a
** 'volatile' temporary, which forces the store-and-reload on any target
** that would otherwise keep an x87 80-bit intermediate. The build
** already passes '-fexcess-precision=standard' where the compiler
** accepts it (3.5); this is the same guarantee written into the source,
** so it does not depend on a flag reaching this translation unit.
*/
#define STRICT_ASSIGN(type, lval, rval) \
  do { volatile type dvlibm__v = (rval); (lval) = dvlibm__v; } while (0)

#endif
