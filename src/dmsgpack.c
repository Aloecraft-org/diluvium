/*
** dmsgpack.c
** Diluvium msgpack codec. See dmsgpack.h.
**
** Provenance. Derived from lua-cmsgpack 0.4.0 by Salvatore Sanfilippo, MIT
** licensed; the notice is at the foot of this file and applies. What was kept
** is the part worth keeping and the part easiest to get subtly wrong: the
** format-byte tables, the fixint/uint8..64 and int8..64 ladders, the string
** and collection header widths, and the runtime endianness swap.
**
** What was rewritten, and why, since a reader comparing the two will want to
** know it was deliberate:
**
**   The buffer. Upstream's grows via the raw allocator and calls 'abort()' on
**   overflow, and it is freed by hand -- so any error raised mid-encode
**   long-jumps past the free and leaks. Upstream avoids that with a 'pcall'
**   wrapper that returns nil plus a message; 5.4 of the design says errors
**   are raised, like the standard library, so that wrapper is gone and the
**   buffer is a userdata whose '__gc' frees it however the encode ends.
**
**   Integer decoding. Upstream builds int32 as '((int32_t)p[1] << 24) | ...',
**   which is signed overflow whenever the top byte is >= 0x80 -- undefined
**   behaviour, and this repository runs UBSan over the suite in CI. Every
**   width is accumulated unsigned here and converted once at the end.
**
**   Floats. Upstream narrows to float32 when the value survives the round
**   trip. 5.2 of the design says integers encode as msgpack int and floats as
**   float64, so that a value's encoded width is a property of its Lua type
**   rather than of its magnitude. Decoding still accepts float32, since other
**   encoders emit it.
**
**   Array-versus-map. Upstream treats a table as an array when its keys are
**   positive integers and max == count, which makes the empty table an array.
**   5.3 says the empty table is a map, and names the rule exactly, so the
**   rule is implemented as written rather than inherited.
**
** New here: the ext registry (5.5), 'as_array'/'as_map' (5.3), 'msgpack.ext',
** the resolver seam (4.2), and the key path in a non-encodable value's error
** message (5.6).
*/

#define dmsgpack_c

#include "lprefix.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "dmsgpack.h"


/*
** Nesting cap. In the plain codec this is also the only thing standing
** between a cyclic table and an unbounded recursion, which is why 5.2 keeps
** it: the error it produces is the clean refusal a cycle is required to get.
** The snapshot encoder handles cycles properly, with backreferences, and does
** not rely on this.
*/
#define MP_MAX_DEPTH		16

/* Registry keys. Addresses of these are the keys, so they cannot collide. */
static const char MP_SHAPE_MT = 0;      /* metatable shared by all wrappers */
static const char MP_RESOLVER = 0;      /* light userdata -> resolver */

/* Wrapper kinds, stored at index 2 of a wrapper table. */
#define MP_SHAPE_ARRAY	1
#define MP_SHAPE_MAP	2
#define MP_SHAPE_EXT	3


/*
** Render a byte as 0xNN. 'luaL_error' formats through Lua, which understands
** %s, %d and %I but not printf's width or radix flags -- a "%02x" there is
** copied out literally, which is how this was found.
*/
static const char *mp_hexbyte (int b) {
  static const char digits[] = "0123456789abcdef";
  static char out[5];
  out[0] = '0'; out[1] = 'x';
  out[2] = digits[(b >> 4) & 0xf];
  out[3] = digits[b & 0xf];
  out[4] = '\0';
  return out;
}


/* ======================================================================
** Endianness
** ====================================================================== */

/*
** Reverse a block on a little-endian host. Only floats need it; every other
** width is written byte by byte. Checked at runtime rather than by macro
** because the build system has no endianness probe and the cost is noise
** beside the memcpy it accompanies.
*/
static void mp_memrevifle (void *ptr, size_t len) {
  unsigned char *p = (unsigned char *)ptr;
  unsigned char *e = p + len - 1;
  int test = 1;
  if (*(unsigned char *)&test == 0)
    return;  /* big endian: nothing to do */
  len /= 2;
  while (len--) {
    unsigned char aux = *p;
    *p++ = *e;
    *e-- = aux;
  }
}


/* ======================================================================
** Encode buffer
** ====================================================================== */

typedef struct mp_buf {
  lua_State *L;
  unsigned char *b;
  size_t len, cap;
} mp_buf;


static int mp_buf_gc (lua_State *L) {
  mp_buf *buf = (mp_buf *)lua_touserdata(L, 1);
  if (buf->b != NULL) {
    void *ud;
    lua_Alloc allocf = lua_getallocf(L, &ud);
    allocf(ud, buf->b, buf->cap, 0);
    buf->b = NULL;
    buf->cap = buf->len = 0;
  }
  return 0;
}


/*
** Push a fresh buffer as a userdata and return it. Living on the stack is the
** whole point: an error raised anywhere inside the encode unwinds to the
** caller and the collector frees the bytes, with no cleanup path to forget.
*/
static mp_buf *mp_buf_new (lua_State *L) {
  mp_buf *buf = (mp_buf *)lua_newuserdatauv(L, sizeof(mp_buf), 0);
  buf->L = L;
  buf->b = NULL;
  buf->len = buf->cap = 0;
  if (luaL_newmetatable(L, "diluvium.msgpack.buf")) {
    lua_pushcfunction(L, mp_buf_gc);
    lua_setfield(L, -2, "__gc");
  }
  lua_setmetatable(L, -2);
  return buf;
}


static void mp_buf_add (mp_buf *buf, const void *s, size_t len) {
  if (buf->cap - buf->len < len) {
    void *ud;
    lua_Alloc allocf = lua_getallocf(buf->L, &ud);
    size_t want = buf->len + len;
    size_t cap = (buf->cap != 0) ? buf->cap : 64;
    unsigned char *nb;
    if (want < buf->len)  /* wrapped */
      luaL_error(buf->L, "msgpack: encoded value too large");
    while (cap < want) {
      if (cap > (size_t)-1 / 2)
        luaL_error(buf->L, "msgpack: encoded value too large");
      cap *= 2;
    }
    nb = (unsigned char *)allocf(ud, buf->b, buf->cap, cap);
    if (nb == NULL)  /* raise rather than abort; '__gc' still owns buf->b */
      luaL_error(buf->L, "msgpack: not enough memory to encode");
    buf->b = nb;
    buf->cap = cap;
  }
  memcpy(buf->b + buf->len, s, len);
  buf->len += len;
}


static void mp_buf_byte (mp_buf *buf, unsigned char c) {
  mp_buf_add(buf, &c, 1);
}


/* Append 'n' in 'width' bytes, most significant first. */
static void mp_buf_be (mp_buf *buf, uint64_t n, int width) {
  unsigned char b[8];
  int i;
  for (i = width - 1; i >= 0; i--) {
    b[i] = (unsigned char)(n & 0xff);
    n >>= 8;
  }
  mp_buf_add(buf, b, (size_t)width);
}


/* ======================================================================
** Primitive encoders
** ====================================================================== */

static void mp_enc_nil (mp_buf *buf) { mp_buf_byte(buf, 0xc0); }

static void mp_enc_bool (mp_buf *buf, int b) {
  mp_buf_byte(buf, b ? 0xc3 : 0xc2);
}


static void mp_enc_int (mp_buf *buf, lua_Integer i) {
  if (i >= 0) {
    uint64_t n = (uint64_t)i;
    if (n <= 127) mp_buf_byte(buf, (unsigned char)n);          /* fixint */
    else if (n <= 0xffU) { mp_buf_byte(buf, 0xcc); mp_buf_be(buf, n, 1); }
    else if (n <= 0xffffU) { mp_buf_byte(buf, 0xcd); mp_buf_be(buf, n, 2); }
    else if (n <= 0xffffffffU) { mp_buf_byte(buf, 0xce); mp_buf_be(buf, n, 4); }
    else { mp_buf_byte(buf, 0xcf); mp_buf_be(buf, n, 8); }
  }
  else {
    /* Cast through uint64_t before shifting: negating LUA_MININTEGER or
       shifting a negative value are both traps we do not need. */
    uint64_t n = (uint64_t)i;
    if (i >= -32) mp_buf_byte(buf, (unsigned char)(0xe0 | (i + 32)));
    else if (i >= -128) { mp_buf_byte(buf, 0xd0); mp_buf_be(buf, n, 1); }
    else if (i >= -32768) { mp_buf_byte(buf, 0xd1); mp_buf_be(buf, n, 2); }
    else if (i >= -2147483647LL - 1) {
      mp_buf_byte(buf, 0xd2); mp_buf_be(buf, n, 4);
    }
    else { mp_buf_byte(buf, 0xd3); mp_buf_be(buf, n, 8); }
  }
}


/*
** Always float64. See the provenance note: the encoded width follows the Lua
** type, not the magnitude, so a float round-trips as a float and two equal
** floats always encode identically.
*/
static void mp_enc_float (mp_buf *buf, lua_Number d) {
  double v = (double)d;
  unsigned char b[8];
  mp_buf_byte(buf, 0xcb);
  memcpy(b, &v, 8);
  mp_memrevifle(b, 8);
  mp_buf_add(buf, b, 8);
}


static void mp_enc_str (mp_buf *buf, const char *s, size_t len) {
  if (len < 32) mp_buf_byte(buf, (unsigned char)(0xa0 | len));
  else if (len <= 0xffU) { mp_buf_byte(buf, 0xd9); mp_buf_be(buf, len, 1); }
  else if (len <= 0xffffU) { mp_buf_byte(buf, 0xda); mp_buf_be(buf, len, 2); }
  else if (len <= 0xffffffffU) {
    mp_buf_byte(buf, 0xdb); mp_buf_be(buf, len, 4);
  }
  else
    luaL_error(buf->L, "msgpack: string too long to encode");
  mp_buf_add(buf, s, len);
}


static void mp_enc_array_hdr (mp_buf *buf, size_t n) {
  if (n <= 15) mp_buf_byte(buf, (unsigned char)(0x90 | n));
  else if (n <= 0xffffU) { mp_buf_byte(buf, 0xdc); mp_buf_be(buf, n, 2); }
  else if (n <= 0xffffffffU) {
    mp_buf_byte(buf, 0xdd); mp_buf_be(buf, n, 4);
  }
  else
    luaL_error(buf->L, "msgpack: array too long to encode");
}


static void mp_enc_map_hdr (mp_buf *buf, size_t n) {
  if (n <= 15) mp_buf_byte(buf, (unsigned char)(0x80 | n));
  else if (n <= 0xffffU) { mp_buf_byte(buf, 0xde); mp_buf_be(buf, n, 2); }
  else if (n <= 0xffffffffU) {
    mp_buf_byte(buf, 0xdf); mp_buf_be(buf, n, 4);
  }
  else
    luaL_error(buf->L, "msgpack: map too long to encode");
}


static void mp_enc_ext (mp_buf *buf, int code, const char *data, size_t len) {
  if (len == 1) mp_buf_byte(buf, 0xd4);
  else if (len == 2) mp_buf_byte(buf, 0xd5);
  else if (len == 4) mp_buf_byte(buf, 0xd6);
  else if (len == 8) mp_buf_byte(buf, 0xd7);
  else if (len == 16) mp_buf_byte(buf, 0xd8);
  else if (len <= 0xffU) { mp_buf_byte(buf, 0xc7); mp_buf_be(buf, len, 1); }
  else if (len <= 0xffffU) { mp_buf_byte(buf, 0xc8); mp_buf_be(buf, len, 2); }
  else if (len <= 0xffffffffU) {
    mp_buf_byte(buf, 0xc9); mp_buf_be(buf, len, 4);
  }
  else
    luaL_error(buf->L, "msgpack: ext payload too long to encode");
  mp_buf_byte(buf, (unsigned char)code);
  mp_buf_add(buf, data, len);
}


/* ======================================================================
** Key paths, for the diagnostic 5.6 asks for
** ====================================================================== */

/*
** The message a programmer gets when their state is not serializable is the
** main diagnostic this codec produces, so it names where the offending value
** sits and not only its type. The trail is bounded by the nesting cap, so a
** fixed array is enough and there is nothing to allocate on the error path.
*/
typedef struct mp_path {
  int kind;             /* 0 unused, 1 integer index, 2 string key, 3 other */
  lua_Integer i;
  const char *s;
  size_t slen;
} mp_path;

typedef struct mp_ctx {
  mp_buf *buf;
  mp_path path[MP_MAX_DEPTH + 1];
  int depth;
} mp_ctx;


static void mp_path_push (lua_State *L, mp_ctx *ctx, int keyidx) {
  mp_path *p = &ctx->path[ctx->depth - 1];
  switch (lua_type(L, keyidx)) {
    case LUA_TNUMBER:
      if (lua_isinteger(L, keyidx)) {
        p->kind = 1;
        p->i = lua_tointeger(L, keyidx);
        return;
      }
      break;  /* a float key is not an index; describe it as other */
    case LUA_TSTRING:
      p->kind = 2;
      p->s = lua_tolstring(L, keyidx, &p->slen);
      return;
    default:
      break;
  }
  p->kind = 3;
}


/* Render the trail as something a programmer can find in their source. */
static void mp_path_render (lua_State *L, mp_ctx *ctx, luaL_Buffer *b) {
  int i;
  int wrote = 0;
  for (i = 0; i < ctx->depth; i++) {
    mp_path *p = &ctx->path[i];
    switch (p->kind) {
      case 1:
        lua_pushfstring(L, "[%I]", (lua_Integer)p->i);
        luaL_addvalue(b);
        wrote = 1;
        break;
      case 2:
        if (wrote) luaL_addchar(b, '.');
        luaL_addlstring(b, p->s, p->slen);
        wrote = 1;
        break;
      default:
        luaL_addstring(b, "[?]");
        wrote = 1;
        break;
    }
  }
  if (!wrote)
    luaL_addstring(b, "(the value itself)");
}


static int mp_err_unencodable (lua_State *L, mp_ctx *ctx, int idx) {
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  luaL_addstring(&b, "msgpack: cannot encode a ");
  luaL_addstring(&b, luaL_typename(L, idx));
  luaL_addstring(&b, " value at ");
  mp_path_render(L, ctx, &b);
  luaL_pushresult(&b);
  return lua_error(L);
}


/* ======================================================================
** Shape wrappers: as_array, as_map, ext
** ====================================================================== */

/* Push the shared wrapper metatable, creating it once per state. */
static void mp_shape_mt (lua_State *L) {
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &MP_SHAPE_MT) != LUA_TTABLE) {
    lua_pop(L, 1);
    lua_createtable(L, 0, 1);
    lua_pushliteral(L, "diluvium.msgpack.shape");
    lua_setfield(L, -2, "__name");
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &MP_SHAPE_MT);
  }
}


/*
** Is the value at 'idx' one of our wrappers? Returns the kind, or 0. Compares
** the metatable by identity against the registry copy, so an ordinary table
** carrying a lookalike field is not mistaken for one.
*/
static int mp_shape_of (lua_State *L, int idx) {
  int kind = 0;
  if (!lua_istable(L, idx))
    return 0;
  if (!lua_getmetatable(L, idx))
    return 0;
  mp_shape_mt(L);
  if (lua_rawequal(L, -1, -2)) {
    lua_rawgeti(L, idx, 2);
    kind = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
  }
  lua_pop(L, 2);
  return kind;
}


static int mp_as_array (lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_createtable(L, 2, 0);
  lua_pushvalue(L, 1);
  lua_rawseti(L, -2, 1);
  lua_pushinteger(L, MP_SHAPE_ARRAY);
  lua_rawseti(L, -2, 2);
  mp_shape_mt(L);
  lua_setmetatable(L, -2);
  return 1;
}


static int mp_as_map (lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_createtable(L, 2, 0);
  lua_pushvalue(L, 1);
  lua_rawseti(L, -2, 1);
  lua_pushinteger(L, MP_SHAPE_MAP);
  lua_rawseti(L, -2, 2);
  mp_shape_mt(L);
  lua_setmetatable(L, -2);
  return 1;
}


/*
** msgpack.ext(code, data). Not in 5.4's list, and added because 5.5 requires
** decoding an application-range code to surface its bytes to the program --
** which is only half a feature if the program cannot produce one in reply.
*/
static int mp_ext_new (lua_State *L) {
  lua_Integer code = luaL_checkinteger(L, 1);
  size_t len;
  luaL_checklstring(L, 2, &len);
  if (code < 0x10 || code > 0x7f)
    return luaL_error(L, "msgpack: ext code %I is reserved; the range free "
                         "for application use is 0x10 to 0x7F", code);
  lua_createtable(L, 3, 0);
  lua_pushvalue(L, 2);
  lua_rawseti(L, -2, 1);
  lua_pushinteger(L, MP_SHAPE_EXT);
  lua_rawseti(L, -2, 2);
  lua_pushinteger(L, code);
  lua_rawseti(L, -2, 3);
  mp_shape_mt(L);
  lua_setmetatable(L, -2);
  return 1;
}


/* Push an ext value for the decoder to hand back to the program. */
static void mp_ext_push (lua_State *L, int code, const char *data, size_t len) {
  lua_createtable(L, 3, 0);
  lua_pushlstring(L, data, len);
  lua_rawseti(L, -2, 1);
  lua_pushinteger(L, MP_SHAPE_EXT);
  lua_rawseti(L, -2, 2);
  lua_pushinteger(L, code);
  lua_rawseti(L, -2, 3);
  mp_shape_mt(L);
  lua_setmetatable(L, -2);
}


/* ======================================================================
** Array versus map, exactly as 5.3 states it
** ====================================================================== */

/*
** A table encodes as an array if and only if it has at least one element, its
** keys form a dense integer sequence 1..n, and it has no other keys.
** Everything else, the empty table included, is a map.
**
** Counting is enough to establish density: keys in a table are distinct, so if
** every key is an integer in 1..count then those keys are exactly 1..count.
*/
static int mp_is_array (lua_State *L, int idx, size_t *out_n) {
  size_t count = 0;
  lua_Integer max = 0;
  luaL_checkstack(L, 3, "msgpack: cannot inspect table shape");
  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    lua_pop(L, 1);  /* drop the value; the key drives the walk */
    if (!lua_isinteger(L, -1)) {
      lua_pop(L, 1);
      *out_n = 0;
      return 0;
    }
    else {
      lua_Integer k = lua_tointeger(L, -1);
      if (k <= 0) {
        lua_pop(L, 1);
        *out_n = 0;
        return 0;
      }
      if (k > max) max = k;
      count++;
    }
  }
  *out_n = count;
  /* 'at least one element' is the count test; density is max == count. */
  return count > 0 && max == (lua_Integer)count;
}


/* ======================================================================
** Recursive encode
** ====================================================================== */

static void mp_encode_value (lua_State *L, mp_ctx *ctx, int idx);


static void mp_encode_array_body (lua_State *L, mp_ctx *ctx, int idx,
                                  size_t n) {
  size_t i;
  mp_enc_array_hdr(ctx->buf, n);
  for (i = 1; i <= n; i++) {
    luaL_checkstack(L, 2, "msgpack: nesting too deep to encode");
    lua_rawgeti(L, idx, (lua_Integer)i);
    ctx->path[ctx->depth - 1].kind = 1;
    ctx->path[ctx->depth - 1].i = (lua_Integer)i;
    mp_encode_value(L, ctx, lua_gettop(L));
    lua_pop(L, 1);
  }
}


static void mp_encode_map_body (lua_State *L, mp_ctx *ctx, int idx) {
  size_t n = 0;
  int abs = lua_absindex(L, idx);
  /* A msgpack map is length-prefixed and a header cannot be patched once
     written, so walk once to count and again to write. Two walks is cheaper
     than buffering the body separately to learn its length. */
  luaL_checkstack(L, 3, "msgpack: cannot walk table to encode");
  lua_pushnil(L);
  while (lua_next(L, abs) != 0) {
    lua_pop(L, 1);
    n++;
  }
  mp_enc_map_hdr(ctx->buf, n);
  lua_pushnil(L);
  while (lua_next(L, abs) != 0) {
    /* stack: ... key value */
    int vi = lua_gettop(L);
    int ki = vi - 1;
    mp_path_push(L, ctx, ki);
    /* A key is encoded at the same depth as the map itself: it is not a step
       further into the structure, and naming it in a path would be circular. */
    mp_encode_value(L, ctx, ki);
    mp_encode_value(L, ctx, vi);
    lua_pop(L, 1);  /* drop value, keep key for the next step */
  }
}


static void mp_encode_table (lua_State *L, mp_ctx *ctx, int idx, int forced) {
  size_t n;
  int abs = lua_absindex(L, idx);
  if (ctx->depth >= MP_MAX_DEPTH)
    luaL_error(L, "msgpack: table nesting deeper than %d, or a cycle",
               MP_MAX_DEPTH);
  ctx->depth++;
  if (forced == MP_SHAPE_ARRAY) {
    n = (size_t)luaL_len(L, abs);
    mp_encode_array_body(L, ctx, abs, n);
  }
  else if (forced == MP_SHAPE_MAP)
    mp_encode_map_body(L, ctx, abs);
  else if (mp_is_array(L, abs, &n))
    mp_encode_array_body(L, ctx, abs, n);
  else
    mp_encode_map_body(L, ctx, abs);
  ctx->depth--;
  ctx->path[ctx->depth].kind = 0;  /* leave no stale trail behind */
}


static void mp_encode_value (lua_State *L, mp_ctx *ctx, int idx) {
  int abs = lua_absindex(L, idx);
  switch (lua_type(L, abs)) {
    case LUA_TNIL:
      mp_enc_nil(ctx->buf);
      return;
    case LUA_TBOOLEAN:
      mp_enc_bool(ctx->buf, lua_toboolean(L, abs));
      return;
    case LUA_TNUMBER:
      if (lua_isinteger(L, abs))
        mp_enc_int(ctx->buf, lua_tointeger(L, abs));
      else
        mp_enc_float(ctx->buf, lua_tonumber(L, abs));
      return;
    case LUA_TSTRING: {
      size_t len;
      const char *s = lua_tolstring(L, abs, &len);
      mp_enc_str(ctx->buf, s, len);
      return;
    }
    case LUA_TTABLE: {
      int kind = mp_shape_of(L, abs);
      if (kind == MP_SHAPE_EXT) {
        size_t len;
        const char *data;
        int code;
        lua_rawgeti(L, abs, 3);
        code = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_rawgeti(L, abs, 1);
        data = lua_tolstring(L, -1, &len);
        mp_enc_ext(ctx->buf, code, data, len);
        lua_pop(L, 1);
        return;
      }
      if (kind == MP_SHAPE_ARRAY || kind == MP_SHAPE_MAP) {
        lua_rawgeti(L, abs, 1);  /* the wrapped table */
        if (!lua_istable(L, -1))
          luaL_error(L, "msgpack: shape marker does not wrap a table");
        mp_encode_table(L, ctx, lua_gettop(L), kind);
        lua_pop(L, 1);
        return;
      }
      mp_encode_table(L, ctx, abs, 0);
      return;
    }
    default:
      break;
  }
  /* Anything else -- function, thread, userdata, light userdata -- is the
     resolver's last chance, then an error naming the type and the path. */
  {
    const diluvium_msgpack_resolver *r;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &MP_RESOLVER);
    r = (const diluvium_msgpack_resolver *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (r != NULL && r->encode != NULL && r->encode(L, abs, r->ud))
      return;
  }
  mp_err_unencodable(L, ctx, abs);
}


/* ======================================================================
** Decode
** ====================================================================== */

typedef struct mp_cur {
  lua_State *L;
  const unsigned char *p;
  size_t left;
} mp_cur;


static void mp_need (mp_cur *c, size_t n) {
  if (c->left < n)
    luaL_error(c->L, "msgpack: truncated input");
}


static uint64_t mp_get_be (mp_cur *c, int width) {
  uint64_t n = 0;
  int i;
  mp_need(c, (size_t)width);
  for (i = 0; i < width; i++)
    n = (n << 8) | c->p[i];
  c->p += width;
  c->left -= (size_t)width;
  return n;
}


/*
** A msgpack uint64 can exceed what a Lua integer holds. Wrapping it silently
** would make a round trip lie, so refuse by name; nothing this codec emits
** can produce one, so it only arises from a foreign encoder.
*/
static lua_Integer mp_to_integer (mp_cur *c, uint64_t n, int is_signed,
                                  int width) {
  if (is_signed) {
    /* Sign-extend from 'width' bytes without shifting a signed value. */
    if (width < 8) {
      uint64_t sign = (uint64_t)1 << (width * 8 - 1);
      if (n & sign)
        n |= ~((sign << 1) - 1);
    }
    return (lua_Integer)n;
  }
  if (n > (uint64_t)LUA_MAXINTEGER)
    luaL_error(c->L, "msgpack: unsigned value %I does not fit a Lua integer",
               (lua_Integer)(n >> 1));  /* halved so the message stays signed */
  return (lua_Integer)n;
}


static void mp_decode_value (mp_cur *c, int depth);


static void mp_decode_array (mp_cur *c, size_t n, int depth) {
  lua_State *L = c->L;
  size_t i;
  luaL_checkstack(L, 3, "msgpack: nesting too deep to decode");
  lua_createtable(L, (int)((n > (size_t)INT_MAX) ? 0 : n), 0);
  for (i = 1; i <= n; i++) {
    mp_decode_value(c, depth + 1);
    lua_rawseti(L, -2, (lua_Integer)i);
  }
}


static void mp_decode_map (mp_cur *c, size_t n, int depth) {
  lua_State *L = c->L;
  size_t i;
  luaL_checkstack(L, 4, "msgpack: nesting too deep to decode");
  lua_createtable(L, 0, (int)((n > (size_t)INT_MAX) ? 0 : n));
  for (i = 0; i < n; i++) {
    mp_decode_value(c, depth + 1);  /* key */
    if (lua_isnil(L, -1))
      luaL_error(L, "msgpack: a map key cannot be nil");
    mp_decode_value(c, depth + 1);  /* value */
    lua_rawset(L, -3);
  }
}


/*
** Ext, per the registry in 5.5. The reserved codes fail by name, because a
** generic "unknown ext" tells a programmer nothing about which of several
** unfinished features they have just met.
*/
static void mp_decode_ext (mp_cur *c, int code, size_t len) {
  lua_State *L = c->L;
  const char *data;
  mp_need(c, len);
  data = (const char *)c->p;
  c->p += len;
  c->left -= len;
  if (code >= 0x10 && code <= 0x7f) {  /* application range: hand it over */
    mp_ext_push(L, code, data, len);
    return;
  }
  switch (code) {
    case 0x01:
      luaL_error(L, "msgpack: ext 0x01 is a decimal value; decQuad is not "
                    "implemented yet");
      return;
    case 0x02: {  /* endpoint reference: the resolver's, if there is one */
      const diluvium_msgpack_resolver *r;
      lua_rawgetp(L, LUA_REGISTRYINDEX, &MP_RESOLVER);
      r = (const diluvium_msgpack_resolver *)lua_touserdata(L, -1);
      lua_pop(L, 1);
      if (r != NULL && r->decode != NULL &&
          r->decode(L, (const unsigned char *)data, len, r->ud))
        return;
      /* No resolver: surface it opaquely rather than fail, so a
         single-instance embedder never links the swarm layer. See 4.2. */
      mp_ext_push(L, code, data, len);
      return;
    }
    case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
      luaL_error(L, "msgpack: ext %s is only valid inside a snapshot stream",
                 mp_hexbyte(code));
      return;
    default:
      luaL_error(L, "msgpack: ext %s is reserved for Diluvium and not assigned",
                 mp_hexbyte(code));
      return;
  }
}


static void mp_decode_value (mp_cur *c, int depth) {
  lua_State *L = c->L;
  unsigned char t;
  if (depth > MP_MAX_DEPTH)
    luaL_error(L, "msgpack: nesting deeper than %d", MP_MAX_DEPTH);
  luaL_checkstack(L, 2, "msgpack: cannot grow the stack to decode");
  mp_need(c, 1);
  t = c->p[0];
  c->p++;
  c->left--;

  if (t <= 0x7f) { lua_pushinteger(L, t); return; }            /* +fixint */
  if (t >= 0xe0) {                                             /* -fixint */
    lua_pushinteger(L, (lua_Integer)(signed char)t);
    return;
  }
  if ((t & 0xe0) == 0xa0) {                                    /* fixstr */
    size_t len = t & 0x1f;
    mp_need(c, len);
    lua_pushlstring(L, (const char *)c->p, len);
    c->p += len; c->left -= len;
    return;
  }
  if ((t & 0xf0) == 0x90) { mp_decode_array(c, t & 0x0f, depth); return; }
  if ((t & 0xf0) == 0x80) { mp_decode_map(c, t & 0x0f, depth); return; }

  switch (t) {
    case 0xc0: lua_pushnil(L); return;
    case 0xc2: lua_pushboolean(L, 0); return;
    case 0xc3: lua_pushboolean(L, 1); return;

    case 0xcc: lua_pushinteger(L, mp_to_integer(c, mp_get_be(c, 1), 0, 1)); return;
    case 0xcd: lua_pushinteger(L, mp_to_integer(c, mp_get_be(c, 2), 0, 2)); return;
    case 0xce: lua_pushinteger(L, mp_to_integer(c, mp_get_be(c, 4), 0, 4)); return;
    case 0xcf: lua_pushinteger(L, mp_to_integer(c, mp_get_be(c, 8), 0, 8)); return;

    case 0xd0: lua_pushinteger(L, mp_to_integer(c, mp_get_be(c, 1), 1, 1)); return;
    case 0xd1: lua_pushinteger(L, mp_to_integer(c, mp_get_be(c, 2), 1, 2)); return;
    case 0xd2: lua_pushinteger(L, mp_to_integer(c, mp_get_be(c, 4), 1, 4)); return;
    case 0xd3: lua_pushinteger(L, mp_to_integer(c, mp_get_be(c, 8), 1, 8)); return;

    case 0xca: {  /* float 32: accepted on decode, never emitted */
      float f;
      mp_need(c, 4);
      memcpy(&f, c->p, 4);
      mp_memrevifle(&f, 4);
      c->p += 4; c->left -= 4;
      lua_pushnumber(L, (lua_Number)f);
      return;
    }
    case 0xcb: {
      double d;
      mp_need(c, 8);
      memcpy(&d, c->p, 8);
      mp_memrevifle(&d, 8);
      c->p += 8; c->left -= 8;
      lua_pushnumber(L, (lua_Number)d);
      return;
    }

    case 0xc4: case 0xd9: {  /* bin 8 / str 8 */
      size_t len = (size_t)mp_get_be(c, 1);
      mp_need(c, len);
      lua_pushlstring(L, (const char *)c->p, len);
      c->p += len; c->left -= len;
      return;
    }
    case 0xc5: case 0xda: {  /* bin 16 / str 16 */
      size_t len = (size_t)mp_get_be(c, 2);
      mp_need(c, len);
      lua_pushlstring(L, (const char *)c->p, len);
      c->p += len; c->left -= len;
      return;
    }
    case 0xc6: case 0xdb: {  /* bin 32 / str 32 */
      size_t len = (size_t)mp_get_be(c, 4);
      mp_need(c, len);
      lua_pushlstring(L, (const char *)c->p, len);
      c->p += len; c->left -= len;
      return;
    }

    case 0xdc: mp_decode_array(c, (size_t)mp_get_be(c, 2), depth); return;
    case 0xdd: mp_decode_array(c, (size_t)mp_get_be(c, 4), depth); return;
    case 0xde: mp_decode_map(c, (size_t)mp_get_be(c, 2), depth); return;
    case 0xdf: mp_decode_map(c, (size_t)mp_get_be(c, 4), depth); return;

    /* fixext 1/2/4/8/16 */
    case 0xd4: case 0xd5: case 0xd6: case 0xd7: case 0xd8: {
      size_t len = (size_t)1 << (t - 0xd4);
      int code;
      mp_need(c, 1);
      code = c->p[0];
      c->p++; c->left--;
      mp_decode_ext(c, code, len);
      return;
    }
    case 0xc7: case 0xc8: case 0xc9: {  /* ext 8/16/32 */
      int width = (t == 0xc7) ? 1 : (t == 0xc8) ? 2 : 4;
      size_t len = (size_t)mp_get_be(c, width);
      int code;
      mp_need(c, 1);
      code = c->p[0];
      c->p++; c->left--;
      mp_decode_ext(c, code, len);
      return;
    }

    default:
      luaL_error(L, "msgpack: byte %s is not a valid msgpack type",
                 mp_hexbyte(t));
  }
}


/* ======================================================================
** Lua API
** ====================================================================== */

static int mp_encode (lua_State *L) {
  mp_ctx ctx;
  int i;
  luaL_checkany(L, 1);
  if (lua_gettop(L) != 1)
    return luaL_error(L, "msgpack.encode takes exactly one value");
  memset(&ctx, 0, sizeof(ctx));
  for (i = 0; i <= MP_MAX_DEPTH; i++)
    ctx.path[i].kind = 0;
  ctx.depth = 0;
  ctx.buf = mp_buf_new(L);  /* on the stack: freed by '__gc' however we exit */
  mp_encode_value(L, &ctx, 1);
  lua_pushlstring(L, (const char *)ctx.buf->b, ctx.buf->len);
  return 1;
}


static int mp_decode (lua_State *L) {
  mp_cur c;
  size_t len;
  const char *s = luaL_checklstring(L, 1, &len);
  int wants_next = (lua_gettop(L) >= 2 && !lua_isnil(L, 2));
  lua_Integer offset = luaL_optinteger(L, 2, 1);
  if (offset < 1 || (size_t)offset > len + 1)
    return luaL_error(L, "msgpack.decode: offset %I is outside the string",
                      offset);
  c.L = L;
  c.p = (const unsigned char *)s + (offset - 1);
  c.left = len - (size_t)(offset - 1);
  mp_decode_value(&c, 0);
  if (wants_next) {
    /* 5.4: decode(str, offset) also returns where the next value starts.
       Decided before decoding, which pushes into slot 2 itself. */
    lua_pushinteger(L, (lua_Integer)(len - c.left) + 1);
    return 2;
  }
  return 1;
}


LUA_API void diluvium_msgpack_encode (lua_State *L, int idx) {
  mp_ctx ctx;
  int abs = lua_absindex(L, idx);
  int base;
  memset(&ctx, 0, sizeof(ctx));
  ctx.depth = 0;
  ctx.buf = mp_buf_new(L);
  base = lua_gettop(L);  /* the buffer sits here and must be dropped after */
  mp_encode_value(L, &ctx, abs);
  lua_pushlstring(L, (const char *)ctx.buf->b, ctx.buf->len);
  lua_remove(L, base);  /* drop the buffer, leaving only the string */
}


LUA_API void diluvium_msgpack_decode (lua_State *L, const char *s, size_t len) {
  mp_cur c;
  c.L = L;
  c.p = (const unsigned char *)s;
  c.left = len;
  mp_decode_value(&c, 0);
}


LUA_API void diluvium_msgpack_setresolver (lua_State *L,
                                    const diluvium_msgpack_resolver *r) {
  if (r == NULL)
    lua_pushnil(L);
  else
    lua_pushlightuserdata(L, (void *)r);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &MP_RESOLVER);
}


static const luaL_Reg mp_lib[] = {
  {"encode", mp_encode},
  {"decode", mp_decode},
  {"as_array", mp_as_array},
  {"as_map", mp_as_map},
  {"ext", mp_ext_new},
  {NULL, NULL}
};


LUAMOD_API int luaopen_dmsgpack (lua_State *L) {
  luaL_newlib(L, mp_lib);
  return 1;
}


/******************************************************************************
* The format-byte tables and the endianness swap in this file derive from
* lua-cmsgpack, whose notice follows and applies to those parts.
*
* Copyright (C) 2012 Salvatore Sanfilippo.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
******************************************************************************/
