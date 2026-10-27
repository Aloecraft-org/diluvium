/*
** dlocalhost.c
** The standalone binary's own host: 'host.*' answered in place.
**
** Plain diluvium runs a program with Lua's rights -- 'io', 'os', the whole
** machine of the user who started it -- yet it opens the same 'host'
** library a sandboxed instance reaches DRT through, and until this file
** nothing answered it: a call waited out its timeout on a queue nobody
** drains. This is the answer. The library's transport is swapped for a
** direct call (dhostlib.c's local answerer), and each call is served here
** from Lua's own standard libraries plus a few primitives standard Lua
** lacks. One program then runs in both places: here with Lua's rights,
** under DRT with the capabilities its deployment grants. The calls are
** synchronous, so they need no '--task' and work anywhere Lua can call a
** function, a 'table.sort' comparator included.
**
** It is the binary's, never the library's: lua.c installs it, so an
** embedding host -- DRT, the wasm swarm, the deprecated C host -- never has
** it, and a sealed instance keeps the queue transport and its capability
** checks. '--queue-host' keeps the queue transport here too, for a program
** (or a test) that plays the host itself.
**
** Surface:
**   diluvium_localhost_install(L)   replace the 'host' global with one
**                                   answered here; lua.c calls it after
**                                   the libraries are open
**   LOCALHOST_SRC, top              the configurable values (the key's env
**                                   var, ttls, bounds) and ROUTES, the
**                                   dispatch table of every call answered
**   lh_prims                        the primitives standard Lua lacks:
**                                   wall_ms, mono_ms, random, listdir,
**                                   exec, getenv, sql; plus 'lacks', the
**                                   families this platform's build cannot
**                                   answer
**
** What is not answered, and says so instead of timing out:
** 'crypto/turn_credential' (no TURN secret), named crypto keys (no
** config), the swarm half of the library (one program, no swarm),
** and on WASI, which has neither processes nor file locking, 'exec/run'
** and 'sql'. 'sql' is answered by SQLite, vendored in src/sqlite.
*/

#define dlocalhost_c

#include "lprefix.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lua.h"

#include "lauxlib.h"

#include "dhostlib.h"
#include "dmsgpack.h"
#include "dlocalhost.h"


static int lh_wall_ms (lua_State *L);
static int lh_mono_ms (lua_State *L);
static int lh_random (lua_State *L);
static int lh_listdir (lua_State *L);
static int lh_exec (lua_State *L);
static int lh_getenv (lua_State *L);
static int lh_sql (lua_State *L);

static const luaL_Reg lh_prims[] = {
  {"wall_ms", lh_wall_ms},
  {"mono_ms", lh_mono_ms},
  {"random", lh_random},
  {"listdir", lh_listdir},
  {"exec", lh_exec},
  {"getenv", lh_getenv},
  {"sql", lh_sql},
  {NULL, NULL}
};


static const char LOCALHOST_SRC[] =
  "local prim = ...\n"
  "\n"
  "-- ==== surface ====\n"
  "-- Configurable. The env var is the one place a key enters: plain diluvium\n"
  "-- has no deployment config, so the variable plays DRT's key_env.\n"
  "local KEY_ENV = \"DILUVIUM_CRYPTO_KEY\"\n"
  "local KEY_MIN, KEY_MAX = 16, 512\n"
  "local KDF_LABEL_HMAC = \"diluvium/crypto/hmac/v1\"\n"
  "local KDF_LABEL_JWT = \"diluvium/crypto/jwt-hs256/v1\"\n"
  "local JWT_HEADER_B64 = \"eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9\"\n"
  "local JWT_DEFAULT_TTL, JWT_TTL_MAX = 3600, 315360000\n"
  "local JWT_MAX_TOKEN = 8192\n"
  "local RANDOM_DEFAULT, RANDOM_MAX = 32, 1024\n"
  "local EXEC_DEFAULT_MS = 600000\n"
  "\n"
  "-- Fan-out: every call this host answers, and the handler below that\n"
  "-- answers it. capabilities/list is built from this table, so the menu\n"
  "-- cannot drift from what a call would do.\n"
  "local ROUTES = {\n"
  "  [\"time\"]               = \"time\",\n"
  "  [\"time/monotonic\"]     = \"monotonic\",\n"
  "  [\"fs/read\"]            = \"fs_read\",\n"
  "  [\"fs/write\"]           = \"fs_write\",\n"
  "  [\"fs/list\"]            = \"fs_list\",\n"
  "  [\"fs/remove\"]          = \"fs_remove\",\n"
  "  [\"exec/run\"]           = \"exec_run\",\n"
  "  [\"crypto/hash\"]        = \"crypto_hash\",\n"
  "  [\"crypto/random\"]      = \"crypto_random\",\n"
  "  [\"crypto/hmac\"]        = \"crypto_hmac\",\n"
  "  [\"crypto/jwt_sign\"]    = \"jwt_sign\",\n"
  "  [\"crypto/jwt_verify\"]  = \"jwt_verify\",\n"
  "  [\"sql/query\"]          = \"sql_query\",\n"
  "  [\"sql/exec\"]           = \"sql_exec\",\n"
  "  [\"capabilities/list\"]  = \"capabilities\",\n"
  "}\n"
  "\n"
  "-- Calls a host answers elsewhere and this one does not, with the sentence\n"
  "-- that says why. Looked up by exact name, then by family.\n"
  "local NOT_HERE = {\n"
  "  [\"crypto/turn_credential\"] = \"TURN credentials need a host-held TURN \"\n"
  "    .. \"secret, which plain diluvium does not hold; run under drt\",\n"
  "}\n"
  "\n"
  "-- ==== depth: the handlers ====\n"
  "local H = {}\n"
  "\n"
  "-- A handler returns the value, or raises a string: the answerer below\n"
  "-- turns that into status \"error\" with the string as the detail. A raised\n"
  "-- {status, detail} carries a status of its own.\n"
  "local function fail(fmt, ...) error(fmt:format(...), 0) end\n"
  "\n"
  "local function field(args, name, kind, call)\n"
  "  local v = type(args) == \"table\" and args[name] or nil\n"
  "  if kind and type(v) ~= kind then\n"
  "    fail(\"%s: args.%s must be a %s\", call, name, kind)\n"
  "  end\n"
  "  return v\n"
  "end\n"
  "\n"
  "function H.time() return prim.wall_ms() end\n"
  "function H.monotonic() return prim.mono_ms() end\n"
  "\n"
  "function H.fs_read(args)\n"
  "  local path = field(args, \"path\", \"string\", \"fs/read\")\n"
  "  local f, why = io.open(path, \"rb\")\n"
  "  if not f then fail(\"%s\", why) end\n"
  "  local data = f:read(\"a\")\n"
  "  f:close()\n"
  "  return data\n"
  "end\n"
  "\n"
  "function H.fs_write(args)\n"
  "  local path = field(args, \"path\", \"string\", \"fs/write\")\n"
  "  local data = field(args, \"data\", \"string\", \"fs/write\")\n"
  "  local f, why = io.open(path, args.append and \"ab\" or \"wb\")\n"
  "  if not f then fail(\"%s\", why) end\n"
  "  local ok, werr = f:write(data)\n"
  "  f:close()\n"
  "  if not ok then fail(\"'%s': %s\", path, tostring(werr)) end\n"
  "  return { bytes = #data }\n"
  "end\n"
  "\n"
  "function H.fs_list(args)\n"
  "  local path = (type(args) == \"table\" and args.path) or \".\"\n"
  "  if type(path) ~= \"string\" then fail(\"fs/list: args.path must be a string\") end\n"
  "  local names, why = prim.listdir(path)\n"
  "  if not names then fail(\"'%s': %s\", path, why) end\n"
  "  return names\n"
  "end\n"
  "\n"
  "function H.fs_remove(args)\n"
  "  local path = field(args, \"path\", \"string\", \"fs/remove\")\n"
  "  local ok, why = os.remove(path)\n"
  "  if not ok then fail(\"%s\", why) end\n"
  "  return nil\n"
  "end\n"
  "\n"
  "function H.exec_run(args)\n"
  "  local argv = field(args, \"argv\", \"table\", \"exec/run\")\n"
  "  if #argv == 0 then\n"
  "    fail(\"exec/run: args.argv must be a non-empty array of strings \"\n"
  "         .. \"(a vector, not a shell string)\")\n"
  "  end\n"
  "  for i = 1, #argv do\n"
  "    if type(argv[i]) ~= \"string\" then\n"
  "      fail(\"exec/run: argv[%d] is not a plain string\", i)\n"
  "    end\n"
  "  end\n"
  "  local stdin = args.stdin\n"
  "  if stdin ~= nil and type(stdin) ~= \"string\" then\n"
  "    fail(\"exec/run: args.stdin must be a string\")\n"
  "  end\n"
  "  local timeout = args.timeout_ms\n"
  "  if timeout ~= nil and (math.type(timeout) ~= \"integer\" or timeout <= 0) then\n"
  "    fail(\"exec/run: timeout_ms must be a positive integer\")\n"
  "  end\n"
  "  local cwd = args.cwd\n"
  "  if cwd ~= nil and type(cwd) ~= \"string\" then\n"
  "    fail(\"exec/run: args.cwd must be a string\")\n"
  "  end\n"
  "  local status, out, err = prim.exec(argv, stdin, timeout or EXEC_DEFAULT_MS,\n"
  "                                     cwd)\n"
  "  if status == nil then fail(\"%s\", out) end\n"
  "  return { status = status, stdout = out, stderr = err }\n"
  "end\n"
  "\n"
  "function H.crypto_hash(args)\n"
  "  local data = field(args, \"data\", \"string\", \"crypto/hash\")\n"
  "  local alg = args.alg or \"sha256\"\n"
  "  if alg == \"sha256\" then return bytes.tohex(bytes.sha256(data)) end\n"
  "  if alg == \"sha1\" then return bytes.tohex(bytes.sha1(data)) end\n"
  "  fail(\"crypto/hash: unknown alg '%s'; this host hashes with 'sha256' or \"\n"
  "       .. \"'sha1'\", tostring(alg))\n"
  "end\n"
  "\n"
  "function H.crypto_random(args)\n"
  "  local n = (type(args) == \"table\" and args.bytes) or RANDOM_DEFAULT\n"
  "  if math.type(n) ~= \"integer\" or n < 1 or n > RANDOM_MAX then\n"
  "    fail(\"crypto/random: bytes must be 1..%d\", RANDOM_MAX)\n"
  "  end\n"
  "  local raw, why = prim.random(n)\n"
  "  if not raw then fail(\"crypto/random: %s\", why) end\n"
  "  return bytes.tohex(raw)\n"
  "end\n"
  "\n"
  "-- The master key, read when a call first needs it, so a program that never\n"
  "-- signs never needs the variable. Subkeys are derived exactly as DRT and\n"
  "-- the C host derive them: a token signed here verifies there under the\n"
  "-- same key, and crypto/hmac is no oracle for the JWT key.\n"
  "local keys\n"
  "local function subkeys(call)\n"
  "  if keys then return keys end\n"
  "  local master = prim.getenv(KEY_ENV)\n"
  "  if master == nil then\n"
  "    fail(\"%s: no signing key; set %s to a secret of at least %d bytes\",\n"
  "         call, KEY_ENV, KEY_MIN)\n"
  "  end\n"
  "  master = master:sub(1, KEY_MAX)\n"
  "  if #master < KEY_MIN then\n"
  "    fail(\"%s: %s is shorter than %d bytes\", call, KEY_ENV, KEY_MIN)\n"
  "  end\n"
  "  keys = { hmac = bytes.hmac_sha256(master, KDF_LABEL_HMAC),\n"
  "           jwt = bytes.hmac_sha256(master, KDF_LABEL_JWT) }\n"
  "  return keys\n"
  "end\n"
  "\n"
  "-- Named keys (crypto.secrets and crypto/derive labels) are deployment\n"
  "-- config and per-node state; plain diluvium has neither.\n"
  "local function no_named_key(call, args)\n"
  "  if type(args) == \"table\" and args.key ~= nil then\n"
  "    fail(\"%s: args.key names a configured secret or a derived label, and \"\n"
  "         .. \"plain diluvium has neither; only the %s key signs here\",\n"
  "         call, KEY_ENV)\n"
  "  end\n"
  "end\n"
  "\n"
  "function H.crypto_hmac(args)\n"
  "  local data = field(args, \"data\", \"string\", \"crypto/hmac\")\n"
  "  no_named_key(\"crypto/hmac\", args)\n"
  "  local mac = bytes.hmac_sha256(subkeys(\"crypto/hmac\").hmac, data)\n"
  "  if args.expect ~= nil then\n"
  "    local ok, want = pcall(bytes.fromhex, args.expect)\n"
  "    if type(args.expect) ~= \"string\" or not ok or #want ~= #mac then\n"
  "      fail(\"crypto/hmac: args.expect must be %d hex digits\", #mac * 2)\n"
  "    end\n"
  "    return { valid = bytes.consteq(mac, want) }\n"
  "  end\n"
  "  return bytes.tohex(mac)\n"
  "end\n"
  "\n"
  "local function now_secs() return prim.wall_ms() // 1000 end\n"
  "\n"
  "function H.jwt_sign(args)\n"
  "  no_named_key(\"crypto/jwt_sign\", args)\n"
  "  local k = subkeys(\"crypto/jwt_sign\").jwt\n"
  "  local ttl = type(args) == \"table\" and args.ttl or nil\n"
  "  if math.type(ttl) ~= \"integer\" or ttl <= 0 or ttl > JWT_TTL_MAX then\n"
  "    ttl = JWT_DEFAULT_TTL\n"
  "  end\n"
  "  -- The host owns iat, exp and nbf: whatever the program set is dropped.\n"
  "  local payload = {}\n"
  "  local claims = type(args) == \"table\" and args.claims or nil\n"
  "  if claims ~= nil then\n"
  "    if type(claims) ~= \"table\" then\n"
  "      fail(\"crypto/jwt_sign: args.claims must be a map\")\n"
  "    end\n"
  "    for name, v in pairs(claims) do\n"
  "      if type(name) ~= \"string\" then\n"
  "        fail(\"crypto/jwt_sign: claim keys must be strings\")\n"
  "      end\n"
  "      if name ~= \"iat\" and name ~= \"exp\" and name ~= \"nbf\" then\n"
  "        payload[name] = v\n"
  "      end\n"
  "    end\n"
  "  end\n"
  "  local iat = now_secs()\n"
  "  payload.iat, payload.exp = iat, iat + ttl\n"
  "  local ok, text = pcall(json.encode, payload)\n"
  "  if not ok then fail(\"crypto/jwt_sign: a claim has no JSON form\") end\n"
  "  local input = JWT_HEADER_B64 .. \".\" .. bytes.tobase64url(text)\n"
  "  local token = input .. \".\" .. bytes.tobase64url(bytes.hmac_sha256(k, input))\n"
  "  if #token > JWT_MAX_TOKEN then\n"
  "    fail(\"crypto/jwt_sign: the token is too large\")\n"
  "  end\n"
  "  return token\n"
  "end\n"
  "\n"
  "-- An invalid token is an answer, not an error. The header is compared\n"
  "-- whole against the one this host writes and never read for 'alg', and\n"
  "-- the MAC is checked before the payload is parsed.\n"
  "function H.jwt_verify(args)\n"
  "  local token = field(args, \"token\", \"string\", \"crypto/jwt_verify\")\n"
  "  local function invalid(reason) return { valid = false, reason = reason } end\n"
  "  if #token > JWT_MAX_TOKEN then return invalid(\"oversized\") end\n"
  "  local header, body, sig = token:match(\"^([^.]*)%.([^.]*)%.([^.]*)$\")\n"
  "  if not header then return invalid(\"malformed\") end\n"
  "  if header ~= JWT_HEADER_B64 then\n"
  "    local okh, h = pcall(function()\n"
  "      return json.decode(bytes.frombase64url(header))\n"
  "    end)\n"
  "    if okh and type(h) == \"table\" and type(h.kid) == \"string\" then\n"
  "      return invalid(\"kid\")\n"
  "    end\n"
  "    return invalid(\"alg\")\n"
  "  end\n"
  "  local mac = bytes.hmac_sha256(subkeys(\"crypto/jwt_verify\").jwt,\n"
  "                                header .. \".\" .. body)\n"
  "  if not bytes.consteq(sig, bytes.tobase64url(mac)) then\n"
  "    return invalid(\"signature\")\n"
  "  end\n"
  "  local okp, claims = pcall(function()\n"
  "    return json.decode(bytes.frombase64url(body))\n"
  "  end)\n"
  "  if not okp or type(claims) ~= \"table\" then return invalid(\"payload\") end\n"
  "  local now = now_secs()\n"
  "  if math.type(claims.exp) ~= \"integer\" or now >= claims.exp then\n"
  "    return invalid(\"expired\")\n"
  "  end\n"
  "  if math.type(claims.nbf) == \"integer\" and now < claims.nbf then\n"
  "    return invalid(\"not_yet_valid\")\n"
  "  end\n"
  "  return { valid = true, claims = claims }\n"
  "end\n"
  "\n"
  "local function sql(is_exec, call, args)\n"
  "  local db = field(args, \"db\", \"string\", call)\n"
  "  local text = field(args, \"sql\", \"string\", call)\n"
  "  local value, status, why = prim.sql(is_exec, db, text, args.params)\n"
  "  if value == nil then error({ status = status, detail = why }, 0) end\n"
  "  return value\n"
  "end\n"
  "function H.sql_query(args) return sql(false, \"sql/query\", args) end\n"
  "function H.sql_exec(args) return sql(true, \"sql/exec\", args) end\n"
  "\n"
  "-- One entry per family this host answers, in the shape DRT and the C host\n"
  "-- report. Everything answered here is granted: plain diluvium runs with\n"
  "-- the rights of the user who started it, as a Lua script does. A family\n"
  "-- this platform's build lacks (prim.lacks) is not listed at all.\n"
  "function H.capabilities()\n"
  "  local families, out = {}, {}\n"
  "  for call in pairs(ROUTES) do\n"
  "    local family = call:match(\"^[^/]+\")\n"
  "    families[family] = not prim.lacks[family]\n"
  "  end\n"
  "  local names = {}\n"
  "  for name, built in pairs(families) do\n"
  "    if built then names[#names + 1] = name end\n"
  "  end\n"
  "  table.sort(names)\n"
  "  for _, name in ipairs(names) do\n"
  "    out[#out + 1] = { name = name, kind = \"builtin\", granted = true,\n"
  "                      visibility = \"public\", within_ceiling = true,\n"
  "                      held = { { capability = \"host:\" .. name,\n"
  "                                 effect = \"grant\" } } }\n"
  "  end\n"
  "  return out\n"
  "end\n"
  "\n"
  "-- ==== the answerer ====\n"
  "-- Called by the host library in place of a queue round trip. Answers in\n"
  "-- the reply shape of doc/Hostcall.md, so the library's ask/try read it\n"
  "-- exactly as they read a reply from DRT.\n"
  "return function(call, args)\n"
  "  local name = ROUTES[call]\n"
  "  if name == nil then\n"
  "    local why = NOT_HERE[call] or NOT_HERE[call:match(\"^[^/]+\")]\n"
  "    return { status = \"denied\",\n"
  "             detail = why or (\"plain diluvium does not answer '\" .. call\n"
  "                              .. \"'; a host such as drt may\") }\n"
  "  end\n"
  "  local ok, value = pcall(H[name], args)\n"
  "  if not ok and type(value) == \"table\" then\n"
  "    return { status = value.status, detail = value.detail }\n"
  "  elseif not ok then\n"
  "    return { status = \"error\", detail = tostring(value) }\n"
  "  end\n"
  "  return { status = \"ok\", value = value }\n"
  "end\n"
;


/* ======================================================================
** depth: the primitives
** ====================================================================== */

#if defined(_WIN32)
#include <windows.h>
#define LH_HAVE_EXEC 1
/* msvcrt's CSPRNG (RtlGenRandom underneath). Declared here rather than by
   defining _CRT_RAND_S, which would have to precede every stdlib.h in the
   amalgamation. */
_CRTIMP int __cdecl rand_s (unsigned int *r);
#endif

#if !defined(_MSC_VER)
#include <dirent.h>
#endif

#if defined(LUA_USE_POSIX)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#define LH_HAVE_EXEC 1
#endif

#if defined(__wasi__)
#include <sys/random.h>  /* getentropy */
#endif

/* SQLite, vendored and compiled on its own (dsqlite.c); everywhere but
   WASI, which dsqlite.c leaves empty. */
#if !defined(__wasi__)
#include "sqlite/sqlite3.h"
#define LH_HAVE_SQL 1
#endif


/* Milliseconds since the Unix epoch, and since an arbitrary fixed point. */
static long long lh_clock (int monotonic) {
#if defined(_WIN32)
  if (monotonic)
    return (long long)GetTickCount64();
  else {
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);  /* 100 ns ticks since 1601 */
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)(u.QuadPart / 10000ULL) - 11644473600000LL;
  }
#elif defined(CLOCK_MONOTONIC)
  struct timespec ts;
  clock_gettime(monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#else
  /* No clock_gettime: whole seconds for the wall clock, processor time
     for the interval clock. Both still move forward. */
  return monotonic ? (long long)(clock() / (CLOCKS_PER_SEC / 1000))
                   : (long long)time(NULL) * 1000;
#endif
}

/* 'time/monotonic' counts from when the host was installed, as DRT's
   counts from when its connector was made: a small number that resets
   with the process. */
static long long lh_mono_base = 0;

static int lh_wall_ms (lua_State *L) {
  lua_pushinteger(L, (lua_Integer)lh_clock(0));
  return 1;
}

static int lh_mono_ms (lua_State *L) {
  lua_pushinteger(L, (lua_Integer)(lh_clock(1) - lh_mono_base));
  return 1;
}


/* random(n) -> n bytes from the platform's CSPRNG, or nil, why. */
static int lh_random (lua_State *L) {
  lua_Integer n = luaL_checkinteger(L, 1);
  luaL_Buffer b;
  char *p;
  luaL_argcheck(L, n > 0 && n <= 65536, 1, "out of range");
  p = luaL_buffinitsize(L, &b, (size_t)n);
#if defined(_WIN32)
  {
    lua_Integer i;
    for (i = 0; i < n; i += 4) {
      unsigned int r;
      lua_Integer k;
      if (rand_s(&r) != 0) {
        luaL_pushfail(L);
        lua_pushliteral(L, "the system's random source failed");
        return 2;
      }
      for (k = 0; k < 4 && i + k < n; k++)
        p[i + k] = (char)((r >> (8 * k)) & 0xff);
    }
  }
#elif defined(__wasi__)
  {
    lua_Integer off = 0;
    while (off < n) {  /* getentropy takes at most 256 bytes a call */
      size_t chunk = (size_t)((n - off) > 256 ? 256 : (n - off));
      if (getentropy(p + off, chunk) != 0) {
        luaL_pushfail(L);
        lua_pushstring(L, strerror(errno));
        return 2;
      }
      off += (lua_Integer)chunk;
    }
  }
#else
  {
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got = 0;
    if (f != NULL) {
      got = fread(p, 1, (size_t)n, f);
      fclose(f);
    }
    if (got != (size_t)n) {
      luaL_pushfail(L);
      lua_pushliteral(L, "could not read /dev/urandom");
      return 2;
    }
  }
#endif
  luaL_pushresultsize(&b, (size_t)n);
  return 1;
}


/* listdir(path) -> sorted names, without '.' and '..', or nil, why. Sorted
   so the answer does not depend on the filesystem's own order. */
static int lh_listdir (lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
#if defined(_MSC_VER)
  (void)path;
  luaL_pushfail(L);
  lua_pushliteral(L, "listing a directory is not built for this compiler");
  return 2;
#else
  DIR *d = opendir(path);
  struct dirent *e;
  lua_Integer n = 0;
  if (d == NULL) {
    luaL_pushfail(L);
    lua_pushstring(L, strerror(errno));
    return 2;
  }
  lua_newtable(L);
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    lua_pushstring(L, e->d_name);
    lua_rawseti(L, -2, ++n);
  }
  closedir(d);
  /* table.sort, so the order is Lua's string order on every platform */
  if (lua_getglobal(L, "table") == LUA_TTABLE &&
      lua_getfield(L, -1, "sort") == LUA_TFUNCTION) {
    lua_pushvalue(L, -3);
    lua_call(L, 1, 0);
    lua_pop(L, 1);
  }
  else
    lua_pop(L, 2);
  return 1;
#endif
}


/* getenv(name) -> the value, or nil. Honours '-E', which tells lua.c's
   libraries to ignore the environment: a key is environment too. */
static int lh_getenv (lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  const char *v;
  lua_getfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");
  if (lua_toboolean(L, -1)) {
    luaL_pushfail(L);
    return 1;
  }
  v = getenv(name);
  if (v == NULL)
    luaL_pushfail(L);
  else
    lua_pushstring(L, v);
  return 1;
}


/*
** exec(argv, stdin|nil, timeout_ms, cwd|nil) -> status, stdout, stderr
**                                            or nil, why
**
** The C host's exec/run, less what only a deployment needs: no shell (argv
** is a vector, so no quoting can escape), the child and everything it
** starts held together -- a process group on unix, a Job Object on
** Windows, as DRT's connector does -- so the deadline kills the whole tree,
** and every exit of the call sweeps it, so nothing it starts outlives it.
** A nonzero exit is the child's answer; 127 is a program that could not be
** run, the shell's convention, on both. There is no output cap: the
** program has Lua's rights, and io.popen has none either.
*/
#if defined(LH_HAVE_EXEC)

typedef struct lh_out {
  char *p;
  size_t len, cap;
} lh_out;

static int lh_out_add (lh_out *o, const char *p, size_t n) {
  if (o->len + n > o->cap) {
    size_t ncap = (o->cap == 0) ? 4096 : o->cap;
    char *np;
    while (ncap < o->len + n) ncap *= 2;
    np = (char *)realloc(o->p, ncap);
    if (np == NULL) return -2;
    o->p = np;
    o->cap = ncap;
  }
  memcpy(o->p + o->len, p, n);
  o->len += n;
  return 1;
}

#endif


#if defined(LH_HAVE_EXEC) && !defined(_WIN32)  /* depth: exec on unix */

static int lh_out_take (lh_out *o, int fd) {
  char tmp[4096];
  ssize_t n = read(fd, tmp, sizeof(tmp));
  if (n > 0)
    return lh_out_add(o, tmp, (size_t)n);
  if (n == 0) return 0;  /* end of file */
  return (errno == EAGAIN || errno == EINTR) ? -1 : 0;  /* -1: nothing yet */
}

static int lh_status_of (int wst) {
  if (WIFEXITED(wst)) return WEXITSTATUS(wst);
  if (WIFSIGNALED(wst)) return 128 + WTERMSIG(wst);
  return -1;
}

static const char *lh_run (char **argv, const char *in, size_t inlen,
                           long long timeout_ms, const char *cwd,
                           int *status, lh_out *out, lh_out *err) {
  int ip[2] = { -1, -1 }, op[2] = { -1, -1 }, ep[2] = { -1, -1 };
  const char *why = NULL;
  long long deadline = lh_clock(1) + timeout_ms;
  size_t inoff = 0;
  int in_open, out_open = 1, err_open = 1, exited = 0;
  pid_t pid;
  if (pipe(ip) != 0 || pipe(op) != 0 || pipe(ep) != 0) {
    why = "exec/run: no pipes for the child";
    goto done;
  }
  pid = fork();
  if (pid < 0) {
    why = "exec/run: the child would not fork";
    goto done;
  }
  if (pid == 0) {
    long maxfd, fd;
    setpgid(0, 0);
    signal(SIGPIPE, SIG_DFL);  /* SIG_IGN would survive execvp */
    dup2(ip[0], 0);
    dup2(op[1], 1);
    dup2(ep[1], 2);
    maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 65536) maxfd = 65536;
    for (fd = 3; fd < maxfd; fd++)
      close((int)fd);
    if (cwd != NULL && chdir(cwd) != 0)
      _exit(127);
    execvp(argv[0], argv);
    _exit(127);
  }
  setpgid(pid, pid);  /* both sides, so a kill after this reaches the tree */
  close(ip[0]); ip[0] = -1;
  close(op[1]); op[1] = -1;
  close(ep[1]); ep[1] = -1;
  fcntl(ip[1], F_SETFL, O_NONBLOCK);
  fcntl(op[0], F_SETFL, O_NONBLOCK);
  fcntl(ep[0], F_SETFL, O_NONBLOCK);
  in_open = (in != NULL && inlen > 0);
  if (!in_open) { close(ip[1]); ip[1] = -1; }
  while (!exited || out_open || err_open) {
    struct pollfd fds[3];
    int nfds = 0, oi = -1, ei = -1, ii = -1, wst;
    long long left = deadline - lh_clock(1);
    if (left <= 0) {
      kill(-pid, SIGKILL);
      if (!exited) waitpid(pid, NULL, 0);
      why = "exec/run: the child was killed at its deadline";
      goto done;
    }
    if (out_open) { fds[nfds].fd = op[0]; fds[nfds].events = POLLIN; oi = nfds++; }
    if (err_open) { fds[nfds].fd = ep[0]; fds[nfds].events = POLLIN; ei = nfds++; }
    if (in_open)  { fds[nfds].fd = ip[1]; fds[nfds].events = POLLOUT; ii = nfds++; }
    if (poll(fds, (nfds_t)nfds, (int)(left > 50 ? 50 : left)) < 0 &&
        errno != EINTR) {
      kill(-pid, SIGKILL);
      if (!exited) waitpid(pid, NULL, 0);
      why = "exec/run: poll failed on the child's pipes";
      goto done;
    }
    if (oi >= 0 && (fds[oi].revents & (POLLIN | POLLHUP | POLLERR))) {
      int r = lh_out_take(out, op[0]);
      if (r == -2) { why = "exec/run: no memory for the output"; break; }
      if (r == 0) { close(op[0]); op[0] = -1; out_open = 0; }
    }
    if (ei >= 0 && (fds[ei].revents & (POLLIN | POLLHUP | POLLERR))) {
      int r = lh_out_take(err, ep[0]);
      if (r == -2) { why = "exec/run: no memory for the output"; break; }
      if (r == 0) { close(ep[0]); ep[0] = -1; err_open = 0; }
    }
    if (ii >= 0 && (fds[ii].revents & (POLLOUT | POLLERR | POLLHUP))) {
      ssize_t n = write(ip[1], in + inoff, inlen - inoff);
      if (n > 0) inoff += (size_t)n;
      if ((n < 0 && errno != EAGAIN && errno != EINTR) || inoff >= inlen) {
        close(ip[1]); ip[1] = -1; in_open = 0;  /* done, or the child closed */
      }
    }
    if (!exited && waitpid(pid, &wst, WNOHANG) == pid) {
      exited = 1;
      *status = lh_status_of(wst);
      /* A descendant may hold the pipes open past the child ('sh -c
         "server &"'). The answer is the child's own exit with what its
         pipes held when it left, so drain what is there and stop. */
      while (out_open && lh_out_take(out, op[0]) == 1) {}
      while (err_open && lh_out_take(err, ep[0]) == 1) {}
      break;
    }
  }
  if (why != NULL && !exited) {
    kill(-pid, SIGKILL);
    waitpid(pid, NULL, 0);
  }
  kill(-pid, SIGKILL);  /* the sweep: nothing exec/run starts outlives it */
done:
  if (ip[0] >= 0) close(ip[0]);
  if (ip[1] >= 0) close(ip[1]);
  if (op[0] >= 0) close(op[0]);
  if (op[1] >= 0) close(op[1]);
  if (ep[0] >= 0) close(ep[0]);
  if (ep[1] >= 0) close(ep[1]);
  return why;
}

#endif

#if defined(LH_HAVE_EXEC) && defined(_WIN32)  /* depth: exec on Windows */

/* One argument, quoted the way CommandLineToArgvW and the C runtime read
   it back: a run of backslashes is literal unless a quote follows it, so
   it is doubled there, and a quote is escaped. */
static void lh_win_quote (luaL_Buffer *b, const char *a) {
  if (*a != '\0' && strpbrk(a, " \t\n\v\"") == NULL) {
    luaL_addstring(b, a);
    return;
  }
  luaL_addchar(b, '"');
  for (;;) {
    size_t slashes = 0;
    while (*a == '\\') { a++; slashes++; }
    if (*a == '\0') {
      while (slashes--) luaL_addstring(b, "\\\\");
      break;
    }
    if (*a == '"') {
      while (slashes--) luaL_addstring(b, "\\\\");
      luaL_addstring(b, "\\\"");
    }
    else {
      while (slashes--) luaL_addchar(b, '\\');
      luaL_addchar(b, *a);
    }
    a++;
  }
  luaL_addchar(b, '"');
}

static wchar_t *lh_wide (const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w;
  if (n <= 0) return NULL;
  w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)n);
  if (w != NULL) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}

typedef struct lh_feed {
  HANDLE h;
  const char *p;
  size_t len;
} lh_feed;

/* stdin is written from its own thread: a child that never reads would
   otherwise block the write, and with it the drain of its output. */
static DWORD WINAPI lh_feed_main (LPVOID ud) {
  lh_feed *f = (lh_feed *)ud;
  size_t off = 0;
  while (off < f->len) {
    DWORD put = 0, want = (DWORD)((f->len - off) > 65536 ? 65536
                                                         : (f->len - off));
    if (!WriteFile(f->h, f->p + off, want, &put, NULL)) break;
    off += put;
  }
  CloseHandle(f->h);
  return 0;
}

/* Take what the pipe holds now: 1 data, -1 nothing yet, 0 end of file. */
static int lh_out_take (lh_out *o, HANDLE h) {
  char tmp[4096];
  DWORD avail = 0, got = 0;
  if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) return 0;
  if (avail == 0) return -1;
  if (!ReadFile(h, tmp, avail < sizeof(tmp) ? avail : sizeof(tmp), &got,
                NULL) || got == 0)
    return 0;
  return lh_out_add(o, tmp, got);
}

static const char *lh_run_win (lua_State *L, int argvidx, const char *in,
                               size_t inlen, long long timeout_ms,
                               const char *cwd, int *status, lh_out *out,
                               lh_out *err) {
  SECURITY_ATTRIBUTES sa;
  HANDLE inr = NULL, inw = NULL, outr = NULL, outw = NULL, errr = NULL,
         errw = NULL, job = NULL, feeder = NULL;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  wchar_t *wcmd = NULL, *wcwd = NULL;
  const char *why = NULL;
  lh_feed feed;
  long long deadline = lh_clock(1) + timeout_ms;
  int out_open = 1, err_open = 1;
  lua_Integer i, argc = luaL_len(L, argvidx);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  for (i = 1; i <= argc; i++) {
    lua_geti(L, argvidx, i);
    if (i > 1) luaL_addchar(&b, ' ');
    lh_win_quote(&b, lua_tostring(L, -1));
    lua_pop(L, 1);
  }
  luaL_pushresult(&b);
  wcmd = lh_wide(lua_tostring(L, -1));
  lua_pop(L, 1);
  if (cwd != NULL) wcwd = lh_wide(cwd);
  memset(&pi, 0, sizeof(pi));
  sa.nLength = sizeof(sa);
  sa.lpSecurityDescriptor = NULL;
  sa.bInheritHandle = TRUE;
  if (wcmd == NULL || (cwd != NULL && wcwd == NULL)) {
    why = "exec/run: argv is not valid UTF-8";
    goto done;
  }
  if (!CreatePipe(&inr, &inw, &sa, 0) || !CreatePipe(&outr, &outw, &sa, 0) ||
      !CreatePipe(&errr, &errw, &sa, 0)) {
    why = "exec/run: no pipes for the child";
    goto done;
  }
  /* The child inherits its own ends only. */
  SetHandleInformation(inw, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(outr, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(errr, HANDLE_FLAG_INHERIT, 0);
  job = CreateJobObjectW(NULL, NULL);
  memset(&lim, 0, sizeof(lim));
  lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (job == NULL || !SetInformationJobObject(job,
          JobObjectExtendedLimitInformation, &lim, sizeof(lim))) {
    why = "exec/run: no job object for the child";
    goto done;
  }
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = inr;
  si.hStdOutput = outw;
  si.hStdError = errw;
  /* Suspended until it is in the job, so nothing it starts escapes. */
  if (!CreateProcessW(NULL, wcmd, NULL, NULL, TRUE,
                      CREATE_SUSPENDED | CREATE_NO_WINDOW, NULL, wcwd, &si,
                      &pi)) {
    *status = 127;  /* could not be run: no such program, or no such cwd */
    goto done;
  }
  AssignProcessToJobObject(job, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(inr); inr = NULL;
  CloseHandle(outw); outw = NULL;
  CloseHandle(errw); errw = NULL;
  if (in != NULL && inlen > 0) {
    feed.h = inw; feed.p = in; feed.len = inlen;
    feeder = CreateThread(NULL, 0, lh_feed_main, &feed, 0, NULL);
    if (feeder == NULL) { CloseHandle(inw); }
  }
  else
    CloseHandle(inw);
  inw = NULL;  /* the feeder owns it now, or it is closed */
  for (;;) {
    int moved = 0, r;
    DWORD code;
    if (out_open) {
      r = lh_out_take(out, outr);
      if (r == -2) { why = "exec/run: no memory for the output"; break; }
      if (r == 0) out_open = 0;
      moved |= (r == 1);
    }
    if (err_open) {
      r = lh_out_take(err, errr);
      if (r == -2) { why = "exec/run: no memory for the output"; break; }
      if (r == 0) err_open = 0;
      moved |= (r == 1);
    }
    if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
      /* As on unix: the child's own exit, with what its pipes hold now. */
      while (out_open && lh_out_take(out, outr) == 1) {}
      while (err_open && lh_out_take(err, errr) == 1) {}
      GetExitCodeProcess(pi.hProcess, &code);
      *status = (int)code;
      break;
    }
    if (lh_clock(1) >= deadline) {
      why = "exec/run: the child was killed at its deadline";
      break;
    }
    if (!moved) Sleep(2);
  }
done:
  /* The sweep: the job ends, and everything in it with it. */
  if (job != NULL) {
    TerminateJobObject(job, 1);
    CloseHandle(job);
  }
  if (feeder != NULL) {
    WaitForSingleObject(feeder, INFINITE);
    CloseHandle(feeder);
  }
  if (pi.hProcess != NULL) CloseHandle(pi.hProcess);
  if (pi.hThread != NULL) CloseHandle(pi.hThread);
  if (inr != NULL) CloseHandle(inr);
  if (inw != NULL) CloseHandle(inw);
  if (outr != NULL) CloseHandle(outr);
  if (outw != NULL) CloseHandle(outw);
  if (errr != NULL) CloseHandle(errr);
  if (errw != NULL) CloseHandle(errw);
  free(wcmd);
  free(wcwd);
  return why;
}

#endif

static int lh_exec (lua_State *L) {
#if defined(LH_HAVE_EXEC)
  lua_Integer argc, i;
  size_t inlen = 0;
  const char *in = luaL_optlstring(L, 2, NULL, &inlen);
  long long timeout_ms = (long long)luaL_checkinteger(L, 3);
  const char *cwd = luaL_optstring(L, 4, NULL);
  char **argv;
  int status = -1;
  lh_out out = { NULL, 0, 0 }, err = { NULL, 0, 0 };
  const char *why;
#if !defined(_WIN32)
  void (*oldpipe)(int);
#endif
  luaL_checktype(L, 1, LUA_TTABLE);
  argc = luaL_len(L, 1);
  /* The vector is a userdata, so an error below frees it; the strings it
     points at stay anchored in the argv table for the whole call. */
  argv = (char **)lua_newuserdatauv(L, sizeof(char *) * (size_t)(argc + 1), 0);
  for (i = 1; i <= argc; i++) {
    size_t len;
    const char *s;
    lua_geti(L, 1, i);
    s = lua_tolstring(L, -1, &len);
    if (s == NULL || strlen(s) != len)
      return luaL_error(L, "exec/run: argv[%d] is not a plain string", (int)i);
    argv[i - 1] = (char *)s;
    lua_pop(L, 1);  /* the table still holds the string */
  }
  argv[argc] = NULL;
#if defined(_WIN32)
  (void)argv;
  why = lh_run_win(L, 1, in, inlen, timeout_ms, cwd, &status, &out, &err);
#else
  /* A child that exits before reading its stdin would raise SIGPIPE on the
     write, and the default action ends this process. */
  oldpipe = signal(SIGPIPE, SIG_IGN);
  why = lh_run(argv, in, inlen, timeout_ms, cwd, &status, &out, &err);
  signal(SIGPIPE, oldpipe);
#endif
  if (why != NULL) {
    free(out.p);
    free(err.p);
    luaL_pushfail(L);
    lua_pushstring(L, why);
    return 2;
  }
  lua_pushinteger(L, status);
  lua_pushlstring(L, out.p != NULL ? out.p : "", out.len);
  lua_pushlstring(L, err.p != NULL ? err.p : "", err.len);
  free(out.p);
  free(err.p);
  return 3;
#else
  luaL_pushfail(L);
  lua_pushliteral(L, "exec/run is not built for this platform in plain "
                     "diluvium; run under drt, or use os.execute");
  return 2;
#endif
}


/*
** sql(is_exec, db, statement, params|nil) -> value, or nil, status, why
**
** The C host's sql/query and sql/exec, against a file the program names
** the way io.open names one. Its rules are kept, so a program behaves the
** same here and under a host that grants sql: one statement per call,
** autocommit; a query that would write is refused (SQLite's own
** classification, sqlite3_stmt_readonly); the parameters must match the
** placeholders exactly. There is no row cap, as there is none on io.
** Handles are opened on first use and cached by name for the life of the
** process, in the cache table every primitive shares as its upvalue.
*/
#if defined(LH_HAVE_SQL)

#define LH_SQL_HANDLE "diluvium.localhost.sqlite"

static int lh_sql_gc (lua_State *L) {
  sqlite3 **pdb = (sqlite3 **)luaL_checkudata(L, 1, LH_SQL_HANDLE);
  if (*pdb != NULL) {
    sqlite3_close(*pdb);
    *pdb = NULL;
  }
  return 0;
}

static sqlite3 *lh_sql_open (lua_State *L, const char *name) {
  sqlite3 **pdb;
  if (lua_getfield(L, lua_upvalueindex(1), name) == LUA_TUSERDATA) {
    pdb = (sqlite3 **)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return *pdb;
  }
  lua_pop(L, 1);
  pdb = (sqlite3 **)lua_newuserdatauv(L, sizeof(sqlite3 *), 0);
  *pdb = NULL;
  if (luaL_newmetatable(L, LH_SQL_HANDLE)) {
    lua_pushcfunction(L, lh_sql_gc);
    lua_setfield(L, -2, "__gc");
  }
  lua_setmetatable(L, -2);
  if (sqlite3_open_v2(name, pdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                      NULL) != SQLITE_OK) {
    lua_pushfstring(L, "'%s' would not open: %s", name,
                    (*pdb != NULL) ? sqlite3_errmsg(*pdb) : "no memory");
    return NULL;  /* the userdata's __gc closes the failed handle */
  }
  lua_setfield(L, lua_upvalueindex(1), name);
  return *pdb;
}

static int lh_sql_fail (lua_State *L, sqlite3_stmt *st, const char *status,
                        const char *why) {
  if (st != NULL)
    sqlite3_finalize(st);
  luaL_pushfail(L);
  lua_pushstring(L, status);
  lua_pushstring(L, why);
  return 3;
}

static int lh_sql_bind (lua_State *L, sqlite3_stmt *st, int pidx,
                        char *why, size_t whycap) {
  int want = sqlite3_bind_parameter_count(st);
  int n = lua_istable(L, pidx) ? (int)luaL_len(L, pidx) : 0;
  int i;
  if (want != n) {
    snprintf(why, whycap, "the statement has %d parameter(s) but %d were "
             "supplied; bind them all or none", want, n);
    return -1;
  }
  for (i = 1; i <= n; i++) {
    int rc;
    lua_geti(L, pidx, i);
    switch (lua_type(L, -1)) {
      case LUA_TBOOLEAN:
        rc = sqlite3_bind_int(st, i, lua_toboolean(L, -1));
        break;
      case LUA_TNUMBER:
        rc = lua_isinteger(L, -1)
             ? sqlite3_bind_int64(st, i, (sqlite3_int64)lua_tointeger(L, -1))
             : sqlite3_bind_double(st, i, (double)lua_tonumber(L, -1));
        break;
      case LUA_TSTRING: {
        size_t len;
        const char *s = lua_tolstring(L, -1, &len);
        rc = sqlite3_bind_text(st, i, s, (int)len, SQLITE_TRANSIENT);
        break;
      }
      default:
        if (diluvium_msgpack_isnull(L, -1)) {
          rc = sqlite3_bind_null(st, i);
          break;
        }
        snprintf(why, whycap, "param %d is not a scalar; a parameter is "
                 "host.sql.NULL, boolean, number or string", i);
        lua_pop(L, 1);
        return -1;
    }
    lua_pop(L, 1);
    if (rc != SQLITE_OK) {
      snprintf(why, whycap, "param %d would not bind", i);
      return -1;
    }
  }
  return 0;
}

static void lh_sql_push_value (lua_State *L, sqlite3_stmt *st, int col) {
  switch (sqlite3_column_type(st, col)) {
    case SQLITE_INTEGER:
      lua_pushinteger(L, (lua_Integer)sqlite3_column_int64(st, col));
      break;
    case SQLITE_FLOAT:
      lua_pushnumber(L, (lua_Number)sqlite3_column_double(st, col));
      break;
    case SQLITE_NULL:
      lua_pushnil(L);  /* a hole, as a nil decoded off the wire is */
      break;
    default: {  /* TEXT and BLOB alike: a Lua string is bytes */
      const void *p = sqlite3_column_blob(st, col);
      lua_pushlstring(L, (p != NULL) ? (const char *)p : "",
                      (size_t)sqlite3_column_bytes(st, col));
      break;
    }
  }
}

#endif

static int lh_sql (lua_State *L) {
#if defined(LH_HAVE_SQL)
  int is_exec = lua_toboolean(L, 1);
  const char *name = luaL_checkstring(L, 2);
  size_t len;
  const char *text = luaL_checklstring(L, 3, &len);
  const char *tail = NULL;
  sqlite3_stmt *st = NULL;
  sqlite3 *db;
  char why[256];
  int rc;
  if (memchr(text, '\0', len) != NULL)
    return lh_sql_fail(L, NULL, "error",
                       "the statement has an embedded NUL byte");
  db = lh_sql_open(L, name);
  if (db == NULL)
    return lh_sql_fail(L, NULL, "error", lua_tostring(L, -1));
  rc = sqlite3_prepare_v2(db, text, (int)len + 1, &st, &tail);
  if (rc != SQLITE_OK || st == NULL) {
    snprintf(why, sizeof(why), "the statement would not prepare: %s",
             sqlite3_errmsg(db));
    return lh_sql_fail(L, st, "error", why);
  }
  while (tail != NULL && (*tail == ' ' || *tail == '\t' || *tail == '\n' ||
                          *tail == '\r' || *tail == ';'))
    tail++;
  if (tail != NULL && *tail != '\0')
    return lh_sql_fail(L, st, "error", "one statement per call; the text "
                       "past the first statement was not run and the whole "
                       "call is refused");
  if (!is_exec && !sqlite3_stmt_readonly(st))
    return lh_sql_fail(L, st, "denied", "'sql/query' is for statements "
                       "that read; this one writes, which is 'sql/exec'");
  if (lh_sql_bind(L, st, 4, why, sizeof(why)) != 0)
    return lh_sql_fail(L, st, "error", why);
  if (is_exec) {
    rc = sqlite3_step(st);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
      snprintf(why, sizeof(why), "the statement failed: %s",
               sqlite3_errmsg(db));
      return lh_sql_fail(L, st, "error", why);
    }
    sqlite3_finalize(st);
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, (lua_Integer)sqlite3_changes(db));
    lua_setfield(L, -2, "changes");
    lua_pushinteger(L, (lua_Integer)sqlite3_last_insert_rowid(db));
    lua_setfield(L, -2, "rowid");
    return 1;
  }
  {
    int ncols = sqlite3_column_count(st), i;
    lua_Integer nrows = 0;
    lua_createtable(L, 0, 2);
    lua_createtable(L, ncols, 0);
    for (i = 0; i < ncols; i++) {
      lua_pushstring(L, sqlite3_column_name(st, i));
      lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "cols");
    lua_newtable(L);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      lua_createtable(L, ncols, 0);
      for (i = 0; i < ncols; i++) {
        lh_sql_push_value(L, st, i);
        lua_rawseti(L, -2, i + 1);
      }
      lua_rawseti(L, -2, ++nrows);
    }
    if (rc != SQLITE_DONE) {
      snprintf(why, sizeof(why), "the query failed mid-walk: %s",
               sqlite3_errmsg(db));
      return lh_sql_fail(L, st, "error", why);
    }
    sqlite3_finalize(st);
    lua_setfield(L, -2, "rows");
    return 1;
  }
#else
  luaL_pushfail(L);
  lua_pushliteral(L, "denied");
  lua_pushliteral(L, "plain diluvium has no SQL engine on this platform; "
                     "run under drt");
  return 3;
#endif
}


/* ======================================================================
** The entry point
** ====================================================================== */

LUA_API void diluvium_localhost_install (lua_State *L) {
  lh_mono_base = lh_clock(1);
  if (luaL_loadbufferx(L, LOCALHOST_SRC, sizeof(LOCALHOST_SRC) - 1,
                       "=localhost", "t") != LUA_OK)
    lua_error(L);
  luaL_newlibtable(L, lh_prims);
  lua_newtable(L);                      /* the shared upvalue: sql handles */
  luaL_setfuncs(L, lh_prims, 1);
  lua_newtable(L);                      /* families this build cannot answer */
#if !defined(LH_HAVE_EXEC)
  lua_pushboolean(L, 1);
  lua_setfield(L, -2, "exec");
#endif
#if !defined(LH_HAVE_SQL)
  lua_pushboolean(L, 1);
  lua_setfield(L, -2, "sql");
#endif
  lua_setfield(L, -2, "lacks");
  lua_call(L, 1, 1);                    /* the chunk returns the answerer */
  diluvium_hostlib_open_local(L);       /* answerer -> the host module */
  lua_pushvalue(L, -1);
  lua_setglobal(L, "host");
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  lua_insert(L, -2);
  lua_setfield(L, -2, "host");          /* so require "host" agrees */
  lua_pop(L, 1);
}
