#include "../log/log.h"
#include "../nntp/nntp_pool.h"
#include "api.h"
#include "app_state.h"
#include "httpd.h"
#include "json_util.h"

#define MAX_JOBS_TO_SCAN 512

/* Cancels whatever job is currently downloading/verifying/repairing/
 * extracting so download.c's loops (dl_should_continue(), and the
 * verify/repair/extract progress callbacks via shutdown_requested) unwind
 * instead of running that job to completion. At most one such job exists
 * at a time (single downloader + single finalizer thread), but this scans
 * rather than assumes that, same as api_jobs_list(). */
static void
cancel_active_job(void) {
  job_t *jobs[MAX_JOBS_TO_SCAN];
  size_t n, i;

  queue_lock();
  n = queue_list_jobs(g_app.queue, jobs, MAX_JOBS_TO_SCAN);
  for (i = 0; i < n; i++) {
    if (jobs[i]->state == JOB_DOWNLOADING || jobs[i]->state == JOB_VERIFYING ||
        jobs[i]->state == JOB_REPAIRING || jobs[i]->state == JOB_EXTRACTING) {
      queue_cancel_job(g_app.queue, jobs[i]->id);
    }
  }
  queue_unlock();
}

/* Same clean-shutdown path main.c's SIGINT/SIGTERM handler triggers --
 * httpd_stop() unblocks httpd_listen()'s accept loop (within ~1s, see its
 * select() timeout), main() finishes tearing down and returns, and the
 * payload hands control back to the loader. Unlike a plain Ctrl+C, this
 * also cancels whatever job is active and sets shutdown_requested
 * (app_state.h) so main()'s downloader_stop()/nntp_pool_destroy() join
 * promptly instead of blocking until an in-progress download/verify/
 * repair/extraction runs to completion -- the whole point of "eject" is
 * not needing to wait, or reboot, between test runs. Queuing the response
 * before any of that gives this connection's reply a full daemon lifetime
 * to flush before MHD_stop_daemon() runs. */
enum MHD_Result
api_system_eject(struct MHD_Connection *conn) {
  enum MHD_Result res = json_respond(conn, MHD_HTTP_OK, cJSON_CreateObject());

  log_info("api_system: eject requested - cancelling any active job and shutting down");

  cancel_active_job();

  /* Drops segments still queued (not yet dispatched) in the NNTP pool --
   * same as run_action()'s cancel_pool in api_jobs.c. In-flight segments
   * finish their current read regardless, same as a plain job cancel. */
  pool_lock();
  nntp_pool_cancel_all(g_app.pool);
  pool_unlock();

  /* Set last: every abort check downstream (dl_should_continue(), the
   * three progress callbacks in download.c) reads this, so it must be
   * visible before httpd_stop() lets main() start tearing down. */
  g_app.shutdown_requested = 1;

  httpd_stop();

  return res;
}
