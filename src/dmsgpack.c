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

/*
** Snapshot mode gets its own, much larger cap, because the reason the plain
** one is 16 does not apply: cycles are handled by backreferences, so this is
** only a bound on C recursion. And 16 would be far too shallow for real
** program state -- a linked list of twenty nodes is twenty levels deep, and
** refusing to hibernate an agent for holding one would be absurd.
**
** 150 is below Lua's own LUAI_MAXCCALLS of 200, so a graph that would overflow
** the C stack here is refused by this check first, with a message that says
** what happened.
*/
#define MP_SNAP_MAX_DEPTH	150

/* The deepest level whose key is recorded for an error path. Beyond this the
   path is truncated: it is a diagnostic, and a 100-element key trail helps
   nobody. */
#define MP_PATH_DEPTH		MP_MAX_DEPTH

/* Registry keys. Addresses of these are the keys, so they cannot collide. */
static const char MP_SHAPE_MT = 0;      /* metatable shared by all wrappers */
static const char MP_RESOLVER = 0;      /* light userdata -> resolver */
static const char MP_CURRENT = 0;       /* light userdata -> encode in progress */
static const char MP_CURDEC = 0;        /* light userdata -> decode in progress */

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

/*
** Snapshot-mode encode state.
**
** 'seen' maps each table already written to its 1-based position in the object
** stream, which is what an ext 0x04 backreference names. Position is assigned
** when a table is first *encountered*, before its contents are written, and the
** decoder assigns positions the same way -- so both sides number the graph in
** the same pre-order walk without either transmitting the numbers.
**
** 'fix' is a flat worklist of three slots per entry: tag, and two operands whose
** meaning depends on the tag. A metatable is queued as (META, owner, metatable);
** a closure's upvalue as (UPVAL, closure, index), with the value fetched at
** drain time rather than stored, since the closure still holds it.
**
** Neither a metatable nor an upvalue is written inline, and for the same reason:
** it is not part of its owner's contents, so writing it inline would put it in
** the numbering at a place the decoder cannot predict -- the decoder has to
** create the owner before it can know whether anything follows. They go in a
** trailing section and get their positions there. The list grows while that
** section is being written, since a metatable may have a metatable and an
** upvalue may be a table with one.
**
** 'upown' and 'upidx' map an upvalue's identity ('lua_upvalueid', a pointer) to
** the closure position and index that first claimed it. A later closure sharing
** it emits UPJOIN instead of UPVAL, which is the whole of 10.3's upvalue
** identity requirement.
*/
typedef struct mp_snap {
  int seen;                            /* stack index: object -> position */
  int fix;                             /* stack index: flat fixup worklist */
  int upown;                           /* stack index: upvalue id -> position */
  int upidx;                           /* stack index: upvalue id -> index */
  int nwritten;                        /* fixup entries already emitted */
  unsigned long n;                     /* positions assigned so far */
  const diluvium_snap_hooks *hooks;
} mp_snap;

typedef struct mp_ctx {
  mp_buf *buf;
  mp_path path[MP_PATH_DEPTH + 1];
  int depth;
  mp_snap *snap;                       /* NULL in the plain codec */
} mp_ctx;


#define mp_maxdepth(ctx)  \
	((ctx)->snap != NULL ? MP_SNAP_MAX_DEPTH : MP_MAX_DEPTH)


static void mp_path_push (lua_State *L, mp_ctx *ctx, int keyidx) {
  mp_path *p;
  if (ctx->depth < 1 || ctx->depth > MP_PATH_DEPTH)
    return;  /* deeper than the path is recorded; see MP_PATH_DEPTH */
  p = &ctx->path[ctx->depth - 1];
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
  int upto = (ctx->depth < MP_PATH_DEPTH) ? ctx->depth : MP_PATH_DEPTH;
  for (i = 0; i < upto; i++) {
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


/*
** The same shape for anything else the encoder refuses. 5.6 makes the key path
** part of the contract rather than a nicety, on the grounds that this message
** is the whole diagnostic a programmer gets when their state will not
** serialize -- and that argument does not stop applying just because the reason
** is something other than the type.
*/
static int mp_err_at (lua_State *L, mp_ctx *ctx, const char *what) {
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  luaL_addstring(&b, "msgpack: ");
  luaL_addstring(&b, what);
  luaL_addstring(&b, " at ");
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


/* Append one fixup to the worklist: tag, then two operands from the stack. */
static void mp_snap_queue (lua_State *L, mp_snap *s, int tag) {
  /* stack: ... operand1 operand2 */
  lua_Integer k = (lua_Integer)lua_rawlen(L, s->fix);
  lua_rawseti(L, s->fix, k + 3);
  lua_rawseti(L, s->fix, k + 2);
  lua_pushinteger(L, tag);
  lua_rawseti(L, s->fix, k + 1);
}


/*
** Give an object its position in the object stream, or write a backreference to
** the position it already has.
**
** Returns 1 when a backreference was written and the caller must not encode the
** object, 0 when it is new and the caller should carry on. Assigning the
** position *before* the contents are written is what makes a cycle work: a table
** that contains itself meets its own position on the way down, and so does a
** closure whose upvalue holds the table that holds the closure.
**
** Works for any collectable value, not only tables, because a closure needs
** exactly the same treatment: two references to one closure must still be one
** closure after a round trip.
*/
static int mp_snap_intern (lua_State *L, mp_ctx *ctx, int abs) {
  mp_snap *s = ctx->snap;
  luaL_checkstack(L, 4, "msgpack: cannot track object identity");
  lua_pushvalue(L, abs);
  if (lua_rawget(L, s->seen) != LUA_TNIL) {
    unsigned long pos = (unsigned long)lua_tointeger(L, -1);
    unsigned char be[4];
    lua_pop(L, 1);
    be[0] = (unsigned char)((pos >> 24) & 0xff);
    be[1] = (unsigned char)((pos >> 16) & 0xff);
    be[2] = (unsigned char)((pos >> 8) & 0xff);
    be[3] = (unsigned char)(pos & 0xff);
    mp_enc_ext(ctx->buf, 0x04, (const char *)be, 4);
    return 1;
  }
  lua_pop(L, 1);
  if (s->n >= 0xffffffffUL)
    luaL_error(L, "msgpack: too many distinct objects for a snapshot");
  s->n++;
  lua_pushvalue(L, abs);
  lua_pushinteger(L, (lua_Integer)s->n);
  lua_rawset(L, s->seen);
  /* Queue the metatable rather than writing it here. See mp_snap. */
  if (lua_getmetatable(L, abs)) {     /* +metatable */
    lua_pushvalue(L, abs);            /* +owner */
    lua_insert(L, -2);                /* owner, metatable */
    mp_snap_queue(L, s, DILUVIUM_FIX_META);
  }
  return 0;
}


/*
** Queue every stack slot of the thread at 'abs', then a build. The build must be
** last, because building the frames validates them against the slots -- a frame
** claims its function is at slot N, and that can only be checked once slot N
** holds something.
*/
static void mp_snap_queuethread (lua_State *L, mp_snap *s, int abs) {
  int i;
  for (i = 1; ; i++) {
    if (!s->hooks->slot(L, abs, i, s->hooks->ud))
      break;
    lua_pop(L, 1);                    /* the value; fetched again at drain */
    lua_pushvalue(L, abs);
    lua_pushinteger(L, i);
    mp_snap_queue(L, s, DILUVIUM_FIX_SLOT);
  }
  lua_pushvalue(L, abs);
  lua_pushinteger(L, 0);
  mp_snap_queue(L, s, DILUVIUM_FIX_BUILD);
}


/* Queue every upvalue of the Lua closure at 'abs' for filling. */
static void mp_snap_queueupvals (lua_State *L, mp_snap *s, int abs) {
  int i;
  for (i = 1; lua_getupvalue(L, abs, i) != NULL; i++) {
    lua_pop(L, 1);                    /* the value; fetched again at drain */
    lua_pushvalue(L, abs);
    lua_pushinteger(L, i);
    mp_snap_queue(L, s, DILUVIUM_FIX_UPVAL);
  }
}


/*
** Offer a value to the resolver's encode hook (4.2), and write what it hands
** back. Returns 1 when the hook claimed the value.
**
** The hook returns bytes rather than appending them: it pushes an ext code and a
** payload string, and the codec writes the object. That direction was chosen
** deliberately. The encode buffer is a stack userdata whose '__gc' owns it, and
** handing a hook a door into the encode in progress -- which is what the
** snapshot layer's 'appendext' is -- means that door has to be published in a
** registry slot, and an error raised mid-encode unwinds past whatever would
** clear it. Returning the bytes has no such slot to leave dangling, and it makes
** the hook's job the part it actually knows: what this value is, not how msgpack
** frames it.
**
** Not consulted in snapshot mode. A snapshot has its own hooks, with interning
** and positions that an ext written behind their back would not participate in;
** ext 0x02 in a snapshot is a separate question and 10.7 does not answer it yet.
*/
static int mp_resolver_encode (lua_State *L, mp_ctx *ctx, int abs) {
  const diluvium_msgpack_resolver *r;
  int code;
  size_t len;
  const char *data;
  if (ctx->snap != NULL)
    return 0;
  lua_rawgetp(L, LUA_REGISTRYINDEX, &MP_RESOLVER);
  r = (const diluvium_msgpack_resolver *)lua_touserdata(L, -1);
  lua_pop(L, 1);
  if (r == NULL || r->encode == NULL)
    return 0;
  luaL_checkstack(L, 3, "msgpack: cannot offer a value to the resolver");
  if (!r->encode(L, abs, r->ud))
    return 0;
  /* The hook said yes, so from here a malformed answer is the host's bug and an
     error naming it is better than bytes nobody can decode. */
  if (!lua_isinteger(L, -2) || lua_type(L, -1) != LUA_TSTRING) {
    lua_pop(L, 2);
    luaL_error(L, "msgpack: the resolver claimed a value but did not return an "
                  "ext code and a payload string");
  }
  code = (int)lua_tointeger(L, -2);
  data = lua_tolstring(L, -1, &len);
  mp_enc_ext(ctx->buf, code, data, len);
  lua_pop(L, 2);
  return 1;
}


static void mp_encode_array_body (lua_State *L, mp_ctx *ctx, int idx,
                                  size_t n) {
  size_t i;
  mp_enc_array_hdr(ctx->buf, n);
  for (i = 1; i <= n; i++) {
    luaL_checkstack(L, 2, "msgpack: nesting too deep to encode");
    lua_rawgeti(L, idx, (lua_Integer)i);
    if (ctx->depth >= 1 && ctx->depth <= MP_PATH_DEPTH) {
      ctx->path[ctx->depth - 1].kind = 1;
      ctx->path[ctx->depth - 1].i = (lua_Integer)i;
    }
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
  if (ctx->depth >= mp_maxdepth(ctx)) {
    if (ctx->snap != NULL)
      luaL_error(L, "msgpack: table nesting deeper than %d in a snapshot",
                 MP_SNAP_MAX_DEPTH);
    else
      luaL_error(L, "msgpack: table nesting deeper than %d, or a cycle",
                 MP_MAX_DEPTH);
  }
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
  if (ctx->depth <= MP_PATH_DEPTH)
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
      if (ctx->snap != NULL) {
        /* A shape wrapper is an encoding directive, not program state, and its
           hidden metatable is shared runtime furniture that must not end up in
           a snapshot's object graph. Refusing is the honest answer: silently
           unwrapping would restore a plain table where the program left a
           wrapper, which is a difference it can see. */
        if (kind != 0)
          mp_err_at(L, ctx, "a snapshot cannot contain a msgpack shape "
                            "wrapper (as_array, as_map or ext)");
        /* Permanents first, and before interning. '_G' is a table; an encoder
           that interned it would drag the whole global environment into the
           snapshot, every C function in it included. */
        if (ctx->snap->hooks != NULL && ctx->snap->hooks->permanent != NULL &&
            ctx->snap->hooks->permanent(L, abs, ctx->snap->hooks->ud))
          return;
        if (mp_snap_intern(L, ctx, abs))
          return;
        mp_encode_table(L, ctx, abs, 0);
        return;
      }
      if (kind == MP_SHAPE_EXT) {
        size_t len;
        const char *data;
        lua_Integer code;
        lua_rawgeti(L, abs, 3);
        code = lua_tointeger(L, -1);
        lua_pop(L, 1);
        /*
        ** Checked here and not only in 'msgpack.ext', because the wrapper is an
        ** ordinary table and a guest can assign to it: 'w = msgpack.ext(0x10, s)'
        ** then 'w[3] = 0x02' produced a reserved code from a constructor that had
        ** just refused one. That mattered little while nothing read those codes;
        ** with an endpoint reference now encoding as ext 0x02, it would have been
        ** a way to write bytes that a host's own trusted delivery path resolves
        ** into a reference to any peer it has authorised.
        **
        ** The check belongs at the point the bytes are written rather than at the
        ** point the object is made. That is where the guarantee has to hold, and
        ** a constructor cannot hold it for a value that stays mutable.
        */
        if (code < 0x10 || code > 0x7f)
          mp_err_at(L, ctx, "an ext wrapper whose code is outside the "
                            "application range 0x10 to 0x7F");
        lua_rawgeti(L, abs, 1);
        if (lua_type(L, -1) != LUA_TSTRING)
          mp_err_at(L, ctx, "an ext wrapper whose payload is not a string");
        data = lua_tolstring(L, -1, &len);
        mp_enc_ext(ctx->buf, (int)code, data, len);
        lua_pop(L, 1);
        return;
      }
      /*
      ** A table with a metatable may be something the host understands better
      ** than the codec does -- an endpoint reference is exactly that: 7.3 gives
      ** it a private metatable so a guest can neither build one nor look inside,
      ** and the consequence was that it encoded as a plain one-element array. A
      ** router could not hand a handler its destination, which is the shape 9.1
      ** is written around.
      **
      ** Only tables carrying a metatable are offered, so an ordinary table pays
      ** one 'lua_getmetatable' and nothing else. Shape wrappers are settled above
      ** first: they carry a metatable too, and they are the codec's own.
      */
      if (kind == 0 && lua_getmetatable(L, abs)) {
        lua_pop(L, 1);
        if (mp_resolver_encode(L, ctx, abs))
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
  if (mp_resolver_encode(L, ctx, abs))
    return;
  if (ctx->snap != NULL) {
    /* A named object -- a C function, most often -- takes no position, because
       it is not part of the object graph. Asked before the light-userdata
       refusal below, and the order matters: see that comment. */
    if (ctx->snap->hooks != NULL && ctx->snap->hooks->permanent != NULL &&
        ctx->snap->hooks->permanent(L, abs, ctx->snap->hooks->ud))
      return;
    /*
    ** 10.7 item 3 refuses light userdata, and the reason is worth saying here
    ** rather than only in the document: a light userdata is a bare pointer. There
    ** is no type to consult, no metatable to carry a '__persist' hook, and
    ** nothing that makes the address mean the same thing in the process that
    ** restores it.
    **
    ** But "anywhere reachable" was too absolute, and the runtime's own wait
    ** protocol proved it: 'queue.wait' pushes a light userdata sentinel onto the
    ** thread's stack before yielding, so a parked agent -- the thing hibernation
    ** exists for -- always has one. A light userdata the *runtime* owns can be
    ** named like any other permanent, and the name resolves to the same sentinel
    ** in the new process. Only an unnamed one is impossible, which is why the
    ** permanent check runs first.
    */
    if (lua_type(L, abs) == LUA_TLIGHTUSERDATA)
      mp_err_at(L, ctx, "a snapshot cannot contain light userdata that the "
                        "runtime has not named (a bare pointer has no type and "
                        "no way to be reconstituted)");
    /* Everything else that gets here is a graph object and takes a position
       before the hook writes it, so that a reference back to it -- from one of
       its own upvalues, which is ordinary for a recursive local function --
       resolves to a backreference rather than recursing. */
    if (ctx->snap->hooks != NULL && ctx->snap->hooks->encode != NULL) {
      int r;
      if (mp_snap_intern(L, ctx, abs))
        return;
      r = ctx->snap->hooks->encode(L, abs, ctx->snap->hooks->ud);
      if (r == 2) {
        mp_snap_queueupvals(L, ctx->snap, abs);
        return;
      }
      if (r == 3) {
        if (ctx->snap->hooks->slot == NULL)
          mp_err_at(L, ctx, "the snapshot layer claimed a thread but offers no "
                            "way to read its stack");
        mp_snap_queuethread(L, ctx->snap, abs);
        return;
      }
      if (r == 1)
        return;
      /* The hook declined after a position was spent. That leaves a hole in the
         numbering, which does not matter because the error below abandons the
         whole encode -- but it does mean a caller must not swallow that error
         and keep the buffer. Falling through gives the message that names the
         type and the key path, which is the more useful one. */
    }
  }
  mp_err_unencodable(L, ctx, abs);
}


/* ======================================================================
** Decode
** ====================================================================== */

/*
** Snapshot-mode decode state. 'objs' is the mirror of the encoder's 'seen': an
** array whose Nth slot is the Nth object the stream mentioned. A table is
** registered as soon as it exists and before its contents are read, which is
** the same pre-order rule the encoder uses -- so the two agree on numbering
** without the numbers being on the wire, and a cycle resolves because the
** backreference inside a table finds the table itself already registered.
*/
typedef struct mp_unsnap {
  int objs;                            /* stack index: position -> object */
  unsigned long n;                     /* positions filled so far */
  const diluvium_snap_hooks *hooks;
} mp_unsnap;

typedef struct mp_cur {
  lua_State *L;
  const unsigned char *p;
  size_t left;
  mp_unsnap *snap;                     /* NULL in the plain codec */
  /*
  ** Are these bytes the host's, or the guest's own string?
  **
  ** Only the host's may mint an endpoint reference. 7.3 says a program "receives
  ** one in a message and never builds one", and that has to be enforced by where
  ** the bytes came from, because the bytes themselves carry no authority -- ext
  ** 0x02's payload is a name a guest can guess or simply try. Set on the queue
  ** delivery path and on snapshot restore; clear in guest-callable
  ** 'msgpack.decode'.
  */
  int trusted;
} mp_cur;


/*
** Register the table on top of the stack as the next object. Leaves it on top.
*/
static void mp_unsnap_register (mp_cur *c) {
  lua_State *L = c->L;
  mp_unsnap *s = c->snap;
  s->n++;
  lua_pushvalue(L, -1);
  lua_rawseti(L, s->objs, (lua_Integer)s->n);
}


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


/*
** How large a table to pre-size for a claimed element count.
**
** Never the claim itself. Every element costs at least one byte of input, so a
** container promising more elements than there are bytes left cannot be satisfied
** by any input, and sizing for the claim just allocates on a stranger's word:
** measured, the five bytes 'dd 08 00 00 00' -- an array header claiming 134 million
** elements -- made the decoder allocate 131 MB before reading a single element, a
** 26,000-fold amplification on a path that then failed with "truncated input". The
** extreme claim of 2^31-1 fails safe with "not enough memory", so the usable range
** is exactly the middle: large enough to hurt, small enough to succeed.
**
** This matters beyond a guest harming itself. Queue delivery decodes on the guest's
** behalf, and 7.4's store-and-forward means those bytes can come from another
** instance -- so one agent could make a peer balloon with a five-byte message, which
** makes 6.2's bounded queues bound the wrong thing. The hint is only a hint, so
** clamping it costs a correct decode nothing but one comparison.
*/
static int mp_size_hint (const mp_cur *c, size_t n) {
  if (n > c->left)
    n = c->left;
  return (int)((n > (size_t)INT_MAX) ? 0 : n);
}


static void mp_decode_array (mp_cur *c, size_t n, int depth) {
  lua_State *L = c->L;
  size_t i;
  luaL_checkstack(L, 3, "msgpack: nesting too deep to decode");
  lua_createtable(L, mp_size_hint(c, n), 0);
  if (c->snap != NULL)
    mp_unsnap_register(c);  /* before the contents: a cycle needs this */
  for (i = 1; i <= n; i++) {
    mp_decode_value(c, depth + 1);
    lua_rawseti(L, -2, (lua_Integer)i);
  }
}


static void mp_decode_map (mp_cur *c, size_t n, int depth) {
  lua_State *L = c->L;
  size_t i;
  luaL_checkstack(L, 4, "msgpack: nesting too deep to decode");
  /* A pair is a key and a value, so two bytes at least: halve the room before
     clamping rather than after, or a map claiming 'left' pairs still doubles. */
  lua_createtable(L, 0, mp_size_hint(c, (n > c->left / 2) ? c->left / 2 : n));
  if (c->snap != NULL)
    mp_unsnap_register(c);  /* before the contents: a cycle needs this */
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
      /*
      ** Untrusted bytes never reach the resolver, and this is a security boundary
      ** rather than a tidiness rule. 7.3 claims a reference "cannot be forged"
      ** because "there is no constructor anywhere" -- but 'msgpack.decode' is
      ** guest-callable, and running the resolver on whatever string it was handed
      ** made it exactly that constructor. A program could write
      ** msgpack.decode('\xd4\x02' .. name), get a genuine reference with the real
      ** hidden metatable, and bind it: proven against a host that had authorised
      ** one peer and delivered nothing, where the guest reached that peer's queue.
      ** The reference was a guessable name, not a capability.
      **
      ** Falling through to the opaque ext value is the same thing that already
      ** happens when no resolver is installed, so a guest decoding these bytes gets
      ** a documented, inert value rather than an error -- and a real reference stays
      ** something only the host can put in front of a program.
      */
      if (!c->trusted) {
        mp_ext_push(L, code, data, len);
        return;
      }
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
    case 0x04:
      if (c->snap != NULL) {  /* backreference to an object already read */
        unsigned long pos;
        if (len != 4)
          luaL_error(L, "msgpack: a backreference must be 4 bytes, not %d",
                     (int)len);
        pos = ((unsigned long)(unsigned char)data[0] << 24) |
              ((unsigned long)(unsigned char)data[1] << 16) |
              ((unsigned long)(unsigned char)data[2] << 8) |
              (unsigned long)(unsigned char)data[3];
        /* 10.10 wants this checked rather than trusted: in range, and never
           forward. A forward reference would name an object that does not
           exist yet, and accepting one would put a nil where a table belongs
           and fail somewhere else entirely. */
        if (pos < 1 || pos > c->snap->n)
          luaL_error(L, "msgpack: backreference %I is out of range (%I objects "
                        "so far)", (lua_Integer)pos, (lua_Integer)c->snap->n);
        lua_rawgeti(L, c->snap->objs, (lua_Integer)pos);
        return;
      }
      luaL_error(L, "msgpack: ext %s is only valid inside a snapshot stream",
                 mp_hexbyte(code));
      return;
    case 0x03: case 0x05: case 0x06: case 0x07: case 0x08:
      if (c->snap != NULL && c->snap->hooks != NULL &&
          c->snap->hooks->decode != NULL &&
          c->snap->hooks->decode(L, code, (const unsigned char *)data, len,
                                 c->snap->hooks->ud)) {
        /*
        ** 0x05 and 0x06 are graph objects and take a position; 0x03 and 0x07
        ** are named or embedded and do not. This mirrors exactly what the
        ** encoder's 'permanent' and 'encode' hooks did on the way out, and the
        ** mirror has to be exact: give a position to something the encoder did
        ** not, or withhold one it did, and every position after the first such
        ** object refers to the wrong object. Nothing detects that except a
        ** backreference landing somewhere absurd, some distance later.
        */
        if (code == 0x05 || code == 0x06 || code == 0x08)
          mp_unsnap_register(c);
        return;
      }
      if (c->snap != NULL)
        luaL_error(L, "msgpack: ext %s needs a snapshot decoder that is not "
                      "installed", mp_hexbyte(code));
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
  if (depth > (c->snap != NULL ? MP_SNAP_MAX_DEPTH : MP_MAX_DEPTH))
    luaL_error(L, "msgpack: nesting deeper than %d",
               c->snap != NULL ? MP_SNAP_MAX_DEPTH : MP_MAX_DEPTH);
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
  for (i = 0; i <= MP_PATH_DEPTH; i++)
    ctx.path[i].kind = 0;
  ctx.depth = 0;
  ctx.snap = NULL;
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
  c.snap = NULL;
  c.trusted = 0;                  /* a guest's own string mints no references */
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
  ctx.snap = NULL;
  ctx.buf = mp_buf_new(L);
  base = lua_gettop(L);  /* the buffer sits here and must be dropped after */
  mp_encode_value(L, &ctx, abs);
  lua_pushlstring(L, (const char *)ctx.buf->b, ctx.buf->len);
  lua_remove(L, base);  /* drop the buffer, leaving only the string */
}


LUA_API void diluvium_msgpack_decode (lua_State *L, const char *s, size_t len) {
  diluvium_msgpack_decode_n(L, s, len, NULL);
}


LUA_API void diluvium_msgpack_decode_n (lua_State *L, const char *s, size_t len,
                                        size_t *used) {
  mp_cur c;
  c.L = L;
  c.p = (const unsigned char *)s;
  c.left = len;
  c.snap = NULL;
  /* The host's own entry point, used by queue delivery: these bytes arrived from
     outside the guest, so a reference in them is one the host chose to hand over. */
  c.trusted = 1;
  mp_decode_value(&c, 0);
  if (used != NULL)
    *used = len - c.left;
}


/* ======================================================================
** Snapshot mode
** ====================================================================== */

static mp_ctx *mp_current (lua_State *L) {
  mp_ctx *ctx;
  lua_rawgetp(L, LUA_REGISTRYINDEX, &MP_CURRENT);
  ctx = (mp_ctx *)lua_touserdata(L, -1);
  lua_pop(L, 1);
  return ctx;
}


LUA_API void diluvium_msgpack_appendext (lua_State *L, int code,
                                         const char *data, size_t len) {
  mp_ctx *ctx = mp_current(L);
  if (ctx == NULL || ctx->snap == NULL)
    luaL_error(L, "msgpack: nothing is being encoded, so an ext object has "
                  "nowhere to go");
  mp_enc_ext(ctx->buf, code, data, len);
}


LUA_API void diluvium_msgpack_appendfixup (lua_State *L, int tag,
                                           const lua_Integer *ops, int nops) {
  mp_ctx *ctx = mp_current(L);
  int i;
  if (ctx == NULL || ctx->snap == NULL)
    luaL_error(L, "msgpack: nothing is being encoded, so a fixup has nowhere "
                  "to go");
  mp_enc_int(ctx->buf, tag);
  for (i = 0; i < nops; i++)
    mp_enc_int(ctx->buf, ops[i]);
}


LUA_API lua_Integer diluvium_msgpack_position (lua_State *L, int idx) {
  mp_ctx *ctx = mp_current(L);
  lua_Integer pos;
  if (ctx == NULL || ctx->snap == NULL)
    return 0;
  lua_pushvalue(L, idx);
  lua_rawget(L, ctx->snap->seen);
  pos = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return pos;
}


LUA_API void diluvium_msgpack_encode_graph (lua_State *L, int idx,
                                    const diluvium_snap_hooks *h) {
  mp_ctx ctx;
  mp_snap snap;
  int abs = lua_absindex(L, idx);
  int base = lua_gettop(L);
  int i;
  void *outer;
  luaL_checkstack(L, 8, "msgpack: cannot start a snapshot");
  memset(&ctx, 0, sizeof(ctx));
  for (i = 0; i <= MP_PATH_DEPTH; i++)
    ctx.path[i].kind = 0;
  ctx.depth = 0;
  ctx.buf = mp_buf_new(L);                    /* base + 1 */
  lua_newtable(L);                            /* base + 2: seen */
  lua_newtable(L);                            /* base + 3: fixup worklist */
  lua_newtable(L);                            /* base + 4: upvalue owner pos */
  lua_newtable(L);                            /* base + 5: upvalue owner index */
  snap.seen = base + 2;
  snap.fix = base + 3;
  snap.upown = base + 4;
  snap.upidx = base + 5;
  snap.nwritten = 0;
  snap.n = 0;
  snap.hooks = h;
  ctx.snap = &snap;
  /*
  ** Published so a hook can append through 'diluvium_msgpack_appendext'. Saved
  ** and restored rather than simply cleared: a nested snapshot encode is not
  ** supported, but leaving a dangling pointer behind if one is attempted would
  ** turn an unsupported case into a crash.
  */
  lua_rawgetp(L, LUA_REGISTRYINDEX, &MP_CURRENT);
  outer = lua_touserdata(L, -1);
  lua_pop(L, 1);
  lua_pushlightuserdata(L, &ctx);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &MP_CURRENT);

  if (h != NULL && h->begin != NULL && !h->begin(L, h->ud))
    luaL_error(L, "msgpack: the snapshot layer could not start an encode");

  mp_encode_value(L, &ctx, abs);              /* the root */

  /*
  ** Drain the fixup worklist. Written as a re-checked length rather than a
  ** cached one on purpose: writing a fixup can discover more of them -- a
  ** metatable with a metatable, an upvalue holding a table with one -- and a
  ** cached bound would drop them silently, restoring a graph that looks right
  ** until something two levels down has lost its metatable.
  */
  while ((lua_Integer)snap.nwritten * 3 < (lua_Integer)lua_rawlen(L, snap.fix)) {
    lua_Integer k = (lua_Integer)snap.nwritten * 3;
    int tag;
    unsigned long pos;
    luaL_checkstack(L, 8, "msgpack: cannot walk the fixup list");
    lua_rawgeti(L, snap.fix, k + 1);
    tag = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_rawgeti(L, snap.fix, k + 2);          /* the owner */
    lua_pushvalue(L, -1);
    lua_rawget(L, snap.seen);
    pos = (unsigned long)lua_tointeger(L, -1);
    lua_pop(L, 1);
    /*
    ** An owner reaches this list only from inside 'mp_snap_intern' or
    ** 'mp_snap_queueupvals', after its position is recorded, so a zero here is
    ** impossible unless the two have drifted apart. Checked rather than
    ** asserted because the failure without it is not a wrong answer but a
    ** *hang*: a position of zero means the entry is re-queued every pass and
    ** the loop never drains. Found by removing the identity map to see what the
    ** tests would say, and getting no answer at all.
    */
    if (pos == 0)
      luaL_error(L, "msgpack: internal error, a queued fixup owner has no "
                    "position");
    snap.nwritten++;
    if (tag == DILUVIUM_FIX_META) {
      mp_enc_int(ctx.buf, DILUVIUM_FIX_META);
      mp_enc_int(ctx.buf, (lua_Integer)pos);
      lua_rawgeti(L, snap.fix, k + 3);        /* the metatable */
      mp_encode_value(L, &ctx, lua_gettop(L));
      lua_pop(L, 2);                          /* metatable, owner */
    }
    else if (tag == DILUVIUM_FIX_SLOT) {
      /* stack: ... thread */
      int th = lua_gettop(L);
      lua_Integer slot;
      lua_Integer ops[2];
      lua_rawgeti(L, snap.fix, k + 3);
      slot = lua_tointeger(L, -1);
      lua_pop(L, 1);
      ops[0] = (lua_Integer)pos;
      ops[1] = slot;
      diluvium_msgpack_appendfixup(L, DILUVIUM_FIX_SLOT, ops, 2);
      if (!snap.hooks->slot(L, th, (int)slot, snap.hooks->ud))
        luaL_error(L, "msgpack: internal error, a queued stack slot vanished");
      mp_encode_value(L, &ctx, lua_gettop(L));
      lua_pop(L, 2);                          /* the value, the thread */
    }
    else if (tag == DILUVIUM_FIX_BUILD) {
      lua_Integer ops[1];
      ops[0] = (lua_Integer)pos;
      diluvium_msgpack_appendfixup(L, DILUVIUM_FIX_BUILD, ops, 1);
      lua_pop(L, 1);                          /* the thread */
    }
    else if (tag == DILUVIUM_FIX_UPVAL) {
      /* stack: ... closure */
      int cl = lua_gettop(L);
      int idx;
      const void *id;
      lua_rawgeti(L, snap.fix, k + 3);
      idx = (int)lua_tointeger(L, -1);
      lua_pop(L, 1);
      /*
      ** An upvalue open against a thread in this graph is an alias for a stack
      ** slot, not a value. The layer gets first refusal because only it knows
      ** which threads are being captured; writing the value instead would sever
      ** the alias, and the parked coroutine could then assign to that local
      ** without the closure seeing it.
      */
      if (snap.hooks != NULL && snap.hooks->openupval != NULL &&
          snap.hooks->openupval(L, cl, idx, snap.hooks->ud)) {
        lua_pop(L, 1);                        /* the closure */
        continue;
      }
      id = lua_upvalueid(L, cl, idx);
      if (id == NULL)
        luaL_error(L, "msgpack: internal error, upvalue %d of a queued closure "
                      "does not exist", idx);
      /*
      ** Whoever gets here first owns the upvalue; everyone after joins to it.
      ** That is what keeps two closures over one variable sharing it after a
      ** round trip, which 10.3 requires and which no amount of copying values
      ** would achieve.
      */
      lua_pushlightuserdata(L, (void *)id);
      if (lua_rawget(L, snap.upown) != LUA_TNIL) {
        lua_Integer opos = lua_tointeger(L, -1);
        lua_Integer oidx;
        lua_pop(L, 1);
        lua_pushlightuserdata(L, (void *)id);
        lua_rawget(L, snap.upidx);
        oidx = lua_tointeger(L, -1);
        lua_pop(L, 1);
        mp_enc_int(ctx.buf, DILUVIUM_FIX_UPJOIN);
        mp_enc_int(ctx.buf, (lua_Integer)pos);
        mp_enc_int(ctx.buf, idx);
        mp_enc_int(ctx.buf, opos);
        mp_enc_int(ctx.buf, oidx);
      }
      else {
        lua_pop(L, 1);
        lua_pushlightuserdata(L, (void *)id);
        lua_pushinteger(L, (lua_Integer)pos);
        lua_rawset(L, snap.upown);
        lua_pushlightuserdata(L, (void *)id);
        lua_pushinteger(L, idx);
        lua_rawset(L, snap.upidx);
        mp_enc_int(ctx.buf, DILUVIUM_FIX_UPVAL);
        mp_enc_int(ctx.buf, (lua_Integer)pos);
        mp_enc_int(ctx.buf, idx);
        lua_getupvalue(L, cl, idx);
        mp_encode_value(L, &ctx, lua_gettop(L));
        lua_pop(L, 1);
      }
      lua_pop(L, 1);                          /* the closure */
    }
    else
      luaL_error(L, "msgpack: internal error, unknown fixup tag %d", tag);
  }
  mp_enc_nil(ctx.buf);                        /* end of the fixup section */

  if (outer == NULL)
    lua_pushnil(L);
  else
    lua_pushlightuserdata(L, outer);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &MP_CURRENT);

  lua_pushlstring(L, (const char *)ctx.buf->b, ctx.buf->len);
  lua_replace(L, base + 1);                   /* over the buffer */
  lua_settop(L, base + 1);                    /* drop the working tables */
}


/* Read an integer operand from a fixup, or raise naming what was expected. */
static lua_Integer mp_unsnap_readint (mp_cur *c, const char *what) {
  lua_State *L = c->L;
  lua_Integer v;
  if (c->left == 0)
    luaL_error(L, "msgpack: a fixup ended before its %s", what);
  mp_decode_value(c, 0);
  if (!lua_isinteger(L, -1))
    luaL_error(L, "msgpack: a fixup's %s must be an integer", what);
  v = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return v;
}


/* The same, for an operand that names an object position. Range-checked. */
static lua_Integer mp_unsnap_readpos (mp_cur *c, mp_unsnap *s,
                                      const char *what) {
  lua_Integer v = mp_unsnap_readint(c, what);
  if (v < 1 || (unsigned long)v > s->n)
    luaL_error(c->L, "msgpack: %s position %I is out of range (%I objects)",
               what, v, (lua_Integer)s->n);
  return v;
}


LUA_API void diluvium_msgpack_pushobject (lua_State *L, lua_Integer pos) {
  mp_unsnap *s;
  lua_rawgetp(L, LUA_REGISTRYINDEX, &MP_CURDEC);
  s = (mp_unsnap *)lua_touserdata(L, -1);
  lua_pop(L, 1);
  if (s == NULL)
    luaL_error(L, "msgpack: nothing is being decoded, so there is no object at "
                  "position %I", pos);
  if (pos < 1 || (unsigned long)pos > s->n)
    luaL_error(L, "msgpack: position %I is out of range (%I objects)", pos,
               (lua_Integer)s->n);
  lua_rawgeti(L, s->objs, pos);
}


LUA_API void diluvium_msgpack_decode_graph (lua_State *L, const char *s,
                                    size_t len,
                                    const diluvium_snap_hooks *h) {
  mp_cur c;
  mp_unsnap snap;
  int base = lua_gettop(L);
  void *outerdec;
  luaL_checkstack(L, 8, "msgpack: cannot start a restore");
  lua_newtable(L);                            /* base + 1: objs */
  snap.objs = base + 1;
  snap.n = 0;
  snap.hooks = h;
  c.L = L;
  c.p = (const unsigned char *)s;
  c.left = len;
  c.snap = &snap;
  /* A snapshot is the host's bytes too. 10.10 treats one as untrusted for
     *validation* -- every field is range-checked -- but a reference inside it was
     put there by a previous run of this same program, so restoring it is restoring
     what the host had already granted, not minting something new. Refusing here
     would mean a parked agent lost its endpoints across a hibernation. */
  c.trusted = 1;
  /* Published so a fixup hook can resolve a position. Saved and restored, since
     a nested decode is unsupported but must not leave a dangling pointer. */
  lua_rawgetp(L, LUA_REGISTRYINDEX, &MP_CURDEC);
  outerdec = lua_touserdata(L, -1);
  lua_pop(L, 1);
  lua_pushlightuserdata(L, &snap);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &MP_CURDEC);

  if (h != NULL && h->begin != NULL && !h->begin(L, h->ud))
    luaL_error(L, "msgpack: the snapshot layer could not start a decode");

  mp_decode_value(&c, 0);                     /* the root: base + 2 */

  /* The fixup section, terminated by nil. */
  for (;;) {
    int tag;
    lua_Integer pos, idx;
    if (c.left == 0)
      luaL_error(L, "msgpack: snapshot graph ended without its fixup "
                    "terminator");
    mp_decode_value(&c, 0);                   /* the tag, or nil */
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      break;
    }
    if (!lua_isinteger(L, -1))
      luaL_error(L, "msgpack: a fixup must begin with an integer tag");
    tag = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    /* Validated before any operand is consumed. An unknown tag means the rest
       of the section cannot be parsed at all -- the operand count depends on
       the tag -- so reporting "bad owner" after reading whatever came next
       would name the wrong thing. */
    if (tag < 1 || tag > DILUVIUM_FIX_LAST)
      luaL_error(L, "msgpack: fixup tag %d is not one this runtime knows", tag);
    pos = mp_unsnap_readpos(&c, &snap, "fixup owner");
    if (tag == DILUVIUM_FIX_META) {
      mp_decode_value(&c, 0);                 /* the metatable */
      if (!lua_istable(L, -1))
        luaL_error(L, "msgpack: a metatable must be a table");
      lua_rawgeti(L, snap.objs, pos);         /* the owner */
      if (!lua_istable(L, -1))
        luaL_error(L, "msgpack: object %I is not a table and cannot take a "
                      "metatable", pos);
      lua_insert(L, -2);                      /* owner, metatable */
      /*
      ** The metatable goes on after the contents, which is the only order
      ** available: the owner has to exist before anything can refer to it, and
      ** its contents are read on the way past. For a weak table that means the
      ** entries were inserted while it was still strong and '__mode' is applied
      ** afterwards. The collector reads '__mode' from the metatable at each
      ** traversal rather than caching it at insert time, so the weakness does
      ** take effect -- but this is the one place in the restore where the result
      ** depends on collector behaviour rather than on the format, and it is
      ** worth knowing that before something else is built on top of it.
      */
      lua_setmetatable(L, -2);
      lua_pop(L, 1);                          /* the owner */
    }
    else if (tag == DILUVIUM_FIX_SLOT || tag == DILUVIUM_FIX_BUILD ||
             tag == DILUVIUM_FIX_UPOPEN) {
      /*
      ** The layer's tags. The codec parses their shape -- which it must, since a
      ** reader has to be able to walk the section without knowing what a tag
      ** means -- and hands the parsed operands over.
      */
      lua_Integer ops[4];
      int nops = (tag == DILUVIUM_FIX_BUILD) ? 0 :
                 (tag == DILUVIUM_FIX_SLOT) ? 1 : 3;
      int hasvalue = (tag == DILUVIUM_FIX_SLOT);
      int i;
      ops[0] = pos;
      for (i = 0; i < nops; i++)
        ops[i + 1] = (i == 1 && tag == DILUVIUM_FIX_UPOPEN)
                     ? mp_unsnap_readpos(&c, &snap, "upvalue owner thread")
                     : mp_unsnap_readint(&c, "fixup operand");
      if (hasvalue) {
        if (c.left == 0)
          luaL_error(L, "msgpack: a fixup ended before its value");
        mp_decode_value(&c, 0);
      }
      if (snap.hooks == NULL || snap.hooks->fixup == NULL ||
          !snap.hooks->fixup(L, tag, ops, nops + 1, snap.hooks->ud))
        luaL_error(L, "msgpack: fixup tag %d needs a snapshot layer that is not "
                      "installed", tag);
      if (hasvalue)
        lua_pop(L, 1);
    }
    else if (tag == DILUVIUM_FIX_UPVAL || tag == DILUVIUM_FIX_UPJOIN) {
      idx = mp_unsnap_readint(&c, "upvalue index");
      lua_rawgeti(L, snap.objs, pos);         /* the closure */
      if (!lua_isfunction(L, -1) || lua_iscfunction(L, -1))
        luaL_error(L, "msgpack: object %I is not a Lua closure and has no "
                      "upvalue %I", pos, idx);
      if (tag == DILUVIUM_FIX_UPVAL) {
        mp_decode_value(&c, 0);               /* the value */
        if (lua_setupvalue(L, -2, (int)idx) == NULL)
          luaL_error(L, "msgpack: object %I has no upvalue %I", pos, idx);
        lua_pop(L, 1);                        /* the closure */
      }
      else {
        lua_Integer spos = mp_unsnap_readpos(&c, &snap, "upvalue source");
        lua_Integer sidx = mp_unsnap_readint(&c, "upvalue source index");
        lua_rawgeti(L, snap.objs, spos);       /* the source closure */
        if (!lua_isfunction(L, -1) || lua_iscfunction(L, -1))
          luaL_error(L, "msgpack: object %I is not a Lua closure and cannot be "
                        "an upvalue source", spos);
        /* Range-checked before the join, because 'lua_upvaluejoin' has no way
           to report a bad index and 10.10 will not have a malformed snapshot
           reaching into memory. */
        if (lua_getupvalue(L, -2, (int)idx) == NULL)
          luaL_error(L, "msgpack: object %I has no upvalue %I to join", pos, idx);
        lua_pop(L, 1);
        if (lua_getupvalue(L, -1, (int)sidx) == NULL)
          luaL_error(L, "msgpack: object %I has no upvalue %I to join from",
                     spos, sidx);
        lua_pop(L, 1);
        lua_upvaluejoin(L, -2, (int)idx, -1, (int)sidx);
        lua_pop(L, 2);                        /* source, closure */
      }
    }
  }

  /* Everything is in place, so anything that needed the finished graph runs now.
     Still inside the published decode, because a finisher resolves positions. */
  if (snap.hooks != NULL && snap.hooks->finish != NULL &&
      !snap.hooks->finish(L, snap.hooks->ud))
    luaL_error(L, "msgpack: the snapshot layer could not finish the restore");

  if (outerdec == NULL)
    lua_pushnil(L);
  else
    lua_pushlightuserdata(L, outerdec);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &MP_CURDEC);

  if (c.left != 0)
    luaL_error(L, "msgpack: %I bytes left over after the snapshot graph",
               (lua_Integer)c.left);
  lua_remove(L, base + 1);                    /* drop objs, leaving the root */
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


/* ======================================================================
** The read-only token cursor. See dmsgpack.h for why it is here.
** ====================================================================== */

LUA_API void diluvium_mp_open (diluvium_mp_cursor *c, const void *s,
                               size_t len) {
  c->p = (const unsigned char *)s;
  c->left = len;
}


/* Take 'n' bytes big-endian. The caller has already checked the length. */
static uint64_t mpc_be (diluvium_mp_cursor *c, int width) {
  uint64_t n = 0;
  int i;
  for (i = 0; i < width; i++)
    n = (n << 8) | c->p[i];
  c->p += width;
  c->left -= (size_t)width;
  return n;
}


static int mpc_have (diluvium_mp_cursor *c, size_t n) {
  return c->left >= n;
}


static int mpc_bad (diluvium_mp_token *out) {
  out->kind = DILUVIUM_MP_BAD;
  return 0;
}


LUA_API int diluvium_mp_read (diluvium_mp_cursor *c, diluvium_mp_token *out) {
  unsigned char t;
  memset(out, 0, sizeof(*out));
  if (c->left == 0) {
    out->kind = DILUVIUM_MP_END;
    return 0;
  }
  t = c->p[0];
  c->p++;
  c->left--;
  /* The fixed-form ranges first, since they are most of a real message. */
  if (t <= 0x7f) { out->kind = DILUVIUM_MP_INT; out->i = t; return 1; }
  if (t >= 0xe0) {
    out->kind = DILUVIUM_MP_INT;
    out->i = (long long)(signed char)t;
    return 1;
  }
  if ((t & 0xf0) == 0x80) {
    out->kind = DILUVIUM_MP_MAP; out->len = t & 0x0f; return 1;
  }
  if ((t & 0xf0) == 0x90) {
    out->kind = DILUVIUM_MP_ARRAY; out->len = t & 0x0f; return 1;
  }
  if ((t & 0xe0) == 0xa0) {
    size_t n = t & 0x1f;
    if (!mpc_have(c, n)) return mpc_bad(out);
    out->kind = DILUVIUM_MP_STR;
    out->p = (const char *)c->p;
    out->len = n;
    c->p += n; c->left -= n;
    return 1;
  }
  switch (t) {
    case 0xc0: out->kind = DILUVIUM_MP_NIL; return 1;
    case 0xc2: out->kind = DILUVIUM_MP_BOOL; out->b = 0; return 1;
    case 0xc3: out->kind = DILUVIUM_MP_BOOL; out->b = 1; return 1;
    case 0xc4: case 0xc5: case 0xc6:          /* bin 8, 16, 32 */
    case 0xd9: case 0xda: case 0xdb: {        /* str 8, 16, 32 */
      int w = (t == 0xc4 || t == 0xd9) ? 1 : (t == 0xc5 || t == 0xda) ? 2 : 4;
      size_t n;
      if (!mpc_have(c, (size_t)w)) return mpc_bad(out);
      n = (size_t)mpc_be(c, w);
      if (!mpc_have(c, n)) return mpc_bad(out);
      out->kind = DILUVIUM_MP_STR;
      out->p = (const char *)c->p;
      out->len = n;
      c->p += n; c->left -= n;
      return 1;
    }
    case 0xca: case 0xcb: {                   /* float 32, 64 */
      int w = (t == 0xca) ? 4 : 8;
      unsigned char buf[8];
      if (!mpc_have(c, (size_t)w)) return mpc_bad(out);
      memcpy(buf, c->p, (size_t)w);
      c->p += w; c->left -= (size_t)w;
      mp_memrevifle(buf, (size_t)w);
      out->kind = DILUVIUM_MP_FLOAT;
      if (w == 4) { float g; memcpy(&g, buf, 4); out->f = (double)g; }
      else { double g; memcpy(&g, buf, 8); out->f = g; }
      return 1;
    }
    case 0xcc: case 0xcd: case 0xce: case 0xcf: {   /* uint 8..64 */
      int w = 1 << (t - 0xcc);
      uint64_t v;
      if (!mpc_have(c, (size_t)w)) return mpc_bad(out);
      v = mpc_be(c, w);
      /* A uint64 above the signed range would wrap, and a caller reading a
         budget or a count would then act on a negative number. Refused rather
         than truncated, matching what the Lua decoder does with the same value. */
      if (w == 8 && (v >> 63) != 0) return mpc_bad(out);
      out->kind = DILUVIUM_MP_INT;
      out->i = (long long)v;
      return 1;
    }
    case 0xd0: case 0xd1: case 0xd2: case 0xd3: {   /* int 8..64 */
      int w = 1 << (t - 0xd0);
      uint64_t v;
      if (!mpc_have(c, (size_t)w)) return mpc_bad(out);
      v = mpc_be(c, w);
      out->kind = DILUVIUM_MP_INT;
      /* Sign-extend through unsigned arithmetic; shifting a negative signed
         value is what the Lua decoder was rewritten to avoid. */
      switch (w) {
        case 1: out->i = (long long)(signed char)(unsigned char)v; break;
        case 2: out->i = (long long)(short)(unsigned short)v; break;
        case 4: out->i = (long long)(int)(unsigned int)v; break;
        default: out->i = (long long)(int64_t)v; break;
      }
      return 1;
    }
    case 0xdc: case 0xdd: {                   /* array 16, 32 */
      int w = (t == 0xdc) ? 2 : 4;
      if (!mpc_have(c, (size_t)w)) return mpc_bad(out);
      out->kind = DILUVIUM_MP_ARRAY;
      out->len = (size_t)mpc_be(c, w);
      return 1;
    }
    case 0xde: case 0xdf: {                   /* map 16, 32 */
      int w = (t == 0xde) ? 2 : 4;
      if (!mpc_have(c, (size_t)w)) return mpc_bad(out);
      out->kind = DILUVIUM_MP_MAP;
      out->len = (size_t)mpc_be(c, w);
      return 1;
    }
    case 0xd4: case 0xd5: case 0xd6: case 0xd7: case 0xd8: {  /* fixext */
      size_t n = (size_t)1 << (t - 0xd4);
      if (!mpc_have(c, n + 1)) return mpc_bad(out);
      out->kind = DILUVIUM_MP_EXT;
      out->code = c->p[0];
      out->p = (const char *)(c->p + 1);
      out->len = n;
      c->p += n + 1; c->left -= n + 1;
      return 1;
    }
    case 0xc7: case 0xc8: case 0xc9: {        /* ext 8, 16, 32 */
      int w = (t == 0xc7) ? 1 : (t == 0xc8) ? 2 : 4;
      size_t n;
      if (!mpc_have(c, (size_t)w + 1)) return mpc_bad(out);
      n = (size_t)mpc_be(c, w);
      out->code = c->p[0];
      c->p++; c->left--;
      if (!mpc_have(c, n)) return mpc_bad(out);
      out->kind = DILUVIUM_MP_EXT;
      out->p = (const char *)c->p;
      out->len = n;
      c->p += n; c->left -= n;
      return 1;
    }
    default:
      return mpc_bad(out);                    /* 0xc1, which nothing may emit */
  }
}


LUA_API int diluvium_mp_skip (diluvium_mp_cursor *c) {
  diluvium_mp_token tok;
  size_t pending = 1;
  /*
  ** Iterative rather than recursive, and counted rather than nested: a hostile
  ** message can nest as deeply as it likes, and a recursive skip would meet the C
  ** stack before it met an error. The count is the number of values still owed.
  */
  while (pending > 0) {
    if (!diluvium_mp_read(c, &tok))
      return 0;
    pending--;
    if (tok.kind == DILUVIUM_MP_ARRAY || tok.kind == DILUVIUM_MP_MAP) {
      size_t need = tok.len;
      /*
      ** Bound the claim by the bytes remaining, and do it BEFORE any arithmetic
      ** that could overflow. Every value costs at least one byte, so a container
      ** promising more values than there are bytes left cannot be satisfied by any
      ** input -- refusing here is not a heuristic, it is the only possible verdict.
      **
      ** The earlier version counted instead: it added the claim to 'pending' and
      ** waited to run out of bytes. That reaches the right answer on a 64-bit
      ** size_t and the wrong one on a 32-bit size_t, which is what wasm32 is and
      ** what 11's 'dv_layout' exists because of. Two array32 headers claiming
      ** 0xFFFFFFFF and 2 elements drive the running total to exactly 2^32, which
      ** wraps to zero, and 'pending == 0' is how this loop reports success -- so a
      ** twelve-byte message would be skipped in ten bytes and '_field' would read
      ** whatever followed as a map key.
      **
      ** Keeping 'pending <= c->left' as an invariant makes the overflow
      ** unreachable at any width rather than merely unlikely at one of them, and it
      ** turns a four-billion-iteration count-down into one comparison.
      */
      if (need > c->left)
        return 0;
      if (tok.kind == DILUVIUM_MP_MAP) {
        /* Pairs are two values each. 'need <= c->left' already holds, so the
           doubling is checked against that bound instead of being performed and
           checked afterwards. */
        if (need > c->left - need)
          return 0;
        need += need;
      }
      if (pending > c->left - need)
        return 0;
      pending += need;
    }
  }
  return 1;
}


LUA_API int diluvium_mp_field (diluvium_mp_cursor *c, const char *key) {
  diluvium_mp_token tok;
  size_t klen = strlen(key);
  size_t n;
  if (!diluvium_mp_read(c, &tok) || tok.kind != DILUVIUM_MP_MAP)
    return 0;
  for (n = tok.len; n > 0; n--) {
    diluvium_mp_token k;
    if (!diluvium_mp_read(c, &k))
      return 0;
    if (k.kind == DILUVIUM_MP_STR && k.len == klen &&
        memcmp(k.p, key, klen) == 0)
      return 1;                               /* the cursor is on the value */
    if (!diluvium_mp_skip(c))
      return 0;
  }
  return 0;
}
