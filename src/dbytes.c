/*
** dbytes.c
** The 'bytes' guest library. See dbytes.h for why it is a library and not a
** hostcall.
**
** Six functions, three pairs:
**
**   bytes.tohex(s)         -> lowercase hex
**   bytes.fromhex(s)       -> the bytes, refusing an odd length or a non-digit
**   bytes.tobase64(s)      -> standard base64, padded
**   bytes.frombase64(s)    -> the bytes
**   bytes.tobase64url(s)   -> base64url, unpadded (the JWT/URL alphabet)
**   bytes.frombase64url(s) -> the bytes
**
** All operate on byte strings, because a Lua string already is one. The
** decoders are strict about the alphabet -- a character outside it is an error
** with a position, not a silently skipped byte -- and about a truncated final
** group (a length one past a multiple of four cannot be the encoding of any
** byte string). They are deliberately lenient about the unused low bits of a
** final group: a decoder that rejected non-canonical padding would refuse input
** that every other base64 reader accepts, and unlike the host's token parser
** (host/dhost_crypto.c, where strictness is a security property) this is a
** general utility whose job is to read what it is handed. Padding on the decode
** side is optional for both alphabets: '=' is consumed at the end and required
** nowhere.
*/

#define dbytes_c

#include "lprefix.h"

#include <stddef.h>

#include "lua.h"

#include "lauxlib.h"
#include "dbytes.h"


/* ---- hex ------------------------------------------------------------- */

static const char HEXD[] = "0123456789abcdef";

static int b_tohex (lua_State *L) {
  size_t n, i;
  const char *s = luaL_checklstring(L, 1, &n);
  luaL_Buffer b;
  char *out;
  if (n > (size_t)-1 / 2)
    return luaL_error(L, "bytes.tohex: string too long to encode");
  out = luaL_buffinitsize(L, &b, n * 2);
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    out[2 * i]     = HEXD[c >> 4];
    out[2 * i + 1] = HEXD[c & 0x0f];
  }
  luaL_pushresultsize(&b, n * 2);
  return 1;
}

static int hexval (int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int b_fromhex (lua_State *L) {
  size_t n, i;
  const char *s = luaL_checklstring(L, 1, &n);
  luaL_Buffer b;
  char *out;
  if (n % 2 != 0)
    return luaL_error(L, "bytes.fromhex: an odd number of hex digits (%d)",
                      (int)n);
  out = luaL_buffinitsize(L, &b, n / 2);
  for (i = 0; i < n; i += 2) {
    int hi = hexval((unsigned char)s[i]);
    int lo = hexval((unsigned char)s[i + 1]);
    if (hi < 0 || lo < 0)
      return luaL_error(L, "bytes.fromhex: a non-hex digit at position %d",
                        (int)(hi < 0 ? i : i + 1) + 1);
    out[i / 2] = (char)((hi << 4) | lo);
  }
  luaL_pushresultsize(&b, n / 2);
  return 1;
}


/* ---- base64 / base64url ---------------------------------------------- */

static const char B64STD[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char B64URL[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static void b64_encode (luaL_Buffer *b, const char *s, size_t n,
                        const char *alpha, int pad) {
  size_t i = 0;
  char q[4];
  while (i + 3 <= n) {
    unsigned v = ((unsigned char)s[i] << 16) |
                 ((unsigned char)s[i + 1] << 8) |
                  (unsigned char)s[i + 2];
    q[0] = alpha[(v >> 18) & 63]; q[1] = alpha[(v >> 12) & 63];
    q[2] = alpha[(v >> 6) & 63];  q[3] = alpha[v & 63];
    luaL_addlstring(b, q, 4);
    i += 3;
  }
  if (n - i == 1) {
    unsigned v = (unsigned)((unsigned char)s[i]) << 16;
    q[0] = alpha[(v >> 18) & 63]; q[1] = alpha[(v >> 12) & 63];
    luaL_addlstring(b, q, 2);
    if (pad) luaL_addlstring(b, "==", 2);
  }
  else if (n - i == 2) {
    unsigned v = ((unsigned char)s[i] << 16) | ((unsigned char)s[i + 1] << 8);
    q[0] = alpha[(v >> 18) & 63]; q[1] = alpha[(v >> 12) & 63];
    q[2] = alpha[(v >> 6) & 63];
    luaL_addlstring(b, q, 3);
    if (pad) luaL_addlstring(b, "=", 1);
  }
}

static int b64_val (int c, int url) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (url) { if (c == '-') return 62; if (c == '_') return 63; }
  else     { if (c == '+') return 62; if (c == '/') return 63; }
  return -1;
}

static int b64_decode (lua_State *L, int url, const char *who) {
  size_t n, i;
  const char *s = luaL_checklstring(L, 1, &n);
  luaL_Buffer b;
  unsigned acc = 0;
  int bits = 0;
  while (n > 0 && s[n - 1] == '=')       /* consume optional trailing padding */
    n--;
  luaL_buffinit(L, &b);
  for (i = 0; i < n; i++) {
    int v = b64_val((unsigned char)s[i], url);
    if (v < 0)                           /* an '=' that was not trailing lands here too */
      return luaL_error(L, "%s: an invalid character at position %d", who,
                        (int)i + 1);
    acc = (acc << 6) | (unsigned)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      luaL_addchar(&b, (char)((acc >> bits) & 0xff));
    }
  }
  /* 6 leftover bits means one dangling character -- a length one past a
     multiple of four, which is no byte string's encoding. 2 or 4 leftover bits
     are a final group's unused padding bits; drop them. */
  if (bits == 6)
    return luaL_error(L, "%s: a truncated group (one character too many)", who);
  luaL_pushresult(&b);
  return 1;
}

static int b_tobase64 (lua_State *L) {
  size_t n;
  const char *s = luaL_checklstring(L, 1, &n);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  b64_encode(&b, s, n, B64STD, 1);
  luaL_pushresult(&b);
  return 1;
}

static int b_tobase64url (lua_State *L) {
  size_t n;
  const char *s = luaL_checklstring(L, 1, &n);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  b64_encode(&b, s, n, B64URL, 0);
  luaL_pushresult(&b);
  return 1;
}

static int b_frombase64 (lua_State *L) {
  return b64_decode(L, 0, "bytes.frombase64");
}

static int b_frombase64url (lua_State *L) {
  return b64_decode(L, 1, "bytes.frombase64url");
}


/* ---- registration ---------------------------------------------------- */

static const luaL_Reg bytes_lib[] = {
  {"tohex",         b_tohex},
  {"fromhex",       b_fromhex},
  {"tobase64",      b_tobase64},
  {"frombase64",    b_frombase64},
  {"tobase64url",   b_tobase64url},
  {"frombase64url", b_frombase64url},
  {NULL, NULL}
};

LUAMOD_API int luaopen_dbytes (lua_State *L) {
  luaL_newlib(L, bytes_lib);
  return 1;
}
