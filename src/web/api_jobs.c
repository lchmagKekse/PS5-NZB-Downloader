#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../download/download.h"
#include "../log/log.h"
#include "../nntp/nntp_pool.h"
#include "../nzb/nzb_parse.h"
#include "../storage/paths.h"
#include "api.h"
#include "app_state.h"
#include "job_json.h"
#include "json_util.h"

#define MAX_JOBS_LISTED 512

enum MHD_Result
api_jobs_list(struct MHD_Connection *conn) {
  cJSON *arr = cJSON_CreateArray();
  job_t *jobs[MAX_JOBS_LISTED];
  size_t n, i;

  queue_lock();
  n = queue_list_jobs(g_app.queue, jobs, MAX_JOBS_LISTED);
  for (i = 0; i < n; i++) {
    job_ensure_nfo_scanned(jobs[i]);
    cJSON_AddItemToArray(arr, job_to_json_summary(jobs[i]));
  }
  queue_unlock();

  return json_respond(conn, MHD_HTTP_OK, arr);
}

enum MHD_Result
api_jobs_get(struct MHD_Connection *conn, const char *id) {
  job_t *job;
  cJSON *body = NULL;

  queue_lock();
  job = queue_find_job(g_app.queue, id);
  if (job) {
    job_ensure_nfo_scanned(job);
    body = job_to_json_detail(job);
  }
  queue_unlock();

  if (!body) return json_respond_error(conn, MHD_HTTP_NOT_FOUND, "no such job");
  return json_respond(conn, MHD_HTTP_OK, body);
}

#define NFO_MAX_BYTES (2 * 1024 * 1024) /* real .nfo files are a few KB at most; this is just a sanity cap */

/* True if s (len bytes) is well-formed UTF-8. Not just "no high bytes" --
 * classic CP437 .nfo art is packed with bytes >= 0x80, but real UTF-8
 * multi-byte sequences follow a strict continuation-byte pattern that
 * random CP437 box-drawing runs essentially never satisfy, so this
 * reliably tells the two apart (see cp437_to_utf8() below). */
static int
is_valid_utf8(const unsigned char *s, size_t len) {
  size_t i, j, extra;
  unsigned int cp;
  unsigned char c;

  for (i = 0; i < len; i += extra + 1) {
    c = s[i];

    if (c < 0x80)                 { extra = 0; cp = c; }
    else if ((c & 0xE0) == 0xC0)  { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0)  { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0)  { extra = 3; cp = c & 0x07; }
    else return 0;

    if (i + extra >= len) return 0;

    for (j = 1; j <= extra; j++) {
      unsigned char cc = s[i + j];
      if ((cc & 0xC0) != 0x80) return 0;
      cp = (cp << 6) | (cc & 0x3F);
    }

    if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
        (extra == 3 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
      return 0;
    }
  }

  return 1;
}

/* CP437 (IBM PC/MS-DOS "OEM-US" codepage) upper half -> Unicode -- 0x00-0x7F
 * is identical to ASCII, so only 0x80-0xFF needs a table. This is the
 * codepage classic scene .nfo box-drawing/block art is authored in; a .nfo
 * never declares its own encoding, so it has to be assumed whenever the raw
 * bytes aren't already valid UTF-8 (see is_valid_utf8() above -- some newer
 * releases do author their .nfo directly in UTF-8, which is left alone). */
static const unsigned short cp437_upper[128] = {
  0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,
  0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
  0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,
  0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
  0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,
  0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
  0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,
  0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
  0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,
  0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
  0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,
  0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
  0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,
  0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
  0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,
  0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0,
};

/* Encodes cp (always <= 0xFFFF, all cp437_upper/ASCII ever produces) as
 * UTF-8 into out (room for 3 bytes needed). Returns bytes written. */
static size_t
utf8_encode(unsigned int cp, char *out) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  out[0] = (char)(0xE0 | (cp >> 12));
  out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[2] = (char)(0x80 | (cp & 0x3F));
  return 3;
}

/* Converts len bytes of CP437 text at in to a malloc'd, NUL-terminated
 * UTF-8 string (len*3+1 bytes is always enough -- one CP437 byte is always
 * exactly one codepoint, and every codepoint here fits in 3 UTF-8 bytes).
 * Returns NULL on allocation failure. */
static char *
cp437_to_utf8(const unsigned char *in, size_t len) {
  char *out;
  size_t oi = 0, i;

  if (!(out = malloc(len * 3 + 1))) return NULL;

  for (i = 0; i < len; i++) {
    unsigned char c = in[i];
    oi += utf8_encode(c < 0x80 ? c : cp437_upper[c - 0x80], out + oi);
  }
  out[oi] = 0;

  return out;
}

enum MHD_Result
api_jobs_get_nfo(struct MHD_Connection *conn, const char *id) {
  job_t *job;
  char nfo_path[900] = {0};
  char *content;
  const char *fname;
  long size;
  FILE *f;
  cJSON *body;

  queue_lock();
  job = queue_find_job(g_app.queue, id);
  if (job) {
    job_ensure_nfo_scanned(job);
    snprintf(nfo_path, sizeof nfo_path, "%s", job->nfo_path);
  }
  queue_unlock();

  if (!job) return json_respond_error(conn, MHD_HTTP_NOT_FOUND, "no such job");
  if (!nfo_path[0]) return json_respond_error(conn, MHD_HTTP_NOT_FOUND, "no .nfo file found for this job");

  if (!(f = fopen(nfo_path, "rb"))) {
    log_error("api_jobs_get_nfo: fopen(%s): %s", nfo_path, strerror(errno));
    return json_respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "could not open .nfo file");
  }

  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (size < 0 || size > NFO_MAX_BYTES) {
    fclose(f);
    return json_respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "nfo file is too large to display");
  }

  if (!(content = malloc((size_t)size + 1))) {
    fclose(f);
    return json_respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory reading .nfo file");
  }
  if (fread(content, 1, (size_t)size, f) != (size_t)size) {
    fclose(f);
    free(content);
    return json_respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "short read on .nfo file");
  }
  content[size] = 0;
  fclose(f);

  /* .nfo files never declare an encoding -- if these bytes don't already
   * form valid UTF-8, assume the classic scene CP437 art codepage rather
   * than ship raw bytes the browser's UTF-8 JSON parser would mangle into
   * replacement characters. */
  if (!is_valid_utf8((const unsigned char *)content, (size_t)size)) {
    char *converted = cp437_to_utf8((const unsigned char *)content, (size_t)size);
    if (converted) {
      free(content);
      content = converted;
    }
  }

  fname = strrchr(nfo_path, '/');
  fname = fname ? fname + 1 : nfo_path;

  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "filename", fname);
  cJSON_AddStringToObject(body, "content", content);
  free(content);

  return json_respond(conn, MHD_HTTP_OK, body);
}

/* Writes the uploaded NZB to a scratch file so nzb_parse_file() (which
 * streams from disk) doesn't need a second in-memory copy of it. */
enum MHD_Result
api_jobs_create(struct MHD_Connection *conn, const unsigned char *nzb_data,
                size_t nzb_len, const char *nzb_filename, const char *display_name,
                const char *output_dir, int add_to_shadowmount) {
  char scratch_dir[600], scratch_path[768];
  unsigned char rand_bytes[8];
  char rand_hex[17];
  FILE *f;
  job_t *job;
  cJSON *body;
  int i;

  if (nzb_len == 0) {
    return json_respond_error(conn, MHD_HTTP_BAD_REQUEST, "empty NZB upload");
  }

  config_lock();
  snprintf(scratch_dir, sizeof scratch_dir, "%s/.uploads", g_app.config.storage.temp_dir);
  config_unlock();

  if (mkdir_p(scratch_dir, 0755) < 0) {
    return json_respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "could not prepare upload scratch space");
  }

  arc4random_buf(rand_bytes, sizeof rand_bytes);
  for (i = 0; i < 8; i++) snprintf(rand_hex + i * 2, 3, "%02x", rand_bytes[i]);
  snprintf(scratch_path, sizeof scratch_path, "%s/%s.nzb", scratch_dir, rand_hex);

  if (!(f = fopen(scratch_path, "wb"))) {
    log_error("api_jobs_create: fopen(%s): %s", scratch_path, strerror(errno));
    return json_respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "could not buffer upload to disk");
  }
  if (fwrite(nzb_data, 1, nzb_len, f) != nzb_len) {
    fclose(f);
    remove(scratch_path);
    return json_respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "short write buffering upload");
  }
  fclose(f);

  job = nzb_parse_file(scratch_path);
  remove(scratch_path);

  if (!job) {
    return json_respond_error(conn, MHD_HTTP_BAD_REQUEST, "could not parse NZB (malformed XML?)");
  }
  if (job->file_count == 0) {
    job_free(job);
    return json_respond_error(conn, MHD_HTTP_BAD_REQUEST, "NZB has no files");
  }

  /* nzb_parse_file() names the job after the scratch path's random hex
   * basename; prefer the original filename, or display_name if the client
   * sent one. Also becomes the output directory name (download.c). */
  if (display_name && display_name[0]) {
    snprintf(job->name, sizeof job->name, "%s", display_name);
    path_sanitize_component(job->name, sizeof job->name);
  } else if (nzb_filename && nzb_filename[0]) {
    snprintf(job->name, sizeof job->name, "%s", nzb_filename);
    path_sanitize_component(job->name, sizeof job->name);
  }

  /* Full path, not a single component, so unlike job->name this doesn't
   * go through path_sanitize_component() -- same trust level as
   * storage.output_dir, also free-form text from the Settings page. */
  if (output_dir && output_dir[0]) {
    snprintf(job->output_dir, sizeof job->output_dir, "%s", output_dir);
  }
  job->add_to_shadowmount = add_to_shadowmount;

  queue_lock();
  if (queue_add_job(g_app.queue, job) < 0) {
    queue_unlock();
    return json_respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "could not persist job");
  }
  body = job_to_json_detail(job);
  queue_unlock();

  return json_respond(conn, MHD_HTTP_CREATED, body);
}

typedef int (*job_action_fn)(queue_t *, const char *);

/* cancel_pool: pause/cancel need this, resume/retry don't. Flipping
 * job->state alone stops new segments from being dispatched but leaves
 * already-queued ones in the NNTP pool to keep downloading regardless --
 * nntp_pool_cancel_all() drops those (in-flight ones still finish). Safe
 * unconditionally: only one job downloads at a time, so the pool's queue
 * never holds a different job's segments. */
static enum MHD_Result
run_action(struct MHD_Connection *conn, const char *id, job_action_fn fn, int cancel_pool) {
  int rc;

  queue_lock();
  rc = fn(g_app.queue, id);
  queue_unlock();

  if (rc < 0) {
    return json_respond_error(conn, MHD_HTTP_CONFLICT,
                               "unknown job id, or action not valid for this job's current state");
  }

  if (cancel_pool) {
    pool_lock();
    nntp_pool_cancel_all(g_app.pool);
    pool_unlock();
  }

  return json_respond(conn, MHD_HTTP_OK, cJSON_CreateObject());
}

enum MHD_Result api_jobs_pause(struct MHD_Connection *conn, const char *id)  { return run_action(conn, id, queue_pause_job, 1); }
enum MHD_Result api_jobs_resume(struct MHD_Connection *conn, const char *id) { return run_action(conn, id, queue_resume_job, 0); }
enum MHD_Result api_jobs_cancel(struct MHD_Connection *conn, const char *id) { return run_action(conn, id, queue_cancel_job, 1); }
enum MHD_Result api_jobs_retry(struct MHD_Connection *conn, const char *id)  { return run_action(conn, id, queue_retry_job, 0); }

enum MHD_Result
api_jobs_delete(struct MHD_Connection *conn, const char *id) {
  char temp_dir[700], dest_dir[900] = {0};
  job_t *job;
  int rc;

  config_lock();
  snprintf(temp_dir, sizeof temp_dir, "%s/%s", g_app.config.storage.temp_dir, id);
  config_unlock();

  queue_lock();

  if ((job = queue_find_job(g_app.queue, id)) && job->state != JOB_COMPLETED) {
    job_output_dest_dir(job, dest_dir, sizeof dest_dir);
  }
  rc = queue_remove_job(g_app.queue, id);
  queue_unlock();

  if (rc == -2) {
    return json_respond_error(conn, MHD_HTTP_CONFLICT,
        "job is still actively being processed - try again in a moment");
  }
  if (rc < 0) return json_respond_error(conn, MHD_HTTP_NOT_FOUND, "no such job");

  /* Best-effort: the job record is gone either way, so a cleanup
   * failure here is logged, not surfaced as a DELETE failure. */
  rmdir_recursive(temp_dir);

  if (dest_dir[0]) rmdir_recursive(dest_dir);

  return json_respond(conn, MHD_HTTP_OK, cJSON_CreateObject());
}
