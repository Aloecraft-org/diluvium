/*
** dhost_main.c
** The generic host's entry point: a config, a loop, an exit code.
**
**   diluvium-host deployment.host.lua
**
** Everything interesting is in dhost.c and doc/Host.md; what main owns is
** the outer loop's pacing. dvs_step is cheap on a parked swarm, so the loop
** sleeps on poll() when there is a listener and on a plain nanosleep when
** there is not -- a swarm with no listener, no elapsed timeouts and nothing
** runnable is waiting for an outside world this deployment did not wire,
** and burning a core would not hurry it.
*/

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dhost.h"

int main (int argc, char **argv) {
  dh_config cfg;
  dh_host h;
  char err[512];
  if (argc != 2 || strcmp(argv[1], "--help") == 0) {
    fprintf(stderr, "usage: %s <deployment.host.lua>\n"
                    "The config format is host/types/host.lua; the duties "
                    "are doc/Host.md.\n", argv[0]);
    return 2;
  }
  if (dh_config_load(argv[1], &cfg, err, sizeof(err)) != 0) {
    fprintf(stderr, "%s: %s\n", argv[1], err);
    return 2;
  }
  if (dh_host_open(&h, &cfg, err, sizeof(err)) != 0) {
    fprintf(stderr, "%s: %s\n", argv[1], err);
    return 1;
  }
  {
    char listening[48];
    listening[0] = '\0';
    if (cfg.nlisteners == 1)
      snprintf(listening, sizeof(listening), ", listening on 1 port");
    else if (cfg.nlisteners > 1)
      snprintf(listening, sizeof(listening), ", listening on %d ports",
               (int)cfg.nlisteners);
    fprintf(stderr, "diluvium-host: supervisor '%s' up%s%s%s%s%s%s\n",
            cfg.supervisor,
            listening,
            cfg.sql.enabled ? ", sql wired" : "",
            cfg.crypto.enabled ? ", crypto wired" : "",
            cfg.fs.enabled ? ", fs wired" : "",
            cfg.exec.enabled ? ", exec wired" : "",
            cfg.time_connector ? ", time wired" : "");
  }
  for (;;) {
    int alive = dh_host_turn(&h);
    int timeout = dh_host_poll_timeout(&h);
    if (!alive && h.listener == NULL)
      break;                           /* drained, and nothing can arrive */
    if (h.listener != NULL) {
      /* Bound the sleep so guest timeouts fire near their moment even when
         the sockets are quiet. */
      int t = (timeout < 0 || timeout > 100) ? 100 : timeout;
      dh_http_poll(&h, t);
    }
    else {
      struct timespec ts;
      int t = (timeout < 0 || timeout > 50) ? 50 : timeout;
      ts.tv_sec = t / 1000;
      ts.tv_nsec = (long)(t % 1000) * 1000000;
      nanosleep(&ts, NULL);
    }
  }
  dh_host_close(&h);
  fprintf(stderr, "diluvium-host: the swarm drained; done\n");
  return 0;
}
