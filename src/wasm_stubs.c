#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "drepl.h"

int system(const char *c) { return -1; }
FILE *tmpfile(void) { return NULL; }
char *tmpnam(char *s) { return NULL; }

lua_State *global_L = NULL;

#ifdef __wasm__
#define WASM_EXPORT(name) __attribute__((export_name(name)))
#else
#define WASM_EXPORT(name)
#endif

WASM_EXPORT("init_lua") void init_lua()
{
    if (global_L == NULL)
    {

        // Ensure Lua doesn't buffer internally
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);

        global_L = luaL_newstate();
        luaL_openlibs(global_L);
    }
}

WASM_EXPORT("run_lua") int run_lua(const char *code)
{
    if (global_L == NULL)
        init_lua();

    int res = luaL_dostring(global_L, code);

    if (res != LUA_OK)
    {
        const char *err = lua_tostring(global_L, -1);
        printf("Error: %s\n", err);
        lua_pop(global_L, 1);
    }

    fflush(stdout);
    fflush(stderr);

    return res;
}


/* ---------------------------------------------------------------------
** Diluvium: REPL entry points, so a browser terminal behaves like the
** native one instead of guessing.
**
** 'run_lua' is enough to run a script, but not to be a REPL: it cannot
** say whether input is merely unfinished, and it does not echo the value
** of a bare expression. Without those, a front end has to decide when to
** show a continuation prompt by matching the text of Lua's error
** messages, and '1 + 1' prints nothing.
**
** These wrap drepl.c over the same persistent state 'run_lua' uses, so
** the browser shares one implementation with the interpreter.
** ------------------------------------------------------------------ */

/* 0 ok (and evaluated), 1 unfinished, 2 error (printed) */
WASM_EXPORT("repl_eval") int repl_eval(const char *code)
{
    int st;
    if (global_L == NULL)
        init_lua();
    st = diluvium_repl_load(global_L, code, strlen(code), "=stdin");
    if (st == DILUVIUM_REPL_INCOMPLETE)
        return 1;
    if (st == DILUVIUM_REPL_ERROR) {
        printf("%s\n", lua_tostring(global_L, -1));
        lua_pop(global_L, 1);
        fflush(stdout);
        return 2;
    }
    if (lua_pcall(global_L, 0, LUA_MULTRET, 0) != LUA_OK) {
        printf("%s\n", lua_tostring(global_L, -1));
        lua_pop(global_L, 1);
        fflush(stdout);
        return 2;
    }
    /* print whatever the chunk returned, as the REPL does */
    {
        int n = lua_gettop(global_L);
        int i;
        for (i = 1; i <= n; i++) {
            size_t l;
            const char *s = luaL_tolstring(global_L, i, &l);
            if (i > 1) printf("\t");
            printf("%s", s);
            lua_pop(global_L, 1);
        }
        if (n > 0) printf("\n");
        lua_settop(global_L, 0);
    }
    fflush(stdout);
    fflush(stderr);
    return 0;
}


/*
** Completion candidates for 'cursor' in 'buf', as one NUL-terminated
** string: the byte offset to replace from, then a tab-separated list.
** A single allocation keeps the JS side to one read and one free.
** The caller frees it; 'free' is already exported.
*/
WASM_EXPORT("repl_complete") char *repl_complete(const char *buf, int cursor)
{
    size_t off, need = 24;
    int i, n;
    char *out, *p;
    if (global_L == NULL)
        init_lua();
    off = diluvium_repl_complete(global_L, buf, (size_t)cursor);
    n = (int)lua_rawlen(global_L, -1);
    for (i = 1; i <= n; i++) {
        size_t l;
        lua_rawgeti(global_L, -1, i);
        lua_tolstring(global_L, -1, &l);
        need += l + 1;
        lua_pop(global_L, 1);
    }
    out = (char *)malloc(need);
    if (out == NULL) { lua_pop(global_L, 1); return NULL; }
    p = out + sprintf(out, "%d", (int)off);
    for (i = 1; i <= n; i++) {
        size_t l;
        const char *s;
        lua_rawgeti(global_L, -1, i);
        s = lua_tolstring(global_L, -1, &l);
        *p++ = '\t';
        memcpy(p, s, l);
        p += l;
        lua_pop(global_L, 1);
    }
    *p = '\0';
    lua_pop(global_L, 1);  /* the candidate table */
    return out;
}


/* Drop the interpreter state; the next call builds a fresh one. */
WASM_EXPORT("reset_lua") void reset_lua(void)
{
    if (global_L != NULL) {
        lua_close(global_L);
        global_L = NULL;
    }
    init_lua();
}
