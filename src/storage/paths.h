/* Shared path-safety helpers. Every string from outside this process (an
 * NZB's declared filename/subject, a job's display name) MUST go through
 * path_sanitize_component() before use in a filesystem path -- that's what
 * keeps a crafted NZB from writing outside the storage directories.
 */
#pragma once

#include <stddef.h>
#include <sys/types.h>

/* Sanitizes name in place: '/' and '\\' become '_', and an empty/"."/".."
 * result is replaced with "unnamed". After this call, name is safe to use
 * as a single path component (it can no longer change directory). */
void path_sanitize_component(char *name, size_t size);

/* Joins dir and component with a single '/', writing into out. Plain
 * concatenation is safe here specifically because callers are expected to
 * have already run component through path_sanitize_component(). */
void path_join(char *out, size_t out_size, const char *dir, const char *component);

/* mkdir -p: creates path and any missing parent directories. path must be
 * absolute. Returns 0 on success (including "already exists"), -1 on
 * failure (already logged). */
int mkdir_p(const char *path, mode_t mode);

/* rm -rf: removes path and, if a directory, everything under it. Returns 0
 * on success or if path doesn't exist, -1 on failure (already logged).
 * Used to clean up a job's temp dir once its job record is gone for good --
 * not for pause/cancel, which keep temp data for a possible retry. */
int rmdir_recursive(const char *path);

/* Recursively sums the size of every regular file under path (matching
 * what "du" measures) -- used to report a completed job's real output
 * size, since extraction can leave that very different from what was
 * downloaded. Returns 0 if path doesn't exist or can't be read (logged). */
long long path_dir_total_bytes(const char *path);

/* Bytes available (to non-superuser) on the filesystem containing path,
 * or -1 on error (already logged) -- e.g. path's directory doesn't
 * exist yet. Used to check disk space before starting a download rather
 * than discovering it's full partway through (see download.c). */
long long path_free_bytes(const char *path);
