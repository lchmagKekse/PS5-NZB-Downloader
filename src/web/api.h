/* Route handler declarations, implemented across api_status.c, api_jobs.c,
 * api_config.c, api_logs.c. httpd.c's router is the only caller.
 */
#pragma once

#include <microhttpd.h>

enum MHD_Result api_status_get(struct MHD_Connection *conn);

enum MHD_Result api_jobs_list(struct MHD_Connection *conn);
enum MHD_Result api_jobs_get(struct MHD_Connection *conn, const char *id);
/* {"filename": "...", "content": "..."} for the first .nfo file found under
 * the job's output directory (see job_ensure_nfo_scanned(), download.h).
 * 404 if the job doesn't exist, or has no .nfo (yet, or ever). */
enum MHD_Result api_jobs_get_nfo(struct MHD_Connection *conn, const char *id);
/* nzb_data/nzb_len/nzb_filename come from the multipart "nzb" field
 * (httpd.c's upload_iterator). display_name, if non-empty, overrides
 * nzb_filename as the job's name/output subdir; output_dir, if non-empty,
 * overrides storage.output_dir for this job alone. */
enum MHD_Result api_jobs_create(struct MHD_Connection *conn, const unsigned char *nzb_data,
                                 size_t nzb_len, const char *nzb_filename,
                                 const char *display_name, const char *output_dir,
                                 int add_to_shadowmount);
enum MHD_Result api_jobs_pause(struct MHD_Connection *conn, const char *id);
enum MHD_Result api_jobs_resume(struct MHD_Connection *conn, const char *id);
enum MHD_Result api_jobs_cancel(struct MHD_Connection *conn, const char *id);
enum MHD_Result api_jobs_retry(struct MHD_Connection *conn, const char *id);
enum MHD_Result api_jobs_delete(struct MHD_Connection *conn, const char *id);

enum MHD_Result api_config_get(struct MHD_Connection *conn);
enum MHD_Result api_config_post(struct MHD_Connection *conn, const char *body, size_t body_len);

enum MHD_Result api_logs_get(struct MHD_Connection *conn);
/* Streams the full on-disk logfile as a download, unlike api_logs_get()'s
 * ring buffer of just the last LOG_RING_CAPACITY lines. */
enum MHD_Result api_logs_download(struct MHD_Connection *conn);

/* Cleanly stops the httpd loop so the payload returns control to whatever
 * loaded it, without rebooting the console -- see api_system.c. */
enum MHD_Result api_system_eject(struct MHD_Connection *conn);
