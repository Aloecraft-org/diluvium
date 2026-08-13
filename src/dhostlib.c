/*
** dhostlib.c
** The 'host' guest library. See dhostlib.h for what it is and why.
**
** Implementation: an embedded Lua chunk. Everything the library does is
** orchestrate 'queue.*' -- declare the pair lazily, push a request, wait,
** correlate by token -- and Lua says that in a page. Keeping the body Lua is
** also the point of the design: when the language grows a real way to await
** a hostcall, this chunk's internals change and no call site does. The C
** below only compiles the chunk and hands its module table to
** 'luaL_requiref' (dlibs.c), which sets the 'host' global.
**
** State and snapshots. The chunk's state -- queue handles, the token
** counter, the stash of out-of-order replies -- lives in upvalues reachable
** only through the 'host' table, and the table is a *named permanent*
** (dsnap.c DS_MODULES): a snapshot stores the name, and a restore resolves
** it to the restored runtime's own fresh copy, state and all reset. That is
** correct because none of the state is program state: handles are runtime
** identity re-bound by name on first use, and a token only needs to be
** unique among calls outstanding *now*. A thread parked inside a call rides
** the snapshot as a content copy of these closures, exactly as the
** hand-rolled ask() it replaces did.
*/

#define dhostlib_c

#include "lprefix.h"

#include "lua.h"

#include "lauxlib.h"

#include "dhostlib.h"


static const char HOSTLIB_SRC[] =
  "local TIMEOUT_MS = 10000\n"
  "\n"
  "local calls, replies      -- queue handles, bound lazily and by name\n"
  "local pending = {}        -- tok -> reply that arrived for another call\n"
  "local tok = 0\n"
  "\n"
  "-- Look up before declaring: after a snapshot restore the queues exist\n"
  "-- (queue state rides in the root) while these upvalues start over, and a\n"
  "-- program mixing raw pushes with this library may have declared the pair\n"
  "-- itself. Hostcall.md's conventions: calls exported and rejecting when\n"
  "-- full, replies sized for the calls outstanding.\n"
  "local function ensure()\n"
  "  if not calls then\n"
  "    calls = queue.lookup(\"host/calls\") or queue.declare(\"host/calls\",\n"
  "              { capacity = 16, exported = true, on_full = \"reject\" })\n"
  "  end\n"
  "  if not replies then\n"
  "    replies = queue.lookup(\"host/replies\") or\n"
  "              queue.declare(\"host/replies\", { capacity = 16 })\n"
  "  end\n"
  "end\n"
  "\n"
  "-- One request, one reply. Replies arrive in whatever order the host\n"
  "-- answers, so a reply that is not ours is stashed for the call it does\n"
  "-- answer; a reply with no token answers nothing and is dropped. Error\n"
  "-- levels: every public function is one closure above ask/try, so level 4\n"
  "-- here and level 3 there both name the program's own call site.\n"
  "local function roundtrip(call, args)\n"
  "  ensure()\n"
  "  tok = tok + 1\n"
  "  local t = tok\n"
  "  local sent, why = queue.push(calls, { tok = t, call = call, args = args })\n"
  "  if not sent then\n"
  "    error((\"host: could not send '%s': the call queue is %s\")\n"
  "          :format(call, tostring(why)), 4)\n"
  "  end\n"
  "  if pending[t] ~= nil then\n"
  "    local m = pending[t]\n"
  "    pending[t] = nil\n"
  "    return m\n"
  "  end\n"
  "  while true do\n"
  "    local _, m, st = queue.wait({ replies }, TIMEOUT_MS)\n"
  "    if st ~= \"ok\" then\n"
  "      error((\"host: no reply to '%s' within %d ms (%s)\")\n"
  "            :format(call, TIMEOUT_MS, st), 4)\n"
  "    end\n"
  "    if m.tok == t then\n"
  "      return m\n"
  "    elseif m.tok ~= nil then\n"
  "      pending[m.tok] = m\n"
  "    end\n"
  "  end\n"
  "end\n"
  "\n"
  "-- The readable default: a non-ok status raises, with the connector's own\n"
  "-- sentence in the message. An unknown status is treated as an error, as\n"
  "-- Hostcall.md instructs.\n"
  "local function ask(call, args)\n"
  "  local m = roundtrip(call, args)\n"
  "  if m.status ~= \"ok\" then\n"
  "    error((\"host.%s: %s: %s\"):format((call:gsub(\"/\", \".\")),\n"
  "          tostring(m.status), tostring(m.detail)), 3)\n"
  "  end\n"
  "  return m.value\n"
  "end\n"
  "\n"
  "-- The pcall-friendly escape, for a program that expects a denial.\n"
  "local function try(call, args)\n"
  "  local m = roundtrip(call, args)\n"
  "  if m.status == \"ok\" then return m.value, \"ok\" end\n"
  "  return nil, tostring(m.status), m.detail\n"
  "end\n"
  "\n"
  "-- Parameters ride as a msgpack array, which is dense by definition: a nil\n"
  "-- hole cannot cross the wire, and SQL already has a spelling for NULL.\n"
  "local function params_of(what, ...)\n"
  "  local n = select(\"#\", ...)\n"
  "  if n == 0 then return nil end\n"
  "  local p = { ... }\n"
  "  for i = 1, n do\n"
  "    if p[i] == nil then\n"
  "      error((\"host.sql.%s: parameter %d is nil; SQL NULL is spelled NULL \"\n"
  "             .. \"in the statement text\"):format(what, i), 3)\n"
  "    end\n"
  "  end\n"
  "  return p\n"
  "end\n"
  "\n"
  "local sql = {}\n"
  "function sql.exec(text, ...)\n"
  "  return ask(\"sql/exec\", { sql = text, params = params_of(\"exec\", ...) })\n"
  "end\n"
  "function sql.query(text, ...)\n"
  "  return ask(\"sql/query\", { sql = text, params = params_of(\"query\", ...) })\n"
  "end\n"
  "function sql.try_exec(text, ...)\n"
  "  return try(\"sql/exec\",\n"
  "             { sql = text, params = params_of(\"try_exec\", ...) })\n"
  "end\n"
  "function sql.try_query(text, ...)\n"
  "  return try(\"sql/query\",\n"
  "             { sql = text, params = params_of(\"try_query\", ...) })\n"
  "end\n"
  "\n"
  "local crypto = {}\n"
  "function crypto.hash(data)\n"
  "  return ask(\"crypto/hash\", { data = data })\n"
  "end\n"
  "function crypto.hmac(data)\n"
  "  return ask(\"crypto/hmac\", { data = data })\n"
  "end\n"
  "function crypto.random(nbytes)\n"
  "  return ask(\"crypto/random\", { bytes = nbytes })\n"
  "end\n"
  "function crypto.jwt_sign(claims, ttl)\n"
  "  return ask(\"crypto/jwt_sign\", { claims = claims, ttl = ttl })\n"
  "end\n"
  "-- An invalid token is an answer ({valid=false, reason}), not an error:\n"
  "-- the connector says so with status \"ok\", and this library agrees.\n"
  "function crypto.jwt_verify(token)\n"
  "  return ask(\"crypto/jwt_verify\", { token = token })\n"
  "end\n"
  "\n"
  "return {\n"
  "  sql = sql,\n"
  "  crypto = crypto,\n"
  "  time = function() return ask(\"time\") end,\n"
  "  call = function(name, args) return ask(name, args) end,\n"
  "  try = function(name, args) return try(name, args) end,\n"
  "}\n"
;


LUAMOD_API int luaopen_dhostlib (lua_State *L) {
  if (luaL_loadbufferx(L, HOSTLIB_SRC, sizeof(HOSTLIB_SRC) - 1,
                       "=host", "t") != LUA_OK)
    return lua_error(L);
  lua_call(L, 0, 1);  /* the chunk returns the module table */
  return 1;
}
