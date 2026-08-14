#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../log/log.h"
#include "../storage/paths.h"
#include "queue.h"

struct queue {
  char       dir[512];
  job_t    **jobs;
  size_t     count;
  size_t     capacity;
};

static int
ensure_capacity(queue_t *q) {
  if (q->count < q->capacity) return 0;

  size_t new_cap = q->capacity ? q->capacity * 2 : 16;
  job_t **grown = realloc(q->jobs, new_cap * sizeof *grown);
  if (!grown) return -1;

  q->jobs = grown;
  q->capacity = new_cap;
  return 0;
}

static void
job_path(const queue_t *q, const char *id, char *buf, size_t size) {
  snprintf(buf, size, "%s/%s.json", q->dir, id);
}

static int
has_suffix(const char *s, const char *suffix) {
  size_t sl = strlen(s), fl = strlen(suffix);
  return sl >= fl && !strcmp(s + sl - fl, suffix);
}

static int
job_priority_cmp(const void *a, const void *b) {
  const job_t *ja = *(job_t * const *)a;
  const job_t *jb = *(job_t * const *)b;

  if (ja->priority != jb->priority) return ja->priority - jb->priority;
  return strcmp(ja->id, jb->id);
}

static void
renumber_priorities(queue_t *q) {
  size_t i;
  for (i = 0; i < q->count; i++) q->jobs[i]->priority = (int)i;
}

/* Removes any subdirectory of temp_dir with no matching job id in q (see
 * queue_open()). Best-effort: an unreadable temp_dir is silently skipped. */
static void
sweep_orphaned_temp_dirs(const queue_t *q, const char *temp_dir) {
  DIR *d;
  struct dirent *ent;

  if (!temp_dir || !temp_dir[0]) return;
  if (!(d = opendir(temp_dir))) {
    if (errno != ENOENT) log_warn("queue: opendir(%s): %s", temp_dir, strerror(errno));
    return;
  }

  while ((ent = readdir(d))) {
    char child[900];
    struct stat st;
    size_t i;
    int known = 0;

    if (ent->d_name[0] == '.') continue; /* "." / ".." / e.g. api_jobs.c's ".uploads" scratch dir */

    snprintf(child, sizeof child, "%s/%s", temp_dir, ent->d_name);
    if (stat(child, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

    for (i = 0; i < q->count; i++) {
      if (!strcmp(q->jobs[i]->id, ent->d_name)) { known = 1; break; }
    }
    if (known) continue;

    log_warn("queue: removing orphaned temp dir with no matching job: %s", child);
    rmdir_recursive(child);
  }
  closedir(d);
}

queue_t *
queue_open(const char *dir, const char *temp_dir) {
  queue_t *q = calloc(1, sizeof *q);
  DIR *d;
  struct dirent *ent;

  if (!q) return NULL;
  snprintf(q->dir, sizeof q->dir, "%s", dir);

  if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
    log_error("queue: mkdir(%s): %s", dir, strerror(errno));
    free(q);
    return NULL;
  }

  if (!(d = opendir(dir))) {
    log_error("queue: opendir(%s): %s", dir, strerror(errno));
    free(q);
    return NULL;
  }

  while ((ent = readdir(d))) {
    char full[768];

    if (has_suffix(ent->d_name, ".json.tmp")) {
      snprintf(full, sizeof full, "%s/%s", dir, ent->d_name);
      log_warn("queue: removing leftover incomplete write %s", full);
      remove(full);
      continue;
    }

    if (!has_suffix(ent->d_name, ".json")) continue;

    snprintf(full, sizeof full, "%s/%s", dir, ent->d_name);

    if (ensure_capacity(q) < 0) {
      log_error("queue: out of memory loading %s", full);
      continue;
    }

    job_t *job = job_load(full);
    if (!job) {
      log_error("queue: skipping unreadable job file %s", full);
      continue;
    }

    /* Job was mid-DOWNLOADING/VERIFYING/REPAIRING/EXTRACTING when the process
     * stopped; the downloader only ever picks up JOB_QUEUED, so re-queue it.
     * Already-downloaded segments stay marked done, so this resumes. */
    if (job->state == JOB_DOWNLOADING || job->state == JOB_VERIFYING ||
        job->state == JOB_REPAIRING || job->state == JOB_EXTRACTING) {
      log_warn("[%s] queue: was %s at last shutdown, re-queueing to resume",
               job->id, job_state_name(job->state));
      job_set_state(job, JOB_QUEUED);
    }

    q->jobs[q->count++] = job;
  }
  closedir(d);

  qsort(q->jobs, q->count, sizeof *q->jobs, job_priority_cmp);

  log_info("queue: loaded %zu job(s) from %s", q->count, dir);

  sweep_orphaned_temp_dirs(q, temp_dir);

  return q;
}

void
queue_close(queue_t *q) {
  size_t i;

  if (!q) return;

  for (i = 0; i < q->count; i++) job_free(q->jobs[i]);
  free(q->jobs);
  free(q);
}

size_t
queue_job_count(const queue_t *q) {
  return q->count;
}

size_t
queue_list_jobs(const queue_t *q, job_t **out_jobs, size_t max) {
  size_t n = q->count < max ? q->count : max;
  size_t i;

  for (i = 0; i < n; i++) out_jobs[i] = q->jobs[i];
  return n;
}

job_t *
queue_find_job(queue_t *q, const char *id) {
  size_t i;

  for (i = 0; i < q->count; i++) {
    if (!strcmp(q->jobs[i]->id, id)) return q->jobs[i];
  }
  return NULL;
}

job_t *
queue_find_next_queued(queue_t *q) {
  size_t i;

  for (i = 0; i < q->count; i++) {
    if (q->jobs[i]->state == JOB_QUEUED) return q->jobs[i];
  }
  return NULL;
}

int
queue_save_job(queue_t *q, const job_t *job) {
  char path[600];

  job_path(q, job->id, path, sizeof path);
  return job_save(job, path);
}

int
queue_add_job(queue_t *q, job_t *job) {
  char path[600];

  job->priority = q->count ? q->jobs[q->count - 1]->priority + 1 : 0;

  job_path(q, job->id, path, sizeof path);
  if (job_save(job, path) < 0) {
    job_free(job);
    return -1;
  }

  if (ensure_capacity(q) < 0) {
    log_error("[%s] queue: out of memory adding job", job->id);
    job_free(job);
    return -1;
  }

  q->jobs[q->count++] = job;
  return 0;
}

int
queue_remove_job(queue_t *q, const char *id) {
  size_t i;

  for (i = 0; i < q->count; i++) {
    if (strcmp(q->jobs[i]->id, id)) continue;

    /* A job in one of these states, or with busy set, may be a raw pointer
     * download.c's worker/finalizer threads are still dereferencing, so
     * freeing it now would be a use-after-free. busy covers the gap state
     * alone can't: Cancel flips state to JOB_CANCELLED immediately, but a
     * worker thread blocked inside PAR2 verify/repair or archive extraction
     * only notices at its next progress-callback tick and needs a moment to
     * unwind -- see job.h's busy comment and download.c's
     * job_busy_begin()/job_busy_end(). */
    if (q->jobs[i]->state == JOB_DOWNLOADING || q->jobs[i]->state == JOB_VERIFYING ||
        q->jobs[i]->state == JOB_REPAIRING || q->jobs[i]->state == JOB_EXTRACTING ||
        q->jobs[i]->busy) {
      log_warn("[%s] queue: remove: job is %s%s, refusing to delete while active - try again shortly",
               id, job_state_name(q->jobs[i]->state), q->jobs[i]->busy ? " (still finishing up)" : "");
      return -2;
    }

    char path[600];
    job_path(q, id, path, sizeof path);
    if (remove(path) != 0 && errno != ENOENT) {
      log_warn("[%s] queue: remove(%s): %s", id, path, strerror(errno));
    }

    job_free(q->jobs[i]);
    memmove(&q->jobs[i], &q->jobs[i + 1], (q->count - i - 1) * sizeof *q->jobs);
    q->count--;
    return 0;
  }

  log_warn("[%s] queue: remove: unknown job id", id);
  return -1;
}

int
queue_reorder_job(queue_t *q, const char *id, size_t new_index) {
  size_t i, old_index = q->count;
  job_t *job;

  for (i = 0; i < q->count; i++) {
    if (!strcmp(q->jobs[i]->id, id)) { old_index = i; break; }
  }
  if (old_index == q->count || new_index >= q->count) {
    log_warn("[%s] queue: reorder: unknown job id or index %zu out of range", id, new_index);
    return -1;
  }

  job = q->jobs[old_index];
  memmove(&q->jobs[old_index], &q->jobs[old_index + 1], (q->count - old_index - 1) * sizeof *q->jobs);
  memmove(&q->jobs[new_index + 1], &q->jobs[new_index], (q->count - new_index - 1) * sizeof *q->jobs);
  q->jobs[new_index] = job;

  renumber_priorities(q);
  for (i = 0; i < q->count; i++) queue_save_job(q, q->jobs[i]);

  return 0;
}

int
queue_pause_job(queue_t *q, const char *id) {
  job_t *job = queue_find_job(q, id);

  if (!job) { log_warn("[%s] queue: pause: unknown job id", id); return -1; }
  if (job->state != JOB_QUEUED && job->state != JOB_DOWNLOADING) {
    log_warn("[%s] queue: pause: job is %s, not queued/downloading", id, job_state_name(job->state));
    return -1;
  }

  job_set_state(job, JOB_PAUSED);
  return queue_save_job(q, job);
}

int
queue_resume_job(queue_t *q, const char *id) {
  job_t *job = queue_find_job(q, id);

  if (!job) { log_warn("[%s] queue: resume: unknown job id", id); return -1; }
  if (job->state != JOB_PAUSED) {
    log_warn("[%s] queue: resume: job is %s, not paused", id, job_state_name(job->state));
    return -1;
  }

  job_set_state(job, JOB_QUEUED);
  return queue_save_job(q, job);
}

int
queue_cancel_job(queue_t *q, const char *id) {
  job_t *job = queue_find_job(q, id);

  if (!job) { log_warn("[%s] queue: cancel: unknown job id", id); return -1; }
  if (job->state == JOB_COMPLETED || job->state == JOB_CANCELLED) {
    log_warn("[%s] queue: cancel: job is already %s", id, job_state_name(job->state));
    return -1;
  }

  job_set_state(job, JOB_CANCELLED);
  return queue_save_job(q, job);
}

int
queue_retry_job(queue_t *q, const char *id) {
  job_t *job = queue_find_job(q, id);

  if (!job) { log_warn("[%s] queue: retry: unknown job id", id); return -1; }
  if (job->state != JOB_FAILED && job->state != JOB_CANCELLED) {
    log_warn("[%s] queue: retry: job is %s, not failed/cancelled", id, job_state_name(job->state));
    return -1;
  }

  job->last_error[0] = 0;
  job->retries_used = 0;
  job_set_state(job, JOB_QUEUED);
  return queue_save_job(q, job);
}
