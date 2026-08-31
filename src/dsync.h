/*
** dsync.h
** The one lock this runtime has, and why it exists.
**
** dv.h's contract is "one instance, one thread", and every other file in this
** tree is entitled to believe it. This header is for the two places that are
** not covered by it, because they are not per-instance at all: the named
** continuation registries in dshim.c and dsnap.c are *process*-global arrays,
** and 'diluvium_openlibs' appends to them on every 'dv_new'. A host obeying
** the instance contract to the letter -- one thread per instance, never two
** threads on the same one -- still has two threads inside 'diluvium_openlibs'
** at once the first time it creates two instances concurrently, and the
** registries had no synchronisation of any kind.
**
** The observed failure was a SIGSEGV in 'strcmp' inside the registry scan: two
** threads read the same 'ncont', both wrote the same slot, both incremented,
** and the count then named a slot whose 'name' was still NULL. There is a
** second route to the same crash that needs no lost slot -- nothing ordered
** the '.name' store before the count increment, so a scanner could see the
** larger count and the older name. Both are the same defect and both are
** closed by making the whole scan-then-append sequence atomic.
**
** Scope, deliberately: this is for process-global state only. It is not a step
** towards locking instances, and nothing here should be reached on a path that
** runs more than once per registration. The reproduction, and why nothing
** in the suite had found this, is in test/dshim_race_check.c.
**
** The primitive is a plain mutex rather than a once-guard, because the
** registries have a *public* writer -- 'diluvium_shim_addcont' and
** 'diluvium_snap_addcont' are in dshim.h and dsnap.h for libraries and hosts
** to call -- so "registration happens once, at startup" is a convention and
** not something the core can enforce. A once-guard would fix 'dv_new' and
** leave the exported writers racy. The mutex fixes both, and costs a handful
** of uncontended acquisitions per instance created.
**
** C99 is the standard this tree builds against (see the -std=c99 in Makefile's
** PLAT_CFLAGS for Linux, macOS and Windows alike), so '_Atomic', <stdatomic.h>
** and C11 <threads.h> are all out of reach without a standard bump. That
** leaves the platform threading libraries, hence the three arms below.
*/

#ifndef dsync_h
#define dsync_h


/*
** Which arm. 'DILUVIUM_NO_THREADS' forces the no-op one, for a build that
** knows it is single-threaded and does not want the dependency.
**
** wasm is checked first and on purpose: 'src/wasm_stubs.c' calls
** 'diluvium_openlibs', both wasm targets are single-threaded, and wasi-libc
** carries pthread declarations whose implementations are only present in a
** threads build -- so probing for POSIX there gets a link error rather than a
** lock.
*/
#if defined(DILUVIUM_NO_THREADS) || defined(__wasm__) || defined(__wasi__) || \
    defined(__EMSCRIPTEN__)
#define DSYNC_NONE

#elif defined(_WIN32)
#define DSYNC_WINDOWS

#elif defined(__unix__) || defined(__linux__) || defined(__APPLE__) || \
      defined(__CYGWIN__) || defined(_POSIX_VERSION)
#define DSYNC_PTHREAD

#else
/*
** An unrecognised target. A no-op lock is the only thing that can be offered
** without guessing at a threading library, and on a genuinely single-threaded
** target it is also the right answer. A target that does have threads and
** lands here will reintroduce the crash this header exists to fix, so add an
** arm rather than leaving it.
*/
#define DSYNC_NONE
#endif


#if defined(DSYNC_PTHREAD)

#include <pthread.h>

typedef pthread_mutex_t dsync_lock;
#define DSYNC_LOCK_INIT		PTHREAD_MUTEX_INITIALIZER

/*
** The return values are ignored, and that is a decision rather than an
** oversight: for a statically-initialised, non-recursive, non-robust mutex
** that is never destroyed, the only documented failures are programming errors
** (EINVAL on a mutex that was never initialised, EDEADLK on a self-deadlock)
** which cannot arise here, and there is no useful recovery from a failed lock
** on a registration path that has no way to report one.
*/
static void dsync_lock_acquire (dsync_lock *m) { (void)pthread_mutex_lock(m); }
static void dsync_lock_release (dsync_lock *m) { (void)pthread_mutex_unlock(m); }

/*
** On linking, which was the open question when this was written and is worth
** writing down rather than re-deriving: nothing in this tree passes -lpthread,
** and nothing needs to. Checked, not assumed -- 'make build_host' compiles the
** amalgamation with no LUA_USE_* macro and no threading flag, and the binary
** it produces resolves 'pthread_mutex_lock@GLIBC_2.2.5' out of libc.so.6 with
** libpthread nowhere in its 'ldd'. glibc >= 2.34 moved the implementations
** into libc; before that libc carried working stubs and the real ones came
** with libpthread, which any program that actually starts a thread is linking
** anyway. musl and Apple's libSystem have never had a separate library. The
** test targets pass -pthread because a *test* creates the threads itself.
*/

#elif defined(DSYNC_WINDOWS)

/*
** SRWLOCK rather than CRITICAL_SECTION, because it has a static initialiser
** and so needs no run-time setup call -- which is the whole difficulty being
** avoided, since a lazy initialisation would itself be a race.
**
** Note for the amalgamation: on a Windows build loadlib.c pulls in <windows.h>
** before this header is reached (see src/onelua.c's include order), so the
** _WIN32_WINNT below is a no-op there and the toolchain's own default decides.
** Every current MinGW-w64 and Windows SDK defaults well past the 0x0600 that
** SRWLOCK needs; a toolchain that does not will fail to compile here, loudly,
** rather than silently building something unsafe.
*/
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

typedef SRWLOCK dsync_lock;
#define DSYNC_LOCK_INIT		SRWLOCK_INIT

/* Exclusive on both: every critical section here writes, or reads something a
   writer may be changing, so a shared mode would buy nothing. */
static void dsync_lock_acquire (dsync_lock *m) { AcquireSRWLockExclusive(m); }
static void dsync_lock_release (dsync_lock *m) { ReleaseSRWLockExclusive(m); }

#else	/* DSYNC_NONE */

/*
** No threads on this target, so the lock is a compile-time nothing. The 'char'
** is only so the declarations below have a type to name and an initialiser to
** take; it is never read.
*/
typedef char dsync_lock;
#define DSYNC_LOCK_INIT		0

static void dsync_lock_acquire (dsync_lock *m) { (void)m; }
static void dsync_lock_release (dsync_lock *m) { (void)m; }

#endif


#endif
