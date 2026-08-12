/* Archive extraction for a completed job, using the vendored libarchive.
 * Detects the starting volume of a (possibly multi-volume) RAR/7z/zip
 * among job's files, extracts into dest_dir, trying job->passwords[] in
 * order if encrypted. No archive-shaped files is not an error -- see
 * extract_result_t.
 *
 * Also handles rar-within-rar (scene release layout): after extracting,
 * dest_dir is rescanned recursively for another archive set, extracted
 * in place with its volume files removed; failures there are logged,
 * not fatal.
 */
#pragma once

#include <stddef.h>

#include "../queue/job.h"

typedef enum {
  EXTRACT_NONE,    /* no archive-shaped file found among job's files -- nothing to do */
  EXTRACT_OK,      /* found and extracted at least one archive (individual bad entries are logged and skipped, not fatal) */
  EXTRACT_FAILED,  /* found an archive but couldn't extract it -- see err */
} extract_result_t;

/* Progress callback invoked as bytes are written during extract_job() --
 * may fire once per libarchive read/write block, so keep it cheap.
 * bytes_done is cumulative across all archive sets; bytes_total is an
 * approximation (sum of source volume sizes, not decompressed output).
 * May be NULL. */
typedef void (*extract_progress_cb)(void *ctx, long long bytes_done, long long bytes_total);

/* Scans job's files in src_dir for the starting volume of an archive
 * (multi-volume RAR old/new-style, multi-volume 7z, or single-file
 * rar/7z/zip) and extracts each into dest_dir (created if needed;
 * libarchive locates continuation volumes on its own). Returns
 * EXTRACT_NONE if nothing archive-shaped is found; err holds a short
 * reason on EXTRACT_FAILED. */
extract_result_t extract_job(const job_t *job, const char *src_dir, const char *dest_dir,
                              extract_progress_cb progress_cb, void *progress_ctx,
                              char *err, size_t err_size);
