/* Jobs download one at a time (the NNTP pool is one shared, bounded
 * resource -- interleaving jobs through it wouldn't add throughput), but
 * extraction is pure disk/CPU work, so download_job() hands each finished
 * job to a FIFO (finalize_queue_t) drained by a second thread
 * (finalizer_main()), pipelining extraction with the next job's download.
 *
 * Each job_file_t is one shared file, named after the NZB's own filename.
 * Each segment yEnc-decodes itself and pwrite()s directly at the decoded
 * byte offset from its article's "=ypart begin=/end=" line (0 for a
 * single-part article) -- exact placement, no assembly pass. A segment
 * that fails CRC or yEnc framing is left unmarked for nntp_pool's retry. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../extract/extract.h"
#include "../log/log.h"
#include "../nntp/nntp_conn.h"
#include "../par2/par2.h"
#include "../storage/paths.h"
#include "../storage/shadowmount.h"
#include "../util/crc32.h"
#include "../web/app_state.h"
#include "../yenc/yenc.h"
#include "download.h"

#define DL_CHECKPOINT_INTERVAL 25  /* persist job state every N segments completed */

/* FIFO handoff from download_job() to finalizer_main() -- see top comment.
 * shutdown does not discard what's already queued (finalize_dequeue()
 * drains it first), only stops waiting for new work, so a shutdown
 * mid-burst still lets already-downloaded jobs finish extracting. */
typedef struct finalize_req {
  job_t              *job;
  struct finalize_req *next;
} finalize_req_t;

typedef struct {
  pthread_mutex_t mu;
  pthread_cond_t  not_empty;
  finalize_req_t *head, *tail;
  int             shutdown;
} finalize_queue_t;

struct downloader {
  pthread_t        thread;           /* downloader_main() -- download+verify+repair */
  pthread_t        finalize_thread;  /* finalizer_main() -- extract+finalize */
  volatile int     stop;
  finalize_queue_t fq;
};

/* Hands job off to the finalizer thread and returns immediately. */
static void
finalize_enqueue(finalize_queue_t *fq, job_t *job) {
  finalize_req_t *r = calloc(1, sizeof *r);

  if (!r) {
    /* Shouldn't happen (two pointers) -- fail the job outright rather
     * than leave it stuck forever, same as other OOM branches here. */
    log_error("[%s] download: out of memory queueing for extraction", job->id);
    queue_lock();
    snprintf(job->last_error, sizeof job->last_error, "out of memory queueing for extraction");
    job_set_state(job, JOB_FAILED);
    queue_save_job(g_app.queue, job);
    queue_unlock();
    return;
  }
  r->job = job;

  pthread_mutex_lock(&fq->mu);
  r->next = NULL;
  if (fq->tail) fq->tail->next = r; else fq->head = r;
  fq->tail = r;
  pthread_cond_signal(&fq->not_empty);
  pthread_mutex_unlock(&fq->mu);
}

/* Blocks until a job is available, or returns NULL once the queue is
 * both shut down and drained (the finalizer thread's signal to exit). */
static job_t *
finalize_dequeue(finalize_queue_t *fq) {
  finalize_req_t *r;
  job_t *job;

  pthread_mutex_lock(&fq->mu);
  while (!fq->head && !fq->shutdown) {
    pthread_cond_wait(&fq->not_empty, &fq->mu);
  }
  if (!fq->head) {
    pthread_mutex_unlock(&fq->mu);
    return NULL;
  }
  r = fq->head;
  fq->head = r->next;
  if (!fq->head) fq->tail = NULL;
  pthread_mutex_unlock(&fq->mu);

  job = r->job;
  free(r);
  return job;
}

/* Tracks in-flight segment fetches for one job's download_job() call. */
typedef struct {
  pthread_mutex_t mu;
  pthread_cond_t  cond;
  int             pending;
  int             since_checkpoint;
} job_dl_state_t;

/* One per job_file_t, shared by every segment_ctx_t for that file for one
 * download_segments() call. fd opens lazily on the first segment to
 * actually start, not at dispatch time -- opening per-segment caused
 * EMFILE with hundreds/thousands of segments in flight and no
 * backpressure. Closed all together after dl_state_wait() drains
 * everything (see download_segments()), not via a per-file refcount --
 * segments of one file don't finish in dispatch order, so a naive
 * refcount could hit zero and close the fd while more are still coming. */
typedef struct {
  pthread_mutex_t mu;
  int             fd;
  int             open_failed;
} file_dl_t;

#define SEG_WBUF_SIZE 65536

/* Where a segment's article body is in its own yEnc framing, tracked
 * across the whole streamed sequence of body_line_cb() calls for that
 * segment (one call per NNTP body line). See body_line_cb(). */
typedef enum {
  YENC_WANT_BEGIN,        /* haven't seen "=ybegin" yet */
  YENC_WANT_PART_OR_DATA, /* saw =ybegin; next is "=ypart" (multi-part) or the first data line (single-part) */
  YENC_IN_DATA,           /* offset/budget resolved (see resolve_segment_range()), decoding data lines */
  YENC_DONE               /* saw "=yend" -- ignore anything further */
} yenc_state_t;

/* One per in-flight segment fetch. offset/budget are this segment's
 * decoded byte range within its file_dl_t's fd -- unknown until the
 * article's own =ybegin/=ypart lines are parsed, see
 * resolve_segment_range(). */
typedef struct {
  job_dl_state_t  *st;
  job_t           *job;
  job_file_t      *jf;
  job_segment_t   *seg;
  file_dl_t       *fs;

  yenc_state_t     yenc_state;
  long             ybegin_size;      /* =ybegin's size= -- only meaningful for a single-part article (no =ypart) */
  int              have_ybegin_size;
  unsigned long    crc;              /* running CRC32 of decoded bytes so far, see util/crc32.h */
  uint32_t         expected_crc;     /* =yend's pcrc32= (preferred) or crc32= */
  int              have_expected_crc;
  int              yenc_failed;      /* malformed/missing yEnc framing */

  long             offset;       /* this segment's start position in fs->fd -- set by resolve_segment_range() */
  long             budget;       /* decoded byte count this segment is allowed to write -- ditto */
  long             written;      /* bytes actually flushed to fs->fd so far, <= budget */
  int              range_resolved; /* true once offset/budget/fs->fd are resolved -- see resolve_segment_range() */
  int              budget_warned;
  int              write_failed;

  /* Write-coalescing buffer: one pwrite() per SEG_WBUF_SIZE instead of a
   * syscall per ~128-byte yEnc line, which otherwise drops real-hardware
   * throughput from ~30Mbit to ~100Kbit. Allocated lazily once the
   * segment's range resolves (freed in segment_done_cb()) -- a large job
   * dispatches every segment up front, so eager allocation would mean
   * thousands of not-yet-running segments each holding a 64KB buffer. */
  unsigned char   *wbuf;
  size_t           wbuf_len;
} segment_ctx_t;

static void
dl_state_add(job_dl_state_t *st) {
  pthread_mutex_lock(&st->mu);
  st->pending++;
  pthread_mutex_unlock(&st->mu);
}

static void
dl_state_done(job_dl_state_t *st) {
  pthread_mutex_lock(&st->mu);
  st->pending--;
  pthread_cond_signal(&st->cond);
  pthread_mutex_unlock(&st->mu);
}

static void
dl_state_wait(job_dl_state_t *st) {
  pthread_mutex_lock(&st->mu);
  while (st->pending > 0) pthread_cond_wait(&st->cond, &st->mu);
  pthread_mutex_unlock(&st->mu);
}

/* True as long as the job is still meant to be actively working -- false
 * once an API handler has paused/cancelled it out from under us, or the
 * whole payload is shutting down (api_system_eject(), see app_state.h's
 * shutdown_requested). Reads job->state under queue_lock since API handler
 * threads write it concurrently. */
static int
dl_should_continue(const job_t *job) {
  int cont;

  if (app_is_shutting_down()) return 0;

  queue_lock();
  cont = (job->state != JOB_PAUSED && job->state != JOB_CANCELLED);
  queue_unlock();

  return cont;
}

/* True if an API handler has cancelled this job. Unlike dl_should_continue()
 * (checked between segments/files, where dispatch just stops), this is
 * checked from inside verify_progress_update()/repair_progress_update()/
 * extract_progress_update() -- the only safe point during a single blocking
 * par2_verify_job()/par2_repair_job()/extract_job() call to notice a Cancel
 * and abort within one chunk instead of running the whole pass to
 * completion regardless. */
static int
job_was_cancelled(const job_t *job) {
  int cancelled;

  queue_lock();
  cancelled = (job->state == JOB_CANCELLED);
  queue_unlock();

  return cancelled;
}

/* Brackets a blocking par2_verify_job()/par2_repair_job()/finalize_job()
 * call so queue_remove_job() can tell a job that merely *says*
 * JOB_CANCELLED (state flips the instant the API handler runs) from one
 * this thread is still actually dereferencing -- see job.h's busy comment.
 * Must be paired: job_busy_end() before every exit out of the bracketed
 * call, including early returns. */
static void
job_busy_begin(job_t *job) {
  queue_lock();
  job->busy = 1;
  queue_unlock();
}

static void
job_busy_end(job_t *job) {
  queue_lock();
  job->busy = 0;
  queue_unlock();
}

/* storage.temp_dir/<job id>/, computed the same way everywhere it's
 * needed so they can't drift. */
static void
job_temp_dir(const job_t *job, char *out, size_t out_size) {
  config_lock();
  snprintf(out, out_size, "%s/%s", g_app.config.storage.temp_dir, job->id);
  config_unlock();
}

/* storage.temp_dir/<job id>/<sanitized filename> -- the exact path a
 * job_file_t's content lives at, on disk, computed identically wherever
 * it's needed (writing in resolve_segment_range(), reading for PAR2
 * verification in par2_verify_job()) so they can't drift apart. */
static void
job_file_path(const job_t *job, const job_file_t *jf, char *out, size_t out_size) {
  char dir[700], safe_name[512];

  job_temp_dir(job, dir, sizeof dir);
  snprintf(safe_name, sizeof safe_name, "%s", jf->filename);
  path_sanitize_component(safe_name, sizeof safe_name);
  snprintf(out, out_size, "%s/%s", dir, safe_name);
}

/* Flushes sc->wbuf (if non-empty) via one pwrite() at sc->offset+sc->written
 * and resets it. Safe to call with an empty buffer. */
static void
segment_flush(segment_ctx_t *sc) {
  if (sc->wbuf_len == 0) return;

  if (sc->write_failed || sc->fs->open_failed || sc->fs->fd < 0) {
    sc->wbuf_len = 0;
    return;
  }

  if (pwrite(sc->fs->fd, sc->wbuf, sc->wbuf_len, sc->offset + sc->written) != (ssize_t)sc->wbuf_len) {
    log_error("[%s] download: segment %s: pwrite failed: %s",
              sc->job->id, sc->seg->message_id, strerror(errno));
    sc->write_failed = 1;
    sc->wbuf_len = 0;
    return;
  }

  sc->written += (long)sc->wbuf_len;
  app_stats_add_bytes(sc->wbuf_len);
  sc->wbuf_len = 0;
}

/* Buffers decoded data for sc, flushing to disk (segment_flush()) whenever
 * wbuf fills. Clamped to sc->budget so a malformed/lying article's decoded
 * content can't spill into the next segment's slot. */
static void
buffer_append(segment_ctx_t *sc, const void *data, size_t len) {
  const unsigned char *p = data;
  long remaining_budget;

  if (sc->write_failed || sc->fs->open_failed || sc->fs->fd < 0 || !sc->wbuf) return;

  remaining_budget = sc->budget - sc->written - (long)sc->wbuf_len;
  if ((long)len > remaining_budget) {
    if (!sc->budget_warned) {
      log_warn("[%s] download: segment %s: content exceeds its declared %ld-byte budget, truncating",
                sc->job->id, sc->seg->message_id, sc->budget);
      sc->budget_warned = 1;
    }
    len = remaining_budget > 0 ? (size_t)remaining_budget : 0;
  }

  while (len > 0) {
    size_t space = SEG_WBUF_SIZE - sc->wbuf_len;
    size_t take = len < space ? len : space;

    memcpy(sc->wbuf + sc->wbuf_len, p, take);
    sc->wbuf_len += take;
    p += take;
    len -= take;

    if (sc->wbuf_len == SEG_WBUF_SIZE) segment_flush(sc);
  }
}

/* Called once a segment's placement in the final file is known (from
 * "=ypart begin=/end=", or offset 0 for a single-part article). Allocates
 * the write buffer and ensures the shared file is open. Takes fs->mu once
 * per segment, not per line -- same per-chunk lock/syscall overhead
 * nntp_conn.c's read buffer avoids on the read side. */
static void
resolve_segment_range(segment_ctx_t *sc, long offset, long budget) {
  sc->offset = offset;
  sc->budget = budget > 0 ? budget : 0;
  sc->range_resolved = 1;

  if (!(sc->wbuf = malloc(SEG_WBUF_SIZE))) {
    log_error("[%s] download: segment %s: out of memory for write buffer",
              sc->job->id, sc->seg->message_id);
    sc->write_failed = 1;
    return;
  }

  pthread_mutex_lock(&sc->fs->mu);
  if (sc->fs->fd < 0 && !sc->fs->open_failed) {
    char dir[700], path[900];

    job_temp_dir(sc->job, dir, sizeof dir);

    if (mkdir_p(dir, 0755) < 0) {
      log_error("[%s] download: could not create %s", sc->job->id, dir);
      sc->fs->open_failed = 1;
    } else {
      job_file_path(sc->job, sc->jf, path, sizeof path);

      /* No O_TRUNC: reopening (not recreating) lets a resumed job's
       * pwrite()s land alongside segments already on disk from the prior
       * run. No pre-sizing ftruncate() either -- sizing to jf->bytes (the
       * NZB's declared encoded size, always larger than the real decoded
       * size) left a zero-padded tail that made the file's MD5 never
       * match PAR2's declared hash despite every real block being fine.
       * pwrite() grows the file naturally as segments land instead. */
      if ((sc->fs->fd = open(path, O_RDWR | O_CREAT, 0644)) < 0) {
        log_error("[%s] download: open(%s): %s", sc->job->id, path, strerror(errno));
        sc->fs->open_failed = 1;
      }
    }
  }
  pthread_mutex_unlock(&sc->fs->mu);
}

/* Streams one article's yEnc-framed body, one dot-unstuffed line at a
 * time: recognizes =ybegin/=ypart/=yend, decodes data lines via rapidyenc
 * (../yenc/yenc.h) through buffer_append(), and checks a running CRC32
 * against =yend's declared crc. Lines arrive complete (CRLF already
 * stripped by nntp_conn_fetch()), never partial. */
static int
body_line_cb(void *ctx, const char *line, size_t len) {
  segment_ctx_t *sc = ctx;
  yenc_line_t yl;
  yenc_line_kind_t kind;
  unsigned char decode_buf[8192]; /* matches nntp_conn_fetch()'s own line[8192] -- decoded output is never longer than its encoded input */
  size_t declen;

  if (sc->yenc_state == YENC_DONE) return 0; /* trailing junk after =yend -- ignore */

  kind = yenc_classify_line(line, len, &yl);

  /* A second "=ybegin" mid-stream means nntp_pool retried this article
   * (run_job() replays it from scratch, reusing this segment_ctx_t) --
   * reset decode state so the retry's fresh bytes don't pile onto a
   * partial previous attempt's leftovers. */
  if (kind == YENC_LINE_BEGIN && sc->yenc_state != YENC_WANT_BEGIN) {
    free(sc->wbuf);
    sc->wbuf = NULL;
    sc->wbuf_len = 0;
    sc->written = 0;
    sc->crc = crc32_init();
    sc->have_expected_crc = 0;
    sc->expected_crc = 0;
    sc->yenc_failed = 0;
    sc->budget_warned = 0;
    sc->range_resolved = 0;
    sc->yenc_state = YENC_WANT_BEGIN;
  }

  if (sc->yenc_state == YENC_WANT_BEGIN) {
    if (kind != YENC_LINE_BEGIN) return 0; /* tolerate leading junk before =ybegin */
    sc->have_ybegin_size = yl.has_size;
    sc->ybegin_size = yl.size;
    sc->yenc_state = YENC_WANT_PART_OR_DATA;
    return 0;
  }

  if (sc->yenc_state == YENC_WANT_PART_OR_DATA) {
    if (kind == YENC_LINE_PART) {
      if (!yl.has_range || yl.begin < 1 || yl.end < yl.begin) {
        log_warn("[%s] download: segment %s: malformed =ypart line, giving up on this segment",
                  sc->job->id, sc->seg->message_id);
        sc->yenc_failed = 1;
        sc->yenc_state = YENC_DONE;
        return 0;
      }
      resolve_segment_range(sc, yl.begin - 1, yl.end - yl.begin + 1);
      sc->yenc_state = YENC_IN_DATA;
      return 0;
    }

    /* No =ypart -- single-part article, the whole file is this one part
     * (offset 0). Falls through to the shared =yend/data handling below
     * whether this line turned out to be data or (a degenerate,
     * zero-byte part's) =yend. */
    resolve_segment_range(sc, 0, sc->have_ybegin_size ? sc->ybegin_size : sc->seg->bytes);
    sc->yenc_state = YENC_IN_DATA;
  }

  /* YENC_IN_DATA, whether just entered above or already in it. */
  if (kind == YENC_LINE_END) {
    if (yl.has_crc32) {
      sc->expected_crc = yl.crc32;
      sc->have_expected_crc = 1;
    }
    sc->yenc_state = YENC_DONE;
    return 0;
  }

  if (kind != YENC_LINE_DATA) return 0; /* stray control line mid-part, ignore */

  declen = yenc_decode_line(line, len, decode_buf);
  sc->crc = crc32_update(sc->crc, decode_buf, declen);
  buffer_append(sc, decode_buf, declen);

  return 0;
}

static void
segment_done_cb(void *ctx, int status) {
  segment_ctx_t *sc = ctx;
  int ok = 0;

  segment_flush(sc); /* write out whatever's still buffered before judging completeness */

  if (sc->write_failed || sc->fs->open_failed) {
    /* already logged in resolve_segment_range()/segment_flush() */
  } else if (status != 220 && status != 222) {
    if (status != NNTP_POOL_ERR_CANCELLED) {
      log_warn("[%s] download: segment %s: fetch failed (status %d)",
                sc->job->id, sc->seg->message_id, status);
    } /* NNTP_POOL_ERR_CANCELLED is expected during shutdown/pause -- no log noise */
  } else if (!sc->range_resolved || sc->yenc_state != YENC_DONE || sc->yenc_failed) {
    log_warn("[%s] download: segment %s: no valid yEnc framing found in the article body",
              sc->job->id, sc->seg->message_id);
  } else if (sc->have_expected_crc && crc32_final(sc->crc) != (unsigned long)sc->expected_crc) {
    log_warn("[%s] download: segment %s: yEnc CRC32 mismatch (got %08lx, expected %08lx) - corrupt article",
              sc->job->id, sc->seg->message_id, crc32_final(sc->crc), (unsigned long)sc->expected_crc);
  } else {
    ok = 1;
  }

  if (ok) {
    int checkpoint;

    log_debug("[%s] download: segment %s: decoded %ld byte(s)",
              sc->job->id, sc->seg->message_id, sc->budget);

    queue_lock();
    job_mark_segment_downloaded(sc->job, sc->seg->message_id);
    queue_unlock();

    pthread_mutex_lock(&sc->st->mu);
    sc->st->since_checkpoint++;
    checkpoint = sc->st->since_checkpoint >= DL_CHECKPOINT_INTERVAL;
    if (checkpoint) sc->st->since_checkpoint = 0;
    pthread_mutex_unlock(&sc->st->mu);

    if (checkpoint) {
      queue_lock();
      queue_save_job(g_app.queue, sc->job);
      queue_unlock();
    }
  }

  free(sc->wbuf);
  dl_state_done(sc->st);
  free(sc);
}

static long long
job_remaining_bytes(const job_t *job) {
  long long total = 0;
  size_t fi, si;

  for (fi = 0; fi < job->file_count; fi++) {
    const job_file_t *jf = &job->files[fi];
    for (si = 0; si < jf->segment_count; si++) {
      if (!jf->segments[si].downloaded) total += jf->segments[si].bytes;
    }
  }

  return total;
}

/* Dispatches every not-yet-downloaded segment across the given files into
 * the pool and blocks until they've all finished (success, failure, or
 * cancellation). Segments already marked downloaded are skipped, same
 * resume semantics as a fresh download_job() call. */
static void
download_segments(job_t *job, job_file_t **files, size_t file_count) {
  job_dl_state_t st;
  file_dl_t *file_states;
  size_t fi, si;

  pthread_mutex_init(&st.mu, NULL);
  pthread_cond_init(&st.cond, NULL);
  st.pending = 0;
  st.since_checkpoint = 0;

  if (!(file_states = calloc(file_count, sizeof *file_states))) {
    log_error("[%s] download: out of memory", job->id);
    pthread_mutex_destroy(&st.mu);
    pthread_cond_destroy(&st.cond);
    return;
  }
  for (fi = 0; fi < file_count; fi++) {
    pthread_mutex_init(&file_states[fi].mu, NULL);
    file_states[fi].fd = -1;
  }

  for (fi = 0; fi < file_count && dl_should_continue(job); fi++) {
    job_file_t *jf = files[fi];
    file_dl_t *fs = &file_states[fi];
    size_t already = 0;

    for (si = 0; si < jf->segment_count; si++) {
      if (jf->segments[si].downloaded) already++;
    }
    log_info("[%s] download: %s: %zu/%zu segment(s) already downloaded, fetching %zu more",
             job->id, jf->filename, already, jf->segment_count, jf->segment_count - already);

    for (si = 0; si < jf->segment_count && dl_should_continue(job); si++) {
      job_segment_t *seg = &jf->segments[si];
      segment_ctx_t *sc;

      if (seg->downloaded) continue; /* resume: already have this one */

      if (!(sc = calloc(1, sizeof *sc))) continue;

      sc->st = &st;
      sc->job = job;
      sc->jf = jf;
      sc->seg = seg;
      sc->fs = fs;
      sc->crc = crc32_init();
      /* offset/budget are resolved dynamically from the article's own
       * =ybegin/=ypart lines once its body starts arriving -- see
       * resolve_segment_range(). */

      dl_state_add(&st);

      {
        int rc;

        /* pool_lock() held around the call, not just the pointer read, to
         * exclude a concurrent GET /api/status reading g_app.pool mid
         * rebuild -- see app_state.h's pool comment. */
        pool_lock();
        rc = nntp_pool_fetch(g_app.pool, "BODY", seg->message_id, body_line_cb, segment_done_cb, sc);
        pool_unlock();

        if (rc < 0) {
          dl_state_done(&st);
          free(sc);
        }
      }
    }
  }

  dl_state_wait(&st);
  pthread_mutex_destroy(&st.mu);
  pthread_cond_destroy(&st.cond);

  /* Every dispatched segment has now finished -- safe to close every fd
   * this run opened in one pass (see file_dl_t for why not incrementally). */
  for (fi = 0; fi < file_count; fi++) {
    if (file_states[fi].fd >= 0) close(file_states[fi].fd);
    pthread_mutex_destroy(&file_states[fi].mu);
  }
  free(file_states);
}

/* Opens and closes one throwaway connection up front so an unreachable
 * server or rejected login fails the job immediately with a clear reason,
 * instead of every segment separately working through nntp_pool's own
 * connect-retry loop only to report the same problem. err filled with a
 * short reason on failure. Returns 0 if reachable/authenticated, -1
 * otherwise. */
static int
check_server_reachable(char *err, size_t err_size) {
  nntp_config_t nntp;
  nntp_conn_opts_t opts = {0};
  nntp_conn_t *conn;

  config_lock();
  nntp = g_app.config.nntp;
  config_unlock();

  opts.host = nntp.host;
  opts.port = nntp.port;
  opts.tls = nntp.tls;
  opts.connect_timeout_sec = nntp.connect_timeout_sec;
  opts.read_timeout_sec = nntp.read_timeout_sec;

  if (!(conn = nntp_conn_open(&opts, NULL, 0))) {
    snprintf(err, err_size, "could not connect to NNTP server %s:%s", nntp.host, nntp.port);
    return -1;
  }

  if (nntp.user[0] && nntp_conn_authenticate(conn, nntp.user, nntp.pass) < 0) {
    nntp_conn_close(conn);
    snprintf(err, err_size, "NNTP authentication failed for user %s", nntp.user);
    return -1;
  }

  nntp_conn_close(conn);
  return 0;
}

/* par2_progress_cb for par2_verify_job() -- updates g_app's verify-progress
 * fields (app_state.h) so GET /api/jobs can report a percentage. Called
 * once per read() chunk, so just a locked field update, no I/O. Returns
 * nonzero once shutdown_requested is set, or the job itself gets
 * cancelled, telling par2_verify_file() to abort within one chunk instead
 * of verifying the rest of a possibly huge file. ctx is the job_t* (see
 * par2_verify_job()'s pg.cb_ctx). */
static int
verify_progress_update(void *ctx, long long bytes_done, long long bytes_total) {
  const job_t *job = ctx;

  verify_lock();
  g_app.verify_bytes_done = bytes_done;
  g_app.verify_bytes_total = bytes_total;
  verify_unlock();
  return app_is_shutting_down() || job_was_cancelled(job);
}

/* Runs PAR2 verification (../par2/par2.h) over every downloaded file the
 * recovery set covers -- a job with no PAR2 data, or none of whose files
 * are covered, passes trivially. Doesn't attempt repair itself (see
 * par2_repair_job()): a covered file that fails just fails the job. err
 * filled with a short reason on failure. Returns 1 if every covered file
 * verified, 0 otherwise. */
static int
par2_verify_job(job_t *job, char *err, size_t err_size) {
  char temp_dir[700];
  par2_set_t *set;
  size_t fi;
  int ok = 1;
  par2_progress_t pg = {0};

  job_temp_dir(job, temp_dir, sizeof temp_dir);

  if (!(set = par2_scan_dir(temp_dir))) {
    log_warn("[%s] download: par2_scan_dir out of memory, skipping verification", job->id);
    return 1;
  }

  if (set->file_count == 0) {
    log_info("[%s] download: no PAR2 recovery data found, skipping verification", job->id);
    par2_set_free(set);
    return 1;
  }

  queue_lock();
  job_set_state(job, JOB_VERIFYING);
  queue_save_job(g_app.queue, job);
  queue_unlock();

  /* Known upfront (sum of every covered file's PAR2-declared length) so
   * the very first file's progress reports a meaningful percentage
   * rather than climbing from an unknown total. */
  for (fi = 0; fi < job->file_count; fi++) {
    const par2_file_t *pf = par2_find_file(set, job->files[fi].filename);
    if (pf) pg.total += pf->length;
  }

  pg.cb = verify_progress_update;
  pg.cb_ctx = job;
  verify_lock();
  snprintf(g_app.verify_job_id, sizeof g_app.verify_job_id, "%s", job->id);
  g_app.verify_bytes_done = 0;
  g_app.verify_bytes_total = pg.total;
  verify_unlock();

  for (fi = 0; fi < job->file_count && ok; fi++) {
    const job_file_t *jf = &job->files[fi];
    const par2_file_t *pf;
    char path[900];
    par2_verify_report_t report;

    if (!(pf = par2_find_file(set, jf->filename))) continue; /* not covered by the recovery set -- fine */

    job_file_path(job, jf, path, sizeof path);

    /* Shrink-only fixup: a file from before pre-sizing was removed can
     * still carry a zero-padded tail past its true content, and a
     * resumed job (which skips already-downloaded segments) wouldn't
     * otherwise rewrite it. Trim to PAR2's declared length -- never grow,
     * a file genuinely shorter than that is real missing content. */
    {
      struct stat st;
      if (stat(path, &st) == 0 && pf->length > 0 && st.st_size > pf->length) {
        if (truncate(path, pf->length) != 0) {
          log_warn("[%s] download: truncate(%s, %lld): %s",
                    job->id, path, pf->length, strerror(errno));
        }
      }
    }

    if (par2_verify_file(set, pf, path, &pg, &report) < 0) {
      snprintf(err, err_size, "PAR2 verify: could not read %s", jf->filename);
      ok = 0;
      break;
    }

    if (report.result == PAR2_VERIFY_OK) {
      log_debug("[%s] download: %s: PAR2 verify OK", job->id, jf->filename);
      continue;
    }

    ok = 0;
    if (report.result == PAR2_VERIFY_DAMAGED) {
      snprintf(err, err_size, "PAR2 verify: %s: %zu of %zu blocks corrupt/missing",
               jf->filename, report.bad_blocks, report.block_count);
    } else {
      snprintf(err, err_size, "PAR2 verify: %s: content does not match (no block-level detail available)",
               jf->filename);
    }
  }

  verify_lock();
  g_app.verify_job_id[0] = 0;
  verify_unlock();

  par2_set_free(set);
  return ok;
}

/* par2_progress_cb for par2_repair_job()'s par2_repair_set() call --
 * mirrors verify_progress_update()/extract_progress_update() exactly,
 * including the shutdown/cancel-abort return. ctx is the job_t*. */
static int
repair_progress_update(void *ctx, long long bytes_done, long long bytes_total) {
  const job_t *job = ctx;

  repair_lock();
  g_app.repair_bytes_done = bytes_done;
  g_app.repair_bytes_total = bytes_total;
  repair_unlock();
  return app_is_shutting_down() || job_was_cancelled(job);
}

/* Attempts full PAR2 repair (Reed-Solomon over GF(2^16), see
 * ../par2/par2.h and ../par2/rs.h). Only called after par2_verify_job()
 * has already failed, so recovery data is known to exist. err filled
 * with a specific reason on failure. Returns 1 if repair completed
 * without a fatal error -- success here doesn't mean the result is
 * correct, the caller must re-run par2_verify_job() to confirm (see
 * par2_repair_set()'s comment). */
static int
par2_repair_job(job_t *job, char *err, size_t err_size) {
  char temp_dir[700];
  par2_set_t *set;
  par2_repair_file_t *files;
  size_t fi;
  par2_progress_t pg = {0};
  int ok;

  job_temp_dir(job, temp_dir, sizeof temp_dir);

  if (!(set = par2_scan_dir(temp_dir))) {
    snprintf(err, err_size, "PAR2 repair: out of memory scanning recovery data");
    return 0;
  }
  if (set->file_count == 0) {
    /* Shouldn't happen -- caller only reaches here after par2_verify_job()
     * already found PAR2 data -- but handle it gracefully anyway. */
    par2_set_free(set);
    snprintf(err, err_size, "PAR2 repair: no recovery data found");
    return 0;
  }

  if (!(files = calloc(job->file_count, sizeof *files))) {
    par2_set_free(set);
    snprintf(err, err_size, "PAR2 repair: out of memory");
    return 0;
  }
  for (fi = 0; fi < job->file_count; fi++) {
    snprintf(files[fi].name, sizeof files[fi].name, "%s", job->files[fi].filename);
    job_file_path(job, &job->files[fi], files[fi].path, sizeof files[fi].path);
  }

  queue_lock();
  job_set_state(job, JOB_REPAIRING);
  queue_save_job(g_app.queue, job);
  queue_unlock();

  /* Rough progress total -- repair also reads present blocks and recovery
   * slice data, not just the missing content, but it's still a meaningful
   * "it's working" signal. */
  for (fi = 0; fi < job->file_count; fi++) {
    const par2_file_t *pf = par2_find_file(set, job->files[fi].filename);
    if (pf) pg.total += pf->length;
  }

  pg.cb = repair_progress_update;
  pg.cb_ctx = job;
  repair_lock();
  snprintf(g_app.repair_job_id, sizeof g_app.repair_job_id, "%s", job->id);
  g_app.repair_bytes_done = 0;
  g_app.repair_bytes_total = pg.total;
  repair_unlock();

  ok = par2_repair_set(set, files, job->file_count, &pg, err, err_size) == 0;

  repair_lock();
  g_app.repair_job_id[0] = 0;
  repair_unlock();

  free(files);
  par2_set_free(set);
  return ok;
}

/* Derives a filesystem-safe directory name from job->name for placing
 * finalized (extracted or moved) output under -- strips a trailing
 * ".nzb" (the usual case, job->name is the uploaded NZB's own filename)
 * before sanitizing, so "My.Release.nzb" becomes a directory named
 * "My.Release", not "My.Release.nzb". */
static void
job_output_subdir(const job_t *job, char *out, size_t out_size) {
  char name[256];
  size_t len;

  snprintf(name, sizeof name, "%s", job->name);
  len = strlen(name);
  if (len > 4 && !strcasecmp(name + len - 4, ".nzb")) name[len - 4] = 0;

  path_sanitize_component(name, sizeof name);
  snprintf(out, out_size, "%s", name);
}

/* storage.output_dir (or job->output_dir, if the job set a per-job
 * override -- see job.h) joined with job_output_subdir() -- the exact
 * directory finalize_job() extracts/moves a job's real output into,
 * computed identically wherever it's needed (here, and
 * api_jobs_delete()'s cleanup of an incomplete job's partial output) so
 * they can't drift apart. out[0] is left 0 if no output_dir is configured
 * at all (job->output_dir empty and storage.output_dir unset) --
 * finalize_job() leaves files under temp_dir in that case, so there's
 * never a separate output directory to clean up. */
void
job_output_dest_dir(const job_t *job, char *out, size_t out_size) {
  char subdir[300], output_dir[512];

  job_output_subdir(job, subdir, sizeof subdir);

  if (job->output_dir[0]) {
    snprintf(output_dir, sizeof output_dir, "%s", job->output_dir);
  } else {
    config_lock();
    snprintf(output_dir, sizeof output_dir, "%s", g_app.config.storage.output_dir);
    config_unlock();
  }

  if (!output_dir[0]) { out[0] = 0; return; }

  snprintf(out, out_size, "%s/%s", output_dir, subdir);
}

#define NFO_SEARCH_MAX_DEPTH 3

static int
has_nfo_suffix(const char *name) {
  size_t len = strlen(name);
  return len > 4 && !strcasecmp(name + len - 4, ".nfo");
}

static int
find_nfo_recursive(const char *dir, char *out_path, size_t out_size, int depth) {
  DIR *d = opendir(dir);
  struct dirent *ent;
  int found = 0;

  if (!d) return 0;

  while (!found && (ent = readdir(d))) {
    char child[900];
    struct stat st;

    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    snprintf(child, sizeof child, "%s/%s", dir, ent->d_name);
    if (stat(child, &st) != 0) continue;

    if (S_ISREG(st.st_mode) && has_nfo_suffix(ent->d_name)) {
      snprintf(out_path, out_size, "%s", child);
      found = 1;
    }
  }

  if (!found && depth > 1) {
    rewinddir(d);
    while (!found && (ent = readdir(d))) {
      char child[900];
      struct stat st;

      if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      snprintf(child, sizeof child, "%s/%s", dir, ent->d_name);
      if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
        found = find_nfo_recursive(child, out_path, out_size, depth - 1);
      }
    }
  }

  closedir(d);
  return found;
}

void
job_ensure_nfo_scanned(job_t *job) {
  char dest_dir[900];

  if (job->state != JOB_COMPLETED) return;
  if (job->nfo_path[0] || job->nfo_checked) return; /* already resolved, one way or the other */

  job_output_dest_dir(job, dest_dir, sizeof dest_dir);
  if (dest_dir[0]) find_nfo_recursive(dest_dir, job->nfo_path, sizeof job->nfo_path, NFO_SEARCH_MAX_DEPTH);
  job->nfo_checked = 1;
}

/* Moves src to dst: rename() first, falling back to copy+delete on EXDEV
 * (temp_dir/output_dir on different filesystems -- plausible with
 * multiple mounted USB drives). chmod's dst 0777 since files may arrive
 * with no executable bit, which matters for a PS5 homebrew ELF (see
 * extract.c's matching chmod). Returns 0 on success, -1 on failure
 * (already logged). */
static int
move_file(const char *src, const char *dst) {
  FILE *in, *out;
  char buf[65536];
  size_t n;
  int ok = 1;

  if (rename(src, dst) == 0) {
    if (chmod(dst, 0777) != 0) {
      log_warn("download: chmod(%s, 0777): %s", dst, strerror(errno));
    }
    return 0;
  }
  if (errno != EXDEV) {
    log_error("download: rename(%s, %s): %s", src, dst, strerror(errno));
    return -1;
  }

  if (!(in = fopen(src, "rb"))) {
    log_error("download: fopen(%s): %s", src, strerror(errno));
    return -1;
  }
  if (!(out = fopen(dst, "wb"))) {
    log_error("download: fopen(%s) for write: %s", dst, strerror(errno));
    fclose(in);
    return -1;
  }

  while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
    if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
  }
  if (ferror(in)) ok = 0;

  fclose(in);
  if (fclose(out) != 0) ok = 0;

  if (!ok) {
    log_error("download: copy %s -> %s failed: %s", src, dst, strerror(errno));
    remove(dst);
    return -1;
  }

  remove(src);
  if (chmod(dst, 0777) != 0) {
    log_warn("download: chmod(%s, 0777): %s", dst, strerror(errno));
  }
  return 0;
}

/* extract_progress_cb for finalize_job()'s extract_job() call -- updates
 * g_app's extraction-progress fields (app_state.h) so GET /api/jobs can
 * report a percentage. Called once per libarchive block, so just a
 * locked field update, no I/O. Returns nonzero once shutdown_requested is
 * set, or the job itself gets cancelled, telling extract_job() to abort
 * within one block instead of running a possibly multi-GB extraction to
 * completion. ctx is the job_t* (see finalize_job()'s extract_job() call). */
static int
extract_progress_update(void *ctx, long long bytes_done, long long bytes_total) {
  const job_t *job = ctx;

  extract_lock();
  g_app.extract_bytes_done = bytes_done;
  g_app.extract_bytes_total = bytes_total;
  extract_unlock();
  return app_is_shutting_down() || job_was_cancelled(job);
}

/* True if name is a PAR2/archive sidecar file -- never the actual
 * download output. Only matters for finalize_job()'s no-archive branch
 * (a job with a real archive takes the extract_job() path instead). */
static int
is_sidecar_file(const char *name) {
  static const char *const skip_ext[] = { ".par2", ".rar", ".r00", ".7z", ".zip", NULL };
  size_t len = strlen(name);
  int i;

  for (i = 0; skip_ext[i]; i++) {
    size_t ext_len = strlen(skip_ext[i]);
    if (len >= ext_len && !strcasecmp(name + len - ext_len, skip_ext[i])) return 1;
  }

  return 0;
}

/* Places a verified job's output under storage.output_dir/<job name>/:
 * extracts any archive found among its files (../extract/extract.h)
 * there, or -- if there's no archive -- moves its real files (see
 * is_sidecar_file(), which excludes .par2 parts) as-is. Blank output_dir
 * skips this step, leaving files under temp_dir (nothing to clean up in
 * that case -- temp_dir *is* the output). On success, temp_dir is removed
 * afterward: its contents (source .rar/.par2 parts, and the moved-away
 * originals) are no longer needed once the real output is in place. err
 * filled with a short reason on failure. Returns 1 on success, 0 on
 * failure -- on failure, temp_dir is left alone so the job can be retried
 * or its files inspected. */
static int
finalize_job(job_t *job, char *err, size_t err_size) {
  char temp_dir[700], dest_dir[900];
  extract_result_t res;

  job_temp_dir(job, temp_dir, sizeof temp_dir);
  job_output_dest_dir(job, dest_dir, sizeof dest_dir);

  if (!dest_dir[0]) return 1; /* nothing configured to do -- leave files in temp_dir */

  queue_lock();
  job_set_state(job, JOB_EXTRACTING);
  queue_save_job(g_app.queue, job);
  queue_unlock();

  extract_lock();
  snprintf(g_app.extract_job_id, sizeof g_app.extract_job_id, "%s", job->id);
  g_app.extract_bytes_done = 0;
  g_app.extract_bytes_total = 0;
  extract_unlock();

  res = extract_job(job, temp_dir, dest_dir, extract_progress_update, job, err, err_size);

  extract_lock();
  g_app.extract_job_id[0] = 0;
  extract_unlock();

  if (res == EXTRACT_FAILED) return 0;
  if (res == EXTRACT_OK) {
    log_info("[%s] download: extracted to %s", job->id, dest_dir);
    job->final_bytes = path_dir_total_bytes(dest_dir);
    if (job->add_to_shadowmount) shadowmount_register(job->id, dest_dir);
    rmdir_recursive(temp_dir);
    return 1;
  }

  /* EXTRACT_NONE -- no archive among this job's files; move its real
   * content into the same destination directory instead. */
  {
    size_t fi;

    if (mkdir_p(dest_dir, 0755) < 0) {
      snprintf(err, err_size, "could not create output directory %s", dest_dir);
      return 0;
    }

    for (fi = 0; fi < job->file_count; fi++) {
      const job_file_t *jf = &job->files[fi];
      char src[900], safe_name[512], dst[1200];

      if (is_sidecar_file(jf->filename)) continue; /* .par2/archive parts stay in temp_dir, not the real output */

      job_file_path(job, jf, src, sizeof src);
      snprintf(safe_name, sizeof safe_name, "%s", jf->filename);
      path_sanitize_component(safe_name, sizeof safe_name);
      snprintf(dst, sizeof dst, "%s/%s", dest_dir, safe_name);

      if (move_file(src, dst) < 0) {
        snprintf(err, err_size, "could not move %s to the output directory", jf->filename);
        return 0;
      }
    }

    log_info("[%s] download: moved to %s", job->id, dest_dir);
  }

  job->final_bytes = path_dir_total_bytes(dest_dir);
  if (job->add_to_shadowmount) shadowmount_register(job->id, dest_dir);
  rmdir_recursive(temp_dir);
  return 1;
}

/* Marks a job failed for a transient, recoverable reason (server
 * unreachable, segments never arrived) -- re-queues instead of failing
 * outright as long as retries_used < config.queue.max_retries (persisted,
 * so bounded across restarts too). Used only for the two transient cases
 * below, not for disk space, PAR2, or extract failures, where retrying
 * the same job is unlikely to change the outcome. */
static void
job_fail_or_retry(job_t *job, const char *err) {
  int max_retries;

  config_lock();
  max_retries = g_app.config.queue.max_retries;
  config_unlock();

  queue_lock();
  snprintf(job->last_error, sizeof job->last_error, "%s", err);

  if (job->retries_used < max_retries) {
    job->retries_used++;
    log_warn("[%s] download (%s): %s - auto-retrying (%d/%d)",
             job->id, job->name, err, job->retries_used, max_retries);
    job_set_state(job, JOB_QUEUED);
  } else {
    if (max_retries > 0) {
      log_error("[%s] download (%s): %s - giving up after %d retr%s",
                job->id, job->name, err, max_retries, max_retries == 1 ? "y" : "ies");
    }
    job_set_state(job, JOB_FAILED);
  }

  queue_save_job(g_app.queue, job);
  queue_unlock();
}

/* Used wherever a verify/repair/extract step failed only because
 * api_system_eject() asked everything to stop, or an API handler cancelled
 * this job mid-step (see verify_progress_update()/repair_progress_update()/
 * extract_progress_update()) -- forces the job to a clean JOB_CANCELLED
 * rather than the JOB_FAILED a real verify/repair/extract error would get,
 * since par2_verify_job()/par2_repair_job()/finalize_job() overwrite
 * job->state to JOB_VERIFYING/JOB_REPAIRING/JOB_EXTRACTING right as they
 * start, clobbering whatever state was set before the abort was even
 * noticed (including a user Cancel that raced with that overwrite). Leaves
 * the job retryable (queue_retry_job() accepts JOB_CANCELLED) instead of
 * stuck showing a misleading "aborted" error. */
static void
job_mark_stopped(job_t *job) {
  const char *reason = app_is_shutting_down() ? "payload is shutting down" : "cancelled by user";

  queue_lock();
  job_set_state(job, JOB_CANCELLED);
  queue_save_job(g_app.queue, job);
  queue_unlock();
  log_info("[%s] download: stopped (%s)", job->id, reason);
}

static void
download_job(finalize_queue_t *fq, job_t *job) {
  job_file_t **all_files;
  size_t fi;
  int all_ok;
  char reach_err[256];

  if (check_server_reachable(reach_err, sizeof reach_err) < 0) {
    log_error("[%s] download: %s - not attempting any segments", job->id, reach_err);
    job_fail_or_retry(job, reach_err);
    return;
  }

  {
    char temp_dir[700];
    long long needed, free_bytes;

    job_temp_dir(job, temp_dir, sizeof temp_dir);
    needed = job_remaining_bytes(job);
    free_bytes = path_free_bytes(temp_dir);

    log_info("[%s] download (%s): starting - %zu file(s), %lld bytes remaining",
             job->id, job->name, job->file_count, needed);
    log_info("[%s] download: space check on temp dir %s: %lld bytes needed, %lld available",
             job->id, temp_dir, needed, free_bytes);

    /* free_bytes < 0 means the check itself failed (e.g. statfs error) --
     * proceed rather than block on a check we couldn't perform; a
     * genuinely full disk still surfaces as write failures downstream. */
    if (free_bytes >= 0 && free_bytes < needed) {
      log_error("[%s] download: insufficient disk space (%lld bytes needed, %lld available on %s)",
                job->id, needed, free_bytes, temp_dir);
      queue_lock();
      snprintf(job->last_error, sizeof job->last_error,
               "insufficient disk space: %lld bytes needed, %lld available", needed, free_bytes);
      job_set_state(job, JOB_FAILED);
      queue_save_job(g_app.queue, job);
      queue_unlock();
      return;
    }
  }

  queue_lock();
  /* Cleared here, not only on manual retry, so an auto-retried job that
   * goes on to succeed doesn't end up JOB_COMPLETED with a stale error
   * message left over from the attempt before. */
  job->last_error[0] = 0;
  job_set_state(job, JOB_DOWNLOADING);
  queue_save_job(g_app.queue, job);
  queue_unlock();

  if (!(all_files = malloc(job->file_count * sizeof *all_files))) {
    log_error("[%s] download: out of memory", job->id);
    queue_lock();
    snprintf(job->last_error, sizeof job->last_error, "out of memory");
    job_set_state(job, JOB_FAILED);
    queue_save_job(g_app.queue, job);
    queue_unlock();
    return;
  }
  for (fi = 0; fi < job->file_count; fi++) all_files[fi] = &job->files[fi];

  download_segments(job, all_files, job->file_count);
  free(all_files);

  if (!dl_should_continue(job)) {
    log_info("[%s] download: stopped (state is now %s)", job->id, job_state_name(job->state));
    return;
  }

  /* Every file is already fully written (decoded, not yEnc-encoded)
   * under storage.temp_dir/<job id>/ -- what's left is confirming every
   * segment arrived, then PAR2-verifying the result. */
  all_ok = 1;
  {
    char detail[256] = "";
    size_t detail_len = 0;

    for (fi = 0; fi < job->file_count; fi++) {
      const job_file_t *jf = &job->files[fi];
      size_t total = 0, downloaded = 0, si;

      for (si = 0; si < jf->segment_count; si++) {
        total++;
        if (jf->segments[si].downloaded) downloaded++;
      }

      if (downloaded < total) {
        all_ok = 0;
        if (detail_len < sizeof detail) {
          detail_len += (size_t)snprintf(detail + detail_len, sizeof detail - detail_len,
                                          "%s%s (%zu/%zu segments)", detail_len ? "; " : "",
                                          jf->filename, downloaded, total);
        }
      }
    }

    if (!all_ok) {
      char msg[300];

      log_error("[%s] download (%s): failed - %s", job->id, job->name, detail);
      snprintf(msg, sizeof msg, "one or more files incomplete: %s", detail);
      job_fail_or_retry(job, msg);
      return;
    }
  }

  {
    char verify_err[256];

    /* job_busy_begin()/job_busy_end() bracket this whole block (not just
     * one call): queue_remove_job() must refuse to free job for as long as
     * any of these blocking calls -- or the state/log writes right after
     * one aborts -- might still touch it, see job.h's busy comment. */
    job_busy_begin(job);

    if (!par2_verify_job(job, verify_err, sizeof verify_err)) {
      char repair_err[256];

      if (app_is_shutting_down() || job_was_cancelled(job)) { job_busy_end(job); job_mark_stopped(job); return; }

      log_warn("[%s] download (%s): PAR2 verify failed (%s) - attempting repair",
               job->id, job->name, verify_err);

      if (!par2_repair_job(job, repair_err, sizeof repair_err)) {
        if (app_is_shutting_down() || job_was_cancelled(job)) { job_busy_end(job); job_mark_stopped(job); return; }
        job_busy_end(job);
        log_error("[%s] download (%s): %s", job->id, job->name, repair_err);
        queue_lock();
        snprintf(job->last_error, sizeof job->last_error, "%s", repair_err);
        job_set_state(job, JOB_FAILED);
        queue_save_job(g_app.queue, job);
        queue_unlock();
        return;
      }

      /* par2_repair_job() succeeding only means the reconstruction math
       * and disk writes went through without a fatal error -- the real
       * confirmation is re-running verify and having it actually pass. */
      if (!par2_verify_job(job, verify_err, sizeof verify_err)) {
        if (app_is_shutting_down() || job_was_cancelled(job)) { job_busy_end(job); job_mark_stopped(job); return; }
        job_busy_end(job);
        log_error("[%s] download (%s): repair completed but re-verification still failed - %s",
                   job->id, job->name, verify_err);
        queue_lock();
        snprintf(job->last_error, sizeof job->last_error,
                 "PAR2 repair completed but re-verification still failed: %s", verify_err);
        job_set_state(job, JOB_FAILED);
        queue_save_job(g_app.queue, job);
        queue_unlock();
        return;
      }

      log_info("[%s] download (%s): PAR2 repair succeeded, re-verified OK", job->id, job->name);
    }

    job_busy_end(job);
  }

  /* Download+verify+repair done -- hand extract+finalize to the finalizer
   * thread and return so downloader_main() can pick up the next job
   * without waiting (see this file's top comment). JOB_EXTRACTING/
   * COMPLETED/FAILED happen on that thread once it gets to this job. */
  finalize_enqueue(fq, job);
}

/* Drains finalize_queue_t, extracting+finalizing one job at a time. Runs
 * for the process lifetime, exiting once finalize_dequeue() reports the
 * queue shut down and empty. */
static void *
finalizer_main(void *arg) {
  downloader_t *d = arg;
  job_t *job;

  log_set_thread_name("finalizer");

  while ((job = finalize_dequeue(&d->fq)) != NULL) {
    char finalize_err[256];

    /* If the job was paused/cancelled while waiting behind an earlier
     * extraction, don't extract it anyway (same check download_job()
     * does right after downloading, see dl_should_continue()). */
    if (!dl_should_continue(job)) {
      log_info("[%s] download: skipping extraction (state is now %s)",
                job->id, job_state_name(job->state));
      continue;
    }

    /* Bracketed the same way as download_job()'s verify/repair block --
     * finalize_job() blocks inside extract_job() for as long as the
     * archive takes, only checking in via extract_progress_update(), so
     * queue_remove_job() needs job->busy to know this thread is still
     * dereferencing job even after its state says JOB_CANCELLED. */
    job_busy_begin(job);
    if (!finalize_job(job, finalize_err, sizeof finalize_err)) {
      job_busy_end(job);
      if (app_is_shutting_down() || job_was_cancelled(job)) { job_mark_stopped(job); continue; }
      log_error("[%s] download (%s): %s", job->id, job->name, finalize_err);
      queue_lock();
      snprintf(job->last_error, sizeof job->last_error, "%s", finalize_err);
      job_set_state(job, JOB_FAILED);
      queue_save_job(g_app.queue, job);
      queue_unlock();
      continue;
    }
    job_busy_end(job);

    log_info("[%s] download (%s): completed", job->id, job->name);
    queue_lock();
    job_set_state(job, JOB_COMPLETED);
    job_ensure_nfo_scanned(job); /* before the save just below, so nfo_path is persisted immediately */
    queue_save_job(g_app.queue, job);
    queue_unlock();
  }

  return NULL;
}

static void *
downloader_main(void *arg) {
  downloader_t *d = arg;
  int pool_idle = 0; /* avoids re-broadcasting nntp_pool_close_idle() every
                       * second while nothing's queued. */

  log_set_thread_name("downloader");

  while (!d->stop) {
    job_t *next;

    /* Only safe to rebuild the pool here, between jobs -- never while
     * download_job() is running (see app_state.h's pool comment). */
    app_reload_pool_if_needed();

    queue_lock();
    next = queue_find_next_queued(g_app.queue);
    queue_unlock();

    if (!next) {
      /* Nothing to download -- don't sit on open connections for no
       * reason; they reopen lazily on next use. */
      if (!pool_idle) {
        pool_lock();
        nntp_pool_close_idle(g_app.pool);
        pool_unlock();
        pool_idle = 1;
      }
      sleep(1);
      continue;
    }

    pool_idle = 0;
    download_job(&d->fq, next);
  }

  return NULL;
}

downloader_t *
downloader_start(void) {
  downloader_t *d = calloc(1, sizeof *d);
  pthread_attr_t attr;
  int rc;

  if (!d) return NULL;

  pthread_mutex_init(&d->fq.mu, NULL);
  pthread_cond_init(&d->fq.not_empty, NULL);

  /* Same explicit stack-size precaution as nntp_pool's workers (see
   * nntp_pool.c) -- the platform default is too small. */
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 1024 * 1024);
  rc = pthread_create(&d->thread, &attr, downloader_main, d);
  if (rc == 0) rc = pthread_create(&d->finalize_thread, &attr, finalizer_main, d);
  pthread_attr_destroy(&attr);

  if (rc != 0) {
    /* A finalizer that never starts would leave every future job stuck
     * unextracted forever -- if the downloader thread did start, stop it
     * the same way downloader_stop() would before giving up. */
    log_error("download: pthread_create failed");
    if (d->thread) {
      d->stop = 1;
      pthread_join(d->thread, NULL);
    }
    pthread_mutex_destroy(&d->fq.mu);
    pthread_cond_destroy(&d->fq.not_empty);
    free(d);
    return NULL;
  }

  return d;
}

void
downloader_stop(downloader_t *d) {
  if (!d) return;

  /* Stop and join the downloader thread first so it can never call
   * finalize_enqueue() again, only then tell the finalize queue to drain
   * and exit -- reversing this order could shut the finalizer down
   * before the downloader's last job was enqueued. */
  d->stop = 1;
  pthread_join(d->thread, NULL);

  pthread_mutex_lock(&d->fq.mu);
  d->fq.shutdown = 1;
  pthread_cond_signal(&d->fq.not_empty);
  pthread_mutex_unlock(&d->fq.mu);
  pthread_join(d->finalize_thread, NULL);

  pthread_mutex_destroy(&d->fq.mu);
  pthread_cond_destroy(&d->fq.not_empty);
  free(d);
}
