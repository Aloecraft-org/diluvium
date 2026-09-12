/*
** wasm_browser.c
** A browser embedder for Diluvium: a small wasm surface a page drives from
** JavaScript. It is the "run a bit of dlua logic, compile a chunk, look at
** what it compiled to" artifact, not the WASI command module and not the
** swarm.
**
** ------------------------------------------------------------------ surface
** Exports (all marshalling is bytes-in / heap-pointer-out; the caller frees
** every returned pointer with 'free', which is exported):
**
**   dl_version() -> char*        the _DILUVIUM version string (static; do not free)
**   dl_eval(src, len) -> int     run dlua, REPL semantics; 0 ok, 1 error.
**                                output and any error go to stdout/stderr,
**                                which the host captures through fd_write.
**   dl_reset() -> void           drop the eval state; the next dl_eval is fresh
**   dl_compile(src, len, out_len*) -> char*   source -> bytecode; NULL on a
**                                compile error, else a buffer of *out_len bytes
**   dl_analyze(src, len) -> char*  the JSON analysis report for source, or NULL
**   malloc / free                for the host to pass bytes in and free results
**
** No allocator lives here: malloc/realloc/free come from wasi-libc (a real
** dlmalloc that grows linear memory), which is the point -- the old browser
** build hand-rolled a bump allocator with a fixed ceiling.
**
** Configurable values: none. The eval state is sealed guest libraries plus
** the base library; a page that wants os/io must say so, and this artifact
** deliberately does not.
** ------------------------------------------------------------------
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "dlibs.h"
#include "drepl.h"
#include "analyze.h"

#define EXPORT(name) __attribute__((export_name(name)))

/*
** Three C library functions wasi-libc declares but cannot implement on this
** target (they need a filesystem and a process). Defined here so they are not
** left as host imports; a program that reaches them gets the honest failure.
*/
int system(const char *c) { (void)c; return -1; }
FILE *tmpfile(void) { return NULL; }
char *tmpnam(char *s) { (void)s; return NULL; }


/* depth: the persistent eval state, built on first use and dropped by dl_reset. */
static lua_State *G = NULL;

static lua_State *state(void) {
  if (G == NULL) {
    G = luaL_newstate();
    if (G == NULL) return NULL;
    /* Line buffering off, so print() reaches the host at the call rather than
       at exit -- there is no exit here. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    luaL_openlibs(G);      /* the base library, string, table, math, ... */
    diluvium_openlibs(G);  /* queue, json, regex, bytes, time, ... and _DILUVIUM */
  }
  return G;
}


EXPORT("dl_version") const char *dl_version(void) {
  lua_State *L = state();
  const char *v = "unknown";
  if (L == NULL) return v;
  lua_getglobal(L, "_DILUVIUM");
  if (lua_type(L, -1) == LUA_TTABLE) {
    lua_getfield(L, -1, "version");
    if (lua_type(L, -1) == LUA_TSTRING) v = lua_tostring(L, -1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  return v;  /* points into an interned string in L; stable while L lives */
}


EXPORT("dl_reset") void dl_reset(void) {
  if (G != NULL) { lua_close(G); G = NULL; }
}


/*
** Run one piece of source with REPL semantics: a bare expression prints its
** value, an unfinished chunk is reported as an error rather than hanging (a
** page has no continuation prompt to offer here), and output crosses stdout.
*/
EXPORT("dl_eval") int dl_eval(const char *src, size_t len) {
  lua_State *L = state();
  int st, n, i;
  if (L == NULL) return 1;
  st = diluvium_repl_load(L, src, len, "=web");
  if (st == DILUVIUM_REPL_INCOMPLETE) {
    fprintf(stderr, "unfinished input\n");
    return 1;
  }
  if (st == DILUVIUM_REPL_ERROR) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    return 1;
  }
  if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    return 1;
  }
  n = lua_gettop(L);
  for (i = 1; i <= n; i++) {
    size_t l;
    const char *s = luaL_tolstring(L, i, &l);
    fputs(i > 1 ? "\t" : "", stdout);
    fwrite(s, 1, l, stdout);
    lua_pop(L, 1);
  }
  if (n > 0) fputc('\n', stdout);
  lua_settop(L, 0);
  return 0;
}


/* depth: a lua_dump writer that appends to a growable heap buffer. */
typedef struct { char *p; size_t len, cap; } dumpbuf;

static int dump_writer(lua_State *L, const void *b, size_t sz, void *ud) {
  dumpbuf *d = (dumpbuf *)ud;
  (void)L;
  if (d->len + sz > d->cap) {
    size_t nc = (d->cap == 0) ? 256 : d->cap;
    char *np;
    while (nc < d->len + sz) nc *= 2;
    np = (char *)realloc(d->p, nc);
    if (np == NULL) return 1;  /* out of memory: stop the dump */
    d->p = np;
    d->cap = nc;
  }
  memcpy(d->p + d->len, b, sz);
  d->len += sz;
  return 0;
}

/*
** Compile source to a bytecode chunk. On success returns a heap buffer and
** writes its length through 'out_len'; on any compile error returns NULL. A
** fresh state, so compiling never disturbs the eval session.
*/
EXPORT("dl_compile") char *dl_compile(const char *src, size_t len, size_t *out_len) {
  lua_State *L;
  dumpbuf d = { NULL, 0, 0 };
  if (out_len != NULL) *out_len = 0;
  L = luaL_newstate();
  if (L == NULL) return NULL;
  if (luaL_loadbuffer(L, src, len, "=compile") != LUA_OK) {
    lua_close(L);
    return NULL;
  }
  if (lua_dump(L, dump_writer, &d, 0) != 0) {
    free(d.p);
    lua_close(L);
    return NULL;
  }
  lua_close(L);
  if (out_len != NULL) *out_len = d.len;
  return d.p;  /* caller frees */
}


/*
** The JSON analysis report for source: what it compiled to, function by
** function. A thin wrapper over the analyzer's own entry point, which builds
** and tears down its own state.
*/
EXPORT("dl_analyze") char *dl_analyze(const char *src, size_t len) {
  return diluvium_generate_report(src, len, "=analyze");  /* caller frees */
}
