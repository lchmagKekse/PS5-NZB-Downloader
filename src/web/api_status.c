#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../log/log.h"
#include "api.h"
#include "app_state.h"
#include "job_json.h"
#include "json_util.h"

#define MAX_JOBS_FOR_STATUS 512
#define MAX_RECENT_ERRORS 10
#define ERR_LINE_MAX 200

typedef struct {
  char lines[MAX_RECENT_ERRORS][ERR_LINE_MAX];
  int  count;
  int  next;
} error_ring_t;

static void
collect_error_cb(void *ctx, const char *line) {
  error_ring_t *er = ctx;

  if (strncmp(line, "[ERROR]", 7)) return;

  snprintf(er->lines[er->next], ERR_LINE_MAX, "%s", line);
  er->next = (er->next + 1) % MAX_RECENT_ERRORS;
  if (er->count < MAX_RECENT_ERRORS) er->count++;
}

enum MHD_Result
api_status_get(struct MHD_Connection *conn) {
  cJSON *root = cJSON_CreateObject();
  cJSON *active = cJSON_CreateArray();
  cJSON *errors = cJSON_CreateArray();
  error_ring_t er = {0};
  job_t *jobs[MAX_JOBS_FOR_STATUS];
  size_t n, i, queue_len;
  unsigned long speed;
  struct timespec ts;
  long long now_ms;
  int active_conns, max_conns, start, j;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  now_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

  queue_lock();
  n = queue_list_jobs(g_app.queue, jobs, MAX_JOBS_FOR_STATUS);
  queue_len = queue_job_count(g_app.queue);
  for (i = 0; i < n; i++) {
    if (jobs[i]->state == JOB_DOWNLOADING || jobs[i]->state == JOB_VERIFYING ||
        jobs[i]->state == JOB_EXTRACTING) {
      cJSON_AddItemToArray(active, job_to_json_summary(jobs[i]));
    }
  }
  queue_unlock();

  pthread_mutex_lock(&g_app.stats_mu);
  if (g_app.last_poll_time_ms == 0) {
    /* First sample ever -- nothing to diff against yet. */
    g_app.last_poll_bytes = g_app.total_bytes;
    g_app.last_poll_time_ms = now_ms;
    g_app.last_speed = 0;
  } else {
    long long dt_ms = now_ms - g_app.last_poll_time_ms;

    /* Only advance the snapshot once ~1s has elapsed -- see app_state.h's
     * stats_mu comment for why multiple pollers need this gate. */
    if (dt_ms >= 1000) {
      unsigned long dbytes = g_app.total_bytes - g_app.last_poll_bytes;
      g_app.last_speed = (unsigned long)((dbytes * 1000ULL) / (unsigned long long)dt_ms);
      g_app.last_poll_bytes = g_app.total_bytes;
      g_app.last_poll_time_ms = now_ms;
    }
  }
  speed = g_app.last_speed;
  pthread_mutex_unlock(&g_app.stats_mu);

  pool_lock();
  active_conns = nntp_pool_active_count(g_app.pool);
  pool_unlock();

  config_lock();
  max_conns = g_app.config.nntp.max_connections;
  config_unlock();

  log_for_each_recent(collect_error_cb, &er);
  start = er.count < MAX_RECENT_ERRORS ? 0 : er.next;
  for (j = 0; j < er.count; j++) {
    cJSON_AddItemToArray(errors, cJSON_CreateString(er.lines[(start + j) % MAX_RECENT_ERRORS]));
  }

  cJSON_AddItemToObject(root, "active_jobs", active);
  cJSON_AddNumberToObject(root, "queue_length", (double)queue_len);
  cJSON_AddNumberToObject(root, "download_speed_bytes_per_sec", (double)speed);
  cJSON_AddNumberToObject(root, "active_connections", active_conns);
  cJSON_AddNumberToObject(root, "max_connections", max_conns);
  cJSON_AddItemToObject(root, "recent_errors", errors);

  return json_respond(conn, MHD_HTTP_OK, root);
}
