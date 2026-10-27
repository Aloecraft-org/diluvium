/*
** dsqlite.c
** SQLite, vendored (src/sqlite, public domain), for the standalone
** binary's sql connector (dlocalhost.c).
**
** Its own translation unit, never #included into onelua.c: SQLite's
** amalgamation and Lua's share internal names (Table, Token, rehash), so
** the two cannot meet in one file. Every recipe that links the diluvium
** binary compiles this beside onelua.c, as src/makefile's LUA_O does.
**
** Configured for one thread and no extensions, so the binary needs
** neither -lpthread nor -ldl. Empty on WASI, which has no file locking for
** SQLite to stand on; dlocalhost.c answers sql there with a refusal.
*/

#if !defined(__wasi__)

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE  /* pread, fdatasync, mremap under -std=c99 */
#endif

#define SQLITE_THREADSAFE 0
#define SQLITE_OMIT_LOAD_EXTENSION 1
#define SQLITE_DEFAULT_MEMSTATUS 0
#define SQLITE_DQS 0

#include "sqlite/sqlite3.c"

#else

typedef int dsqlite_empty_on_wasi;  /* ISO C wants a declaration per file */

#endif
