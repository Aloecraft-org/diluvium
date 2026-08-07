/*
** drepl.c
** Diluvium REPL support. See drepl.h.
*/

#define drepl_c

#include "lprefix.h"

#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "drepl.h"


/*
** Lua marks a syntax error that ran out of input by ending the message
** with "<eof>". That is the only signal distinguishing "keep typing" from
** "this is wrong", and matching it here means front ends never have to.
*/
#define EOFMARK		"<eof>"
#define marklen		(sizeof(EOFMARK) / sizeof(char) - 1)


static int endedearly (lua_State *L, int status) {
  if (status == LUA_ERRSYNTAX) {
    size_t lmsg;
    const char *msg = lua_tolstring(L, -1, &lmsg);
    if (lmsg >= marklen && strcmp(msg + lmsg - marklen, EOFMARK) == 0)
      return 1;
  }
  return 0;
}


LUA_API int diluvium_repl_load (lua_State *L, const char *code, size_t len,
                                const char *chunkname) {
  int status;
  /* First as an expression, so a REPL shows the value of '1 + 1' rather
     than complaining that it is not a statement. */
  {
    luaL_Buffer b;
    const char *wrapped;
    size_t wlen;
    luaL_buffinit(L, &b);
    luaL_addstring(&b, "return ");
    luaL_addlstring(&b, code, len);
    luaL_addstring(&b, ";");
    luaL_pushresult(&b);
    wrapped = lua_tolstring(L, -1, &wlen);
    status = luaL_loadbufferx(L, wrapped, wlen, chunkname, "t");
    if (status == LUA_OK) {
      lua_remove(L, -2);  /* drop the wrapped source, keep the chunk */
      return DILUVIUM_REPL_OK;
    }
    lua_pop(L, 2);  /* the error and the wrapped source */
  }
  /* Not an expression, so take it as written. */
  status = luaL_loadbufferx(L, code, len, chunkname, "t");
  if (status == LUA_OK)
    return DILUVIUM_REPL_OK;
  if (endedearly(L, status)) {
    lua_pop(L, 1);  /* the caller wants more input, not a message */
    return DILUVIUM_REPL_INCOMPLETE;
  }
  return DILUVIUM_REPL_ERROR;  /* the message stays on the stack */
}


/*
** Words that can begin a statement but are not values, so they would
** never be found by walking a table. Diluvium's contextual keywords are
** here too: they are not reserved, but at the start of a statement they
** are what the user means.
*/
static const char *const keywords[] = {
  "and", "break", "defer", "do", "else", "elseif", "end", "false", "for",
  "function", "global", "goto", "if", "in", "local", "nil", "not", "or",
  "repeat", "return", "switch", "then", "true", "until", "while", NULL
};


static int ispart (char c) {  /* part of an identifier? */
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}


/* Append 'name' to the result table at 'res' when it matches. */
static void offer (lua_State *L, int res, const char *name,
                   const char *part, size_t partlen, int *n) {
  if (strncmp(name, part, partlen) == 0) {
    lua_pushstring(L, name);
    lua_rawseti(L, res, ++(*n));
  }
}


/*
** Every matching string key of the table at 'tab', appended to 'res'.
** Both are absolute indices. Keys are only ever read, never converted:
** 'lua_next' would lose its place if a number key were turned into a
** string underneath it, so the type is checked before anything else.
*/
static void offerkeys (lua_State *L, int tab, int res, const char *part,
                       size_t partlen, int *n) {
  lua_pushnil(L);
  while (lua_next(L, tab) != 0) {
    lua_pop(L, 1);  /* the value; only keys are candidates */
    if (lua_type(L, -1) == LUA_TSTRING &&
        strncmp(lua_tostring(L, -1), part, partlen) == 0) {
      lua_pushvalue(L, -1);  /* leave the key for 'lua_next' */
      lua_rawseti(L, res, ++(*n));
    }
  }
}


/*
** Follow 'a.b.c' from the globals table, raw all the way down, and leave
** the table it names on the stack. Returns 0, pushing nothing, when the
** path does not resolve to a table.
*/
static int resolve (lua_State *L, const char *path, size_t len) {
  size_t i = 0;
  lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
  while (i < len) {
    size_t start = i;
    while (i < len && path[i] != '.' && path[i] != ':') i++;
    lua_pushlstring(L, path + start, i - start);
    lua_rawget(L, -2);
    lua_remove(L, -2);  /* the table we came from */
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      return 0;
    }
    if (i < len) i++;  /* step over the separator */
  }
  return 1;
}


/* Insertion sort, so candidates come back in a stable, useful order. */
static void sortcandidates (lua_State *L, int res, int n) {
  int i, j;
  for (i = 2; i <= n; i++) {
    lua_rawgeti(L, res, i);  /* the one being placed */
    for (j = i - 1; j > 0; j--) {
      lua_rawgeti(L, res, j);
      if (strcmp(lua_tostring(L, -1), lua_tostring(L, -2)) <= 0) {
        lua_pop(L, 1);
        break;
      }
      lua_rawseti(L, res, j + 1);  /* shift the larger one up */
    }
    lua_rawseti(L, res, j + 1);
  }
}


LUA_API size_t diluvium_repl_complete (lua_State *L, const char *buf,
                                       size_t cursor) {
  size_t start = cursor;
  size_t sep;      /* just past the last '.' or ':', when there is one */
  size_t i;
  const char *part;
  size_t partlen;
  int res, n = 0;
  /* back over the identifier chain the cursor sits at the end of */
  while (start > 0 && (ispart(buf[start - 1]) ||
                       buf[start - 1] == '.' || buf[start - 1] == ':'))
    start--;
  sep = start;
  for (i = start; i < cursor; i++)
    if (buf[i] == '.' || buf[i] == ':') sep = i + 1;
  part = buf + sep;
  partlen = cursor - sep;
  lua_newtable(L);
  res = lua_absindex(L, -1);
  if (sep == start) {  /* a bare word: the globals, plus the keywords */
    const char *const *kw;
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    offerkeys(L, lua_absindex(L, -1), res, part, partlen, &n);
    lua_pop(L, 1);
    for (kw = keywords; *kw != NULL; kw++)
      offer(L, res, *kw, part, partlen, &n);
  }
  else if (resolve(L, buf + start, sep - 1 - start)) {
    int tab = lua_absindex(L, -1);
    offerkeys(L, tab, res, part, partlen, &n);
    /* and a metatable's __index table, so string methods and object
       methods complete; still raw, so nothing is invoked */
    if (lua_getmetatable(L, tab)) {
      lua_pushliteral(L, "__index");
      lua_rawget(L, -2);
      if (lua_istable(L, -1))
        offerkeys(L, lua_absindex(L, -1), res, part, partlen, &n);
      lua_pop(L, 2);  /* __index and the metatable */
    }
    lua_pop(L, 1);  /* the resolved table */
  }
  sortcandidates(L, res, n);
  return sep;  /* the front end replaces buf[sep..cursor) */
}
