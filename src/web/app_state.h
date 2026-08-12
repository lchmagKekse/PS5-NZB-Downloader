/* Shared state reachable from every HTTP handler thread (MHD runs one
 * thread per connection) and the download orchestrator thread. queue_t
 * is documented as not thread-safe on its own (see queue.h) -- every
 * access anywhere in the app must go through queue_lock()/queue_unlock().
 * config is similarly guarded so a POST /api/config from one browser tab
 * can't race a GET /api/config from another.
 */
#pragma once

#include <pthread.h>
#include <stddef.h>

#include "../config/config.h"
#include "../nntp/nntp_pool.h"
#include "../queue/queue.h"

typedef struct {
  app_config_t     config;
  pthread_mutex_t  config_mu;
  char             config_path[512];

  queue_t         *queue;
  pthread_mutex_t  queue_mu;

  /* Rebuilt in place when NNTP settings change (app_reload_pool_if_needed()).
   * pool_mu guards every read and the swap/destroy so no thread ever
   * dereferences a pool mid-teardown. Only rebuilt between jobs (never
   * mid-download, which would cancel in-flight segments and fail a
   * perfectly good job). */
  nntp_pool_t     *pool;
  pthread_mutex_t  pool_mu;
  int              pool_reload_needed; /* guarded by config_mu; see api_config_post() */

  /* Download-speed tracking for GET /api/status: total_bytes is a
   * monotonic counter the downloader adds to; api_status_get() diffs it
   * against this (bytes, time) snapshot, only advancing the snapshot once
   * >=1s has passed -- with multiple tabs polling independently, always
   * overwriting it lets one poller perpetually land in another's window,
   * computing a ~0 delta forever. last_poll_time_ms is CLOCK_MONOTONIC
   * milliseconds, not whole seconds -- integer-second truncation swung
   * the reported rate 2x+ between polls on real hardware. */
  pthread_mutex_t  stats_mu;
  unsigned long    total_bytes;
  unsigned long    last_poll_bytes;
  long long        last_poll_time_ms; /* 0 == no previous sample yet */
  unsigned long    last_speed;        /* cached bytes/sec from the last real sample */

  /* Extraction progress for GET /api/jobs (job_json.c's progress_json()).
   * Only one job ever extracts at a time (finalizer thread), so a single
   * set of fields suffices; empty extract_job_id means none in progress.
   * Set/cleared around download.c's extract_job() call, updated from
   * extract.c's progress callback. */
  pthread_mutex_t  extract_mu;
  char             extract_job_id[JOB_ID_LEN];
  long long        extract_bytes_done;
  long long        extract_bytes_total;

  /* Same idea, for PAR2 verification -- see par2_verify_job() in download.c. */
  pthread_mutex_t  verify_mu;
  char             verify_job_id[JOB_ID_LEN];
  long long        verify_bytes_done;
  long long        verify_bytes_total;

  /* Same idea, for PAR2 repair -- see par2_repair_job() in download.c.
   * bytes_done/total count input bytes read during reconstruction, not
   * output written, but still climb meaningfully toward total. */
  pthread_mutex_t  repair_mu;
  char             repair_job_id[JOB_ID_LEN];
  long long        repair_bytes_done;
  long long        repair_bytes_total;
} app_state_t;

/* One process, one instance -- every web/download module reaches it here
 * rather than threading it through every function call. */
extern app_state_t g_app;

void app_state_init(const app_config_t *config, const char *config_path, queue_t *queue, nntp_pool_t *pool);

/* Fills out from cfg's NNTP settings -- shared by main() (initial pool) and
 * app_reload_pool_if_needed() (rebuilt pool) so the two can't drift. */
void app_build_pool_opts(const app_config_t *cfg, nntp_pool_opts_t *out);

/* If a config change flagged the pool for a rebuild (see api_config.c),
 * rebuilds it from the current config and swaps it in. Cheap no-op
 * otherwise. Must only be called from a point where no job is actively
 * downloading -- currently just downloader_main()'s loop, between jobs. */
void app_reload_pool_if_needed(void);

static inline void queue_lock(void)   { pthread_mutex_lock(&g_app.queue_mu); }
static inline void queue_unlock(void) { pthread_mutex_unlock(&g_app.queue_mu); }

static inline void config_lock(void)   { pthread_mutex_lock(&g_app.config_mu); }
static inline void config_unlock(void) { pthread_mutex_unlock(&g_app.config_mu); }

static inline void pool_lock(void)   { pthread_mutex_lock(&g_app.pool_mu); }
static inline void pool_unlock(void) { pthread_mutex_unlock(&g_app.pool_mu); }

static inline void extract_lock(void)   { pthread_mutex_lock(&g_app.extract_mu); }
static inline void extract_unlock(void) { pthread_mutex_unlock(&g_app.extract_mu); }

static inline void verify_lock(void)   { pthread_mutex_lock(&g_app.verify_mu); }
static inline void verify_unlock(void) { pthread_mutex_unlock(&g_app.verify_mu); }

static inline void repair_lock(void)   { pthread_mutex_lock(&g_app.repair_mu); }
static inline void repair_unlock(void) { pthread_mutex_unlock(&g_app.repair_mu); }

/* Called by the downloader as decoded bytes are written to disk. */
static inline void
app_stats_add_bytes(size_t n) {
  pthread_mutex_lock(&g_app.stats_mu);
  g_app.total_bytes += n;
  pthread_mutex_unlock(&g_app.stats_mu);
}
