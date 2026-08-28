/*
** wasm_compat.c
** Definitions the Diluvium core references that rustc's bundled wasi-libc is
** too old to carry, for wasm targets only.
**
** Rust ships its own copy of wasi-libc for wasm32-wasip1/p2, older than the
** wasi-sdk that compiles the core here. Three stdio/stdlib stubs it lacks
** (`tmpfile`, `tmpnam`, `system`) surface as `env::` imports that no host
** defines, so a module carrying them instantiates nowhere -- found by running
** the spike guest (bindings/rust/WASM-SPIKE.md, wrinkle 3).
**
** Every definition here is **weak**: a libc that has its own wins at link
** time and nothing collides. That is what makes this file safe to archive
** unconditionally rather than guess which libc a consumer will bring.
**
** The semantics are C's own for a machine with no such facility, not
** pretences: there are no temporary files and no command processor on wasm,
** so the calls fail the way the standard says they fail when the facility is
** unavailable. `os.tmpname`, `io.tmpfile` and `os.execute` are absent from a
** sealed instance anyway; these exist so the *link* succeeds.
*/

#include <stddef.h>

/* No temporary files: `tmpfile` reports failure with a null FILE*. */
__attribute__((weak)) void *tmpfile(void) {
  return NULL;
}

/* Likewise `tmpnam`: no name can be produced. */
__attribute__((weak)) char *tmpnam(char *s) {
  (void)s;
  return NULL;
}

/*
** `system(NULL)` answering 0 is ISO C for "there is no command processor",
** which on wasm is the truth rather than a stub; any actual command fails.
*/
__attribute__((weak)) int system(const char *command) {
  return command == NULL ? 0 : -1;
}

/*
** wasm32-wasip2 only: the preview1 `libwasi-emulated-process-clocks.a` cannot
** cross componentization (its `__wasi_clock_time_get` resolves to nothing in a
** p2 world), so it is not linked there and `clock` would be undefined. wasi-libc
** defines CLOCKS_PER_SEC as 1000000000, so clock_t counts nanoseconds.
**
** Weak, so the emulated archive still wins wherever it *is* linked (p1).
*/
#if defined(__wasm32__)

struct compat_timespec {
  long long tv_sec;
  long tv_nsec;
};
extern int clock_gettime(int clock_id, struct compat_timespec *tp);
#define COMPAT_CLOCK_MONOTONIC 1

__attribute__((weak)) long long clock(void) {
  struct compat_timespec ts;
  if (clock_gettime(COMPAT_CLOCK_MONOTONIC, &ts) != 0)
    return -1;
  return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

#endif
