#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "../log/log.h"
#include "../storage/paths.h"
#include "../util/notify.h"
#include "../vendor/cjson/cJSON.h"
#include "job.h"

const char *
job_state_name(job_state_t state) {
  switch (state) {
    case JOB_QUEUED:      return "queued";
    case JOB_DOWNLOADING: return "downloading";
    case JOB_PAUSED:      return "paused";
    case JOB_VERIFYING:   return "verifying";
    case JOB_REPAIRING:   return "repairing";
    case JOB_EXTRACTING:  return "extracting";
    case JOB_COMPLETED:   return "completed";
    case JOB_FAILED:      return "failed";
    case JOB_CANCELLED:   return "cancelled";
  }
  return "unknown";
}

/* 6 random bytes as 12 lowercase hex chars -- not a real UUID (no
 * version/variant bits to set), just a short, filesystem-safe token used
 * as the job's id everywhere: <id>.json/.segments/.progress filenames,
 * API URLs, log lines. 48 bits of entropy is effectively collision-free
 * at the scale a personal download queue ever reaches (a UUID's 122 bits
 * was always far more than this needed). */
static void
generate_job_id(char out[JOB_ID_LEN]) {
  unsigned char b[6];

  arc4random_buf(b, sizeof b);

  snprintf(out, JOB_ID_LEN, "%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5]);
}

job_t *
job_create(const char *name) {
  job_t *job = calloc(1, sizeof *job);
  if (!job) return NULL;

  generate_job_id(job->id);
  snprintf(job->name, sizeof job->name, "%s", name);
  path_sanitize_component(job->name, sizeof job->name); /* becomes a path component in assemble.c */
  job->state = JOB_QUEUED;

  return job;
}

void
job_free(job_t *job) {
  size_t i;

  if (!job) return;

  for (i = 0; i < job->file_count; i++) {
    free(job->files[i].segments);
  }
  free(job->files);
  free(job);
}

job_file_t *
job_add_file(job_t *job, const char *filename, const char *subject) {
  job_file_t *f;

  if (job->file_count == job->file_capacity) {
    size_t new_cap = job->file_capacity ? job->file_capacity * 2 : 8;
    job_file_t *grown = realloc(job->files, new_cap * sizeof *grown);
    if (!grown) return NULL;
    job->files = grown;
    job->file_capacity = new_cap;
  }

  f = &job->files[job->file_count++];
  memset(f, 0, sizeof *f);
  snprintf(f->filename, sizeof f->filename, "%s", filename);
  snprintf(f->subject, sizeof f->subject, "%s", subject);

  return f;
}

int
job_file_add_segment(job_file_t *file, const char *message_id, long bytes, int number) {
  job_segment_t *s;

  if (file->segment_count == file->segment_capacity) {
    size_t new_cap = file->segment_capacity ? file->segment_capacity * 2 : 64;
    job_segment_t *grown = realloc(file->segments, new_cap * sizeof *grown);
    if (!grown) return -1;
    file->segments = grown;
    file->segment_capacity = new_cap;
  }

  s = &file->segments[file->segment_count++];
  memset(s, 0, sizeof *s);
  snprintf(s->message_id, sizeof s->message_id, "%s", message_id);
  s->bytes = bytes;
  s->number = number;

  file->bytes += bytes;

  return 0;
}

void
job_add_password(job_t *job, const char *password) {
  size_t i;

  if (!password || !password[0]) return;

  for (i = 0; i < job->password_count; i++) {
    if (!strcmp(job->passwords[i], password)) return; /* already have it */
  }

  if (job->password_count >= JOB_MAX_PASSWORDS) return;

  snprintf(job->passwords[job->password_count], sizeof job->passwords[0], "%s", password);
  job->password_count++;
}

void
job_add_pkg_path(job_t *job, const char *path) {
  if (job->pkg_count >= JOB_MAX_PKGS) return;

  snprintf(job->pkg_paths[job->pkg_count], sizeof job->pkg_paths[0], "%s", path);
  job->pkg_count++;
}

void
job_set_state(job_t *job, job_state_t state) {
  /* Same state as already set -- skip the log line and notify() below
   * instead of recording (and pushing) a no-op "X -> X" transition. Every
   * call site is expected to already know it's changing state, but this
   * guards against a future one that doesn't bother checking first. */
  if (job->state == state) return;

  log_info("[%s] state %s -> %s", job->id, job_state_name(job->state), job_state_name(state));
  job->state = state;

  switch (state) {
    case JOB_QUEUED:      notify("Queued: %s", job->name);      break;
    case JOB_DOWNLOADING: notify("Downloading: %s", job->name); break;
    case JOB_PAUSED:      notify("Paused: %s", job->name);      break;
    case JOB_VERIFYING:   notify("Verifying: %s", job->name);   break;
    case JOB_REPAIRING:   notify("Repairing: %s", job->name);   break;
    case JOB_EXTRACTING:  notify("Extracting: %s", job->name);  break;
    case JOB_COMPLETED:   notify("Finished: %s", job->name);    break;
    case JOB_FAILED:      notify("Failed: %s", job->name);      break;
    case JOB_CANCELLED:   notify("Cancelled: %s", job->name);   break;
    default: break;
  }
}

int
job_mark_segment_downloaded(job_t *job, const char *message_id) {
  size_t fi, si;

  for (fi = 0; fi < job->file_count; fi++) {
    job_file_t *f = &job->files[fi];
    for (si = 0; si < f->segment_count; si++) {
      if (!strcmp(f->segments[si].message_id, message_id)) {
        f->segments[si].downloaded = 1;
        return 1;
      }
    }
  }

  return 0;
}

int
job_file_is_par2_volume(const char *name) {
  size_t len = strlen(name);
  const char *p, *digits_end;

  if (len < 5 || strcasecmp(name + len - 5, ".par2") != 0) return 0;
  len -= 5; /* everything up to ".par2" */

  /* Walk back over "MMM" (block count), '+', then "NNN" (start index) --
   * scanning from the known ".par2" suffix rather than forward-searching
   * for ".vol" so a filename that happens to contain "vol" earlier (or in
   * different case) can't produce a false match. */
  p = digits_end = name + len;
  while (p > name && p[-1] >= '0' && p[-1] <= '9') p--;
  if (p == digits_end || p == name || p[-1] != '+') return 0;
  p--;

  digits_end = p;
  while (p > name && p[-1] >= '0' && p[-1] <= '9') p--;
  if (p == digits_end) return 0;

  return (size_t)(p - name) >= 4 && !strncasecmp(p - 4, ".vol", 4);
}

void
job_segment_progress(const job_t *job, size_t *total, size_t *downloaded) {
  size_t fi, si, t = 0, d = 0;

  for (fi = 0; fi < job->file_count; fi++) {
    const job_file_t *f = &job->files[fi];

    /* A recovery volume download.c hasn't fetched yet (see its two-phase
     * download: volumes are skipped unless PAR2 verify actually finds
     * damage) shouldn't count against progress -- otherwise the job looks
     * permanently stuck below 100% for bytes it was never going to need.
     * Once any segment of a volume has been fetched (repair was needed),
     * count that file normally so the recovery-fetch itself is visible. */
    if (job_file_is_par2_volume(f->filename)) {
      int touched = 0;

      for (si = 0; si < f->segment_count; si++) {
        if (f->segments[si].downloaded) { touched = 1; break; }
      }
      if (!touched) continue;
    }

    for (si = 0; si < f->segment_count; si++) {
      t++;
      if (f->segments[si].downloaded) d++;
    }
  }

  *total = t;
  *downloaded = d;
}

static job_state_t
job_state_from_name(const char *s) {
  if (!strcmp(s, "queued"))      return JOB_QUEUED;
  if (!strcmp(s, "downloading")) return JOB_DOWNLOADING;
  if (!strcmp(s, "paused"))      return JOB_PAUSED;
  if (!strcmp(s, "verifying"))   return JOB_VERIFYING;
  if (!strcmp(s, "repairing"))   return JOB_REPAIRING;
  if (!strcmp(s, "extracting"))  return JOB_EXTRACTING;
  if (!strcmp(s, "completed"))   return JOB_COMPLETED;
  if (!strcmp(s, "failed"))      return JOB_FAILED;
  if (!strcmp(s, "cancelled"))   return JOB_CANCELLED;
  return JOB_QUEUED;
}

/* A job is persisted as up to three sibling files, deliberately not all
 * named "*.json" -- queue.c's startup scan globs "*.json" for job headers,
 * and a sidecar matching that glob would get mistaken for one:
 *   "<id>.json"     -- the header: state, priority, error, output
 *     settings, etc. Nothing here scales with segment count.
 *   "<id>.segments" -- the file/segment structure: filenames, subjects,
 *     message-ids, sizes -- everything job_add_file()/job_file_add_segment()
 *     fill in from the NZB. Fixed at parse time and never mutated again by
 *     any code path afterward (see job_ensure_segments_saved()'s comment),
 *     so unlike the header this is written once and never rewritten.
 *   "<id>.progress" -- a raw bitmap, one bit per segment in the same
 *     file/segment order as ".segments", set once that segment's article
 *     is fetched (job_mark_segment_downloaded()). The only thing that
 *     changes on every segment completion, so it's the only thing
 *     download.c's per-job checkpoint has to rewrite there -- at most a
 *     couple hundred KB even for a job with a million segments, instead of
 *     the 100+ MB a full "rewrite everything" checkpoint meant for one
 *     that size (message-ids alone dominate that cost, and never change).
 * Splitting the always-changing bit (progress) from the write-once bit
 * (segments) from the small-and-occasionally-changing bit (header) is
 * this project's answer to the same problem tools like SABnzbd solve by
 * not re-persisting a job's full per-article state on every single
 * article -- the article list itself is derived once from the NZB and
 * doesn't need saving again; only "which of these did we get" changes,
 * and that's cheap to represent compactly. */
void
job_sidecar_path(const char *json_path, const char *suffix, char *out, size_t out_size) {
  size_t len = strlen(json_path);
  static const char ext[] = ".json";
  size_t ext_len = sizeof ext - 1;

  if (len >= ext_len && !strcmp(json_path + len - ext_len, ext)) {
    snprintf(out, out_size, "%.*s%s", (int)(len - ext_len), json_path, suffix);
  } else {
    snprintf(out, out_size, "%s%s", json_path, suffix);
  }
}

/* Serializes disk writes across every job_write_file() call (any job, any
 * of its sidecars) so two concurrent saves can never interleave writes to
 * -- or race the fopen("w")/rename() of -- the same path. The only
 * realistic case is a checkpoint's unlocked progress write (see
 * job_write_progress()) landing at the same moment as an API-triggered
 * header save (pause/cancel/etc, still made under queue_lock) for that
 * same job; different jobs never share a path so never actually contend,
 * but serializing everyone is simpler than a per-job lock and disk writes
 * are effectively serialized by the hardware anyway. Distinct from
 * queue_lock -- never held while trying to acquire it -- so it can't
 * deadlock against anything that does the reverse. */
static pthread_mutex_t g_save_mu = PTHREAD_MUTEX_INITIALIZER;

/* Durably writes len bytes from buf to path via a temp-file+fsync+
 * rename+dir-fsync sequence. Always frees buf, whether it succeeds or
 * fails. Shared by every kind of job file (header/segments/progress) --
 * none of them touch job_t itself, just the given buffer and the
 * filesystem, which is what lets a caller (see job_write_progress())
 * release whatever lock protects the job this was snapshotted from before
 * calling this. Returns 0/-1 (already logged). */
static int
job_write_file(const char *job_id, const char *path, void *buf, size_t len) {
  char tmp_path[700];
  FILE *f;
  int ok = 1;

  pthread_mutex_lock(&g_save_mu);

  snprintf(tmp_path, sizeof tmp_path, "%s.tmp", path);
  if (!(f = fopen(tmp_path, "wb"))) {
    log_error("[%s] job: fopen(%s): %s", job_id, tmp_path, strerror(errno));
    free(buf);
    pthread_mutex_unlock(&g_save_mu);
    return -1;
  }
  if (len > 0 && fwrite(buf, 1, len, f) != len) ok = 0;
  /* fsync before close: fclose() alone only reaches the page cache, and an
   * unclean shutdown could let the rename() below land durably while these
   * bytes are lost, leaving a correctly-named but truncated file. */
  if (ok && fflush(f) != 0) ok = 0;
  if (ok && fsync(fileno(f)) != 0) ok = 0;
  if (fclose(f) != 0) ok = 0;
  free(buf);

  if (!ok) {
    log_error("[%s] job: write error saving %s", job_id, path);
    pthread_mutex_unlock(&g_save_mu);
    return -1;
  }

  if (rename(tmp_path, path) != 0) {
    log_error("[%s] job: rename(%s, %s): %s", job_id, tmp_path, path, strerror(errno));
    pthread_mutex_unlock(&g_save_mu);
    return -1;
  }

  /* rename() is itself a directory-metadata change; fsync the dir too so a
   * crash right after can't lose it. Best-effort -- the file data is
   * already safe. */
  {
    char dir_path[700];
    char *slash;
    int dirfd;

    snprintf(dir_path, sizeof dir_path, "%s", path);
    if ((slash = strrchr(dir_path, '/'))) {
      *slash = 0;
      if ((dirfd = open(dir_path, O_RDONLY)) >= 0) {
        if (fsync(dirfd) != 0) {
          log_warn("[%s] job: fsync(%s): %s", job_id, dir_path, strerror(errno));
        }
        close(dirfd);
      }
    }
  }

  pthread_mutex_unlock(&g_save_mu);
  return 0;
}

/* Builds job's file/segment structure ("files": [...], each with
 * filename/subject/bytes and its segments' message_id/bytes/number, but
 * deliberately no "downloaded" -- that lives in the ".progress" bitmap
 * instead) as a malloc'd JSON string. Returns NULL on failure (already
 * logged). */
static char *
job_segments_to_json_text(const job_t *job) {
  cJSON *root = cJSON_CreateObject();
  cJSON *files = cJSON_CreateArray();
  char *text;
  size_t fi, si;

  cJSON_AddItemToObject(root, "files", files);

  for (fi = 0; fi < job->file_count; fi++) {
    const job_file_t *jf = &job->files[fi];
    cJSON *file = cJSON_CreateObject();
    cJSON *segments = cJSON_CreateArray();

    cJSON_AddStringToObject(file, "filename", jf->filename);
    cJSON_AddStringToObject(file, "subject", jf->subject);
    cJSON_AddNumberToObject(file, "bytes", (double)jf->bytes);
    cJSON_AddItemToObject(file, "segments", segments);

    for (si = 0; si < jf->segment_count; si++) {
      const job_segment_t *s = &jf->segments[si];
      cJSON *seg = cJSON_CreateObject();

      cJSON_AddStringToObject(seg, "message_id", s->message_id);
      cJSON_AddNumberToObject(seg, "bytes", (double)s->bytes);
      cJSON_AddNumberToObject(seg, "number", s->number);

      cJSON_AddItemToArray(segments, seg);
    }

    cJSON_AddItemToArray(files, file);
  }

  text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  if (!text) log_error("[%s] job: failed to serialize segment structure", job->id);
  return text;
}

/* Writes job's ".segments" sidecar if (and only if) it doesn't already
 * exist -- a brand new job's first-ever save (see job_save()/
 * job_write_progress()) always hits this, once; every save after that
 * finds it already there and skips straight past.
 *
 * Reads job->files -- filename/subject/message_id/bytes/number, fixed at
 * NZB-parse time (job_add_file()/job_file_add_segment(), both called only
 * from nzb_parse.c, before the job is ever queued) and never mutated
 * again by any code path -- so, like job->id elsewhere in this codebase,
 * it's safe to read here without queue_lock even from job_write_progress()'s
 * unlocked caller. Logs and returns on failure without treating it as
 * fatal to the caller's own save: a missing sidecar just means this runs
 * again next time. */
static void
job_ensure_segments_saved(const job_t *job, const char *json_path) {
  char segments_path[700];
  char *text;

  job_sidecar_path(json_path, ".segments", segments_path, sizeof segments_path);
  if (access(segments_path, F_OK) == 0) return;

  if (!(text = job_segments_to_json_text(job))) return;
  job_write_file(job->id, segments_path, text, strlen(text));
}

/* Builds job's ".progress" bitmap as a malloc'd buffer -- pure in-memory,
 * no I/O, so the point of calling this separately from
 * job_write_progress() is shrinking how long whatever lock protects job's
 * fields (normally queue_lock) has to stay held. *out_len is the buffer's
 * length in bytes (ceil(total segments / 8)). Returns NULL (leaving
 * *out_len unset) only on allocation failure. */
unsigned char *
job_progress_snapshot(const job_t *job, size_t *out_len) {
  size_t total = 0, fi, si, bit;
  unsigned char *bitmap;

  for (fi = 0; fi < job->file_count; fi++) total += job->files[fi].segment_count;

  *out_len = (total + 7) / 8;
  if (!(bitmap = calloc(*out_len ? *out_len : 1, 1))) return NULL;

  bit = 0;
  for (fi = 0; fi < job->file_count; fi++) {
    const job_file_t *jf = &job->files[fi];

    for (si = 0; si < jf->segment_count; si++, bit++) {
      if (jf->segments[si].downloaded) bitmap[bit / 8] |= (unsigned char)(1u << (bit % 8));
    }
  }

  return bitmap;
}

/* Durably writes bitmap (a job_progress_snapshot() result -- this
 * function takes ownership and frees it) as job_id's ".progress" sidecar
 * next to json_path. Ensures the ".segments" sidecar exists first (see
 * job_ensure_segments_saved()) since a progress bitmap is meaningless
 * without the structure it lines up against. Unlike job_save(), touches
 * nothing but the given buffer and the filesystem, so a caller doesn't
 * need to keep holding whatever lock protects the job bitmap was
 * snapshotted from -- see download.c's per-job checkpoint, the reason
 * this split exists: even a bitmap this small is still real disk I/O, and
 * every web API request needing queue_lock would otherwise queue up
 * behind it. Returns 0/-1 (already logged). */
int
job_write_progress(const job_t *job, const char *json_path, unsigned char *bitmap, size_t len) {
  char progress_path[700];

  job_ensure_segments_saved(job, json_path);
  job_sidecar_path(json_path, ".progress", progress_path, sizeof progress_path);
  return job_write_file(job->id, progress_path, bitmap, len);
}

/* Builds job's header -- everything about it except its file/segment
 * structure and download progress, both of which live in their own
 * sidecars -- as a malloc'd JSON string. Small and O(1) in job size
 * regardless of segment count: every state change (pause, cancel,
 * download/verify/repair/extract/complete transitions, ...) rewrites
 * this, and none of them should have to pay for a huge job's segment list
 * to do it. Returns NULL on failure (already logged). */
static char *
job_header_to_json_text(const job_t *job) {
  cJSON *root = cJSON_CreateObject();
  char *text;
  size_t i;

  cJSON_AddStringToObject(root, "id", job->id);
  cJSON_AddStringToObject(root, "name", job->name);
  cJSON_AddStringToObject(root, "state", job_state_name(job->state));
  cJSON_AddNumberToObject(root, "priority", job->priority);
  cJSON_AddNumberToObject(root, "retries_used", job->retries_used);
  cJSON_AddStringToObject(root, "last_error", job->last_error);
  cJSON_AddNumberToObject(root, "final_bytes", (double)job->final_bytes);
  cJSON_AddStringToObject(root, "output_dir", job->output_dir);
  cJSON_AddBoolToObject(root, "add_to_shadowmount", job->add_to_shadowmount);
  cJSON_AddBoolToObject(root, "auto_install_pkgs", job->auto_install_pkgs);
  cJSON_AddStringToObject(root, "nfo_path", job->nfo_path);

  {
    cJSON *passwords = cJSON_CreateArray();
    for (i = 0; i < job->password_count; i++) {
      cJSON_AddItemToArray(passwords, cJSON_CreateString(job->passwords[i]));
    }
    cJSON_AddItemToObject(root, "passwords", passwords);
  }

  {
    cJSON *pkg_paths = cJSON_CreateArray();
    for (i = 0; i < job->pkg_count; i++) {
      cJSON_AddItemToArray(pkg_paths, cJSON_CreateString(job->pkg_paths[i]));
    }
    cJSON_AddItemToObject(root, "pkg_paths", pkg_paths);
  }

  text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  if (!text) log_error("[%s] job: failed to serialize", job->id);
  return text;
}

int
job_save(const job_t *job, const char *path) {
  char *text;

  job_ensure_segments_saved(job, path);

  if (!(text = job_header_to_json_text(job))) return -1;
  return job_write_file(job->id, path, text, strlen(text));
}

/* Loads job's ".segments" and ".progress" sidecars (see job_sidecar_path()'s
 * comment) into job, whose header job_load() already populated from
 * json_path. A missing or corrupt segments sidecar leaves job with no
 * files -- logged, but not fatal to loading the job itself. A missing or
 * short progress sidecar just leaves the affected segments unmarked
 * (re-fetched next run) rather than losing the job. */
static void
job_load_segments_and_progress(job_t *job, const char *json_path) {
  char segments_path[700], progress_path[700];
  FILE *f;
  char *buf;
  long size;
  cJSON *root, *files, *file_item;
  unsigned char *bitmap = NULL;
  size_t bitmap_len = 0, bit = 0;

  job_sidecar_path(json_path, ".segments", segments_path, sizeof segments_path);
  job_sidecar_path(json_path, ".progress", progress_path, sizeof progress_path);

  if (!(f = fopen(segments_path, "rb"))) {
    log_error("[%s] job: fopen(%s): %s", job->id, segments_path, strerror(errno));
    return;
  }
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size < 0 || !(buf = malloc((size_t)size + 1))) {
    fclose(f);
    return;
  }
  if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
    log_error("[%s] job: short read on %s", job->id, segments_path);
    free(buf);
    fclose(f);
    return;
  }
  buf[size] = 0;
  fclose(f);

  root = cJSON_Parse(buf);
  free(buf);
  if (!root) {
    log_error("[%s] job: invalid JSON in %s", job->id, segments_path);
    return;
  }

  /* Read whole into memory up front -- typically at most a couple hundred
   * KB even for a huge job -- rather than seeking per segment below. */
  if ((f = fopen(progress_path, "rb"))) {
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0 && (bitmap = malloc((size_t)size))) {
      if (fread(bitmap, 1, (size_t)size, f) == (size_t)size) {
        bitmap_len = (size_t)size;
      } else {
        free(bitmap);
        bitmap = NULL;
      }
    }
    fclose(f);
  }
  if (!bitmap) {
    log_warn("[%s] job: no usable %s -- treating all segments as not yet downloaded",
             job->id, progress_path);
  }

  files = cJSON_GetObjectItemCaseSensitive(root, "files");
  cJSON_ArrayForEach(file_item, files) {
    const cJSON *filename = cJSON_GetObjectItemCaseSensitive(file_item, "filename");
    const cJSON *subject  = cJSON_GetObjectItemCaseSensitive(file_item, "subject");
    const cJSON *segments = cJSON_GetObjectItemCaseSensitive(file_item, "segments");
    const cJSON *seg_item;
    job_file_t *jf;

    jf = job_add_file(job,
                       cJSON_IsString(filename) ? filename->valuestring : "",
                       cJSON_IsString(subject) ? subject->valuestring : "");
    if (!jf) continue;

    cJSON_ArrayForEach(seg_item, segments) {
      const cJSON *msgid  = cJSON_GetObjectItemCaseSensitive(seg_item, "message_id");
      const cJSON *bytes  = cJSON_GetObjectItemCaseSensitive(seg_item, "bytes");
      const cJSON *number = cJSON_GetObjectItemCaseSensitive(seg_item, "number");
      /* Advance bit for every segment entry seen, valid or not, so a
       * malformed entry can't desync the rest of this file's (or a later
       * file's) bits from the ones job_progress_snapshot() actually
       * meant -- see that function's comment for the ordering they share. */
      int downloaded = bitmap && bit / 8 < bitmap_len && (bitmap[bit / 8] & (1u << (bit % 8)));
      bit++;

      if (!cJSON_IsString(msgid)) continue;

      if (job_file_add_segment(jf, msgid->valuestring,
                                cJSON_IsNumber(bytes) ? (long)bytes->valuedouble : 0,
                                cJSON_IsNumber(number) ? number->valueint : 0) == 0) {
        jf->segments[jf->segment_count - 1].downloaded = downloaded;
      }
    }
  }

  free(bitmap);
  cJSON_Delete(root);
}

job_t *
job_load(const char *path) {
  FILE *f;
  char *buf;
  long size;
  cJSON *root;
  job_t *job;
  const cJSON *id, *name, *state, *priority, *retries, *last_error, *final_bytes;
  const cJSON *output_dir, *add_to_shadowmount, *auto_install_pkgs, *nfo_path;

  if (!(f = fopen(path, "rb"))) {
    log_error("job: fopen(%s): %s", path, strerror(errno));
    return NULL;
  }

  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (size < 0 || !(buf = malloc((size_t)size + 1))) {
    fclose(f);
    return NULL;
  }

  if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
    log_error("job: short read on %s", path);
    free(buf);
    fclose(f);
    return NULL;
  }
  buf[size] = 0;
  fclose(f);

  root = cJSON_Parse(buf);
  free(buf);

  if (!root) {
    log_error("job: invalid JSON in %s", path);
    return NULL;
  }

  if (!(job = calloc(1, sizeof *job))) {
    cJSON_Delete(root);
    return NULL;
  }

  id          = cJSON_GetObjectItemCaseSensitive(root, "id");
  name        = cJSON_GetObjectItemCaseSensitive(root, "name");
  state       = cJSON_GetObjectItemCaseSensitive(root, "state");
  priority    = cJSON_GetObjectItemCaseSensitive(root, "priority");
  retries     = cJSON_GetObjectItemCaseSensitive(root, "retries_used");
  last_error  = cJSON_GetObjectItemCaseSensitive(root, "last_error");
  final_bytes = cJSON_GetObjectItemCaseSensitive(root, "final_bytes");
  output_dir  = cJSON_GetObjectItemCaseSensitive(root, "output_dir");
  add_to_shadowmount = cJSON_GetObjectItemCaseSensitive(root, "add_to_shadowmount");
  auto_install_pkgs = cJSON_GetObjectItemCaseSensitive(root, "auto_install_pkgs");
  nfo_path    = cJSON_GetObjectItemCaseSensitive(root, "nfo_path");

  if (cJSON_IsString(id))   snprintf(job->id, sizeof job->id, "%s", id->valuestring);
  if (cJSON_IsString(name)) snprintf(job->name, sizeof job->name, "%s", name->valuestring);
  job->state = cJSON_IsString(state) ? job_state_from_name(state->valuestring) : JOB_QUEUED;
  if (cJSON_IsNumber(priority)) job->priority = priority->valueint;
  if (cJSON_IsNumber(retries)) job->retries_used = retries->valueint;
  if (cJSON_IsString(last_error)) snprintf(job->last_error, sizeof job->last_error, "%s", last_error->valuestring);
  if (cJSON_IsNumber(final_bytes)) job->final_bytes = (long long)final_bytes->valuedouble;
  if (cJSON_IsString(output_dir)) snprintf(job->output_dir, sizeof job->output_dir, "%s", output_dir->valuestring);
  job->add_to_shadowmount = cJSON_IsTrue(add_to_shadowmount);
  job->auto_install_pkgs = cJSON_IsTrue(auto_install_pkgs);
  if (cJSON_IsString(nfo_path)) snprintf(job->nfo_path, sizeof job->nfo_path, "%s", nfo_path->valuestring);

  {
    const cJSON *passwords = cJSON_GetObjectItemCaseSensitive(root, "passwords");
    const cJSON *pw_item;
    cJSON_ArrayForEach(pw_item, passwords) {
      if (cJSON_IsString(pw_item)) job_add_password(job, pw_item->valuestring);
    }
  }

  {
    const cJSON *pkg_paths = cJSON_GetObjectItemCaseSensitive(root, "pkg_paths");
    const cJSON *pkg_item;
    cJSON_ArrayForEach(pkg_item, pkg_paths) {
      if (cJSON_IsString(pkg_item)) job_add_pkg_path(job, pkg_item->valuestring);
    }
  }

  /* File/segment structure and download progress live in their own
   * sidecars -- see job_sidecar_path()'s comment. */
  job_load_segments_and_progress(job, path);

  cJSON_Delete(root);
  return job;
}
