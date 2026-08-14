/* The download orchestrator: glue between the NNTP connection pool and the
 * job queue. Owns two background threads: one downloads+PAR2-verifies/
 * repairs jobs one at a time (every segment of the job in progress is
 * still dispatched concurrently across the whole nntp_pool); the other
 * drains a small internal queue extracting+finalizing each finished job,
 * pipelined alongside the next job's download -- see download.c's top
 * comment for why and how.
 *
 * Reads/writes g_app (see app_state.h); takes no parameters since there
 * is exactly one of each per process.
 */
#pragma once

#include <stddef.h>

#include "../queue/job.h"

typedef struct downloader downloader_t;

/* Spawns both orchestrator threads (download, and extract/finalize).
 * Returns NULL on failure (already logged) -- if either thread fails to
 * start, neither is left running. */
downloader_t *downloader_start(void);

/* Stops the download loop and waits for the in-flight job to finish
 * downloading, then stops the finalizer and waits for it to finish
 * extracting whatever is already queued -- a shutdown never abandons a
 * job mid-extraction. Joins both threads and frees d. Safe with d == NULL. */
void downloader_stop(downloader_t *d);

/* Computes the directory finalize_job() extracts/moves a job's real output
 * into (storage.output_dir or job->output_dir, plus a sanitized subdir
 * derived from job->name) -- exposed so api_jobs_delete() can clean up an
 * incomplete job's partial output the same way finalize_job() would have
 * placed it. out[0] is left 0 if no output_dir is configured at all (see
 * download.c's job_output_dest_dir() for the exact rule). */
void job_output_dest_dir(const job_t *job, char *out, size_t out_size);

/* Resolves job->nfo_path (job.h) if it isn't already resolved: no-op unless
 * job->state is JOB_COMPLETED and this hasn't run for the job yet (see
 * job->nfo_checked). finalize_job() already calls this once extraction
 * lands, so it's normally a no-op by the time the API needs it -- this
 * exists mainly to lazily backfill it for jobs completed by a build before
 * this field existed. Safe to call under queue_lock (no I/O beyond the
 * bounded directory scan itself). */
void job_ensure_nfo_scanned(job_t *job);

/* Resolves job->pkg_paths (job.h) if it isn't already resolved: no-op
 * unless job->state is JOB_COMPLETED and this hasn't run for the job yet
 * (see job->pkg_checked) -- same lifecycle as job_ensure_nfo_scanned()
 * above, including the "already have results, and/or already looked"
 * skip check. finalize_job() already calls this once extraction lands, so
 * it's normally a no-op by the time the API needs it. Safe to call under
 * queue_lock (no I/O beyond the bounded directory scan itself). */
void job_ensure_pkg_scanned(job_t *job);
