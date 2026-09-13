/*
** contraction_check.c
** The contraction canary for the numeric build (doc/Plan-2026-09.md 3.5).
**
** Two build systems compile the same C in this project -- the Makefile and
** diluvium-sys's build.rs -- and the numeric feature's floating-point flags
** have to reach both, on every target, or one target's results disagree with
** another's in the last digits for no visible reason. Nothing about a missing
** flag is loud: the build succeeds, the tests pass, and the divergence surfaces
** later as an `expected.txt` that is right on x86-64 and wrong on aarch64.
**
** So this asserts the flag's *effect* rather than its presence. There is no
** predefined macro for -ffp-contract, and a build system that lost the flag
** would still define DV_NUMERIC; only the arithmetic can tell.
**
** Run it built the same way the numeric artifacts are built (`make NUMERIC=1
** contraction_check`). Built without those flags it is expected to fail on any
** target whose compiler contracts by default -- GCC on aarch64 does -- and that
** failure is the check working.
**
** ------------------------------------------------------------------ surface --
**
** Entry points:
**   main                  runs every case, prints failures, exits non-zero on any
**
** Configurable values:
**   TWO_POW_M27           the perturbation that makes fused and unfused differ
**   WANT_UNFUSED_BITS     the IEEE bit pattern an uncontracted a*b+c must give
**   WANT_FUSED_BITS       what a contracted one gives, reported to name the cause
**
** Cases (each a function called from main):
**   case_multiply_add     the canary itself: a*b+c
**   case_negated_multiply_add   the same for c-a*b, which lowers to fnmsub
**   case_bits_roundtrip   the punning this and every numeric example rely on
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int failures = 0;
static int checks = 0;

/*
** 2^-27, spelled as a literal rather than reached through ldexp: this file must
** compile and run with no libm, since a wrong libm is one of the things the
** numeric round exists to remove.
*/
#define TWO_POW_M27 7.450580596923828125e-09

/*
** a = 1 + 2^-27, b = 1 - 2^-27, c = -1.
**
** a*b is exactly 1 - 2^-54, which is not representable and rounds to 1.0. So
** an uncontracted a*b+c is 1.0 + -1.0, exactly zero. A contracted one keeps
** the full product and returns -2^-54, whose bits are below. One rounding
** apart, and it is the whole difference between two targets agreeing and not.
*/
#define WANT_UNFUSED_BITS UINT64_C(0x0000000000000000)
#define WANT_FUSED_BITS   UINT64_C(0xbc90000000000000)

/* depth: the type pun every numeric example prints through (3.3). memcpy
   rather than a union or a cast, which is the spelling that is defined for
   every compiler in the matrix and that none of them refuses to optimise. */
static uint64_t bits (double x) {
  uint64_t u;
  memcpy(&u, &x, sizeof(u));
  return u;
}

static void expect_bits (const char *what, uint64_t got, uint64_t want) {
  checks++;
  if (got != want) {
    printf("[FAIL] %s\n  got  %016llx\n  want %016llx\n", what,
           (unsigned long long)got, (unsigned long long)want);
    /* Sign-insensitive: the negated form fuses to +2^-54 where the plain one
       fuses to -2^-54, and both mean the same missing flag. */
    if ((got & UINT64_C(0x7fffffffffffffff)) ==
        (WANT_FUSED_BITS & UINT64_C(0x7fffffffffffffff))) {
      printf("  this is the *fused* result, so the compiler contracted a*b+c\n"
             "  into an fma. -ffp-contract=off did not reach this translation\n"
             "  unit. See doc/Plan-2026-09.md 3.5 and the Makefile's\n"
             "  NUMERIC_FPFLAGS.\n");
    }
    failures++;
  }
}

/*
** volatile, and it is load-bearing. Without it the compiler evaluates the whole
** expression at compile time, where it folds correctly whatever -ffp-contract
** says, and the canary passes in a build that would contract every runtime
** multiply-add it ever emitted.
*/
static void case_multiply_add (void) {
  volatile double a = 1.0 + TWO_POW_M27;
  volatile double b = 1.0 - TWO_POW_M27;
  volatile double c = -1.0;
  double r = a * b + c;
  expect_bits("a*b + c is not contracted", bits(r), WANT_UNFUSED_BITS);
}

/*
** The same product reached through the negated form, because a compiler that
** has been told not to fuse a*b+c can still lower c-a*b to fnmsub -- a separate
** instruction on aarch64, and a separate chance to get one flag's coverage
** wrong. Uncontracted this is -1.0 + 1.0, again exactly zero.
*/
static void case_negated_multiply_add (void) {
  volatile double a = 1.0 + TWO_POW_M27;
  volatile double b = 1.0 - TWO_POW_M27;
  volatile double c = 1.0;
  double r = c - a * b;
  expect_bits("c - a*b is not contracted", bits(r), WANT_UNFUSED_BITS);
}

/*
** The bit pun itself, on values whose patterns are fixed by IEEE 754. If this
** fails the machine is not one this project's `expected.txt` files describe,
** and every numeric example is meaningless before any kernel has run.
*/
static void case_bits_roundtrip (void) {
  expect_bits("1.0", bits(1.0), UINT64_C(0x3ff0000000000000));
  expect_bits("-2^-54", bits(-5.5511151231257827e-17), WANT_FUSED_BITS);
  expect_bits("0.0", bits(0.0), UINT64_C(0x0000000000000000));
}

int main (void) {
  case_bits_roundtrip();
  case_multiply_add();
  case_negated_multiply_add();
  if (failures == 0) {
    printf("contraction_check: %d checks, no contraction\n", checks);
    return 0;
  }
  printf("contraction_check: %d of %d checks failed\n", failures, checks);
  return 1;
}
