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


/*
** The permanents fingerprint.
**
** 10.4 wants a hash of the permanents table, and the permanents table is the
** next milestone. The mechanism is here now and computes over whatever the
** permanents registry holds -- today nothing, so this is the hash of the empty
** set. Written this way rather than left out so the field, the comparison and
** its refusal are all exercised before the content arrives; when the table is
** built, this starts returning a different value and every snapshot taken
** before it is correctly refused.
*/
static void ds_permanents (lua_State *L, char *out) {
  diluvium_sha256_ctx h;
  unsigned char digest[DILUVIUM_SHA256_SIZE];
  (void)L;
  diluvium_sha256_init(&h);
  diluvium_sha256_update(&h, "diluvium-permanents-0\0", 22);
  diluvium_sha256_final(&h, digest);
  diluvium_sha256_hex(digest, out);
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
