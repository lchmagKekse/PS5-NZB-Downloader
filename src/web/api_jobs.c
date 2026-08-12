#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../log/log.h"
#include "../nntp/nntp_pool.h"
#include "../nzb/nzb_parse.h"
#include "../storage/paths.h"
#include "api.h"
#include "app_state.h"
#include "job_json.h"
#include "json_util.h"

#define MAX_JOBS_LISTED 512

enum MHD_Result
api_jobs_list(struct MHD_Connection *conn) {
  cJSON *arr = cJSON_CreateArray();
  job_t *jobs[MAX_JOBS_LISTED];
  size_t n, i;

  queue_lock();
  n = queue_list_jobs(g_app.queue, jobs, MAX_JOBS_LISTED);
  for (i = 0; i < n; i++) cJSON_AddItemToArray(arr, job_to_json_summary(jobs[i]));
  queue_unlock();

  return json_respond(conn, MHD_HTTP_OK, arr);
}

enum MHD_Result
api_jobs_get(struct MHD_Connection *conn, const char *id) {
  job_t *job;
  cJSON *body = NULL;

  queue_lock();
  job = queue_find_job(g_app.queue, id);
  if (job) body = job_to_json_detail(job);
  queue_unlock();

  if (!body) return json_respond_error(conn, MHD_HTTP_NOT_FOUND, "no such job");
  return json_respond(conn, MHD_HTTP_OK, body);
}

/* Writes the uploaded NZB to a scratch file so nzb_parse_file() (which
 * streams from disk) doesn't need a second in-memory copy of it. */
enum MHD_Result
api_jobs_create(struct MHD_Connection *conn, const unsigned char *nzb_data,
                size_t nzb_len, const char *nzb_filename, const char *display_name,
                const char *output_dir, int add_to_shadowmount) {
  char scratch_dir[600], scratch_path[768];
  unsigned char rand_bytes[8];
  char rand_hex[17];
  FILE *f;
  job_t *job;
  cJSON *body;
  int i;

  if (nzb_len == 0) {
    return json_respond_error(conn, MHD_HTTP_BAD_REQUEST, "empty NZB upload");
  }

  config_lock();
  snprintf(scratch_dir, sizeof scratch_dir, "%s/.uploads", g_app.config.storage.temp_dir);
  config_unlock();

  if (mkdir_p(scratch_dir, 0755) < 0) {
    return json_respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "could not prepare upload scratch space");
  }

  arc4random_buf(rand_bytes, sizeof rand_bytes);
  for (i = 0; i < 8; i++) snprintf(rand_hex + i * 2, 3, "%02x", rand_bytes[i]);
  snprintf(scratch_path, sizeof scratch_path, "%s/%s.nzb", scratch_dir, rand_hex);

  if (!(f = fopen(scratch_path, "wb"))) {
    log_error("api_jobs_create: fopen(%s): %s", scratch_path, strerror(errno));
    return json_respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "could not buffer upload to disk");
  }
  if (fwrite(nzb_data, 1, nzb_len, f) != nzb_len) {
    fclose(f);
    remove(scratch_path);
    return json_respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "short write buffering upload");
  }
  fclose(f);

  job = nzb_parse_file(scratch_path);
  remove(scratch_path);

  if (!job) {
    return json_respond_error(conn, MHD_HTTP_BAD_REQUEST, "could not parse NZB (malformed XML?)");
  }
  if (job->file_count == 0) {
    job_free(job);
    return json_respond_error(conn, MHD_HTTP_BAD_REQUEST, "NZB has no files");
  }

  /* nzb_parse_file() names the job after the scratch path's random hex
   * basename; prefer the original filename, or display_name if the client
   * sent one. Also becomes the output directory name (download.c). */
  if (display_name && display_name[0]) {
    snprintf(job->name, sizeof job->name, "%s", display_name);
    path_sanitize_component(job->name, sizeof job->name);
  } else if (nzb_filename && nzb_filename[0]) {
    snprintf(job->name, sizeof job->name, "%s", nzb_filename);
    path_sanitize_component(job->name, sizeof job->name);
  }

  /* Full path, not a single component, so unlike job->name this doesn't
   * go through path_sanitize_component() -- same trust level as
   * storage.output_dir, also free-form text from the Settings page. */
  if (output_dir && output_dir[0]) {
    snprintf(job->output_dir, sizeof job->output_dir, "%s", output_dir);
  }
  job->add_to_shadowmount = add_to_shadowmount;

  queue_lock();
  if (queue_add_job(g_app.queue, job) < 0) {
    queue_unlock();
    return json_respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "could not persist job");
  }
  body = job_to_json_detail(job);
  queue_unlock();

  return json_respond(conn, MHD_HTTP_CREATED, body);
}

typedef int (*job_action_fn)(queue_t *, const char *);

/* cancel_pool: pause/cancel need this, resume/retry don't. Flipping
 * job->state alone stops new segments from being dispatched but leaves
 * already-queued ones in the NNTP pool to keep downloading regardless --
 * nntp_pool_cancel_all() drops those (in-flight ones still finish). Safe
 * unconditionally: only one job downloads at a time, so the pool's queue
 * never holds a different job's segments. */
static enum MHD_Result
run_action(struct MHD_Connection *conn, const char *id, job_action_fn fn, int cancel_pool) {
  int rc;

  queue_lock();
  rc = fn(g_app.queue, id);
  queue_unlock();

  if (rc < 0) {
    return json_respond_error(conn, MHD_HTTP_CONFLICT,
                               "unknown job id, or action not valid for this job's current state");
  }

  if (cancel_pool) {
    pool_lock();
    nntp_pool_cancel_all(g_app.pool);
    pool_unlock();
  }

  return json_respond(conn, MHD_HTTP_OK, cJSON_CreateObject());
}

enum MHD_Result api_jobs_pause(struct MHD_Connection *conn, const char *id)  { return run_action(conn, id, queue_pause_job, 1); }
enum MHD_Result api_jobs_resume(struct MHD_Connection *conn, const char *id) { return run_action(conn, id, queue_resume_job, 0); }
enum MHD_Result api_jobs_cancel(struct MHD_Connection *conn, const char *id) { return run_action(conn, id, queue_cancel_job, 1); }
enum MHD_Result api_jobs_retry(struct MHD_Connection *conn, const char *id)  { return run_action(conn, id, queue_retry_job, 0); }

enum MHD_Result
api_jobs_delete(struct MHD_Connection *conn, const char *id) {
  char temp_dir[700];
  int rc;

  config_lock();
  snprintf(temp_dir, sizeof temp_dir, "%s/%s", g_app.config.storage.temp_dir, id);
  config_unlock();

  queue_lock();
  rc = queue_remove_job(g_app.queue, id);
  queue_unlock();

  if (rc == -2) {
    return json_respond_error(conn, MHD_HTTP_CONFLICT,
        "job is actively downloading/verifying/repairing/extracting -- cancel it first");
  }
  if (rc < 0) return json_respond_error(conn, MHD_HTTP_NOT_FOUND, "no such job");

  /* Best-effort: the job record is gone either way, so a cleanup
   * failure here is logged, not surfaced as a DELETE failure. */
  rmdir_recursive(temp_dir);

  return json_respond(conn, MHD_HTTP_OK, cJSON_CreateObject());
}
