#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <archive.h>
#include <archive_entry.h>

#include "../log/log.h"
#include "../storage/paths.h"
#include "extract.h"
#include "rar5_crypt.h"

/* Larger than libarchive's typical 10240 default: avoids a read() syscall
 * per small refill on multi-GB volume files, same rationale as
 * nntp_conn.c's RBUF_SIZE. */
#define ARCHIVE_READ_BLOCK_SIZE (256 * 1024)

typedef enum {
  AKIND_RAR,
  AKIND_SEVENZ,
  AKIND_ZIP,
} archive_kind_t;

typedef struct {
  archive_kind_t kind;
  char base[400];  /* identity shared by every volume of the same set, e.g. "movie" for both "movie.rar" and "movie.r00", or "movie" for "movie.7z.001"/"movie.7z.002" */
  long volume;      /* ascending sort key within a (kind, base) group -- NOT necessarily the number printed in the filename, see classify() */
} archive_id_t;

typedef struct {
  archive_kind_t kind;
  char base[400];
  long volume;
  char filename[512];
} member_t;

/* Shared across archive sets in one extract_job() call so bytes_done
 * accumulates rather than resets -- see extract_progress_cb in extract.h. */
typedef struct {
  long long done;
  long long total;
  extract_progress_cb cb;
  void *cb_ctx;
} progress_t;

/* Returns nonzero if the callback asked us to abort. */
static int
report_progress(progress_t *pg, size_t n) {
  if (!pg) return 0;
  pg->done += (long long)n;
  return pg->cb ? pg->cb(pg->cb_ctx, pg->done, pg->total) : 0;
}

static int
has_suffix_ci(const char *s, const char *suffix) {
  size_t slen = strlen(s), suflen = strlen(suffix);
  return slen >= suflen && !strcasecmp(s + slen - suflen, suffix);
}

static int
all_digits(const char *s, size_t len) {
  size_t i;
  if (len == 0) return 0;
  for (i = 0; i < len; i++) {
    if (!isdigit((unsigned char)s[i])) return 0;
  }
  return 1;
}

static void
set_base(archive_id_t *out, const char *name, size_t base_len) {
  if (base_len >= sizeof out->base) base_len = sizeof out->base - 1;
  memcpy(out->base, name, base_len);
  out->base[base_len] = 0;
}

/* Classifies an archive-family filename into (kind, base identity, ascending
 * volume order) so callers can group and order volumes for
 * archive_read_open_filenames(), which needs them pre-sorted (it does not
 * discover or order sibling volumes itself). Returns 0 if not archive-shaped. */
static int
classify(const char *name, archive_id_t *out) {
  size_t len = strlen(name);

  memset(out, 0, sizeof *out);

  if (has_suffix_ci(name, ".rar")) {
    size_t i = len - 4, digits_end = i;

    while (i > 0 && isdigit((unsigned char)name[i - 1])) i--;
    if (i != digits_end && i >= 5 && !strncasecmp(name + i - 5, ".part", 5)) {
      /* New-style "<base>.part<N>.rar". */
      set_base(out, name, i - 5);
      out->kind = AKIND_RAR;
      out->volume = strtol(name + i, NULL, 10);
      return 1;
    }
    /* Old-style start "<base>.rar" (volume 0 -- always sorts before any
     * ".r00"/".r01"/... continuation, see below). */
    set_base(out, name, len - 4);
    out->kind = AKIND_RAR;
    out->volume = 0;
    return 1;
  }

  {
    const char *p = strrchr(name, '.');
    if (p && isalpha((unsigned char)p[1]) &&
        tolower((unsigned char)p[1]) >= 'r' && tolower((unsigned char)p[1]) <= 'z' &&
        all_digits(p + 2, strlen(p + 2))) {
      int letter_offset = tolower((unsigned char)p[1]) - 'r';
      set_base(out, name, (size_t)(p - name));
      out->kind = AKIND_RAR;
      out->volume = 1 + letter_offset * 100 + strtol(p + 2, NULL, 10); /* r00 -> 1, ..., r99 -> 100, s00 -> 101, ... */
      return 1;
    }
  }

  if (has_suffix_ci(name, ".7z")) {
    set_base(out, name, len - 3);
    out->kind = AKIND_SEVENZ;
    out->volume = 0;
    return 1;
  }

  /* Multi-volume "<base>.7z.<digits>". */
  {
    size_t i = len, digits_end;
    while (i > 0 && isdigit((unsigned char)name[i - 1])) i--;
    digits_end = i;
    if (i != len && i >= 4 && name[i - 1] == '.' && !strncasecmp(name + i - 4, ".7z.", 4)) {
      set_base(out, name, i - 4);
      out->kind = AKIND_SEVENZ;
      out->volume = strtol(name + digits_end, NULL, 10);
      return 1;
    }
  }

  if (has_suffix_ci(name, ".zip")) {
    set_base(out, name, len - 4);
    out->kind = AKIND_ZIP;
    out->volume = 0;
    return 1;
  }

  return 0;
}

static int
cmp_member_volume(const void *a, const void *b) {
  const member_t *ma = a, *mb = b;
  if (ma->volume < mb->volume) return -1;
  if (ma->volume > mb->volume) return 1;
  return 0;
}

/* Rejects an absolute path or ".." component before it's joined onto
 * dest_dir -- defense in depth alongside archive_write_disk's
 * SECURE_NODOTDOT (see extract_one_archive()). Forward slashes are legit
 * subdirectory separators here, unlike path_sanitize_component() which
 * flattens a single component. */
static int
is_safe_relpath(const char *p) {
  const char *seg;

  if (!p || !p[0] || p[0] == '/') return 0;

  for (seg = p; seg; ) {
    const char *slash = strchr(seg, '/');
    size_t len = slash ? (size_t)(slash - seg) : strlen(seg);

    if (len == 0) return 0;                             /* "//" or trailing "/" */
    if (len == 2 && seg[0] == '.' && seg[1] == '.') return 0;

    seg = slash ? slash + 1 : NULL;
  }

  return 1;
}

static int
copy_data(struct archive *ar, struct archive *aw, const char *display_name, const char *job_id,
          la_int64_t entry_size, progress_t *pg, char *err, size_t err_size) {
  la_int64_t copied = 0;

  for (;;) {
    const void *buff;
    size_t size;
    la_int64_t offset;
    int r = archive_read_data_block(ar, &buff, &size, &offset);

    if (r == ARCHIVE_EOF) return 0;
    if (r != ARCHIVE_OK) {
      if (r == ARCHIVE_WARN) {
        log_warn("[%s] extract: %s: %s (after %lld/%lld bytes, %.1f%%)", job_id, display_name,
                 archive_error_string(ar), (long long)copied, (long long)entry_size,
                 entry_size > 0 ? (100.0 * (double)copied / (double)entry_size) : 0.0);
      } else {
        log_error("[%s] extract: %s: read error after %lld/%lld bytes (%.1f%%): %s",
                  job_id, display_name, (long long)copied, (long long)entry_size,
                  entry_size > 0 ? (100.0 * (double)copied / (double)entry_size) : 0.0,
                  archive_error_string(ar));
        snprintf(err, err_size, "%s: read error after %lld/%lld bytes: %s",
                 display_name, (long long)copied, (long long)entry_size, archive_error_string(ar));
        return -1;
      }
    }

    if (archive_write_data_block(aw, buff, size, offset) != ARCHIVE_OK) {
      snprintf(err, err_size, "%s: write error after %lld/%lld bytes: %s",
               display_name, (long long)copied, (long long)entry_size, archive_error_string(aw));
      return -1;
    }

    copied += (la_int64_t)size;

    if (report_progress(pg, size)) {
      snprintf(err, err_size, "%s: extraction aborted", display_name);
      return -1;
    }
  }
}

/* Extracts every entry of the archive spanning volumes (NULL-terminated,
 * ordered paths) into dest_dir. A single bad entry is logged and skipped;
 * only a fatal archive-level error (open failure, bad password, corrupt
 * headers) fails the call. Returns 0/-1 (err filled on failure). */
static int
extract_one_archive(const char **volumes, const char *dest_dir, const job_t *job,
                     progress_t *pg, char *err, size_t err_size) {
  struct archive *a;
  struct archive *ext;
  int rc = 0;
  size_t pi;
  size_t entries_seen = 0, entries_written = 0;
  const char *display = volumes[0];
  void *rar5_ctx = NULL;

  if (!(a = archive_read_new())) {
    snprintf(err, err_size, "out of memory");
    return -1;
  }
  archive_read_support_format_all(a);
  archive_read_support_filter_all(a);

  /* Register every candidate password (see job.h's passwords[]); libarchive
   * tries each registered passphrase against an encrypted entry as needed. */
  for (pi = 0; pi < job->password_count; pi++) {
    archive_read_add_passphrase(a, job->passwords[pi]);
  }

  /* libarchive's RAR5 reader can't decrypt "headers encrypted" archives
   * (WinRAR's "Encrypt file names"; libarchive/libarchive#1374) -- and that
   * failure doesn't reliably surface until archive_read_next_header(), not
   * at open. Detect it upfront and decrypt on the fly instead (rar5_crypt.h). */
  rc = rar5_headers_encrypted(volumes[0])
           ? rar5_open_encrypted(a, job, volumes, &rar5_ctx)
           : archive_read_open_filenames(a, volumes, ARCHIVE_READ_BLOCK_SIZE);

  if (rc != ARCHIVE_OK) {
    snprintf(err, err_size, "could not open %s: %s", display, archive_error_string(a));
    archive_read_free(a);
    rar5_stream_free(rar5_ctx);
    return -1;
  }

  if (!(ext = archive_write_disk_new())) {
    snprintf(err, err_size, "out of memory");
    archive_read_close(a);
    archive_read_free(a);
    rar5_stream_free(rar5_ctx);
    return -1;
  }
  /* No SECURE_NOABSOLUTEPATHS: this module always rewrites entry paths to
   * an absolute path under dest_dir on purpose; is_safe_relpath() already
   * rejects archive-supplied absolute paths. Setting both flags together
   * makes write_header() reject every entry as "Path is absolute".
   * No SECURE_SYMLINKS: it lstat()s every path component and treats
   * non-ENOENT errors as fatal, which fails on this SDK's libc for a
   * not-yet-created parent dir -- is_safe_relpath()+SECURE_NODOTDOT already
   * cover the real threat. No ARCHIVE_EXTRACT_TIME: futimens()/utimes()
   * fails on this SDK's libc for every entry; timestamps don't matter here. */
  archive_write_disk_set_options(ext,
      ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_SECURE_NODOTDOT);
  archive_write_disk_set_standard_lookup(ext);

  for (;;) {
    struct archive_entry *entry;
    const char *entry_path;
    char full_path[1200];
    int r = archive_read_next_header(a, &entry);

    if (r == ARCHIVE_EOF) break;
    if (r == ARCHIVE_WARN) {
      log_warn("[%s] extract: %s: %s", job->id, display, archive_error_string(a));
    } else if (r != ARCHIVE_OK) {
      snprintf(err, err_size, "%s: %s", display, archive_error_string(a));
      rc = -1;
      break;
    }

    entries_seen++;

    entry_path = archive_entry_pathname(entry);
    if (!is_safe_relpath(entry_path)) {
      log_warn("[%s] extract: %s: skipping entry with unsafe path '%s'",
               job->id, display, entry_path ? entry_path : "(null)");
      continue;
    }

    snprintf(full_path, sizeof full_path, "%s/%s", dest_dir, entry_path);
    archive_entry_set_pathname(entry, full_path);

    r = archive_write_header(ext, entry);
    if (r == ARCHIVE_WARN) {
      log_warn("[%s] extract: %s: %s", job->id, full_path, archive_error_string(ext));
    } else if (r != ARCHIVE_OK) {
      log_warn("[%s] extract: %s: %s - skipping this entry", job->id, full_path, archive_error_string(ext));
      continue;
    }

    if (archive_entry_size(entry) > 0 &&
        copy_data(a, ext, full_path, job->id, archive_entry_size(entry), pg, err, err_size) < 0) {
      rc = -1;
      break;
    }

    if (archive_write_finish_entry(ext) != ARCHIVE_OK) {
      log_warn("[%s] extract: %s: %s", job->id, full_path, archive_error_string(ext));
    }

    /* Force 0777 on every entry, overriding ARCHIVE_EXTRACT_PERM: archived
     * PS5 homebrew ELFs often arrive without the executable bit set. */
    if (chmod(full_path, 0777) != 0) {
      log_warn("[%s] extract: chmod(%s, 0777): %s", job->id, full_path, strerror(errno));
    }

    entries_written++;
  }

  archive_write_close(ext);
  archive_write_free(ext);
  archive_read_close(a);
  archive_read_free(a);
  rar5_stream_free(rar5_ctx);

  /* Per-entry failures just log and continue, but if every entry failed,
   * nothing was actually extracted -- that's a real failure, not a
   * partial success. */
  if (rc == 0 && entries_seen > 0 && entries_written == 0) {
    snprintf(err, err_size, "%s: all %zu entries failed to extract", display, entries_seen);
    rc = -1;
  }

  return rc;
}

/* Directory depth to search for a nested (rar-within-rar) archive set.
 * 1 covers the common scene-release layout; the rest is headroom. */
#define NESTED_EXTRACT_MAX_DEPTH 5

typedef struct {
  char name[512];
  int is_dir;
} dirent_info_t;

/* Snapshots dir's entries upfront: readdir() while later deleting consumed
 * volumes from the same directory is asking for trouble. Returns 0 on
 * success (out may be NULL if empty), -1 on failure. */
static int
scan_dir_entries(const char *dir, dirent_info_t **out, size_t *out_count) {
  DIR *d = opendir(dir);
  struct dirent *ent;
  dirent_info_t *list = NULL;
  size_t count = 0, cap = 0;

  *out = NULL;
  *out_count = 0;

  if (!d) return -1;

  while ((ent = readdir(d))) {
    char child[1200];
    struct stat st;

    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;

    if (count == cap) {
      size_t new_cap = cap ? cap * 2 : 16;
      dirent_info_t *tmp = realloc(list, new_cap * sizeof *list);
      if (!tmp) {
        free(list);
        closedir(d);
        return -1;
      }
      list = tmp;
      cap = new_cap;
    }

    snprintf(child, sizeof child, "%s/%s", dir, ent->d_name);
    snprintf(list[count].name, sizeof list[count].name, "%s", ent->d_name);
    list[count].is_dir = (stat(child, &st) == 0 && S_ISDIR(st.st_mode));
    count++;
  }
  closedir(d);

  *out = list;
  *out_count = count;
  return 0;
}

/* Sums each entry's declared uncompressed size (archive_entry_size()),
 * skipping actual data (archive_read_data_skip()) -- cheap, and accurate
 * unlike on-disk volume size when an entry is really compressed. Returns
 * -1 if the archive couldn't be opened; see size_group_fn()'s fallback. */
static long long
peek_archive_total_bytes(const char **volumes, const job_t *job) {
  struct archive *a;
  long long total = 0;
  size_t pi;
  void *rar5_ctx = NULL;
  int rc;

  if (!(a = archive_read_new())) return -1;
  archive_read_support_format_all(a);
  archive_read_support_filter_all(a);

  for (pi = 0; pi < job->password_count; pi++) {
    archive_read_add_passphrase(a, job->passwords[pi]);
  }

  /* Same header-encrypted detection as extract_one_archive(): stock
   * libarchive can't parse a RAR5 "headers encrypted" archive at all, so
   * without this the loop below reads zero entries and silently reports a
   * total of 0 instead of falling back to on-disk sizing. */
  rc = rar5_headers_encrypted(volumes[0])
           ? rar5_open_encrypted(a, job, volumes, &rar5_ctx)
           : archive_read_open_filenames(a, volumes, ARCHIVE_READ_BLOCK_SIZE);

  if (rc != ARCHIVE_OK) {
    archive_read_free(a);
    rar5_stream_free(rar5_ctx);
    return -1;
  }

  for (;;) {
    struct archive_entry *entry;
    int r = archive_read_next_header(a, &entry);

    if (r == ARCHIVE_EOF) break;
    if (r != ARCHIVE_OK && r != ARCHIVE_WARN) break;

    if (archive_entry_size_is_set(entry)) {
      total += (long long)archive_entry_size(entry);
    }
    archive_read_data_skip(a);
  }

  archive_read_close(a);
  archive_read_free(a);
  rar5_stream_free(rar5_ctx);
  return total;
}

/* Per-group callback for walk_nested_archives(): paths is a NULL-terminated,
 * volume-ordered array (not owned by the callback), gcount its length. */
typedef void (*nested_group_fn)(const char *dir, const char **paths, size_t gcount, void *ctx);

/* Recursively walks dir, invoking fn once per rar/7z/zip volume set found
 * (same classify()/group-by-base logic as extract_job()). Shared traversal
 * for both the dry-run sizing pass (size_group_fn) and the real extraction
 * pass (extract_group_fn). */
static void
walk_nested_archives(const char *dir, int depth, const char *job_id, nested_group_fn fn, void *ctx) {
  dirent_info_t *entries = NULL;
  size_t entry_count = 0, ei;
  member_t *members = NULL, *group_buf = NULL;
  int *grouped = NULL;
  size_t member_count = 0;

  if (depth <= 0) return;

  if (scan_dir_entries(dir, &entries, &entry_count) < 0) {
    log_warn("[%s] extract: nested scan: opendir(%s): %s", job_id, dir, strerror(errno));
    return;
  }

  if (entry_count > 0 &&
      (!(members = calloc(entry_count, sizeof *members)) ||
       !(group_buf = malloc(entry_count * sizeof *group_buf)) ||
       !(grouped = calloc(entry_count, sizeof *grouped)))) {
    log_warn("[%s] extract: nested scan: out of memory for %s", job_id, dir);
    goto recurse;
  }

  for (ei = 0; ei < entry_count; ei++) {
    archive_id_t c;

    if (entries[ei].is_dir) continue;
    if (!classify(entries[ei].name, &c)) continue;

    members[member_count].kind = c.kind;
    snprintf(members[member_count].base, sizeof members[member_count].base, "%s", c.base);
    members[member_count].volume = c.volume;
    snprintf(members[member_count].filename, sizeof members[member_count].filename,
             "%s", entries[ei].name);
    member_count++;
  }

  for (ei = 0; ei < member_count; ei++) {
    size_t gj, gcount = 0;
    const char **paths;
    size_t pi;

    if (grouped[ei]) continue;

    for (gj = ei; gj < member_count; gj++) {
      if (members[gj].kind == members[ei].kind && !strcmp(members[gj].base, members[ei].base)) {
        group_buf[gcount++] = members[gj];
        grouped[gj] = 1;
      }
    }

    qsort(group_buf, gcount, sizeof *group_buf, cmp_member_volume);

    if (!(paths = calloc(gcount + 1, sizeof *paths))) {
      log_warn("[%s] extract: nested scan: out of memory for %s", job_id, dir);
      continue;
    }

    {
      int alloc_failed = 0;

      for (pi = 0; pi < gcount; pi++) {
        char *path = malloc(1200);

        if (!path) {
          alloc_failed = 1;
          break;
        }
        snprintf(path, 1200, "%s/%s", dir, group_buf[pi].filename);
        paths[pi] = path;
      }

      if (alloc_failed) {
        log_warn("[%s] extract: nested scan: out of memory for %s", job_id, dir);
      } else {
        fn(dir, paths, gcount, ctx);
      }
    }

    for (pi = 0; pi < gcount; pi++) free((void *)paths[pi]);
    free(paths);
  }

recurse:
  free(members);
  free(group_buf);
  free(grouped);

  for (ei = 0; ei < entry_count; ei++) {
    if (entries[ei].is_dir) {
      char child[1200];
      snprintf(child, sizeof child, "%s/%s", dir, entries[ei].name);
      walk_nested_archives(child, depth - 1, job_id, fn, ctx);
    }
  }

  free(entries);
}

typedef struct {
  const job_t *job;
  long long total;
} nested_size_ctx_t;

/* Dry-run sizing callback: adds this group's real size to ctx->total, or
 * falls back to on-disk size if it couldn't be opened (a group about to
 * fail extraction anyway -- see extract_group_fn()). */
static void
size_group_fn(const char *dir, const char **paths, size_t gcount, void *ctx_) {
  nested_size_ctx_t *ctx = ctx_;
  long long bytes = peek_archive_total_bytes(paths, ctx->job);

  if (bytes >= 0) {
    ctx->total += bytes;
    return;
  }

  log_warn("[%s] extract: nested scan: could not read %s to size it (starting %s) - "
           "falling back to on-disk size for this set", ctx->job->id, dir, paths[0]);
  {
    size_t pi;
    struct stat st;
    for (pi = 0; pi < gcount; pi++) {
      if (stat(paths[pi], &st) == 0) ctx->total += (long long)st.st_size;
    }
  }
}

typedef struct {
  const job_t *job;
  progress_t  *pg;
  int failed;         /* set on the first group that fails to extract */
  char err[256];       /* reason for the first failure, valid iff failed */
} nested_extract_ctx_t;

/* Extraction callback: extracts this group in place into dir and removes
 * the now-redundant volume files on success. A failure (e.g. unmatched
 * password, disk full mid-write) logs and leaves the volumes in place
 * (for inspection or a manual retry), and records the failure on ctx so
 * extract_job() fails the whole job instead of reporting success with
 * incomplete output. Later groups still run so every failure gets
 * logged, but only the first is reported as the job's error. */
static void
extract_group_fn(const char *dir, const char **paths, size_t gcount, void *ctx_) {
  nested_extract_ctx_t *ctx = ctx_;
  char nested_err[256];
  size_t pi;
  long long on_disk_total = 0;

  for (pi = 0; pi < gcount; pi++) {
    struct stat st;
    if (stat(paths[pi], &st) == 0) on_disk_total += (long long)st.st_size;
  }

  log_info("[%s] extract: found nested archive inside %s: %zu volume(s), %s .. %s (%lld bytes on disk)",
           ctx->job->id, dir, gcount, paths[0], paths[gcount - 1], on_disk_total);

  nested_err[0] = 0;
  if (extract_one_archive(paths, dir, ctx->job, ctx->pg, nested_err, sizeof nested_err) == 0) {
    for (pi = 0; pi < gcount; pi++) {
      if (remove(paths[pi]) != 0) {
        log_warn("[%s] extract: nested scan: remove(%s): %s", ctx->job->id, paths[pi], strerror(errno));
      }
    }
  } else {
    log_warn("[%s] extract: nested archive inside %s failed: %s", ctx->job->id, dir, nested_err);
    if (!ctx->failed) {
      ctx->failed = 1;
      snprintf(ctx->err, sizeof ctx->err, "nested archive inside %s failed: %s", dir, nested_err);
    }
  }
}

extract_result_t
extract_job(const job_t *job, const char *src_dir, const char *dest_dir,
            extract_progress_cb progress_cb, void *progress_ctx,
            char *err, size_t err_size) {
  member_t *members = NULL, *group_buf = NULL;
  int *grouped = NULL;
  size_t member_count = 0;
  size_t fi;
  int found_any = 0;
  int dir_ready = 0;
  extract_result_t result = EXTRACT_NONE;
  progress_t pg = {0};

  pg.cb = progress_cb;
  pg.cb_ctx = progress_ctx;

  if (job->file_count == 0) return EXTRACT_NONE;

  if (!(members = malloc(job->file_count * sizeof *members)) ||
      !(group_buf = malloc(job->file_count * sizeof *group_buf)) ||
      !(grouped = calloc(job->file_count, sizeof *grouped))) {
    snprintf(err, err_size, "out of memory");
    result = EXTRACT_FAILED;
    goto done;
  }

  for (fi = 0; fi < job->file_count; fi++) {
    archive_id_t c;

    if (!classify(job->files[fi].filename, &c)) continue;

    members[member_count].kind = c.kind;
    snprintf(members[member_count].base, sizeof members[member_count].base, "%s", c.base);
    members[member_count].volume = c.volume;
    snprintf(members[member_count].filename, sizeof members[member_count].filename,
             "%s", job->files[fi].filename);
    member_count++;
  }

  for (fi = 0; fi < member_count; fi++) {
    size_t gj, gcount = 0;
    const char **paths;
    size_t pi;

    if (grouped[fi]) continue;

    for (gj = fi; gj < member_count; gj++) {
      if (members[gj].kind == members[fi].kind && !strcmp(members[gj].base, members[fi].base)) {
        group_buf[gcount++] = members[gj];
        grouped[gj] = 1;
      }
    }

    qsort(group_buf, gcount, sizeof *group_buf, cmp_member_volume);

    found_any = 1;

    if (!dir_ready) {
      if (mkdir_p(dest_dir, 0755) < 0) {
        snprintf(err, err_size, "could not create extraction directory %s", dest_dir);
        result = EXTRACT_FAILED;
        goto done;
      }
      dir_ready = 1;
    }

    if (!(paths = calloc(gcount + 1, sizeof *paths))) {
      snprintf(err, err_size, "out of memory");
      result = EXTRACT_FAILED;
      goto done;
    }

    for (pi = 0; pi < gcount; pi++) {
      char safe_name[512];
      char *path = malloc(900);

      if (!path) {
        for (; pi > 0; pi--) free((void *)paths[pi - 1]);
        free(paths);
        snprintf(err, err_size, "out of memory");
        result = EXTRACT_FAILED;
        goto done;
      }

      snprintf(safe_name, sizeof safe_name, "%s", group_buf[pi].filename);
      path_sanitize_component(safe_name, sizeof safe_name);
      snprintf(path, 900, "%s/%s", src_dir, safe_name);
      paths[pi] = path;
    }

    /* Real decompressed size of this set (headers, not on-disk volume
     * size) -- same basis nested extraction uses, so bytes_done never
     * outruns bytes_total, and a good-faith free-space check below. */
    {
      long long group_bytes = peek_archive_total_bytes(paths, job);
      long long free_bytes;

      if (group_bytes < 0) {
        struct stat st;
        group_bytes = 0;
        for (pi = 0; pi < gcount; pi++) {
          if (stat(paths[pi], &st) == 0) group_bytes += (long long)st.st_size;
        }
      }
      pg.total += group_bytes;
      if (pg.cb) pg.cb(pg.cb_ctx, pg.done, pg.total);

      free_bytes = path_free_bytes(dest_dir);
      log_info("[%s] extract: space check on output dir %s: %lld bytes needed, %lld available",
               job->id, dest_dir, group_bytes, free_bytes);
      if (free_bytes >= 0 && free_bytes < group_bytes) {
        snprintf(err, err_size,
                 "insufficient disk space to extract %s: %lld bytes needed, %lld available",
                 group_buf[0].filename, group_bytes, free_bytes);
        for (pi = 0; pi < gcount; pi++) free((void *)paths[pi]);
        free(paths);
        result = EXTRACT_FAILED;
        goto done;
      }
    }

    log_info("[%s] extract: extracting %zu volume(s) starting with %s -> %s",
             job->id, gcount, group_buf[0].filename, dest_dir);

    {
      int rc = extract_one_archive(paths, dest_dir, job, &pg, err, err_size);

      for (pi = 0; pi < gcount; pi++) free((void *)paths[pi]);
      free(paths);

      if (rc < 0) {
        result = EXTRACT_FAILED;
        goto done;
      }
    }
  }

  /* Look for a nested rar-within-rar layer inside what was just extracted.
   * Two walks: size_group_fn first computes the real uncompressed total via
   * headers (on-disk size would undercount a genuinely compressed nested
   * archive), then extract_group_fn runs the real extraction against a
   * fresh progress_t -- deliberately restarting the progress bar at 0% for
   * this second, separate unpacking step. A failure in that second walk
   * (including running out of space) fails the whole job -- see
   * nested_extract_ctx_t. */
  if (found_any) {
    nested_size_ctx_t size_ctx = { job, 0 };

    walk_nested_archives(dest_dir, NESTED_EXTRACT_MAX_DEPTH, job->id, size_group_fn, &size_ctx);

    if (size_ctx.total > 0) {
      long long free_bytes = path_free_bytes(dest_dir);

      log_info("[%s] extract: space check on output dir %s for nested archive: %lld bytes needed, %lld available",
               job->id, dest_dir, size_ctx.total, free_bytes);

      if (free_bytes >= 0 && free_bytes < size_ctx.total) {
        snprintf(err, err_size,
                 "insufficient disk space to extract nested archive: %lld bytes needed, %lld available",
                 size_ctx.total, free_bytes);
        result = EXTRACT_FAILED;
        goto done;
      }

      {
        progress_t nested_pg = {0};
        nested_extract_ctx_t extract_ctx = {0};

        nested_pg.total = size_ctx.total;
        nested_pg.cb = pg.cb;
        nested_pg.cb_ctx = pg.cb_ctx;

        if (nested_pg.cb) nested_pg.cb(nested_pg.cb_ctx, 0, nested_pg.total);

        extract_ctx.job = job;
        extract_ctx.pg = &nested_pg;
        walk_nested_archives(dest_dir, NESTED_EXTRACT_MAX_DEPTH, job->id, extract_group_fn, &extract_ctx);

        if (extract_ctx.failed) {
          snprintf(err, err_size, "%s", extract_ctx.err);
          result = EXTRACT_FAILED;
          goto done;
        }
      }
    }
  }

  result = found_any ? EXTRACT_OK : EXTRACT_NONE;

done:
  free(members);
  free(group_buf);
  free(grouped);
  return result;
}
