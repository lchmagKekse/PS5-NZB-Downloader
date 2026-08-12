#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../log/log.h"
#include "nntp_pool.h"

/* A connection idle longer than this is proactively closed and reopened,
 * since the provider silently drops idle connections after some timeout.
 * Backstop for the case nntp_pool_close_idle() doesn't cover: a worker whose
 * connection sits unused mid-job while the queue itself isn't idle. 90s is a
 * guess; erring conservative just costs one extra handshake. */
#define CONN_IDLE_MAX_SECS 90

typedef struct job {
  char                cmd[16];
  char               *message_id;   /* owned, strdup'd */
  nntp_body_line_cb   body_cb;
  nntp_pool_done_cb   done_cb;
  void               *ctx;
  struct job         *next;
} job_t;

struct nntp_pool {
  nntp_pool_opts_t opts;
  char             host_buf[256]; /* opts.conn.host is repointed here -- see nntp_pool_create() */
  char             port_buf[8];   /* opts.conn.port is repointed here */

  pthread_mutex_t mu;
  pthread_cond_t  not_empty;
  job_t          *head;
  job_t          *tail;
  int             shutdown;

  pthread_t *workers;
  int        n_workers;
  int        active; /* workers currently mid-fetch; guarded by mu */

  /* Bumped by nntp_pool_close_idle() and broadcast on not_empty so every
   * idle worker wakes up, notices its own seen_close_gen is behind, and
   * drops its connection. See worker_main(). */
  int        close_gen;
};

static job_t *
job_create(const char *cmd, const char *message_id,
           nntp_body_line_cb body_cb, nntp_pool_done_cb done_cb, void *ctx) {
  job_t *j = calloc(1, sizeof *j);
  if (!j) return NULL;

  snprintf(j->cmd, sizeof j->cmd, "%s", cmd);
  j->message_id = strdup(message_id);
  j->body_cb = body_cb;
  j->done_cb = done_cb;
  j->ctx = ctx;

  if (!j->message_id) { free(j); return NULL; }
  return j;
}

static void
job_free(job_t *j) {
  if (!j) return;
  free(j->message_id);
  free(j);
}

/* Ensures *conn is connected and authenticated, (re)connecting if needed.
 * Returns 0 on success, -1 on failure (already logged). */
static int
ensure_connected(nntp_pool_t *pool, nntp_conn_t **conn) {
  if (*conn) return 0;

  *conn = nntp_conn_open(&pool->opts.conn, NULL, 0);
  if (!*conn) return -1;

  if (pool->opts.user[0]) {
    if (nntp_conn_authenticate(*conn, pool->opts.user, pool->opts.pass) < 0) {
      nntp_conn_close(*conn);
      *conn = NULL;
      return -1;
    }
  }

  return 0;
}

static int
run_job(nntp_pool_t *pool, nntp_conn_t **conn, job_t *j) {
  int attempt, status = NNTP_POOL_ERR_TRANSPORT;

  for (attempt = 0; attempt <= pool->opts.retry_count; attempt++) {
    if (ensure_connected(pool, conn) < 0) {
      status = NNTP_POOL_ERR_TRANSPORT;
      continue; /* ensure_connected already logged; try again next attempt */
    }

    status = nntp_conn_fetch(*conn, j->cmd, j->message_id, j->body_cb, j->ctx);

    /* Retry NNTP-level errors too, not just transport failures: 430 "no such
     * article" can be transient (propagation lag between backend peers)
     * rather than a truly missing article. */
    if (status >= 200 && status < 400) return status;

    if (status < 0) {
      log_warn("nntp_pool: transport error on %s %s (attempt %d/%d), reconnecting",
               j->cmd, j->message_id, attempt + 1, pool->opts.retry_count + 1);
      nntp_conn_close(*conn);
      *conn = NULL;
    } else if (attempt < pool->opts.retry_count) {
      log_warn("nntp_pool: %s %s -> %d (attempt %d/%d), retrying",
               j->cmd, j->message_id, status, attempt + 1, pool->opts.retry_count + 1);
    }
  }

  return status; /* last status seen -- a real NNTP error code, or NNTP_POOL_ERR_TRANSPORT */
}

static void *
worker_main(void *arg) {
  nntp_pool_t *pool = arg;
  nntp_conn_t *conn = NULL;
  time_t last_used = 0;
  int seen_close_gen = 0;

  log_set_thread_name("nntp-worker");

  for (;;) {
    job_t *j;

    pthread_mutex_lock(&pool->mu);
    for (;;) {
      /* Queue's still empty and someone asked idle connections to close
       * (nntp_pool_close_idle()) since we last checked -- drop ours, then
       * go back to waiting. Done here rather than after cond_wait so this
       * also fires immediately on the first pass, before ever blocking. */
      if (conn && seen_close_gen != pool->close_gen) {
        seen_close_gen = pool->close_gen;
        pthread_mutex_unlock(&pool->mu);
        log_debug("nntp_pool: queue is empty, closing idle connection");
        nntp_conn_close(conn);
        conn = NULL;
        pthread_mutex_lock(&pool->mu);
        continue;
      }
      seen_close_gen = pool->close_gen;

      if (pool->head || pool->shutdown) break;
      pthread_cond_wait(&pool->not_empty, &pool->mu);
    }
    if (!pool->head && pool->shutdown) {
      pthread_mutex_unlock(&pool->mu);
      break;
    }
    j = pool->head;
    pool->head = j->next;
    if (!pool->head) pool->tail = NULL;
    pool->active++;
    pthread_mutex_unlock(&pool->mu);

    if (conn && last_used && (time(NULL) - last_used) > CONN_IDLE_MAX_SECS) {
      log_debug("nntp_pool: connection idle over %ds, reconnecting proactively", CONN_IDLE_MAX_SECS);
      nntp_conn_close(conn);
      conn = NULL;
    }

    int status = run_job(pool, &conn, j);
    last_used = time(NULL);
    if (j->done_cb) j->done_cb(j->ctx, status);
    job_free(j);

    pthread_mutex_lock(&pool->mu);
    pool->active--;
    pthread_mutex_unlock(&pool->mu);
  }

  nntp_conn_close(conn);
  return NULL;
}

/* Default pthread stack size on this platform is too small: worker threads
 * crashed with a stack-overflow SIGSEGV before picking up any job. */
#define WORKER_STACK_SIZE (1024 * 1024)

nntp_pool_t *
nntp_pool_create(const nntp_pool_opts_t *opts) {
  nntp_pool_t *pool = calloc(1, sizeof *pool);
  pthread_attr_t attr;
  int i;

  if (!pool) return NULL;

  pool->opts = *opts;
  if (pool->opts.max_connections < 1) pool->opts.max_connections = 1;

  snprintf(pool->host_buf, sizeof pool->host_buf, "%s", opts->conn.host ? opts->conn.host : "");
  snprintf(pool->port_buf, sizeof pool->port_buf, "%s", opts->conn.port ? opts->conn.port : "");
  pool->opts.conn.host = pool->host_buf;
  pool->opts.conn.port = pool->port_buf;

  pthread_mutex_init(&pool->mu, NULL);
  pthread_cond_init(&pool->not_empty, NULL);

  pool->n_workers = pool->opts.max_connections;
  pool->workers = calloc((size_t)pool->n_workers, sizeof *pool->workers);
  if (!pool->workers) {
    free(pool);
    return NULL;
  }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, WORKER_STACK_SIZE);

  for (i = 0; i < pool->n_workers; i++) {
    if (pthread_create(&pool->workers[i], &attr, worker_main, pool) != 0) {
      log_error("nntp_pool: pthread_create failed for worker %d/%d", i + 1, pool->n_workers);
      pool->n_workers = i; /* only join the ones that actually started */
      break;
    }
  }

  pthread_attr_destroy(&attr);

  log_info("nntp_pool: started %d connection(s) to %s:%s (%s)",
           pool->n_workers, opts->conn.host, opts->conn.port,
           opts->conn.tls ? "TLS" : "plain");

  return pool;
}

int
nntp_pool_fetch(nntp_pool_t *pool, const char *cmd, const char *message_id,
                nntp_body_line_cb body_cb, nntp_pool_done_cb done_cb, void *ctx) {
  job_t *j;

  pthread_mutex_lock(&pool->mu);
  if (pool->shutdown) {
    pthread_mutex_unlock(&pool->mu);
    return -1;
  }
  pthread_mutex_unlock(&pool->mu);

  if (!(j = job_create(cmd, message_id, body_cb, done_cb, ctx))) return -1;

  pthread_mutex_lock(&pool->mu);
  if (pool->shutdown) {
    pthread_mutex_unlock(&pool->mu);
    job_free(j);
    return -1;
  }
  if (pool->tail) pool->tail->next = j; else pool->head = j;
  pool->tail = j;
  pthread_cond_signal(&pool->not_empty);
  pthread_mutex_unlock(&pool->mu);

  return 0;
}

int
nntp_pool_active_count(nntp_pool_t *pool) {
  int n;

  pthread_mutex_lock(&pool->mu);
  n = pool->active;
  pthread_mutex_unlock(&pool->mu);

  return n;
}

void
nntp_pool_close_idle(nntp_pool_t *pool) {
  pthread_mutex_lock(&pool->mu);
  pool->close_gen++;
  pthread_cond_broadcast(&pool->not_empty);
  pthread_mutex_unlock(&pool->mu);
}

void
nntp_pool_cancel_all(nntp_pool_t *pool) {
  job_t *j;

  pthread_mutex_lock(&pool->mu);
  j = pool->head;
  pool->head = pool->tail = NULL;
  pthread_mutex_unlock(&pool->mu);

  while (j) {
    job_t *next = j->next;
    if (j->done_cb) j->done_cb(j->ctx, NNTP_POOL_ERR_CANCELLED);
    job_free(j);
    j = next;
  }
}

void
nntp_pool_destroy(nntp_pool_t *pool) {
  int i;

  if (!pool) return;

  nntp_pool_cancel_all(pool);

  pthread_mutex_lock(&pool->mu);
  pool->shutdown = 1;
  pthread_cond_broadcast(&pool->not_empty);
  pthread_mutex_unlock(&pool->mu);

  for (i = 0; i < pool->n_workers; i++) {
    pthread_join(pool->workers[i], NULL);
  }

  free(pool->workers);
  pthread_mutex_destroy(&pool->mu);
  pthread_cond_destroy(&pool->not_empty);
  free(pool);
}
