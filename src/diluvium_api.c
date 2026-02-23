#include "lua.h"
#include "lauxlib.h"   /* luaL_newstate, luaL_loadbuffer */
#include "lstate.h"    /* lua_State internals */
#include "lfunc.h"     /* LClosure */
#include "lobject.h"   /* Proto */
#include "analyze.h"

/* In a new diluvium_api.c, compiled without MAKE_LUAC */
char *diluvium_generate_report(const char *lua_source, size_t source_len, const char *chunkname) {
  lua_State *L = luaL_newstate();
  if (!L) return NULL;
  if (luaL_loadbuffer(L, lua_source, source_len, chunkname) != LUA_OK) {
    lua_close(L);
    return NULL;
  }
  /* The loaded chunk sits on the stack as a closure; get its Proto */
  LClosure *cl = (LClosure *)lua_topointer(L, -1);
  InterfaceReport *report = analyze_proto(cl->p);
  char *json = report_to_json_string(report);
  free_report(report);
  lua_close(L);
  return json; /* caller free()s */
}