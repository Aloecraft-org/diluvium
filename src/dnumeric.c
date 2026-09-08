/*
** dnumeric.c
** The 'array' guest library. See dnumeric.h for why it is a library.
**
** Stage 0 of doc/diluvium-numeric-spec.md: a typed array value and the
** portable kernels over it, every one of them reproducible tier --
** bit-identical on every target this runtime builds for. That claim is
** the design, not a property the code happens to have, and section 4 of
** the spec is the list of things it costs:
**
**   * Reductions run in a canonical order: eight accumulators, element i
**     into accumulator i mod 8 ascending, combined as
**     ((a0+a1)+(a2+a3))+((a4+a5)+(a6+a7)). Chosen so scalar code, wasm
**     SIMD128 (2 lanes), AVX2 (4) and AVX-512 (8) can all implement it
**     exactly while staying vectorised -- a later backend has to match
**     these bits, and this order is what makes that possible rather than
**     merely hoped for.
**   * Nothing re-associates and nothing fuses. That is the compiler's
**     side of the bargain and it is bought with the flags in
**     doc/Plan-2026-09.md 3.5, which test/contraction_check.c asserts.
**   * Sorts are stable, the comparator is total, and NaN sorts last.
**   * Group ids are assigned in first-appearance order, and the hash
**     behind them is a fixed 64-bit mix -- never the string hash seed,
**     which is a per-build constant.
**
** ------------------------------------------------------------ surface --
**
** Entry points:
**   luaopen_dnumeric        registers the 'array' table; see 'arraylib'
**   diluvium_array_adopt    the ABI's zero-copy handover ('dv_array_adopt')
**
** Configurable values:
**   DVN_BLOCK      elements charged against the instruction budget as one
**   DVN_ACC        accumulators in the canonical reduction order
**   DVN_MTNAME     the metatable's registry name
**   DVN_MAXDIM     dimensions an array may have
**   DVN_TAU        2*pi, the constant every FFT twiddle is built from
**   DVN_NTT_P      the NTT's prime, and DVN_NTT_G its primitive root
**
** Fan-out points, each a table or a switch and nothing declared
** elsewhere:
**   arraylib       the library's functions, and the method set behind
**                  '__index'
**   arraymeta      the metamethods
**   dvn_width      the dtype table: bytes per element
**   dvn_getf / dvn_geti / dvn_setf / dvn_seti   the dtype accessors
**   dvn_getc / dvn_setc                         the c128 pair accessors
**   dvn_check / dvn_checkany   which kernels a c128 array may enter
**   dvn_binop      the elementwise operator set
**   dvn_reduce     the reduction set
**
** Below the surface the file runs: values and layout, the budget, dtype
** access, construction, views, the canonical reduction, elementwise,
** reductions, sorting, grouping, linear algebra, complex, transforms,
** formatting, registration.
*/

#define dnumeric_c

#include "lprefix.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "dlibm.h"    /* the twiddles: stage 1's dv_cos and dv_sin */
#include "dnumeric.h"

#if defined(DV_NUMERIC)

/* ============================================================ values == */

/*
** Elements charged as one instruction (doc/Plan-2026-09.md 3.4). Also the
** block a kernel checks its budget at, so the element a run stops on is
** the same on every target.
*/
#define DVN_BLOCK	64u

/* Accumulators in the canonical reduction order. Not adjustable in any
   real sense: the number is part of the spec and a backend has to match
   it. Named so the arithmetic below reads. */
#define DVN_ACC		8

#define DVN_MAXDIM	2

#define DVN_MTNAME	"diluvium.array"

/*
** An array, or a view of one.
**
** 'data' points at logical element zero, so a view needs no separate
** offset. Strides are in elements and never negative: no form here
** produces a reversed view, and the kernels rely on that to index with
** 'size_t'.
**
** A view keeps its base alive through uservalue 1 rather than a
** refcount, which is also what stops a base being collected while a
** kernel is walking it.
*/
#define DVN_OWN_NONE	0	/* a view; 'data' belongs to the base */
#define DVN_OWN_INLINE	1	/* the buffer is the tail of this userdata */
#define DVN_OWN_EXTERN	2	/* adopted from a host; '__gc' frees it */

typedef struct dv_array {
  void *data;
  size_t shape[DVN_MAXDIM];
  ptrdiff_t stride[DVN_MAXDIM];   /* in elements */
  size_t nelem;                   /* product of 'shape' */
  unsigned char dtype;
  unsigned char ndim;
  unsigned char owns;
  unsigned char contig;           /* row-major and dense: offset == index */
} dv_array;


/* ============================================================ budget == */

/*
** A kernel's budget cursor. Opened once, charged once per block; see
** 'diluvium_budget_charge' in dv.c for why it is split in two.
*/
typedef struct dvn_meter {
  lua_State *L;
  void *cookie;
} dvn_meter;

static void dvn_meter_open (lua_State *L, dvn_meter *m) {
  m->L = L;
  m->cookie = diluvium_budget_open(L);
}

/* One block processed. Raises when the budget is spent, at a block
   boundary, which is the same element on every target. */
static void dvn_meter_block (dvn_meter *m) {
  diluvium_budget_charge(m->L, m->cookie, 1);
}

/* Charge for 'n' elements that were walked without a block loop -- a
   whole-array pass whose shape makes blocking pointless. Rounded up, so
   a short pass still costs something. */
static void dvn_meter_elems (dvn_meter *m, size_t n) {
  diluvium_budget_charge(m->L, m->cookie,
                         (uint64_t)((n + DVN_BLOCK - 1) / DVN_BLOCK));
}


/* ====================================================== dtype access == */

static size_t dvn_width (int dtype) {
  switch (dtype) {
    case DVN_F64:  return sizeof(double);
    case DVN_I64:  return sizeof(lua_Integer);
    case DVN_U8:   return 1;
    case DVN_C128: return 2 * sizeof(double);
    default:       return 0;
  }
}

static const char *dvn_dtypename (int dtype) {
  switch (dtype) {
    case DVN_F64:  return "f64";
    case DVN_I64:  return "i64";
    case DVN_U8:   return "u8";
    case DVN_C128: return "c128";
    default:       return "?";
  }
}

static int dvn_dtypecode (lua_State *L, const char *name) {
  if (strcmp(name, "f64") == 0) return DVN_F64;
  if (strcmp(name, "i64") == 0) return DVN_I64;
  if (strcmp(name, "u8") == 0) return DVN_U8;
  if (strcmp(name, "c128") == 0) return DVN_C128;
  return luaL_error(L, "unknown dtype '%s' (f64, i64, u8 or c128)", name);
}

/*
** The offset of logical element 'k' in row-major order.
**
** The contiguous case is the common one and is a comparison, not a
** division; the strided case pays for the view it bought.
*/
static size_t dvn_off (const dv_array *a, size_t k) {
  if (a->contig)
    return k;
  if (a->ndim == 1)
    return (size_t)((ptrdiff_t)k * a->stride[0]);
  else {
    size_t r = k / a->shape[1];
    size_t c = k - r * a->shape[1];
    return (size_t)((ptrdiff_t)r * a->stride[0] + (ptrdiff_t)c * a->stride[1]);
  }
}

static size_t dvn_off2 (const dv_array *a, size_t r, size_t c) {
  return (size_t)((ptrdiff_t)r * a->stride[0] + (ptrdiff_t)c * a->stride[1]);
}

/*
** The four scalar accessors. 'c128' is not one of the cases and must not
** reach them: it is two doubles per element, so a c128 array read as u8
** would return a byte of a mantissa and say nothing was wrong.
** 'dvn_check' is what keeps it away: every kernel that takes a real
** array goes through it, and 'dvn_operand_read' makes the same refusal
** for the elementwise path. The 'default' case is u8, and the comment on
** each is there because a reader of these four cannot see that from
** here.
*/
static double dvn_getf (const dv_array *a, size_t off) {
  switch (a->dtype) {
    case DVN_F64: return ((const double *)a->data)[off];
    case DVN_I64: return (double)((const lua_Integer *)a->data)[off];
    default: /* DVN_U8; c128 never arrives, see 'dvn_check' */
             return (double)((const unsigned char *)a->data)[off];
  }
}

static lua_Integer dvn_geti (const dv_array *a, size_t off) {
  switch (a->dtype) {
    case DVN_F64: return (lua_Integer)((const double *)a->data)[off];
    case DVN_I64: return ((const lua_Integer *)a->data)[off];
    default: /* DVN_U8; c128 never arrives, see 'dvn_check' */
             return (lua_Integer)((const unsigned char *)a->data)[off];
  }
}

static void dvn_setf (dv_array *a, size_t off, double v) {
  switch (a->dtype) {
    case DVN_F64: ((double *)a->data)[off] = v; break;
    case DVN_I64: ((lua_Integer *)a->data)[off] = (lua_Integer)v; break;
    default: /* DVN_U8; c128 never arrives, see 'dvn_check' */
             ((unsigned char *)a->data)[off] = (unsigned char)v; break;
  }
}

static void dvn_seti (dv_array *a, size_t off, lua_Integer v) {
  switch (a->dtype) {
    case DVN_F64: ((double *)a->data)[off] = (double)v; break;
    case DVN_I64: ((lua_Integer *)a->data)[off] = v; break;
    default: /* DVN_U8; c128 never arrives, see 'dvn_check' */
             ((unsigned char *)a->data)[off] = (unsigned char)(v & 0xFF);
             break;
  }
}

/*
** A c128 element is the pair of doubles at 2*off, real part first. The
** offset arithmetic is the same as every other dtype's -- 'dvn_off'
** counts elements -- and only these two functions know an element is
** two of anything.
*/
static void dvn_getc (const dv_array *a, size_t off, double *re, double *im) {
  const double *p = (const double *)a->data + 2 * off;
  *re = p[0];
  *im = p[1];
}

static void dvn_setc (dv_array *a, size_t off, double re, double im) {
  double *p = (double *)a->data + 2 * off;
  p[0] = re;
  p[1] = im;
}

/* Is this dtype a whole number? Decides whether an operator stays exact
   tier or becomes a float one. */
static int dvn_isint (int dtype) {
  return dtype == DVN_I64 || dtype == DVN_U8;
}


/* ====================================================== construction == */

/*
** Two ways to take an array argument, and which one a function uses is
** the whole of how 'c128' is kept out of the kernels that cannot mean
** anything for it.
**
** 'dvn_checkany' is any array. It is what the shape and view functions
** use, and the transforms, and anything that copies elements without
** interpreting them.
**
** 'dvn_check' is an array of a *real* dtype, and is the default. Every
** elementwise operator, every reduction, the sorts, the grouping and the
** linear algebra go through it, so none of them had to grow a complex
** case and none of them can silently read a c128 array as bytes. What a
** program gets instead is a sentence naming the dtype and the two
** functions that turn it into one this kernel can take.
*/
static dv_array *dvn_checkany (lua_State *L, int idx) {
  return (dv_array *)luaL_checkudata(L, idx, DVN_MTNAME);
}

static dv_array *dvn_check (lua_State *L, int idx) {
  dv_array *a = (dv_array *)luaL_checkudata(L, idx, DVN_MTNAME);
  if (a->dtype == DVN_C128)
    luaL_error(L, "this operation needs a real array, not c128 "
                  "(use array.real or array.imag)");
  return a;
}

static dv_array *dvn_test (lua_State *L, int idx) {
  return (dv_array *)luaL_testudata(L, idx, DVN_MTNAME);
}

static void dvn_setcontig (dv_array *a) {
  if (a->ndim == 1)
    a->contig = (a->stride[0] == 1);
  else
    a->contig = (a->stride[1] == 1 &&
                 a->stride[0] == (ptrdiff_t)a->shape[1]);
}

/*
** A fresh array owning its elements, zero-filled, pushed on the stack.
**
** The buffer is the tail of the userdata rather than a separate
** allocation: one block for the collector to account and free, and every
** byte of it goes through the instance's allocator, so a memory budget
** sees an array exactly as it sees a table.
*/
static dv_array *dvn_new (lua_State *L, int dtype, size_t d0, size_t d1,
                          int ndim) {
  size_t width = dvn_width(dtype);
  size_t nelem = (ndim == 1) ? d0 : d0 * d1;
  dv_array *a;
  /* Overflow is a real input, not a theoretical one: 'array.new("f64",
     m, n)' takes two numbers straight from a program. */
  if (ndim == 2 && d1 != 0 && d0 > (size_t)-1 / d1)
    luaL_error(L, "array too large: %I by %I elements",
               (lua_Integer)d0, (lua_Integer)d1);
  if (width != 0 && nelem > ((size_t)-1 - sizeof(dv_array)) / width)
    luaL_error(L, "array too large: %I elements", (lua_Integer)nelem);
  a = (dv_array *)lua_newuserdatauv(L, sizeof(dv_array) + nelem * width, 1);
  memset(a, 0, sizeof(dv_array));
  a->data = (char *)a + sizeof(dv_array);
  memset(a->data, 0, nelem * width);
  a->dtype = (unsigned char)dtype;
  a->ndim = (unsigned char)ndim;
  a->owns = DVN_OWN_INLINE;
  a->nelem = nelem;
  a->shape[0] = d0;
  a->shape[1] = (ndim == 2) ? d1 : 0;
  a->stride[0] = (ndim == 1) ? 1 : (ptrdiff_t)d1;
  a->stride[1] = (ndim == 2) ? 1 : 0;
  dvn_setcontig(a);
  luaL_setmetatable(L, DVN_MTNAME);
  return a;
}

/*
** A view of the array at 'baseidx', pushed on the stack. The caller fills
** in 'data', the shape and the strides; everything else is here so a view
** cannot be built half-formed.
*/
static dv_array *dvn_newview (lua_State *L, int baseidx, const dv_array *base) {
  dv_array *v = (dv_array *)lua_newuserdatauv(L, sizeof(dv_array), 1);
  memset(v, 0, sizeof(dv_array));
  v->dtype = base->dtype;
  v->owns = DVN_OWN_NONE;
  luaL_setmetatable(L, DVN_MTNAME);
  /* uservalue 1 holds the base, so the buffer outlives every view of it */
  lua_pushvalue(L, baseidx);
  lua_setiuservalue(L, -2, 1);
  return v;
}

static int dvn_gc (lua_State *L) {
  dv_array *a = (dv_array *)luaL_checkudata(L, 1, DVN_MTNAME);
  if (a->owns == DVN_OWN_EXTERN && a->data != NULL) {
    free(a->data);
    a->data = NULL;
  }
  return 0;
}


/* ====================================== the canonical reduction order == */

/*
** Sum in the canonical order (numeric spec section 4).
**
** Eight accumulators, element i into accumulator i mod 8 for i ascending
** with the tail included, combined pairwise as
** ((a0+a1)+(a2+a3))+((a4+a5)+(a6+a7)).
**
** Every float reduction in this file goes through here or through the
** same shape, because the order *is* the contract: a backend that sums
** differently is a different answer, not a faster one.
*/
static double dvn_csum (const dv_array *a, size_t from, size_t n,
                        dvn_meter *m) {
  double acc[DVN_ACC];
  size_t i;
  int k;
  for (k = 0; k < DVN_ACC; k++)
    acc[k] = 0.0;
  for (i = 0; i < n; i++) {
    acc[i & (DVN_ACC - 1)] += dvn_getf(a, dvn_off(a, from + i));
    if ((i % DVN_BLOCK) == DVN_BLOCK - 1)
      dvn_meter_block(m);
  }
  return ((acc[0] + acc[1]) + (acc[2] + acc[3])) +
         ((acc[4] + acc[5]) + (acc[6] + acc[7]));
}

/* The same order over a caller's buffer, for the two-pass variance and
   for 'dot', whose products are formed before they are summed. */
static double dvn_csumbuf (const double *x, size_t n, dvn_meter *m) {
  double acc[DVN_ACC];
  size_t i;
  int k;
  for (k = 0; k < DVN_ACC; k++)
    acc[k] = 0.0;
  for (i = 0; i < n; i++) {
    acc[i & (DVN_ACC - 1)] += x[i];
    if ((i % DVN_BLOCK) == DVN_BLOCK - 1)
      dvn_meter_block(m);
  }
  return ((acc[0] + acc[1]) + (acc[2] + acc[3])) +
         ((acc[4] + acc[5]) + (acc[6] + acc[7]));
}

/*
** The integer sum, in the same shape.
**
** Integer addition is associative and exact, so the order changes
** nothing here -- it is written the same way so there is one reduction
** to reason about rather than two, and so a reader who checks the float
** one against the spec has checked both.
*/
static lua_Integer dvn_cisum (const dv_array *a, size_t from, size_t n,
                              dvn_meter *m) {
  lua_Unsigned acc[DVN_ACC];
  size_t i;
  int k;
  for (k = 0; k < DVN_ACC; k++)
    acc[k] = 0;
  for (i = 0; i < n; i++) {
    acc[i & (DVN_ACC - 1)] += (lua_Unsigned)dvn_geti(a, dvn_off(a, from + i));
    if ((i % DVN_BLOCK) == DVN_BLOCK - 1)
      dvn_meter_block(m);
  }
  return (lua_Integer)(((acc[0] + acc[1]) + (acc[2] + acc[3])) +
                       ((acc[4] + acc[5]) + (acc[6] + acc[7])));
}


/* ================================================= the total ordering == */

/*
** A total order with NaN last (numeric spec section 4).
**
** IEEE comparison is not an ordering -- every comparison with NaN is
** false, so a sort using '<' directly has undefined behaviour the moment
** a NaN is present and can produce a different permutation on two
** targets for the same input. This is a strict weak ordering in which all
** NaNs are equivalent and greater than everything, and in which -0.0 and
** +0.0 are equivalent, which is what a stable sort needs to be
** deterministic.
*/
static int dvn_ltf (double x, double y) {
  if (x != x) return 0;             /* NaN is less than nothing */
  if (y != y) return 1;             /* and everything else precedes it */
  return x < y;
}

/*
** The same order between two logical positions of one array.
**
** Integer arrays compare as integers and never through 'double': an
** i64 beyond 2^53 does not survive that conversion, so a sort or a
** 'min' that converted first would answer the wrong element for values
** a program can perfectly well hold.
*/
static int dvn_ltat (const dv_array *a, size_t ia, size_t ib) {
  if (dvn_isint(a->dtype))
    return dvn_geti(a, dvn_off(a, ia)) < dvn_geti(a, dvn_off(a, ib));
  return dvn_ltf(dvn_getf(a, dvn_off(a, ia)), dvn_getf(a, dvn_off(a, ib)));
}


/* ========================================================= elementwise == */

/* The operator set. One entry per '__' metamethod plus the named
   comparisons; nothing dispatches on an operator anywhere else. */
typedef enum dvn_binop {
  DVN_ADD, DVN_SUB, DVN_MUL, DVN_DIV, DVN_IDIV, DVN_MOD, DVN_POW,
  DVN_EQ, DVN_NE, DVN_LT, DVN_LE, DVN_GT, DVN_GE
} dvn_binop;

static int dvn_iscmp (dvn_binop op) {
  return op >= DVN_EQ;
}

/* Operators that are float whatever they are given, exactly as in Lua:
   '/' and '^' always produce a float. */
static int dvn_isfloatop (dvn_binop op) {
  return op == DVN_DIV || op == DVN_POW;
}

/*
** Lua's integer floor division and modulo, reimplemented rather than
** reached for.
**
** 'luaV_idiv' and 'luaV_mod' are core internals, and this file's rule --
** the rule for every d*.c but dshim.c -- is the public C API only. The
** definitions are short and they are the ones a program will compare
** against, so they are worth getting exactly right: floor semantics, a
** raise on division by zero, and the two special cases around
** LUA_MININTEGER that make the wrapping well defined.
*/
static lua_Integer dvn_idiv (lua_State *L, lua_Integer m, lua_Integer n) {
  if ((lua_Unsigned)n + 1u <= 1u) {  /* n is 0 or -1 */
    if (n == 0)
      luaL_error(L, "attempt to perform 'n//0'");
    return (lua_Integer)(0u - (lua_Unsigned)m);  /* n == -1; avoid overflow */
  }
  else {
    lua_Integer q = m / n;
    if ((m ^ n) < 0 && m % n != 0)
      q -= 1;  /* correct a truncation towards zero into a floor */
    return q;
  }
}

static lua_Integer dvn_imod (lua_State *L, lua_Integer m, lua_Integer n) {
  if ((lua_Unsigned)n + 1u <= 1u) {
    if (n == 0)
      luaL_error(L, "attempt to perform 'n%%0'");
    return 0;  /* n == -1; avoid overflow with LUA_MININTEGER */
  }
  else {
    lua_Integer r = m % n;
    if (r != 0 && (r ^ n) < 0)
      r += n;  /* the result takes the divisor's sign, as Lua's does */
    return r;
  }
}

static double dvn_fmod (double m, double n) {
  double r = fmod(m, n);
  if (r * n < 0)
    r += n;  /* Lua's float modulo, whose result takes the divisor's sign */
  return r;
}

static double dvn_applyf (lua_State *L, dvn_binop op, double x, double y) {
  (void)L;  /* the float operators raise nothing; the integer ones do */
  switch (op) {
    case DVN_ADD:  return x + y;
    case DVN_SUB:  return x - y;
    case DVN_MUL:  return x * y;
    case DVN_DIV:  return x / y;
    case DVN_IDIV: return floor(x / y);
    case DVN_MOD:  return dvn_fmod(x, y);
    case DVN_POW:  return pow(x, y);
    case DVN_EQ:   return (x == y) ? 1.0 : 0.0;
    case DVN_NE:   return (x != y) ? 1.0 : 0.0;
    case DVN_LT:   return dvn_ltf(x, y) ? 1.0 : 0.0;
    case DVN_LE:   return dvn_ltf(y, x) ? 0.0 : 1.0;
    case DVN_GT:   return dvn_ltf(y, x) ? 1.0 : 0.0;
    default:       return dvn_ltf(x, y) ? 0.0 : 1.0;   /* DVN_GE */
  }
}

static lua_Integer dvn_applyi (lua_State *L, dvn_binop op,
                               lua_Integer x, lua_Integer y) {
  switch (op) {
    case DVN_ADD:  return (lua_Integer)((lua_Unsigned)x + (lua_Unsigned)y);
    case DVN_SUB:  return (lua_Integer)((lua_Unsigned)x - (lua_Unsigned)y);
    case DVN_MUL:  return (lua_Integer)((lua_Unsigned)x * (lua_Unsigned)y);
    case DVN_IDIV: return dvn_idiv(L, x, y);
    case DVN_MOD:  return dvn_imod(L, x, y);
    case DVN_EQ:   return (x == y);
    case DVN_NE:   return (x != y);
    case DVN_LT:   return (x < y);
    case DVN_LE:   return (x <= y);
    case DVN_GT:   return (x > y);
    case DVN_GE:   return (x >= y);
    default:       return 0;  /* DVN_DIV and DVN_POW never reach here */
  }
}

/*
** One operand of an elementwise operation: an array, or a scalar
** broadcast over one. Scalars only, per the spec -- shape broadcasting is
** not stage 0, and two arrays must agree exactly.
*/
typedef struct dvn_operand {
  dv_array *a;        /* NULL for a scalar */
  double f;
  lua_Integer i;
  int isint;
} dvn_operand;

static void dvn_operand_read (lua_State *L, int idx, dvn_operand *o) {
  o->a = dvn_test(L, idx);
  if (o->a != NULL) {
    if (o->a->dtype == DVN_C128)  /* the same refusal 'dvn_check' makes */
      dvn_check(L, idx);
    o->isint = dvn_isint(o->a->dtype);
    o->f = 0.0;
    o->i = 0;
  }
  else if (lua_isinteger(L, idx)) {
    o->isint = 1;
    o->i = lua_tointeger(L, idx);
    o->f = (double)o->i;
  }
  else if (lua_isnumber(L, idx)) {
    o->isint = 0;
    o->f = lua_tonumber(L, idx);
    o->i = 0;
  }
  else {
    luaL_error(L, "array operand expected, got %s", luaL_typename(L, idx));
  }
}

static double dvn_operand_f (const dvn_operand *o, size_t k) {
  return (o->a != NULL) ? dvn_getf(o->a, dvn_off(o->a, k)) : o->f;
}

static lua_Integer dvn_operand_i (const dvn_operand *o, size_t k) {
  return (o->a != NULL) ? dvn_geti(o->a, dvn_off(o->a, k)) : o->i;
}

/*
** The shape both operands share, and the array that defines it. Raises
** when two arrays disagree, because a silent broadcast is the thing this
** stage is explicitly not doing.
*/
static dv_array *dvn_shape2 (lua_State *L, dvn_operand *x, dvn_operand *y) {
  if (x->a != NULL && y->a != NULL) {
    if (x->a->ndim != y->a->ndim ||
        x->a->shape[0] != y->a->shape[0] ||
        x->a->shape[1] != y->a->shape[1])
      luaL_error(L, "shape mismatch: %I by %I against %I by %I",
                 (lua_Integer)x->a->shape[0], (lua_Integer)x->a->shape[1],
                 (lua_Integer)y->a->shape[0], (lua_Integer)y->a->shape[1]);
    return x->a;
  }
  if (x->a != NULL) return x->a;
  if (y->a != NULL) return y->a;
  luaL_error(L, "at least one operand must be an array");
  return NULL;  /* unreachable; keeps the compiler quiet */
}

static int dvn_binary (lua_State *L, dvn_binop op) {
  dvn_operand x, y;
  dv_array *shape, *out;
  dvn_meter m;
  size_t k, n;
  int rdtype, useint;
  dvn_operand_read(L, 1, &x);
  dvn_operand_read(L, 2, &y);
  shape = dvn_shape2(L, &x, &y);
  useint = x.isint && y.isint && !dvn_isfloatop(op);
  if (dvn_iscmp(op))
    rdtype = DVN_U8;              /* a mask, per the spec */
  else if (useint)
    rdtype = DVN_I64;             /* u8 arithmetic promotes; masks do not
                                     silently wrap into a byte */
  else
    rdtype = DVN_F64;
  dvn_meter_open(L, &m);
  out = dvn_new(L, rdtype, shape->shape[0], shape->shape[1], shape->ndim);
  n = shape->nelem;
  for (k = 0; k < n; k++) {
    /* Integer operands take the integer operator, whatever the result
       dtype is: a comparison writes 0 or 1 into a u8 mask either way,
       and comparing two i64 through 'double' would lose the answer
       above 2^53. 'useint' is already false for '/' and '^'. */
    if (useint)
      dvn_seti(out, k, dvn_applyi(L, op, dvn_operand_i(&x, k),
                                          dvn_operand_i(&y, k)));
    else
      dvn_setf(out, k, dvn_applyf(L, op, dvn_operand_f(&x, k),
                                          dvn_operand_f(&y, k)));
    if ((k % DVN_BLOCK) == DVN_BLOCK - 1)
      dvn_meter_block(&m);
  }
  return 1;
}

static int dvn_mm_add (lua_State *L) { return dvn_binary(L, DVN_ADD); }
static int dvn_mm_sub (lua_State *L) { return dvn_binary(L, DVN_SUB); }
static int dvn_mm_mul (lua_State *L) { return dvn_binary(L, DVN_MUL); }
static int dvn_mm_div (lua_State *L) { return dvn_binary(L, DVN_DIV); }
static int dvn_mm_idiv (lua_State *L) { return dvn_binary(L, DVN_IDIV); }
static int dvn_mm_mod (lua_State *L) { return dvn_binary(L, DVN_MOD); }
static int dvn_mm_pow (lua_State *L) { return dvn_binary(L, DVN_POW); }

static int dvn_f_eq (lua_State *L) { return dvn_binary(L, DVN_EQ); }
static int dvn_f_ne (lua_State *L) { return dvn_binary(L, DVN_NE); }
static int dvn_f_lt (lua_State *L) { return dvn_binary(L, DVN_LT); }
static int dvn_f_le (lua_State *L) { return dvn_binary(L, DVN_LE); }
static int dvn_f_gt (lua_State *L) { return dvn_binary(L, DVN_GT); }
static int dvn_f_ge (lua_State *L) { return dvn_binary(L, DVN_GE); }

static int dvn_mm_unm (lua_State *L) {
  dv_array *a = dvn_check(L, 1);
  dv_array *out;
  dvn_meter m;
  size_t k;
  int isint = dvn_isint(a->dtype);
  dvn_meter_open(L, &m);
  out = dvn_new(L, isint ? DVN_I64 : DVN_F64, a->shape[0], a->shape[1],
                a->ndim);
  for (k = 0; k < a->nelem; k++) {
    if (isint)
      dvn_seti(out, k,
               (lua_Integer)(0u - (lua_Unsigned)dvn_geti(a, dvn_off(a, k))));
    else
      dvn_setf(out, k, -dvn_getf(a, dvn_off(a, k)));
    if ((k % DVN_BLOCK) == DVN_BLOCK - 1)
      dvn_meter_block(&m);
  }
  return 1;
}


/* ========================================================= reductions == */

typedef enum dvn_reduce {
  DVN_SUM, DVN_MEAN, DVN_MIN, DVN_MAX, DVN_PROD, DVN_VAR, DVN_STD,
  DVN_ARGMIN, DVN_ARGMAX
} dvn_reduce;

/*
** Reduce a run of 'n' elements starting at logical index 'from'.
**
** Pushes one Lua value. Integer arrays stay exact for sum, prod, min and
** max; mean, var and std are float by definition, so they convert. An
** empty run gives 0 for sum, 1 for prod, and nil for everything whose
** answer would have to be invented.
*/
static void dvn_reduce_run (lua_State *L, dv_array *a, size_t from, size_t n,
                            dvn_reduce which, dvn_meter *m) {
  int isint = dvn_isint(a->dtype);
  size_t i;
  switch (which) {
    case DVN_SUM: {
      if (isint) lua_pushinteger(L, dvn_cisum(a, from, n, m));
      else lua_pushnumber(L, dvn_csum(a, from, n, m));
      return;
    }
    case DVN_MEAN: {
      /* Sum in the canonical order, then exactly one division. */
      if (n == 0) { lua_pushnil(L); return; }
      lua_pushnumber(L, dvn_csum(a, from, n, m) / (double)n);
      return;
    }
    case DVN_PROD: {
      if (isint) {
        lua_Unsigned p = 1;
        for (i = 0; i < n; i++) {
          p *= (lua_Unsigned)dvn_geti(a, dvn_off(a, from + i));
          if ((i % DVN_BLOCK) == DVN_BLOCK - 1) dvn_meter_block(m);
        }
        lua_pushinteger(L, (lua_Integer)p);
      }
      else {
        /* Sequential, and it has to be: multiplication does not
           re-associate in floating point either, so a product has one
           order and this is it -- ascending, left to right. */
        double p = 1.0;
        for (i = 0; i < n; i++) {
          p *= dvn_getf(a, dvn_off(a, from + i));
          if ((i % DVN_BLOCK) == DVN_BLOCK - 1) dvn_meter_block(m);
        }
        lua_pushnumber(L, p);
      }
      return;
    }
    case DVN_MIN: case DVN_MAX: case DVN_ARGMIN: case DVN_ARGMAX: {
      size_t best = 0;
      int wantmin = (which == DVN_MIN || which == DVN_ARGMIN);
      if (n == 0) { lua_pushnil(L); return; }
      for (i = 1; i < n; i++) {
        /* Strictly better only, so ties keep the lowest index -- which is
           what makes 'argmin' answerable at all.
           NaN follows from the ordering rather than being special-cased:
           it sorts last, so it never wins 'min' and always wins 'max'.
           One rule, and 'min'/'max' agree with the first and last
           elements of 'sort' -- which a 'max' that skipped NaN would
           not. */
        if (wantmin ? dvn_ltat(a, from + i, from + best)
                    : dvn_ltat(a, from + best, from + i))
          best = i;
        if ((i % DVN_BLOCK) == DVN_BLOCK - 1) dvn_meter_block(m);
      }
      if (which == DVN_ARGMIN || which == DVN_ARGMAX)
        lua_pushinteger(L, (lua_Integer)best + 1);  /* 1-based, as Lua is */
      else if (isint)
        lua_pushinteger(L, dvn_geti(a, dvn_off(a, from + best)));
      else
        lua_pushnumber(L, dvn_getf(a, dvn_off(a, from + best)));
      return;
    }
    default: {  /* DVN_VAR and DVN_STD */
      /* Two passes, as the spec states: the canonical mean, then the
         canonical sum of squared deviations. Population, not sample --
         one convention, stated, rather than a 'ddof' argument whose
         default would be the real decision. */
      double mean, ss;
      double *dev;
      if (n == 0) { lua_pushnil(L); return; }
      mean = dvn_csum(a, from, n, m) / (double)n;
      dev = (double *)lua_newuserdatauv(L, n * sizeof(double), 0);
      for (i = 0; i < n; i++) {
        double d = dvn_getf(a, dvn_off(a, from + i)) - mean;
        dev[i] = d * d;
        if ((i % DVN_BLOCK) == DVN_BLOCK - 1) dvn_meter_block(m);
      }
      ss = dvn_csumbuf(dev, n, m);
      lua_pop(L, 1);  /* the scratch buffer */
      lua_pushnumber(L, (which == DVN_VAR) ? ss / (double)n
                                           : sqrt(ss / (double)n));
      return;
    }
  }
}

/*
** reduce(a [, axis])
**
** With no axis the whole array reduces to one value. With an axis a 2D
** array reduces along it: axis 1 collapses rows and gives one value per
** column, axis 2 collapses columns and gives one value per row -- the
** 1-based reading of "axis" that matches this library's 1-based
** indexing.
*/
static int dvn_reduction (lua_State *L, dvn_reduce which) {
  dv_array *a = dvn_check(L, 1);
  dvn_meter m;
  lua_Integer axis = luaL_optinteger(L, 2, 0);
  dvn_meter_open(L, &m);
  if (axis == 0) {
    if (a->contig || a->ndim == 1) {
      dvn_reduce_run(L, a, 0, a->nelem, which, &m);
      return 1;
    }
    else {
      /* A strided 2D array has no single run to walk, so it is gathered
         into one first. Gathering costs a copy and keeps the reduction
         order the same as the contiguous case, which is what matters. */
      dv_array *flat = dvn_new(L, a->dtype, a->nelem, 0, 1);
      size_t k;
      for (k = 0; k < a->nelem; k++) {
        if (dvn_isint(a->dtype)) dvn_seti(flat, k, dvn_geti(a, dvn_off(a, k)));
        else dvn_setf(flat, k, dvn_getf(a, dvn_off(a, k)));
        if ((k % DVN_BLOCK) == DVN_BLOCK - 1) dvn_meter_block(&m);
      }
      dvn_reduce_run(L, flat, 0, flat->nelem, which, &m);
      return 1;
    }
  }
  luaL_argcheck(L, a->ndim == 2, 2, "an axis needs a 2D array");
  luaL_argcheck(L, axis == 1 || axis == 2, 2, "axis must be 1 or 2");
  {
    /* One lane at a time, gathered into a contiguous run so the
       reduction sees the same shape it always does. */
    size_t nout = (axis == 1) ? a->shape[1] : a->shape[0];
    size_t nin = (axis == 1) ? a->shape[0] : a->shape[1];
    size_t j, i;
    dv_array *lane = dvn_new(L, a->dtype, nin, 0, 1);
    int laneidx = lua_gettop(L);
    dv_array *out = NULL;
    int outidx;
    lua_pushnil(L);  /* placeholder for the result, filled on the first lane */
    outidx = lua_gettop(L);
    for (j = 0; j < nout; j++) {
      for (i = 0; i < nin; i++) {
        size_t off = (axis == 1) ? dvn_off2(a, i, j) : dvn_off2(a, j, i);
        if (dvn_isint(a->dtype)) dvn_seti(lane, i, dvn_geti(a, off));
        else dvn_setf(lane, i, dvn_getf(a, off));
        if ((i % DVN_BLOCK) == DVN_BLOCK - 1) dvn_meter_block(&m);
      }
      dvn_reduce_run(L, lane, 0, nin, which, &m);
      if (out == NULL) {
        /* The result dtype is whatever the first lane produced, so an
           integer sum stays integer and a mean is a float. */
        int rd = lua_isinteger(L, -1) ? DVN_I64 : DVN_F64;
        if (lua_isnil(L, -1)) rd = DVN_F64;
        out = dvn_new(L, rd, nout, 0, 1);
        lua_replace(L, outidx);
      }
      if (dvn_isint(out->dtype)) dvn_seti(out, j, lua_tointeger(L, -1));
      else dvn_setf(out, j, lua_tonumber(L, -1));
      lua_pop(L, 1);
    }
    if (out == NULL) {  /* no lanes at all */
      out = dvn_new(L, DVN_F64, 0, 0, 1);
      lua_replace(L, outidx);
    }
    lua_pushvalue(L, outidx);
    (void)laneidx;
    return 1;
  }
}

static int dvn_f_sum (lua_State *L) { return dvn_reduction(L, DVN_SUM); }
static int dvn_f_mean (lua_State *L) { return dvn_reduction(L, DVN_MEAN); }
static int dvn_f_min (lua_State *L) { return dvn_reduction(L, DVN_MIN); }
static int dvn_f_max (lua_State *L) { return dvn_reduction(L, DVN_MAX); }
static int dvn_f_prod (lua_State *L) { return dvn_reduction(L, DVN_PROD); }
static int dvn_f_var (lua_State *L) { return dvn_reduction(L, DVN_VAR); }
static int dvn_f_std (lua_State *L) { return dvn_reduction(L, DVN_STD); }
static int dvn_f_argmin (lua_State *L) { return dvn_reduction(L, DVN_ARGMIN); }
static int dvn_f_argmax (lua_State *L) { return dvn_reduction(L, DVN_ARGMAX); }

static int dvn_f_cumsum (lua_State *L) {
  dv_array *a = dvn_check(L, 1);
  dv_array *out;
  dvn_meter m;
  size_t k;
  int isint = dvn_isint(a->dtype);
  luaL_argcheck(L, a->ndim == 1, 1, "cumsum needs a 1D array");
  dvn_meter_open(L, &m);
  out = dvn_new(L, isint ? DVN_I64 : DVN_F64, a->shape[0], 0, 1);
  /* Sequential by definition: every prefix is a separate answer, so
     there is no accumulator order to choose. Ascending, left to right. */
  if (isint) {
    lua_Unsigned run = 0;
    for (k = 0; k < a->nelem; k++) {
      run += (lua_Unsigned)dvn_geti(a, dvn_off(a, k));
      dvn_seti(out, k, (lua_Integer)run);
      if ((k % DVN_BLOCK) == DVN_BLOCK - 1) dvn_meter_block(&m);
    }
  }
  else {
    double run = 0.0;
    for (k = 0; k < a->nelem; k++) {
      run += dvn_getf(a, dvn_off(a, k));
      dvn_setf(out, k, run);
      if ((k % DVN_BLOCK) == DVN_BLOCK - 1) dvn_meter_block(&m);
    }
  }
  return 1;
}


/* ============================================================ sorting == */

/*
** A stable sort of indices, bottom-up merge, comparing through the total
** order above.
**
** Merge rather than quicksort because stability is in the contract and
** because a merge visits its inputs in a fixed pattern: the same
** permutation on every target for the same input, which a pivot choice
** would not guarantee. Bottom-up rather than recursive so the depth is
** the scratch buffer's and not the C stack's.
**
** 'array.sort' is 'argsort' followed by a gather, so the two can never
** disagree about what order means.
*/
static void dvn_argsort_into (lua_State *L, dv_array *a, size_t *idx,
                              size_t *tmp, dvn_meter *m) {
  size_t n = a->nelem;
  size_t width, i;
  for (i = 0; i < n; i++)
    idx[i] = i;
  for (width = 1; width < n; width *= 2) {
    size_t lo;
    for (lo = 0; lo < n; lo += 2 * width) {
      size_t mid = lo + width < n ? lo + width : n;
      size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
      size_t i1 = lo, i2 = mid, k = lo;
      while (i1 < mid && i2 < hi) {
        /* The right element strictly before the left, or the left one
           goes first: equal keys keep their input order, which is what
           makes the sort stable. */
        tmp[k++] = dvn_ltat(a, idx[i2], idx[i1]) ? idx[i2++] : idx[i1++];
      }
      while (i1 < mid) tmp[k++] = idx[i1++];
      while (i2 < hi) tmp[k++] = idx[i2++];
    }
    memcpy(idx, tmp, n * sizeof(size_t));
    dvn_meter_elems(m, n);
  }
  (void)L;
}

/* Scratch for a sort: the index array and the merge buffer, as one
   userdata so an error inside the sort cannot leak either. */
static size_t *dvn_sortscratch (lua_State *L, size_t n, size_t **tmp) {
  size_t *p;
  if (n != 0 && n > ((size_t)-1 / sizeof(size_t)) / 2)
    luaL_error(L, "array too large to sort: %I elements", (lua_Integer)n);
  p = (size_t *)lua_newuserdatauv(L, 2 * n * sizeof(size_t) + 1, 0);
  *tmp = p + n;
  return p;
}

static int dvn_f_argsort (lua_State *L) {
  dv_array *a = dvn_check(L, 1);
  dv_array *out;
  dvn_meter m;
  size_t *idx, *tmp, k;
  luaL_argcheck(L, a->ndim == 1, 1, "argsort needs a 1D array");
  dvn_meter_open(L, &m);
  idx = dvn_sortscratch(L, a->nelem, &tmp);
  dvn_argsort_into(L, a, idx, tmp, &m);
  out = dvn_new(L, DVN_I64, a->nelem, 0, 1);
  for (k = 0; k < a->nelem; k++)
    dvn_seti(out, k, (lua_Integer)idx[k] + 1);  /* 1-based, as Lua is */
  return 1;
}

static int dvn_f_sort (lua_State *L) {
  dv_array *a = dvn_check(L, 1);
  dv_array *out;
  dvn_meter m;
  size_t *idx, *tmp, k;
  luaL_argcheck(L, a->ndim == 1, 1, "sort needs a 1D array");
  dvn_meter_open(L, &m);
  idx = dvn_sortscratch(L, a->nelem, &tmp);
  dvn_argsort_into(L, a, idx, tmp, &m);
  out = dvn_new(L, a->dtype, a->nelem, 0, 1);
  for (k = 0; k < a->nelem; k++) {
    size_t off = dvn_off(a, idx[k]);
    if (dvn_isint(a->dtype)) dvn_seti(out, k, dvn_geti(a, off));
    else dvn_setf(out, k, dvn_getf(a, off));
  }
  dvn_meter_elems(&m, a->nelem);
  return 1;
}


/* =========================================================== masking == */

static int dvn_truthy (const dv_array *mask, size_t k) {
  return dvn_getf(mask, dvn_off(mask, k)) != 0.0;
}

/* where(mask, a, b) -- elementwise select. 'a' and 'b' may be arrays of
   the mask's shape, or scalars. */
static int dvn_f_where (lua_State *L) {
  dv_array *mask = dvn_check(L, 1);
  dvn_operand x, y;
  dv_array *out;
  dvn_meter m;
  size_t k;
  int useint;
  dvn_operand_read(L, 2, &x);
  dvn_operand_read(L, 3, &y);
  if (x.a != NULL && x.a->nelem != mask->nelem)
    return luaL_error(L, "where: the second operand does not match the mask");
  if (y.a != NULL && y.a->nelem != mask->nelem)
    return luaL_error(L, "where: the third operand does not match the mask");
  useint = x.isint && y.isint;
  dvn_meter_open(L, &m);
  out = dvn_new(L, useint ? DVN_I64 : DVN_F64, mask->shape[0], mask->shape[1],
                mask->ndim);
  for (k = 0; k < mask->nelem; k++) {
    int pick = dvn_truthy(mask, k);
    if (useint)
      dvn_seti(out, k, pick ? dvn_operand_i(&x, k) : dvn_operand_i(&y, k));
    else
      dvn_setf(out, k, pick ? dvn_operand_f(&x, k) : dvn_operand_f(&y, k));
    if ((k % DVN_BLOCK) == DVN_BLOCK - 1) dvn_meter_block(&m);
  }
  return 1;
}

/* select(a, mask) -- the elements where the mask is non-zero, in index
   order, as a 1D array. */
static int dvn_f_select (lua_State *L) {
  dv_array *a = dvn_check(L, 1);
  dv_array *mask = dvn_check(L, 2);
  dv_array *out;
  dvn_meter m;
  size_t k, n = 0;
  luaL_argcheck(L, mask->nelem == a->nelem, 2, "the mask does not match");
  dvn_meter_open(L, &m);
  for (k = 0; k < a->nelem; k++)
    if (dvn_truthy(mask, k)) n++;
  dvn_meter_elems(&m, a->nelem);
  out = dvn_new(L, a->dtype, n, 0, 1);
  n = 0;
  for (k = 0; k < a->nelem; k++) {
    if (dvn_truthy(mask, k)) {
      size_t off = dvn_off(a, k);
      if (dvn_isint(a->dtype)) dvn_seti(out, n, dvn_geti(a, off));
      else dvn_setf(out, n, dvn_getf(a, off));
      n++;
    }
    if ((k % DVN_BLOCK) == DVN_BLOCK - 1) dvn_meter_block(&m);
  }
  return 1;
}


/* ========================================================== grouping == */

/*
** The 64-bit mix behind 'group_index'.
**
** A fixed function -- the SplitMix64 finaliser -- and not Lua's string
** hash, whose seed is a per-build constant: a group id that depended on
** it would differ between two builds of the same program, which is the
** opposite of what this library promises. Ids are assigned in
** first-appearance order, so the mix decides only where a key lands in
** the table and never what id it gets.
*/
static uint64_t dvn_mix (uint64_t x) {
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  x ^= x >> 31;
  return x;
}

/*
** An element as a 64-bit key.
**
** Two canonicalisations, both of which make the key agree with what '=='
** would say: every NaN is one key, and -0.0 and +0.0 are one key. Without
** them a table of returns could produce a different number of groups on
** two targets for inputs that compare equal.
*/
static uint64_t dvn_key (const dv_array *a, size_t off) {
  switch (a->dtype) {
    case DVN_F64: {
      double v = ((const double *)a->data)[off];
      uint64_t u;
      if (v != v) return UINT64_C(0x7ff8000000000000);  /* one NaN */
      if (v == 0.0) v = 0.0;                            /* one zero */
      memcpy(&u, &v, sizeof(u));
      return u;
    }
    case DVN_I64: return (uint64_t)((const lua_Integer *)a->data)[off];
    default:      return ((const unsigned char *)a->data)[off];
  }
}

/*
** group_index(keys) -> ids, ngroups
**
** 'ids' is an i64 array of 1-based group ids, one per key, assigned in
** first-appearance order. Open addressing with linear probing over a
** power-of-two table; the empty marker is a separate occupancy array
** rather than a reserved key value, because every 64-bit pattern is a
** legitimate key.
*/
static int dvn_f_group_index (lua_State *L) {
  dv_array *keys = dvn_check(L, 1);
  dv_array *ids;
  dvn_meter m;
  size_t n = keys->nelem, cap = 8, k, ngroups = 0;
  uint64_t *slotkey;
  size_t *slotid;
  unsigned char *used;
  luaL_argcheck(L, keys->ndim == 1, 1, "group_index needs a 1D array");
  while (cap < n * 2) {
    if (cap > ((size_t)-1) / 2)
      return luaL_error(L, "too many keys to group");
    cap *= 2;
  }
  dvn_meter_open(L, &m);
  /* One allocation for the whole table, through the instance's allocator
     so a memory budget sees it. */
  slotkey = (uint64_t *)lua_newuserdatauv(
      L, cap * (sizeof(uint64_t) + sizeof(size_t) + 1), 0);
  slotid = (size_t *)(void *)(slotkey + cap);
  used = (unsigned char *)(void *)(slotid + cap);
  memset(used, 0, cap);
  ids = dvn_new(L, DVN_I64, n, 0, 1);
  for (k = 0; k < n; k++) {
    uint64_t key = dvn_key(keys, dvn_off(keys, k));
    size_t slot = (size_t)(dvn_mix(key) & (uint64_t)(cap - 1));
    for (;;) {
      if (!used[slot]) {
        used[slot] = 1;
        slotkey[slot] = key;
        slotid[slot] = ++ngroups;   /* first appearance decides the id */
        break;
      }
      if (slotkey[slot] == key)
        break;
      slot = (slot + 1) & (cap - 1);
    }
    dvn_seti(ids, k, (lua_Integer)slotid[slot]);
    if ((k % DVN_BLOCK) == DVN_BLOCK - 1) dvn_meter_block(&m);
  }
  lua_pushinteger(L, (lua_Integer)ngroups);
  return 2;
}

typedef enum dvn_segop {
  DVN_SEG_SUM, DVN_SEG_MEAN, DVN_SEG_COUNT, DVN_SEG_MIN, DVN_SEG_MAX
} dvn_segop;

/*
** A segmented reduction over group ids.
**
** The sums keep the canonical order *within each group*: eight
** accumulators per group, and an element goes into accumulator
** (position within its group) mod 8. That is what "canonical order"
** means once the elements of a group are not contiguous -- the position
** is the group's own ascending index, not the array's.
**
** It costs eight doubles per group. Stated rather than hidden: that is
** the price of a segmented sum whose answer does not depend on how the
** rows happened to be ordered relative to each other.
*/
static int dvn_segreduce (lua_State *L, dvn_segop op) {
  dv_array *vals = dvn_check(L, 1);
  dv_array *ids = dvn_check(L, 2);
  lua_Integer ng = luaL_checkinteger(L, 3);
  dv_array *out;
  dvn_meter m;
  size_t n = vals->nelem, g, k, ngroups;
  double *acc = NULL;
  size_t *count;
  luaL_argcheck(L, ids->nelem == n, 2, "ids do not match the values");
  luaL_argcheck(L, ng >= 0, 3, "the group count cannot be negative");
  ngroups = (size_t)ng;
  dvn_meter_open(L, &m);
  count = (size_t *)lua_newuserdatauv(L, (ngroups + 1) * sizeof(size_t), 0);
  memset(count, 0, (ngroups + 1) * sizeof(size_t));
  if (op != DVN_SEG_COUNT) {
    if (ngroups > ((size_t)-1) / (DVN_ACC * sizeof(double)))
      return luaL_error(L, "too many groups");
    acc = (double *)lua_newuserdatauv(L, ngroups * DVN_ACC * sizeof(double), 0);
    for (g = 0; g < ngroups * DVN_ACC; g++)
      acc[g] = 0.0;
  }
  for (k = 0; k < n; k++) {
    lua_Integer id = dvn_geti(ids, dvn_off(ids, k));
    size_t gi;
    if (id < 1 || (size_t)id > ngroups)
      return luaL_error(L, "group id %I is outside 1..%I at element %I",
                        id, ng, (lua_Integer)k + 1);
    gi = (size_t)id - 1;
    if (op == DVN_SEG_COUNT)
      count[gi]++;
    else {
      double v = dvn_getf(vals, dvn_off(vals, k));
      if (op == DVN_SEG_MIN || op == DVN_SEG_MAX) {
        if (count[gi] == 0) acc[gi * DVN_ACC] = v;
        else {
          double b = acc[gi * DVN_ACC];
          if (op == DVN_SEG_MIN ? dvn_ltf(v, b) : dvn_ltf(b, v))
            acc[gi * DVN_ACC] = v;
        }
      }
      else {
        acc[gi * DVN_ACC + (count[gi] & (DVN_ACC - 1))] += v;
      }
      count[gi]++;
    }
    if ((k % DVN_BLOCK) == DVN_BLOCK - 1) dvn_meter_block(&m);
  }
  out = dvn_new(L, (op == DVN_SEG_COUNT) ? DVN_I64 : DVN_F64, ngroups, 0, 1);
  for (g = 0; g < ngroups; g++) {
    if (op == DVN_SEG_COUNT)
      dvn_seti(out, g, (lua_Integer)count[g]);
    else if (op == DVN_SEG_MIN || op == DVN_SEG_MAX)
      /* An empty group has no smallest element; NaN is the one answer
         that is not a value the group could have contained. */
      dvn_setf(out, g, count[g] == 0 ? (0.0 / 0.0) : acc[g * DVN_ACC]);
    else {
      const double *p = acc + g * DVN_ACC;
      double s = ((p[0] + p[1]) + (p[2] + p[3])) +
                 ((p[4] + p[5]) + (p[6] + p[7]));
      dvn_setf(out, g, (op == DVN_SEG_MEAN)
                       ? (count[g] == 0 ? (0.0 / 0.0) : s / (double)count[g])
                       : s);
    }
  }
  dvn_meter_elems(&m, ngroups);
  return 1;
}

static int dvn_f_group_sum (lua_State *L) {
  return dvn_segreduce(L, DVN_SEG_SUM);
}
static int dvn_f_group_mean (lua_State *L) {
  return dvn_segreduce(L, DVN_SEG_MEAN);
}
static int dvn_f_group_count (lua_State *L) {
  return dvn_segreduce(L, DVN_SEG_COUNT);
}
static int dvn_f_group_min (lua_State *L) {
  return dvn_segreduce(L, DVN_SEG_MIN);
}
static int dvn_f_group_max (lua_State *L) {
  return dvn_segreduce(L, DVN_SEG_MAX);
}


/* ==================================================== linear algebra == */

/*
** dot(a, b) -- elementwise product, unfused, then the canonical sum.
**
** The products are formed into a buffer first and summed afterwards, and
** that is not a detour: the spec defines 'dot' as exactly that, so a
** backend that fuses the multiply into the accumulation is a different
** answer. The buffer is what makes the two steps separable and visible.
*/
static int dvn_f_dot (lua_State *L) {
  dv_array *x = dvn_check(L, 1);
  dv_array *y = dvn_check(L, 2);
  dvn_meter m;
  size_t k, n;
  luaL_argcheck(L, x->ndim == 1 && y->ndim == 1, 1, "dot needs 1D arrays");
  luaL_argcheck(L, x->nelem == y->nelem, 2, "lengths differ");
  n = x->nelem;
  dvn_meter_open(L, &m);
  if (dvn_isint(x->dtype) && dvn_isint(y->dtype)) {
    lua_Unsigned acc[DVN_ACC];
    int i;
    for (i = 0; i < DVN_ACC; i++) acc[i] = 0;
    for (k = 0; k < n; k++) {
      acc[k & (DVN_ACC - 1)] +=
        (lua_Unsigned)dvn_geti(x, dvn_off(x, k)) *
        (lua_Unsigned)dvn_geti(y, dvn_off(y, k));
      if ((k % DVN_BLOCK) == DVN_BLOCK - 1) dvn_meter_block(&m);
    }
    lua_pushinteger(L, (lua_Integer)(((acc[0] + acc[1]) + (acc[2] + acc[3])) +
                                     ((acc[4] + acc[5]) + (acc[6] + acc[7]))));
    return 1;
  }
  else {
    double *prod = (double *)lua_newuserdatauv(L, (n + 1) * sizeof(double), 0);
    for (k = 0; k < n; k++) {
      prod[k] = dvn_getf(x, dvn_off(x, k)) * dvn_getf(y, dvn_off(y, k));
      if ((k % DVN_BLOCK) == DVN_BLOCK - 1) dvn_meter_block(&m);
    }
    lua_pushnumber(L, dvn_csumbuf(prod, n, &m));
    return 1;
  }
}

/*
** matmul(a, b) -- each cell is a dot in the canonical order.
**
** Naive triple loop on purpose. Blocking over rows and columns would be
** free -- the spec says so -- but the inner k order is not, and a naive
** loop is the one implementation that obviously has the order the spec
** asks for. Speed is Stage 3's job, through the backend vtable, and it
** will be held to these bits.
*/
static int dvn_f_matmul (lua_State *L) {
  dv_array *a = dvn_check(L, 1);
  dv_array *b = dvn_check(L, 2);
  dv_array *out;
  dvn_meter m;
  size_t i, j, k, rows, cols, inner;
  double *prod;
  luaL_argcheck(L, a->ndim == 2 && b->ndim == 2, 1, "matmul needs 2D arrays");
  luaL_argcheck(L, a->shape[1] == b->shape[0], 2,
                "inner dimensions do not agree");
  rows = a->shape[0];
  cols = b->shape[1];
  inner = a->shape[1];
  dvn_meter_open(L, &m);
  prod = (double *)lua_newuserdatauv(L, (inner + 1) * sizeof(double), 0);
  out = dvn_new(L, DVN_F64, rows, cols, 2);
  for (i = 0; i < rows; i++) {
    for (j = 0; j < cols; j++) {
      for (k = 0; k < inner; k++)
        prod[k] = dvn_getf(a, dvn_off2(a, i, k)) * dvn_getf(b, dvn_off2(b, k, j));
      dvn_setf(out, i * cols + j, dvn_csumbuf(prod, inner, &m));
    }
  }
  return 1;
}


/* ============================================= views and construction == */

static int dvn_f_new (lua_State *L) {
  int dtype = dvn_dtypecode(L, luaL_checkstring(L, 1));
  lua_Integer d0 = luaL_checkinteger(L, 2);
  lua_Integer d1 = luaL_optinteger(L, 3, -1);
  luaL_argcheck(L, d0 >= 0, 2, "a dimension cannot be negative");
  if (d1 < 0)
    dvn_new(L, dtype, (size_t)d0, 0, 1);
  else
    dvn_new(L, dtype, (size_t)d0, (size_t)d1, 2);
  return 1;
}

static int dvn_fill (lua_State *L, double v) {
  dv_array *a;
  size_t k;
  dvn_f_new(L);
  a = dvn_checkany(L, -1);
  for (k = 0; k < a->nelem; k++) {
    if (a->dtype == DVN_C128) dvn_setc(a, k, v, 0.0);  /* 'ones' is 1+0i */
    else dvn_setf(a, k, v);
  }
  return 1;
}

static int dvn_f_zeros (lua_State *L) { return dvn_fill(L, 0.0); }
static int dvn_f_ones (lua_State *L) { return dvn_fill(L, 1.0); }

/*
** from(t [, dtype]) -- a table, or a table of tables for 2D.
**
** The dtype defaults to f64 unless every value read is a Lua integer, in
** which case i64: the same rule Lua itself uses to decide whether an
** arithmetic result is an integer, applied once to the whole table
** rather than per element.
*/
static int dvn_f_from (lua_State *L) {
  dv_array *out;
  size_t rows, cols = 0, i, j;
  int dtype = -1, twod = 0;
  luaL_checktype(L, 1, LUA_TTABLE);
  if (!lua_isnoneornil(L, 2))
    dtype = dvn_dtypecode(L, luaL_checkstring(L, 2));
  rows = (size_t)luaL_len(L, 1);
  if (rows > 0) {
    lua_geti(L, 1, 1);
    twod = lua_istable(L, -1);
    if (twod) cols = (size_t)luaL_len(L, -1);
    lua_pop(L, 1);
  }
  if (dtype < 0) {
    /* One pass to decide the dtype, so a table of whole numbers does not
       silently become floats. */
    int allint = 1;
    for (i = 1; i <= rows && allint; i++) {
      lua_geti(L, 1, (lua_Integer)i);
      if (twod) {
        for (j = 1; j <= cols && allint; j++) {
          lua_geti(L, -1, (lua_Integer)j);
          if (!lua_isinteger(L, -1)) allint = 0;
          lua_pop(L, 1);
        }
      }
      else if (!lua_isinteger(L, -1))
        allint = 0;
      lua_pop(L, 1);
    }
    dtype = allint ? DVN_I64 : DVN_F64;
  }
  /* A c128 table is a list of pairs, so its outer entries are tables and
     the 2D test above would have read the first pair as a row. One
     dimension, and the pair is the element. */
  if (dtype == DVN_C128) twod = 0;
  out = twod ? dvn_new(L, dtype, rows, cols, 2)
             : dvn_new(L, dtype, rows, 0, 1);
  for (i = 1; i <= rows; i++) {
    lua_geti(L, 1, (lua_Integer)i);
    if (twod) {
      luaL_argcheck(L, lua_istable(L, -1), 1, "a ragged table");
      luaL_argcheck(L, (size_t)luaL_len(L, -1) == cols, 1, "a ragged table");
      for (j = 1; j <= cols; j++) {
        lua_geti(L, -1, (lua_Integer)j);
        if (dvn_isint(dtype))
          dvn_seti(out, (i - 1) * cols + (j - 1), luaL_checkinteger(L, -1));
        else
          dvn_setf(out, (i - 1) * cols + (j - 1), luaL_checknumber(L, -1));
        lua_pop(L, 1);
      }
    }
    else if (dtype == DVN_C128) {
      /* '{re, im}', or a bare number for a real value. */
      if (lua_istable(L, -1)) {
        double re, im;
        lua_geti(L, -1, 1); re = luaL_checknumber(L, -1); lua_pop(L, 1);
        lua_geti(L, -1, 2); im = luaL_optnumber(L, -1, 0.0); lua_pop(L, 1);
        dvn_setc(out, i - 1, re, im);
      }
      else
        dvn_setc(out, i - 1, luaL_checknumber(L, -1), 0.0);
    }
    else {
      if (dvn_isint(dtype)) dvn_seti(out, i - 1, luaL_checkinteger(L, -1));
      else dvn_setf(out, i - 1, luaL_checknumber(L, -1));
    }
    lua_pop(L, 1);
  }
  return 1;
}

/*
** One element onto the table below the top, at index 'k'. A c128
** element becomes a two-element table {re, im} -- the shape
** 'array.from' reads back, so 'from(to_table(a), "c128")' is 'a'.
*/
static void dvn_seteleminto (lua_State *L, const dv_array *a, size_t off,
                             lua_Integer k) {
  if (a->dtype == DVN_C128) {
    double re, im;
    dvn_getc(a, off, &re, &im);
    lua_createtable(L, 2, 0);
    lua_pushnumber(L, re); lua_seti(L, -2, 1);
    lua_pushnumber(L, im); lua_seti(L, -2, 2);
  }
  else if (dvn_isint(a->dtype)) lua_pushinteger(L, dvn_geti(a, off));
  else lua_pushnumber(L, dvn_getf(a, off));
  lua_seti(L, -2, k);
}

static int dvn_f_to_table (lua_State *L) {
  dv_array *a = dvn_checkany(L, 1);
  size_t i, j;
  if (a->ndim == 1) {
    lua_createtable(L, (int)a->nelem, 0);
    for (i = 0; i < a->nelem; i++)
      dvn_seteleminto(L, a, dvn_off(a, i), (lua_Integer)i + 1);
  }
  else {
    lua_createtable(L, (int)a->shape[0], 0);
    for (i = 0; i < a->shape[0]; i++) {
      lua_createtable(L, (int)a->shape[1], 0);
      for (j = 0; j < a->shape[1]; j++)
        dvn_seteleminto(L, a, dvn_off2(a, i, j), (lua_Integer)j + 1);
      lua_seti(L, -2, (lua_Integer)i + 1);
    }
  }
  return 1;
}

/* arange(dtype, start, stop [, step]) -- start inclusive, stop exclusive,
   the half-open interval every other range in this file uses. */
static int dvn_f_arange (lua_State *L) {
  int dtype = dvn_dtypecode(L, luaL_checkstring(L, 1));
  dv_array *out;
  size_t n = 0, k;
  if (dvn_isint(dtype)) {
    lua_Integer start = luaL_checkinteger(L, 2);
    lua_Integer stop = luaL_checkinteger(L, 3);
    lua_Integer step = luaL_optinteger(L, 4, 1);
    luaL_argcheck(L, step != 0, 4, "step cannot be zero");
    if ((step > 0 && stop > start) || (step < 0 && stop < start)) {
      lua_Unsigned span = (step > 0) ? (lua_Unsigned)stop - (lua_Unsigned)start
                                     : (lua_Unsigned)start - (lua_Unsigned)stop;
      lua_Unsigned mag = (step > 0) ? (lua_Unsigned)step
                                    : 0u - (lua_Unsigned)step;
      n = (size_t)((span + mag - 1) / mag);
    }
    out = dvn_new(L, dtype, n, 0, 1);
    for (k = 0; k < n; k++)
      dvn_seti(out, k, start + (lua_Integer)k * step);
  }
  else {
    double start = luaL_checknumber(L, 2);
    double stop = luaL_checknumber(L, 3);
    double step = luaL_optnumber(L, 4, 1.0);
    luaL_argcheck(L, step != 0.0, 4, "step cannot be zero");
    if ((step > 0.0 && stop > start) || (step < 0.0 && stop < start)) {
      double count = ceil((stop - start) / step);
      if (count > 0.0) n = (size_t)count;
    }
    out = dvn_new(L, dtype, n, 0, 1);
    /* start + k*step, never a running sum: a running sum accumulates
       rounding and makes the last element depend on the length. */
    for (k = 0; k < n; k++)
      dvn_setf(out, k, start + (double)k * step);
  }
  return 1;
}

/* linspace(start, stop, n) -- both ends inclusive, always f64. */
static int dvn_f_linspace (lua_State *L) {
  double start = luaL_checknumber(L, 1);
  double stop = luaL_checknumber(L, 2);
  lua_Integer n = luaL_checkinteger(L, 3);
  dv_array *out;
  size_t k;
  luaL_argcheck(L, n >= 0, 3, "count cannot be negative");
  out = dvn_new(L, DVN_F64, (size_t)n, 0, 1);
  if (n == 1)
    dvn_setf(out, 0, start);
  else if (n > 1) {
    double span = stop - start;
    for (k = 0; k < (size_t)n; k++)
      dvn_setf(out, k, start + span * ((double)k / (double)(n - 1)));
    /* The last element is the endpoint exactly, not start + span * 1.0,
       which need not round back to 'stop'. */
    dvn_setf(out, (size_t)n - 1, stop);
  }
  return 1;
}

/* slice(a, i, j) -- elements i..j inclusive for a 1D array, rows i..j for
   a 2D one. A view: no copy, and the base stays alive behind it. */
static int dvn_f_slice (lua_State *L) {
  dv_array *a = dvn_checkany(L, 1);
  lua_Integer i = luaL_checkinteger(L, 2);
  lua_Integer j = luaL_optinteger(L, 3, (lua_Integer)a->shape[0]);
  dv_array *v;
  size_t width = dvn_width(a->dtype);
  luaL_argcheck(L, i >= 1 && (size_t)i <= a->shape[0] + 1, 2, "out of range");
  luaL_argcheck(L, j >= i - 1 && (size_t)j <= a->shape[0], 3, "out of range");
  v = dvn_newview(L, 1, a);
  v->ndim = a->ndim;
  v->shape[0] = (size_t)(j - i + 1);
  v->shape[1] = a->shape[1];
  v->stride[0] = a->stride[0];
  v->stride[1] = a->stride[1];
  v->nelem = (a->ndim == 1) ? v->shape[0] : v->shape[0] * v->shape[1];
  v->data = (char *)a->data + (size_t)(i - 1) * (size_t)a->stride[0] * width;
  dvn_setcontig(v);
  return 1;
}

/* row(a, i) -- a 1D view of one row of a 2D array. */
static int dvn_f_row (lua_State *L) {
  dv_array *a = dvn_checkany(L, 1);
  lua_Integer i = luaL_checkinteger(L, 2);
  dv_array *v;
  size_t width = dvn_width(a->dtype);
  luaL_argcheck(L, a->ndim == 2, 1, "row needs a 2D array");
  luaL_argcheck(L, i >= 1 && (size_t)i <= a->shape[0], 2, "out of range");
  v = dvn_newview(L, 1, a);
  v->ndim = 1;
  v->shape[0] = a->shape[1];
  v->stride[0] = a->stride[1];
  v->nelem = v->shape[0];
  v->data = (char *)a->data + (size_t)(i - 1) * (size_t)a->stride[0] * width;
  dvn_setcontig(v);
  return 1;
}

/* transpose(a) -- a 2D view with the axes swapped. Strides, not a copy,
   which is the whole reason strides are in the header. */
static int dvn_f_transpose (lua_State *L) {
  dv_array *a = dvn_checkany(L, 1);
  dv_array *v;
  luaL_argcheck(L, a->ndim == 2, 1, "transpose needs a 2D array");
  v = dvn_newview(L, 1, a);
  v->ndim = 2;
  v->shape[0] = a->shape[1];
  v->shape[1] = a->shape[0];
  v->stride[0] = a->stride[1];
  v->stride[1] = a->stride[0];
  v->nelem = a->nelem;
  v->data = a->data;
  dvn_setcontig(v);
  return 1;
}

/* copy(a) -- a dense array with the same values, which is what turns a
   view back into something contiguous. */
static int dvn_f_copy (lua_State *L) {
  dv_array *a = dvn_checkany(L, 1);
  dv_array *out;
  dvn_meter m;
  size_t k;
  dvn_meter_open(L, &m);
  out = dvn_new(L, a->dtype, a->shape[0], a->shape[1], a->ndim);
  for (k = 0; k < a->nelem; k++) {
    size_t off = dvn_off(a, k);
    if (a->dtype == DVN_C128) {
      double re, im;
      dvn_getc(a, off, &re, &im);
      dvn_setc(out, k, re, im);
    }
    else if (dvn_isint(a->dtype)) dvn_seti(out, k, dvn_geti(a, off));
    else dvn_setf(out, k, dvn_getf(a, off));
    if ((k % DVN_BLOCK) == DVN_BLOCK - 1) dvn_meter_block(&m);
  }
  return 1;
}

/*
** cast(a, dtype) -- a dense array of another element type.
**
** A real array casts to c128 with a zero imaginary part, which is what
** every complex program means by it. The other direction is refused:
** dropping the imaginary part is a decision, and 'array.real' is where a
** program makes it in a word a reader can see.
*/
static int dvn_f_cast (lua_State *L) {
  dv_array *a = dvn_checkany(L, 1);
  int dtype = dvn_dtypecode(L, luaL_checkstring(L, 2));
  dv_array *out;
  dvn_meter m;
  size_t k;
  if (a->dtype == DVN_C128 && dtype != DVN_C128)
    return luaL_error(L, "cannot cast c128 to %s: use array.real, "
                         "array.imag or array.magnitude",
                      dvn_dtypename(dtype));
  dvn_meter_open(L, &m);
  out = dvn_new(L, dtype, a->shape[0], a->shape[1], a->ndim);
  for (k = 0; k < a->nelem; k++) {
    size_t off = dvn_off(a, k);
    if (dtype == DVN_C128) {
      if (a->dtype == DVN_C128) {
        double re, im;
        dvn_getc(a, off, &re, &im);
        dvn_setc(out, k, re, im);
      }
      else
        dvn_setc(out, k, dvn_getf(a, off), 0.0);
    }
    else if (dvn_isint(dtype)) dvn_seti(out, k, dvn_geti(a, off));
    else dvn_setf(out, k, dvn_getf(a, off));
    if ((k % DVN_BLOCK) == DVN_BLOCK - 1) dvn_meter_block(&m);
  }
  return 1;
}


/* ======================================================== inspection == */

static int dvn_f_dtype (lua_State *L) {
  lua_pushstring(L, dvn_dtypename(dvn_checkany(L, 1)->dtype));
  return 1;
}

static int dvn_f_shape (lua_State *L) {
  dv_array *a = dvn_checkany(L, 1);
  lua_pushinteger(L, (lua_Integer)a->shape[0]);
  if (a->ndim == 2) {
    lua_pushinteger(L, (lua_Integer)a->shape[1]);
    return 2;
  }
  return 1;
}

static int dvn_f_size (lua_State *L) {
  lua_pushinteger(L, (lua_Integer)dvn_checkany(L, 1)->nelem);
  return 1;
}

static int dvn_f_isview (lua_State *L) {
  lua_pushboolean(L, dvn_checkany(L, 1)->owns == DVN_OWN_NONE);
  return 1;
}

/*
** bits(a) -- the IEEE bit pattern of every element, as text.
**
** doc/Plan-2026-09.md 3.3: a numeric example never prints a float with
** '%g', because glibc, musl, wasmtime's host, Chromium and mingw do not
** all format decimals the same way and one 'expected.txt' has to be
** valid on all of them. A bit pattern is the same 16 characters
** everywhere or the run is wrong, which is exactly the property a
** cross-target diff needs.
**
** 16 hex digits for f64 and i64, 2 for u8 -- the element's width, so
** nothing is padded with zeros that are not in the value. Elements are
** separated by a single space, rows of a 2D array by a newline.
*/
static int dvn_f_bits (lua_State *L) {
  dv_array *a = dvn_checkany(L, 1);
  luaL_Buffer b;
  size_t i, j, rows, cols;
  char tmp[48];  /* a c128 element is two 16-digit halves and a colon */
  luaL_buffinit(L, &b);
  rows = (a->ndim == 1) ? 1 : a->shape[0];
  cols = (a->ndim == 1) ? a->shape[0] : a->shape[1];
  for (i = 0; i < rows; i++) {
    if (i > 0) luaL_addchar(&b, '\n');
    for (j = 0; j < cols; j++) {
      size_t off = (a->ndim == 1) ? dvn_off(a, j) : dvn_off2(a, i, j);
      if (j > 0) luaL_addchar(&b, ' ');
      switch (a->dtype) {
        case DVN_F64: {
          uint64_t u;
          double v = ((const double *)a->data)[off];
          memcpy(&u, &v, sizeof(u));
          l_sprintf(tmp, sizeof(tmp), "%016llx", (unsigned long long)u);
          break;
        }
        case DVN_I64: {
          lua_Integer v = ((const lua_Integer *)a->data)[off];
          l_sprintf(tmp, sizeof(tmp), "%016llx",
                    (unsigned long long)(lua_Unsigned)v);
          break;
        }
        case DVN_C128: {
          /* Both halves, real first, joined by a colon -- one element is
             still one space-separated field. */
          uint64_t ur, ui;
          const double *pv = (const double *)a->data + 2 * off;
          memcpy(&ur, pv, sizeof(ur));
          memcpy(&ui, pv + 1, sizeof(ui));
          l_sprintf(tmp, sizeof(tmp), "%016llx", (unsigned long long)ur);
          tmp[16] = ':';
          l_sprintf(tmp + 17, sizeof(tmp) - 17, "%016llx",
                    (unsigned long long)ui);
          break;
        }
        default: {
          unsigned v = ((const unsigned char *)a->data)[off];
          l_sprintf(tmp, sizeof(tmp), "%02x", v);
          break;
        }
      }
      luaL_addstring(&b, tmp);
    }
  }
  luaL_pushresult(&b);
  return 1;
}

/*
** __tostring names the array and never prints a value.
**
** Deliberately: '%g' is banned in examples (3.3) and a '__tostring' that
** printed elements would put it back through the front door, in the one
** place a program is most likely to reach for. 'array.bits' is how an
** array's contents are printed.
*/
static int dvn_mm_tostring (lua_State *L) {
  dv_array *a = dvn_checkany(L, 1);
  if (a->ndim == 1)
    lua_pushfstring(L, "array<%s>[%I]%s", dvn_dtypename(a->dtype),
                    (lua_Integer)a->shape[0],
                    a->owns == DVN_OWN_NONE ? " view" : "");
  else
    lua_pushfstring(L, "array<%s>[%I,%I]%s", dvn_dtypename(a->dtype),
                    (lua_Integer)a->shape[0], (lua_Integer)a->shape[1],
                    a->owns == DVN_OWN_NONE ? " view" : "");
  return 1;
}

/* '#a' is the first dimension: the length of a 1D array, the number of
   rows of a 2D one -- so it agrees with what 'a[i]' indexes. */
static int dvn_mm_len (lua_State *L) {
  lua_pushinteger(L, (lua_Integer)dvn_checkany(L, 1)->shape[0]);
  return 1;
}

/*
** get(a, i [, j]) -- one element.
**
** A c128 element is two numbers and is returned as two, real part
** first, rather than as a table: a pair a program can feed straight
** back to 'set' costs no allocation and no unpacking.
*/
static int dvn_pushelem (lua_State *L, const dv_array *a, size_t off) {
  if (a->dtype == DVN_C128) {
    double re, im;
    dvn_getc(a, off, &re, &im);
    lua_pushnumber(L, re);
    lua_pushnumber(L, im);
    return 2;
  }
  if (dvn_isint(a->dtype)) lua_pushinteger(L, dvn_geti(a, off));
  else lua_pushnumber(L, dvn_getf(a, off));
  return 1;
}

static int dvn_f_get (lua_State *L) {
  dv_array *a = dvn_checkany(L, 1);
  lua_Integer i = luaL_checkinteger(L, 2);
  if (a->ndim == 2 && !lua_isnoneornil(L, 3)) {
    lua_Integer j = luaL_checkinteger(L, 3);
    luaL_argcheck(L, i >= 1 && (size_t)i <= a->shape[0], 2, "out of range");
    luaL_argcheck(L, j >= 1 && (size_t)j <= a->shape[1], 3, "out of range");
    return dvn_pushelem(L, a, dvn_off2(a, (size_t)i - 1, (size_t)j - 1));
  }
  luaL_argcheck(L, a->ndim == 1, 2, "a 2D array needs two indices");
  luaL_argcheck(L, i >= 1 && (size_t)i <= a->shape[0], 2, "out of range");
  return dvn_pushelem(L, a, dvn_off(a, (size_t)i - 1));
}

static int dvn_f_set (lua_State *L) {
  dv_array *a = dvn_checkany(L, 1);
  lua_Integer i = luaL_checkinteger(L, 2);
  int vidx = (a->ndim == 2) ? 4 : 3;
  size_t off;
  if (a->ndim == 2) {
    lua_Integer j = luaL_checkinteger(L, 3);
    luaL_argcheck(L, i >= 1 && (size_t)i <= a->shape[0], 2, "out of range");
    luaL_argcheck(L, j >= 1 && (size_t)j <= a->shape[1], 3, "out of range");
    off = dvn_off2(a, (size_t)i - 1, (size_t)j - 1);
  }
  else {
    luaL_argcheck(L, i >= 1 && (size_t)i <= a->shape[0], 2, "out of range");
    off = dvn_off(a, (size_t)i - 1);
  }
  if (a->dtype == DVN_C128)  /* the imaginary part is optional and 0 */
    dvn_setc(a, off, luaL_checknumber(L, vidx),
             luaL_optnumber(L, vidx + 1, 0.0));
  else if (dvn_isint(a->dtype)) dvn_seti(a, off, luaL_checkinteger(L, vidx));
  else dvn_setf(a, off, luaL_checknumber(L, vidx));
  return 0;
}


/*
** The mock a fast-tier backend will one day replace.
**
** doc/Plan-2026-09.md A3 asks for the flag to be "wired and tested with a
** mock", and this is the mock: it does what a fast-tier kernel would do
** on entry and nothing else. Only in the debug build, which is the build
** the suite and the contract tests run and the one no artifact ships, so
** a program cannot reach it anywhere it matters.
**
** Why a hook rather than a kernel that pretends to be fast: the thing
** worth proving is the path from "a kernel decided to run fast" to
** 'dv_numeric_touched_fast' answering a host, and a real kernel would
** prove the same path with more moving parts in front of it.
*/
#if defined(LUA_DEBUG)
static int dvn_f_mark_fast (lua_State *L) {
  diluvium_numeric_touched(L);
  return 0;
}
#endif


/* ============================================================ complex == */

/*
** Everything a program can say about a c128 array, which is five
** functions and deliberately not more.
**
** The elementwise operators and the reductions do not take c128 (see
** 'dvn_check'), so this is the whole boundary: real arrays go in,
** a complex array comes out, and the transforms below are what happens
** in between. A complex arithmetic library is not stage 2 and would be a
** second implementation of every kernel above for a use nothing here
** has.
*/

/* complex(re [, im]) -- a c128 array from one or two real arrays. */
static int dvn_f_complex (lua_State *L) {
  dv_array *re = dvn_check(L, 1);
  dv_array *im = lua_isnoneornil(L, 2) ? NULL : dvn_check(L, 2);
  dv_array *out;
  dvn_meter m;
  size_t k;
  luaL_argcheck(L, re->ndim == 1, 1, "complex needs 1D arrays");
  if (im != NULL) {
    luaL_argcheck(L, im->ndim == 1, 2, "complex needs 1D arrays");
    luaL_argcheck(L, im->nelem == re->nelem, 2, "lengths differ");
  }
  dvn_meter_open(L, &m);
  out = dvn_new(L, DVN_C128, re->nelem, 0, 1);
  for (k = 0; k < re->nelem; k++) {
    dvn_setc(out, k, dvn_getf(re, dvn_off(re, k)),
             (im != NULL) ? dvn_getf(im, dvn_off(im, k)) : 0.0);
    if ((k % DVN_BLOCK) == DVN_BLOCK - 1) dvn_meter_block(&m);
  }
  return 1;
}

/* One half of a complex array as f64. 'part' is 0 for real, 1 for
   imaginary; the two entries below are the only callers. */
static int dvn_half (lua_State *L, int part) {
  dv_array *a = dvn_checkany(L, 1);
  dv_array *out;
  dvn_meter m;
  size_t k;
  luaL_argcheck(L, a->dtype == DVN_C128, 1, "a c128 array expected");
  luaL_argcheck(L, a->ndim == 1, 1, "a 1D array expected");
  dvn_meter_open(L, &m);
  out = dvn_new(L, DVN_F64, a->nelem, 0, 1);
  for (k = 0; k < a->nelem; k++) {
    double r, i;
    dvn_getc(a, dvn_off(a, k), &r, &i);
    dvn_setf(out, k, part ? i : r);
    if ((k % DVN_BLOCK) == DVN_BLOCK - 1) dvn_meter_block(&m);
  }
  return 1;
}

static int dvn_f_real (lua_State *L) { return dvn_half(L, 0); }
static int dvn_f_imag (lua_State *L) { return dvn_half(L, 1); }

/* conj(a) -- the same array with the imaginary part negated. */
static int dvn_f_conj (lua_State *L) {
  dv_array *a = dvn_checkany(L, 1);
  dv_array *out;
  dvn_meter m;
  size_t k;
  luaL_argcheck(L, a->dtype == DVN_C128, 1, "a c128 array expected");
  luaL_argcheck(L, a->ndim == 1, 1, "a 1D array expected");
  dvn_meter_open(L, &m);
  out = dvn_new(L, DVN_C128, a->nelem, 0, 1);
  for (k = 0; k < a->nelem; k++) {
    double r, i;
    dvn_getc(a, dvn_off(a, k), &r, &i);
    dvn_setc(out, k, r, -i);
    if ((k % DVN_BLOCK) == DVN_BLOCK - 1) dvn_meter_block(&m);
  }
  return 1;
}

/*
** magnitude(a) -- sqrt(re*re + im*im) as f64.
**
** Written out rather than handed to 'hypot': 'hypot' is not correctly
** rounded and is not one of the functions stage 1 vendored, so a
** platform's would put a cross-target difference back into a value the
** reproducible tier is supposed to pin. Multiplication, addition and
** 'sqrt' are each correctly rounded by IEEE 754, so this expression has
** one answer everywhere. What it costs is 'hypot''s scaling: a
** magnitude overflows here when the squares do, at about 1.3e154.
*/
static int dvn_f_magnitude (lua_State *L) {
  dv_array *a = dvn_checkany(L, 1);
  dv_array *out;
  dvn_meter m;
  size_t k;
  luaL_argcheck(L, a->dtype == DVN_C128, 1, "a c128 array expected");
  luaL_argcheck(L, a->ndim == 1, 1, "a 1D array expected");
  dvn_meter_open(L, &m);
  out = dvn_new(L, DVN_F64, a->nelem, 0, 1);
  for (k = 0; k < a->nelem; k++) {
    double r, i;
    dvn_getc(a, dvn_off(a, k), &r, &i);
    dvn_setf(out, k, sqrt(r * r + i * i));
    if ((k % DVN_BLOCK) == DVN_BLOCK - 1) dvn_meter_block(&m);
  }
  return 1;
}


/* ========================================================= transforms == */

/*
** Stage 2 of the numeric spec: FFT and NTT, both reproducible tier, and
** the convolutions built on them.
**
** The two are the same algorithm over different rings and are written
** the same way on purpose -- one bit-reversal, one iterative
** doubling loop, power-of-two sizes only -- so a reader who has followed
** one has followed both. Where they differ is where the arithmetic
** does, and there is exactly one such place: the FFT computes its
** twiddle from the index every time, and the NTT walks its root by
** repeated multiplication. That is not an inconsistency. Modular
** multiplication is exact, so a recurrence over roots of unity mod p is
** the same number however many steps preceded it; a float recurrence is
** not, and its error would depend on the transform length, which is the
** one thing a stage whose acceptance is "bit-identical across targets"
** cannot have.
**
** Sizes are powers of two and nothing here pads for you. The spec's
** bail-out (section on stage 2) says arbitrary sizes are Bluestein and
** deferred, and explicit padding is the v1 contract -- so a length that
** is not a power of two is refused with the number it would have needed,
** rather than quietly rounded up into an answer about a different
** signal.
*/

/* Is 'n' a power of two? Zero is not, which is what the callers want. */
static int dvn_ispow2 (size_t n) {
  return n != 0 && (n & (n - 1)) == 0;
}

/* The smallest power of two >= n, or 0 if that would not fit. */
static size_t dvn_ceilpow2 (size_t n) {
  size_t p = 1;
  while (p < n) {
    if (p > ((size_t)-1) / 2) return 0;
    p <<= 1;
  }
  return p;
}

static void dvn_checkpow2 (lua_State *L, size_t n) {
  if (!dvn_ispow2(n)) {
    size_t up = dvn_ceilpow2(n);
    luaL_error(L, "length %I is not a power of two (pad to %I)",
               (lua_Integer)n, (lua_Integer)up);
  }
}

/* Swap the two complex numbers at 'i' and 'j' in an interleaved buffer. */
static void dvn_cswap (double *b, size_t i, size_t j) {
  double tr = b[2 * i], ti = b[2 * i + 1];
  b[2 * i] = b[2 * j];       b[2 * i + 1] = b[2 * j + 1];
  b[2 * j] = tr;             b[2 * j + 1] = ti;
}

/*
** The decimation-in-time permutation, shared in shape by both
** transforms. Written as the standard incremental counter rather than
** by counting bits, because the loop below has to visit the pairs in
** exactly one order on every target and this one has no width to get
** wrong.
*/
static void dvn_bitrev_c (double *b, size_t n) {
  size_t i, j = 0;
  for (i = 1; i < n; i++) {
    size_t bit = n >> 1;
    for (; (j & bit) != 0; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j) dvn_cswap(b, i, j);
  }
}

/*
** exp(sign * 2*pi*i * j / len), computed from (j, len) and nothing else.
**
** 'j / len' is exact -- 'len' is a power of two and 'j' is below it, so
** the quotient is 'j' with a shifted exponent -- which leaves exactly
** one rounding, in the multiplication by tau. 'dv_cos' and 'dv_sin' are
** the vendored ones from stage 1, so they answer the same on every
** target; the platform's would not.
*/
#define DVN_TAU		6.283185307179586476925286766559

static void dvn_twiddle (int sign, size_t j, size_t len,
                         double *wr, double *wi) {
  double theta = ((double)sign * DVN_TAU) * ((double)j / (double)len);
  *wr = dv_cos(theta);
  *wi = dv_sin(theta);
}

/*
** The transform itself, in place over 'n' interleaved complex numbers.
** 'sign' is -1 forward and +1 inverse; the inverse's 1/n is the caller's,
** because 'rfft' and the convolutions want it at different moments.
*/
static void dvn_fft_core (double *b, size_t n, int sign, dvn_meter *m) {
  size_t len, i, j;
  dvn_bitrev_c(b, n);
  dvn_meter_elems(m, n);   /* the permutation is a pass like any other */
  for (len = 2; len <= n; len <<= 1) {
    size_t half = len >> 1;
    for (i = 0; i < n; i += len) {
      for (j = 0; j < half; j++) {
        double wr, wi, ur, ui, vr, vi;
        double *pu = b + 2 * (i + j);
        double *pv = b + 2 * (i + j + half);
        dvn_twiddle(sign, j, len, &wr, &wi);
        ur = pu[0]; ui = pu[1];
        vr = pv[0] * wr - pv[1] * wi;
        vi = pv[0] * wi + pv[1] * wr;
        pu[0] = ur + vr; pu[1] = ui + vi;
        pv[0] = ur - vr; pv[1] = ui - vi;
      }
    }
    /* One stage is one pass over the array; charged as such, at a stage
       boundary, so the point a budget stops a transform is the same
       everywhere. */
    dvn_meter_elems(m, n);
  }
}

/* Scale 'n' complex numbers by 1/n, which for a power of two is exact. */
static void dvn_cscale (double *b, size_t n) {
  double f = 1.0 / (double)n;
  size_t k;
  for (k = 0; k < 2 * n; k++)
    b[k] = b[k] * f;
}

/*
** A working buffer of 'n' complex numbers, zeroed, owned by a userdata
** on the stack so the collector accounts for it and an error unwinds it.
** The array's first 'a->nelem' elements are copied in, real or complex.
*/
static double *dvn_cbuf (lua_State *L, const dv_array *a, size_t n) {
  double *b;
  size_t k, have = (a == NULL) ? 0 : a->nelem;
  if (n > (((size_t)-1) - sizeof(double)) / (2 * sizeof(double)))
    luaL_error(L, "transform too large: %I elements", (lua_Integer)n);
  b = (double *)lua_newuserdatauv(L, 2 * n * sizeof(double) +
                                     sizeof(double), 0);
  memset(b, 0, 2 * n * sizeof(double));
  if (have > n) have = n;
  for (k = 0; k < have; k++) {
    if (a->dtype == DVN_C128)
      dvn_getc(a, dvn_off(a, k), &b[2 * k], &b[2 * k + 1]);
    else
      b[2 * k] = dvn_getf(a, dvn_off(a, k));
  }
  return b;
}

/* The buffer as a fresh c128 array of 'n' elements. */
static void dvn_pushc (lua_State *L, const double *b, size_t n) {
  dv_array *out = dvn_new(L, DVN_C128, n, 0, 1);
  size_t k;
  for (k = 0; k < n; k++)
    dvn_setc(out, k, b[2 * k], b[2 * k + 1]);
}

/* fft(a) / ifft(a) -- a 1D array of any dtype in, c128 of the same
   length out. The inverse divides by n; a round trip is the identity to
   within rounding, which 'test_transform.lua' measures. */
static int dvn_fft (lua_State *L, int sign) {
  dv_array *a = dvn_checkany(L, 1);
  dvn_meter m;
  double *b;
  size_t n = a->nelem;
  luaL_argcheck(L, a->ndim == 1, 1, "a 1D array expected");
  dvn_checkpow2(L, n);
  dvn_meter_open(L, &m);
  b = dvn_cbuf(L, a, n);
  dvn_fft_core(b, n, sign, &m);
  if (sign > 0) dvn_cscale(b, n);
  dvn_pushc(L, b, n);
  return 1;
}

static int dvn_f_fft (lua_State *L) { return dvn_fft(L, -1); }
static int dvn_f_ifft (lua_State *L) { return dvn_fft(L, 1); }

/*
** rfft(a) -- the half spectrum of a real signal: elements 0..n/2, which
** is n/2+1 of them, the rest being the conjugates of these.
**
** The full transform, truncated. The half-length complex trick that
** halves the work would give different last bits, and the contract this
** stage owes is a fixed answer rather than a fast one; the saving is
** recorded as available and not taken.
*/
static int dvn_f_rfft (lua_State *L) {
  dv_array *a = dvn_check(L, 1);
  dvn_meter m;
  double *b;
  size_t n = a->nelem;
  luaL_argcheck(L, a->ndim == 1, 1, "a 1D array expected");
  dvn_checkpow2(L, n);
  dvn_meter_open(L, &m);
  b = dvn_cbuf(L, a, n);
  dvn_fft_core(b, n, -1, &m);
  dvn_pushc(L, b, n / 2 + 1);
  return 1;
}

/*
** irfft(h [, n]) -- the real signal a half spectrum came from.
**
** 'n' defaults to 2*(#h - 1), which is the length 'rfft' was given. The
** other half is rebuilt from the Hermitian symmetry X[n-k] = conj(X[k]),
** so what comes back is real by construction and the imaginary parts are
** dropped rather than checked -- they are zero to within rounding, and a
** tolerance here would be a number this file had to justify.
*/
static int dvn_f_irfft (lua_State *L) {
  dv_array *h = dvn_checkany(L, 1);
  dv_array *out;
  dvn_meter m;
  double *b;
  size_t n, k;
  luaL_argcheck(L, h->dtype == DVN_C128, 1, "a c128 array expected");
  luaL_argcheck(L, h->ndim == 1, 1, "a 1D array expected");
  luaL_argcheck(L, h->nelem >= 2, 1, "a half spectrum has at least 2 bins");
  n = (size_t)luaL_optinteger(L, 2, (lua_Integer)(2 * (h->nelem - 1)));
  dvn_checkpow2(L, n);
  luaL_argcheck(L, h->nelem == n / 2 + 1, 1,
                "the half spectrum does not match that length");
  dvn_meter_open(L, &m);
  b = dvn_cbuf(L, h, n);
  for (k = n / 2 + 1; k < n; k++) {   /* X[n-k] = conj(X[k]) */
    b[2 * k]     =  b[2 * (n - k)];
    b[2 * k + 1] = -b[2 * (n - k) + 1];
  }
  dvn_fft_core(b, n, 1, &m);
  dvn_cscale(b, n);
  out = dvn_new(L, DVN_F64, n, 0, 1);
  for (k = 0; k < n; k++)
    dvn_setf(out, k, b[2 * k]);
  return 1;
}


/* ------------------------------------------------------------- the NTT -- */

/*
** The number-theoretic transform over p = 2013265921 = 15 * 2^27 + 1,
** which doc/Plan-2026-09.md section 2 names as the prime and which the
** numeric spec left open.
**
** Why this one, in the two numbers that decide it: p - 1 is divisible by
** 2^27, so a transform of up to 134 million points has a root of unity;
** and (p-1)^2 is 4.05e18, which is under 2^63, so a product of two
** residues fits a 64-bit integer and no 128-bit arithmetic is needed
** anywhere below. That second number is the whole reason a 31-bit prime
** was chosen over a 62-bit one.
**
** Everything here is exact. There is no tier question and no tolerance:
** two targets that disagree about this disagree about integer
** multiplication.
*/
#define DVN_NTT_P	2013265921u	/* 2^31 - 2^27 + 1 */
#define DVN_NTT_G	31u		/* a primitive root of it */
#define DVN_NTT_MAXLG	27		/* the 2-adic order of p - 1 */

static uint64_t dvn_powmod (uint64_t b, uint64_t e) {
  uint64_t r = 1;
  b %= DVN_NTT_P;
  while (e != 0) {
    if ((e & 1) != 0) r = r * b % DVN_NTT_P;
    b = b * b % DVN_NTT_P;
    e >>= 1;
  }
  return r;
}

static void dvn_uswap (uint32_t *a, size_t i, size_t j) {
  uint32_t t = a[i]; a[i] = a[j]; a[j] = t;
}

static void dvn_bitrev_u (uint32_t *a, size_t n) {
  size_t i, j = 0;
  for (i = 1; i < n; i++) {
    size_t bit = n >> 1;
    for (; (j & bit) != 0; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j) dvn_uswap(a, i, j);
  }
}

/*
** The transform, in place over 'n' residues. 'inverse' takes the
** conjugate root and divides by n at the end, so 'intt(ntt(x))' is 'x'
** exactly rather than nearly.
*/
static void dvn_ntt_core (uint32_t *a, size_t n, int inverse, dvn_meter *m) {
  size_t len, i, j;
  dvn_bitrev_u(a, n);
  dvn_meter_elems(m, n);
  for (len = 2; len <= n; len <<= 1) {
    uint64_t w = dvn_powmod(DVN_NTT_G, (DVN_NTT_P - 1) / len);
    size_t half = len >> 1;
    if (inverse) w = dvn_powmod(w, DVN_NTT_P - 2);  /* the inverse root */
    for (i = 0; i < n; i += len) {
      uint64_t wj = 1;
      for (j = 0; j < half; j++) {
        uint64_t u = a[i + j];
        uint64_t v = (uint64_t)a[i + j + half] * wj % DVN_NTT_P;
        a[i + j] = (uint32_t)((u + v) % DVN_NTT_P);
        a[i + j + half] = (uint32_t)((u + DVN_NTT_P - v) % DVN_NTT_P);
        wj = wj * w % DVN_NTT_P;
      }
    }
    dvn_meter_elems(m, n);
  }
  if (inverse) {
    uint64_t ninv = dvn_powmod(n % DVN_NTT_P, DVN_NTT_P - 2);
    for (i = 0; i < n; i++)
      a[i] = (uint32_t)((uint64_t)a[i] * ninv % DVN_NTT_P);
  }
}

/* A residue buffer of 'n', zero-padded, owned by a userdata on the
   stack. Every value of 'a' must already be a residue. */
static uint32_t *dvn_ubuf (lua_State *L, size_t n) {
  uint32_t *a = (uint32_t *)lua_newuserdatauv(L, (n + 1) * sizeof(uint32_t), 0);
  memset(a, 0, n * sizeof(uint32_t));
  return a;
}

/* The residue of one element, refusing anything outside [0, p). */
static uint32_t dvn_checkresidue (lua_State *L, lua_Integer v) {
  if (v < 0 || (lua_Unsigned)v >= DVN_NTT_P)
    luaL_error(L, "%I is outside 0..%I, which is what a residue mod the "
                  "transform's prime has to be",
               v, (lua_Integer)(DVN_NTT_P - 1));
  return (uint32_t)v;
}

/* ntt(a) / intt(a) -- an i64 array of residues in, another out. */
static int dvn_ntt (lua_State *L, int inverse) {
  dv_array *a = dvn_check(L, 1);
  dv_array *out;
  dvn_meter m;
  uint32_t *u;
  size_t n = a->nelem, k;
  luaL_argcheck(L, a->ndim == 1, 1, "a 1D array expected");
  luaL_argcheck(L, dvn_isint(a->dtype), 1, "an integer array expected");
  dvn_checkpow2(L, n);
  luaL_argcheck(L, n <= ((size_t)1 << DVN_NTT_MAXLG), 1,
                "longer than the prime's 2^27 root of unity allows");
  dvn_meter_open(L, &m);
  u = dvn_ubuf(L, n);
  for (k = 0; k < n; k++)
    u[k] = dvn_checkresidue(L, dvn_geti(a, dvn_off(a, k)));
  dvn_meter_elems(&m, n);   /* the read, before the transform's own passes */
  dvn_ntt_core(u, n, inverse, &m);
  out = dvn_new(L, DVN_I64, n, 0, 1);
  for (k = 0; k < n; k++)
    dvn_seti(out, k, (lua_Integer)u[k]);
  return 1;
}

static int dvn_f_ntt (lua_State *L) { return dvn_ntt(L, 0); }
static int dvn_f_intt (lua_State *L) { return dvn_ntt(L, 1); }


/* ----------------------------------------------------- the convolutions -- */

/*
** The largest magnitude a signed convolution may reach and still be
** recoverable: residues are read back as the representative nearest
** zero, so the true value has to fit in half the ring.
*/
#define DVN_NTT_HALF	((lua_Integer)(DVN_NTT_P / 2))

/* |v| as an unsigned, refusing what the bound below could not hold. */
static uint64_t dvn_absbound (lua_State *L, int arg, lua_Integer v) {
  lua_Integer a;
  /* Tested before the negation, not after: '-LUA_MININTEGER' is
     undefined and this project runs UBSan with 'halt_on_error'. */
  if (v == LUA_MININTEGER || v > DVN_NTT_HALF || v < -DVN_NTT_HALF)
    luaL_error(L, "argument #%d: %I is too large for an exact convolution "
                  "(values must be within +-%I)", arg, v, DVN_NTT_HALF);
  a = (v < 0) ? -v : v;
  return (uint64_t)a;
}

/*
** The exact convolution of two integer arrays, through the NTT.
**
** The bound is checked before any work: the answer is only the true
** convolution if it never wraps, and the largest term is
** max|x| * max|y| * min(#x, #y). Checked by division rather than by
** multiplying, so the check itself cannot overflow. A program that
** exceeds it is told the number it exceeded rather than handed a
** residue.
*/
static int dvn_conv_exact (lua_State *L, dv_array *x, dv_array *y) {
  dv_array *out;
  dvn_meter m;
  uint32_t *fx, *fy;
  uint64_t ax = 0, ay = 0, bound = (uint64_t)DVN_NTT_HALF;
  size_t nx = x->nelem, ny = y->nelem, n, len, k;
  n = nx + ny - 1;
  len = dvn_ceilpow2(n);
  luaL_argcheck(L, len != 0 && len <= ((size_t)1 << DVN_NTT_MAXLG), 1,
                "longer than the prime's 2^27 root of unity allows");
  dvn_meter_open(L, &m);
  for (k = 0; k < nx; k++) {
    uint64_t v = dvn_absbound(L, 1, dvn_geti(x, dvn_off(x, k)));
    if (v > ax) ax = v;
  }
  for (k = 0; k < ny; k++) {
    uint64_t v = dvn_absbound(L, 2, dvn_geti(y, dvn_off(y, k)));
    if (v > ay) ay = v;
  }
  dvn_meter_elems(&m, nx + ny);   /* the two bound scans */
  {
    uint64_t reach = (nx < ny) ? (uint64_t)nx : (uint64_t)ny;
    if (ax != 0 && ay != 0 && reach != 0) {
      if (ax > bound / ay || ax * ay > bound / reach)
        return luaL_error(L, "this convolution can reach %I * %I * %I, "
                             "which is past the exact range of +-%I",
                          (lua_Integer)ax, (lua_Integer)ay,
                          (lua_Integer)reach, DVN_NTT_HALF);
    }
  }
  fx = dvn_ubuf(L, len);
  fy = dvn_ubuf(L, len);
  for (k = 0; k < nx; k++) {
    lua_Integer v = dvn_geti(x, dvn_off(x, k));
    fx[k] = (uint32_t)((v < 0) ? v + (lua_Integer)DVN_NTT_P : v);
  }
  for (k = 0; k < ny; k++) {
    lua_Integer v = dvn_geti(y, dvn_off(y, k));
    fy[k] = (uint32_t)((v < 0) ? v + (lua_Integer)DVN_NTT_P : v);
  }
  dvn_ntt_core(fx, len, 0, &m);
  dvn_ntt_core(fy, len, 0, &m);
  for (k = 0; k < len; k++)
    fx[k] = (uint32_t)((uint64_t)fx[k] * fy[k] % DVN_NTT_P);
  dvn_ntt_core(fx, len, 1, &m);
  out = dvn_new(L, DVN_I64, n, 0, 1);
  for (k = 0; k < n; k++) {
    lua_Integer v = (lua_Integer)fx[k];
    /* the representative nearest zero, which is where the sign comes back */
    if (v > DVN_NTT_HALF) v -= (lua_Integer)DVN_NTT_P;
    dvn_seti(out, k, v);
  }
  return 1;
}

/* The same product through the FFT, for anything with a float in it. */
static int dvn_conv_fft (lua_State *L, dv_array *x, dv_array *y) {
  dv_array *out;
  dvn_meter m;
  double *bx, *by;
  size_t nx = x->nelem, ny = y->nelem, n = nx + ny - 1, len, k;
  len = dvn_ceilpow2(n);
  luaL_argcheck(L, len != 0, 1, "the result would be too long");
  dvn_meter_open(L, &m);
  bx = dvn_cbuf(L, x, len);
  by = dvn_cbuf(L, y, len);
  dvn_fft_core(bx, len, -1, &m);
  dvn_fft_core(by, len, -1, &m);
  for (k = 0; k < len; k++) {
    double ar = bx[2 * k], ai = bx[2 * k + 1];
    double br = by[2 * k], bi = by[2 * k + 1];
    bx[2 * k]     = ar * br - ai * bi;
    bx[2 * k + 1] = ar * bi + ai * br;
  }
  dvn_fft_core(bx, len, 1, &m);
  dvn_cscale(bx, len);
  out = dvn_new(L, DVN_F64, n, 0, 1);
  for (k = 0; k < n; k++)
    dvn_setf(out, k, bx[2 * k]);
  return 1;
}

/*
** convolve(x, y) -- the full linear convolution, #x + #y - 1 long.
**
** Exact through the NTT when both arrays hold integers, and through the
** FFT otherwise. The dtypes decide, not a flag: an integer program gets
** the answer it can check against schoolbook, and a float program gets
** the one it was going to get anyway.
*/
static int dvn_f_convolve (lua_State *L) {
  dv_array *x = dvn_check(L, 1);
  dv_array *y = dvn_check(L, 2);
  luaL_argcheck(L, x->ndim == 1, 1, "convolve needs 1D arrays");
  luaL_argcheck(L, y->ndim == 1, 2, "convolve needs 1D arrays");
  luaL_argcheck(L, x->nelem > 0, 1, "an empty array has no convolution");
  luaL_argcheck(L, y->nelem > 0, 2, "an empty array has no convolution");
  if (dvn_isint(x->dtype) && dvn_isint(y->dtype))
    return dvn_conv_exact(L, x, y);
  return dvn_conv_fft(L, x, y);
}

/*
** correlate(x, y) -- the full cross-correlation, #x + #y - 1 long.
**
** The convolution of x with y reversed, which is the definition rather
** than a trick: sum_n x[n+k] * y[n] is what a correlation is, and
** reversing one operand turns the shift the other way. Element k of the
** result is lag k - #y + 1, so the zero lag is at index #y.
*/
static int dvn_f_correlate (lua_State *L) {
  dv_array *x = dvn_check(L, 1);
  dv_array *y = dvn_check(L, 2);
  dv_array *r;
  size_t k;
  luaL_argcheck(L, x->ndim == 1, 1, "correlate needs 1D arrays");
  luaL_argcheck(L, y->ndim == 1, 2, "correlate needs 1D arrays");
  luaL_argcheck(L, y->nelem > 0, 2, "an empty array has no correlation");
  r = dvn_new(L, y->dtype, y->nelem, 0, 1);
  for (k = 0; k < y->nelem; k++) {
    size_t off = dvn_off(y, y->nelem - 1 - k);
    if (dvn_isint(y->dtype)) dvn_seti(r, k, dvn_geti(y, off));
    else dvn_setf(r, k, dvn_getf(y, off));
  }
  lua_replace(L, 2);   /* the reversed copy is the second operand now */
  return dvn_f_convolve(L);
}

/* ======================================================== the library == */

/*
** The library's functions, which are also its methods: '__index' looks a
** string key up in this table, so 'a:sum()' and 'array.sum(a)' are the
** same call. Every entry that takes an array takes it first, which is
** what makes that work.
*/
static const luaL_Reg arraylib[] = {
  /* construction */
  {"new", dvn_f_new},
  {"zeros", dvn_f_zeros},
  {"ones", dvn_f_ones},
  {"from", dvn_f_from},
  {"arange", dvn_f_arange},
  {"linspace", dvn_f_linspace},
  {"to_table", dvn_f_to_table},
  {"copy", dvn_f_copy},
  {"cast", dvn_f_cast},
  /* shape and views */
  {"dtype", dvn_f_dtype},
  {"shape", dvn_f_shape},
  {"size", dvn_f_size},
  {"isview", dvn_f_isview},
  {"slice", dvn_f_slice},
  {"row", dvn_f_row},
  {"transpose", dvn_f_transpose},
  {"get", dvn_f_get},
  {"set", dvn_f_set},
  {"bits", dvn_f_bits},
  /* elementwise, named so a mask is asked for rather than assumed */
  {"add", dvn_mm_add},
  {"sub", dvn_mm_sub},
  {"mul", dvn_mm_mul},
  {"div", dvn_mm_div},
  {"idiv", dvn_mm_idiv},
  {"mod", dvn_mm_mod},
  {"pow", dvn_mm_pow},
  {"neg", dvn_mm_unm},
  {"eq", dvn_f_eq},
  {"ne", dvn_f_ne},
  {"lt", dvn_f_lt},
  {"le", dvn_f_le},
  {"gt", dvn_f_gt},
  {"ge", dvn_f_ge},
  /* reductions */
  {"sum", dvn_f_sum},
  {"mean", dvn_f_mean},
  {"min", dvn_f_min},
  {"max", dvn_f_max},
  {"prod", dvn_f_prod},
  {"var", dvn_f_var},
  {"std", dvn_f_std},
  {"argmin", dvn_f_argmin},
  {"argmax", dvn_f_argmax},
  {"cumsum", dvn_f_cumsum},
  /* ordering and masking */
  {"sort", dvn_f_sort},
  {"argsort", dvn_f_argsort},
  {"where", dvn_f_where},
  {"select", dvn_f_select},
  /* grouping */
  {"group_index", dvn_f_group_index},
  {"group_sum", dvn_f_group_sum},
  {"group_mean", dvn_f_group_mean},
  {"group_count", dvn_f_group_count},
  {"group_min", dvn_f_group_min},
  {"group_max", dvn_f_group_max},
  /* linear algebra */
  {"dot", dvn_f_dot},
  {"matmul", dvn_f_matmul},
  /* complex, which is the whole of what a c128 array can be asked */
  {"complex", dvn_f_complex},
  {"real", dvn_f_real},
  {"imag", dvn_f_imag},
  {"conj", dvn_f_conj},
  {"magnitude", dvn_f_magnitude},
  /* transforms */
  {"fft", dvn_f_fft},
  {"ifft", dvn_f_ifft},
  {"rfft", dvn_f_rfft},
  {"irfft", dvn_f_irfft},
  {"ntt", dvn_f_ntt},
  {"intt", dvn_f_intt},
  {"convolve", dvn_f_convolve},
  {"correlate", dvn_f_correlate},
  /* '__slice' as well as 'slice': 'dv.slice' looks the metamethod up, so
     this is what makes 'a[2:5]' a view rather than a copy. The two are
     the same function -- an array's slice already takes (a, i, j) and
     already treats a missing end as "to the end". */
  {"__slice", dvn_f_slice},
#if defined(LUA_DEBUG)
  /* the fast-tier mock; see above. Absent from every shipped build. */
  {"__mark_fast", dvn_f_mark_fast},
#endif
  {NULL, NULL}
};

/*
** '__index': an integer key reads an element (or, for a 2D array, a row
** view); a string key finds a method. '__eq' is deliberately absent --
** two arrays cannot answer '==' with a boolean and Lua would silently
** say false for mixed operands anyway, which is why 'array.eq' exists
** and returns a mask.
*/
static int dvn_mm_index (lua_State *L) {
  dv_array *a = dvn_checkany(L, 1);
  if (lua_isinteger(L, 2)) {
    lua_Integer i = lua_tointeger(L, 2);
    if (i < 1 || (size_t)i > a->shape[0])
      return luaL_error(L, "index %I is outside 1..%I", i,
                        (lua_Integer)a->shape[0]);
    if (a->ndim == 2)
      return dvn_f_row(L);
    /* '__index' yields one value and a c128 element is two, so 'a[i]'
       could only hand back the real part and say nothing about the rest.
       Refused rather than truncated; 'array.get' returns both. */
    if (a->dtype == DVN_C128)
      return luaL_error(L, "a c128 element is two numbers: "
                           "use array.get(a, i)");
    lua_settop(L, 2);
    return dvn_f_get(L);
  }
  /* a method, from the library table kept in the metatable */
  luaL_getmetatable(L, DVN_MTNAME);
  lua_getfield(L, -1, "__methods");
  lua_pushvalue(L, 2);
  lua_gettable(L, -2);
  return 1;
}

static int dvn_mm_newindex (lua_State *L) {
  dv_array *a = dvn_checkany(L, 1);
  luaL_argcheck(L, lua_isinteger(L, 2), 2, "an array is indexed by integers");
  luaL_argcheck(L, a->ndim == 1, 1,
                "a 2D array is written through 'set(a, i, j, v)'");
  /* The other half of the rule above: 'a[i] = x' would set the real part
     and zero the imaginary one without being asked to. */
  if (a->dtype == DVN_C128)
    return luaL_error(L, "a c128 element is two numbers: "
                         "use array.set(a, i, re, im)");
  lua_settop(L, 3);      /* 'set' reads the value from argument 3 */
  return dvn_f_set(L);
}

static const luaL_Reg arraymeta[] = {
  /* proposals 4.4: 'a[i:j]' asks 'dv.slice', which asks this. */
  {"__slice", dvn_f_slice},
  {"__add", dvn_mm_add},
  {"__sub", dvn_mm_sub},
  {"__mul", dvn_mm_mul},
  {"__div", dvn_mm_div},
  {"__idiv", dvn_mm_idiv},
  {"__mod", dvn_mm_mod},
  {"__pow", dvn_mm_pow},
  {"__unm", dvn_mm_unm},
  {"__len", dvn_mm_len},
  {"__tostring", dvn_mm_tostring},
  {"__index", dvn_mm_index},
  {"__newindex", dvn_mm_newindex},
  {"__gc", dvn_gc},
  {NULL, NULL}
};


LUAMOD_API int luaopen_dnumeric (lua_State *L) {
  /* The dump header already pins these two, and a kernel that assumed
     them without checking would be a silent wrong answer rather than a
     refusal on the one target where they differ. */
  if (sizeof(lua_Integer) != 8 || sizeof(double) != 8)
    return luaL_error(L, "the array library needs a 64-bit lua_Integer and "
                         "a binary64 lua_Number");
  luaL_newlib(L, arraylib);
  if (luaL_newmetatable(L, DVN_MTNAME)) {
    luaL_setfuncs(L, arraymeta, 0);
    lua_pushvalue(L, -2);                 /* the library table */
    lua_setfield(L, -2, "__methods");     /* what '__index' searches */
    lua_pushliteral(L, "array");
    lua_setfield(L, -2, "__name");
    /* Not readable through 'getmetatable', so a program cannot reach in
       and replace a metamethod on every array at once. */
    lua_pushliteral(L, "array");
    lua_setfield(L, -2, "__metatable");
  }
  lua_pop(L, 1);
  return 1;
}


LUA_API int diluvium_array_adopt (lua_State *L, int dtype, size_t len,
                                  void *bytes) {
  size_t width = dvn_width(dtype);
  dv_array *a;
  /* 'c128' is a guest-side dtype and is not one of the three 'dv.h'
     documents (see dnumeric.h). Refused here rather than accepted
     quietly, so the header stays the whole truth about this call. */
  if (dtype == DVN_C128)
    return 1;
  if (width == 0 || len % width != 0 || (bytes == NULL && len != 0))
    return 1;
  a = (dv_array *)lua_newuserdatauv(L, sizeof(dv_array), 1);
  memset(a, 0, sizeof(dv_array));
  a->data = bytes;
  a->dtype = (unsigned char)dtype;
  a->ndim = 1;
  a->owns = DVN_OWN_EXTERN;   /* '__gc' frees it; the host gave it up */
  a->nelem = len / width;
  a->shape[0] = a->nelem;
  a->stride[0] = 1;
  a->contig = 1;
  /* Not 'luaL_setmetatable', which would set nil as a metatable and
     raise if the library has not been opened in this state. A host that
     adopts into a state without the library gets the documented 1
     instead. */
  luaL_getmetatable(L, DVN_MTNAME);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 2);  /* the metatable slot and the half-built array */
    return 1;
  }
  lua_setmetatable(L, -2);
  return 0;
}

#else  /* !DV_NUMERIC */

/*
** Without the feature there is no array to adopt into, so the ABI's
** documented fallback -- a Lua string copy -- is what 'dv_array_adopt'
** does, and it does it in dv.c. Reporting 1 from here is what tells it
** to.
*/
LUA_API int diluvium_array_adopt (lua_State *L, int dtype, size_t len,
                                  void *bytes) {
  (void)L; (void)dtype; (void)len; (void)bytes;
  return 1;
}

#endif  /* DV_NUMERIC */
