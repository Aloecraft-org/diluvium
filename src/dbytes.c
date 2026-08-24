/*
** dbytes.c
** The 'bytes' guest library. See dbytes.h for why it is a library and not a
** hostcall.
**
** The codec pairs:
**
**   bytes.tohex(s)         -> lowercase hex
**   bytes.fromhex(s)       -> the bytes, refusing an odd length or a non-digit
**   bytes.tobase64(s)      -> standard base64, padded
**   bytes.frombase64(s)    -> the bytes
**   bytes.tobase64url(s)   -> base64url, unpadded (the JWT/URL alphabet)
**   bytes.frombase64url(s) -> the bytes
**
** The digests, raw bytes in and raw bytes out (compose with the codecs):
**
**   bytes.sha256(s)          -> 32 bytes
**   bytes.sha1(s)            -> 20 bytes; interop only, see dhash.h
**   bytes.hmac_sha256(k, s)  -> 32 bytes
**   bytes.hmac_sha1(k, s)    -> 20 bytes
**   bytes.consteq(a, b)      -> boolean, compared without a data-dependent
**                               branch once lengths agree
**
** These are here and not in the crypto connector because their key -- when
** they have one -- is the caller's: a per-user TOTP secret out of the
** program's own database, a content hash with no key at all. The connector's
** premise is a key the host holds and a guest must never see; a guest-held
** key gains nothing from a round-trip, and costs the log a copy of it. Raw
** bytes out, unlike the connector's hex, because composition is the point:
** TOTP truncates the raw MAC, a TURN password base64s it, a webhook check
** hexes it -- the program picks the codec it needs.
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
#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "dbytes.h"
#include "dhash.h"


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


/* ---- percent-encoding (RFC 3986) ------------------------------------ */

/* The one place hex is uppercase: RFC 3986 says a producer should emit
   %XX in upper case, even though a reader must accept either. */
static const char HEXU[] = "0123456789ABCDEF";

static int is_unreserved (int c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') ||
         c == '-' || c == '.' || c == '_' || c == '~';
}

static int b_urlencode (lua_State *L) {
  size_t n, i;
  const char *s = luaL_checklstring(L, 1, &n);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (is_unreserved(c))
      luaL_addchar(&b, (char)c);
    else {
      char e[3];
      e[0] = '%'; e[1] = HEXU[c >> 4]; e[2] = HEXU[c & 0x0f];
      luaL_addlstring(&b, e, 3);
    }
  }
  luaL_pushresult(&b);
  return 1;
}

/* Decode %XX escapes. Generic RFC 3986: '+' is a literal plus, not a space --
   that is the application/x-www-form-urlencoded convention, and a query value
   that wants it does a '+' -> ' ' pass of its own first. A truncated or
   non-hex escape is an error, not a passed-through '%'. */
static int b_urldecode (lua_State *L) {
  size_t n, i;
  const char *s = luaL_checklstring(L, 1, &n);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  for (i = 0; i < n; i++) {
    if (s[i] == '%') {
      int hi, lo;
      if (i + 2 >= n)
        return luaL_error(L, "bytes.urldecode: a truncated %% escape at "
                          "position %d", (int)i + 1);
      hi = hexval((unsigned char)s[i + 1]);
      lo = hexval((unsigned char)s[i + 2]);
      if (hi < 0 || lo < 0)
        return luaL_error(L, "bytes.urldecode: a non-hex %% escape at "
                          "position %d", (int)i + 1);
      luaL_addchar(&b, (char)((hi << 4) | lo));
      i += 2;
    }
    else
      luaL_addchar(&b, s[i]);
  }
  luaL_pushresult(&b);
  return 1;
}


/* ---- digests --------------------------------------------------------- */

static int b_sha256 (lua_State *L) {
  size_t n;
  const char *s = luaL_checklstring(L, 1, &n);
  unsigned char d[DILUVIUM_SHA256_SIZE];
  diluvium_sha256(s, n, d);
  lua_pushlstring(L, (const char *)d, sizeof(d));
  return 1;
}

static int b_sha1 (lua_State *L) {
  size_t n;
  const char *s = luaL_checklstring(L, 1, &n);
  unsigned char d[DILUVIUM_SHA1_SIZE];
  diluvium_sha1(s, n, d);
  lua_pushlstring(L, (const char *)d, sizeof(d));
  return 1;
}

/*
** HMAC (RFC 2104) over either digest; both have a 64-byte block, so one
** skeleton parameterised by the three hash calls serves both. The host's
** connector carries its own copy (host/dhost_crypto.c) rather than calling
** this one, because this file is a guest library and that one must build
** with no Lua state in sight.
*/
static void b_hmac (int sha1, const unsigned char *key, size_t keylen,
                    const unsigned char *msg, size_t msglen,
                    unsigned char *out) {
  unsigned char k[64], ipad[64], opad[64];
  unsigned char inner[DILUVIUM_SHA256_SIZE];    /* fits either digest */
  size_t dlen = sha1 ? DILUVIUM_SHA1_SIZE : DILUVIUM_SHA256_SIZE;
  size_t i;
  memset(k, 0, sizeof(k));
  if (keylen > 64) {
    if (sha1) diluvium_sha1(key, keylen, k);
    else diluvium_sha256(key, keylen, k);
  }
  else
    memcpy(k, key, keylen);
  for (i = 0; i < 64; i++) {
    ipad[i] = k[i] ^ 0x36;
    opad[i] = k[i] ^ 0x5c;
  }
  if (sha1) {
    diluvium_sha1_ctx c;
    diluvium_sha1_init(&c);
    diluvium_sha1_update(&c, ipad, 64);
    diluvium_sha1_update(&c, msg, msglen);
    diluvium_sha1_final(&c, inner);
    diluvium_sha1_init(&c);
    diluvium_sha1_update(&c, opad, 64);
    diluvium_sha1_update(&c, inner, dlen);
    diluvium_sha1_final(&c, out);
  }
  else {
    diluvium_sha256_ctx c;
    diluvium_sha256_init(&c);
    diluvium_sha256_update(&c, ipad, 64);
    diluvium_sha256_update(&c, msg, msglen);
    diluvium_sha256_final(&c, inner);
    diluvium_sha256_init(&c);
    diluvium_sha256_update(&c, opad, 64);
    diluvium_sha256_update(&c, inner, dlen);
    diluvium_sha256_final(&c, out);
  }
  memset(k, 0, sizeof(k));
  memset(ipad, 0, sizeof(ipad));
  memset(opad, 0, sizeof(opad));
}

static int b_hmac_sha256 (lua_State *L) {
  size_t kn, mn;
  const char *key = luaL_checklstring(L, 1, &kn);
  const char *msg = luaL_checklstring(L, 2, &mn);
  unsigned char mac[DILUVIUM_SHA256_SIZE];
  b_hmac(0, (const unsigned char *)key, kn,
         (const unsigned char *)msg, mn, mac);
  lua_pushlstring(L, (const char *)mac, sizeof(mac));
  return 1;
}

static int b_hmac_sha1 (lua_State *L) {
  size_t kn, mn;
  const char *key = luaL_checklstring(L, 1, &kn);
  const char *msg = luaL_checklstring(L, 2, &mn);
  unsigned char mac[DILUVIUM_SHA1_SIZE];
  b_hmac(1, (const unsigned char *)key, kn,
         (const unsigned char *)msg, mn, mac);
  lua_pushlstring(L, (const char *)mac, sizeof(mac));
  return 1;
}

/*
** Equal-or-not without a data-dependent branch once the lengths agree.
** Lengths are not treated as secret (a MAC's length is public), so a
** mismatch returns early. For comparing MACs, tokens and pins, where Lua's
** '==' would stop at the first differing byte and time the answer.
*/
static int b_consteq (lua_State *L) {
  size_t an, bn, i;
  const char *a = luaL_checklstring(L, 1, &an);
  const char *b = luaL_checklstring(L, 2, &bn);
  unsigned char diff = 0;
  if (an != bn) {
    lua_pushboolean(L, 0);
    return 1;
  }
  for (i = 0; i < an; i++)
    diff |= (unsigned char)((unsigned char)a[i] ^ (unsigned char)b[i]);
  lua_pushboolean(L, diff == 0);
  return 1;
}


/* ---- registration ---------------------------------------------------- */

static const luaL_Reg bytes_lib[] = {
  {"tohex",         b_tohex},
  {"fromhex",       b_fromhex},
  {"tobase64",      b_tobase64},
  {"frombase64",    b_frombase64},
  {"tobase64url",   b_tobase64url},
  {"frombase64url", b_frombase64url},
  {"urlencode",     b_urlencode},
  {"urldecode",     b_urldecode},
  {"sha256",        b_sha256},
  {"sha1",          b_sha1},
  {"hmac_sha256",   b_hmac_sha256},
  {"hmac_sha1",     b_hmac_sha1},
  {"consteq",       b_consteq},
  {NULL, NULL}
};

LUAMOD_API int luaopen_dbytes (lua_State *L) {
  luaL_newlib(L, bytes_lib);
  return 1;
}
