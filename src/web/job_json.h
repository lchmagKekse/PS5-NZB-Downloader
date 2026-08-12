/* job_t -> cJSON, shared by api_status.c and api_jobs.c so the "queued
 * job" and "job on the dashboard" shapes never drift apart.
 *
 * Caller must hold queue_lock() across the call -- these read job_t
 * fields with no locking of their own.
 */
#pragma once

#include "../queue/job.h"
#include "../vendor/cjson/cJSON.h"

/* id, name, state, priority, retries_used, last_error, progress
 * (segment/byte counts). Used for job list views. */
cJSON *job_to_json_summary(const job_t *job);

/* Everything job_to_json_summary() has, plus a per-file breakdown
 * (filename, subject, bytes, segment counts). Used for the single-job
 * detail view. */
cJSON *job_to_json_detail(const job_t *job);
