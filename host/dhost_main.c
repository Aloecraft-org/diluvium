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

#include "lua.h"                        /* LUA_RELEASE */
#include "dhost.h"

/*
** Which build this binary is.
**
** The VERSION file is the single source of truth and the makefile passes it
** in, so there is no second copy to drift. A build that does not define it
** still compiles and says so, which is more useful than a number that might
** be a lie.
**
** This exists because it was missing when it was needed: a host built before
** a feature refused a config that used it, correctly and by name, and the
** only way to find out that the BINARY was old rather than the config wrong
** was to go and read the source. `--version`, and the startup banner, are
** the two places that question gets asked.
*/
#ifndef DILUVIUM_HOST_BUILD
#define DILUVIUM_HOST_BUILD "(version not compiled in)"
#endif

int main (int argc, char **argv) {
  dh_config cfg;
  dh_host h;
  char err[512];
  if (argc == 2 && (strcmp(argv[1], "--version") == 0 ||
                    strcmp(argv[1], "-v") == 0)) {
    printf("diluvium-host %s (%s)\n", DILUVIUM_HOST_BUILD, LUA_RELEASE);
    return 0;
  }
  if (argc != 2 || strcmp(argv[1], "--help") == 0) {
    fprintf(stderr, "usage: %s <deployment.host.lua>\n"
                    "       %s --version\n"
                    "The config format is host/types/host.lua; the duties "
                    "are doc/Host.md.\n", argv[0], argv[0]);
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
    fprintf(stderr, "diluvium-host %s: supervisor '%s' up%s%s%s%s%s%s\n",
            DILUVIUM_HOST_BUILD, cfg.supervisor,
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
    /* A drained swarm still waits on a plugin that owes it an answer: the
       instance that asked may be gone, but the plugin is a child of this
       process and exiting out from under it would orphan the work. */
    if (!alive && h.listener == NULL && !dh_plug_busy(&h))
      break;                           /* drained, and nothing can arrive */
    /* Bound the sleep so guest timeouts and the ledger's own backstops fire
       near their moment even when every descriptor is quiet. */
    dh_host_sleep(&h, (timeout < 0 || timeout > 100) ? 100 : timeout);
  }
  dh_host_close(&h);
  fprintf(stderr, "diluvium-host: the swarm drained; done\n");
  return 0;
}
