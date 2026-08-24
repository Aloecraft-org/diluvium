/*
** djson.c
** The 'json' guest library. See djson.h for where it sits and why.
**
**   json.encode(value)  -> a JSON string
**   json.decode(string) -> the value
**
** The type mapping, encode side:
**   nil            -> null            number (integer) -> a JSON integer
**   boolean        -> true / false    number (float)   -> a JSON number
**   string         -> a JSON string   table            -> array or object
** A non-finite float, a function, a userdata or a thread has no JSON form and
** is an error. A table is an array when its keys are exactly 1..n, an object
** when its keys are all strings, an object when it is empty, and an error
** otherwise -- see 'json.encode' below.
**
** The empty table is the one honest ambiguity in that rule: {} is a valid
** array and a valid object, and this encoder says object. A program that
** means [] wraps the table in 'msgpack.as_array' -- the same tag the
** msgpack encoder honours, deliberately, so both codecs speak one shape
** vocabulary -- and the wrapper is honoured at ANY depth, because an empty
** array is almost never the top-level value: it is a field inside an
** envelope. A non-empty tagged array encodes exactly as it would untagged;
** a tag that contradicts the keys (as_array over string keys, as_map over
** 1..n) is an error, not a coercion.
**
** Decode side, the inverse, with two things worth stating:
**   null -> nil. In an array this leaves a hole, the same way a nil does in
**     any Lua sequence; a program that must keep JSON null distinct from
**     absent should check for it before decoding, or carry the data in
**     msgpack. This matches how the runtime already treats nil.
**   A JSON number with no '.', 'e' or 'E' becomes a Lua integer when it fits
**     one and a float otherwise, so an id stays exact and 1e309 does not.
*/

#define djson_c

#include "lprefix.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "djson.h"
#include "dmsgpack.h"

/* Deep enough for any real document, bounded so a hostile one cannot recurse
   the C stack out from under the process before a Lua-level limit is reached.
   Both directions share it. */
#define JSON_MAX_DEPTH 200


/* ====================================================================== */
/* encode: a Lua value -> JSON text                                       */
/*                                                                        */
/* A private heap buffer, not luaL_Buffer, on purpose. luaL_Buffer keeps a */
/* box on the Lua stack and requires it on top whenever the buffer grows,  */
/* which a recursive encoder cannot honour: the table it is walking sits   */
/* on the stack above the box while it writes that table's bytes. So the   */
/* buffer lives off the stack, the encoders return 0/-1 with a reason in   */
/* eb.err rather than raising (a raise would longjmp past the free), and   */
/* json_encode does the one free and the one luaL_error at the top.        */
/* ====================================================================== */

typedef struct eb { char *p; size_t len, cap; char err[192]; } eb;

static int eb_reserve (eb *b, size_t extra) {
  if (extra > (size_t)-1 - b->len) return -1;
  if (b->len + extra > b->cap) {
    size_t nc = b->cap ? b->cap : 128;
    char *np;
    while (nc < b->len + extra) {
      if (nc > (size_t)-1 / 2) return -1;
      nc *= 2;
    }
    np = (char *)realloc(b->p, nc);
    if (np == NULL) return -1;
    b->p = np; b->cap = nc;
  }
  return 0;
}

static int eb_add (eb *b, const char *s, size_t n) {
  if (n == 0) return 0;
  if (eb_reserve(b, n) != 0) return -1;
  memcpy(b->p + b->len, s, n);
  b->len += n;
  return 0;
}

static int eb_char (eb *b, char c) { return eb_add(b, &c, 1); }

static int enc_oom (eb *b) {
  snprintf(b->err, sizeof(b->err), "out of memory");
  return -1;
}

static int encode_value (lua_State *L, int idx, eb *b, int depth);

static int encode_string (lua_State *L, int idx, eb *b) {
  size_t n, i;
  const char *s = lua_tolstring(L, idx, &n);
  if (eb_char(b, '"') != 0) return enc_oom(b);
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    int rc = 0;
    switch (c) {
      case '"':  rc = eb_add(b, "\\\"", 2); break;
      case '\\': rc = eb_add(b, "\\\\", 2); break;
      case '\b': rc = eb_add(b, "\\b", 2); break;
      case '\f': rc = eb_add(b, "\\f", 2); break;
      case '\n': rc = eb_add(b, "\\n", 2); break;
      case '\r': rc = eb_add(b, "\\r", 2); break;
      case '\t': rc = eb_add(b, "\\t", 2); break;
      default:
        if (c < 0x20) {
          char u[7];
          snprintf(u, sizeof(u), "\\u%04x", c);
          rc = eb_add(b, u, 6);
        }
        else
          rc = eb_char(b, (char)c);     /* bytes >= 0x20 pass; UTF-8 stays */
    }
    if (rc != 0) return enc_oom(b);
  }
  return eb_char(b, '"') != 0 ? enc_oom(b) : 0;
}

/* Decide a table's JSON shape without emitting it. 1 = array (length in
   *len), 0 = object, -1 = neither: a mix of integer and string keys, a sparse
   or out-of-range integer key, or a key that is neither. */
static int table_shape (lua_State *L, int idx, lua_Integer *len) {
  lua_Integer n = (lua_Integer)lua_rawlen(L, idx);
  lua_Integer nkeys = 0;
  int all_string = 1;                   /* every key seen is a string */
  int clean_array = 1;                  /* every key is an int in 1..n */
  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    nkeys++;
    if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2)) {
      lua_Integer k = lua_tointeger(L, -2);
      all_string = 0;
      if (k < 1 || k > n) clean_array = 0;
    }
    else if (lua_type(L, -2) == LUA_TSTRING)
      clean_array = 0;
    else {
      all_string = 0;
      clean_array = 0;
    }
    lua_pop(L, 1);                      /* value; the key drives the next step */
  }
  if (n > 0 && clean_array && nkeys == n) { *len = n; return 1; }
  if (all_string) return 0;             /* includes the empty table -> object */
  return -1;
}

static int encode_table_body (lua_State *L, int idx, eb *b, int depth,
                              int shape, lua_Integer len);

static int encode_table (lua_State *L, int idx, eb *b, int depth) {
  lua_Integer len = 0;
  int shape, wrapped, rc;
  if (depth > JSON_MAX_DEPTH) {
    snprintf(b->err, sizeof(b->err),
             "nesting past %d levels, or a cycle", JSON_MAX_DEPTH);
    return -1;
  }
  if (!lua_checkstack(L, 4)) {
    snprintf(b->err, sizeof(b->err), "the value is too deep to walk");
    return -1;
  }
  /* A msgpack shape wrapper names the answer to the empty-table question,
     wherever it sits in the value. Unwrap, and hold the inner table to the
     declared shape rather than guessing it. */
  wrapped = diluvium_msgpack_shapeof(L, idx);
  if (wrapped != 0) {
    lua_rawgeti(L, lua_absindex(L, idx), 1);
    idx = lua_gettop(L);
    if (diluvium_msgpack_shapeof(L, idx) != 0) {
      snprintf(b->err, sizeof(b->err),
               "a shape wrapper wrapping another has no meaning");
      lua_pop(L, 1);
      return -1;
    }
    shape = table_shape(L, idx, &len);
    if (wrapped == DILUVIUM_MP_SHAPE_ARRAY) {
      if (shape == 0) {
        /* shape 0 is "all keys are strings", vacuously true of the empty
           table -- the case the tag exists for. Non-empty string keys
           contradict the tag. */
        lua_pushnil(L);
        if (lua_next(L, idx) != 0) {
          lua_pop(L, 2);
          shape = -1;
        }
        else {
          shape = 1;
          len = 0;
        }
      }
      if (shape != 1) {
        snprintf(b->err, sizeof(b->err),
                 "tagged as_array, but the keys are not exactly 1..n");
        lua_pop(L, 1);
        return -1;
      }
    }
    else {                              /* as_map */
      if (shape != 0) {
        snprintf(b->err, sizeof(b->err),
                 "tagged as_map, but the keys are not all strings");
        lua_pop(L, 1);
        return -1;
      }
    }
    rc = encode_table_body(L, idx, b, depth, shape, len);
    lua_pop(L, 1);
    return rc;
  }
  shape = table_shape(L, idx, &len);
  if (shape < 0) {
    snprintf(b->err, sizeof(b->err),
             "a table with mixed or non-string keys has no JSON form (keys "
             "are neither exactly 1..n nor all strings; an empty array is "
             "spelled msgpack.as_array)");
    return -1;
  }
  return encode_table_body(L, idx, b, depth, shape, len);
}

static int encode_table_body (lua_State *L, int idx, eb *b, int depth,
                              int shape, lua_Integer len) {
  if (shape == 1) {                     /* array */
    lua_Integer i;
    if (eb_char(b, '[') != 0) return enc_oom(b);
    for (i = 1; i <= len; i++) {
      if (i > 1 && eb_char(b, ',') != 0) return enc_oom(b);
      lua_rawgeti(L, idx, i);
      if (encode_value(L, lua_gettop(L), b, depth + 1) != 0) return -1;
      lua_pop(L, 1);
    }
    return eb_char(b, ']') != 0 ? enc_oom(b) : 0;
  }
  else {                                /* object */
    int first = 1;
    if (eb_char(b, '{') != 0) return enc_oom(b);
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
      /* stack: ... key value; keys are all strings (table_shape checked). */
      if (!first && eb_char(b, ',') != 0) { lua_pop(L, 2); return enc_oom(b); }
      first = 0;
      lua_pushvalue(L, -2);             /* a copy of the key to stringify */
      if (encode_string(L, lua_gettop(L), b) != 0) { lua_pop(L, 3); return -1; }
      lua_pop(L, 1);
      if (eb_char(b, ':') != 0) { lua_pop(L, 2); return enc_oom(b); }
      if (encode_value(L, lua_gettop(L), b, depth + 1) != 0) {
        lua_pop(L, 2); return -1;
      }
      lua_pop(L, 1);                    /* value; key drives the next step */
    }
    return eb_char(b, '}') != 0 ? enc_oom(b) : 0;
  }
}

static int encode_value (lua_State *L, int idx, eb *b, int depth) {
  switch (lua_type(L, idx)) {
    case LUA_TNIL:
      return eb_add(b, "null", 4) != 0 ? enc_oom(b) : 0;
    case LUA_TBOOLEAN:
      return (lua_toboolean(L, idx) ? eb_add(b, "true", 4)
                                    : eb_add(b, "false", 5)) != 0
             ? enc_oom(b) : 0;
    case LUA_TNUMBER: {
      char tmp[40];
      int n;
      if (lua_isinteger(L, idx))
        n = snprintf(tmp, sizeof(tmp), "%lld",
                     (long long)lua_tointeger(L, idx));
      else {
        double d = lua_tonumber(L, idx);
        if (!isfinite(d)) {
          snprintf(b->err, sizeof(b->err), "%s has no JSON form",
                   d != d ? "nan" : "inf");
          return -1;
        }
        n = snprintf(tmp, sizeof(tmp), "%.17g", d);
      }
      return eb_add(b, tmp, (size_t)n) != 0 ? enc_oom(b) : 0;
    }
    case LUA_TSTRING:
      return encode_string(L, idx, b);
    case LUA_TTABLE:
      return encode_table(L, idx, b, depth);
    default:
      snprintf(b->err, sizeof(b->err), "a %s has no JSON form",
               luaL_typename(L, idx));
      return -1;
  }
}

/* Free the buffer when its userdata is collected -- the safety net for the
   one throw the encoder cannot otherwise guard: an OOM inside the final
   lua_pushlstring, which longjmps past an ordinary free(). */
static int eb_gc (lua_State *L) {
  eb *b = (eb *)lua_touserdata(L, 1);
  if (b != NULL) { free(b->p); b->p = NULL; }
  return 0;
}

static int json_encode (lua_State *L) {
  eb *b;
  luaL_checkany(L, 1);
  if (lua_gettop(L) > 1)
    return luaL_error(L, "json.encode takes one value");
  /* The buffer's one heap block lives under a userdata, so __gc frees it on
     any throw -- a bad value mid-encode, or an OOM in the push below -- rather
     than leaking (and, since the block is off Lua's books, leaking unbudgeted
     host memory a sandboxed guest could grow on purpose). */
  b = (eb *)lua_newuserdatauv(L, sizeof(eb), 0);
  b->p = NULL; b->len = 0; b->cap = 0; b->err[0] = '\0';
  if (luaL_newmetatable(L, "diluvium.json.eb")) {
    lua_pushcfunction(L, eb_gc);
    lua_setfield(L, -2, "__gc");
  }
  lua_setmetatable(L, -2);
  if (encode_value(L, 1, b, 0) != 0)
    return luaL_error(L, "json.encode: %s", b->err);   /* __gc frees b->p */
  lua_pushlstring(L, b->p != NULL ? b->p : "", b->len);
  free(b->p); b->p = NULL;              /* success: free now; __gc no-ops */
  return 1;
}


/* ====================================================================== */
/* decode: JSON text -> a Lua value                                       */
/* ====================================================================== */

typedef struct jctx { const char *p, *end; lua_State *L; } jctx;

static void jd_value (jctx *j, int depth);

static void jd_ws (jctx *j) {
  while (j->p < j->end &&
         (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r'))
    j->p++;
}

/* The start offset is not threaded through, so a refusal reports how many
   bytes remained -- enough to point a reader at the region -- rather than an
   absolute column. */
static int jd_error (jctx *j, const char *why) {
  return luaL_error(j->L, "json.decode: %s (%d bytes from the end)", why,
                    (int)(j->end - j->p));
}

static void jd_string (jctx *j) {
  luaL_Buffer b;
  luaL_buffinit(j->L, &b);
  j->p++;                               /* the opening quote */
  for (;;) {
    unsigned char c;
    if (j->p >= j->end) { jd_error(j, "a string with no closing quote"); }
    c = (unsigned char)*j->p++;
    if (c == '"') break;
    if (c == '\\') {
      char e;
      if (j->p >= j->end) { jd_error(j, "a trailing backslash"); }
      e = *j->p++;
      switch (e) {
        case '"':  luaL_addchar(&b, '"'); break;
        case '\\': luaL_addchar(&b, '\\'); break;
        case '/':  luaL_addchar(&b, '/'); break;
        case 'b':  luaL_addchar(&b, '\b'); break;
        case 'f':  luaL_addchar(&b, '\f'); break;
        case 'n':  luaL_addchar(&b, '\n'); break;
        case 'r':  luaL_addchar(&b, '\r'); break;
        case 't':  luaL_addchar(&b, '\t'); break;
        case 'u': {
          unsigned cp = 0;
          int k;
          if (j->end - j->p < 4) { jd_error(j, "a truncated \\u escape"); }
          for (k = 0; k < 4; k++) {
            char h = *j->p++;
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
            else { jd_error(j, "a \\u escape that is not four hex digits"); }
          }
          if (cp >= 0xD800 && cp <= 0xDBFF) {   /* high surrogate: need low */
            unsigned lo = 0;
            if (j->end - j->p < 6 || j->p[0] != '\\' || j->p[1] != 'u')
              jd_error(j, "a high surrogate with no low surrogate");
            j->p += 2;
            for (k = 0; k < 4; k++) {
              char h = *j->p++;
              lo <<= 4;
              if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
              else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
              else { jd_error(j, "a surrogate escape that is not four hex "
                                 "digits"); }
            }
            if (lo < 0xDC00 || lo > 0xDFFF)
              jd_error(j, "a high surrogate followed by a non-low one");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          else if (cp >= 0xDC00 && cp <= 0xDFFF)
            jd_error(j, "a low surrogate with no high surrogate");
          /* UTF-8 encode cp */
          if (cp < 0x80)
            luaL_addchar(&b, (char)cp);
          else if (cp < 0x800) {
            luaL_addchar(&b, (char)(0xc0 | (cp >> 6)));
            luaL_addchar(&b, (char)(0x80 | (cp & 0x3f)));
          }
          else if (cp < 0x10000) {
            luaL_addchar(&b, (char)(0xe0 | (cp >> 12)));
            luaL_addchar(&b, (char)(0x80 | ((cp >> 6) & 0x3f)));
            luaL_addchar(&b, (char)(0x80 | (cp & 0x3f)));
          }
          else {
            luaL_addchar(&b, (char)(0xf0 | (cp >> 18)));
            luaL_addchar(&b, (char)(0x80 | ((cp >> 12) & 0x3f)));
            luaL_addchar(&b, (char)(0x80 | ((cp >> 6) & 0x3f)));
            luaL_addchar(&b, (char)(0x80 | (cp & 0x3f)));
          }
          break;
        }
        default: jd_error(j, "an unknown backslash escape");
      }
    }
    else if (c < 0x20)
      jd_error(j, "a control character in a string; escape it");
    else
      luaL_addchar(&b, (char)c);
  }
  luaL_pushresult(&b);                   /* the decoded string */
}

/* The JSON number grammar, enforced: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?
   [0-9]+)?. Lenient acceptance here -- "-", "1.", "01" -- would silently
   decode to a wrong value, so each is a refusal. */
static void jd_number (jctx *j) {
  const char *start = j->p;
  int is_float = 0;
  char buf[64];
  size_t len;
  if (j->p < j->end && *j->p == '-') j->p++;
  if (j->p >= j->end || *j->p < '0' || *j->p > '9')
    jd_error(j, "a number with no digits");
  if (*j->p == '0') {
    j->p++;                             /* a lone zero; a leading zero is out */
    if (j->p < j->end && *j->p >= '0' && *j->p <= '9')
      jd_error(j, "a number with a leading zero");
  }
  else
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
  if (j->p < j->end && *j->p == '.') {
    is_float = 1; j->p++;
    if (j->p >= j->end || *j->p < '0' || *j->p > '9')
      jd_error(j, "a '.' with no fractional digits");
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
  }
  if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
    is_float = 1; j->p++;
    if (j->p < j->end && (*j->p == '+' || *j->p == '-')) j->p++;
    if (j->p >= j->end || *j->p < '0' || *j->p > '9')
      jd_error(j, "an exponent with no digits");
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
  }
  len = (size_t)(j->p - start);
  if (len >= sizeof(buf))
    jd_error(j, "a number too long to parse");
  memcpy(buf, start, len);
  buf[len] = '\0';
  if (!is_float) {
    char *ep;
    long long v;
    errno = 0;
    v = strtoll(buf, &ep, 10);
    if (errno == 0 && *ep == '\0') {    /* fits a 64-bit integer exactly */
      lua_pushinteger(j->L, (lua_Integer)v);
      return;
    }
    /* overflowed the integer: fall back to a float, so a huge id is inexact
       rather than an error */
  }
  lua_pushnumber(j->L, (lua_Number)strtod(buf, NULL));
}

static void jd_value (jctx *j, int depth) {
  if (depth > JSON_MAX_DEPTH)
    jd_error(j, "nesting past this decoder's depth");
  luaL_checkstack(j->L, 6, "json.decode");
  jd_ws(j);
  if (j->p >= j->end) { jd_error(j, "an empty document"); }
  switch (*j->p) {
    case '"':
      jd_string(j);
      return;
    case '{': {
      int first = 1;
      lua_newtable(j->L);
      j->p++;
      jd_ws(j);
      if (j->p < j->end && *j->p == '}') { j->p++; return; }
      for (;;) {
        jd_ws(j);
        if (j->p >= j->end || *j->p != '"')
          jd_error(j, "an object key that is not a string");
        jd_string(j);                   /* pushes the key */
        jd_ws(j);
        if (j->p >= j->end || *j->p != ':')
          jd_error(j, "an object member with no ':'");
        j->p++;
        jd_value(j, depth + 1);         /* pushes the value */
        lua_rawset(j->L, -3);           /* t[key] = value */
        jd_ws(j);
        if (j->p >= j->end) jd_error(j, "an object with no closing '}'");
        if (*j->p == ',') { j->p++; first = 0; continue; }
        if (*j->p == '}') { j->p++; break; }
        jd_error(j, "an object member not followed by ',' or '}'");
      }
      (void)first;
      return;
    }
    case '[': {
      lua_Integer i = 0;
      lua_newtable(j->L);
      j->p++;
      jd_ws(j);
      if (j->p < j->end && *j->p == ']') { j->p++; return; }
      for (;;) {
        jd_value(j, depth + 1);         /* pushes the element */
        lua_rawseti(j->L, -2, ++i);
        jd_ws(j);
        if (j->p >= j->end) jd_error(j, "an array with no closing ']'");
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == ']') { j->p++; break; }
        jd_error(j, "an array element not followed by ',' or ']'");
      }
      return;
    }
    case 't':
      if (j->end - j->p >= 4 && memcmp(j->p, "true", 4) == 0) {
        j->p += 4; lua_pushboolean(j->L, 1); return;
      }
      jd_error(j, "a bare 't' that is not 'true'");
      return;
    case 'f':
      if (j->end - j->p >= 5 && memcmp(j->p, "false", 5) == 0) {
        j->p += 5; lua_pushboolean(j->L, 0); return;
      }
      jd_error(j, "a bare 'f' that is not 'false'");
      return;
    case 'n':
      if (j->end - j->p >= 4 && memcmp(j->p, "null", 4) == 0) {
        j->p += 4; lua_pushnil(j->L); return;
      }
      jd_error(j, "a bare 'n' that is not 'null'");
      return;
    default:
      if (*j->p == '-' || (*j->p >= '0' && *j->p <= '9')) {
        jd_number(j);
        return;
      }
      jd_error(j, "a value that starts with no JSON token");
      return;
  }
}

static int json_decode (lua_State *L) {
  size_t n;
  const char *s = luaL_checklstring(L, 1, &n);
  jctx j;
  j.p = s;
  j.end = s + n;
  j.L = L;
  jd_value(&j, 0);
  jd_ws(&j);
  if (j.p != j.end)
    luaL_error(L, "json.decode: trailing bytes after the value");
  return 1;
}


/* ---- registration ---------------------------------------------------- */

static const luaL_Reg json_lib[] = {
  {"encode", json_encode},
  {"decode", json_decode},
  {NULL, NULL}
};

LUAMOD_API int luaopen_djson (lua_State *L) {
  luaL_newlib(L, json_lib);
  return 1;
}
