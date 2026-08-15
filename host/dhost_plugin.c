/*
** dhost_plugin.c
** The plugin channel: a capability that lives in another program.
**
** doc/BUILD8.md §2 is the contract. The shape, in one paragraph: each
** configured plugin is a process the host execs from an absolute path,
** handed one end of a socketpair as fd 3; the host writes length-prefixed
** msgpack request frames and reads reply frames back, correlating on a
** host-global wire id. Every call is taken with 'dh_defer' and settled with
** 'dh_reply', so a plugin that is slow, wedged, or dead costs the calling
** instance its latency and costs the host thread nothing.
**
** Why not stdin/stdout: a stray print, a library log line, a panic trace or
** a JVM warning would desync the framing permanently and unrecoverably, and
** the failure would look like corruption rather than like the log line it
** is. Keeping the channel on its own descriptor leaves stdout and stderr
** free for exactly that noise, which is also what makes a plugin debuggable
** by running it from a shell.
**
** The inherited-fd binding has a property worth naming: the channel has no
** filesystem path and no port, so nothing else can connect to it. Parentage
** is structural rather than negotiated, which is why build 8 ships no plugin
** authentication and does not need any (§2.7).
**
** On the wire id: it is a host-global counter, NOT the guest's 'tok'. Two
** instances pick their tokens independently and will collide; the map from
** wire id back to (instance, tok) is this file's in-flight table.
*/

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "djson.h"

#include "dmsgpack.h"
#include "dhost.h"


/* A frame's length prefix. Bounded well below what a pipe or a peer could
   claim, because the length arrives from the plugin and an unbounded one is
   an allocation the plugin gets to choose. */
#define PLUG_FRAME_MAX   (8u * 1024u * 1024u)
#define PLUG_MAX_CALLS   256
#define PLUG_VERSION     1


/* ======================================================================
** The manifest.
**
** JSON, parsed with the runtime's own 'json' library in a lua_State this
** function owns -- the same discipline dh_config_load uses for the
** deployment file, for the same reason: the tree already has a strict,
** bounded decoder (depth-limited, control bytes refused, trailing bytes
** refused), so nothing is vendored and nothing is hand-rolled.
**
** Only the flat metadata is read. 'args' and 'result' are the plugin's and
** the tooling's business; a host that validated them would need a JSON
** Schema implementation inside a binary whose whole point is to static-link
** onto a lean box.
** ====================================================================== */

static int man_fail (char *err, size_t cap, const char *fmt, const char *a,
                     const char *b) {
  snprintf(err, cap, fmt, a, b);
  return -1;
}

/* Fetch a string field; 0 when absent (leaving 'out' untouched), -1 on a
   wrong type. */
static int man_str (lua_State *L, int idx, const char *key, char *out,
                    size_t cap, const char *where, char *err, size_t errcap,
                    int *got) {
  int t = lua_getfield(L, idx, key);
  *got = 0;
  if (t == LUA_TNIL) {
    lua_pop(L, 1);
    return 0;
  }
  if (t != LUA_TSTRING) {
    lua_pop(L, 1);
    return man_fail(err, errcap, "%s: '%s' must be a string", where, key);
  }
  {
    const char *s = lua_tostring(L, -1);
    if (strlen(s) >= cap) {
      lua_pop(L, 1);
      return man_fail(err, errcap, "%s: '%s' is too long for this host", where,
                      key);
    }
    strcpy(out, s);
  }
  lua_pop(L, 1);
  *got = 1;
  return 0;
}

static int man_num (lua_State *L, int idx, const char *key, long lo, long hi,
                    long *out, const char *where, char *err, size_t errcap) {
  int t = lua_getfield(L, idx, key);
  if (t == LUA_TNIL) {
    lua_pop(L, 1);
    return 0;
  }
  if (t != LUA_TNUMBER) {
    lua_pop(L, 1);
    return man_fail(err, errcap, "%s: '%s' must be a number", where, key);
  }
  {
    lua_Number v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    if (v < (lua_Number)lo || v > (lua_Number)hi)
      return man_fail(err, errcap, "%s: '%s' is outside what this host will "
                                   "accept", where, key);
    *out = (long)v;
  }
  return 0;
}

static int read_whole (const char *path, char **out, size_t *outlen) {
  FILE *f = fopen(path, "rb");
  long n;
  char *buf;
  if (f == NULL)
    return -1;
  if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
      fseek(f, 0, SEEK_SET) != 0 || n > (long)PLUG_FRAME_MAX) {
    fclose(f);
    return -1;
  }
  buf = (char *)malloc((size_t)n + 1);
  if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
    free(buf);
    fclose(f);
    return -1;
  }
  fclose(f);
  buf[n] = '\0';
  *out = buf;
  *outlen = (size_t)n;
  return 0;
}

int dh_plugin_manifest_load (const char *path, dh_plugin_cfg *out,
                             char *err, size_t errcap) {
  lua_State *L;
  char *text = NULL;
  size_t textlen = 0;
  int rc = -1;
  long n;
  int got;

  if (read_whole(path, &text, &textlen) != 0)
    return man_fail(err, errcap, "could not read the plugin manifest '%s'%s",
                    path, "");
  L = luaL_newstate();
  if (L == NULL) {
    free(text);
    return man_fail(err, errcap, "no memory to read '%s'%s", path, "");
  }
  luaL_requiref(L, "json", luaopen_djson, 0);
  lua_getfield(L, -1, "decode");
  lua_pushlstring(L, text, textlen);
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
    man_fail(err, errcap, "the plugin manifest is not valid JSON: %s%s",
             lua_tostring(L, -1), "");
    goto done;
  }
  if (!lua_istable(L, -1)) {
    man_fail(err, errcap, "a plugin manifest must be a JSON object%s%s", "",
             "");
    goto done;
  }

  /* schema: present and 1. Refusing an unknown version by name beats
     guessing at a document written for a host that does not exist yet. */
  n = 0;
  if (man_num(L, -1, "schema", 0, 1000000, &n, "manifest", err, errcap) != 0)
    goto done;
  if (n != 1) {
    man_fail(err, errcap, "this host reads plugin manifest schema 1; '%s' "
                          "declares another%s", path, "");
    goto done;
  }

  if (lua_getfield(L, -1, "plugin") != LUA_TTABLE) {
    man_fail(err, errcap, "a plugin manifest needs a 'plugin' object saying "
                          "what the program is%s%s", "", "");
    goto done;
  }
  {
    char transport[DH_NAME_MAX];
    transport[0] = '\0';
    if (man_str(L, -1, "exec", out->exec, sizeof(out->exec), "manifest.plugin",
                err, errcap, &got) != 0)
      goto done;
    if (!got || out->exec[0] == '\0') {
      man_fail(err, errcap, "manifest.plugin needs an 'exec' path%s%s", "", "");
      goto done;
    }
    if (out->exec[0] != '/') {
      /* No PATH lookup, ever. A plugin path comes from a manifest an
         operator wrote; resolving it through the environment would be an
         injection surface with nothing on the other side of the trade. */
      man_fail(err, errcap, "manifest.plugin.exec must be an absolute path; "
                            "'%s' is not, and this host never searches PATH "
                            "for a plugin%s", out->exec, "");
      goto done;
    }
    if (man_str(L, -1, "checksum", out->checksum, sizeof(out->checksum),
                "manifest.plugin", err, errcap, &got) != 0)
      goto done;
    if (man_str(L, -1, "transport", transport, sizeof(transport),
                "manifest.plugin", err, errcap, &got) != 0)
      goto done;
    if (got && transport[0] != '\0' && strcmp(transport, "socketpair") != 0) {
      man_fail(err, errcap, "this host binds the plugin channel to a "
                            "socketpair; '%s' is a binding it does not "
                            "have%s", transport, "");
      goto done;
    }
    /* The manifest's max_inflight is the plugin author's statement of what
       the program can take. A deployment may lower it (the operator's
       statement of what this box will send), which dh_config_load has
       already written here -- so only fill it in when nothing did. */
    if (out->max_inflight <= 0) {
      n = 0;
      if (man_num(L, -1, "max_inflight", 1, 4096, &n, "manifest.plugin", err,
                  errcap) != 0)
        goto done;
      out->max_inflight = (n > 0) ? n : 4;
    }
    n = 0;
    if (man_num(L, -1, "call_timeout_ms", 1, 3600000, &n, "manifest.plugin",
                err, errcap) != 0)
      goto done;
    if (n > 0)
      out->call_timeout_ms = n;
  }
  lua_pop(L, 1);                         /* manifest.plugin */

  if (lua_getfield(L, -1, "capabilities") != LUA_TTABLE) {
    man_fail(err, errcap, "a plugin manifest needs a 'capabilities' array%s%s",
             "", "");
    goto done;
  }
  {
    lua_Integer i, ncaps = (lua_Integer)lua_rawlen(L, -1);
    if (ncaps <= 0) {
      man_fail(err, errcap, "a plugin that answers nothing is a typo, not a "
                            "deployment%s%s", "", "");
      goto done;
    }
    if (ncaps > DH_MAX_PLUGIN_CAPS) {
      man_fail(err, errcap, "'%s' declares more capabilities than this host "
                            "holds%s", path, "");
      goto done;
    }
    for (i = 1; i <= ncaps; i++) {
      char wake[DH_NAME_MAX];
      dh_plugin_cap *cp = &out->caps[out->ncaps];
      if (lua_rawgeti(L, -1, i) != LUA_TTABLE) {
        man_fail(err, errcap, "every entry of 'capabilities' must be an "
                              "object%s%s", "", "");
        goto done;
      }
      memset(cp, 0, sizeof(*cp));
      wake[0] = '\0';
      if (man_str(L, -1, "name", cp->name, sizeof(cp->name),
                  "manifest.capabilities[]", err, errcap, &got) != 0)
        goto done;
      if (!got || cp->name[0] == '\0') {
        man_fail(err, errcap, "every capability needs a 'name'%s%s", "", "");
        goto done;
      }
      if (strchr(cp->name, '/') != NULL) {
        man_fail(err, errcap, "a capability name is one segment; '%s' has a "
                              "'/', and the plugin's own name supplies the "
                              "other half%s", cp->name, "");
        goto done;
      }
      if (man_str(L, -1, "wake", wake, sizeof(wake),
                  "manifest.capabilities[]", err, errcap, &got) != 0)
        goto done;
      /* Required, and deliberately so: there is no default that is right
         for everything, and a silent wrong one would only be discovered by
         a program waking up with a reissued payment. */
      if (!got || wake[0] == '\0') {
        man_fail(err, errcap, "capability '%s' must declare a 'wake' policy "
                              "-- reissue, cached or error -- because the "
                              "host cannot guess whether asking twice is "
                              "safe%s", cp->name, "");
        goto done;
      }
      if (strcmp(wake, "reissue") == 0)     cp->wake = DH_WAKE_REISSUE;
      else if (strcmp(wake, "cached") == 0) cp->wake = DH_WAKE_CACHED;
      else if (strcmp(wake, "error") == 0)  cp->wake = DH_WAKE_ERROR;
      else {
        man_fail(err, errcap, "capability '%s': 'wake' is reissue, cached or "
                              "error; '%s' is none of them", cp->name, wake);
        goto done;
      }
      /* 'args', 'result' and 'errors' are read past on purpose. See the
         header: the runtime reads metadata, not schema. */
      out->ncaps++;
      lua_pop(L, 1);                     /* the capability object */
    }
  }
  rc = 0;
done:
  free(text);
  lua_close(L);
  return rc;
}


/* ======================================================================
** Runtime state.
** ====================================================================== */

typedef enum plug_state {
  PC_FREE = 0,
  PC_WAITING,                           /* accepted, frame not yet written */
  PC_INFLIGHT                           /* frame written, reply outstanding */
} plug_state;

typedef struct plug_call {
  plug_state state;
  uint64_t wire;
  dvs_id id;
  int64_t tok;
  dh_buf req;                           /* the framed bytes, until written */
} plug_call;

typedef struct plug_rt {
  dh_host *h;
  dh_plugin_cfg cfg;
  pid_t pid;
  int fd;                               /* host end of the socketpair */
  int alive;
  uint64_t next_wire;
  plug_call calls[PLUG_MAX_CALLS];
  size_t inflight;
  dh_buf out;                           /* framed bytes awaiting write */
  size_t out_off;                       /* how much of 'out' has gone */
  unsigned char *in;                    /* inbound accumulator */
  size_t in_len, in_cap;
} plug_rt;

typedef struct plug_all {
  plug_rt *p[DH_MAX_PLUGINS];
  size_t n;
} plug_all;


static plug_call *call_slot (plug_rt *p) {
  int i;
  for (i = 0; i < PLUG_MAX_CALLS; i++) {
    if (p->calls[i].state == PC_FREE)
      return &p->calls[i];
  }
  return NULL;
}

static plug_call *call_by_wire (plug_rt *p, uint64_t wire) {
  int i;
  for (i = 0; i < PLUG_MAX_CALLS; i++) {
    if (p->calls[i].state == PC_INFLIGHT && p->calls[i].wire == wire)
      return &p->calls[i];
  }
  return NULL;
}

static void call_release (plug_rt *p, plug_call *c) {
  if (c->state == PC_INFLIGHT && p->inflight > 0)
    p->inflight--;
  dh_buf_free(&c->req);
  memset(c, 0, sizeof(*c));
  c->state = PC_FREE;
}


/* ======================================================================
** Spawning.
**
** The discipline is dhost_exec.c's, which is the worked example in this
** tree, with two deliberate differences: fd 3 survives the close sweep
** because it IS the channel, and the exec is 'execv' against an absolute
** path rather than 'execvp'. A guest naming 'git' should find git; a
** manifest naming a plugin should name the file.
** ====================================================================== */

static int plug_spawn (plug_rt *p, char *err, size_t errcap) {
  int sv[2];
  pid_t pid;
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    snprintf(err, errcap, "no channel for plugin '%s': %s", p->cfg.name,
             strerror(errno));
    return -1;
  }
  pid = fork();
  if (pid < 0) {
    close(sv[0]);
    close(sv[1]);
    snprintf(err, errcap, "plugin '%s' would not fork: %s", p->cfg.name,
             strerror(errno));
    return -1;
  }
  if (pid == 0) {
    long maxfd;
    int fd;
    char *argv[2];
    setpgid(0, 0);
    signal(SIGPIPE, SIG_DFL);
    close(sv[0]);
    /* The channel lands on fd 3 and nowhere else. dup2 to a scratch first,
       because sv[1] may already BE 3 and dup2(3,3) would leave the
       close-on-nothing sweep below to shut it. */
    if (sv[1] != 3) {
      if (dup2(sv[1], 3) < 0)
        _exit(127);
      close(sv[1]);
    }
    maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 65536) maxfd = 65536;
    for (fd = 4; fd < (int)maxfd; fd++)
      close(fd);
    /* stdin from nowhere; stdout and stderr stay the host's, which is what
       makes a plugin's logging visible where an operator is already
       looking. */
    argv[0] = p->cfg.exec;
    argv[1] = NULL;
    execv(p->cfg.exec, argv);
    _exit(127);
  }
  setpgid(pid, pid);
  close(sv[1]);
  p->pid = pid;
  p->fd = sv[0];
  fcntl(p->fd, F_SETFL, O_NONBLOCK);
  p->alive = 1;
  return 0;
}


/* ======================================================================
** Failing calls.
** ====================================================================== */

/*
** Answer a call with one of the three classes. The class is the first word
** of the detail so a guest can match on it -- 'transport' is retryable and
** may recover on its own, 'plugin' is a bug on the other side, 'capability'
** is the service the plugin fronts having said no. §2.5.
*/
static void fail_call (plug_rt *p, plug_call *c, const char *cls,
                       const char *why) {
  char detail[256];
  dvs_id id = c->id;
  int64_t tok = c->tok;
  snprintf(detail, sizeof(detail), "%s: %s", cls, why);
  call_release(p, c);
  dh_reply(p->h, id, tok, DH_CALL_ERROR, NULL, 0, detail);
}

static void plug_die (plug_rt *p, const char *why) {
  int i;
  if (p->fd >= 0) {
    close(p->fd);
    p->fd = -1;
  }
  p->alive = 0;
  for (i = 0; i < PLUG_MAX_CALLS; i++) {
    if (p->calls[i].state != PC_FREE)
      fail_call(p, &p->calls[i], "transport", why);
  }
  p->out.len = 0;
  p->out_off = 0;
  p->in_len = 0;
}


/* ======================================================================
** Writing.
** ====================================================================== */

/* Promote waiting calls into the write buffer while the plugin has room.
   max_inflight is what keeps a plugin with a simple read-work-reply loop
   from being handed forty frames it will answer serially -- the pipe would
   fill at about 64KB and the host's write would block, which is exactly the
   failure this whole build exists to remove. */
static void plug_promote (plug_rt *p) {
  int i;
  for (i = 0; i < PLUG_MAX_CALLS; i++) {
    plug_call *c = &p->calls[i];
    if (c->state != PC_WAITING)
      continue;
    if ((long)p->inflight >= p->cfg.max_inflight)
      return;
    if (dh_raw(&p->out, c->req.p, c->req.len) != 0)
      return;                            /* out of memory: try again later */
    dh_buf_free(&c->req);
    c->state = PC_INFLIGHT;
    p->inflight++;
  }
}

/* Write what we can. Never blocks: the fd is non-blocking and a partial
   write is retained, which is the guarantee max_inflight is only a bound
   on. */
static void plug_write (plug_rt *p) {
  while (p->alive && p->out_off < p->out.len) {
    ssize_t n = write(p->fd, p->out.p + p->out_off, p->out.len - p->out_off);
    if (n > 0) {
      p->out_off += (size_t)n;
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return;                            /* the peer is behind; try later */
    if (n < 0 && errno == EINTR)
      continue;
    plug_die(p, "the plugin's channel would not accept a request");
    return;
  }
  if (p->out_off > 0 && p->out_off == p->out.len) {
    p->out.len = 0;                      /* drained: reuse the allocation */
    p->out_off = 0;
  }
}


/* ======================================================================
** Reading.
** ====================================================================== */

static int in_reserve (plug_rt *p, size_t need) {
  if (p->in_len + need <= p->in_cap)
    return 0;
  {
    size_t want = (p->in_cap == 0) ? 4096 : p->in_cap;
    unsigned char *q;
    while (want < p->in_len + need)
      want *= 2;
    if (want > PLUG_FRAME_MAX + 64)
      return -1;
    q = (unsigned char *)realloc(p->in, want);
    if (q == NULL)
      return -1;
    p->in = q;
    p->in_cap = want;
  }
  return 0;
}

/* One reply frame's body. Correlate, decode, settle. */
static void plug_reply_frame (plug_rt *p, const unsigned char *body,
                              size_t len) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  uint64_t wire = 0;
  int has_wire = 0;
  plug_call *pc;

  diluvium_mp_open(&c, body, len);
  if (diluvium_mp_field(&c, "id") && diluvium_mp_read(&c, &t) &&
      t.kind == DILUVIUM_MP_INT && t.i >= 0) {
    wire = (uint64_t)t.i;
    has_wire = 1;
  }
  if (!has_wire)
    return;                              /* uncorrelatable: nothing to answer */
  pc = call_by_wire(p, wire);
  if (pc == NULL)
    return;                              /* already failed, or never ours */

  /* An error object wins over a value: a frame carrying both is the
     plugin's bug, and reporting the failure is the safer read of it. */
  diluvium_mp_open(&c, body, len);
  if (diluvium_mp_field(&c, "error")) {
    const unsigned char *errstart = c.p;
    diluvium_mp_cursor e;
    char cls[32], msg[192];
    strcpy(cls, "plugin");
    msg[0] = '\0';
    if (diluvium_mp_read(&c, &t) && t.kind != DILUVIUM_MP_NIL) {
      diluvium_mp_open(&e, errstart, len - (size_t)(errstart - body));
      if (diluvium_mp_field(&e, "class") && diluvium_mp_read(&e, &t) &&
          t.kind == DILUVIUM_MP_STR && t.len < sizeof(cls)) {
        memcpy(cls, t.p, t.len);
        cls[t.len] = '\0';
        if (strcmp(cls, "transport") != 0 && strcmp(cls, "plugin") != 0 &&
            strcmp(cls, "capability") != 0)
          strcpy(cls, "plugin");         /* an unknown class is the plugin's */
      }
      diluvium_mp_open(&e, errstart, len - (size_t)(errstart - body));
      if (diluvium_mp_field(&e, "message") && diluvium_mp_read(&e, &t) &&
          t.kind == DILUVIUM_MP_STR && t.len < sizeof(msg)) {
        memcpy(msg, t.p, t.len);
        msg[t.len] = '\0';
      }
      {
        char code[64];
        code[0] = '\0';
        diluvium_mp_open(&e, errstart, len - (size_t)(errstart - body));
        if (diluvium_mp_field(&e, "code") && diluvium_mp_read(&e, &t) &&
            t.kind == DILUVIUM_MP_STR && t.len < sizeof(code)) {
          memcpy(code, t.p, t.len);
          code[t.len] = '\0';
        }
        if (code[0] != '\0' && msg[0] != '\0') {
          /* code is 64 and msg is 192, so the pair plus its separator can
             just exceed 256. Sized to hold them rather than sized to look
             round: a truncated diagnostic is the one case where losing the
             end of the sentence loses the part that said what to do. */
          char joined[sizeof(code) + sizeof(msg) + 4];
          snprintf(joined, sizeof(joined), "%s: %s", code, msg);
          fail_call(p, pc, cls, joined);
        }
        else
          fail_call(p, pc, cls,
                    (msg[0] != '\0') ? msg
                    : (code[0] != '\0') ? code
                    : "the plugin refused without saying why");
      }
      return;
    }
  }

  diluvium_mp_open(&c, body, len);
  if (diluvium_mp_field(&c, "value")) {
    const unsigned char *start = c.p;
    if (diluvium_mp_skip(&c)) {
      dvs_id id = pc->id;
      int64_t tok = pc->tok;
      size_t vlen = (size_t)(c.p - start);
      call_release(p, pc);
      dh_reply(p->h, id, tok, DH_CALL_OK, start, vlen, NULL);
      return;
    }
  }
  /* No error and no readable value. Answering nothing would hang the guest
     until its own timeout, which diagnoses the wrong party. */
  fail_call(p, pc, "plugin",
            "the reply frame carried neither a value nor an error");
}

static void plug_read (plug_rt *p) {
  for (;;) {
    ssize_t n;
    if (in_reserve(p, 4096) != 0) {
      plug_die(p, "no memory to read the plugin's reply");
      return;
    }
    n = read(p->fd, p->in + p->in_len, p->in_cap - p->in_len);
    if (n > 0) {
      p->in_len += (size_t)n;
      /* Consume every complete frame in the buffer. */
      for (;;) {
        uint32_t flen;
        if (p->in_len < 4)
          break;
        flen = ((uint32_t)p->in[0] << 24) | ((uint32_t)p->in[1] << 16) |
               ((uint32_t)p->in[2] << 8) | (uint32_t)p->in[3];
        if (flen == 0 || flen > PLUG_FRAME_MAX) {
          plug_die(p, "the plugin sent a frame length this host will not "
                      "allocate");
          return;
        }
        if (p->in_len < (size_t)flen + 4) {
          if (in_reserve(p, (size_t)flen + 4 - p->in_len) != 0) {
            plug_die(p, "no memory for the plugin's frame");
            return;
          }
          break;                         /* partial: wait for the rest */
        }
        plug_reply_frame(p, p->in + 4, flen);
        memmove(p->in, p->in + 4 + flen, p->in_len - 4 - flen);
        p->in_len -= 4 + (size_t)flen;
        if (!p->alive)
          return;
      }
      continue;
    }
    if (n == 0) {
      plug_die(p, "the plugin closed its channel");
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;
    if (errno == EINTR)
      continue;
    plug_die(p, "the plugin's channel failed");
    return;
  }
}


/* ======================================================================
** The connector.
** ====================================================================== */

static dh_call_status conn_plug (void *ud, dvs_id id, int64_t tok,
                                 const char *call,
                                 const unsigned char *args, size_t argslen,
                                 dh_buf *value, char *detail,
                                 size_t detailcap) {
  plug_rt *p = (plug_rt *)ud;
  const char *slash = strchr(call, '/');
  const char *cap = (slash != NULL) ? slash + 1 : "";
  plug_call *c;
  size_t i;
  (void)value;

  if (!p->alive) {
    snprintf(detail, detailcap,
             "transport: the '%s' plugin is not running; this host does not "
             "restart a plugin that died", p->cfg.name);
    return DH_CALL_ERROR;
  }
  /* The manifest is the list of what this plugin answers, so a typo is
     caught here rather than by a plugin that has to invent a refusal. */
  for (i = 0; i < p->cfg.ncaps; i++) {
    if (strcmp(p->cfg.caps[i].name, cap) == 0)
      break;
  }
  if (i == p->cfg.ncaps) {
    snprintf(detail, detailcap,
             "the '%s' plugin does not declare '%s' in its manifest",
             p->cfg.name, cap);
    return DH_CALL_ERROR;
  }
  c = call_slot(p);
  if (c == NULL) {
    snprintf(detail, detailcap,
             "transport: the '%s' plugin already holds every call slot this "
             "host will keep", p->cfg.name);
    return DH_CALL_ERROR;
  }

  memset(c, 0, sizeof(*c));
  dh_buf_init(&c->req);
  c->wire = ++p->next_wire;
  c->id = id;
  c->tok = tok;

  /* Frame it now, while the argument bytes are still in hand. Nothing held
     across the deferral points into the guest's memory: the request is
     copied into the connector's own buffer here, and the reply is copied
     out of the inbound buffer there. */
  {
    dh_buf body;
    dh_buf_init(&body);
    dh_map(&body, 4);
    dh_str(&body, "version"); dh_uint(&body, PLUG_VERSION);
    dh_str(&body, "id");      dh_uint(&body, c->wire);
    dh_str(&body, "target");  dh_str(&body, cap);
    dh_str(&body, "args");
    if (args != NULL && argslen > 0)
      dh_raw(&body, args, argslen);
    else
      dh_nil(&body);
    if (body.len == 0 || body.len > PLUG_FRAME_MAX) {
      dh_buf_free(&body);
      dh_buf_free(&c->req);
      memset(c, 0, sizeof(*c));
      snprintf(detail, detailcap, "the request would not frame");
      return DH_CALL_ERROR;
    }
    {
      unsigned char hdr[4];
      hdr[0] = (unsigned char)(body.len >> 24);
      hdr[1] = (unsigned char)(body.len >> 16);
      hdr[2] = (unsigned char)(body.len >> 8);
      hdr[3] = (unsigned char)(body.len);
      dh_raw(&c->req, hdr, 4);
      dh_raw(&c->req, body.p, body.len);
    }
    dh_buf_free(&body);
  }

  {
    int64_t deadline = (p->cfg.call_timeout_ms > 0)
                     ? dh_now_ms() + p->cfg.call_timeout_ms : 0;
    if (dh_defer(p->h, id, tok, c, deadline) != 0) {
      dh_buf_free(&c->req);
      memset(c, 0, sizeof(*c));
      snprintf(detail, detailcap,
               "the host would not take this call as pending");
      return DH_CALL_ERROR;
    }
  }
  c->state = PC_WAITING;
  /* Try to get it moving now; whatever does not fit rides the next turn. */
  plug_promote(p);
  plug_write(p);
  return DH_CALL_PENDING;
}

/* The host is dropping a pending entry -- its instance died, or a deadline
   fired. Release the slot without answering: the ledger has already gone,
   and a dh_reply against it would be refused anyway. */
static void plug_cancel (void *ud, dvs_id id, int64_t tok, void *callud) {
  plug_rt *p = (plug_rt *)ud;
  plug_call *c = (plug_call *)callud;
  (void)id; (void)tok;
  if (c != NULL && c->state != PC_FREE)
    call_release(p, c);
}


/* ======================================================================
** Open, poll, close.
** ====================================================================== */

int dh_plug_open (dh_host *h, char *err, size_t errcap) {
  plug_all *all;
  size_t i;
  if (h->cfg.nplugins == 0)
    return 0;
  all = (plug_all *)calloc(1, sizeof(plug_all));
  if (all == NULL)
    return snprintf(err, errcap, "no memory for the plugin channel"), -1;
  h->plugctx = all;
  for (i = 0; i < h->cfg.nplugins; i++) {
    plug_rt *p = (plug_rt *)calloc(1, sizeof(plug_rt));
    if (p == NULL) {
      snprintf(err, errcap, "no memory for plugin '%s'",
               h->cfg.plugins[i].name);
      return -1;
    }
    p->h = h;
    p->cfg = h->cfg.plugins[i];
    p->fd = -1;
    dh_buf_init(&p->out);
    all->p[all->n++] = p;
    if (plug_spawn(p, err, errcap) != 0)
      return -1;
    if (dh_register_deferrable(h, p->cfg.name, conn_plug, plug_cancel, p)
        != 0) {
      snprintf(err, errcap, "plugin '%s' could not claim its call prefix; "
                            "another connector already answers it",
               p->cfg.name);
      return -1;
    }
    /* Recorded, not enforced. A checksum in the log is forensics today and
       a one-line change to enforcement later; enforcing it now would buy
       little against an attacker who could equally swap this binary. */
    fprintf(stderr, "diluvium-host: plugin '%s' up: %s%s%s\n", p->cfg.name,
            p->cfg.exec,
            (p->cfg.checksum[0] != '\0') ? " checksum " : "",
            (p->cfg.checksum[0] != '\0') ? p->cfg.checksum : "");
    if (p->cfg.checksum[0] == '\0' && !h->cfg.insecure_plugins)
      fprintf(stderr, "diluvium-host: plugin '%s' declares no checksum; run "
                      "with --insecure-plugins to say that is deliberate\n",
              p->cfg.name);
  }
  return 0;
}

void dh_plug_close (dh_host *h) {
  plug_all *all = (plug_all *)h->plugctx;
  size_t i;
  if (all == NULL)
    return;
  for (i = 0; i < all->n; i++) {
    plug_rt *p = all->p[i];
    int k;
    if (p == NULL)
      continue;
    if (p->fd >= 0)
      close(p->fd);
    if (p->pid > 0) {
      /* The channel closing is the plugin's cue to exit; SIGTERM to the
         group is for one that does not take it. */
      kill(-p->pid, SIGTERM);
      waitpid(p->pid, NULL, 0);
    }
    for (k = 0; k < PLUG_MAX_CALLS; k++)
      dh_buf_free(&p->calls[k].req);
    dh_buf_free(&p->out);
    free(p->in);
    free(p);
  }
  free(all);
  h->plugctx = NULL;
}

size_t dh_plug_arm (dh_host *h, struct pollfd *fds, size_t cap) {
  plug_all *all = (plug_all *)h->plugctx;
  size_t i, n = 0;
  if (all == NULL)
    return 0;
  for (i = 0; i < all->n && n < cap; i++) {
    plug_rt *p = all->p[i];
    if (p == NULL || !p->alive || p->fd < 0)
      continue;
    plug_promote(p);
    fds[n].fd = p->fd;
    fds[n].events = POLLIN;
    if (p->out_off < p->out.len)
      fds[n].events |= POLLOUT;
    fds[n].revents = 0;
    n++;
  }
  return n;
}

void dh_plug_fire (dh_host *h, struct pollfd *fds, size_t n) {
  plug_all *all = (plug_all *)h->plugctx;
  size_t i, k;
  if (all == NULL)
    return;
  for (k = 0; k < n; k++) {
    if (fds[k].revents == 0)
      continue;
    for (i = 0; i < all->n; i++) {
      plug_rt *p = all->p[i];
      if (p == NULL || !p->alive || p->fd != fds[k].fd)
        continue;
      if (fds[k].revents & POLLOUT)
        plug_write(p);
      if (p->alive && (fds[k].revents & (POLLIN | POLLHUP | POLLERR)))
        plug_read(p);
      break;
    }
  }
  /* And a promote/write pass regardless, so a call accepted between polls
     starts moving without waiting for the descriptor to say something. */
  for (i = 0; i < all->n; i++) {
    plug_rt *p = all->p[i];
    if (p != NULL && p->alive) {
      plug_promote(p);
      plug_write(p);
    }
  }
}

int dh_plug_busy (dh_host *h) {
  plug_all *all = (plug_all *)h->plugctx;
  size_t i;
  int k;
  if (all == NULL)
    return 0;
  for (i = 0; i < all->n; i++) {
    plug_rt *p = all->p[i];
    if (p == NULL)
      continue;
    for (k = 0; k < PLUG_MAX_CALLS; k++) {
      if (p->calls[k].state != PC_FREE)
        return 1;
    }
  }
  return 0;
}
