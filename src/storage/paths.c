#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include "../log/log.h"
#include "paths.h"

void
path_sanitize_component(char *name, size_t size) {
  char *p;

  (void)size;

  for (p = name; *p; p++) {
    if (*p == '/' || *p == '\\') *p = '_';
  }
  if (name[0] == 0 || !strcmp(name, ".") || !strcmp(name, "..")) {
    snprintf(name, size, "unnamed");
  }
}

void
path_join(char *out, size_t out_size, const char *dir, const char *component) {
  snprintf(out, out_size, "%s/%s", dir, component);
}

int
mkdir_p(const char *path, mode_t mode) {
  char tmp[768];
  char *p;

  snprintf(tmp, sizeof tmp, "%s", path);

  for (p = tmp + 1; *p; p++) {
    if (*p != '/') continue;

    *p = 0;
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
      log_error("mkdir_p: mkdir(%s): %s", tmp, strerror(errno));
      return -1;
    }
    *p = '/';
  }

  if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
    log_error("mkdir_p: mkdir(%s): %s", tmp, strerror(errno));
    return -1;
  }

  return 0;
}

long long
path_dir_total_bytes(const char *path) {
  DIR *d = opendir(path);
  struct dirent *ent;
  long long total = 0;

  if (!d) {
    if (errno != ENOENT) {
      log_warn("path_dir_total_bytes: opendir(%s): %s", path, strerror(errno));
    }
    return 0;
  }

  while ((ent = readdir(d))) {
    char child[900];
    struct stat st;

    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;

    snprintf(child, sizeof child, "%s/%s", path, ent->d_name);

    if (stat(child, &st) != 0) continue;

    if (S_ISDIR(st.st_mode)) {
      total += path_dir_total_bytes(child);
    } else if (S_ISREG(st.st_mode)) {
      total += (long long)st.st_size;
    }
  }
  closedir(d);

  return total;
}

long long
path_free_bytes(const char *path) {
  char probe[900];
  struct statfs sfs;
  int attempts, last_errno = 0;

  snprintf(probe, sizeof probe, "%s", path);

  /* path (e.g. a not-yet-created per-job temp dir) may not exist yet --
   * walk up to the nearest existing ancestor. Walks up on ANY statfs()
   * failure, not just ENOENT: on this platform statfs() on a nonexistent
   * path doesn't reliably set errno to ENOENT (seen EPIPE, even 0). */
  for (attempts = 0; attempts < 8; attempts++) {
    if (statfs(probe, &sfs) == 0) {
      return (long long)sfs.f_bavail * (long long)sfs.f_bsize;
    }
    last_errno = errno;

    {
      char *slash = strrchr(probe, '/');
      if (!slash || slash == probe) break;
      *slash = 0;
    }
  }

  log_error("path_free_bytes(%s): %s", path, strerror(last_errno));
  return -1;
}

int
rmdir_recursive(const char *path) {
  DIR *d = opendir(path);
  struct dirent *ent;

  if (!d) {
    if (errno == ENOENT) return 0;
    log_error("rmdir_recursive: opendir(%s): %s", path, strerror(errno));
    return -1;
  }

  while ((ent = readdir(d))) {
    char child[900];
    struct stat st;

    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;

    snprintf(child, sizeof child, "%s/%s", path, ent->d_name);

    if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
      rmdir_recursive(child);
    } else if (remove(child) != 0) {
      log_warn("rmdir_recursive: remove(%s): %s", child, strerror(errno));
    }
  }
  closedir(d);

  if (rmdir(path) != 0) {
    log_error("rmdir_recursive: rmdir(%s): %s", path, strerror(errno));
    return -1;
  }

  return 0;
}
