#include <stdio.h>
#include <string.h>

#include "../log/log.h"
#include "app_state.h"

app_state_t g_app;

void
app_state_init(const app_config_t *config, const char *config_path, queue_t *queue, nntp_pool_t *pool) {
  memset(&g_app, 0, sizeof g_app);

  g_app.config = *config;
  snprintf(g_app.config_path, sizeof g_app.config_path, "%s", config_path);
  pthread_mutex_init(&g_app.config_mu, NULL);

  g_app.queue = queue;
  pthread_mutex_init(&g_app.queue_mu, NULL);

  g_app.pool = pool;
  pthread_mutex_init(&g_app.pool_mu, NULL);

  pthread_mutex_init(&g_app.stats_mu, NULL);

  pthread_mutex_init(&g_app.extract_mu, NULL);
  pthread_mutex_init(&g_app.verify_mu, NULL);
  pthread_mutex_init(&g_app.repair_mu, NULL);
}

void
app_build_pool_opts(const app_config_t *cfg, nntp_pool_opts_t *out) {
  memset(out, 0, sizeof *out);

  out->conn.host = cfg->nntp.host;
  out->conn.port = cfg->nntp.port;
  out->conn.tls = cfg->nntp.tls;
  out->conn.connect_timeout_sec = cfg->nntp.connect_timeout_sec;
  out->conn.read_timeout_sec = cfg->nntp.read_timeout_sec;

  snprintf(out->user, sizeof out->user, "%s", cfg->nntp.user);
  snprintf(out->pass, sizeof out->pass, "%s", cfg->nntp.pass);
  out->max_connections = cfg->nntp.max_connections;
  out->retry_count = cfg->nntp.retry_count;
}

void
app_reload_pool_if_needed(void) {
  app_config_t snapshot;
  nntp_pool_opts_t opts;
  nntp_pool_t *new_pool, *old_pool;

  config_lock();
  if (!g_app.pool_reload_needed) {
    config_unlock();
    return;
  }
  g_app.pool_reload_needed = 0;
  snapshot = g_app.config;
  config_unlock();

  app_build_pool_opts(&snapshot, &opts);

  if (!(new_pool = nntp_pool_create(&opts))) {
    log_error("app_state: failed to rebuild NNTP pool with updated settings -- keeping the previous one");
    return;
  }

  pool_lock();
  old_pool = g_app.pool;
  g_app.pool = new_pool;
  nntp_pool_destroy(old_pool); /* still holding pool_mu: see the struct comment in app_state.h */
  pool_unlock();

  log_info("app_state: NNTP pool rebuilt with updated settings (%s:%s, %d connection(s))",
           snapshot.nntp.host, snapshot.nntp.port, snapshot.nntp.max_connections);
}
