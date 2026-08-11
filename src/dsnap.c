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
#include "dshim.h"
#include "dtask.h"
#include "dsnap.h"


int ds_decodeheader_trampoline (lua_State *L);

/* Threads live at the foot of this file, but the hooks in the middle need them. */
static int ds_encodethread (lua_State *L, int abs);
static int ds_decodethread (lua_State *L, const unsigned char *data, size_t len);
static int ds_buildthread (lua_State *L, int thidx);
static int ds_hook_begin (lua_State *L, void *ud);
static int ds_hook_finish (lua_State *L, void *ud);


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
  /* ldump.c signals the end of the dump with (NULL, 0), the same way it does for
     'ds_codewriter' below. There is nothing to hash; saying so is clearer than
     relying on the hash function tolerating it, even though it now does. */
  if (p == NULL)
    return 0;
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
  if (t != LUA_TFUNCTION && t != LUA_TTABLE && t != LUA_TLIGHTUSERDATA) {
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

  /*
  ** dtask.c's driver body. It is a C function that appears on the *stack* of every
  ** parked agent -- it is the outermost frame's function -- so a snapshot of one
  ** cannot be taken without a name for it. Unlike 'queue.wait', which the module
  ** walk above already found under that name, the driver is deliberately not
  ** exposed to the guest, so nothing reaches it except this.
  */
  diluvium_task_pushbody(L);
  ds_perm_put(L, nameidx, validx, "dtask.body");

  /*
  ** 'queue.wait''s park marker: a light userdata whose address is a static in
  ** dqueue.c, pushed onto a parked thread's stack so a host can tell this yield
  ** from an ordinary 'coroutine.yield'. Naming it is what makes an agent parked
  ** on 'wait' capturable at all -- 10.7's blanket refusal of light userdata would
  ** otherwise refuse the one shape hibernation is built for.
  */
  diluvium_queue_pushmark(L);
  ds_perm_put(L, nameidx, validx, "dqueue.parkmark");

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
static void ds_buildheader (lua_State *L, const diluvium_snap_opts *opts,
                            const char *payload, size_t plen) {
  char fp[DILUVIUM_SHA256_HEX];
  char perm[DILUVIUM_SHA256_HEX];
  lua_Integer id = 0;
  lua_Integer n = 0;
  diluvium_snap_fingerprint(L, fp);
  ds_permanents(L, perm);
  lua_createtable(L, 0, 7);
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
  /* Instructions consumed so far, so a budget spans residencies (9.4). Written
     even when zero: a field that is sometimes absent is a field every reader
     has to have an opinion about. */
  lua_pushinteger(L, (opts != NULL) ? (lua_Integer)opts->insn_used : 0);
  lua_setfield(L, -2, "insn_used");
  /* The payload digest. Empty when the caller has no payload yet -- a bare
     'diluvium_snap_header' is used by tests and by a host that only wants to
     report its runtime identity. */
  if (payload != NULL) {
    unsigned char d[DILUVIUM_SHA256_SIZE];
    char hex[DILUVIUM_SHA256_HEX];
    diluvium_sha256(payload, plen, d);
    diluvium_sha256_hex(d, hex);
    lua_pushstring(L, hex);
  }
  else
    lua_pushliteral(L, "");
  lua_setfield(L, -2, "payload");
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
  return diluvium_snap_headerfor(L, opts, NULL, 0, out, cap);
}


LUA_API size_t diluvium_snap_headerfor (lua_State *L,
                                        const diluvium_snap_opts *opts,
                                        const char *payload, size_t plen,
                                        char *out, size_t cap) {
  int base = lua_gettop(L);
  size_t len;
  const char *s;
  ds_buildheader(L, opts, payload, plen);
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
  if (rc == DILUVIUM_SNAP_ACCEPT) {
    /* The instruction count must be a non-negative integer. Checked here so
       that a header this function accepts is one 'diluvium_snap_headerusage'
       can read -- a caller acting on an acceptance should not meet a second
       opinion from the reader. */
    if (lua_getfield(L, hdr, "insn_used") != LUA_TNUMBER ||
        !lua_isinteger(L, -1) || lua_tointeger(L, -1) < 0)
      rc = DILUVIUM_SNAP_BAD_HEADER;
    lua_pop(L, 1);
  }
  /*
  ** The payload digest, when the header carries one and there is a payload to
  ** check. Last, because every other refusal says something more specific: a
  ** corrupt payload under a foreign runtime is a foreign runtime first.
  */
  if (rc == DILUVIUM_SNAP_ACCEPT && used != NULL && *used < len) {
    if (lua_getfield(L, hdr, "payload") == LUA_TSTRING) {
      const char *want = lua_tostring(L, -1);
      if (*want != '\0') {
        unsigned char d[DILUVIUM_SHA256_SIZE];
        char hex[DILUVIUM_SHA256_HEX];
        diluvium_sha256(s + *used, len - *used, d);
        diluvium_sha256_hex(d, hex);
        if (strcmp(hex, want) != 0)
          rc = DILUVIUM_SNAP_CORRUPT;
      }
    }
    lua_pop(L, 1);
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
    case DILUVIUM_SNAP_BAD_PAYLOAD:
      return "the header matched but the payload after it could not be read";
    case DILUVIUM_SNAP_CORRUPT:
      return "the payload is not the bytes the header was written for, so the "
             "snapshot has been truncated, altered or corrupted in transit";
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


LUA_API int diluvium_snap_headerusage (lua_State *L, const char *s, size_t len,
                                       uint64_t *insn_used) {
  int base = lua_gettop(L);
  int ok = 0;
  lua_pushcfunction(L, ds_decodeheader_trampoline);
  lua_pushlstring(L, s, len);
  if (lua_pcall(L, 1, 2, 0) != LUA_OK) {
    lua_settop(L, base);
    return 0;
  }
  lua_pop(L, 1);                                /* the consumed count */
  if (lua_istable(L, -1) &&
      lua_getfield(L, -1, "insn_used") == LUA_TNUMBER &&
      lua_isinteger(L, -1) && lua_tointeger(L, -1) >= 0) {
    if (insn_used != NULL)
      *insn_used = (uint64_t)lua_tointeger(L, -1);
    ok = 1;
  }
  lua_settop(L, base);
  return ok;
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

static const char DS_PROTOS = 0;        /* hash hex -> the stripped dump */
/*
** Prototypes inlined by the encode in progress. Separate from DS_PROTOS, and the
** separation is the point: what a *host* registered is durable and makes a
** snapshot reference code the target is expected to have, while what this stream
** happened to inline is true only of this stream. Conflating them made taking a
** snapshot quietly register its own prototypes, so the next one referenced code no
** other runtime had.
*/
static const char DS_INLINED = 0;

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


/* Look 'hex' up in one of the two tables. Pushes the dump if found. */
static int ds_lookupin (lua_State *L, const void *key, const char *hex) {
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, key) != LUA_TTABLE) {
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


/* The host's registry: what a reference in a snapshot may name. */
static int ds_lookupproto (lua_State *L, const char *hex) {
  return ds_lookupin(L, &DS_PROTOS, hex);
}


/* This stream's: what has already been inlined and may be referenced within it. */
static int ds_lookupinlined (lua_State *L, const char *hex) {
  return ds_lookupin(L, &DS_INLINED, hex);
}


static void ds_markinlined (lua_State *L, const char *hex, int dumpidx) {
  int abs = lua_absindex(L, dumpidx);
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DS_INLINED) != LUA_TTABLE) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &DS_INLINED);
  }
  lua_pushvalue(L, abs);
  lua_setfield(L, -2, hex);
  lua_pop(L, 1);
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
  known = ds_lookupproto(L, hex) || ds_lookupinlined(L, hex);
  if (known)
    lua_pop(L, 1);                      /* the copy that was found */
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
    /* Recorded for *this stream only*, so a second closure of the same prototype
       later in the same snapshot is a reference rather than another copy -- without
       making the snapshot depend on the target runtime having it. */
    ds_markinlined(L, hex, lua_gettop(L) - 1);
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
    /* Recorded so a later reference in this same stream resolves. */
    ds_markinlined(L, hex, lua_gettop(L));
  }
  else {
    /* 10.5: "restore requires an exact match. Mismatch is a clean refusal,
       never a best-effort load." */
    /* Either the host registered it, or this same stream inlined it earlier. The
       second case is what makes ten agents over one prototype cost one copy, and
       the decoder has to know about it too -- looking only in the host's registry
       refused a reference to a prototype the snapshot did carry. */
    if (!ds_lookupproto(L, hex) && !ds_lookupinlined(L, hex))
      return luaL_error(L, "snapshot: prototype %s is referenced but is neither "
                           "carried by this snapshot nor registered in this "
                           "runtime", hex);
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

/*
** Learn the continuations this runtime's own libraries use, by watching them.
**
** 'pcall' yields with a continuation, and that continuation is 'finishpcall' --
** a static function in lbaselib.c. Naming it from there would mean editing a
** core file that is not on the patch-series allowlist, and its address is not
** reachable any other way. So it is discovered instead: park a canary thread
** inside a 'pcall' and read the continuation off the frame the shim reports.
**
** The same trick as the fingerprint canary, and honest for the same reason. It
** names whatever this build actually uses rather than what a list of names
** assumes, so a change to which continuation 'pcall' installs is picked up
** rather than mis-restored.
**
** Nothing here can fail usefully: a build where the canary does not park simply
** learns nothing, and a snapshot that needs the name is then refused by name
** later, which is the correct outcome and not a crash.
*/
static void ds_learncont (lua_State *L, const char *src, const char *name) {
  int base = lua_gettop(L);
  lua_State *co = lua_newthread(L);
  int nres, n, i;
  if (luaL_loadstring(co, src) != LUA_OK) {
    lua_settop(L, base);
    return;
  }
  lua_resume(co, L, 0, &nres);
  if (diluvium_shim_status(co) != LUA_YIELD) {
    lua_settop(L, base);
    return;
  }
  n = diluvium_shim_framecount(co);
  for (i = 0; i < n; i++) {
    diluvium_frame f;
    if (diluvium_shim_frame(co, i, &f) && f.is_c && f.has_k && f.k != NULL) {
      diluvium_shim_addcont(name, f.k);
      break;                          /* the outermost such frame is the one */
    }
  }
  lua_settop(L, base);
}


static void ds_learnconts (lua_State *L) {
  static int done = 0;
  if (done)
    return;
  done = 1;
  ds_learncont(L, "pcall(function() coroutine.yield() end)", "baselib.pcall");
  ds_learncont(L, "xpcall(function() coroutine.yield() end, function(e) "
                  "return e end)", "baselib.xpcall");
}


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
    case LUA_TTHREAD:
      return ds_encodethread(L, abs);
    case LUA_TUSERDATA:
      /*
      ** Ext 0x05 is unimplemented, deliberately, and this is the refusal 10.7
      ** item 2 actually asks for. The message names '__persist' because that is
      ** what a host has to supply once the positive path exists.
      **
      ** Why it does not exist yet is worth writing down, because the obvious
      ** design does not work. The natural shape -- '__persist' returns a
      ** reconstructor closure, which the graph already knows how to carry -- founders
      ** on ordering. A userdata needs its position before the reconstructor is
      ** written, so that a reference to it from the reconstructor's own upvalues
      ** resolves; but on the way back the reconstructor cannot be *called* until
      ** the whole graph exists, and by then any table that referenced the
      ** userdata has already been given whatever placeholder stood at that
      ** position. A program would find a placeholder where its file handle
      ** should be, and find it silently.
      **
      ** Fixing that needs either a patch-up pass over every reference or an ext
      ** that can carry a nested graph value, and both are more than a footnote.
      ** Refusing is correct in the meantime and is what 10.7 specifies; shipping
      ** the version that breaks when a userdata is referenced twice would not be.
      */
      return luaL_error(L, "snapshot: this userdata cannot be captured -- "
                           "persisted userdata (ext 0x05, the '__persist' hook) "
                           "is not implemented, so a program that hibernates must "
                           "release its userdata first (10.7 item 2)");
    default:
      return 0;                        /* threads are handled above */
  }
}


static int ds_hook_decode (lua_State *L, int code, const unsigned char *data,
                           size_t len, void *ud) {
  (void)ud;
  switch (code) {
    case 0x06:
      return ds_decodeclosure(L, data, len);
    case 0x08:
      return ds_decodethread(L, data, len);
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


static int ds_hook_slot (lua_State *L, int idx, int i, void *ud) {
  lua_State *co = lua_tothread(L, idx);
  (void)ud;
  if (co == NULL || i < 1 || i > diluvium_shim_stacksize(co))
    return 0;
  return diluvium_shim_pushslot(co, i, L);
}


/*
** Is this upvalue open against a thread in the graph?
**
** If it is, the upvalue is an alias for one of that thread's stack slots, and a
** UPOPEN says so. The codec's ordinary UPVAL would write the slot's *value*
** instead, severing the alias -- so the parked coroutine could later assign to
** that local and the closure would not see it.
**
** 'DS_CAPTURED' is the set of threads this runtime has encoded, weak-keyed so a
** thread that dies is not held alive by it. Membership alone is not enough,
** though: what matters is whether the thread has a position in *this* graph, and
** 'diluvium_msgpack_position' is what answers that -- it returns 0 for a thread
** the current encode has not reached.
**
** One case this does not catch, stated rather than left to be found: a thread
** reachable only through the upvalue of some *other* closure that was drained
** first. The thread has no position yet at that moment, so the earlier closure
** falls back to writing a value. It needs a graph where a closure's upvalue holds
** a coroutine which another, earlier-drained closure also closes over -- narrow,
** but not impossible, and the failure is a severed alias rather than an error.
*/
static const char DS_CAPTURED = 0;      /* weak set of threads encoded so far */
static const char DS_TARGET = 0;        /* the one thread this save may capture */

static void ds_captured (lua_State *L) {
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DS_CAPTURED) != LUA_TTABLE) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_newtable(L);
    lua_pushliteral(L, "k");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &DS_CAPTURED);
  }
}


static int ds_hook_openupval (lua_State *L, int idx, int n, void *ud) {
  int base = lua_gettop(L);
  (void)ud;
  if (!diluvium_shim_upisopen(L, idx, n))
    return 0;
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DS_CAPTURED) != LUA_TTABLE) {
    lua_settop(L, base);
    return 0;                          /* no thread in this graph */
  }
  lua_pushnil(L);
  while (lua_next(L, -2) != 0) {
    lua_State *co = lua_tothread(L, -2);
    int slot = (co == NULL) ? 0 : diluvium_shim_upslot(L, idx, n, co);
    if (slot > 0) {
      lua_Integer clpos = diluvium_msgpack_position(L, idx);
      lua_Integer thpos = diluvium_msgpack_position(L, -2);
      if (clpos > 0 && thpos > 0) {
        lua_Integer ops[4];
        ops[0] = clpos;
        ops[1] = n;
        ops[2] = thpos;
        ops[3] = slot;
        diluvium_msgpack_appendfixup(L, DILUVIUM_FIX_UPOPEN, ops, 4);
        lua_settop(L, base);
        return 1;
      }
    }
    lua_pop(L, 1);                     /* the value; the key drives the walk */
  }
  lua_settop(L, base);
  return 0;
}


static int ds_hook_fixup (lua_State *L, int tag, const lua_Integer *ops,
                          int nops, void *ud) {
  (void)ud;
  (void)nops;
  switch (tag) {
    case DILUVIUM_FIX_SLOT: {
      /* stack: ... objs-resolved thread is not here; the codec gives positions,
         so the layer resolves them through the graph it is building. */
      lua_State *co;
      int base = lua_gettop(L);
      diluvium_msgpack_pushobject(L, ops[0]);
      co = lua_tothread(L, -1);
      if (co == NULL)
        return luaL_error(L, "snapshot: a slot fixup names something that is not "
                             "a thread");
      lua_pushvalue(L, base);          /* the value, above the thread */
      if (!diluvium_shim_setslot(co, (int)ops[1], L))
        return luaL_error(L, "snapshot: slot %d is outside the thread's stack",
                          (int)ops[1]);
      lua_settop(L, base);
      return 1;
    }
    case DILUVIUM_FIX_BUILD: {
      int base = lua_gettop(L);
      int r;
      diluvium_msgpack_pushobject(L, ops[0]);
      r = ds_buildthread(L, lua_gettop(L));
      lua_settop(L, base);
      return r;
    }
    case DILUVIUM_FIX_UPOPEN: {
      int base = lua_gettop(L);
      lua_State *co;
      diluvium_msgpack_pushobject(L, ops[0]);    /* the closure */
      diluvium_msgpack_pushobject(L, ops[2]);    /* the thread */
      co = lua_tothread(L, -1);
      if (co == NULL)
        return luaL_error(L, "snapshot: an upvalue fixup names something that is "
                             "not a thread");
      if (!diluvium_shim_reopenupval(L, -2, (int)ops[1], co, (int)ops[3]))
        return luaL_error(L, "snapshot: upvalue %d cannot be re-opened against "
                             "slot %d", (int)ops[1], (int)ops[3]);
      lua_settop(L, base);
      return 1;
    }
    default:
      return 0;
  }
}


LUA_API const diluvium_snap_hooks *diluvium_snap_hooks_for (lua_State *L) {
  static const diluvium_snap_hooks hooks = {
    ds_hook_begin,          /* begin */
    ds_hook_permanent,      /* permanent */
    ds_hook_encode,         /* encode */
    ds_hook_slot,           /* slot */
    ds_hook_openupval,      /* openupval */
    ds_hook_fixup,          /* fixup */
    ds_hook_finish,         /* finish */
    ds_hook_decode,         /* decode */
    NULL                    /* ud */
  };
  diluvium_snap_permanents(L);
  ds_learnconts(L);
  return &hooks;
}


/* ======================================================================
** Threads (10.3): capture and rebuild a suspended coroutine
** ====================================================================== */

/*
** Named C continuations. See dsnap.h for why this is an array and not a table.
*/
#define DS_MAXCONT	64

typedef struct ds_cont { const char *name; lua_KFunction k; } ds_cont;
static ds_cont ds_conts[DS_MAXCONT];
static int ds_ncont = 0;

LUA_API int diluvium_snap_addcont (const char *name, lua_KFunction k) {
  int i;
  if (name == NULL || k == NULL || ds_ncont >= DS_MAXCONT)
    return 0;
  for (i = 0; i < ds_ncont; i++) {
    if (strcmp(ds_conts[i].name, name) == 0)
      return (ds_conts[i].k == k);   /* idempotent, but not a silent rebind */
  }
  ds_conts[ds_ncont].name = name;
  ds_conts[ds_ncont].k = k;
  ds_ncont++;
  return 1;
}

static const char *ds_contname (lua_KFunction k) {
  int i;
  for (i = 0; i < ds_ncont; i++)
    if (ds_conts[i].k == k) return ds_conts[i].name;
  return NULL;
}

static lua_KFunction ds_contfunc (const char *name, size_t len) {
  int i;
  for (i = 0; i < ds_ncont; i++) {
    if (strlen(ds_conts[i].name) == len &&
        memcmp(ds_conts[i].name, name, len) == 0)
      return ds_conts[i].k;
  }
  return NULL;
}


/*
** Ext 0x08: a suspended thread.
**
** 0x08 comes from 5.5's "Diluvium core, unassigned" range; the draft's registry
** had no code for a thread, which is an omission rather than a decision -- 10.3
** requires one thread to be captured.
**
** The payload is the frame metadata and nothing else: sizes, then one record per
** frame, then the to-be-closed slots, then the continuation names. The stack
** *values* are not here. They are graph values -- a slot can hold a table that
** holds the thread -- so they arrive as SLOT fixups, and a final BUILD fixup
** validates the frames against the filled stack and constructs the chain. That
** ordering is forced: a frame claims its function is at slot N, and nothing can
** check that until slot N holds something.
**
** Fixed 32-bit big-endian fields rather than msgpack, because this is a blob
** inside an ext payload and a self-describing encoding would buy nothing: the
** reader is this same runtime, guarded by the header's fingerprint.
*/
/*
** 2: a sixth header word carries the thread's error-handler slot and an
** eleventh frame word carries each pcall frame's saved one, so a restored
** program's error runs the handler at the throw point and gets its traceback
** (audit: old_errfunc). Rides the same release as DILUVIUM_SNAP_FORMAT 2.
*/
#define DS_THREAD_VERSION	2
#define DS_THREAD_WORDS		6        /* 32-bit words in the record header */
#define DS_FRAME_WORDS		11       /* 32-bit words per frame record */

static void ds_put32 (luaL_Buffer *b, unsigned long v) {
  char four[4];
  four[0] = (char)((v >> 24) & 0xff);
  four[1] = (char)((v >> 16) & 0xff);
  four[2] = (char)((v >> 8) & 0xff);
  four[3] = (char)(v & 0xff);
  luaL_addlstring(b, four, 4);
}

static unsigned long ds_get32 (const unsigned char *p) {
  return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
         ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

/* Signed fields go through a two's-complement round trip rather than a cast. */
static long ds_ssigned (unsigned long v) {
  return (v & 0x80000000UL) ? -(long)(~v & 0xffffffffUL) - 1 : (long)v;
}


static int ds_encodethread (lua_State *L, int abs) {
  lua_State *co = lua_tothread(L, abs);
  int base = lua_gettop(L);
  luaL_Buffer b;
  int n, i, ntbc = 0, slot, capacity;
  int code;
  if (co == NULL)
    return 0;
  if (co == L)
    return luaL_error(L, "snapshot: a thread cannot capture itself");
  /*
  ** 10.7 precondition 4: a snapshot holds a single thread (audit finding 14).
  ** The convention is that a supervisor *tells* an instance to hibernate and
  ** the instance cleans up and parks somewhere capturable, so this is a check
  ** a well-behaved program never trips -- and quietly capturing a nested
  ** coroutine was worse than refusing one, because whether it restores as the
  ** same coroutine is a promise nothing here tests or keeps.
  **
  ** Which thread is *the* thread depends on who is asking. 'diluvium_snap_save'
  ** declares its target in DS_TARGET before the walk; anything else the graph
  ** reaches is a nested coroutine. Driven without a declared target -- the
  ** codec's own entry point -- the first thread the encode meets stands for
  ** the target, and a second distinct one is the same shape by another door:
  ** DS_CAPTURED is per-stream, so it being non-empty here means another thread
  ** already went in. A *re-reference* to either never reaches this function,
  ** because the codec's seen-table writes a backreference first.
  */
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DS_TARGET) == LUA_TTHREAD) {
    lua_State *target = lua_tothread(L, -1);
    lua_pop(L, 1);
    if (co != target)
      return luaL_error(L, "snapshot: this thread is a nested coroutine, not "
                           "the thread being captured -- a snapshot holds a "
                           "single thread (10.7), so a program must let its "
                           "coroutines finish, or drop them, before it "
                           "hibernates");
  }
  else {
    lua_pop(L, 1);
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DS_CAPTURED) == LUA_TTABLE) {
      lua_pushnil(L);
      if (lua_next(L, -2) != 0)
        return luaL_error(L, "snapshot: this thread is a nested coroutine, not "
                             "the thread being captured -- a snapshot holds a "
                             "single thread (10.7), so a program must let its "
                             "coroutines finish, or drop them, before it "
                             "hibernates");
    }
    lua_settop(L, base);
  }
  /*
  ** The rest of 10.7's preconditions, applied to the one thread that may pass
  ** the gate above: it has to be capturable in its own right.
  */
  code = diluvium_shim_capturable(co, &i);
  if (code != DILUVIUM_SNAP_OK)
    return luaL_error(L, "snapshot: this thread cannot be captured: %s",
                      diluvium_shim_reason(code));
  n = diluvium_shim_framecount(co);
  if (n < 1)
    return luaL_error(L, "snapshot: a thread with no frames has nothing to "
                         "resume");
  for (slot = diluvium_shim_tbcnext(co, 0); slot != 0;
       slot = diluvium_shim_tbcnext(co, slot))
    ntbc++;
  /*
  ** How much stack the restore has to reserve, which is not the same as how many
  ** slots are live. A frame's top is 'func + 1 + maxstacksize' and sits above the
  ** live top by however many registers the frame is not currently using, so
  ** allocating only the live count leaves 'luaV_execute' reading past the end on
  ** its first instruction. Carried explicitly rather than recomputed, since the
  ** restore validates frames against it and must not derive it from them.
  */
  capacity = diluvium_shim_stacksize(co);
  for (i = 0; i < n; i++) {
    diluvium_frame f;
    if (diluvium_shim_frame(co, i, &f) && f.top_index > capacity)
      capacity = f.top_index;
  }
  luaL_buffinit(L, &b);
  {
    char head[1];
    head[0] = (char)DS_THREAD_VERSION;
    luaL_addlstring(&b, head, 1);
  }
  ds_put32(&b, (unsigned long)diluvium_shim_stacksize(co));
  ds_put32(&b, (unsigned long)diluvium_shim_nyield(co));
  ds_put32(&b, (unsigned long)n);
  ds_put32(&b, (unsigned long)ntbc);
  ds_put32(&b, (unsigned long)capacity);
  ds_put32(&b, (unsigned long)diluvium_shim_errfunc(co));
  for (i = 0; i < n; i++) {
    diluvium_frame f;
    if (!diluvium_shim_frame(co, i, &f))
      return luaL_error(L, "snapshot: frame %d of a thread vanished mid-capture",
                        i);
    ds_put32(&b, (unsigned long)(f.is_c ? 1 : 0));
    ds_put32(&b, (unsigned long)(f.has_k ? 1 : 0));
    ds_put32(&b, (unsigned long)f.func_index);
    ds_put32(&b, (unsigned long)f.top_index);
    ds_put32(&b, (unsigned long)(long)f.pc);
    ds_put32(&b, (unsigned long)f.nextraargs);
    ds_put32(&b, (unsigned long)(long)f.nresults);
    ds_put32(&b, f.callstatus);
    ds_put32(&b, (unsigned long)f.ctx);
    ds_put32(&b, (unsigned long)(f.is_vararg ? 1 : 0));
    ds_put32(&b, (unsigned long)f.old_errfunc);
  }
  /* Innermost first, which is the order 'tbcnext' walks and the reverse of the
     order a restore must apply them -- the restore reverses, because
     'luaF_newtbcupval' asserts each new slot is above the last. */
  for (slot = diluvium_shim_tbcnext(co, 0); slot != 0;
       slot = diluvium_shim_tbcnext(co, slot))
    ds_put32(&b, (unsigned long)slot);
  /* Continuation names, one per C frame that has one. */
  for (i = 0; i < n; i++) {
    diluvium_frame f;
    const char *name;
    if (!diluvium_shim_frame(co, i, &f) || !f.is_c || !f.has_k)
      continue;
    name = diluvium_shim_contname(f.k);
    if (name == NULL)
      return luaL_error(L, "snapshot: frame %d of this thread is a C function "
                           "waiting on a continuation that has no name; a host "
                           "whose call yields must name its continuation "
                           "(diluvium_snap_addcont)", i);
    ds_put32(&b, (unsigned long)strlen(name));
    luaL_addstring(&b, name);
  }
  luaL_pushresult(&b);
  {
    size_t plen;
    const char *p = lua_tolstring(L, -1, &plen);
    diluvium_msgpack_appendext(L, 0x08, p, plen);
  }
  /* Remember it, so an upvalue open against its stack is written as an alias
     rather than as a value. See ds_hook_openupval. */
  ds_captured(L);
  lua_pushvalue(L, abs);
  lua_pushboolean(L, 1);
  lua_rawset(L, -3);
  lua_settop(L, base);
  return 3;                             /* 3: queue this thread's slots */
}


/*
** A thread being rebuilt keeps its metadata here until its BUILD fixup arrives,
** keyed by the thread itself. A registry table rather than C state, because
** several threads can be part way through one restore.
*/
static const char DS_PENDING = 0;
static const char DS_BUILT = 0;

static void ds_built (lua_State *L) {
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DS_BUILT) != LUA_TTABLE) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &DS_BUILT);
  }
}


static void ds_pending (lua_State *L) {
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DS_PENDING) != LUA_TTABLE) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &DS_PENDING);
  }
}


static int ds_decodethread (lua_State *L, const unsigned char *data,
                            size_t len) {
  lua_State *co;
  unsigned long nslots, nframes;
  unsigned long capacity;
  if (len < 1 + DS_THREAD_WORDS * 4 || data[0] != DS_THREAD_VERSION)
    return luaL_error(L, "snapshot: a thread record is malformed or of a "
                         "version this runtime does not write");
  nslots = ds_get32(data + 1);
  nframes = ds_get32(data + 9);
  capacity = ds_get32(data + 17);
  if (nslots < 1 || nslots > 1000000UL || nframes < 1 || nframes > 100000UL ||
      capacity < nslots || capacity > 1000000UL)
    return luaL_error(L, "snapshot: a thread record claims %d slots and %d "
                         "frames, which is not credible",
                      (int)nslots, (int)nframes);
  /*
  ** The metadata is checked for *self*-consistency here and against the thread
  ** later, in the BUILD fixup, once the stack has values in it. Splitting it that
  ** way is what lets the length check happen before anything is allocated.
  */
  if (len < 1 + DS_THREAD_WORDS * 4 + nframes * DS_FRAME_WORDS * 4)
    return luaL_error(L, "snapshot: a thread record is shorter than the %d "
                         "frames it claims", (int)nframes);
  co = lua_newthread(L);
  if (!diluvium_shim_prepstack(co, (int)nslots, (int)capacity))
    return luaL_error(L, "snapshot: could not make room for a thread's %d "
                         "stack slots", (int)nslots);
  /* Stash the metadata against the thread until BUILD. */
  ds_pending(L);
  lua_pushvalue(L, -2);                 /* the thread */
  lua_pushlstring(L, (const char *)data, len);
  lua_rawset(L, -3);
  lua_pop(L, 1);                        /* the pending table */
  return 1;
}


/*
** BUILD: the stack is populated, so now the frames can be checked against it and
** the chain constructed. Everything that can refuse does so before
** 'diluvium_shim_restore' is reached, per 10.10.
*/
static int ds_buildthread (lua_State *L, int thidx) {
  lua_State *co = lua_tothread(L, thidx);
  const unsigned char *data;
  size_t len;
  unsigned long nslots, nyield, nframes, ntbc, errfunc;
  diluvium_frame *frames;
  lua_KFunction *ks;
  size_t namepos;
  int rc, bad = -1;
  unsigned long i;
  if (co == NULL)
    return luaL_error(L, "snapshot: a build fixup names something that is not a "
                         "thread");
  ds_pending(L);
  lua_pushvalue(L, thidx);
  if (lua_rawget(L, -2) != LUA_TSTRING)
    return luaL_error(L, "snapshot: a build fixup names a thread that has no "
                         "record, so it was built twice or never created");
  data = (const unsigned char *)lua_tolstring(L, -1, &len);
  nslots = ds_get32(data + 1);
  nyield = ds_get32(data + 5);
  nframes = ds_get32(data + 9);
  ntbc = ds_get32(data + 13);
  errfunc = ds_get32(data + 21);
  /* Sized from the record, which was length-checked when the thread was made. */
  frames = (diluvium_frame *)lua_newuserdatauv(
             L, nframes * sizeof(diluvium_frame), 0);
  ks = (lua_KFunction *)lua_newuserdatauv(L, nframes * sizeof(lua_KFunction), 0);
  namepos = 1 + DS_THREAD_WORDS * 4 + nframes * DS_FRAME_WORDS * 4 + ntbc * 4;
  if (namepos > len)
    return luaL_error(L, "snapshot: a thread record is shorter than its frame "
                         "and to-be-closed tables require");
  for (i = 0; i < nframes; i++) {
    const unsigned char *f = data + 1 + DS_THREAD_WORDS * 4 +
                             i * DS_FRAME_WORDS * 4;
    diluvium_frame *out = &frames[i];
    memset(out, 0, sizeof(*out));
    out->is_c = (ds_get32(f) != 0);
    out->has_k = (ds_get32(f + 4) != 0);
    out->func_index = (int)ds_get32(f + 8);
    out->top_index = (int)ds_get32(f + 12);
    out->pc = (int)ds_ssigned(ds_get32(f + 16));
    out->nextraargs = (int)ds_get32(f + 20);
    out->is_vararg = (ds_get32(f + 36) != 0);
    out->nresults = (int)ds_ssigned(ds_get32(f + 24));
    out->callstatus = ds_get32(f + 28);
    out->ctx = ds_ssigned(ds_get32(f + 32));
    out->old_errfunc = (long)ds_get32(f + 40);
    ks[i] = NULL;
    if (out->is_c && out->has_k) {
      unsigned long nlen;
      if (namepos + 4 > len)
        return luaL_error(L, "snapshot: a thread record ends before its "
                             "continuation names");
      nlen = ds_get32(data + namepos);
      namepos += 4;
      if (nlen > len - namepos)
        return luaL_error(L, "snapshot: a continuation name runs past the end "
                             "of a thread record");
      ks[i] = diluvium_shim_contfunc((const char *)(data + namepos),
                                     (size_t)nlen);
      if (ks[i] == NULL) {
        lua_pushlstring(L, (const char *)(data + namepos), (size_t)nlen);
        return luaL_error(L, "snapshot: continuation '%s' is named by this "
                             "snapshot but not registered in this runtime",
                          lua_tostring(L, -1));
      }
      namepos += nlen;
    }
  }
  rc = diluvium_shim_checkframes(co, frames, (int)nframes, (int)nslots,
                                 (int)errfunc, &bad);
  if (rc != DILUVIUM_RES_OK) {
    if (bad < 0)
      return luaL_error(L, "snapshot: a thread cannot be restored: %s",
                        diluvium_shim_resreason(rc));
    return luaL_error(L, "snapshot: frame %d of a thread cannot be restored: %s",
                      bad, diluvium_shim_resreason(rc));
  }
  if (!diluvium_shim_restore(co, frames, (int)nframes, ks, (int)nyield,
                             (int)errfunc))
    return luaL_error(L, "snapshot: a thread's frames would not build");
  /*
  ** The to-be-closed slots are *not* marked here, and that is the one ordering
  ** subtlety in the whole restore. Marking a slot requires its value to have a
  ** '__close' metamethod, and a metatable arrives as its own fixup -- which for a
  ** 'defer' object, a self-referential 'setmetatable(t, t)', is queued after the
  ** thread that holds it. So marking here found a table with no metatable and
  ** refused it. They are applied in 'ds_hook_finish', once the graph is whole.
  **
  ** The record therefore stays; 'DS_BUILT' records that the frames are done, so
  ** the finisher can tell a thread that was never built from one that was.
  */
  ds_built(L);
  lua_pushvalue(L, thidx);
  lua_pushboolean(L, 1);
  lua_rawset(L, -3);
  return 1;
}


static int ds_hook_begin (lua_State *L, void *ud) {
  (void)ud;
  /* Clear the per-stream inlined set. Also the captured-thread set, since it is
     only meaningful while an encode is running. */
  lua_pushnil(L);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &DS_INLINED);
  lua_pushnil(L);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &DS_CAPTURED);
  return 1;
}


static int ds_hook_finish (lua_State *L, void *ud) {
  int base = lua_gettop(L);
  (void)ud;
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DS_PENDING) != LUA_TTABLE) {
    lua_settop(L, base);
    return 1;                          /* no thread in this restore */
  }
  ds_built(L);                         /* base+2 */
  lua_pushnil(L);
  while (lua_next(L, base + 1) != 0) {
    /* stack: pending, built, thread, record */
    lua_State *co = lua_tothread(L, -2);
    size_t len;
    const unsigned char *data =
      (const unsigned char *)lua_tolstring(L, -1, &len);
    unsigned long nframes, ntbc, i;
    lua_pushvalue(L, -2);
    if (lua_rawget(L, base + 2) == LUA_TNIL)
      return luaL_error(L, "snapshot: a thread was created but its frames were "
                           "never built, so it would restore unrunnable");
    lua_pop(L, 1);
    if (co == NULL || data == NULL)
      return luaL_error(L, "snapshot: a pending thread entry is malformed");
    nframes = ds_get32(data + 9);
    ntbc = ds_get32(data + 13);
    /* Bottom-up: the record holds them innermost first, and
       'luaF_newtbcupval' requires each new slot to sit above the last. */
    for (i = ntbc; i > 0; i--) {
      unsigned long slot =
        ds_get32(data + 1 + DS_THREAD_WORDS * 4 +
                 nframes * DS_FRAME_WORDS * 4 + (i - 1) * 4);
      if (!diluvium_shim_settbc(co, (int)slot))
        return luaL_error(L, "snapshot: %s (slot %d)",
                          diluvium_shim_resreason(DILUVIUM_RES_TBC), (int)slot);
    }
    lua_pop(L, 1);                     /* the record; the thread drives on */
  }
  /* Clear both tables, so a second restore starts clean. */
  lua_settop(L, base);
  lua_pushnil(L);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &DS_PENDING);
  lua_pushnil(L);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &DS_BUILT);
  return 1;
}


/* ======================================================================
** Whole-instance save and restore
** ====================================================================== */

LUA_API int diluvium_snap_addpermanent (lua_State *L, const char *name) {
  int base = lua_gettop(L);
  int nameidx, validx, ok;
  if (name == NULL || base < 1)
    return 0;
  diluvium_snap_permanents(L);
  lua_rawgetp(L, LUA_REGISTRYINDEX, &DS_PERM_NAME);
  lua_rawgetp(L, LUA_REGISTRYINDEX, &DS_PERM_VALUE);
  if (!lua_istable(L, -2) || !lua_istable(L, -1)) {
    lua_settop(L, base - 1);
    return 0;
  }
  nameidx = base + 1;
  validx = base + 2;
  lua_pushstring(L, name);
  ok = (lua_rawget(L, validx) == LUA_TNIL);
  lua_pop(L, 1);
  if (ok) {
    lua_pushvalue(L, base);            /* the value the caller pushed */
    ds_perm_put(L, nameidx, validx, name);
  }
  lua_settop(L, base - 1);             /* drop the tables and the value */
  /*
  ** The permanents fingerprint has to be recomputed, and letting it be
  ** recomputed is the point: a host that names an extra function has a different
  ** permanents set, and a snapshot taken under one set must be refused by the
  ** other. Dropping the cache is what makes that happen rather than something to
  ** remember.
  */
  lua_pushnil(L);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &DS_PERM_FP);
  return ok;
}


/* The root of an instance snapshot: the thread, plus the queue subsystem. */
static void ds_buildroot (lua_State *L, int thidx) {
  lua_createtable(L, 0, 2);
  lua_pushvalue(L, thidx);
  lua_setfield(L, -2, "thread");
  diluvium_queue_pushstate(L);
  lua_setfield(L, -2, "queues");
}


/* The graph encode, shaped for 'lua_pcall': the root is at 1, the encoded
   stream is the one result. Protection exists so DS_TARGET clears on every
   exit path; see the caller. */
static int ds_savegraph (lua_State *L) {
  diluvium_msgpack_encode_graph(L, 1, diluvium_snap_hooks_for(L));
  return 1;
}


LUA_API void diluvium_snap_save (lua_State *L, int thidx,
                                 const diluvium_snap_opts *opts) {
  int abs = lua_absindex(L, thidx);
  int base = lua_gettop(L);
  char head[8192];
  size_t hlen;
  luaL_Buffer b;
  if (lua_type(L, abs) != LUA_TTHREAD)
    luaL_error(L, "snapshot: dv_snapshot needs a thread, not a %s",
               luaL_typename(L, abs));
  /* The header is built first and *after* the permanents exist, because its
     fingerprint covers them -- and a host that named its own functions changes
     that fingerprint. */
  diluvium_snap_permanents(L);
  /*
  ** Declare the target, so the walk can tell *the* thread from a nested
  ** coroutine it reaches (10.7 precondition 4; the check lives in
  ** 'ds_encodethread'). The encode runs under protection so the slot clears
  ** on the refusal path too: this function raises through to a caller, and a
  ** target left behind by a refused save would make the next *directly
  ** driven* encode in this state -- a test, typically -- apply the wrong
  ** thread's identity check.
  */
  lua_pushvalue(L, abs);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &DS_TARGET);
  ds_buildroot(L, abs);
  lua_pushcfunction(L, ds_savegraph);
  lua_insert(L, -2);
  {
    int rc = lua_pcall(L, 1, 1, 0);
    lua_pushnil(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &DS_TARGET);
    if (rc != LUA_OK)
      lua_error(L);                     /* the message is on top; re-raise */
  }
  /*
  ** The graph's index is taken from 'lua_gettop' rather than computed. Both
  ** guesses at computing it were wrong -- 'encode_graph' leaves its result above
  ** the value it was given, not in its place -- and the symptom was a zero-length
  ** graph appended to a valid header, which the restore reported as "truncated
  ** input" and looked for all the world like a codec bug.
  **
  ** Read before 'luaL_buffinit', too. That pushes a placeholder in Lua 5.5, so
  ** afterwards -1 is the placeholder and not the string.
  */
  {
    int gidx = lua_gettop(L);
    size_t glen;
    const char *g = lua_tolstring(L, gidx, &glen);
    if (g == NULL)
      luaL_error(L, "snapshot: the graph did not encode to a string");
    /* The header is written *after* the graph, because it carries the graph's
       digest. Queues are read for the name list before either, and nothing
       between here and there can change them. */
    hlen = diluvium_snap_headerfor(L, opts, g, glen, head, sizeof(head));
    if (hlen == 0)
      luaL_error(L, "snapshot: the header does not fit in %d bytes, which means "
                    "an implausible number of queues", (int)sizeof(head));
    luaL_buffinit(L, &b);
    luaL_addlstring(&b, head, hlen);
    luaL_addlstring(&b, g, glen);
  }
  luaL_pushresult(&b);
  lua_replace(L, base + 1);
  lua_settop(L, base + 1);
}


/* The protected half of 'load'; see there for why this is split. */
static int ds_loadbody (lua_State *L) {
  size_t len, used;
  const char *s = lua_tolstring(L, 1, &len);
  used = (size_t)lua_tointeger(L, 2);
  diluvium_msgpack_decode_graph(L, s + used, len - used,
                                diluvium_snap_hooks_for(L));
  if (!lua_istable(L, -1))
    return luaL_error(L, "snapshot: the payload is not an instance record");
  if (lua_getfield(L, -1, "thread") != LUA_TTHREAD)
    return luaL_error(L, "snapshot: the payload has no thread");
  if (lua_getfield(L, -2, "queues") != LUA_TTABLE)
    return luaL_error(L, "snapshot: the payload has no queue state");
  if (!diluvium_queue_setstate(L, -1))
    return luaL_error(L, "snapshot: this instance already has queues, so a "
                         "restore would merge two handle spaces");
  lua_pop(L, 1);                       /* the queue state */
  return 1;                            /* the thread */
}


LUA_API int diluvium_snap_load (lua_State *L, const diluvium_snap_opts *opts,
                                const char *s, size_t len,
                                const char **out_msg) {
  int base = lua_gettop(L);
  size_t used = 0;
  int rc;
  if (out_msg != NULL) *out_msg = NULL;
  diluvium_snap_permanents(L);
  rc = diluvium_snap_checkheader(L, opts, s, len, &used);
  if (rc != DILUVIUM_SNAP_ACCEPT) {
    if (out_msg != NULL) *out_msg = diluvium_snap_why(rc);
    return rc;
  }
  /*
  ** The payload runs under protection and the header does not, because they fail
  ** differently. A bad header is a mismatch with a code and a sentence; a bad
  ** payload is anything at all, and 10.10 requires that anything at all produce a
  ** refusal rather than a crash. Nothing in here may raise past this point.
  */
  lua_pushcfunction(L, ds_loadbody);
  lua_pushlstring(L, s, len);
  lua_pushinteger(L, (lua_Integer)used);
  if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
    if (out_msg != NULL) {
      const char *m = lua_tostring(L, -1);
      *out_msg = (m != NULL) ? m : "snapshot: the payload was refused";
    }
    /* The message is left on the stack so the pointer stays valid until the
       caller's next operation, which is the same contract 'dv_last_error' has. */
    return DILUVIUM_SNAP_BAD_PAYLOAD;
  }
  /*
  ** The result is already at 'base + 1' -- 'lua_pcall' left its single return
  ** value where the function was -- so there is nothing to move. A
  ** 'lua_replace(L, base + 1)' here pops the top and stores it at its own index,
  ** which discards it; the following 'settop' then padded with nil and the caller
  ** got a NULL thread. Trimming is all that is wanted.
  */
  lua_settop(L, base + 1);
  return DILUVIUM_SNAP_ACCEPT;
}
