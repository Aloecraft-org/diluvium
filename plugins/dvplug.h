/*
** dvplug.h
** A single-header kit for writing a Diluvium plugin in C.
**
** This is NOT part of the runtime and includes nothing from it. It is a
** convenience for plugin authors, and the fact that it can exist as one
** dependency-free header is the argument for the protocol being a protocol:
** doc/BUILD8.md section 2.4 is a length-prefixed msgpack frame on fd 3, and
** that is small enough to implement in four hundred lines with no library.
**
** If you would rather not use it, don't -- test/plugin_echo.c implements the
** same protocol from scratch in about sixty lines, on purpose, so that the
** claim "a plugin needs the protocol and not a header" stays checkable.
**
** Copy this file into your plugin's source tree. It has no version and no
** ABI; it is a starting point, not a dependency.
**
** Usage:
**
**   #define DVPLUG_IMPL
**   #include "dvplug.h"
**
**   static void on_call (dv_req *q) {
**     char url[1024];
**     if (!dv_arg_str(q, "url", url, sizeof(url)))
**       { dv_error(q, DV_ERR_PLUGIN, "bad_args", "no url"); return; }
**     dv_ok_begin(q, 2);
**     dv_key(q, "status"); dv_int(q, 200);
**     dv_key(q, "body");   dv_bin(q, data, len);
**     dv_send(q);
**   }
**   int main (void) { return dv_serve(on_call); }
**
** The frame you answer must be answered exactly once. dv_serve does not
** enforce that -- the host does, and a second reply for one id is simply
** dropped there -- but a plugin that returns from on_call without calling
** dv_send or dv_error leaves the guest waiting for its own timeout.
*/

#ifndef DVPLUG_H
#define DVPLUG_H

#include <stddef.h>
#include <stdint.h>

#define DVPLUG_CHANNEL_FD 3
#define DVPLUG_FRAME_MAX  (8u * 1024u * 1024u)
#define DVPLUG_VERSION    1

/* The three error classes. They are not decoration: the guest's retry
** decision differs for each, which is why the protocol carries a class and
** not just a string. */
#define DV_ERR_TRANSPORT  "transport"   /* the channel or this process failed */
#define DV_ERR_PLUGIN     "plugin"      /* this program is wrong */
#define DV_ERR_CAPABILITY "capability"  /* the service behind it said no */

typedef struct dv_req {
  uint64_t id;
  char target[64];
  const unsigned char *args;            /* NULL when the call sent none */
  const unsigned char *end;             /* one past the frame body */
  /* reply accumulator; use the dv_* emitters below */
  unsigned char *out;
  size_t olen, ocap;
  int answered;
} dv_req;

typedef void (*dv_handler) (dv_req *q);

/* Read frames off fd 3 and dispatch until the host closes the channel.
   Returns 0 on a clean shutdown, nonzero on a protocol or IO failure. */
int dv_serve (dv_handler fn);

/* ---- reading arguments ---- */
int dv_arg_str (dv_req *q, const char *key, char *out, size_t cap);
int dv_arg_int (dv_req *q, const char *key, int64_t *out);
int dv_arg_bin (dv_req *q, const char *key, const unsigned char **p,
                size_t *len);
/* The value of 'key' as a msgpack map, for walking a nested object. Returns
   NULL when absent or not a map; 'n' receives the pair count. */
const unsigned char *dv_arg_map (dv_req *q, const char *key, uint32_t *n);
/* Step through a map's pairs: returns the value pointer and fills 'key'. */
const unsigned char *dv_map_next (dv_req *q, const unsigned char *p,
                                  char *key, size_t keycap);
const unsigned char *dv_skip (dv_req *q, const unsigned char *p);

/* ---- writing a reply ---- */
/* Begin a successful reply whose value is a map of 'nfields' pairs. */
void dv_ok_begin (dv_req *q, unsigned nfields);
/* Begin a successful reply whose value is a single scalar you then emit. */
void dv_ok_scalar (dv_req *q);
void dv_key (dv_req *q, const char *k);
void dv_str (dv_req *q, const char *s);
void dv_lstr (dv_req *q, const char *s, size_t n);
void dv_bin (dv_req *q, const void *p, size_t n);
void dv_int (dv_req *q, int64_t v);
void dv_bool (dv_req *q, int b);
void dv_nil (dv_req *q);
void dv_map (dv_req *q, unsigned n);
void dv_array (dv_req *q, unsigned n);
/* Send what has been built. */
int dv_send (dv_req *q);
/* Answer with an error instead. 'code' is one of the capability's declared
   error enums, or NULL. */
int dv_error (dv_req *q, const char *cls, const char *code, const char *msg);

#endif /* DVPLUG_H */


#ifdef DVPLUG_IMPL
#ifndef DVPLUG_IMPL_ONCE
#define DVPLUG_IMPL_ONCE

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ read */

static uint64_t dvp_be (const unsigned char *p, int n) {
  uint64_t v = 0;
  int i;
  for (i = 0; i < n; i++) v = (v << 8) | p[i];
  return v;
}

static const unsigned char *dvp_skip (const unsigned char *p,
                                      const unsigned char *end);

static const unsigned char *dvp_skipn (const unsigned char *p,
                                       const unsigned char *end, uint64_t n) {
  while (n-- > 0) {
    p = dvp_skip(p, end);
    if (p == NULL) return NULL;
  }
  return p;
}

static const unsigned char *dvp_skip (const unsigned char *p,
                                      const unsigned char *end) {
  unsigned char c;
  if (p == NULL || p >= end) return NULL;
  c = *p++;
  if (c <= 0x7f || c >= 0xe0) return p;
  if ((c & 0xe0) == 0xa0) { uint64_t n = c & 0x1f;
                            return (p + n <= end) ? p + n : NULL; }
  if ((c & 0xf0) == 0x80) return dvp_skipn(p, end, (uint64_t)(c & 0x0f) * 2);
  if ((c & 0xf0) == 0x90) return dvp_skipn(p, end, c & 0x0f);
  switch (c) {
    case 0xc0: case 0xc2: case 0xc3: return p;
    case 0xcc: case 0xd0: return (p + 1 <= end) ? p + 1 : NULL;
    case 0xcd: case 0xd1: return (p + 2 <= end) ? p + 2 : NULL;
    case 0xce: case 0xd2: case 0xca: return (p + 4 <= end) ? p + 4 : NULL;
    case 0xcf: case 0xd3: case 0xcb: return (p + 8 <= end) ? p + 8 : NULL;
    case 0xc4: case 0xd9: { uint64_t n; if (p + 1 > end) return NULL;
                            n = dvp_be(p, 1); p += 1;
                            return (p + n <= end) ? p + n : NULL; }
    case 0xc5: case 0xda: { uint64_t n; if (p + 2 > end) return NULL;
                            n = dvp_be(p, 2); p += 2;
                            return (p + n <= end) ? p + n : NULL; }
    case 0xc6: case 0xdb: { uint64_t n; if (p + 4 > end) return NULL;
                            n = dvp_be(p, 4); p += 4;
                            return (p + n <= end) ? p + n : NULL; }
    case 0xdc: { uint64_t n; if (p + 2 > end) return NULL;
                 n = dvp_be(p, 2); p += 2; return dvp_skipn(p, end, n); }
    case 0xdd: { uint64_t n; if (p + 4 > end) return NULL;
                 n = dvp_be(p, 4); p += 4; return dvp_skipn(p, end, n); }
    case 0xde: { uint64_t n; if (p + 2 > end) return NULL;
                 n = dvp_be(p, 2); p += 2; return dvp_skipn(p, end, n * 2); }
    case 0xdf: { uint64_t n; if (p + 4 > end) return NULL;
                 n = dvp_be(p, 4); p += 4; return dvp_skipn(p, end, n * 2); }
    default: return NULL;
  }
}

const unsigned char *dv_skip (dv_req *q, const unsigned char *p) {
  return dvp_skip(p, q->end);
}

/* String-ish header: returns the data pointer and length for str and bin. */
static const unsigned char *dvp_strval (const unsigned char *p,
                                        const unsigned char *end,
                                        uint64_t *len) {
  if (p == NULL || p >= end) return NULL;
  if ((*p & 0xe0) == 0xa0) { *len = *p & 0x1f; return p + 1; }
  switch (*p) {
    case 0xd9: case 0xc4:
      if (p + 2 > end) return NULL;
      *len = p[1];
      return p + 2;
    case 0xda: case 0xc5:
      if (p + 3 > end) return NULL;
      *len = dvp_be(p + 1, 2);
      return p + 3;
    case 0xdb: case 0xc6:
      if (p + 5 > end) return NULL;
      *len = dvp_be(p + 1, 4);
      return p + 5;
    default: return NULL;
  }
}

static const unsigned char *dvp_mapopen (const unsigned char *p,
                                         const unsigned char *end,
                                         uint32_t *n) {
  if (p == NULL || p >= end) return NULL;
  if ((*p & 0xf0) == 0x80) { *n = *p & 0x0f; return p + 1; }
  if (*p == 0xde) { if (p + 3 > end) return NULL;
                    *n = (uint32_t)dvp_be(p + 1, 2); return p + 3; }
  if (*p == 0xdf) { if (p + 5 > end) return NULL;
                    *n = (uint32_t)dvp_be(p + 1, 4); return p + 5; }
  return NULL;
}

static const unsigned char *dvp_field (const unsigned char *map,
                                       const unsigned char *end,
                                       const char *key) {
  uint32_t n, i;
  const unsigned char *p = dvp_mapopen(map, end, &n);
  size_t klen = strlen(key);
  if (p == NULL) return NULL;
  for (i = 0; i < n; i++) {
    uint64_t kl = 0;
    const unsigned char *ks = dvp_strval(p, end, &kl);
    p = dvp_skip(p, end);
    if (p == NULL) return NULL;
    if (ks != NULL && kl == klen && memcmp(ks, key, klen) == 0)
      return p;
    p = dvp_skip(p, end);
    if (p == NULL) return NULL;
  }
  return NULL;
}

int dv_arg_str (dv_req *q, const char *key, char *out, size_t cap) {
  uint64_t n = 0;
  const unsigned char *s;
  if (q->args == NULL) return 0;
  s = dvp_strval(dvp_field(q->args, q->end, key), q->end, &n);
  if (s == NULL || s + n > q->end || n + 1 > cap) return 0;
  memcpy(out, s, (size_t)n);
  out[n] = '\0';
  return 1;
}

int dv_arg_bin (dv_req *q, const char *key, const unsigned char **p,
                size_t *len) {
  uint64_t n = 0;
  const unsigned char *s;
  if (q->args == NULL) return 0;
  s = dvp_strval(dvp_field(q->args, q->end, key), q->end, &n);
  if (s == NULL || s + n > q->end) return 0;
  *p = s;
  *len = (size_t)n;
  return 1;
}

int dv_arg_int (dv_req *q, const char *key, int64_t *out) {
  const unsigned char *p;
  if (q->args == NULL) return 0;
  p = dvp_field(q->args, q->end, key);
  if (p == NULL || p >= q->end) return 0;
  if (*p <= 0x7f) { *out = *p; return 1; }
  if (*p >= 0xe0) { *out = (int8_t)*p; return 1; }
  switch (*p) {
    case 0xcc:
      if (p + 2 > q->end) return 0;
      *out = (int64_t)dvp_be(p+1,1);
      return 1;
    case 0xcd:
      if (p + 3 > q->end) return 0;
      *out = (int64_t)dvp_be(p+1,2);
      return 1;
    case 0xce:
      if (p + 5 > q->end) return 0;
      *out = (int64_t)dvp_be(p+1,4);
      return 1;
    case 0xcf:
      if (p + 9 > q->end) return 0;
      *out = (int64_t)dvp_be(p+1,8);
      return 1;
    case 0xd0:
      if (p + 2 > q->end) return 0;
      *out = (int8_t)p[1];
      return 1;
    case 0xd1:
      if (p + 3 > q->end) return 0;
      *out = (int16_t)dvp_be(p+1,2);
      return 1;
    case 0xd2:
      if (p + 5 > q->end) return 0;
      *out = (int32_t)dvp_be(p+1,4);
      return 1;
    case 0xd3:
      if (p + 9 > q->end) return 0;
      *out = (int64_t)dvp_be(p+1,8);
      return 1;
    default: return 0;
  }
}

const unsigned char *dv_arg_map (dv_req *q, const char *key, uint32_t *n) {
  if (q->args == NULL) return NULL;
  return dvp_mapopen(dvp_field(q->args, q->end, key), q->end, n);
}

const unsigned char *dv_map_next (dv_req *q, const unsigned char *p,
                                  char *key, size_t keycap) {
  uint64_t kl = 0;
  const unsigned char *ks = dvp_strval(p, q->end, &kl);
  const unsigned char *v;
  key[0] = '\0';
  if (ks != NULL && kl + 1 <= keycap && ks + kl <= q->end) {
    memcpy(key, ks, (size_t)kl);
    key[kl] = '\0';
  }
  v = dvp_skip(p, q->end);
  return v;
}

/* ----------------------------------------------------------------- write */

static void dvp_raw (dv_req *q, const void *p, size_t n) {
  if (q->olen + n > q->ocap) {
    size_t want = (q->ocap == 0) ? 4096 : q->ocap;
    unsigned char *b;
    while (want < q->olen + n) want *= 2;
    if (want > DVPLUG_FRAME_MAX) return;
    b = (unsigned char *)realloc(q->out, want);
    if (b == NULL) return;
    q->out = b;
    q->ocap = want;
  }
  memcpy(q->out + q->olen, p, n);
  q->olen += n;
}

static void dvp_byte (dv_req *q, unsigned char c) { dvp_raw(q, &c, 1); }

static void dvp_be_out (dv_req *q, uint64_t v, int n) {
  int i;
  for (i = n - 1; i >= 0; i--) dvp_byte(q, (unsigned char)(v >> (i * 8)));
}

void dv_map (dv_req *q, unsigned n) {
  if (n < 16) dvp_byte(q, (unsigned char)(0x80 | n));
  else { dvp_byte(q, 0xde); dvp_be_out(q, n, 2); }
}

void dv_array (dv_req *q, unsigned n) {
  if (n < 16) dvp_byte(q, (unsigned char)(0x90 | n));
  else { dvp_byte(q, 0xdc); dvp_be_out(q, n, 2); }
}

void dv_lstr (dv_req *q, const char *s, size_t n) {
  /* fixstr tops out at 31; the byte after it is nil, so getting this wrong
     does not truncate, it changes type. */
  if (n < 32) dvp_byte(q, (unsigned char)(0xa0 | n));
  else if (n < 256) { dvp_byte(q, 0xd9); dvp_byte(q, (unsigned char)n); }
  else if (n < 65536) { dvp_byte(q, 0xda); dvp_be_out(q, n, 2); }
  else { dvp_byte(q, 0xdb); dvp_be_out(q, n, 4); }
  dvp_raw(q, s, n);
}

void dv_str (dv_req *q, const char *s) { dv_lstr(q, s, strlen(s)); }
void dv_key (dv_req *q, const char *k) { dv_str(q, k); }

/* msgpack has a real binary type, so raw bytes cross the wire as bytes.
   The base64-in-string convention in a manifest describes how a JSON Schema
   *documents* this field; it is not what travels. */
void dv_bin (dv_req *q, const void *p, size_t n) {
  if (n < 256) { dvp_byte(q, 0xc4); dvp_byte(q, (unsigned char)n); }
  else if (n < 65536) { dvp_byte(q, 0xc5); dvp_be_out(q, n, 2); }
  else { dvp_byte(q, 0xc6); dvp_be_out(q, n, 4); }
  dvp_raw(q, p, n);
}

void dv_int (dv_req *q, int64_t v) {
  if (v >= 0 && v <= 0x7f) { dvp_byte(q, (unsigned char)v); return; }
  if (v < 0 && v >= -32) { dvp_byte(q, (unsigned char)(0xe0 | (v + 32))); return; }
  if (v >= 0) { dvp_byte(q, 0xcf); dvp_be_out(q, (uint64_t)v, 8); }
  else { dvp_byte(q, 0xd3); dvp_be_out(q, (uint64_t)v, 8); }
}

void dv_bool (dv_req *q, int b) { dvp_byte(q, b ? 0xc3 : 0xc2); }
void dv_nil (dv_req *q) { dvp_byte(q, 0xc0); }

static void dvp_header (dv_req *q, const char *last_key) {
  q->olen = 0;
  dv_map(q, 4);
  dv_key(q, "version"); dv_int(q, DVPLUG_VERSION);
  dv_key(q, "id");      dv_int(q, (int64_t)q->id);
  dv_key(q, "final");   dv_bool(q, 1);
  dv_key(q, last_key);
}

void dv_ok_begin (dv_req *q, unsigned nfields) {
  dvp_header(q, "value");
  dv_map(q, nfields);
}

void dv_ok_scalar (dv_req *q) { dvp_header(q, "value"); }

static int dvp_write_all (const unsigned char *p, size_t n) {
  while (n > 0) {
    ssize_t w = write(DVPLUG_CHANNEL_FD, p, n);
    if (w > 0) { p += (size_t)w; n -= (size_t)w; continue; }
    return -1;
  }
  return 0;
}

int dv_send (dv_req *q) {
  unsigned char hdr[4];
  if (q->answered) return 0;
  q->answered = 1;
  hdr[0] = (unsigned char)(q->olen >> 24);
  hdr[1] = (unsigned char)(q->olen >> 16);
  hdr[2] = (unsigned char)(q->olen >> 8);
  hdr[3] = (unsigned char)(q->olen);
  if (dvp_write_all(hdr, 4) != 0) return -1;
  return dvp_write_all(q->out, q->olen);
}

int dv_error (dv_req *q, const char *cls, const char *code, const char *msg) {
  dvp_header(q, "error");
  dv_map(q, 3);
  dv_key(q, "class");   dv_str(q, (cls != NULL) ? cls : DV_ERR_PLUGIN);
  dv_key(q, "code");    dv_str(q, (code != NULL) ? code : "");
  dv_key(q, "message"); dv_str(q, (msg != NULL) ? msg : "");
  return dv_send(q);
}

/* ------------------------------------------------------------------ serve */

static int dvp_read_exact (unsigned char *p, size_t n) {
  while (n > 0) {
    ssize_t r = read(DVPLUG_CHANNEL_FD, p, n);
    if (r > 0) { p += (size_t)r; n -= (size_t)r; continue; }
    if (r == 0) return 1;                /* the host closed: time to go */
    return -1;
  }
  return 0;
}

int dv_serve (dv_handler fn) {
  static unsigned char body[DVPLUG_FRAME_MAX];
  dv_req q;
  memset(&q, 0, sizeof(q));
  for (;;) {
    unsigned char hdr[4];
    uint32_t flen;
    const unsigned char *idp;
    int rc = dvp_read_exact(hdr, 4);
    if (rc != 0) { free(q.out); return (rc == 1) ? 0 : 1; }
    flen = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
           ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
    if (flen == 0 || flen > DVPLUG_FRAME_MAX) { free(q.out); return 1; }
    if (dvp_read_exact(body, flen) != 0) { free(q.out); return 1; }

    q.end = body + flen;
    q.id = 0;
    q.target[0] = '\0';
    q.args = NULL;
    q.olen = 0;
    q.answered = 0;

    idp = dvp_field(body, q.end, "id");
    if (idp == NULL) continue;           /* uncorrelatable: say nothing */
    {
      int64_t v = 0;
      dv_req tmp = q;
      tmp.args = body;                   /* reuse dv_arg_int's decoder */
      if (!dv_arg_int(&tmp, "id", &v)) continue;
      q.id = (uint64_t)v;
    }
    {
      uint64_t n = 0;
      const unsigned char *s = dvp_strval(dvp_field(body, q.end, "target"),
                                          q.end, &n);
      if (s != NULL && n + 1 <= sizeof(q.target) && s + n <= q.end) {
        memcpy(q.target, s, (size_t)n);
        q.target[n] = '\0';
      }
    }
    q.args = dvp_field(body, q.end, "args");
    if (q.args != NULL && q.args < q.end && *q.args == 0xc0)
      q.args = NULL;                     /* an explicit nil is no args */

    fn(&q);
    if (!q.answered)
      dv_error(&q, DV_ERR_PLUGIN, "no_answer",
               "the plugin returned without answering this call");
  }
}

#endif /* DVPLUG_IMPL_ONCE */
#endif /* DVPLUG_IMPL */
