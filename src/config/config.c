#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../log/log.h"
#include "config.h"

static void
trim(char *s) {
  char *start = s;
  size_t len;

  while (*start && isspace((unsigned char)*start)) start++;
  len = strlen(start);
  while (len > 0 && isspace((unsigned char)start[len - 1])) len--;

  memmove(s, start, len);
  s[len] = 0;
}

void
app_config_set_defaults(app_config_t *cfg) {
  memset(cfg, 0, sizeof *cfg);

  cfg->nntp.tls               = 0;
  cfg->nntp.max_connections   = 4;

  /* Fixed, not user-configurable -- tuning these turned out to be a footgun. */
  cfg->nntp.connect_timeout_sec = 60;
  cfg->nntp.read_timeout_sec    = 15;
  cfg->nntp.retry_count         = 5;

  cfg->queue.max_retries        = 3;

  /* temp_dir lives on /data (internal, always present): in-progress downloads
   * are app working state, not the large final media output_dir holds, which
   * stays on external storage /mnt/usb0. */
  snprintf(cfg->storage.output_dir, sizeof cfg->storage.output_dir, "%s", "/data/games");
  snprintf(cfg->storage.temp_dir,   sizeof cfg->storage.temp_dir,   "%s", "/data/nzb/incomplete");
}

static void
apply_kv(app_config_t *cfg, const char *key, const char *val) {
  if      (!strcmp(key, "host")) snprintf(cfg->nntp.host, sizeof cfg->nntp.host, "%s", val);
  else if (!strcmp(key, "port")) snprintf(cfg->nntp.port, sizeof cfg->nntp.port, "%s", val);
  else if (!strcmp(key, "ssl") || !strcmp(key, "tls")) cfg->nntp.tls = atoi(val);
  else if (!strcmp(key, "user")) snprintf(cfg->nntp.user, sizeof cfg->nntp.user, "%s", val);
  else if (!strcmp(key, "pass")) snprintf(cfg->nntp.pass, sizeof cfg->nntp.pass, "%s", val);
  else if (!strcmp(key, "max_connections")) cfg->nntp.max_connections = atoi(val);
  /* connect_timeout/read_timeout/retry_count are fixed in
   * app_config_set_defaults(), not settable here. */
  else if (!strcmp(key, "output_dir"))  snprintf(cfg->storage.output_dir,  sizeof cfg->storage.output_dir,  "%s", val);
  else if (!strcmp(key, "temp_dir"))    snprintf(cfg->storage.temp_dir,    sizeof cfg->storage.temp_dir,    "%s", val);
  else if (!strcmp(key, "max_retries")) cfg->queue.max_retries = atoi(val);
  else log_warn("config: ignoring unknown key '%s'", key);
}

int
app_config_load(const char *path, app_config_t *cfg) {
  char line[1024];
  FILE *f;

  app_config_set_defaults(cfg);

  if (!(f = fopen(path, "r"))) {
    log_error("config: fopen(%s): %s", path, strerror(errno));
    return -1;
  }

  while (fgets(line, sizeof line, f)) {
    char *key, *val, *eq;

    trim(line);
    if (line[0] == 0 || line[0] == '#') continue;

    if (!(eq = strchr(line, '='))) continue;
    *eq = 0;
    key = line;
    val = eq + 1;
    trim(key);
    trim(val);

    apply_kv(cfg, key, val);
  }
  fclose(f);

  if (cfg->nntp.host[0] == 0) {
    log_error("config %s is missing 'host'", path);
    return -1;
  }
  if (cfg->nntp.port[0] == 0) {
    snprintf(cfg->nntp.port, sizeof cfg->nntp.port, "%s", cfg->nntp.tls ? "563" : "119");
  }
  if (cfg->nntp.max_connections < 1) cfg->nntp.max_connections = 1;
  if (cfg->queue.max_retries < 0) cfg->queue.max_retries = 0;

  return 0;
}

int
app_config_save(const char *path, const app_config_t *cfg) {
  FILE *f;

  if (!(f = fopen(path, "w"))) {
    log_error("config: fopen(%s) for write: %s", path, strerror(errno));
    return -1;
  }

  fprintf(f, "host=%s\n", cfg->nntp.host);
  fprintf(f, "port=%s\n", cfg->nntp.port);
  fprintf(f, "ssl=%d\n", cfg->nntp.tls);
  fprintf(f, "user=%s\n", cfg->nntp.user);
  fprintf(f, "pass=%s\n", cfg->nntp.pass);
  fprintf(f, "max_connections=%d\n", cfg->nntp.max_connections);
  fprintf(f, "output_dir=%s\n", cfg->storage.output_dir);
  fprintf(f, "temp_dir=%s\n", cfg->storage.temp_dir);
  fprintf(f, "max_retries=%d\n", cfg->queue.max_retries);

  fclose(f);
  return 0;
}

app_config_t
app_config_redacted(const app_config_t *cfg) {
  app_config_t out = *cfg;
  out.nntp.pass[0] = 0;
  return out;
}
