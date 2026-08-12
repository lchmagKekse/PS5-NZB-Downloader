#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "../log/log.h"
#include "paths.h"
#include "shadowmount.h"

#define MANUAL_LST_DIR  "/data/shadowmount"
#define MANUAL_LST_PATH "/data/shadowmount/manual.lst"

static int
has_suffix_ci(const char *s, const char *suffix) {
  size_t slen = strlen(s), suflen = strlen(suffix);
  return slen >= suflen && !strcasecmp(s + slen - suflen, suffix);
}

static int
is_shadowmount_image(const char *name) {
  static const char *const exts[] = { ".ffpkg", ".exfat", ".ffpfs", ".ffpfsc", NULL };
  int i;

  for (i = 0; exts[i]; i++) {
    if (has_suffix_ci(name, exts[i])) return 1;
  }
  return 0;
}

static int
has_param_json(const char *dir) {
  char path[1200];
  struct stat st;

  snprintf(path, sizeof path, "%s/sce_sys/param.json", dir);
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Depth-first, files before subdirectories, so the shallowest match under
 * the original dest_dir wins. Returns 1 and fills out_path on a match. */
static int
find_shadowmount_target(const char *dir, char *out_path, size_t out_size) {
  DIR *d;
  struct dirent *ent;
  char child[1200];
  struct stat st;
  int found = 0;

  if (has_param_json(dir)) {
    snprintf(out_path, out_size, "%s", dir);
    return 1;
  }

  if (!(d = opendir(dir))) {
    log_warn("shadowmount: opendir(%s): %s", dir, strerror(errno));
    return 0;
  }

  while ((ent = readdir(d))) {
    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;

    snprintf(child, sizeof child, "%s/%s", dir, ent->d_name);
    if (stat(child, &st) != 0) continue;

    if (S_ISREG(st.st_mode) && is_shadowmount_image(ent->d_name)) {
      snprintf(out_path, out_size, "%s", child);
      found = 1;
      break;
    }
  }
  closedir(d);
  if (found) return 1;

  if (!(d = opendir(dir))) return 0;

  while ((ent = readdir(d))) {
    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;

    snprintf(child, sizeof child, "%s/%s", dir, ent->d_name);
    if (stat(child, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

    if (find_shadowmount_target(child, out_path, out_size)) {
      found = 1;
      break;
    }
  }
  closedir(d);

  return found;
}

void
shadowmount_register(const char *job_id, const char *dest_dir) {
  char match[1200];
  FILE *f;

  if (!find_shadowmount_target(dest_dir, match, sizeof match)) {
    log_info("[%s] shadowmount: no sce_sys/param.json or .ffpkg/.exfat/.ffpfs/.ffpfsc "
             "found under %s, not added to " MANUAL_LST_PATH, job_id, dest_dir);
    return;
  }

  if (mkdir_p(MANUAL_LST_DIR, 0755) < 0) {
    log_error("[%s] shadowmount: could not create %s", job_id, MANUAL_LST_DIR);
    return;
  }

  if (!(f = fopen(MANUAL_LST_PATH, "a"))) {
    log_error("[%s] shadowmount: fopen(%s): %s", job_id, MANUAL_LST_PATH, strerror(errno));
    return;
  }
  if (fprintf(f, "%s\n", match) < 0) {
    log_error("[%s] shadowmount: write error appending to %s", job_id, MANUAL_LST_PATH);
  }
  fclose(f);

  log_info("[%s] shadowmount: added %s to %s", job_id, match, MANUAL_LST_PATH);
}
