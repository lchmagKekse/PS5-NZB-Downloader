/* Entry point: loads config, opens the persistent queue, brings up the
 * NNTP connection pool, starts the download orchestrator, and blocks in
 * the HTTP server until a signal asks us to stop. See BUILD_TOOLCHAIN.md
 * / Makefile.probe for how this gets built and deployed.
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "config/config.h"
#include "download/download.h"
#include "log/log.h"
#include "nntp/nntp_pool.h"
#include "par2/rs.h"
#include "queue/queue.h"
#include "util/notify.h"
#include "web/app_state.h"
#include "web/httpd.h"
#include "yenc/yenc.h"

#define DEFAULT_CONFIG_PATH "/data/nzb/nzb.conf"
#define DEFAULT_QUEUE_DIR   "/data/nzb/queue"
#define DEFAULT_LOG_PATH    "/data/nzb/nzb.log"
#define HTTP_PORT 4202

static void
on_signal(int sig) {
  (void)sig;
  httpd_stop();
}

/* Best-effort: picks the first up, non-loopback IPv4 address for the
 * startup notification, so the console's owner doesn't have to dig up the
 * IP some other way. Falls back to a placeholder on any failure. */
static void
get_local_ip(char *out, size_t out_size) {
  struct ifaddrs *ifap, *ifa;

  snprintf(out, out_size, "this console's IP");

  if (getifaddrs(&ifap) != 0) return;

  for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
    struct sockaddr_in *sin;

    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
    if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;

    sin = (struct sockaddr_in *)ifa->ifa_addr;
    if (inet_ntop(AF_INET, &sin->sin_addr, out, out_size)) break;
  }

  freeifaddrs(ifap);
}

int
main(int argc, char **argv) {
  const char *cfgpath = argc > 1 ? argv[1] : DEFAULT_CONFIG_PATH;
  app_config_t cfg;
  nntp_pool_opts_t pool_opts = {0};
  queue_t *queue;
  nntp_pool_t *pool;
  downloader_t *downloader;

  /* INFO, not DEBUG: a line per NNTP response (DNS/connect/TLS/every "S: ..."
   * reply) drowns out download.c's own job/file/segment-level logging,
   * which is enough on its own to reconstruct what happened around a fault. */
  log_init("nzb", LOG_INFO, DEFAULT_LOG_PATH);
  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  /* Not thread-safe (sets up rapidyenc's lookup tables/CPU detection) --
   * must happen here, before nntp_pool_create() below starts any worker
   * thread that could call into the decoder. */
  yenc_global_init();

  /* Builds the GF(2^16) log/antilog tables PAR2's Reed-Solomon math depends
   * on (see par2/rs.h) -- not thread-safe, same rationale as above. */
  gf16_init();

  /* app_config_load() fills cfg with defaults even when the file can't
   * be read, so it's always safe to use afterward -- first run just
   * means an empty NNTP host until the user sets one via Settings. */
  if (app_config_load(cfgpath, &cfg) < 0) {
    log_warn("main: no usable config at %s yet - starting with defaults, configure via the Settings page", cfgpath);
  }

  if (!(queue = queue_open(DEFAULT_QUEUE_DIR, cfg.storage.temp_dir))) {
    log_error("main: queue_open(%s) failed, exiting", DEFAULT_QUEUE_DIR);
    return 1;
  }

  app_build_pool_opts(&cfg, &pool_opts);

  if (!(pool = nntp_pool_create(&pool_opts))) {
    log_error("main: nntp_pool_create failed, exiting");
    queue_close(queue);
    return 1;
  }

  app_state_init(&cfg, cfgpath, queue, pool);

  if (!(downloader = downloader_start())) {
    log_error("main: downloader_start failed, exiting");
    nntp_pool_destroy(pool);
    queue_close(queue);
    return 1;
  }

  {
    char ip[64];

    get_local_ip(ip, sizeof ip);
    log_info("main: nzb downloader starting, web UI on port %d", HTTP_PORT);
    notify("Serving NZB Downloader UI on %s:%d", ip, HTTP_PORT);
  }

  httpd_listen(HTTP_PORT); /* blocks until httpd_stop() (SIGINT/SIGTERM) */

  log_info("main: shutting down");
  downloader_stop(downloader);
  /* g_app.pool, not the local `pool` -- app_reload_pool_if_needed() may
   * have swapped in a rebuilt pool since startup (see app_state.c). */
  nntp_pool_destroy(g_app.pool);
  queue_close(queue);

  return 0;
}
