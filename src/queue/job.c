#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static void
generate_job_id(char out[JOB_ID_LEN]) {
  unsigned char b[16];

  arc4random_buf(b, sizeof b);
  b[6] = (b[6] & 0x0F) | 0x40; /* version 4 */
  b[8] = (b[8] & 0x3F) | 0x80; /* variant 10xx */

  snprintf(out, JOB_ID_LEN,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
           b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
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

void
job_segment_progress(const job_t *job, size_t *total, size_t *downloaded) {
  size_t fi, si, t = 0, d = 0;

  for (fi = 0; fi < job->file_count; fi++) {
    const job_file_t *f = &job->files[fi];
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

int
job_save(const job_t *job, const char *path) {
  cJSON *root = cJSON_CreateObject();
  cJSON *files = cJSON_CreateArray();
  char tmp_path[600];
  FILE *f;
  char *text;
  size_t fi, si;
  int ok = 1;

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
    for (fi = 0; fi < job->password_count; fi++) {
      cJSON_AddItemToArray(passwords, cJSON_CreateString(job->passwords[fi]));
    }
    cJSON_AddItemToObject(root, "passwords", passwords);
  }

  {
    cJSON *pkg_paths = cJSON_CreateArray();
    for (fi = 0; fi < job->pkg_count; fi++) {
      cJSON_AddItemToArray(pkg_paths, cJSON_CreateString(job->pkg_paths[fi]));
    }
    cJSON_AddItemToObject(root, "pkg_paths", pkg_paths);
  }

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
      cJSON_AddBoolToObject(seg, "downloaded", s->downloaded);

      cJSON_AddItemToArray(segments, seg);
    }

    cJSON_AddItemToArray(files, file);
  }

  text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  if (!text) {
    log_error("[%s] job: failed to serialize", job->id);
    return -1;
  }

  snprintf(tmp_path, sizeof tmp_path, "%s.tmp", path);
  if (!(f = fopen(tmp_path, "w"))) {
    log_error("[%s] job: fopen(%s): %s", job->id, tmp_path, strerror(errno));
    free(text);
    return -1;
  }
  if (fputs(text, f) == EOF) ok = 0;
  /* fsync before close: fclose() alone only reaches the page cache, and an
   * unclean shutdown could let the rename() below land durably while these
   * bytes are lost, leaving a correctly-named but 0-byte job file. */
  if (ok && fflush(f) != 0) ok = 0;
  if (ok && fsync(fileno(f)) != 0) ok = 0;
  if (fclose(f) != 0) ok = 0;
  free(text);

  if (!ok) {
    log_error("[%s] job: write error saving %s", job->id, path);
    return -1;
  }

  if (rename(tmp_path, path) != 0) {
    log_error("[%s] job: rename(%s, %s): %s", job->id, tmp_path, path, strerror(errno));
    return -1;
  }

  /* rename() is itself a directory-metadata change; fsync the dir too so a
   * crash right after can't lose it. Best-effort -- job data is already safe. */
  {
    char dir_path[600];
    char *slash;
    int dirfd;

    snprintf(dir_path, sizeof dir_path, "%s", path);
    if ((slash = strrchr(dir_path, '/'))) {
      *slash = 0;
      if ((dirfd = open(dir_path, O_RDONLY)) >= 0) {
        if (fsync(dirfd) != 0) {
          log_warn("[%s] job: fsync(%s): %s", job->id, dir_path, strerror(errno));
        }
        close(dirfd);
      }
    }
  }

  return 0;
}

job_t *
job_load(const char *path) {
  FILE *f;
  char *buf;
  long size;
  cJSON *root, *files, *file_item;
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
      const cJSON *msgid      = cJSON_GetObjectItemCaseSensitive(seg_item, "message_id");
      const cJSON *bytes      = cJSON_GetObjectItemCaseSensitive(seg_item, "bytes");
      const cJSON *number     = cJSON_GetObjectItemCaseSensitive(seg_item, "number");
      const cJSON *downloaded = cJSON_GetObjectItemCaseSensitive(seg_item, "downloaded");

      if (!cJSON_IsString(msgid)) continue;

      if (job_file_add_segment(jf, msgid->valuestring,
                                cJSON_IsNumber(bytes) ? (long)bytes->valuedouble : 0,
                                cJSON_IsNumber(number) ? number->valueint : 0) == 0) {
        jf->segments[jf->segment_count - 1].downloaded = cJSON_IsTrue(downloaded);
      }
    }
  }

  cJSON_Delete(root);
  return job;
}
