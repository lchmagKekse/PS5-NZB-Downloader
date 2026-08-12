/* Leveled logger for the NZB downloader. Every line goes to stdout (visible
 * over the deploy connection) and to a plain logfile on disk (path given to
 * log_init(), truncated fresh each startup) -- see log_get_path() and the
 * web API's GET /api/logs/download, which streams that file. Thread-safe:
 * callers may log concurrently from NNTP worker threads and the HTTP
 * server thread(s).
 *
 * Never pass secrets (NNTP passwords, auth tokens) to these functions.
 */
#pragma once

#include <stddef.h>

typedef enum {
  LOG_ERROR = 0,
  LOG_WARN  = 1,
  LOG_INFO  = 2,
  LOG_DEBUG = 3
} log_level_t;

/* Sets the process/thread name, stdio buffering, and initial log level, and
 * (re)creates log_path, truncating any existing file. Call once from main()
 * before spawning threads. If log_path can't be opened, falls back to
 * stdout only (reported via stderr) rather than treating it as fatal. */
void log_init(const char *thread_name, log_level_t level, const char *log_path);

/* Names the calling thread only; does not touch the process-wide log
 * level or stdio buffering. Safe to call from any worker thread spawned
 * after log_init(). */
void log_set_thread_name(const char *name);

void log_set_level(log_level_t level);
log_level_t log_get_level(void);

void log_log(log_level_t level, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));

/* The path passed to log_init(), for GET /api/logs/download to serve.
 * Empty string if log_init() hasn't been called or its file failed to
 * open. */
const char *log_get_path(void);

/* Calls cb once per buffered recent log line (oldest first), for the web
 * API's GET /api/logs. Holds the last LOG_RING_CAPACITY lines regardless of
 * log_set_level(), capped unlike the logfile. cb runs under the log mutex
 * -- must not call back into log_log()/log_error()/etc. or it deadlocks. */
#define LOG_RING_CAPACITY 500
typedef void (*log_line_cb)(void *ctx, const char *line);
void log_for_each_recent(log_line_cb cb, void *ctx);

#define log_error(...) log_log(LOG_ERROR, __VA_ARGS__)
#define log_warn(...)  log_log(LOG_WARN,  __VA_ARGS__)
#define log_info(...)  log_log(LOG_INFO,  __VA_ARGS__)
#define log_debug(...) log_log(LOG_DEBUG, __VA_ARGS__)
