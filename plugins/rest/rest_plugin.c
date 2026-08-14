/*
** rest_plugin.c
** Outbound HTTP, as a Diluvium plugin.
**
** This is the capability doc/BUILD7.md section 5 deferred as `net` and build 8
** closes without the host ever learning to speak HTTP. That is the whole
** thesis: the host links no TLS, resolves no names, and opens no outbound
** socket; this program does all three, and it is replaced without replacing
** Diluvium.
**
** It answers two calls:
**
**   rest/get  {url, headers?, timeout_ms?}        -> {status, headers, body}
**   rest/post {url, headers?, body, timeout_ms?}  -> {status, headers, body}
**
** `body` is msgpack bin in both directions -- the wire has a real binary type,
** so nothing is base64 in flight. The manifest's JSON Schema documents the
** field as base64 because JSON Schema has no binary type; that is a statement
** about the schema, not about the bytes.
**
** Errors carry the capability class with a declared code: `dns`, `tls`,
** `timeout`, `status` or `too_large`. A non-2xx response is NOT an error --
** it is an answer, with its status in it, exactly as `crypto/jwt_verify`
** returns {valid=false} rather than raising. `status` is reserved for a
** response this program could not read at all.
**
** Build:
**   cc -O2 -o diluvium-rest-plugin rest_plugin.c -lssl -lcrypto
**   cc -O2 -DREST_NO_TLS -o diluvium-rest-plugin rest_plugin.c   (http only)
**
** It is deliberately one call at a time. A plugin that wants concurrency
** forks or threads; the host's max_inflight is what keeps a serial plugin
** like this one from being handed more than it can hold, and the calling
** instances are parked either way.
*/

#define DVPLUG_IMPL
#include "../dvplug.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef REST_NO_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#define REST_MAX_BODY     (16u * 1024u * 1024u)
#define REST_MAX_HEADERS  64
#define REST_DEFAULT_MS   15000


/* ======================================================================
** A URL, split.
** ====================================================================== */

typedef struct url {
  int tls;
  char host[256];
  char port[8];
  char path[2048];
} url;

static int url_parse (const char *s, url *u, char *why, size_t whycap) {
  const char *p = s, *hostend, *slash;
  size_t n;
  memset(u, 0, sizeof(*u));
  if (strncmp(p, "http://", 7) == 0) { u->tls = 0; p += 7; strcpy(u->port, "80"); }
  else if (strncmp(p, "https://", 8) == 0) { u->tls = 1; p += 8; strcpy(u->port, "443"); }
  else {
    snprintf(why, whycap, "the url must begin http:// or https://");
    return -1;
  }
  if (strchr(p, '@') != NULL) {
    /* Credentials in a URL would end up in the host's logs and in the
       guest's message log. Refuse rather than launder them. */
    snprintf(why, whycap, "credentials in a url are refused; send an "
                          "Authorization header instead");
    return -1;
  }
  slash = strchr(p, '/');
  hostend = (slash != NULL) ? slash : p + strlen(p);
  n = (size_t)(hostend - p);
  if (n == 0 || n >= sizeof(u->host)) {
    snprintf(why, whycap, "the url has no usable host");
    return -1;
  }
  memcpy(u->host, p, n);
  u->host[n] = '\0';
  {
    char *colon;
    if (u->host[0] == '[') {                    /* [::1]:8080 */
      char *close = strchr(u->host, ']');
      if (close == NULL) {
        snprintf(why, whycap, "the url's bracketed host is not closed");
        return -1;
      }
      colon = strchr(close, ':');
      if (colon != NULL) { *colon = '\0'; snprintf(u->port, sizeof(u->port), "%s", colon + 1); }
      else *close = '\0';
      memmove(u->host, u->host + 1, strlen(u->host));
    }
    else if ((colon = strrchr(u->host, ':')) != NULL) {
      *colon = '\0';
      snprintf(u->port, sizeof(u->port), "%s", colon + 1);
    }
  }
  if (u->host[0] == '\0') {
    snprintf(why, whycap, "the url has no usable host");
    return -1;
  }
  if (slash != NULL) {
    if (strlen(slash) >= sizeof(u->path)) {
      snprintf(why, whycap, "the url's path is longer than this plugin holds");
      return -1;
    }
    strcpy(u->path, slash);
  }
  else
    strcpy(u->path, "/");
  return 0;
}


/* ======================================================================
** A connection, plain or TLS, with one deadline over the whole exchange.
** ====================================================================== */

typedef struct conn {
  int fd;
#ifndef REST_NO_TLS
  SSL_CTX *ctx;
  SSL *ssl;
#endif
} conn;

static int64_t now_ms (void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void conn_close (conn *c) {
#ifndef REST_NO_TLS
  if (c->ssl != NULL) { SSL_free(c->ssl); c->ssl = NULL; }
  if (c->ctx != NULL) { SSL_CTX_free(c->ctx); c->ctx = NULL; }
#endif
  if (c->fd >= 0) { close(c->fd); c->fd = -1; }
}

/* Returns 0, or -1 with a class/code/message for the caller to relay. */
static int conn_open (conn *c, const url *u, int64_t deadline,
                      const char **code, char *why, size_t whycap) {
  struct addrinfo hints, *res = NULL, *ai;
  int rc;
  memset(c, 0, sizeof(*c));
  c->fd = -1;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  rc = getaddrinfo(u->host, u->port, &hints, &res);
  if (rc != 0 || res == NULL) {
    *code = "dns";
    snprintf(why, whycap, "could not resolve '%s': %s", u->host,
             gai_strerror(rc));
    return -1;
  }
  for (ai = res; ai != NULL; ai = ai->ai_next) {
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    /* Blocking connect with the deadline enforced by poll: set non-blocking
       for the connect, then block for the exchange, which keeps the read and
       write loops simple while still bounding the handshake. */
    {
      struct timeval tv;
      int64_t left = deadline - now_ms();
      if (left <= 0) { close(fd); break; }
      tv.tv_sec = (time_t)(left / 1000);
      tv.tv_usec = (suseconds_t)(left % 1000) * 1000;
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
      int one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      c->fd = fd;
      break;
    }
    close(fd);
  }
  freeaddrinfo(res);
  if (c->fd < 0) {
    *code = "timeout";
    snprintf(why, whycap, "could not connect to %s:%s: %s", u->host, u->port,
             strerror(errno));
    return -1;
  }
  if (!u->tls)
    return 0;
#ifdef REST_NO_TLS
  *code = "tls";
  snprintf(why, whycap, "this build of the rest plugin has no TLS; "
                        "rebuild without -DREST_NO_TLS");
  conn_close(c);
  return -1;
#else
  c->ctx = SSL_CTX_new(TLS_client_method());
  if (c->ctx == NULL) {
    *code = "tls";
    snprintf(why, whycap, "no TLS context");
    conn_close(c);
    return -1;
  }
  /* Verify, and verify the name. A client that skips either is not doing
     TLS, it is doing obfuscation. */
  SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, NULL);
  SSL_CTX_set_min_proto_version(c->ctx, TLS1_2_VERSION);
  if (SSL_CTX_set_default_verify_paths(c->ctx) != 1) {
    *code = "tls";
    snprintf(why, whycap, "no trust store: this host has no CA bundle where "
                          "OpenSSL looks for one");
    conn_close(c);
    return -1;
  }
  c->ssl = SSL_new(c->ctx);
  if (c->ssl == NULL) {
    *code = "tls";
    snprintf(why, whycap, "no TLS session");
    conn_close(c);
    return -1;
  }
  SSL_set_fd(c->ssl, c->fd);
  SSL_set_tlsext_host_name(c->ssl, u->host);          /* SNI */
  if (SSL_set1_host(c->ssl, u->host) != 1) {          /* and the check */
    *code = "tls";
    snprintf(why, whycap, "could not pin the certificate name to '%s'",
             u->host);
    conn_close(c);
    return -1;
  }
  if (SSL_connect(c->ssl) != 1) {
    unsigned long e = ERR_get_error();
    long v = SSL_get_verify_result(c->ssl);
    *code = "tls";
    if (v != X509_V_OK)
      snprintf(why, whycap, "the certificate for '%s' did not verify: %s",
               u->host, X509_verify_cert_error_string(v));
    else
      snprintf(why, whycap, "the TLS handshake with '%s' failed: %s", u->host,
               ERR_reason_error_string(e) ? ERR_reason_error_string(e)
                                          : "no reason given");
    conn_close(c);
    return -1;
  }
  return 0;
#endif
}

static int conn_write (conn *c, const void *p, size_t n) {
#ifndef REST_NO_TLS
  if (c->ssl != NULL) {
    const unsigned char *q = (const unsigned char *)p;
    while (n > 0) {
      int w = SSL_write(c->ssl, q, (int)(n > INT_MAX ? INT_MAX : n));
      if (w <= 0) return -1;
      q += w; n -= (size_t)w;
    }
    return 0;
  }
#endif
  {
    const unsigned char *q = (const unsigned char *)p;
    while (n > 0) {
      ssize_t w = write(c->fd, q, n);
      if (w <= 0) return -1;
      q += w; n -= (size_t)w;
    }
  }
  return 0;
}

static int conn_read (conn *c, void *p, size_t n) {
#ifndef REST_NO_TLS
  if (c->ssl != NULL)
    return SSL_read(c->ssl, p, (int)(n > INT_MAX ? INT_MAX : n));
#endif
  return (int)read(c->fd, p, n);
}


/* ======================================================================
** The exchange.
** ====================================================================== */

typedef struct resp {
  int status;
  char *buf;
  size_t len, cap;
  size_t hdr_end;                       /* first byte of the body */
} resp;

static int resp_grow (resp *r, size_t need) {
  if (r->len + need <= r->cap) return 0;
  {
    size_t want = (r->cap == 0) ? 16384 : r->cap;
    char *b;
    while (want < r->len + need) want *= 2;
    if (want > REST_MAX_BODY + 65536) return -1;
    b = (char *)realloc(r->buf, want);
    if (b == NULL) return -1;
    r->buf = b;
    r->cap = want;
  }
  return 0;
}

/* Case-insensitive header lookup within the response head. */
static const char *hdr_find (const char *head, size_t headlen, const char *name,
                             size_t *vlen) {
  size_t nlen = strlen(name);
  const char *p = memchr(head, '\n', headlen);
  if (p == NULL) return NULL;
  p++;
  while (p < head + headlen) {
    const char *eol = memchr(p, '\n', (size_t)(head + headlen - p));
    size_t linelen = (eol != NULL) ? (size_t)(eol - p) : (size_t)(head + headlen - p);
    if (linelen > nlen && p[nlen] == ':' && strncasecmp(p, name, nlen) == 0) {
      const char *v = p + nlen + 1;
      const char *e = p + linelen;
      while (v < e && (*v == ' ' || *v == '\t')) v++;
      while (e > v && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) e--;
      *vlen = (size_t)(e - v);
      return v;
    }
    if (eol == NULL) break;
    p = eol + 1;
  }
  return NULL;
}

static void handle (dv_req *q) {
  char urlbuf[2048], why[512];
  const char *code = NULL;
  url u;
  conn c;
  int64_t timeout = REST_DEFAULT_MS, deadline;
  int is_post = (strcmp(q->target, "post") == 0);
  resp r;
  const unsigned char *body = NULL;
  size_t bodylen = 0;

  memset(&r, 0, sizeof(r));

  if (strcmp(q->target, "get") != 0 && !is_post) {
    dv_error(q, DV_ERR_PLUGIN, "bad_target",
             "this plugin answers 'get' and 'post'");
    return;
  }
  if (!dv_arg_str(q, "url", urlbuf, sizeof(urlbuf))) {
    dv_error(q, DV_ERR_PLUGIN, "bad_args", "the call needs a 'url' string");
    return;
  }
  if (url_parse(urlbuf, &u, why, sizeof(why)) != 0) {
    dv_error(q, DV_ERR_PLUGIN, "bad_args", why);
    return;
  }
  dv_arg_int(q, "timeout_ms", &timeout);
  if (timeout <= 0 || timeout > 120000) timeout = REST_DEFAULT_MS;
  deadline = now_ms() + timeout;

  if (is_post && !dv_arg_bin(q, "body", &body, &bodylen)) {
    body = (const unsigned char *)"";
    bodylen = 0;
  }

  /* Build the request head BEFORE connecting. A request this plugin will
     refuse should not cost a socket, a DNS lookup or a TLS handshake -- and
     more importantly, a caller must learn that its header was rejected
     rather than that some endpoint was unreachable. */
  {
    char head[8192];
    int n = snprintf(head, sizeof(head),
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: diluvium-rest-plugin/1\r\n"
                     "Accept: */*\r\n"
                     "Connection: close\r\n",
                     is_post ? "POST" : "GET", u.path, u.host);
    uint32_t nh = 0;
    const unsigned char *hp = dv_arg_map(q, "headers", &nh);
    uint32_t i;
    for (i = 0; i < nh && hp != NULL; i++) {
      char key[128], val[1024];
      uint32_t unused;
      const unsigned char *vp;
      (void)unused;
      vp = dv_map_next(q, hp, key, sizeof(key));
      if (vp == NULL) break;
      val[0] = '\0';
      {
        /* Read the value as a string by borrowing dv_arg_str's decoder on a
           one-field view is awkward; do it directly. */
        const unsigned char *s = vp;
        size_t vlen = 0;
        if (s < q->end && (*s & 0xe0) == 0xa0) { vlen = *s & 0x1f; s++; }
        else if (s + 1 < q->end && *s == 0xd9) { vlen = s[1]; s += 2; }
        else if (s + 2 < q->end && *s == 0xda) { vlen = ((size_t)s[1] << 8) | s[2]; s += 3; }
        if (vlen > 0 && vlen < sizeof(val) && s + vlen <= q->end) {
          memcpy(val, s, vlen);
          val[vlen] = '\0';
        }
      }
      hp = dv_skip(q, vp);
      /* Header injection is the one thing a caller must not be able to do
         through a value it controls; a CR or LF is refused, never stripped,
         because stripping silently changes what was sent. */
      if (key[0] == '\0' || val[0] == '\0') continue;
      if (strpbrk(key, "\r\n:") != NULL || strpbrk(val, "\r\n") != NULL) {
        free(r.buf);
        dv_error(q, DV_ERR_PLUGIN, "bad_args",
                 "a header name or value contained CR, LF or a colon; that is "
                 "refused rather than stripped");
        return;
      }
      if (strcasecmp(key, "host") == 0 || strcasecmp(key, "content-length") == 0
          || strcasecmp(key, "connection") == 0)
        continue;                        /* this plugin owns these */
      n += snprintf(head + n, sizeof(head) - (size_t)n, "%s: %s\r\n", key, val);
      if (n >= (int)sizeof(head) - 64) break;
    }
    if (is_post)
      n += snprintf(head + n, sizeof(head) - (size_t)n,
                    "Content-Length: %zu\r\n", bodylen);
    n += snprintf(head + n, sizeof(head) - (size_t)n, "\r\n");

    /* Only now is anything opened. */
    if (conn_open(&c, &u, deadline, &code, why, sizeof(why)) != 0) {
      free(r.buf);
      dv_error(q, DV_ERR_CAPABILITY, code, why);
      return;
    }
    if (conn_write(&c, head, (size_t)n) != 0 ||
        (bodylen > 0 && conn_write(&c, body, bodylen) != 0)) {
      conn_close(&c);
      free(r.buf);
      dv_error(q, DV_ERR_CAPABILITY, "timeout", "the request could not be sent");
      return;
    }
  }

  /* ---- response ---- */
  for (;;) {
    int n;
    if (now_ms() > deadline) {
      conn_close(&c);
      free(r.buf);
      dv_error(q, DV_ERR_CAPABILITY, "timeout",
               "the server did not finish answering within the deadline");
      return;
    }
    if (resp_grow(&r, 16384) != 0) {
      conn_close(&c);
      free(r.buf);
      dv_error(q, DV_ERR_CAPABILITY, "too_large",
               "the response is larger than this plugin will hold");
      return;
    }
    n = conn_read(&c, r.buf + r.len, r.cap - r.len);
    if (n > 0) { r.len += (size_t)n; continue; }
    break;                               /* Connection: close, so EOF ends it */
  }
  conn_close(&c);

  /* Head/body split and the status line. */
  {
    const char *sep = NULL;
    size_t i;
    for (i = 3; i < r.len; i++) {
      if (r.buf[i] == '\n' && r.buf[i-1] == '\r' && r.buf[i-2] == '\n' &&
          r.buf[i-3] == '\r') { sep = r.buf + i + 1; break; }
    }
    if (sep == NULL || r.len < 12 || strncmp(r.buf, "HTTP/1.", 7) != 0) {
      free(r.buf);
      dv_error(q, DV_ERR_CAPABILITY, "status",
               "the response was not readable as HTTP/1.x");
      return;
    }
    r.hdr_end = (size_t)(sep - r.buf);
    r.status = atoi(r.buf + 9);
  }

  /* ---- answer ---- */
  {
    size_t ctlen = 0;
    const char *ct = hdr_find(r.buf, r.hdr_end, "content-type", &ctlen);
    size_t blen = r.len - r.hdr_end;
    if (blen > REST_MAX_BODY) {
      free(r.buf);
      dv_error(q, DV_ERR_CAPABILITY, "too_large",
               "the response body is larger than this plugin will hand back");
      return;
    }
    /* A non-2xx is an ANSWER, not an error: the guest asked what the service
       said, and 404 is what it said. 'status' as an error class is reserved
       for a response this program could not read at all. */
    dv_ok_begin(q, 3);
    dv_key(q, "status");       dv_int(q, r.status);
    dv_key(q, "content_type");
    if (ct != NULL) dv_lstr(q, ct, ctlen); else dv_nil(q);
    dv_key(q, "body");         dv_bin(q, r.buf + r.hdr_end, blen);
    dv_send(q);
  }
  free(r.buf);
}

int main (void) {
#ifndef REST_NO_TLS
  SSL_library_init();
  SSL_load_error_strings();
#endif
  return dv_serve(handle);
}
