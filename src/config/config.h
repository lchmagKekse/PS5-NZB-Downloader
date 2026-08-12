/* Application configuration: NNTP server settings and storage paths.
 *
 * Loaded from a key=value file (see nzb.conf.example). config_save() writes
 * the same format back out, so the file remains hand-editable.
 *
 * The password field is only ever read from / written to this file and
 * used to authenticate with the NNTP server. Callers that hand config to
 * logging, the HTTP API, or anything else outside the NNTP layer MUST use
 * config_redacted() rather than passing the loaded struct directly.
 */
#pragma once

#include <stddef.h>

typedef struct {
  char host[256];
  char port[8];              /* numeric string; getaddrinfo on this SDK is IPv4-only and has no /etc/services */
  int  tls;                  /* 0 = plain NNTP, 1 = implicit TLS */
  char user[128];            /* empty user skips AUTHINFO entirely */
  char pass[128];
  int  max_connections;      /* bounded worker pool size */
  int  connect_timeout_sec;
  int  read_timeout_sec;
  int  retry_count;
} nntp_config_t;

typedef struct {
  char output_dir[512];      /* final destination for completed downloads --
                               * both extracted archive contents and, for a
                               * job with no archive, the moved-as-is real
                               * files; see download.c's finalize_job() */
  char temp_dir[512];        /* in-progress segment/assembly scratch space,
                               * one subdirectory per job (also where PAR2
                               * verification reads from -- no separate
                               * working directory needed, verify is
                               * read-only) */
} storage_config_t;

typedef struct {
  /* Automatic re-queue attempts for a job that fails for a transient reason
   * (server unreachable, segments never arrived) before it's left in
   * JOB_FAILED for a manual retry -- see job_fail_or_retry() in download.c.
   * 0 disables auto-retry. Does not apply to failures unlikely to change on
   * retry (out of disk space, PAR2 repair exhausted, extract errors). */
  int max_retries;
} queue_config_t;

typedef struct {
  nntp_config_t    nntp;
  storage_config_t storage;
  queue_config_t   queue;
} app_config_t;

/* Fills cfg with documented defaults (max_connections=4, timeouts, etc.)
 * before load, so a config file only needs to specify what it overrides. */
void app_config_set_defaults(app_config_t *cfg);

/* Returns 0 on success, -1 on error (missing file, missing required host).
 * Logs the specific problem via log_error() before returning. */
int app_config_load(const char *path, app_config_t *cfg);

/* Writes cfg back to path in the same key=value format. Returns 0 on
 * success, -1 on error. */
int app_config_save(const char *path, const app_config_t *cfg);

/* Returns a copy of cfg with the NNTP password cleared, safe to log,
 * serialize to the HTTP API, or otherwise hand outside the NNTP layer. */
app_config_t app_config_redacted(const app_config_t *cfg);
