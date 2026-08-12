/*
** dhost_http.c
** The listener connector: doc/Host.md duty 4's inbound half, for a
** deployment behind a load balancer that already terminated TLS.
**
** The port is topology, so it comes from configuration, not from a guest --
** a guest cannot read a socket anyway, and a listener that hibernated with
** its program would be host state pretending otherwise. What a guest sees is
** the queue: a completed request becomes one message,
**
**   {conn = <token>, method = "GET", path = "/x?y=1", body = "..."}
**
** pushed to the configured queue on the root instance, and a response is one
** message popped from the configured reply queue,
**
**   {conn = <token>, status = 200, body = "...", content_type = "..."} --
**
** 'conn' echoed verbatim, the same correlation discipline Hostcall.md fixes
** for hostcalls, applied to traffic. The host never interprets what happened
** between the two.
**
** One request per connection ('Connection: close'), no chunked bodies, HTTP/
** 1.1 only: v1 is behind an LB that speaks all of that to the world. The
** per-connection deadline is the host's own timeout logic -- the thing a
** queue push not blocking makes necessary, and the thing only the host can
** implement, since only the host has a clock.
*/

#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "dmsgpack.h"
#include "dhost.h"
#include "picohttpparser.h"

#define HTTP_MAX_HEADERS 32
#define HTTP_HDR_ROOM    8192

typedef enum { CONN_FREE = 0, CONN_READING, CONN_WAITING, CONN_WRITING }
conn_state;

typedef struct dh_conn {
  conn_state state;
  int fd;
  uint32_t token;
  int64_t deadline_ms;
  dh_buf in;
  dh_buf out;
  size_t wrote;
} dh_conn;

typedef struct dh_http {
  int listen_fd;
  dh_conn *conns;
  int max_conns;
  long max_body;
  long deadline_ms;
  uint32_t next_token;
  char queue[DH_NAME_MAX];
  char reply_queue[DH_NAME_MAX];
} dh_http;


static void conn_close (dh_conn *c) {
  if (c->fd >= 0)
    close(c->fd);
  dh_buf_free(&c->in);
  dh_buf_free(&c->out);
  memset(c, 0, sizeof(*c));
  c->fd = -1;
}

static void respond_raw (dh_conn *c, int status, const char *reason,
                         const char *ctype, const char *body, size_t blen) {
  char head[256];
  int n = snprintf(head, sizeof(head),
                   "HTTP/1.1 %d %s\r\n"
                   "Content-Type: %s\r\n"
                   "Content-Length: %zu\r\n"
                   "Connection: close\r\n\r\n",
                   status, reason, ctype, blen);
  dh_buf_free(&c->out);
  /* Clamp defensively. Today the pieces sum to ~238 bytes -- the ctype is
     bounded to 127 by its copy and the reason is a short constant -- so this
     cannot fire; it is here so a later change to either cannot turn a
     truncated (n < 0 or n >= sizeof) header into an out-of-bounds dh_raw.
     A header that would not fit becomes a bare 500, which is honest. */
  if (n < 0 || (size_t)n >= sizeof(head)) {
    static const char fallback[] =
        "HTTP/1.1 500 Internal Server Error\r\n"
        "Content-Length: 0\r\nConnection: close\r\n\r\n";
    dh_raw(&c->out, fallback, sizeof(fallback) - 1);
    c->wrote = 0;
    c->state = CONN_WRITING;
    return;
  }
  dh_raw(&c->out, head, (size_t)n);
  if (blen > 0)
    dh_raw(&c->out, body, blen);
  c->wrote = 0;
  c->state = CONN_WRITING;
}

static void respond_text (dh_conn *c, int status, const char *reason,
                          const char *body) {
  respond_raw(c, status, reason, "text/plain", body, strlen(body));
}

/* The reason phrase for a status the guest chose. The short list a machine
   client actually branches on; anything else is its number. */
static const char *reason_for (int status) {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Status";
  }
}

static int set_nonblock (int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  return (fl < 0) ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* A complete request parsed out of c->in, turned into one message and
   pushed. Refusals are answered on the socket: a full queue is 503 --
   backpressure made visible -- and an oversized body is 413 before the
   bytes are read. */
static void deliver (dh_host *h, dh_http *x, dh_conn *c,
                     const char *method, size_t method_len,
                     const char *path, size_t path_len,
                     const char *body, size_t body_len) {
  dv_instance *root = dvs_instance(h->sw, h->root);
  dv_queue_id q;
  dh_buf msg;
  if (root == NULL) {
    respond_text(c, 503, "Service Unavailable",
                 "the root instance is not resident\n");
    return;
  }
  q = dv_queue_lookup(root, x->queue);
  if (q == 0) {
    respond_text(c, 503, "Service Unavailable",
                 "the program declares no request queue\n");
    return;
  }
  dh_buf_init(&msg);
  dh_map(&msg, 4);
  dh_str(&msg, "conn"); dh_uint(&msg, c->token);
  dh_str(&msg, "method"); dh_lstr(&msg, method, method_len);
  dh_str(&msg, "path"); dh_lstr(&msg, path, path_len);
  dh_str(&msg, "body"); dh_lstr(&msg, body, body_len);
  if (dv_queue_push(root, q, msg.p, msg.len) != DV_OK) {
    respond_text(c, 503, "Service Unavailable",
                 "the request queue is full; try again\n");
    dh_buf_free(&msg);
    return;
  }
  dh_buf_free(&msg);
  c->state = CONN_WAITING;             /* the reply pump will answer */
}

static void try_parse (dh_host *h, dh_http *x, dh_conn *c) {
  const char *method, *path;
  size_t method_len, path_len;
  int minor;
  struct phr_header headers[HTTP_MAX_HEADERS];
  size_t nheaders = HTTP_MAX_HEADERS;
  int hlen = phr_parse_request((const char *)c->in.p, c->in.len,
                               &method, &method_len, &path, &path_len,
                               &minor, headers, &nheaders, 0);
  if (hlen == -1) {
    respond_text(c, 400, "Bad Request", "unparseable request\n");
    return;
  }
  if (hlen == -2) {                    /* incomplete: keep reading */
    if (c->in.len > HTTP_HDR_ROOM)
      respond_text(c, 400, "Bad Request", "headers past this host's room\n");
    return;
  }
  {
    long clen = 0;
    int seen_clen = 0;
    size_t i;
    for (i = 0; i < nheaders; i++) {
      if (headers[i].name_len == 14 &&
          strncasecmp(headers[i].name, "Content-Length", 14) == 0) {
        /* Parse strictly, over the header's own length -- the value is not
           NUL-terminated, and strtol would read past it and accept '+5' or
           stop silently at '5x'. A parser that disagrees with the LB about
           where the body ends is the request-smuggling primitive, so a
           malformed length is a refusal, and a *second* Content-Length is a
           refusal too (RFC 7230: they must agree, and we do not gamble on
           which the LB believed). */
        long v = 0;
        size_t k;
        if (seen_clen || headers[i].value_len == 0) {
          respond_text(c, 400, "Bad Request",
                       "a malformed or duplicated Content-Length\n");
          return;
        }
        for (k = 0; k < headers[i].value_len; k++) {
          char d = headers[i].value[k];
          if (d < '0' || d > '9') {
            respond_text(c, 400, "Bad Request",
                         "Content-Length must be digits only\n");
            return;
          }
          if (v > (LONG_MAX - (d - '0')) / 10) {
            respond_text(c, 413, "Content Too Large", "Content-Length "
                                                      "overflows\n");
            return;
          }
          v = v * 10 + (d - '0');
        }
        clen = v;
        seen_clen = 1;
      }
      if (headers[i].name_len == 17 &&
          strncasecmp(headers[i].name, "Transfer-Encoding", 17) == 0) {
        respond_text(c, 400, "Bad Request",
                     "chunked bodies are not spoken here; the LB should "
                     "buffer\n");
        return;
      }
    }
    if (clen > x->max_body) {
      respond_text(c, 413, "Content Too Large", "body past the configured "
                                                "cap\n");
      return;
    }
    if (c->in.len < (size_t)hlen + (size_t)clen)
      return;                          /* body still arriving */
    deliver(h, x, c, method, method_len, path, path_len,
            (const char *)c->in.p + hlen, (size_t)clen);
  }
}

/* Drain the guest's reply queue and finish the connections it names. */
static void pump_replies (dh_host *h, dh_http *x) {
  dv_instance *root = dvs_instance(h->sw, h->root);
  dv_queue_id q;
  if (root == NULL)
    return;
  q = dv_queue_lookup(root, x->reply_queue);
  if (q == 0)
    return;
  for (;;) {
    const uint8_t *msg = NULL;
    size_t msglen = 0;
    diluvium_mp_cursor c;
    diluvium_mp_token t;
    uint32_t token = 0;
    int status = 200;
    const char *body = "";
    size_t body_len = 0;
    char ctype[128];
    int i, found = -1;
    if (dv_queue_peek(root, q, &msg, &msglen) != DV_OK)
      break;
    strcpy(ctype, "application/octet-stream");
    diluvium_mp_open(&c, msg, msglen);
    if (diluvium_mp_field(&c, "conn") && diluvium_mp_read(&c, &t) &&
        t.kind == DILUVIUM_MP_INT)
      token = (uint32_t)t.i;
    diluvium_mp_open(&c, msg, msglen);
    if (diluvium_mp_field(&c, "status") && diluvium_mp_read(&c, &t) &&
        t.kind == DILUVIUM_MP_INT && t.i >= 100 && t.i <= 599)
      status = (int)t.i;
    diluvium_mp_open(&c, msg, msglen);
    if (diluvium_mp_field(&c, "body") && diluvium_mp_read(&c, &t) &&
        t.kind == DILUVIUM_MP_STR) {
      body = t.p;
      body_len = t.len;
    }
    diluvium_mp_open(&c, msg, msglen);
    if (diluvium_mp_field(&c, "content_type") && diluvium_mp_read(&c, &t) &&
        t.kind == DILUVIUM_MP_STR && t.len < sizeof(ctype)) {
      /* The guest is untrusted, and this string is interpolated into the
         response header block. A control byte in it -- a CR or LF above all
         -- would inject headers or split the response to the client on the
         far side of the LB. A media type has no control characters, so any
         is disqualifying: keep the safe default rather than a sanitized
         guess at what the guest meant. */
      size_t j;
      int clean = 1;
      for (j = 0; j < t.len; j++) {
        if ((unsigned char)t.p[j] < 0x20 || (unsigned char)t.p[j] == 0x7f) {
          clean = 0;
          break;
        }
      }
      if (clean) {
        memcpy(ctype, t.p, t.len);
        ctype[t.len] = '\0';
      }
    }
    for (i = 0; i < x->max_conns; i++) {
      if (x->conns[i].state == CONN_WAITING && x->conns[i].token == token) {
        found = i;
        break;
      }
    }
    if (found >= 0)
      respond_raw(&x->conns[found], status, reason_for(status), ctype,
                  body, body_len);
    /* A reply naming no waiting connection is consumed all the same: the
       connection may have hit its deadline first, and a wedged reply queue
       would take every later response with it. */
    dv_queue_release(root, q);
  }
}

int dh_http_poll (dh_host *h, int timeout_ms) {
  dh_http *x = (dh_http *)h->listener;
  struct pollfd *fds;
  int nfds = 0, i, rc;
  int *slot_of;
  if (x == NULL)
    return 0;
  pump_replies(h, x);
  fds = (struct pollfd *)calloc((size_t)x->max_conns + 1,
                                sizeof(struct pollfd));
  slot_of = (int *)calloc((size_t)x->max_conns + 1, sizeof(int));
  if (fds == NULL || slot_of == NULL) {
    free(fds);
    free(slot_of);
    return 0;
  }
  fds[nfds].fd = x->listen_fd;
  fds[nfds].events = POLLIN;
  slot_of[nfds] = -1;
  nfds++;
  for (i = 0; i < x->max_conns; i++) {
    dh_conn *c = &x->conns[i];
    if (c->state == CONN_FREE)
      continue;
    /* Deadlines first, so a poll that slept does not extend one. */
    if (c->deadline_ms <= h->now_ms) {
      if (c->state == CONN_WAITING)
        respond_text(c, 504, "Gateway Timeout", "the program did not answer "
                                                "within the deadline\n");
      else {
        conn_close(c);
        continue;
      }
    }
    fds[nfds].fd = c->fd;
    fds[nfds].events = (c->state == CONN_WRITING) ? POLLOUT
                     : (c->state == CONN_READING) ? POLLIN : 0;
    if (fds[nfds].events == 0)
      continue;                        /* WAITING: nothing socket-side */
    slot_of[nfds] = i;
    nfds++;
  }
  rc = poll(fds, (nfds_t)nfds, timeout_ms);
  if (rc <= 0) {
    free(fds);
    free(slot_of);
    return 0;
  }
  for (i = 1; i < nfds; i++) {
    dh_conn *c = &x->conns[slot_of[i]];
    if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
      conn_close(c);
      continue;
    }
    if ((fds[i].revents & POLLIN) && c->state == CONN_READING) {
      char tmp[4096];
      ssize_t n = read(c->fd, tmp, sizeof(tmp));
      if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
          continue;
        conn_close(c);
        continue;
      }
      if (c->in.len + (size_t)n > (size_t)x->max_body + HTTP_HDR_ROOM) {
        respond_text(c, 413, "Content Too Large", "request past the "
                                                  "configured cap\n");
        continue;
      }
      dh_raw(&c->in, tmp, (size_t)n);
      try_parse(h, x, c);
    }
    if ((fds[i].revents & POLLOUT) && c->state == CONN_WRITING) {
      ssize_t n = write(c->fd, c->out.p + c->wrote, c->out.len - c->wrote);
      if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
          conn_close(c);
        continue;
      }
      c->wrote += (size_t)n;
      if (c->wrote >= c->out.len)
        conn_close(c);                 /* Connection: close, honestly */
    }
  }
  if (fds[0].revents & POLLIN) {
    for (;;) {
      int fd = accept(x->listen_fd, NULL, NULL);
      dh_conn *c = NULL;
      if (fd < 0)
        break;
      for (i = 0; i < x->max_conns; i++) {
        if (x->conns[i].state == CONN_FREE) {
          c = &x->conns[i];
          break;
        }
      }
      if (c == NULL) {
        /* Full table: refuse loudly rather than queue invisibly. */
        static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\n"
                                   "Content-Length: 0\r\n"
                                   "Connection: close\r\n\r\n";
        (void)!write(fd, busy, sizeof(busy) - 1);
        close(fd);
        continue;
      }
      set_nonblock(fd);
      memset(c, 0, sizeof(*c));
      c->fd = fd;
      c->state = CONN_READING;
      /* Token 0 is the "reply named no connection" sentinel in the reply
         pump, so the counter skips it on wrap -- otherwise the 2^32-th
         connection would answer to a tokenless reply. */
      if (++x->next_token == 0)
        x->next_token = 1;
      c->token = x->next_token;
      c->deadline_ms = h->now_ms + x->deadline_ms;
      dh_buf_init(&c->in);
      dh_buf_init(&c->out);
    }
  }
  free(fds);
  free(slot_of);
  return 0;
}

int dh_http_next_timeout (dh_host *h) {
  dh_http *x = (dh_http *)h->listener;
  int64_t next = -1;
  int i;
  if (x == NULL)
    return -1;
  for (i = 0; i < x->max_conns; i++) {
    if (x->conns[i].state != CONN_FREE &&
        (next < 0 || x->conns[i].deadline_ms < next))
      next = x->conns[i].deadline_ms;
  }
  if (next < 0)
    return -1;
  if (next <= h->now_ms)
    return 0;
  return (next - h->now_ms > 60000) ? 60000 : (int)(next - h->now_ms);
}

int dh_http_open (dh_host *h, char *err, size_t errcap) {
  dh_http *x;
  struct sockaddr_in addr;
  int one = 1;
  x = (dh_http *)calloc(1, sizeof(dh_http));
  if (x == NULL) {
    snprintf(err, errcap, "no memory for the listener");
    return -1;
  }
  x->max_conns = h->cfg.listener.max_conns;
  x->max_body = h->cfg.listener.max_body;
  x->deadline_ms = h->cfg.listener.conn_deadline_ms;
  memcpy(x->queue, h->cfg.listener.queue, sizeof(x->queue));
  memcpy(x->reply_queue, h->cfg.listener.reply_queue, sizeof(x->reply_queue));
  x->conns = (dh_conn *)calloc((size_t)x->max_conns, sizeof(dh_conn));
  if (x->conns == NULL) {
    free(x);
    snprintf(err, errcap, "no memory for the connection table");
    return -1;
  }
  x->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (x->listen_fd < 0) {
    free(x->conns);
    free(x);
    snprintf(err, errcap, "could not make a socket");
    return -1;
  }
  setsockopt(x->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)h->cfg.listener.port);
  if (inet_pton(AF_INET, h->cfg.listener.bind_addr, &addr.sin_addr) != 1) {
    snprintf(err, errcap, "listen.bind '%s' is not an IPv4 address",
             h->cfg.listener.bind_addr);
    close(x->listen_fd);
    free(x->conns);
    free(x);
    return -1;
  }
  if (bind(x->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(x->listen_fd, 64) != 0) {
    snprintf(err, errcap, "could not listen on %s:%d",
             h->cfg.listener.bind_addr, h->cfg.listener.port);
    close(x->listen_fd);
    free(x->conns);
    free(x);
    return -1;
  }
  set_nonblock(x->listen_fd);
  h->listener = x;
  return 0;
}

void dh_http_close (dh_host *h) {
  dh_http *x = (dh_http *)h->listener;
  int i;
  if (x == NULL)
    return;
  for (i = 0; i < x->max_conns; i++) {
    if (x->conns[i].state != CONN_FREE)
      conn_close(&x->conns[i]);
  }
  close(x->listen_fd);
  free(x->conns);
  free(x);
  h->listener = NULL;
}
