/* Job data model: a download job is a list of files, each a list of NNTP
 * segments. Populated by the NZB parser (src/nzb) and persisted/scheduled
 * by the queue manager (src/queue/queue.c).
 *
 * Segment/file arrays grow on demand (job_add_file/job_file_add_segment)
 * so a job can be built incrementally while streaming a large NZB.
 */
#pragma once

#include <stddef.h>

typedef struct {
  char message_id[512];  /* NNTP message-id, angle brackets included, e.g. "<part1of50.abc@news>" */
  long bytes;             /* size in bytes as declared by the NZB, for progress reporting */
  int  number;             /* segment number within the file, as declared by the NZB (1-based) */
  int  downloaded;         /* set once this segment's article has been fetched and assembled */
} job_segment_t;

typedef struct {
  char            filename[512];  /* display filename, extracted from the NZB subject where possible */
  char            subject[1024];  /* raw <file subject="..."> text, kept for reference/UI */
  long            bytes;           /* sum of segment bytes, filled in as segments are added */
  job_segment_t  *segments;
  size_t          segment_count;
  size_t          segment_capacity;
} job_file_t;

typedef enum {
  JOB_QUEUED,
  JOB_DOWNLOADING,
  JOB_PAUSED,
  JOB_VERIFYING,
  JOB_REPAIRING,
  JOB_EXTRACTING,
  JOB_COMPLETED,
  JOB_FAILED,
  JOB_CANCELLED
} job_state_t;

#define JOB_ID_LEN 37  /* UUID string, e.g. "550e8400-e29b-41d4-a716-446655440000\0" */

#define JOB_MAX_PASSWORDS 8  /* some indexers list several guesses, not just one */

typedef struct {
  char         id[JOB_ID_LEN];
  char         name[256];       /* display name, typically the NZB filename */
  job_state_t  state;

  /* True while download.c's downloader/finalizer thread is inside a long
   * blocking call (PAR2 verify/repair, archive extraction) that still
   * dereferences this job_t after an API handler may already have flipped
   * state to JOB_CANCELLED out from under it -- state alone can't tell
   * queue_remove_job() that, since Cancel sets state immediately, before
   * the worker thread has actually noticed and unwound (see download.c's
   * job_busy_begin()/job_busy_end()). Runtime-only, never persisted --
   * job_save() doesn't write it and job_load() leaves it at its calloc'd
   * 0, which is correct since nothing can be mid-operation on a job just
   * loaded from disk. */
  int          busy;

  int          priority;         /* queue ordering; higher runs first, ties broken by add order */
  int          retries_used;
  char         last_error[256];  /* empty when there is no error */

  /* Real on-disk size of the job's final output dir, filled in by
   * finalize_job() (download.c) just before JOB_COMPLETED -- 0 until then.
   * Once set, the API reports this instead of the segment-derived total
   * (see job_json.c), since extraction (esp. nested archives) can leave the
   * output size very different from what was downloaded. */
  long long    final_bytes;

  /* Candidate archive passwords from the NZB's <head><meta type="password">
   * entries (see nzb_parse.c), tried in order by extract.c. */
  char         passwords[JOB_MAX_PASSWORDS][256];
  size_t       password_count;

  /* Per-job override of storage.output_dir (see config.h); when non-empty,
   * finalize_job() (download.c) uses this as the parent output dir instead
   * of the globally configured one. Set from the "Output folder" field in
   * the add-NZB modal. Empty means use the config setting at finalize time. */
  char         output_dir[512];

  /* Set from the add-NZB modal's "Add to ShadowMount list" checkbox --
   * once the job's output lands in its final directory, it's searched for
   * a PS5 title layout and registered (see ../storage/shadowmount.h). */
  int          add_to_shadowmount;

  job_file_t  *files;
  size_t       file_count;
  size_t       file_capacity;
} job_t;

const char *job_state_name(job_state_t state);

/* Allocates a new job with a freshly generated id and JOB_QUEUED state.
 * name is copied (truncated to fit). Returns NULL on allocation failure. */
job_t *job_create(const char *name);

void job_free(job_t *job);

/* Appends a file to the job, growing the files array as needed. Returns the
 * new job_file_t* (owned by job, invalidated by the next job_add_file call
 * -- re-fetch via &job->files[index] if you need a stable pointer), or NULL
 * on allocation failure. */
job_file_t *job_add_file(job_t *job, const char *filename, const char *subject);

/* Appends a segment to a file, growing its segments array as needed.
 * Updates file->bytes. Returns 0 on success, -1 on allocation failure. */
int job_file_add_segment(job_file_t *file, const char *message_id, long bytes, int number);

/* Appends password to job->passwords if not already present and there's
 * room (silently drops beyond JOB_MAX_PASSWORDS -- that many candidate
 * guesses from one NZB would be unusual). No-op if password is empty. */
void job_add_password(job_t *job, const char *password);

/* Sets job->state, logging the transition. */
void job_set_state(job_t *job, job_state_t state);

/* Marks the segment identified by message_id (within any file of the job)
 * as downloaded. Returns 1 if found and marked, 0 if no matching segment
 * exists in the job. */
int job_mark_segment_downloaded(job_t *job, const char *message_id);

/* Fills *total and *downloaded with segment counts across every file in
 * the job, for progress reporting. */
void job_segment_progress(const job_t *job, size_t *total, size_t *downloaded);

/* Serializes job as JSON to path, writing to "<path>.tmp" and renaming over
 * path so a crash mid-write can't leave a corrupt/truncated job file.
 * fsyncs the tmp file before the rename and the dir after, so an unclean
 * shutdown can't lose the write once this returns 0. Returns 0 on success,
 * -1 on failure (already logged). */
int job_save(const job_t *job, const char *path);

/* Parses a job previously written by job_save(). Returns a newly
 * allocated job_t (caller must job_free() it), or NULL on failure
 * (already logged). */
job_t *job_load(const char *path);
