#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

int system(const char *c) { return -1; }
FILE *tmpfile(void) { return NULL; }
char *tmpnam(char *s) { return NULL; }

lua_State *global_L = NULL;

__attribute__((export_name("init_lua"))) void init_lua()
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

__attribute__((export_name("run_lua"))) int run_lua(const char *code)
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