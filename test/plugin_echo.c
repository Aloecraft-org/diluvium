/*
** plugin_echo.c
** A plugin, written the way a plugin is meant to be written.
**
** It is a fixture for host_check's plugin-channel cases, but the point it
** makes is not a test-only one: this file includes NOTHING from Diluvium.
** No dhost.h, no dmsgpack.h, no lua.h, no linking against the runtime. It
** reads doc/BUILD8.md §2.4's frame off fd 3 and writes one back, with about
** sixty lines of hand-rolled msgpack, because that is all the protocol asks
** of the other side. If this file ever needs a Diluvium header, the plugin
** channel has stopped being a protocol and become a C API, and that is a
** regression worth failing over.
**
** It answers three targets:
**   echo   -- the value is the request's own args, spliced back verbatim
**   delay  -- the same, after sleeping; deliberately a naive
**             read-work-reply loop, which is exactly the kind of plugin
**             max_inflight exists to keep from being buried
**   boom   -- an error frame, so the three error classes can be seen to
**             arrive intact on the guest side
**
** stdout and stderr are left alone on purpose: they are the plugin's to log
** on, which is the whole reason the channel is not stdin/stdout.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHANNEL_FD 3
#define FRAME_MAX  (8u * 1024u * 1024u)


/* ---------------------------------------------------------------- msgpack

** Only what the frame needs: skip any value, read a uint, read a string,
** and find a key in a map. Everything the host's emitters can produce is
** covered; anything else returns NULL and the frame is treated as garbage.
*/

static const unsigned char *mp_skip (const unsigned char *p,
                                     const unsigned char *end);

static const unsigned char *mp_skip_n (const unsigned char *p,
                                       const unsigned char *end,
                                       uint64_t n) {
  while (n-- > 0) {
    p = mp_skip(p, end);
    if (p == NULL)
      return NULL;
  }
  return p;
}

static uint64_t be (const unsigned char *p, int n) {
  uint64_t v = 0;
  int i;
  for (i = 0; i < n; i++)
    v = (v << 8) | p[i];
  return v;
}

static const unsigned char *mp_skip (const unsigned char *p,
                                     const unsigned char *end) {
  unsigned char c;
  if (p >= end)
    return NULL;
  c = *p++;
  if (c <= 0x7f || c >= 0xe0) return p;             /* fixint, both signs */
  if ((c & 0xe0) == 0xa0) {                          /* fixstr */
    uint64_t n = c & 0x1f;
    return (p + n <= end) ? p + n : NULL;
  }
  if ((c & 0xf0) == 0x80) return mp_skip_n(p, end, (uint64_t)(c & 0x0f) * 2);
  if ((c & 0xf0) == 0x90) return mp_skip_n(p, end, c & 0x0f);
  switch (c) {
    case 0xc0: case 0xc2: case 0xc3: return p;
    case 0xcc: case 0xd0: return (p + 1 <= end) ? p + 1 : NULL;
    case 0xcd: case 0xd1: return (p + 2 <= end) ? p + 2 : NULL;
    case 0xce: case 0xd2: case 0xca: return (p + 4 <= end) ? p + 4 : NULL;
    case 0xcf: case 0xd3: case 0xcb: return (p + 8 <= end) ? p + 8 : NULL;
    case 0xc4: case 0xd9: {
      uint64_t n;
      if (p + 1 > end) return NULL;
      n = be(p, 1); p += 1;
      return (p + n <= end) ? p + n : NULL;
    }
    case 0xc5: case 0xda: {
      uint64_t n;
      if (p + 2 > end) return NULL;
      n = be(p, 2); p += 2;
      return (p + n <= end) ? p + n : NULL;
    }
    case 0xc6: case 0xdb: {
      uint64_t n;
      if (p + 4 > end) return NULL;
      n = be(p, 4); p += 4;
      return (p + n <= end) ? p + n : NULL;
    }
    case 0xdc: {
      uint64_t n;
      if (p + 2 > end) return NULL;
      n = be(p, 2); p += 2;
      return mp_skip_n(p, end, n);
    }
    case 0xdd: {
      uint64_t n;
      if (p + 4 > end) return NULL;
      n = be(p, 4); p += 4;
      return mp_skip_n(p, end, n);
    }
    case 0xde: {
      uint64_t n;
      if (p + 2 > end) return NULL;
      n = be(p, 2); p += 2;
      return mp_skip_n(p, end, n * 2);
    }
    case 0xdf: {
      uint64_t n;
      if (p + 4 > end) return NULL;
      n = be(p, 4); p += 4;
      return mp_skip_n(p, end, n * 2);
    }
    default: return NULL;
  }
}

/* Point 'vp' at the value stored under 'key' in the map at 'p'. */
static const unsigned char *mp_field (const unsigned char *p,
                                      const unsigned char *end,
                                      const char *key) {
  uint64_t n, i;
  unsigned char c;
  if (p >= end)
    return NULL;
  c = *p++;
  if ((c & 0xf0) == 0x80) n = c & 0x0f;
  else if (c == 0xde) { if (p + 2 > end) return NULL; n = be(p, 2); p += 2; }
  else if (c == 0xdf) { if (p + 4 > end) return NULL; n = be(p, 4); p += 4; }
  else return NULL;
  for (i = 0; i < n; i++) {
    const unsigned char *kp = p;
    uint64_t klen = 0;
    const unsigned char *kstart = NULL;
    if (kp >= end) return NULL;
    if ((*kp & 0xe0) == 0xa0) { klen = *kp & 0x1f; kstart = kp + 1; }
    else if (*kp == 0xd9) {
      if (kp + 2 > end) return NULL;
      klen = kp[1]; kstart = kp + 2;
    }
    p = mp_skip(p, end);                 /* past the key */
    if (p == NULL) return NULL;
    if (kstart != NULL && kstart + klen <= end &&
        klen == strlen(key) && memcmp(kstart, key, klen) == 0)
      return p;                          /* the value */
    p = mp_skip(p, end);                 /* past the value */
    if (p == NULL) return NULL;
  }
  return NULL;
}

static int mp_uint (const unsigned char *p, const unsigned char *end,
                    uint64_t *out) {
  if (p == NULL || p >= end) return 0;
  if (*p <= 0x7f) { *out = *p; return 1; }
  switch (*p) {
    case 0xcc: if (p + 2 > end) return 0; *out = be(p + 1, 1); return 1;
    case 0xcd: if (p + 3 > end) return 0; *out = be(p + 1, 2); return 1;
    case 0xce: if (p + 5 > end) return 0; *out = be(p + 1, 4); return 1;
    case 0xcf: if (p + 9 > end) return 0; *out = be(p + 1, 8); return 1;
    default: return 0;
  }
}

static int mp_str (const unsigned char *p, const unsigned char *end,
                   char *out, size_t cap) {
  uint64_t n;
  const unsigned char *s;
  if (p == NULL || p >= end) return 0;
  if ((*p & 0xe0) == 0xa0) { n = *p & 0x1f; s = p + 1; }
  else if (*p == 0xd9) { if (p + 2 > end) return 0; n = p[1]; s = p + 2; }
  else return 0;
  if (s + n > end || n + 1 > cap) return 0;
  memcpy(out, s, (size_t)n);
  out[n] = '\0';
  return 1;
}


/* ------------------------------------------------------------------ write */

static unsigned char obuf[FRAME_MAX];
static size_t olen;

static void o_raw (const void *p, size_t n) {
  if (olen + n <= sizeof(obuf)) { memcpy(obuf + olen, p, n); olen += n; }
}
static void o_byte (unsigned char c) { o_raw(&c, 1); }
static void o_map (unsigned n) { o_byte((unsigned char)(0x80 | n)); }
/* fixstr tops out at 31 bytes, and the byte after it (0xc0) is nil -- so a
** 32-byte string emitted as fixstr does not merely truncate, it changes
** type and the reader sees a null where a message should be. Which is
** exactly what happened the first time this file was written. */
static void o_str (const char *s) {
  size_t n = strlen(s);
  if (n < 32)
    o_byte((unsigned char)(0xa0 | n));
  else if (n < 256) {
    o_byte(0xd9);
    o_byte((unsigned char)n);
  }
  else {
    o_byte(0xda);
    o_byte((unsigned char)(n >> 8));
    o_byte((unsigned char)n);
  }
  o_raw(s, n);
}
static void o_uint (uint64_t v) {
  if (v <= 0x7f) { o_byte((unsigned char)v); return; }
  o_byte(0xcf);
  { int i; for (i = 7; i >= 0; i--) o_byte((unsigned char)(v >> (i * 8))); }
}
static void o_bool (int b) { o_byte(b ? 0xc3 : 0xc2); }

static int write_all (const unsigned char *p, size_t n) {
  while (n > 0) {
    ssize_t w = write(CHANNEL_FD, p, n);
    if (w > 0) { p += w; n -= (size_t)w; continue; }
    if (w < 0) return -1;
  }
  return 0;
}

/* Length-prefix whatever is in obuf and send it. */
static int send_frame (void) {
  unsigned char hdr[4];
  hdr[0] = (unsigned char)(olen >> 24);
  hdr[1] = (unsigned char)(olen >> 16);
  hdr[2] = (unsigned char)(olen >> 8);
  hdr[3] = (unsigned char)(olen);
  if (write_all(hdr, 4) != 0) return -1;
  return write_all(obuf, olen);
}


/* ------------------------------------------------------------------- read */

static int read_exact (unsigned char *p, size_t n) {
  while (n > 0) {
    ssize_t r = read(CHANNEL_FD, p, n);
    if (r > 0) { p += r; n -= (size_t)r; continue; }
    if (r == 0) return 1;                /* the host closed: time to go */
    return -1;
  }
  return 0;
}

int main (void) {
  static unsigned char body[FRAME_MAX];
  for (;;) {
    unsigned char hdr[4];
    uint32_t flen;
    uint64_t id = 0;
    char target[64];
    const unsigned char *end, *args;
    int rc = read_exact(hdr, 4);
    if (rc != 0)
      return (rc == 1) ? 0 : 1;
    flen = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
           ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
    if (flen == 0 || flen > FRAME_MAX)
      return 1;
    if (read_exact(body, flen) != 0)
      return 1;
    end = body + flen;
    target[0] = '\0';
    if (!mp_uint(mp_field(body, end, "id"), end, &id))
      continue;                          /* uncorrelatable: say nothing */
    mp_str(mp_field(body, end, "target"), end, target, sizeof(target));
    args = mp_field(body, end, "args");

    /* Exit without answering. A plugin that dies mid-call is the failure
       'transport' exists to name, and a fixture that can do it on demand
       beats one that has to be killed from outside. */
    if (strcmp(target, "die") == 0)
      return 0;

    if (strcmp(target, "delay") == 0) {
      uint64_t ms = 0;
      if (args != NULL && mp_uint(mp_field(args, end, "ms"), end, &ms)) {
        struct timespec ts;
        ts.tv_sec = (time_t)(ms / 1000);
        ts.tv_nsec = (long)(ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
      }
    }

    olen = 0;
    if (strcmp(target, "boom") == 0) {
      o_map(4);
      o_str("version"); o_uint(1);
      o_str("id");      o_uint(id);
      o_str("final");   o_bool(1);
      o_str("error");
      o_map(3);
      o_str("class");   o_str("capability");
      o_str("code");    o_str("teapot");
      o_str("message"); o_str("the upstream service is a teapot");
    }
    else {
      const unsigned char *aend = (args != NULL) ? mp_skip(args, end) : NULL;
      o_map(4);
      o_str("version"); o_uint(1);
      o_str("id");      o_uint(id);
      o_str("final");   o_bool(1);
      o_str("value");
      if (args != NULL && aend != NULL)
        o_raw(args, (size_t)(aend - args));   /* spliced back verbatim */
      else
        o_byte(0xc0);                          /* nil */
    }
    if (send_frame() != 0)
      return 1;
  }
}
