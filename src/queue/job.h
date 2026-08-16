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

#define JOB_ID_LEN 13  /* 12 lowercase hex chars + \0, e.g. "a3f9c1e08b47\0" -- see generate_job_id() */

#define JOB_MAX_PASSWORDS 8  /* some indexers list several guesses, not just one */

#define JOB_MAX_PKGS 16  /* generous cap on .pkg files found under one job's output -- multi-disc/DLC bundles rarely exceed a handful */

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

  /* Set from the add-NZB modal's "Automatically install PKGs" checkbox --
   * once the job's output lands in its final directory, every .pkg file
   * job_ensure_pkg_scanned() (download.c) finds under it is installed via
   * pkg_install_file() (system/pkg_install.h), same as clicking "Install
   * PKGs" manually would do. */
  int          auto_install_pkgs;

  /* Absolute path to a .nfo file found under the job's output directory,
   * or empty if none was found -- filled in by job_ensure_nfo_scanned()
   * (download.c), which finalize_job() calls once a job completes so this
   * is normally already resolved by the time the API needs it. Powers the
   * NFO viewer button (see job_json.c, web/api_jobs.c). Persisted like
   * final_bytes so a restart doesn't lose it. */
  char         nfo_path[900];

  /* True once job_ensure_nfo_scanned() has run for this job, regardless of
   * whether it found anything -- distinguishes "confirmed no .nfo exists"
   * (nfo_path empty, nfo_checked set) from "not looked yet" (both empty/0),
   * e.g. for a job completed by a build before this field existed.
   * Runtime-only, never persisted -- same reasoning as `busy` above: a job
   * just loaded from disk can't have been checked yet by this process. */
  int          nfo_checked;

  /* Absolute paths to .pkg files found under the job's output directory --
   * filled in by job_ensure_pkg_scanned() (download.c), same lifecycle as
   * nfo_path/nfo_checked above. Powers the "Install PKGs" button (see
   * job_json.c, web/api_jobs.c, system/pkg_install.h). Persisted like
   * nfo_path so a restart doesn't lose it. */
  char         pkg_paths[JOB_MAX_PKGS][900];
  size_t       pkg_count;

  /* True once job_ensure_pkg_scanned() has run for this job -- same
   * runtime-only reasoning as nfo_checked above. */
  int          pkg_checked;

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

/* Appends path to job->pkg_paths if there's room (silently drops beyond
 * JOB_MAX_PKGS). No dedup -- job_ensure_pkg_scanned() (download.c) only
 * calls this once per .pkg file found. */
void job_add_pkg_path(job_t *job, const char *path);

/* Sets job->state, logging the transition. */
void job_set_state(job_t *job, job_state_t state);

/* Marks the segment identified by message_id (within any file of the job)
 * as downloaded. Returns 1 if found and marked, 0 if no matching segment
 * exists in the job. */
int job_mark_segment_downloaded(job_t *job, const char *message_id);

/* True if filename matches PAR2 recovery *volume* naming
 * ("<setname>.volNNN+MMM.par2") rather than the small non-volume ".par2"
 * index file -- used by download.c to defer fetching recovery data until
 * it's actually needed, and by job_segment_progress()/job_json.c to keep
 * deferred volumes out of progress totals until they are. */
int job_file_is_par2_volume(const char *filename);

/* Fills *total and *downloaded with segment counts across every file in
 * the job, for progress reporting. */
void job_segment_progress(const job_t *job, size_t *total, size_t *downloaded);

/* Persists job's header (state, priority, error, output settings, etc --
 * everything except its file/segment structure and download progress,
 * which live in their own "<path minus .json>.segments"/".progress"
 * sidecars, written/updated separately -- see job.c's format comment for
 * why) to path, writing to "<path>.tmp" and renaming over path so a crash
 * mid-write can't leave a corrupt/truncated file. fsyncs the tmp file
 * before the rename and the dir after, so an unclean shutdown can't lose
 * the write once this returns 0. O(1) in job size regardless of segment
 * count -- safe to call on every state change no matter how large the
 * job is. Returns 0 on success, -1 on failure (already logged). */
int job_save(const job_t *job, const char *path);

/* Snapshot half of a progress save -- builds job's download-progress
 * bitmap (one bit per segment) as a malloc'd buffer, *out_len bytes long.
 * Pure in-memory, no I/O, so the only reason to call this separately from
 * job_write_progress() is to shrink how long whatever lock protects job's
 * fields (normally queue_lock) has to stay held -- see download.c's
 * per-job checkpoint, which does exactly that instead of using job_save()
 * for every completed segment. Returns NULL (out_len left unset) only on
 * allocation failure. */
unsigned char *job_progress_snapshot(const job_t *job, size_t *out_len);

/* Write half of a progress save -- durably writes bitmap (a
 * job_progress_snapshot() result; this function takes ownership and frees
 * it, len bytes) as job_id's ".progress" sidecar of json_path. Ensures
 * the ".segments" sidecar exists first if this is the job's first save of
 * any kind since it was created. Touches nothing but the given buffer and
 * the filesystem, so unlike job_save() a caller doesn't need to keep
 * holding whatever lock protected job while job_progress_snapshot() ran.
 * Returns 0/-1 (already logged). */
int job_write_progress(const job_t *job, const char *json_path, unsigned char *bitmap, size_t len);

/* Parses a job previously written by job_save() and its ".segments"/
 * ".progress" sidecars. Returns a newly allocated job_t (caller must
 * job_free() it), or NULL on failure (already logged). */
job_t *job_load(const char *path);

/* Derives one of job's sidecar paths (suffix ".segments" or ".progress")
 * from its canonical "<dir>/<id>.json" path -- e.g. for queue.c's
 * queue_remove_job() to clean those up alongside the header. Neither
 * suffix starts with ".json" so queue.c's "*.json" directory scan never
 * mistakes one for a job header (see job.c's format comment). */
void job_sidecar_path(const char *json_path, const char *suffix, char *out, size_t out_size);
