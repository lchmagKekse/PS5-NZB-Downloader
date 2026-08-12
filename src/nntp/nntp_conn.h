/* Single NNTP connection: transport (plain TCP or implicit TLS), AUTHINFO
 * authentication, and article fetch with dot-unstuffing.
 *
 * This module knows nothing about pooling, retries, or reconnection --
 * see nntp_pool.h for that. A transport-level failure here (negative
 * return from nntp_conn_read_line/nntp_conn_fetch) means the connection is
 * dead; the caller must nntp_conn_close() it and open a new one.
 */
#pragma once

#include <stddef.h>

typedef struct nntp_conn nntp_conn_t;

typedef struct {
  const char *host;
  const char *port;                /* numeric string, e.g. "119" or "563" */
  int         tls;                 /* 0 = plain, 1 = implicit TLS */
  int         connect_timeout_sec;
  int         read_timeout_sec;
} nntp_conn_opts_t;

/* Resolves, connects, performs the TLS handshake if opts->tls, and reads
 * the server's greeting line. On success, greeting_out (if non-NULL) holds
 * the greeting text and the returned conn is ready for
 * nntp_conn_authenticate()/nntp_conn_fetch(). Returns NULL on any failure
 * (already logged via log_error()/log_warn()). */
nntp_conn_t *nntp_conn_open(const nntp_conn_opts_t *opts,
                             char *greeting_out, size_t greeting_out_size);

/* AUTHINFO USER/PASS per RFC 4643. Returns 0 on success (including the
 * "no password required" 281 case), -1 on failure. */
int nntp_conn_authenticate(nntp_conn_t *c, const char *user, const char *pass);

/* Sends QUIT (best-effort, ignores the reply/errors) and releases all
 * resources. Safe to call with c == NULL. */
void nntp_conn_close(nntp_conn_t *c);

/* Raw line-oriented command/response, for NNTP commands this module
 * doesn't wrap directly. Returns 0 on success, -1 on transport failure. */
int nntp_conn_cmd(nntp_conn_t *c, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));

/* Reads one CRLF-terminated line (CR stripped, LF consumed, NUL-terminated
 * into buf). Returns the line length (>= 0) on success, -1 on transport
 * failure/timeout. Does NOT perform dot-unstuffing -- use for single-line
 * command responses only. */
int nntp_conn_read_line(nntp_conn_t *c, char *buf, size_t size);

/* Invoked once per decoded (dot-unstuffed) body line, without the
 * trailing CRLF. Returning nonzero aborts the fetch early. */
typedef int (*nntp_body_line_cb)(void *ctx, const char *line, size_t len);

/* Issues "<cmd> <message_id>\r\n" (cmd is "ARTICLE", "BODY", or "STAT"),
 * reads the status line, and for ARTICLE/BODY streams the multi-line body
 * to body_cb one line at a time -- nothing beyond one line is buffered, so
 * article size does not bound process memory here.
 *
 * Returns the NNTP status code (e.g. 220, 222, 223, 423, 430) on a
 * well-formed exchange -- callers distinguish "missing article" (4xx) from
 * success this way. Returns a negative value on transport failure, which
 * the caller should treat as connection-dead. */
int nntp_conn_fetch(nntp_conn_t *c, const char *cmd, const char *message_id,
                     nntp_body_line_cb body_cb, void *ctx);
