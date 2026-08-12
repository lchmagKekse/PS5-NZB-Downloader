/* Bounded pool of NNTP connections. Owns a fixed number of worker threads
 * (nntp_pool_opts_t.max_connections), each holding one nntp_conn_t that is
 * connected/authenticated lazily and kept alive across fetches. A
 * transport failure reconnects and retries the fetch up to retry_count
 * times before giving up; it never grows the pool beyond max_connections,
 * per the "don't create an excessive number of connections" constraint.
 */
#pragma once

#include "nntp_conn.h"

typedef struct nntp_pool nntp_pool_t;

/* conn.host/conn.port, like user/pass, are copied by value into pool-owned
 * storage inside nntp_pool_create() -- the caller's strings only need to
 * stay alive for the duration of that call, not the pool's lifetime. */
typedef struct {
  nntp_conn_opts_t conn;         /* host/port/tls/timeouts */
  char user[128];                /* empty user skips AUTHINFO entirely */
  char pass[128];
  int  max_connections;          /* >= 1 */
  int  retry_count;              /* reconnect+retry attempts before giving up */
} nntp_pool_opts_t;

/* Status values passed to nntp_pool_done_cb in addition to the normal
 * NNTP status codes (220/222/223/4xx/5xx) that nntp_conn_fetch() returns. */
#define NNTP_POOL_ERR_TRANSPORT (-1)   /* connect/auth/fetch failed retry_count times */
#define NNTP_POOL_ERR_CANCELLED (-2)   /* dropped by nntp_pool_cancel_all() before running */

/* Invoked once, from a worker thread, when a fetch finishes (successfully,
 * with an NNTP error code, after exhausting retries, or cancelled). */
typedef void (*nntp_pool_done_cb)(void *ctx, int status);

nntp_pool_t *nntp_pool_create(const nntp_pool_opts_t *opts);

/* Enqueues an article fetch. cmd is "ARTICLE", "BODY", or "STAT". body_cb
 * (may be NULL for STAT) is called per body line from a worker thread, same
 * contract as nntp_conn_fetch(). Returns 0 on success, -1 if the pool is
 * shutting down. */
int nntp_pool_fetch(nntp_pool_t *pool, const char *cmd, const char *message_id,
                     nntp_body_line_cb body_cb, nntp_pool_done_cb done_cb, void *ctx);

/* Number of worker threads currently mid-fetch (connecting, authenticating,
 * or transferring), for dashboard "active connections" reporting. Between
 * 0 and opts.max_connections inclusive. */
int nntp_pool_active_count(nntp_pool_t *pool);

/* Tells every idle worker to close its connection now rather than wait for
 * CONN_IDLE_MAX_SECS. Call once the queue has run dry so the pool holds no
 * open sockets between jobs; connections reopen lazily on next use. Workers
 * mid-fetch are left alone. */
void nntp_pool_close_idle(nntp_pool_t *pool);

/* Drops every queued-but-not-yet-started job, invoking each done_cb with
 * NNTP_POOL_ERR_CANCELLED so callers can release per-job resources. Jobs
 * already in flight on a worker thread run to completion. */
void nntp_pool_cancel_all(nntp_pool_t *pool);

/* Signals shutdown, cancels queued jobs (see nntp_pool_cancel_all), waits
 * for in-flight jobs to finish, joins all worker threads, closes every
 * connection, and frees the pool. */
void nntp_pool_destroy(nntp_pool_t *pool);
