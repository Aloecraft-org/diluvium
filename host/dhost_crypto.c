/*
** dhost_crypto.c
** The crypto connector: host:crypto/*. The primitives an API server needs to
** mint and check auth tokens, with the one property the whole system exists
** to give -- the signing key lives in the host and never in a guest. A
** program is granted host:crypto/jwt_sign, which is the right to ask for a
** signature, not the key; a compromised instance cannot exfiltrate a secret
** it was never handed, and the key is in neither its heap nor its snapshot.
**
**   crypto/random   {bytes=N}                 -> N CSPRNG bytes (a bin)
**   crypto/hash     {data=str}                -> lowercase hex of SHA-256
**   crypto/hmac     {data=str}                -> hex of HMAC-SHA256(key, data)
**   crypto/jwt_sign {claims=map, ttl=seconds} -> a JWT-HS256 string
**   crypto/jwt_verify {token=str}             -> {valid, claims?, reason?}
**
** Randomness through the hostcall log is the canonical replay win: the bytes
** arrive as a reply, so they are in the log, so a replay reproduces the run
** exactly while the tokens stay unpredictable to anyone without the key.
**
** JWT design notes, because the mistakes here are famous:
**  - The header is fixed, "{\"alg\":\"HS256\",\"typ\":\"JWT\"}", and verify
**    compares the header segment against its known base64url form rather than
**    parsing it. That closes alg-confusion (alg:none, alg:RS256) structurally
**    -- there is no header field a token can set to change how it is checked.
**  - The host owns iat and exp. jwt_sign drops any iat/exp/nbf a guest put in
**    its claims and injects its own from the host clock and ttl, so a guest
**    cannot mint a token that never expires.
**  - verify checks the HMAC before it decodes or parses anything, so the JSON
**    parser only ever runs on bytes this host signed with its own key.
*/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/random.h>

#include "dhash.h"
#include "dmsgpack.h"
#include "dhost.h"

#define CRYPTO_KEY_MAX     512
#define JWT_MAX_TOKEN      8192
#define JWT_MAX_JSON       6144
#define JSON_MAX_DEPTH     32

typedef struct dh_crypto {
  unsigned char key[CRYPTO_KEY_MAX];
  size_t keylen;
  long default_ttl;
} dh_crypto;


/* ---- CSPRNG ---------------------------------------------------------- */

/* Fill 'buf' with 'n' cryptographically secure bytes. 0 on success. Prefers
   getrandom(2); falls back to /dev/urandom for a kernel without it. */
static int csprng (unsigned char *buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = getrandom(buf + got, n - got, 0);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;                            /* fall through to /dev/urandom */
    }
    got += (size_t)r;
  }
  if (got == n)
    return 0;
  {
    FILE *f = fopen("/dev/urandom", "rb");
    if (f == NULL)
      return -1;
    while (got < n) {
      size_t r = fread(buf + got, 1, n - got, f);
      if (r == 0) { fclose(f); return -1; }
      got += r;
    }
    fclose(f);
  }
  return 0;
}


/* ---- HMAC-SHA256, over the runtime's own SHA-256 --------------------- */

static void hmac_sha256 (const unsigned char *key, size_t keylen,
                         const unsigned char *msg, size_t msglen,
                         unsigned char out[DILUVIUM_SHA256_SIZE]) {
  unsigned char k[64], ipad[64], opad[64];
  unsigned char inner[DILUVIUM_SHA256_SIZE];
  diluvium_sha256_ctx c;
  int i;
  memset(k, 0, sizeof(k));
  if (keylen > 64)
    diluvium_sha256(key, keylen, k);    /* a long key is its own hash */
  else
    memcpy(k, key, keylen);
  for (i = 0; i < 64; i++) {
    ipad[i] = (unsigned char)(k[i] ^ 0x36);
    opad[i] = (unsigned char)(k[i] ^ 0x5c);
  }
  diluvium_sha256_init(&c);
  diluvium_sha256_update(&c, ipad, 64);
  diluvium_sha256_update(&c, msg, msglen);
  diluvium_sha256_final(&c, inner);
  diluvium_sha256_init(&c);
  diluvium_sha256_update(&c, opad, 64);
  diluvium_sha256_update(&c, inner, DILUVIUM_SHA256_SIZE);
  diluvium_sha256_final(&c, out);
}

/* Length-independent-once-equal compare. Lengths are not secret, so a length
   mismatch returns early; equal lengths are compared without a data-dependent
   branch. Returns 1 on equal. */
static int ct_equal (const void *a, const void *b, size_t n) {
  const unsigned char *pa = (const unsigned char *)a;
  const unsigned char *pb = (const unsigned char *)b;
  unsigned char diff = 0;
  size_t i;
  for (i = 0; i < n; i++)
    diff |= (unsigned char)(pa[i] ^ pb[i]);
  return diff == 0;
}

static const char HEX[] = "0123456789abcdef";
static void to_hex (const unsigned char *in, size_t n, char *out) {
  size_t i;
  for (i = 0; i < n; i++) {
    out[2 * i] = HEX[in[i] >> 4];
    out[2 * i + 1] = HEX[in[i] & 0xf];
  }
  out[2 * n] = '\0';
}


/* ---- base64url (no padding) ----------------------------------------- */

static const char B64URL[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static void b64url_encode (const unsigned char *in, size_t n, dh_buf *out) {
  size_t i = 0;
  while (i + 3 <= n) {
    unsigned v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
    char q[4];
    q[0] = B64URL[(v >> 18) & 63]; q[1] = B64URL[(v >> 12) & 63];
    q[2] = B64URL[(v >> 6) & 63];  q[3] = B64URL[v & 63];
    dh_raw(out, q, 4);
    i += 3;
  }
  if (n - i == 1) {
    unsigned v = in[i] << 16;
    char q[2];
    q[0] = B64URL[(v >> 18) & 63]; q[1] = B64URL[(v >> 12) & 63];
    dh_raw(out, q, 2);
  }
  else if (n - i == 2) {
    unsigned v = (in[i] << 16) | (in[i + 1] << 8);
    char q[3];
    q[0] = B64URL[(v >> 18) & 63]; q[1] = B64URL[(v >> 12) & 63];
    q[2] = B64URL[(v >> 6) & 63];
    dh_raw(out, q, 3);
  }
}

static int b64_val (int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

/* Decode base64url into 'out' (cap bytes). Returns length, or -1 on any
   invalid character, padding, or overflow -- strict, because this runs on a
   token from outside. */
static long b64url_decode (const char *in, size_t n, unsigned char *out,
                           size_t cap) {
  size_t i = 0, o = 0;
  while (i < n) {
    int c0, c1, c2, c3, have;
    c0 = b64_val((unsigned char)in[i]);
    if (c0 < 0) return -1;
    if (i + 1 >= n) return -1;          /* a lone sextet is not a byte */
    c1 = b64_val((unsigned char)in[i + 1]);
    if (c1 < 0) return -1;
    have = (int)(n - i);
    if (have >= 4) {
      c2 = b64_val((unsigned char)in[i + 2]);
      c3 = b64_val((unsigned char)in[i + 3]);
      if (c2 < 0 || c3 < 0) return -1;
      if (o + 3 > cap) return -1;
      out[o++] = (unsigned char)((c0 << 2) | (c1 >> 4));
      out[o++] = (unsigned char)((c1 << 4) | (c2 >> 2));
      out[o++] = (unsigned char)((c2 << 6) | c3);
      i += 4;
    }
    else if (have == 3) {
      c2 = b64_val((unsigned char)in[i + 2]);
      if (c2 < 0) return -1;
      if ((c1 & 0x0f) != 0 && 0) {}     /* low bits need not be zero to decode */
      if (o + 2 > cap) return -1;
      out[o++] = (unsigned char)((c0 << 2) | (c1 >> 4));
      out[o++] = (unsigned char)((c1 << 4) | (c2 >> 2));
      i += 3;
    }
    else { /* have == 2 */
      if (o + 1 > cap) return -1;
      out[o++] = (unsigned char)((c0 << 2) | (c1 >> 4));
      i += 2;
    }
  }
  return (long)o;
}


/* ---- msgpack -> JSON (emit; our own data) --------------------------- */

static void json_escape (dh_buf *o, const char *s, size_t n) {
  size_t i;
  dh_raw(o, "\"", 1);
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '"':  dh_raw(o, "\\\"", 2); break;
      case '\\': dh_raw(o, "\\\\", 2); break;
      case '\b': dh_raw(o, "\\b", 2); break;
      case '\f': dh_raw(o, "\\f", 2); break;
      case '\n': dh_raw(o, "\\n", 2); break;
      case '\r': dh_raw(o, "\\r", 2); break;
      case '\t': dh_raw(o, "\\t", 2); break;
      default:
        if (c < 0x20) {
          char u[7];
          snprintf(u, sizeof(u), "\\u%04x", c);
          dh_raw(o, u, 6);
        }
        else
          dh_raw(o, &s[i], 1);          /* bytes >=0x20 pass; UTF-8 stays UTF-8 */
    }
  }
  dh_raw(o, "\"", 1);
}

static int key_is (const diluvium_mp_token *k, const char *name) {
  size_t n = strlen(name);
  return k->len == n && memcmp(k->p, name, n) == 0;
}

/* Emit one msgpack value at the cursor as JSON. Returns 0, or -1 on a value a
   JSON claim cannot carry (a non-string map key, a type with no JSON form) or
   on running past JSON_MAX_DEPTH. */
static int emit_json (diluvium_mp_cursor *c, dh_buf *o, int depth) {
  diluvium_mp_token t;
  if (depth > JSON_MAX_DEPTH)
    return -1;
  if (!diluvium_mp_read(c, &t))
    return -1;
  switch (t.kind) {
    case DILUVIUM_MP_NIL:  dh_raw(o, "null", 4); return 0;
    case DILUVIUM_MP_BOOL: dh_raw(o, t.b ? "true" : "false", t.b ? 4 : 5);
                           return 0;
    case DILUVIUM_MP_INT: {
      char b[24];
      int n = snprintf(b, sizeof(b), "%lld", (long long)t.i);
      dh_raw(o, b, (size_t)n);
      return 0;
    }
    case DILUVIUM_MP_FLOAT: {
      char b[32];
      int n = snprintf(b, sizeof(b), "%.17g", t.f);
      dh_raw(o, b, (size_t)n);
      return 0;
    }
    case DILUVIUM_MP_STR:
      json_escape(o, t.p, t.len);
      return 0;
    case DILUVIUM_MP_ARRAY: {
      size_t i, n = t.len;
      dh_raw(o, "[", 1);
      for (i = 0; i < n; i++) {
        if (i) dh_raw(o, ",", 1);
        if (emit_json(c, o, depth + 1) != 0) return -1;
      }
      dh_raw(o, "]", 1);
      return 0;
    }
    case DILUVIUM_MP_MAP: {
      size_t i, n = t.len;
      dh_raw(o, "{", 1);
      for (i = 0; i < n; i++) {
        diluvium_mp_token k;
        if (!diluvium_mp_read(c, &k) || k.kind != DILUVIUM_MP_STR)
          return -1;
        if (i) dh_raw(o, ",", 1);
        json_escape(o, k.p, k.len);
        dh_raw(o, ":", 1);
        if (emit_json(c, o, depth + 1) != 0) return -1;
      }
      dh_raw(o, "}", 1);
      return 0;
    }
    default:
      return -1;
  }
}


/* ---- JSON -> msgpack (parse; verified bytes, still strict) ---------- */

typedef struct jp { const char *p; const char *end; } jp;

static void jp_ws (jp *j) {
  while (j->p < j->end &&
         (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r'))
    j->p++;
}

static int parse_value (jp *j, dh_buf *out, int depth);

static int parse_string (jp *j, dh_buf *out) {
  dh_buf s;
  if (j->p >= j->end || *j->p != '"') return -1;
  j->p++;
  dh_buf_init(&s);
  for (;;) {
    unsigned char c;
    if (j->p >= j->end) { dh_buf_free(&s); return -1; }
    c = (unsigned char)*j->p++;
    if (c == '"') break;
    if (c == '\\') {
      if (j->p >= j->end) { dh_buf_free(&s); return -1; }
      switch (*j->p++) {
        case '"':  dh_raw(&s, "\"", 1); break;
        case '\\': dh_raw(&s, "\\", 1); break;
        case '/':  dh_raw(&s, "/", 1); break;
        case 'b':  dh_raw(&s, "\b", 1); break;
        case 'f':  dh_raw(&s, "\f", 1); break;
        case 'n':  dh_raw(&s, "\n", 1); break;
        case 'r':  dh_raw(&s, "\r", 1); break;
        case 't':  dh_raw(&s, "\t", 1); break;
        case 'u': {
          unsigned cp = 0;
          int k;
          if (j->end - j->p < 4) { dh_buf_free(&s); return -1; }
          for (k = 0; k < 4; k++) {
            char h = *j->p++;
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
            else { dh_buf_free(&s); return -1; }
          }
          if (cp >= 0xD800 && cp <= 0xDBFF) {   /* high surrogate: need low */
            unsigned lo = 0;
            if (j->end - j->p < 6 || j->p[0] != '\\' || j->p[1] != 'u') {
              dh_buf_free(&s); return -1;
            }
            j->p += 2;
            for (k = 0; k < 4; k++) {
              char h = *j->p++;
              lo <<= 4;
              if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
              else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
              else { dh_buf_free(&s); return -1; }
            }
            if (lo < 0xDC00 || lo > 0xDFFF) { dh_buf_free(&s); return -1; }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          else if (cp >= 0xDC00 && cp <= 0xDFFF) {  /* lone low surrogate */
            dh_buf_free(&s); return -1;
          }
          /* UTF-8 encode cp */
          if (cp < 0x80) {
            char b = (char)cp; dh_raw(&s, &b, 1);
          } else if (cp < 0x800) {
            char b[2];
            b[0] = (char)(0xc0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3f));
            dh_raw(&s, b, 2);
          } else if (cp < 0x10000) {
            char b[3];
            b[0] = (char)(0xe0 | (cp >> 12));
            b[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
            b[2] = (char)(0x80 | (cp & 0x3f));
            dh_raw(&s, b, 3);
          } else {
            char b[4];
            b[0] = (char)(0xf0 | (cp >> 18));
            b[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
            b[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
            b[3] = (char)(0x80 | (cp & 0x3f));
            dh_raw(&s, b, 4);
          }
          break;
        }
        default: dh_buf_free(&s); return -1;
      }
    }
    else if (c < 0x20) {                 /* control chars must be escaped */
      dh_buf_free(&s); return -1;
    }
    else {
      char ch = (char)c;
      dh_raw(&s, &ch, 1);
    }
  }
  dh_lstr(out, (const char *)s.p, s.len);   /* msgpack str of the decoded bytes */
  dh_buf_free(&s);
  return 0;
}

static int parse_number (jp *j, dh_buf *out) {
  const char *start = j->p;
  int is_float = 0;
  char buf[64];
  size_t len;
  if (j->p < j->end && *j->p == '-') j->p++;
  while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
  if (j->p < j->end && *j->p == '.') {
    is_float = 1; j->p++;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
  }
  if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
    is_float = 1; j->p++;
    if (j->p < j->end && (*j->p == '+' || *j->p == '-')) j->p++;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
  }
  len = (size_t)(j->p - start);
  if (len == 0 || len >= sizeof(buf)) return -1;
  memcpy(buf, start, len);
  buf[len] = '\0';
  if (is_float) {
    dh_double(out, strtod(buf, NULL));
  } else {
    /* Fits an int64? exp/iat always do. Overflow falls back to double. */
    errno = 0;
    {
      char *ep;
      long long v = strtoll(buf, &ep, 10);
      if (errno == 0 && *ep == '\0')
        dh_int(out, (int64_t)v);
      else
        dh_double(out, strtod(buf, NULL));
    }
  }
  return 0;
}

static int parse_value (jp *j, dh_buf *out, int depth) {
  if (depth > JSON_MAX_DEPTH) return -1;
  jp_ws(j);
  if (j->p >= j->end) return -1;
  switch (*j->p) {
    case '"': return parse_string(j, out);
    case '{': {
      size_t base = out->len, count = 0;
      dh_buf keysvals;
      dh_buf_init(&keysvals);
      j->p++;
      jp_ws(j);
      if (j->p < j->end && *j->p == '}') { j->p++; dh_map(out, 0);
                                           dh_buf_free(&keysvals); return 0; }
      for (;;) {
        jp_ws(j);
        if (parse_string(j, &keysvals) != 0) { dh_buf_free(&keysvals);
                                               return -1; }
        jp_ws(j);
        if (j->p >= j->end || *j->p != ':') { dh_buf_free(&keysvals);
                                              return -1; }
        j->p++;
        if (parse_value(j, &keysvals, depth + 1) != 0) {
          dh_buf_free(&keysvals); return -1;
        }
        count++;
        jp_ws(j);
        if (j->p >= j->end) { dh_buf_free(&keysvals); return -1; }
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == '}') { j->p++; break; }
        dh_buf_free(&keysvals);
        return -1;
      }
      (void)base;
      dh_map(out, (unsigned)count);
      if (keysvals.len > 0) dh_raw(out, keysvals.p, keysvals.len);
      dh_buf_free(&keysvals);
      return 0;
    }
    case '[': {
      size_t count = 0;
      dh_buf items;
      dh_buf_init(&items);
      j->p++;
      jp_ws(j);
      if (j->p < j->end && *j->p == ']') { j->p++; dh_array(out, 0);
                                           dh_buf_free(&items); return 0; }
      for (;;) {
        if (parse_value(j, &items, depth + 1) != 0) { dh_buf_free(&items);
                                                      return -1; }
        count++;
        jp_ws(j);
        if (j->p >= j->end) { dh_buf_free(&items); return -1; }
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == ']') { j->p++; break; }
        dh_buf_free(&items);
        return -1;
      }
      dh_array(out, (unsigned)count);
      if (items.len > 0) dh_raw(out, items.p, items.len);
      dh_buf_free(&items);
      return 0;
    }
    case 't':
      if (j->end - j->p >= 4 && memcmp(j->p, "true", 4) == 0) {
        j->p += 4; dh_bool(out, 1); return 0;
      }
      return -1;
    case 'f':
      if (j->end - j->p >= 5 && memcmp(j->p, "false", 5) == 0) {
        j->p += 5; dh_bool(out, 0); return 0;
      }
      return -1;
    case 'n':
      if (j->end - j->p >= 4 && memcmp(j->p, "null", 4) == 0) {
        j->p += 4; dh_nil(out); return 0;
      }
      return -1;
    default:
      if (*j->p == '-' || (*j->p >= '0' && *j->p <= '9'))
        return parse_number(j, out);
      return -1;
  }
}

/* Parse a whole JSON document into 'out' as msgpack, requiring a top-level
   object and no trailing bytes. 0 on success. */
static int json_to_msgpack (const char *s, size_t n, dh_buf *out) {
  jp j;
  j.p = s;
  j.end = s + n;
  jp_ws(&j);
  if (j.p >= j.end || *j.p != '{')
    return -1;                          /* a JWT payload is an object */
  if (parse_value(&j, out, 0) != 0)
    return -1;
  jp_ws(&j);
  return (j.p == j.end) ? 0 : -1;       /* trailing garbage is a refusal */
}


/* ---- the connector -------------------------------------------------- */

/* Fixed header segment: base64url of {"alg":"HS256","typ":"JWT"}. Compared as
   a constant on verify so no header field can change how a token is checked. */
static const char JWT_HEADER_B64[] =
  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";

static int64_t now_secs (void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t)ts.tv_sec;
}

/* Pull a top-level string field out of a claims map without emitting it. */
static int arg_map (const unsigned char *args, size_t argslen, const char *key,
                    diluvium_mp_cursor *out) {
  if (args == NULL) return 0;
  diluvium_mp_open(out, args, argslen);
  return diluvium_mp_field(out, key);
}

static dh_call_status do_random (dh_buf *value, const unsigned char *args,
                                 size_t argslen, char *detail, size_t dcap) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  long nbytes = 32;
  unsigned char buf[1024];
  char hex[2 * sizeof(buf) + 1];
  if (args != NULL) {
    diluvium_mp_open(&c, args, argslen);
    if (diluvium_mp_field(&c, "bytes") && diluvium_mp_read(&c, &t) &&
        t.kind == DILUVIUM_MP_INT)
      nbytes = (long)t.i;
  }
  if (nbytes < 1 || nbytes > (long)sizeof(buf)) {
    snprintf(detail, dcap, "crypto/random: bytes must be 1..%d",
             (int)sizeof(buf));
    return DH_CALL_ERROR;
  }
  if (csprng(buf, (size_t)nbytes) != 0) {
    snprintf(detail, dcap, "crypto/random: no entropy source");
    return DH_CALL_ERROR;
  }
  /* Hex, because a guest has no base64/hex library and a raw-byte string is
     awkward to carry: hex is what a token id or a nonce wants anyway. */
  to_hex(buf, (size_t)nbytes, hex);
  dh_str(value, hex);
  return DH_CALL_OK;
}

static dh_call_status do_hash (dh_buf *value, const unsigned char *args,
                               size_t argslen, char *detail, size_t dcap) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  unsigned char digest[DILUVIUM_SHA256_SIZE];
  char hex[DILUVIUM_SHA256_HEX];
  if (!arg_map(args, argslen, "data", &c) || !diluvium_mp_read(&c, &t) ||
      t.kind != DILUVIUM_MP_STR) {
    snprintf(detail, dcap, "crypto/hash: args.data must be a string");
    return DH_CALL_ERROR;
  }
  diluvium_sha256(t.p, t.len, digest);
  to_hex(digest, DILUVIUM_SHA256_SIZE, hex);
  dh_str(value, hex);
  return DH_CALL_OK;
}

static dh_call_status do_hmac (dh_crypto *k, dh_buf *value,
                               const unsigned char *args, size_t argslen,
                               char *detail, size_t dcap) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  unsigned char mac[DILUVIUM_SHA256_SIZE];
  char hex[DILUVIUM_SHA256_HEX];
  if (!arg_map(args, argslen, "data", &c) || !diluvium_mp_read(&c, &t) ||
      t.kind != DILUVIUM_MP_STR) {
    snprintf(detail, dcap, "crypto/hmac: args.data must be a string");
    return DH_CALL_ERROR;
  }
  hmac_sha256(k->key, k->keylen, (const unsigned char *)t.p, t.len, mac);
  to_hex(mac, DILUVIUM_SHA256_SIZE, hex);
  dh_str(value, hex);
  return DH_CALL_OK;
}

static dh_call_status do_jwt_sign (dh_crypto *k, dh_buf *value,
                                   const unsigned char *args, size_t argslen,
                                   char *detail, size_t dcap) {
  dh_buf payload, token;
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  int64_t iat = now_secs();
  long ttl = k->default_ttl;
  unsigned char mac[DILUVIUM_SHA256_SIZE];
  dh_call_status rc = DH_CALL_ERROR;

  if (args != NULL) {
    diluvium_mp_open(&c, args, argslen);
    if (diluvium_mp_field(&c, "ttl") && diluvium_mp_read(&c, &t) &&
        t.kind == DILUVIUM_MP_INT && t.i > 0 && t.i <= 315360000)
      ttl = (long)t.i;
  }

  /* Build the payload JSON: the guest's claims, minus any iat/exp/nbf it
     tried to set, plus the host's own iat and exp. */
  dh_buf_init(&payload);
  dh_raw(&payload, "{", 1);
  {
    int first = 1;
    if (arg_map(args, argslen, "claims", &c)) {
      if (!diluvium_mp_read(&c, &t) || t.kind != DILUVIUM_MP_MAP) {
        snprintf(detail, dcap, "crypto/jwt_sign: args.claims must be a map");
        dh_buf_free(&payload);
        return DH_CALL_ERROR;
      }
      {
        size_t i, n = t.len;
        for (i = 0; i < n; i++) {
          diluvium_mp_token key;
          if (!diluvium_mp_read(&c, &key) || key.kind != DILUVIUM_MP_STR) {
            snprintf(detail, dcap, "crypto/jwt_sign: claim keys must be "
                                   "strings");
            dh_buf_free(&payload);
            return DH_CALL_ERROR;
          }
          if (key_is(&key, "iat") || key_is(&key, "exp") ||
              key_is(&key, "nbf")) {
            diluvium_mp_skip(&c);        /* host owns these; drop the guest's */
            continue;
          }
          if (!first) dh_raw(&payload, ",", 1);
          first = 0;
          json_escape(&payload, key.p, key.len);
          dh_raw(&payload, ":", 1);
          if (emit_json(&c, &payload, 1) != 0) {
            snprintf(detail, dcap, "crypto/jwt_sign: a claim has no JSON form");
            dh_buf_free(&payload);
            return DH_CALL_ERROR;
          }
        }
      }
    }
    {
      char tail[64];
      int m = snprintf(tail, sizeof(tail), "%s\"iat\":%lld,\"exp\":%lld}",
                       first ? "" : ",", (long long)iat,
                       (long long)(iat + ttl));
      dh_raw(&payload, tail, (size_t)m);
    }
  }
  if (payload.len > JWT_MAX_JSON) {
    snprintf(detail, dcap, "crypto/jwt_sign: the claims are too large");
    dh_buf_free(&payload);
    return DH_CALL_ERROR;
  }

  /* token = header "." base64url(payload) "." base64url(hmac(header.payload)) */
  dh_buf_init(&token);
  dh_raw(&token, JWT_HEADER_B64, sizeof(JWT_HEADER_B64) - 1);
  dh_raw(&token, ".", 1);
  b64url_encode(payload.p, payload.len, &token);
  hmac_sha256(k->key, k->keylen, token.p, token.len, mac);
  dh_raw(&token, ".", 1);
  b64url_encode(mac, DILUVIUM_SHA256_SIZE, &token);

  if (token.len <= JWT_MAX_TOKEN) {
    dh_lstr(value, (const char *)token.p, token.len);
    rc = DH_CALL_OK;
  } else {
    snprintf(detail, dcap, "crypto/jwt_sign: the token is too large");
  }
  dh_buf_free(&payload);
  dh_buf_free(&token);
  return rc;
}

static void verify_fail (dh_buf *value, const char *reason) {
  dh_map(value, 2);
  dh_str(value, "valid"); dh_bool(value, 0);
  dh_str(value, "reason"); dh_str(value, reason);
}

static dh_call_status do_jwt_verify (dh_crypto *k, dh_buf *value,
                                     const unsigned char *args, size_t argslen,
                                     char *detail, size_t dcap) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  const char *tok;
  size_t toklen;
  const char *dot1, *dot2, *sigb64;
  size_t siglen;
  unsigned char mac[DILUVIUM_SHA256_SIZE];
  char expsig[64];
  dh_buf esig;
  unsigned char payload[JWT_MAX_JSON + 4];
  long plen;
  dh_buf claims;
  int64_t exp = 0, nbf = 0, now;
  int have_exp = 0, have_nbf = 0;

  if (!arg_map(args, argslen, "token", &c) || !diluvium_mp_read(&c, &t) ||
      t.kind != DILUVIUM_MP_STR) {
    snprintf(detail, dcap, "crypto/jwt_verify: args.token must be a string");
    return DH_CALL_ERROR;
  }
  tok = t.p;
  toklen = t.len;
  if (toklen > JWT_MAX_TOKEN) { verify_fail(value, "oversized"); return DH_CALL_OK; }

  /* Structure: exactly two dots, and the header segment is the one we emit. */
  dot1 = memchr(tok, '.', toklen);
  if (dot1 == NULL) { verify_fail(value, "malformed"); return DH_CALL_OK; }
  dot2 = memchr(dot1 + 1, '.', (size_t)(tok + toklen - (dot1 + 1)));
  if (dot2 == NULL) { verify_fail(value, "malformed"); return DH_CALL_OK; }
  if ((size_t)(dot1 - tok) != sizeof(JWT_HEADER_B64) - 1 ||
      memcmp(tok, JWT_HEADER_B64, sizeof(JWT_HEADER_B64) - 1) != 0) {
    verify_fail(value, "alg");           /* not our HS256 header */
    return DH_CALL_OK;
  }

  /* HMAC over "header.payload" -- everything before the last dot -- and a
     constant-time compare against the token's signature, BEFORE decoding or
     parsing anything. */
  hmac_sha256(k->key, k->keylen, (const unsigned char *)tok,
              (size_t)(dot2 - tok), mac);
  dh_buf_init(&esig);
  b64url_encode(mac, DILUVIUM_SHA256_SIZE, &esig);
  memcpy(expsig, esig.p, esig.len < sizeof(expsig) ? esig.len : sizeof(expsig));
  siglen = esig.len;
  dh_buf_free(&esig);
  sigb64 = dot2 + 1;
  {
    size_t got = (size_t)(tok + toklen - sigb64);
    if (got != siglen || !ct_equal(sigb64, expsig, siglen)) {
      verify_fail(value, "signature");
      return DH_CALL_OK;
    }
  }

  /* Signature good: the payload is bytes we signed. Decode and parse. */
  plen = b64url_decode(dot1 + 1, (size_t)(dot2 - (dot1 + 1)), payload,
                       sizeof(payload));
  if (plen < 0) { verify_fail(value, "payload"); return DH_CALL_OK; }
  dh_buf_init(&claims);
  if (json_to_msgpack((const char *)payload, (size_t)plen, &claims) != 0) {
    dh_buf_free(&claims);
    verify_fail(value, "payload");
    return DH_CALL_OK;
  }

  /* exp/nbf against the host clock -- the host owns the time. */
  {
    diluvium_mp_cursor cc;
    diluvium_mp_token tt;
    diluvium_mp_open(&cc, claims.p, claims.len);
    if (diluvium_mp_field(&cc, "exp") && diluvium_mp_read(&cc, &tt) &&
        tt.kind == DILUVIUM_MP_INT) { exp = tt.i; have_exp = 1; }
    diluvium_mp_open(&cc, claims.p, claims.len);
    if (diluvium_mp_field(&cc, "nbf") && diluvium_mp_read(&cc, &tt) &&
        tt.kind == DILUVIUM_MP_INT) { nbf = tt.i; have_nbf = 1; }
  }
  now = now_secs();
  if (have_exp && now >= exp) { dh_buf_free(&claims);
                                verify_fail(value, "expired");
                                return DH_CALL_OK; }
  if (have_nbf && now < nbf)  { dh_buf_free(&claims);
                                verify_fail(value, "not_yet_valid");
                                return DH_CALL_OK; }

  dh_map(value, 2);
  dh_str(value, "valid"); dh_bool(value, 1);
  dh_str(value, "claims");
  dh_raw(value, claims.p, claims.len);
  dh_buf_free(&claims);
  (void)detail; (void)dcap;
  return DH_CALL_OK;
}

static dh_call_status conn_crypto (void *ud, dvs_id id, const char *call,
                                   const unsigned char *args, size_t argslen,
                                   dh_buf *value, char *detail,
                                   size_t detailcap) {
  dh_crypto *k = (dh_crypto *)ud;
  (void)id;
  if (strcmp(call, "crypto/random") == 0)
    return do_random(value, args, argslen, detail, detailcap);
  if (strcmp(call, "crypto/hash") == 0)
    return do_hash(value, args, argslen, detail, detailcap);
  if (strcmp(call, "crypto/hmac") == 0)
    return do_hmac(k, value, args, argslen, detail, detailcap);
  if (strcmp(call, "crypto/jwt_sign") == 0)
    return do_jwt_sign(k, value, args, argslen, detail, detailcap);
  if (strcmp(call, "crypto/jwt_verify") == 0)
    return do_jwt_verify(k, value, args, argslen, detail, detailcap);
  snprintf(detail, detailcap, "the crypto connector has no call '%s'", call);
  return DH_CALL_DENIED;
}


/* ---- open / close --------------------------------------------------- */

int dh_crypto_open (dh_host *h, char *err, size_t errcap) {
  dh_crypto *k = (dh_crypto *)calloc(1, sizeof(dh_crypto));
  const dh_crypto_cfg *cfg = &h->cfg.crypto;
  if (k == NULL) {
    snprintf(err, errcap, "no memory for the crypto connector");
    return -1;
  }
  k->default_ttl = cfg->default_ttl;
  if (cfg->key_file[0] != '\0') {
    FILE *f = fopen(cfg->key_file, "rb");
    size_t n;
    if (f == NULL) {
      snprintf(err, errcap, "crypto: cannot read key_file '%s'", cfg->key_file);
      free(k);
      return -1;
    }
    n = fread(k->key, 1, sizeof(k->key), f);
    fclose(f);
    /* Trim one trailing newline, the common shape of a key file. */
    if (n > 0 && k->key[n - 1] == '\n') n--;
    k->keylen = n;
  }
  else if (cfg->key_env[0] != '\0') {
    const char *v = getenv(cfg->key_env);
    if (v == NULL) {
      snprintf(err, errcap, "crypto: env var '%s' (key_env) is not set",
               cfg->key_env);
      free(k);
      return -1;
    }
    k->keylen = strlen(v);
    if (k->keylen > sizeof(k->key)) k->keylen = sizeof(k->key);
    memcpy(k->key, v, k->keylen);
  }
  else if (cfg->keylen > 0) {
    k->keylen = cfg->keylen;
    memcpy(k->key, cfg->key, cfg->keylen);
  }
  if (k->keylen < 16) {
    snprintf(err, errcap, "crypto: the signing key is missing or shorter than "
                          "16 bytes (set one of key_file, key_env, key)");
    free(k);
    return -1;
  }
  if (dh_register(h, "crypto", conn_crypto, k) != 0) {
    snprintf(err, errcap, "could not register the crypto connector");
    free(k);
    return -1;
  }
  h->cryptoctx = k;
  return 0;
}

void dh_crypto_close (dh_host *h) {
  dh_crypto *k = (dh_crypto *)h->cryptoctx;
  if (k == NULL)
    return;
  /* The key does not linger in freed memory. */
  memset(k, 0, sizeof(*k));
  free(k);
  h->cryptoctx = NULL;
}
