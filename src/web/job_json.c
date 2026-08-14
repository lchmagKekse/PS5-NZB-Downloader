#include <string.h>

#include "app_state.h"
#include "job_json.h"

static cJSON *
progress_json(const job_t *job) {
  size_t total, downloaded, fi;
  long total_bytes = 0, downloaded_bytes = 0;
  cJSON *p = cJSON_CreateObject();

  job_segment_progress(job, &total, &downloaded);

  for (fi = 0; fi < job->file_count; fi++) {
    const job_file_t *jf = &job->files[fi];
    size_t si;

    total_bytes += jf->bytes;
    for (si = 0; si < jf->segment_count; si++) {
      if (jf->segments[si].downloaded) downloaded_bytes += jf->segments[si].bytes;
    }
  }

  cJSON_AddNumberToObject(p, "total_segments", (double)total);
  cJSON_AddNumberToObject(p, "downloaded_segments", (double)downloaded);

  /* Once finalize_job() sets job->final_bytes (real on-disk output size,
   * see job.h), report that instead of the downloaded size -- extraction
   * can leave a very different byte count than what was fetched. */
  if (job->final_bytes > 0) {
    cJSON_AddNumberToObject(p, "total_bytes", (double)job->final_bytes);
    cJSON_AddNumberToObject(p, "downloaded_bytes", (double)job->final_bytes);
  } else {
    cJSON_AddNumberToObject(p, "total_bytes", (double)total_bytes);
    cJSON_AddNumberToObject(p, "downloaded_bytes", (double)downloaded_bytes);
  }

  /* Segment progress is already 100% during JOB_EXTRACTING, so surface
   * extract_mu's fields too, for the one job currently extracting. */
  extract_lock();
  if (g_app.extract_job_id[0] && !strcmp(g_app.extract_job_id, job->id)) {
    cJSON_AddBoolToObject(p, "extracting", 1);
    cJSON_AddNumberToObject(p, "extract_bytes_done", (double)g_app.extract_bytes_done);
    cJSON_AddNumberToObject(p, "extract_bytes_total", (double)g_app.extract_bytes_total);
  }
  extract_unlock();

  /* Same idea, for JOB_VERIFYING -- see download.c's par2_verify_job(). */
  verify_lock();
  if (g_app.verify_job_id[0] && !strcmp(g_app.verify_job_id, job->id)) {
    cJSON_AddBoolToObject(p, "verifying", 1);
    cJSON_AddNumberToObject(p, "verify_bytes_done", (double)g_app.verify_bytes_done);
    cJSON_AddNumberToObject(p, "verify_bytes_total", (double)g_app.verify_bytes_total);
  }
  verify_unlock();

  /* Same idea, for JOB_REPAIRING -- see download.c's par2_repair_job(). */
  repair_lock();
  if (g_app.repair_job_id[0] && !strcmp(g_app.repair_job_id, job->id)) {
    cJSON_AddBoolToObject(p, "repairing", 1);
    cJSON_AddNumberToObject(p, "repair_bytes_done", (double)g_app.repair_bytes_done);
    cJSON_AddNumberToObject(p, "repair_bytes_total", (double)g_app.repair_bytes_total);
  }
  repair_unlock();

  return p;
}

cJSON *
job_to_json_summary(const job_t *job) {
  cJSON *o = cJSON_CreateObject();

  cJSON_AddStringToObject(o, "id", job->id);
  cJSON_AddStringToObject(o, "name", job->name);
  cJSON_AddStringToObject(o, "state", job_state_name(job->state));
  cJSON_AddNumberToObject(o, "priority", job->priority);
  cJSON_AddNumberToObject(o, "retries_used", job->retries_used);
  cJSON_AddStringToObject(o, "last_error", job->last_error);
  cJSON_AddStringToObject(o, "output_dir", job->output_dir);
  cJSON_AddBoolToObject(o, "add_to_shadowmount", job->add_to_shadowmount);
  cJSON_AddBoolToObject(o, "auto_install_pkgs", job->auto_install_pkgs);
  cJSON_AddBoolToObject(o, "has_nfo", job->nfo_path[0] != 0);
  cJSON_AddNumberToObject(o, "pkg_count", (double)job->pkg_count);
  /* Count only, not the passwords themselves -- distinguishes "none
   * known" from "had one, it just didn't work" without echoing them. */
  cJSON_AddNumberToObject(o, "password_count", (double)job->password_count);
  cJSON_AddItemToObject(o, "progress", progress_json(job));

  return o;
}

cJSON *
job_to_json_detail(const job_t *job) {
  cJSON *o = job_to_json_summary(job);
  cJSON *files = cJSON_CreateArray();
  cJSON *pkg_files = cJSON_CreateArray();
  size_t fi;

  /* Basenames only, for the "Install PKGs" modal's checkbox list (see
   * main.js's installPkgs()) -- the API handler that actually installs
   * them (api_jobs_install_pkgs()) re-derives the full paths itself from
   * job->pkg_paths, so the client never needs to see (or round-trip) an
   * absolute on-console path. */
  for (fi = 0; fi < job->pkg_count; fi++) {
    const char *base = strrchr(job->pkg_paths[fi], '/');
    cJSON_AddItemToArray(pkg_files, cJSON_CreateString(base ? base + 1 : job->pkg_paths[fi]));
  }
  cJSON_AddItemToObject(o, "pkg_files", pkg_files);

  for (fi = 0; fi < job->file_count; fi++) {
    const job_file_t *jf = &job->files[fi];
    cJSON *f = cJSON_CreateObject();
    size_t si, downloaded = 0;

    for (si = 0; si < jf->segment_count; si++) {
      if (jf->segments[si].downloaded) downloaded++;
    }

    cJSON_AddStringToObject(f, "filename", jf->filename);
    cJSON_AddStringToObject(f, "subject", jf->subject);
    cJSON_AddNumberToObject(f, "bytes", (double)jf->bytes);
    cJSON_AddNumberToObject(f, "segment_count", (double)jf->segment_count);
    cJSON_AddNumberToObject(f, "downloaded_segment_count", (double)downloaded);

    cJSON_AddItemToArray(files, f);
  }

  cJSON_AddItemToObject(o, "files", files);

  return o;
}
