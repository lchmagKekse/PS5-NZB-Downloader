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
