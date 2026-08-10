/*
** dsnap.c
** Snapshots: the runtime fingerprint and the header. See dsnap.h.
**
** On-top code: public Lua C API only. Nothing here reads a core internal
** header -- that is dshim.c's job and its alone.
*/

#define dsnap_c

#include "lprefix.h"

#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "dhash.h"
#include "dmsgpack.h"
#include "dqueue.h"
#include "dsnap.h"


int ds_decodeheader_trampoline (lua_State *L);


/* Registry key for the cached fingerprint. Its address is the key. */
static const char DS_FINGERPRINT = 0;


/*
** The canary.
**
** Chosen to exercise the codegen decisions that differ between builds rather
** than to be short. In particular the table constructor with several constant
** fields is what MAXINDEXRK changes -- the measured difference between this
** tree's debug and release builds was exactly here, individual SETFIELDs
** against LOADK pairs plus a SETLIST. The rest covers the ordinary opcode
** families: arithmetic, comparison, a numeric and a generic 'for', varargs, a
** closure with an upvalue, a method call, string and float constants.
**
** It is never run. Compiling and dumping it is the whole point, so it does not
** need to be correct code, only representative code -- but it is valid, because
** a chunk that fails to compile would make the fingerprint an error path.
**
** Changing this string changes the fingerprint, which invalidates every existing
** snapshot and every cached Proto hash. That is a breaking change and should
** come with a DILUVIUM_SNAP_FORMAT bump.
*/
static const char DS_CANARY[] =
  "local t = {alpha = 1, beta = 2.5, gamma = 'three', delta = 4,"
  " epsilon = 5, zeta = 6, eta = 7, theta = 8}\n"
  "local function add(a, b) return a + b - a * b / 2 end\n"
  "local function counter()\n"
  "  local n = 0\n"
  "  return function(...) n = n + select('#', ...) return n end\n"
  "end\n"
  "local acc = 0\n"
  "for i = 1, 10 do acc = add(acc, i) end\n"
  "for k, v in pairs(t) do if k ~= 'gamma' and v > 2 then acc = acc + 1 end end\n"
  "local s = ('%d/%s'):format(acc, t.gamma)\n"
  "return s, counter(), t\n";


/* Collect a dump into a SHA-256 state. */
static int ds_dumpwriter (lua_State *L, const void *p, size_t sz, void *ud) {
  (void)L;
  diluvium_sha256_update((diluvium_sha256_ctx *)ud, p, sz);
  return 0;
}


LUA_API void diluvium_snap_fingerprint (lua_State *L, char *out) {
  int base = lua_gettop(L);
  diluvium_sha256_ctx h;
  unsigned char digest[DILUVIUM_SHA256_SIZE];
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DS_FINGERPRINT) == LUA_TSTRING) {
    const char *cached = lua_tostring(L, -1);
    memcpy(out, cached, DILUVIUM_SHA256_HEX);
    lua_settop(L, base);
    return;
  }
  lua_pop(L, 1);
  diluvium_sha256_init(&h);
  /*
  ** The version string first, so two builds with identical codegen but
  ** different versions are still told apart. 10.6 asks for version *and* build;
  ** the dump supplies the build, this supplies the version.
  */
  diluvium_sha256_update(&h, "diluvium-runtime-1\0", 19);
  diluvium_sha256_update(&h, LUA_RELEASE, strlen(LUA_RELEASE) + 1);
  if (luaL_loadbuffer(L, DS_CANARY, sizeof(DS_CANARY) - 1, "=canary")
      != LUA_OK) {
    /* Not reachable with a valid canary, and if it ever is, failing loudly
       beats fingerprinting an error message. */
    lua_error(L);
  }
  /* Stripped, per 10.5: line numbers and source names must not enter the hash,
     or a comment reflow would invalidate every cached agent. */
  if (lua_dump(L, ds_dumpwriter, &h, 1) != 0)
    luaL_error(L, "snapshot: could not dump the fingerprint canary");
  lua_settop(L, base);
  diluvium_sha256_final(&h, digest);
  diluvium_sha256_hex(digest, out);
  lua_pushlstring(L, out, DILUVIUM_SHA256_HEX - 1);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &DS_FINGERPRINT);
  lua_settop(L, base);
}


/* ======================================================================
** Permanents (10.4)
** ====================================================================== */

/*
** Two registry tables, because the map is needed in both directions: value to
** name while encoding, name to value while restoring. Kept as tables rather than
** one table with mixed keys so that a program-supplied string can never collide
** with a permanent's value.
*/
static const char DS_PERM_NAME = 0;     /* value -> name  */
static const char DS_PERM_VALUE = 0;    /* name  -> value */
static const char DS_PERM_FP = 0;       /* the cached fingerprint hex */

/*
** The modules walked, in this fixed order.
**
** Fixed rather than discovered by iterating 'package.loaded', and the reason is
** determinism of *naming* rather than of iteration. A value reachable by two
** paths -- 'string' is both '_G.string' and 'package.loaded.string' -- must get
** the same name in every process, and first-path-wins only settles that if the
** order of paths is settled. Iteration order inside one module is left to
** 'pairs', which this tree makes deterministic (luaconf.h fixes the string hash
** seed and test_determinism.lua asserts it), and the permanents fingerprint in
** the header is the backstop if that ever stops being true.
*/
static const char *const DS_MODULES[] = {
  "coroutine", "debug", "io", "math", "os", "string", "table", "utf8",
  "msgpack", "queue", "endpoint", NULL
};


/*
** Record one permanent. 'nameidx' and 'validx' are absolute stack indices of
** the two registry tables.
*/
static void ds_perm_put (lua_State *L, int nameidx, int validx,
                         const char *name) {
  /* stack: ... value */
  int t = lua_type(L, -1);
  if (t != LUA_TFUNCTION && t != LUA_TTABLE) {
    lua_pop(L, 1);
    return;
  }
  lua_pushvalue(L, -1);
  if (lua_rawget(L, nameidx) != LUA_TNIL) {  /* already named: first wins */
    lua_pop(L, 2);
    return;
  }
  lua_pop(L, 1);
  /* name -> value */
  lua_pushstring(L, name);
  lua_pushvalue(L, -2);
  lua_rawset(L, validx);
  /* value -> name */
  lua_pushstring(L, name);
  lua_rawset(L, nameidx);
}


/* Walk one table's direct fields, naming C functions as "prefix.key". */
static void ds_perm_walk (lua_State *L, int nameidx, int validx,
                          int tableidx, const char *prefix) {
  char buf[128];
  lua_pushnil(L);
  while (lua_next(L, tableidx) != 0) {
    /* stack: ... key value */
    if (lua_type(L, -2) == LUA_TSTRING && lua_iscfunction(L, -1)) {
      const char *key = lua_tostring(L, -2);
      if (prefix == NULL)
        snprintf(buf, sizeof(buf), "%s", key);
      else
        snprintf(buf, sizeof(buf), "%s.%s", prefix, key);
      ds_perm_put(L, nameidx, validx, buf);  /* pops the value */
    }
    else
      lua_pop(L, 1);
    /* the key stays, driving the walk */
  }
}


LUA_API void diluvium_snap_permanents (lua_State *L) {
  int base = lua_gettop(L);
  int nameidx, validx, i;
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DS_PERM_NAME) == LUA_TTABLE) {
    lua_settop(L, base);
    return;  /* already built */
  }
  lua_pop(L, 1);
  lua_newtable(L);  nameidx = lua_gettop(L);
  lua_newtable(L);  validx = lua_gettop(L);

  /* '_G' first, and by that name, because it is the one every closure that
     touches a global reaches through its '_ENV' upvalue. */
  lua_pushglobaltable(L);
  ds_perm_put(L, nameidx, validx, "_G");

  lua_pushglobaltable(L);
  ds_perm_walk(L, nameidx, validx, lua_gettop(L), NULL);
  lua_pop(L, 1);

  for (i = 0; DS_MODULES[i] != NULL; i++) {
    char buf[128];
    if (lua_getglobal(L, DS_MODULES[i]) != LUA_TTABLE) {
      lua_pop(L, 1);
      continue;  /* a library this build does not open */
    }
    snprintf(buf, sizeof(buf), "%s", DS_MODULES[i]);
    lua_pushvalue(L, -1);
    ds_perm_put(L, nameidx, validx, buf);
    ds_perm_walk(L, nameidx, validx, lua_gettop(L), DS_MODULES[i]);
    lua_pop(L, 1);
  }

  /* The string metatable. Reachable from any string through '__index', so a
     program that keeps 'getmetatable("")' would otherwise serialize a table full
     of C functions -- and it is shared runtime furniture, not program state. */
  lua_pushliteral(L, "");
  if (lua_getmetatable(L, -1)) {
    lua_pushvalue(L, -1);
    ds_perm_put(L, nameidx, validx, "string.metatable");
    ds_perm_walk(L, nameidx, validx, lua_gettop(L), "string.metatable");
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  lua_pushvalue(L, nameidx);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &DS_PERM_NAME);
  lua_pushvalue(L, validx);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &DS_PERM_VALUE);
  lua_settop(L, base);
}


/*
** The permanents fingerprint: SHA-256 over the sorted name list.
**
** Sorted, so the value does not depend on iteration order even though this
** tree's iteration order is deterministic. That costs one 'table.sort' once per
** state and removes a dependency; a fingerprint that silently varied would turn
** every restore into a refusal, which is safe but useless.
*/
static void ds_permanents (lua_State *L, char *out) {
  int base = lua_gettop(L);
  diluvium_sha256_ctx h;
  unsigned char digest[DILUVIUM_SHA256_SIZE];
  lua_Integer i, n;
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DS_PERM_FP) == LUA_TSTRING) {
    memcpy(out, lua_tostring(L, -1), DILUVIUM_SHA256_HEX);
    lua_settop(L, base);
    return;
  }
  lua_pop(L, 1);
  diluvium_snap_permanents(L);
  diluvium_sha256_init(&h);
  diluvium_sha256_update(&h, "diluvium-permanents-1\0", 22);
  lua_rawgetp(L, LUA_REGISTRYINDEX, &DS_PERM_VALUE);
  lua_newtable(L);                             /* the name list */
  n = 0;
  lua_pushnil(L);
  while (lua_next(L, -3) != 0) {
    lua_pop(L, 1);                             /* the value */
    lua_pushvalue(L, -1);                      /* the name */
    lua_rawseti(L, -3, ++n);
  }
  lua_getglobal(L, "table");
  lua_getfield(L, -1, "sort");
  lua_pushvalue(L, -3);
  lua_call(L, 1, 0);
  lua_pop(L, 1);                               /* the 'table' module */
  for (i = 1; i <= n; i++) {
    size_t len;
    const char *nm;
    lua_rawgeti(L, -1, i);
    nm = lua_tolstring(L, -1, &len);
    diluvium_sha256_update(&h, nm, len + 1);
    lua_pop(L, 1);
  }
  lua_settop(L, base);
  diluvium_sha256_final(&h, digest);
  diluvium_sha256_hex(digest, out);
  lua_pushlstring(L, out, DILUVIUM_SHA256_HEX - 1);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &DS_PERM_FP);
  lua_settop(L, base);
}


/*
** Build the header as a Lua table and encode it with the plain codec.
**
** The plain codec on purpose, not the graph encoder: a header has no sharing, no
** cycles and no functions, and a reader that refuses the payload must still be
** able to read the header -- including a reader in another language whose only
** msgpack is the ordinary kind. That is what makes "refused, and here is which
** field differed" possible instead of an opaque no.
*/
static void ds_buildheader (lua_State *L, const diluvium_snap_opts *opts) {
  char fp[DILUVIUM_SHA256_HEX];
  char perm[DILUVIUM_SHA256_HEX];
  lua_Integer id = 0;
  lua_Integer n = 0;
  diluvium_snap_fingerprint(L, fp);
  ds_permanents(L, perm);
  lua_createtable(L, 0, 6);
  lua_pushinteger(L, DILUVIUM_SNAP_FORMAT);
  lua_setfield(L, -2, "format");
  lua_pushstring(L, fp);
  lua_setfield(L, -2, "runtime");
  lua_pushstring(L, perm);
  lua_setfield(L, -2, "permanents");
  lua_pushstring(L, (opts != NULL && opts->capabilities != NULL)
                    ? opts->capabilities : "");
  lua_setfield(L, -2, "capabilities");
  lua_pushstring(L, (opts != NULL && opts->host != NULL) ? opts->host : "");
  lua_setfield(L, -2, "host");
  /* The queue name list, for 10.8's re-resolution. Names and not handles: 6.2
     says a handle is runtime identity, so carrying one would be carrying the
     exact thing that must not survive. */
  lua_newtable(L);
  for (;;) {
    const char *name = NULL;
    id = diluvium_queue_next(L, id, &name);
    if (id == 0)
      break;
    lua_pushstring(L, (name != NULL) ? name : "");
    lua_rawseti(L, -2, ++n);
  }
  lua_setfield(L, -2, "queues");
}


LUA_API size_t diluvium_snap_header (lua_State *L,
                                     const diluvium_snap_opts *opts,
                                     char *out, size_t cap) {
  int base = lua_gettop(L);
  size_t len;
  const char *s;
  ds_buildheader(L, opts);
  diluvium_msgpack_encode(L, -1);
  s = lua_tolstring(L, -1, &len);
  if (len > cap) {
    lua_settop(L, base);
    return 0;
  }
  memcpy(out, s, len);
  lua_settop(L, base);
  return len;
}


/* Read a string field of the header table at 'idx'. Pushes nothing. */
static int ds_field_is (lua_State *L, int idx, const char *key,
                        const char *want) {
  int equal;
  const char *got;
  if (lua_getfield(L, idx, key) != LUA_TSTRING) {
    lua_pop(L, 1);
    return 0;
  }
  got = lua_tostring(L, -1);
  equal = (strcmp(got, want) == 0);
  lua_pop(L, 1);
  return equal;
}


LUA_API int diluvium_snap_checkheader (lua_State *L,
                                       const diluvium_snap_opts *opts,
                                       const char *s, size_t len,
                                       size_t *used) {
  int base = lua_gettop(L);
  char fp[DILUVIUM_SHA256_HEX];
  char perm[DILUVIUM_SHA256_HEX];
  int hdr;
  int rc = DILUVIUM_SNAP_ACCEPT;
  if (used != NULL) *used = 0;
  /*
  ** Decoded inside a protected call. 10.10 is explicit that a snapshot is
  ** untrusted input, and the very first thing that touches it must therefore be
  ** unable to take the host down -- including on bytes that are not msgpack at
  ** all, where the codec raises.
  */
  {
    lua_pushcfunction(L, ds_decodeheader_trampoline);
    lua_pushlstring(L, s, len);
    if (lua_pcall(L, 1, 2, 0) != LUA_OK) {
      lua_settop(L, base);
      return DILUVIUM_SNAP_BAD_HEADER;
    }
  }
  /* stack: header, bytes-consumed */
  if (!lua_isinteger(L, -1) || !lua_istable(L, -2)) {
    lua_settop(L, base);
    return DILUVIUM_SNAP_BAD_HEADER;
  }
  if (used != NULL)
    *used = (size_t)lua_tointeger(L, -1);
  hdr = lua_absindex(L, -2);

  if (lua_getfield(L, hdr, "format") != LUA_TNUMBER ||
      !lua_isinteger(L, -1) ||
      lua_tointeger(L, -1) != DILUVIUM_SNAP_FORMAT)
    rc = DILUVIUM_SNAP_FORMAT_MISMATCH;
  lua_pop(L, 1);

  /* Order matters. Runtime identity is checked before the permanents and
     capability sets because a mismatch there makes the other two meaningless --
     they are strings produced by a different build. Reporting the most
     fundamental difference is the difference between a useful diagnostic and a
     misleading one. */
  if (rc == DILUVIUM_SNAP_ACCEPT) {
    diluvium_snap_fingerprint(L, fp);
    if (!ds_field_is(L, hdr, "runtime", fp))
      rc = DILUVIUM_SNAP_RUNTIME_MISMATCH;
  }
  if (rc == DILUVIUM_SNAP_ACCEPT) {
    ds_permanents(L, perm);
    if (!ds_field_is(L, hdr, "permanents", perm))
      rc = DILUVIUM_SNAP_PERMANENTS_MISMATCH;
  }
  if (rc == DILUVIUM_SNAP_ACCEPT) {
    const char *want = (opts != NULL && opts->capabilities != NULL)
                       ? opts->capabilities : "";
    if (!ds_field_is(L, hdr, "capabilities", want))
      rc = DILUVIUM_SNAP_CAPABILITY_MISMATCH;
  }
  if (rc == DILUVIUM_SNAP_ACCEPT) {
    /*
    ** The host stamp, and the asymmetry is deliberate. A snapshot carrying no
    ** stamp restores anywhere, because a process moving its own state has
    ** nothing to check against. A snapshot that *is* stamped must match, and a
    ** host that supplies a stamp is refused a snapshot without one -- otherwise
    ** stamping would be advisory, and an unstamped snapshot from anywhere would
    ** pass the check a host added precisely to stop that.
    */
    const char *want = (opts != NULL && opts->host != NULL) ? opts->host : "";
    if (!ds_field_is(L, hdr, "host", want))
      rc = DILUVIUM_SNAP_HOST_MISMATCH;
  }
  lua_settop(L, base);
  if (rc != DILUVIUM_SNAP_ACCEPT && used != NULL)
    *used = 0;
  return rc;
}


LUA_API const char *diluvium_snap_why (int code) {
  switch (code) {
    case DILUVIUM_SNAP_ACCEPT:
      return "the header matches this runtime";
    case DILUVIUM_SNAP_BAD_HEADER:
      return "the bytes do not begin with a snapshot header";
    case DILUVIUM_SNAP_FORMAT_MISMATCH:
      return "the snapshot format version is not the one this runtime writes";
    case DILUVIUM_SNAP_RUNTIME_MISMATCH:
      return "the snapshot was taken by a different runtime build; its "
             "bytecode would not match, so it is refused rather than "
             "half-loaded";
    case DILUVIUM_SNAP_PERMANENTS_MISMATCH:
      return "the snapshot's permanents set differs from this runtime's, so a "
             "C function in it could not be resolved to the same thing";
    case DILUVIUM_SNAP_CAPABILITY_MISMATCH:
      return "the snapshot was taken under a different capability set";
    case DILUVIUM_SNAP_HOST_MISMATCH:
      return "the snapshot's host identity stamp is not this host's";
    default:
      return "unknown reason";
  }
}


LUA_API int diluvium_snap_headerqueues (lua_State *L, const char *s,
                                        size_t len) {
  int base = lua_gettop(L);
  lua_pushcfunction(L, ds_decodeheader_trampoline);
  lua_pushlstring(L, s, len);
  if (lua_pcall(L, 1, 2, 0) != LUA_OK) {
    lua_settop(L, base);
    return 0;
  }
  lua_pop(L, 1);                                /* the consumed count */
  if (!lua_istable(L, -1) ||
      lua_getfield(L, -1, "queues") != LUA_TTABLE) {
    lua_settop(L, base);
    return 0;
  }
  lua_remove(L, -2);                            /* drop the header */
  return 1;
}


/*
** Decode a header and report how many bytes it used, under the caller's
** protection. Exposed rather than static because both entry points above run it
** through 'lua_pcall': 10.10 calls a snapshot untrusted input, and the first
** thing that touches those bytes must not be able to take the host down -- the
** codec raises on bytes that are not msgpack at all, which is the common case
** for a file that is not a snapshot.
**
** The string is at index 1. Returns the header table and the byte count.
*/
int ds_decodeheader_trampoline (lua_State *L) {
  size_t len, used = 0;
  const char *s = luaL_checklstring(L, 1, &len);
  diluvium_msgpack_decode_n(L, s, len, &used);
  if (!lua_istable(L, -1))
    return luaL_error(L, "snapshot: the header is not a map");
  lua_pushinteger(L, (lua_Integer)used);
  return 2;
}


/* ======================================================================
** Prototypes and closures (10.5, 10.9)
** ====================================================================== */

static const char DS_PROTOS = 0;        /* hash hex -> a function with it */

/*
** Hash a Lua closure's stripped dump and collect the bytes.
**
** The buffer is initialized *inside* the writer, on its first call, and that is
** not a style choice. 'luaL_buffinit' pushes a placeholder in Lua 5.5, and
** 'lua_dump' requires the function to be on top of the stack -- so initializing
** the buffer first hides the function behind the placeholder and 'lua_dump'
** aborts on its own api_check. Found exactly that way. lstrlib.c's 'str_dump'
** carries the same workaround with the same comment, which is the strongest
** available evidence that it is the intended shape rather than a local hack.
**
** 'lua_dump' also restores the stack top when it returns, so the result cannot
** simply be left on the stack: it goes into a slot reserved below the function
** before the dump starts.
*/
typedef struct ds_dump {
  diluvium_sha256_ctx h;
  luaL_Buffer b;
  int started;                          /* the buffer has been initialized */
  int slot;                             /* where the finished string goes */
} ds_dump;

static int ds_collect (lua_State *L, const void *p, size_t sz, void *ud) {
  ds_dump *d = (ds_dump *)ud;
  if (!d->started) {
    d->started = 1;
    luaL_buffinit(L, &d->b);
  }
  if (p == NULL) {                      /* ldump.c signals the end this way */
    luaL_pushresult(&d->b);
    lua_replace(L, d->slot);
    return 0;
  }
  diluvium_sha256_update(&d->h, p, sz);
  luaL_addlstring(&d->b, (const char *)p, sz);
  return 0;
}


/*
** Dump the Lua closure on top of the stack, stripped, replacing it with the
** bytes and writing the digest to 'digest'.
**
** Stripped for two reasons that happen to agree. 10.5 wants line numbers and
** source names out of the hash domain, so a comment reflow does not invalidate
** every cached agent. And 10.9 wants the *real* dump path used rather than a
** parallel encoder, so that 'taintSecureStrings' and the per-string scramble
** flag come along -- which they do, because this is 'lua_dump' and nothing else.
*/
static void ds_dumpclosure (lua_State *L, unsigned char *digest) {
  ds_dump d;
  luaL_checkstack(L, 4, "snapshot: cannot dump a function");
  lua_pushnil(L);
  lua_insert(L, -2);                    /* reserved slot, then the closure */
  d.slot = lua_absindex(L, -2);
  d.started = 0;
  diluvium_sha256_init(&d.h);
  if (lua_dump(L, ds_collect, &d, 1) != 0)
    luaL_error(L, "snapshot: a function could not be dumped");
  lua_pop(L, 1);                        /* the closure; the bytes are below */
  if (!lua_isstring(L, -1))
    luaL_error(L, "snapshot: a function dumped to no bytes");
  diluvium_sha256_final(&d.h, digest);
}


LUA_API int diluvium_snap_register (lua_State *L, int idx) {
  int abs = lua_absindex(L, idx);
  int base = lua_gettop(L);
  unsigned char digest[DILUVIUM_SHA256_SIZE];
  char hex[DILUVIUM_SHA256_HEX];
  if (lua_type(L, abs) != LUA_TFUNCTION || lua_iscfunction(L, abs))
    return 0;
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DS_PROTOS) != LUA_TTABLE) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &DS_PROTOS);
  }
  lua_pushvalue(L, abs);
  ds_dumpclosure(L, digest);            /* leaves the dump bytes */
  diluvium_sha256_hex(digest, hex);
  lua_pushstring(L, hex);
  lua_insert(L, -2);                    /* hex, bytes */
  lua_rawset(L, -3);                    /* protos[hex] = bytes */
  lua_settop(L, base);
  return 1;
}


LUA_API int diluvium_snap_registered (lua_State *L) {
  int n = 0;
  int base = lua_gettop(L);
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DS_PROTOS) == LUA_TTABLE) {
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
      lua_pop(L, 1);
      n++;
    }
  }
  lua_settop(L, base);
  return n;
}


/* Is this hash in the runtime's proto registry? Pushes the dump if so. */
static int ds_lookupproto (lua_State *L, const char *hex) {
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DS_PROTOS) != LUA_TTABLE) {
    lua_pop(L, 1);
    return 0;
  }
  if (lua_getfield(L, -1, hex) != LUA_TSTRING) {
    lua_pop(L, 2);
    return 0;
  }
  lua_remove(L, -2);
  return 1;
}


/*
** Ext 0x06 payload: a closure.
**
**   byte 0        number of upvalues
**   byte 1        0 = the prototype is referenced by hash, 1 = inlined after
**   bytes 2..33   the SHA-256 of the stripped dump
**   bytes 34..    the stripped dump, when inlined
**
** Bytes 1 through 33 are ext 0x03's payload -- a prototype reference by hash --
** carried inside 0x06 rather than standing alone. A snapshot has no bare
** prototypes to refer to: a prototype is not a Lua value, so it could not take a
** position in the object graph without inventing a pseudo-value for it. 5.5 has
** been corrected to say so.
**
** The upvalues are not here. They are queued as fixups by the codec, which is
** what the hook's return of 2 asks for, because an upvalue is an arbitrary Lua
** value and belongs in the graph rather than in an opaque payload.
*/
static int ds_encodeclosure (lua_State *L, int abs) {
  int base = lua_gettop(L);
  unsigned char digest[DILUVIUM_SHA256_SIZE];
  char hex[DILUVIUM_SHA256_HEX];
  int nup = 0;
  size_t dumplen;
  const char *dump;
  luaL_Buffer out;
  int known;
  while (lua_getupvalue(L, abs, nup + 1) != NULL) {
    lua_pop(L, 1);
    nup++;
  }
  if (nup > 255)
    luaL_error(L, "snapshot: a closure with %d upvalues is beyond the format",
               nup);
  lua_pushvalue(L, abs);
  ds_dumpclosure(L, digest);            /* leaves the dump bytes */
  diluvium_sha256_hex(digest, hex);
  dump = lua_tolstring(L, -1, &dumplen);
  /*
  ** Referenced when the target runtime already has it, inlined otherwise --
  ** 10.5's rule exactly, and the reason a swarm of one-off agents over a shared
  ** library carries only the unique part. A stream-local record is kept too, so
  ** the second closure of one prototype in a single snapshot references the copy
  ** the first one inlined.
  */
  known = ds_lookupproto(L, hex);
  if (known)
    lua_pop(L, 1);                      /* the registry's copy */
  luaL_buffinit(L, &out);
  {
    char head[2];
    head[0] = (char)(unsigned char)nup;
    head[1] = (char)(known ? 0 : 1);
    luaL_addlstring(&out, head, 2);
  }
  luaL_addlstring(&out, (const char *)digest, DILUVIUM_SHA256_SIZE);
  if (!known) {
    luaL_addlstring(&out, dump, dumplen);
    /* Register it, so a second closure of the same prototype later in this
       stream is a reference rather than another copy. */
    lua_pushvalue(L, abs);
    diluvium_snap_register(L, -1);
    lua_pop(L, 1);
  }
  luaL_pushresult(&out);
  /* Hand the finished payload to the codec as an ext object. */
  {
    size_t plen;
    const char *p = lua_tolstring(L, -1, &plen);
    diluvium_msgpack_appendext(L, 0x06, p, plen);
  }
  lua_settop(L, base);
  return 2;                             /* 2: queue this closure's upvalues */
}


static int ds_decodeclosure (lua_State *L, const unsigned char *data,
                             size_t len) {
  char hex[DILUVIUM_SHA256_HEX];
  int nup, inlined, got;
  unsigned char check[DILUVIUM_SHA256_SIZE];
  const char *dump;
  size_t dlen;
  if (len < 2 + DILUVIUM_SHA256_SIZE)
    return luaL_error(L, "snapshot: a closure record is %d bytes, too short to "
                         "hold a prototype hash", (int)len);
  nup = (int)data[0];
  inlined = (int)data[1];
  if (inlined != 0 && inlined != 1)
    return luaL_error(L, "snapshot: a closure record's inline flag is %d, not 0 "
                         "or 1", inlined);
  diluvium_sha256_hex(data + 2, hex);
  if (inlined) {
    dump = (const char *)(data + 2 + DILUVIUM_SHA256_SIZE);
    dlen = len - 2 - DILUVIUM_SHA256_SIZE;
    /*
    ** The hash is verified against the bytes rather than trusted. 10.5 makes the
    ** hash the *name* of the code, so a record whose bytes hash to something
    ** else is either corrupt or is trying to have code stored under a name it
    ** does not own. The bytes still go through the loader and lverify.c after
    ** this, so this is not the only check -- it is the one that keeps the name
    ** meaning what it says, which is what the reference case relies on.
    */
    diluvium_sha256(dump, dlen, check);
    if (memcmp(check, data + 2, DILUVIUM_SHA256_SIZE) != 0)
      return luaL_error(L, "snapshot: an inlined prototype does not hash to the "
                           "name it was given");
    lua_pushlstring(L, dump, dlen);
  }
  else {
    /* 10.5: "restore requires an exact match. Mismatch is a clean refusal,
       never a best-effort load." */
    if (!ds_lookupproto(L, hex))
      return luaL_error(L, "snapshot: prototype %s is referenced but is not "
                           "present in this runtime", hex);
  }
  dump = lua_tolstring(L, -1, &dlen);
  if (luaL_loadbufferx(L, dump, dlen, "=snapshot", "b") != LUA_OK)
    return luaL_error(L, "snapshot: a prototype would not load: %s",
                      lua_tostring(L, -1));
  lua_remove(L, -2);                    /* the dump bytes */
  /*
  ** The declared upvalue count is checked against the loaded closure's. A
  ** mismatch means the record and its bytecode disagree, and while the fixups
  ** are range-checked one by one anyway, catching it here says which record was
  ** wrong instead of which fixup was.
  */
  for (got = 0; lua_getupvalue(L, -1, got + 1) != NULL; got++)
    lua_pop(L, 1);
  if (got != nup)
    return luaL_error(L, "snapshot: a closure record claims %d upvalues but its "
                         "prototype has %d", nup, got);
  /*
  ** 'lua_load' sets upvalue 1 to the globals table when a chunk has upvalues,
  ** which is right for loading a chunk and wrong here: every upvalue is about to
  ** be filled from the graph, and leaving this one would mean a closure whose
  ** first upvalue silently defaulted to '_G' if its fixup were ever missing. So
  ** it is cleared, turning that into a visible nil rather than a plausible
  ** wrong answer.
  */
  if (nup > 0) {
    lua_pushnil(L);
    lua_setupvalue(L, -2, 1);
  }
  return 1;
}


/* ======================================================================
** The hooks
** ====================================================================== */

static int ds_hook_permanent (lua_State *L, int idx, void *ud) {
  int abs = lua_absindex(L, idx);
  int base = lua_gettop(L);
  (void)ud;
  diluvium_snap_permanents(L);
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DS_PERM_NAME) != LUA_TTABLE) {
    lua_settop(L, base);
    return 0;
  }
  lua_pushvalue(L, abs);
  if (lua_rawget(L, -2) != LUA_TSTRING) {
    lua_settop(L, base);
    return 0;
  }
  {
    size_t nlen;
    const char *name = lua_tolstring(L, -1, &nlen);
    diluvium_msgpack_appendext(L, 0x07, name, nlen);
  }
  lua_settop(L, base);
  return 1;
}


static int ds_hook_encode (lua_State *L, int idx, void *ud) {
  int abs = lua_absindex(L, idx);
  (void)ud;
  switch (lua_type(L, abs)) {
    case LUA_TFUNCTION:
      if (lua_iscfunction(L, abs)) {
        /*
        ** A C function that is not a permanent. There is nothing to serialize --
        ** its code is in the host binary -- so this can only be a refusal. The
        ** message is raised here rather than left to the codec's generic "cannot
        ** encode a function value", because the cause is almost always a
        ** host-registered function the host did not add to the permanents, and
        ** saying so is the difference between a fixable report and a puzzle.
        */
        return luaL_error(L, "snapshot: this C function is not in the "
                             "permanents table, so it has no name to be "
                             "restored by (10.4); a host that registers its own "
                             "C functions must name them");
      }
      return ds_encodeclosure(L, abs);
    default:
      return 0;                        /* userdata and threads: not yet */
  }
}


static int ds_hook_decode (lua_State *L, int code, const unsigned char *data,
                           size_t len, void *ud) {
  (void)ud;
  switch (code) {
    case 0x06:
      return ds_decodeclosure(L, data, len);
    case 0x07: {
      int base = lua_gettop(L);
      diluvium_snap_permanents(L);
      if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DS_PERM_VALUE) != LUA_TTABLE) {
        lua_settop(L, base);
        return 0;
      }
      lua_pushlstring(L, (const char *)data, len);
      if (lua_rawget(L, -2) == LUA_TNIL) {
        /* 10.4 says the permanents set must be identical on save and restore,
           and the header's fingerprint is what enforces that -- so reaching
           here means the header was accepted and the sets still differ, which
           is worth naming precisely. */
        lua_pushlstring(L, (const char *)data, len);
        return luaL_error(L, "snapshot: '%s' is named as a permanent but this "
                             "runtime has no such name",
                          lua_tostring(L, -1));
      }
      lua_insert(L, base + 1);
      lua_settop(L, base + 1);
      return 1;
    }
    default:
      return 0;                        /* 0x03 nested in 0x06; 0x05 not yet */
  }
}


LUA_API const diluvium_snap_hooks *diluvium_snap_hooks_for (lua_State *L) {
  static const diluvium_snap_hooks hooks = {
    ds_hook_permanent, ds_hook_encode, ds_hook_decode, NULL
  };
  diluvium_snap_permanents(L);
  return &hooks;
}
