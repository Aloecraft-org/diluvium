/*
** dhost_exec.c
** The exec connector: 'host:exec/run', the honest escape hatch.
**
**   exec/run {argv = {...}, stdin?, timeout_ms?, cwd?}
**            -> {status = n, stdout = "...", stderr = "..."}
**
** Granting exec is leaving the sandbox -- doc/Capabilities.md says it in
** those words -- so the surface here is the *bounding*, because the
** instruction budget cannot reach a subprocess:
**
**   - No shell, by construction: argv is a vector handed to execvp, so
**     there is no string a quote can escape from. A program that wants a
**     shell asks for one by name, visibly: {argv = {'sh', '-c', ...}}.
**   - A wall-clock deadline: the config caps it, a call may ask for less,
**     and a child still running at the deadline is killed (SIGKILL -- the
**     polite signal would be a negotiation with a runaway) and the call
**     answers ERROR. The kill sweeps the child's whole process GROUP, and
**     so does every exit from the call: exec/run is a bounded
**     request/reply, so nothing it starts outlives it -- a program that
**     wants a daemon wants a supervisor, not an escape hatch.
**   - An output cap on each stream: past it the child is killed and the
**     call answers ERROR -- refused rather than truncated, like every
**     other cap in this host.
**
** A nonzero exit is NOT an error: {status = 1} is the child's answer, and
** the program reads it the way a shell script reads $? -- a program that
** does not exist included, which is {status = 127}, the shell's own
** convention. ERROR is reserved for the call itself failing (the deadline,
** an output cap, a malformed request).
**
** One more honesty note, in the config's face rather than buried: the host
** answers hostcalls synchronously, so a running child stalls every guest
** and the listener until it exits or hits the deadline. Bound it tight.
**
** Replay: the reply is a message like any other, logged and replayed --
** a replay does NOT re-run the subprocess.
*/

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "dmsgpack.h"
#include "dhost.h"

#define EXEC_MAX_ARGS 64

typedef struct dh_exec {
  long max_timeout_ms;
  long max_output_bytes;                /* per stream */
} dh_exec;

typedef struct exec_buf {
  char *p;
  size_t len, cap;
} exec_buf;

static int eb_grow (exec_buf *b, size_t need) {
  if (b->len + need <= b->cap)
    return 0;
  {
    size_t ncap = (b->cap == 0) ? 4096 : b->cap * 2;
    char *np;
    while (ncap < b->len + need) ncap *= 2;
    np = (char *)realloc(b->p, ncap);
    if (np == NULL)
      return -1;
    b->p = np;
    b->cap = ncap;
  }
  return 0;
}

static int64_t now_ms (void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static dh_call_status conn_exec (void *ud, dvs_id id, const char *call,
                                 const unsigned char *args, size_t argslen,
                                 dh_buf *value, char *detail,
                                 size_t detailcap) {
  dh_exec *s = (dh_exec *)ud;
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  char *argv[EXEC_MAX_ARGS + 1];
  size_t nargs = 0, i;
  const char *stdin_p = NULL;
  size_t stdin_len = 0;
  char *cwd = NULL;
  long timeout_ms;
  int in_pipe[2] = { -1, -1 }, out_pipe[2] = { -1, -1 },
      err_pipe[2] = { -1, -1 };
  pid_t pid = -1;
  pid_t pg = -1;
  exec_buf outb = { NULL, 0, 0 }, errb = { NULL, 0, 0 };
  dh_call_status result = DH_CALL_ERROR;
  int exit_status = -1;
  (void)id;

  memset(argv, 0, sizeof(argv));
  if (strcmp(call, "exec/run") != 0) {
    snprintf(detail, detailcap, "the exec connector answers 'exec/run'; "
                                "'%s' is not it", call);
    return DH_CALL_DENIED;
  }
  timeout_ms = s->max_timeout_ms;

  /* args.argv: an array of strings, copied out NUL-terminated. */
  if (args == NULL) {
    snprintf(detail, detailcap, "args.argv must be the command vector");
    return DH_CALL_ERROR;
  }
  diluvium_mp_open(&c, args, argslen);
  if (!diluvium_mp_field(&c, "argv") || !diluvium_mp_read(&c, &t) ||
      t.kind != DILUVIUM_MP_ARRAY || t.len == 0) {
    snprintf(detail, detailcap, "args.argv must be a non-empty array of "
                                "strings (a vector, not a shell string)");
    return DH_CALL_ERROR;
  }
  if (t.len > EXEC_MAX_ARGS) {
    snprintf(detail, detailcap, "argv is longer than the host's bound (%d)",
             EXEC_MAX_ARGS);
    return DH_CALL_ERROR;
  }
  nargs = t.len;
  for (i = 0; i < nargs; i++) {
    if (!diluvium_mp_read(&c, &t) || t.kind != DILUVIUM_MP_STR ||
        memchr(t.p, '\0', t.len) != NULL) {
      snprintf(detail, detailcap, "argv[%d] is not a plain string",
               (int)i + 1);
      goto cleanup_argv;
    }
    argv[i] = (char *)malloc(t.len + 1);
    if (argv[i] == NULL) {
      snprintf(detail, detailcap, "no memory for argv");
      goto cleanup_argv;
    }
    memcpy(argv[i], t.p, t.len);
    argv[i][t.len] = '\0';
  }
  argv[nargs] = NULL;

  diluvium_mp_open(&c, args, argslen);
  if (diluvium_mp_field(&c, "stdin") && diluvium_mp_read(&c, &t) &&
      t.kind == DILUVIUM_MP_STR) {
    stdin_p = t.p;
    stdin_len = t.len;
    if (stdin_len > (size_t)s->max_output_bytes) {
      snprintf(detail, detailcap, "stdin is bigger than this deployment's "
                                  "byte cap (%ld)", s->max_output_bytes);
      goto cleanup_argv;
    }
  }
  diluvium_mp_open(&c, args, argslen);
  if (diluvium_mp_field(&c, "timeout_ms") && diluvium_mp_read(&c, &t)) {
    if (t.kind != DILUVIUM_MP_INT || t.i <= 0) {
      snprintf(detail, detailcap, "timeout_ms must be a positive integer");
      goto cleanup_argv;
    }
    if (t.i > s->max_timeout_ms) {
      snprintf(detail, detailcap, "timeout_ms passed the host's ceiling "
                                  "(%ld ms); a call may ask for less, "
                                  "never more", s->max_timeout_ms);
      goto cleanup_argv;
    }
    timeout_ms = (long)t.i;
  }
  diluvium_mp_open(&c, args, argslen);
  if (diluvium_mp_field(&c, "cwd") && diluvium_mp_read(&c, &t) &&
      t.kind == DILUVIUM_MP_STR && t.len > 0 &&
      memchr(t.p, '\0', t.len) == NULL) {
    cwd = (char *)malloc(t.len + 1);
    if (cwd == NULL) {
      snprintf(detail, detailcap, "no memory for cwd");
      goto cleanup_argv;
    }
    memcpy(cwd, t.p, t.len);
    cwd[t.len] = '\0';
  }

  if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
    snprintf(detail, detailcap, "no pipes for the child");
    goto cleanup_pipes;
  }
  pid = fork();
  if (pid < 0) {
    snprintf(detail, detailcap, "the child would not fork: %s",
             strerror(errno));
    goto cleanup_pipes;
  }
  if (pid == 0) {
    /* Child. Its own process group, so the deadline can kill the whole
       tree and not just the direct child; SIGPIPE back to default, because
       the host's ignore is process state execvp would otherwise hand to a
       program expecting a normal world; and every fd above the standard
       three closed -- the listener's sockets and the databases' fds are
       the host's, and 'leaving the sandbox' does not mean taking them. */
    long maxfd;
    int fd;
    setpgid(0, 0);
    signal(SIGPIPE, SIG_DFL);
    dup2(in_pipe[0], 0);
    dup2(out_pipe[1], 1);
    dup2(err_pipe[1], 2);
    maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 65536) maxfd = 65536;
    for (fd = 3; fd < (int)maxfd; fd++)
      close(fd);
    if (cwd != NULL && chdir(cwd) != 0)
      _exit(127);
    execvp(argv[0], argv);
    _exit(127);
  }
  /* Both sides set the group: whichever runs first, a kill(-pid) after
     this line reaches the child and everything it spawns. */
  setpgid(pid, pid);
  close(in_pipe[0]);  in_pipe[0] = -1;
  close(out_pipe[1]); out_pipe[1] = -1;
  close(err_pipe[1]); err_pipe[1] = -1;
  fcntl(in_pipe[1], F_SETFL, O_NONBLOCK);
  fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
  fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);

  pg = pid;                             /* the group every kill sweeps */

  {
    int64_t deadline = now_ms() + timeout_ms;
    size_t stdin_off = 0;
    int out_open = 1, err_open = 1;
    int in_open = 1;
    int child_exited = 0;
    if (stdin_p == NULL || stdin_len == 0) {
      close(in_pipe[1]); in_pipe[1] = -1; in_open = 0;
    }
    while (out_open || err_open || in_open) {
      struct pollfd fds[3];
      int nfds = 0, oi = -1, ei = -1, ii = -1;
      int64_t left = deadline - now_ms();
      int prc;
      if (left <= 0) {
        kill(-pg, SIGKILL);
        if (pid > 0) { waitpid(pid, NULL, 0); pid = -1; }
        snprintf(detail, detailcap, "the child was killed at the %ld ms "
                                    "deadline", timeout_ms);
        goto cleanup_pipes;
      }
      if (out_open) { fds[nfds].fd = out_pipe[0]; fds[nfds].events = POLLIN;
                      oi = nfds++; }
      if (err_open) { fds[nfds].fd = err_pipe[0]; fds[nfds].events = POLLIN;
                      ei = nfds++; }
      if (in_open)  { fds[nfds].fd = in_pipe[1]; fds[nfds].events = POLLOUT;
                      ii = nfds++; }
      prc = poll(fds, (nfds_t)nfds, (int)(left > 100 ? 100 : left));
      if (prc < 0) {
        if (errno == EINTR)
          continue;                      /* revents is indeterminate */
        kill(-pg, SIGKILL);
        if (pid > 0) { waitpid(pid, NULL, 0); pid = -1; }
        snprintf(detail, detailcap, "poll failed on the child's pipes");
        goto cleanup_pipes;
      }
      if (oi >= 0 && (fds[oi].revents & (POLLIN | POLLHUP))) {
        char tmp[4096];
        ssize_t n = read(out_pipe[0], tmp, sizeof(tmp));
        if (n > 0) {
          if (outb.len + (size_t)n > (size_t)s->max_output_bytes) {
            kill(-pg, SIGKILL);
            if (pid > 0) { waitpid(pid, NULL, 0); pid = -1; }
            snprintf(detail, detailcap, "stdout passed this deployment's "
                                        "byte cap (%ld); the child was "
                                        "killed, the output refused",
                     s->max_output_bytes);
            goto cleanup_pipes;
          }
          if (eb_grow(&outb, (size_t)n) != 0) goto nomem;
          memcpy(outb.p + outb.len, tmp, (size_t)n);
          outb.len += (size_t)n;
        }
        else if (n == 0) { close(out_pipe[0]); out_pipe[0] = -1; out_open = 0; }
      }
      if (ei >= 0 && (fds[ei].revents & (POLLIN | POLLHUP))) {
        char tmp[4096];
        ssize_t n = read(err_pipe[0], tmp, sizeof(tmp));
        if (n > 0) {
          if (errb.len + (size_t)n > (size_t)s->max_output_bytes) {
            kill(-pg, SIGKILL);
            if (pid > 0) { waitpid(pid, NULL, 0); pid = -1; }
            snprintf(detail, detailcap, "stderr passed this deployment's "
                                        "byte cap (%ld); the child was "
                                        "killed, the output refused",
                     s->max_output_bytes);
            goto cleanup_pipes;
          }
          if (eb_grow(&errb, (size_t)n) != 0) goto nomem;
          memcpy(errb.p + errb.len, tmp, (size_t)n);
          errb.len += (size_t)n;
        }
        else if (n == 0) { close(err_pipe[0]); err_pipe[0] = -1; err_open = 0; }
      }
      if (ii >= 0 && (fds[ii].revents & (POLLOUT | POLLERR | POLLHUP))) {
        ssize_t n = write(in_pipe[1], stdin_p + stdin_off,
                          stdin_len - stdin_off);
        if (n > 0)
          stdin_off += (size_t)n;
        if (n < 0 && errno != EAGAIN && errno != EINTR)
          stdin_off = stdin_len;          /* the child closed its side */
        if (stdin_off >= stdin_len) {
          close(in_pipe[1]); in_pipe[1] = -1; in_open = 0;
        }
      }
      /* The child may exit while a descendant keeps its pipes open --
         'sh -c "server &"'. Waiting for EOF then would run to the
         deadline and report a kill that never happened; the honest answer
         is the child's own exit, with whatever its pipes held when it
         left. So: notice the exit, take what is buffered, stop. */
      if (!child_exited && pid > 0) {
        int wst;
        if (waitpid(pid, &wst, WNOHANG) == pid) {
          pid = -1;
          child_exited = 1;
          if (WIFEXITED(wst))
            exit_status = WEXITSTATUS(wst);
          else if (WIFSIGNALED(wst))
            exit_status = 128 + WTERMSIG(wst);
        }
      }
      if (child_exited) {
        int drained = 1;
        while (drained) {
          drained = 0;
          if (out_open) {
            char tmp[4096];
            ssize_t n = read(out_pipe[0], tmp, sizeof(tmp));
            if (n > 0) {
              if (outb.len + (size_t)n > (size_t)s->max_output_bytes) {
                kill(-pg, SIGKILL);
                snprintf(detail, detailcap, "stdout passed this "
                                            "deployment's byte cap (%ld); "
                                            "the output is refused",
                         s->max_output_bytes);
                goto cleanup_pipes;
              }
              if (eb_grow(&outb, (size_t)n) != 0) goto nomem;
              memcpy(outb.p + outb.len, tmp, (size_t)n);
              outb.len += (size_t)n;
              drained = 1;
            }
          }
          if (err_open) {
            char tmp[4096];
            ssize_t n = read(err_pipe[0], tmp, sizeof(tmp));
            if (n > 0) {
              if (errb.len + (size_t)n > (size_t)s->max_output_bytes) {
                kill(-pg, SIGKILL);
                snprintf(detail, detailcap, "stderr passed this "
                                            "deployment's byte cap (%ld); "
                                            "the output is refused",
                         s->max_output_bytes);
                goto cleanup_pipes;
              }
              if (eb_grow(&errb, (size_t)n) != 0) goto nomem;
              memcpy(errb.p + errb.len, tmp, (size_t)n);
              errb.len += (size_t)n;
              drained = 1;
            }
          }
        }
        break;
      }
    }
    if (!child_exited) {
      /* Streams are drained; now the exit, still under the same deadline. */
      for (;;) {
        int wstatus;
        pid_t w = waitpid(pid, &wstatus, WNOHANG);
        if (w == pid) {
          pid = -1;
          if (WIFEXITED(wstatus))
            exit_status = WEXITSTATUS(wstatus);
          else if (WIFSIGNALED(wstatus))
            exit_status = 128 + WTERMSIG(wstatus);
          break;
        }
        if (now_ms() >= deadline) {
          kill(-pg, SIGKILL);
          waitpid(pid, NULL, 0);
          pid = -1;
          snprintf(detail, detailcap, "the child was killed at the %ld ms "
                                      "deadline", timeout_ms);
          goto cleanup_pipes;
        }
        {
          struct timespec ts = { 0, 2 * 1000 * 1000 };
          nanosleep(&ts, NULL);
        }
      }
    }
  }

  dh_map(value, 3);
  dh_str(value, "status");
  dh_int(value, (int64_t)exit_status);
  dh_str(value, "stdout");
  dh_lstr(value, (outb.p != NULL) ? outb.p : "", outb.len);
  dh_str(value, "stderr");
  dh_lstr(value, (errb.p != NULL) ? errb.p : "", errb.len);
  result = DH_CALL_OK;
  goto cleanup_pipes;

nomem:
  if (pg > 0) kill(-pg, SIGKILL);
  if (pid > 0) { waitpid(pid, NULL, 0); pid = -1; }
  snprintf(detail, detailcap, "no memory for the child's output");

cleanup_pipes:
  if (pid > 0) { kill(-pg, SIGKILL); waitpid(pid, NULL, 0); }
  /* The group may hold reparented descendants past the child itself --
     sweep it on the way out, every path. ESRCH just means it is empty. */
  if (pg > 0) kill(-pg, SIGKILL);
  if (in_pipe[0] >= 0) close(in_pipe[0]);
  if (in_pipe[1] >= 0) close(in_pipe[1]);
  if (out_pipe[0] >= 0) close(out_pipe[0]);
  if (out_pipe[1] >= 0) close(out_pipe[1]);
  if (err_pipe[0] >= 0) close(err_pipe[0]);
  if (err_pipe[1] >= 0) close(err_pipe[1]);
  free(outb.p);
  free(errb.p);
cleanup_argv:
  for (i = 0; i < nargs; i++)
    free(argv[i]);
  free(cwd);
  return result;
}

int dh_exec_open (dh_host *h, char *err, size_t errcap) {
  dh_exec *s = (dh_exec *)malloc(sizeof(dh_exec));
  if (s == NULL) {
    snprintf(err, errcap, "no memory for the exec connector");
    return -1;
  }
  s->max_timeout_ms = h->cfg.exec.max_timeout_ms;
  s->max_output_bytes = h->cfg.exec.max_output_bytes;
  if (dh_register(h, "exec", conn_exec, s) != 0) {
    snprintf(err, errcap, "could not register the exec connector");
    free(s);
    return -1;
  }
  h->execctx = s;
  return 0;
}

void dh_exec_close (dh_host *h) {
  free(h->execctx);
  h->execctx = NULL;
}
