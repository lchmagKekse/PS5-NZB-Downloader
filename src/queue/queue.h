/* Persistent job queue: an in-memory ordered list of job_t, mirrored to
 * one JSON file per job under a directory (see job_save()/job_load()).
 * queue_open() replays that directory on startup so an interrupted job
 * resumes via job_t's per-segment `downloaded` flags.
 *
 * Not thread-safe by itself; callers driving this from multiple threads
 * must serialize access with their own lock.
 */
#pragma once

#include <stddef.h>

#include "job.h"

typedef struct queue queue_t;

/* Opens (creating if missing) the on-disk queue store at dir and loads
 * every job found there. temp_dir is storage.temp_dir (see config.h); any
 * of its subdirectories with no matching job id is swept as an orphan.
 * Pass NULL or "" to skip the sweep. Returns NULL on failure (logged). */
queue_t *queue_open(const char *dir, const char *temp_dir);

/* Frees in-memory state only; every job already on disk stays there. */
void queue_close(queue_t *q);

size_t queue_job_count(const queue_t *q);

/* Copies up to max job pointers (in queue order) into out_jobs. Returns
 * the number actually copied. Pointers are owned by the queue. */
size_t queue_list_jobs(const queue_t *q, job_t **out_jobs, size_t max);

/* Returns the job with the given id, or NULL. */
job_t *queue_find_job(queue_t *q, const char *id);

/* Returns the first job in queue order whose state is JOB_QUEUED, or NULL
 * if none -- this is the download orchestrator's only scheduling policy
 * (queue order), kept here so it has one place to live. */
job_t *queue_find_next_queued(queue_t *q);

/* Takes ownership of job, appends it to the end of the queue, and
 * persists it to disk. Returns 0 on success, -1 on failure (job is freed
 * either way -- caller must not use it afterward). */
int queue_add_job(queue_t *q, job_t *job);

/* Re-persists a job's header (state, priority, error, output settings,
 * etc -- see job.c's format comment for why that excludes its
 * file/segment structure and download progress) to disk. Call after
 * mutating a job returned by queue_find_job()/queue_add_job() (e.g.
 * job_set_state()) so it survives a restart. O(1) in job size regardless
 * of segment count. Returns 0 on success, -1 on failure. */
int queue_save_job(queue_t *q, const job_t *job);

/* Split save for a job's download-progress bitmap specifically (see
 * job.c's format comment), for a caller -- download.c's per-job
 * checkpoint, the reason this exists -- in a position to release
 * queue_lock between the two halves: call queue_job_progress_snapshot()
 * while still holding it (fast, in-memory), release it, then
 * queue_write_job_progress() with the result, which does the actual disk
 * write and needs no lock at all. Call this instead of queue_save_job()
 * on every segment completion; queue_save_job() only needs calling on
 * actual state changes. queue_job_progress_snapshot() returns NULL on
 * failure (already logged); pass anything else straight to
 * queue_write_job_progress(), which always frees bitmap and returns 0/-1
 * (already logged). */
unsigned char *queue_job_progress_snapshot(const job_t *job, size_t *out_len);
int queue_write_job_progress(queue_t *q, const job_t *job, unsigned char *bitmap, size_t len);

/* State-changing conveniences. Each validates the job is in a state the
 * operation makes sense from, updates it, persists it, and returns 0 on
 * success. Returns -1 if the job id is unknown or the job isn't in a
 * state the operation applies to (already logged). */
int queue_pause_job(queue_t *q, const char *id);   /* QUEUED/DOWNLOADING -> PAUSED */
int queue_resume_job(queue_t *q, const char *id);  /* PAUSED -> QUEUED */
int queue_cancel_job(queue_t *q, const char *id);  /* any non-terminal state -> CANCELLED */
int queue_retry_job(queue_t *q, const char *id);   /* FAILED/CANCELLED -> QUEUED, clears error */

/* Removes the job from the queue (in memory and on disk) and frees it.
 * Refuses (-2, job untouched) while JOB_DOWNLOADING/VERIFYING/REPAIRING/
 * EXTRACTING -- download.c's threads hold a raw job_t* unlocked then, so
 * freeing it would be a use-after-free; cancel it first. -1 if unknown id. */
int queue_remove_job(queue_t *q, const char *id);

/* Moves the job to position new_index (0-based) in queue order. Returns
 * 0 on success, -1 if the job id is unknown or new_index is out of
 * range. */
int queue_reorder_job(queue_t *q, const char *id, size_t new_index);
