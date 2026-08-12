#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "../storage/paths.h"
#include "log.h"

static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
static log_level_t current_level = LOG_INFO;

static FILE *log_fp = NULL;
static char  log_path[512] = "";

/* Matches log_log()'s own `line` buffer size, so a ring copy never
 * truncates a line log_log() itself didn't already truncate. */
#define RING_LINE_MAX 1024
static char   ring[LOG_RING_CAPACITY][RING_LINE_MAX];
static size_t ring_next = 0;   /* next slot to write */
static size_t ring_count = 0;  /* number of valid entries, caps at LOG_RING_CAPACITY */

static const char *
level_prefix(log_level_t level) {
  switch (level) {
    case LOG_ERROR: return "[ERROR]";
    case LOG_WARN:  return "[WARN ]";
    case LOG_INFO:  return "[INFO ]";
    case LOG_DEBUG: return "[DEBUG]";
  }
  return "[?????]";
}

/* Splits off the directory portion of path and mkdir_p()s it, in place in
 * a local copy -- log_init() may run before anything else (queue_open(),
 * app_config_load()) has had a chance to create the app's data directory. */
static void
ensure_parent_dir(const char *path) {
  char dir[512];
  char *slash;

  snprintf(dir, sizeof dir, "%s", path);
  slash = strrchr(dir, '/');
  if (!slash || slash == dir) return;

  *slash = 0;
  mkdir_p(dir, 0755);
}

void
log_init(const char *thread_name, log_level_t level, const char *the_log_path) {
  syscall(SYS_thr_set_name, -1, thread_name);
  setvbuf(stdout, NULL, _IOLBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  current_level = level;

  if (the_log_path && the_log_path[0]) {
    ensure_parent_dir(the_log_path);

    /* "w": truncates a pre-existing file so every startup begins clean. */
    log_fp = fopen(the_log_path, "w");
    if (log_fp) {
      /* _IONBF not _IOLBF: on this SDK's libc, _IOLBF doesn't flush
       * per-line for a regular file (only for a tty), so GET
       * /api/logs/download (which fstat()s via a separate fd) would see a
       * stale size. Costs a write() syscall per line, fine for a logger. */
      setvbuf(log_fp, NULL, _IONBF, 0);
      snprintf(log_path, sizeof log_path, "%s", the_log_path);
    } else {
      fprintf(stderr, "log_init: could not open %s for writing, logging to stdout only\n", the_log_path);
    }
  }
}

void
log_set_thread_name(const char *name) {
  syscall(SYS_thr_set_name, -1, name);
}

void
log_set_level(log_level_t level) {
  current_level = level;
}

log_level_t
log_get_level(void) {
  return current_level;
}

const char *
log_get_path(void) {
  return log_path;
}

void
log_log(log_level_t level, const char *fmt, ...) {
  char msg[896];
  char line[1024];
  char ts[32];
  time_t now;
  struct tm tmv;
  va_list ap;

  if (level > current_level) return;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);

  now = time(NULL);
  localtime_r(&now, &tmv);
  strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);

  snprintf(line, sizeof line, "%s %s %s", ts, level_prefix(level), msg);

  pthread_mutex_lock(&log_mu);
  puts(line);
  if (log_fp) {
    fputs(line, log_fp);
    fputc('\n', log_fp);
  }

  snprintf(ring[ring_next], RING_LINE_MAX, "%s", line);
  ring_next = (ring_next + 1) % LOG_RING_CAPACITY;
  if (ring_count < LOG_RING_CAPACITY) ring_count++;

  pthread_mutex_unlock(&log_mu);
}

void
log_for_each_recent(log_line_cb cb, void *ctx) {
  size_t i, start, n;

  pthread_mutex_lock(&log_mu);
  n = ring_count;
  start = (ring_count < LOG_RING_CAPACITY) ? 0 : ring_next;

  for (i = 0; i < n; i++) {
    cb(ctx, ring[(start + i) % LOG_RING_CAPACITY]);
  }
  pthread_mutex_unlock(&log_mu);
}
