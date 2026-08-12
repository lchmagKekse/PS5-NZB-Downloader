#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "../log/log.h"
#include "nntp_conn.h"

#define RBUF_SIZE 65536       /* app-level read buffer -- see fill_buffer() */
#define SOCK_RCVBUF_SIZE (256 * 1024)  /* kernel socket receive buffer */
#define SOCK_SNDBUF_SIZE (64 * 1024)   /* traffic is download-heavy; commands are tiny */

struct nntp_conn {
  int      fd;
  SSL     *ssl;
  SSL_CTX *ssl_ctx;

  /* Buffered so each line doesn't cost its own recv()/SSL_read() syscall --
   * see fill_buffer()/raw_read_line(). Per-byte syscall overhead otherwise
   * dominates on ~128-byte yEnc lines. */
  char     rbuf[RBUF_SIZE];
  size_t   rbuf_pos;   /* next unread byte in rbuf */
  size_t   rbuf_len;   /* valid bytes in rbuf */
};

static void
log_ssl_errors(void) {
  unsigned long e;
  char ebuf[256];

  while ((e = ERR_get_error())) {
    ERR_error_string_n(e, ebuf, sizeof ebuf);
    log_error("nntp: openssl: %s", ebuf);
  }
}

/* getaddrinfo() on this SDK/platform intermittently fails with EAI_FAIL under
 * normal operation (flaky/contended resolver, not a real DNS problem) --
 * retry a few times before surfacing it. Deliberately not caching the
 * resolved address across calls: the provider load-balances DNS across
 * backend IPs, and pinning to one would defeat failover. */
#define RESOLVE_ATTEMPTS 3
#define RESOLVE_RETRY_DELAY_USEC 200000

static int
resolve_host(const char *host, const char *port, struct addrinfo **res) {
  struct addrinfo hints = {0};
  int rc, attempt;

  hints.ai_family   = AF_INET;   /* SDK getaddrinfo is IPv4-only */
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  for (attempt = 1; attempt <= RESOLVE_ATTEMPTS; attempt++) {
    if (!(rc = getaddrinfo(host, port, &hints, res))) return 0;

    if (attempt < RESOLVE_ATTEMPTS) {
      log_debug("nntp: getaddrinfo(%s) attempt %d/%d: %s, retrying",
                host, attempt, RESOLVE_ATTEMPTS, gai_strerror(rc));
      usleep(RESOLVE_RETRY_DELAY_USEC);
    } else {
      log_error("nntp: getaddrinfo(%s): %s (after %d attempt(s))",
                host, gai_strerror(rc), RESOLVE_ATTEMPTS);
    }
  }

  return -1;
}

static int
tcp_connect(const char *host, const char *port, int connect_timeout_sec, int read_timeout_sec) {
  struct addrinfo *res = 0;
  struct timeval tv = { .tv_sec = read_timeout_sec > 0 ? read_timeout_sec : 30 };
  int fd;

  log_debug("nntp: resolving %s ...", host);
  if (resolve_host(host, port, &res) < 0) return -1;

  char ip[INET_ADDRSTRLEN] = "?";
  inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, ip, sizeof ip);
  log_debug("nntp: %s resolved to %s, connecting to port %s ...", host, ip, port);

  if ((fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol)) < 0) {
    log_error("nntp: socket: %s", strerror(errno));
    freeaddrinfo(res);
    return -1;
  }

  /* SO_SNDTIMEO doubles as the connect() timeout on this SDK -- there is no
   * separate non-blocking-connect path wired up here (matches the probe). */
  struct timeval ctv = { .tv_sec = connect_timeout_sec > 0 ? connect_timeout_sec : 15 };
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &ctv, sizeof ctv);
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &(int){1}, sizeof(int));
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));

  /* Default socket buffer is small enough to itself cap throughput via the
   * TCP receive window -- set it explicitly. */
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &(int){SOCK_RCVBUF_SIZE}, sizeof(int));
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &(int){SOCK_SNDBUF_SIZE}, sizeof(int));

  if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
    log_error("nntp: connect %s:%s: %s", host, port, strerror(errno));
    close(fd);
    freeaddrinfo(res);
    return -1;
  }

  /* Now that we're connected, switch to the steady-state read timeout. */
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

  freeaddrinfo(res);
  log_debug("nntp: TCP connected to %s:%s", host, port);
  return fd;
}

/* Reads one CRLF-terminated line, CR stripped, LF consumed and not stored.
 * Shared by nntp_conn_read_line() and the body-line reader below. */

/* Refills c->rbuf with up to RBUF_SIZE freshly received bytes. Returns
 * the byte count (> 0) on success, 0 on EOF, -1 on a transport error --
 * same contract as recv()/SSL_read(). */
static int
fill_buffer(nntp_conn_t *c) {
  int n = c->ssl ? SSL_read(c->ssl, c->rbuf, sizeof c->rbuf)
                 : (int)recv(c->fd, c->rbuf, sizeof c->rbuf, 0);
  if (n <= 0) return n;

  c->rbuf_pos = 0;
  c->rbuf_len = (size_t)n;
  return n;
}

static int
raw_read_line(nntp_conn_t *c, char *buf, size_t size) {
  size_t off = 0;

  while (off + 1 < size) {
    char ch;

    if (c->rbuf_pos >= c->rbuf_len && fill_buffer(c) <= 0) return -1;

    ch = c->rbuf[c->rbuf_pos++];
    if (ch == '\r') continue;
    if (ch == '\n') break;
    buf[off++] = ch;
  }
  buf[off] = 0;
  return (int)off;
}

int
nntp_conn_read_line(nntp_conn_t *c, char *buf, size_t size) {
  return raw_read_line(c, buf, size);
}

static int
conn_send(nntp_conn_t *c, const void *data, size_t len) {
  if (c->ssl) return SSL_write(c->ssl, data, (int)len) == (int)len ? 0 : -1;
  return send(c->fd, data, len, 0) == (ssize_t)len ? 0 : -1;
}

int
nntp_conn_cmd(nntp_conn_t *c, const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  int len;

  va_start(ap, fmt);
  len = vsnprintf(buf, sizeof buf - 3, fmt, ap);
  va_end(ap);

  if (len < 0) return -1;

  memcpy(buf + len, "\r\n", 3);
  len += 2;

  return conn_send(c, buf, len);
}

nntp_conn_t *
nntp_conn_open(const nntp_conn_opts_t *opts, char *greeting_out, size_t greeting_out_size) {
  nntp_conn_t *c = calloc(1, sizeof *c);
  char line[4096];

  if (!c) return NULL;
  c->fd = -1;

  c->fd = tcp_connect(opts->host, opts->port, opts->connect_timeout_sec, opts->read_timeout_sec);
  if (c->fd < 0) {
    free(c);
    return NULL;
  }

  if (opts->tls) {
    SSL_library_init();
    SSL_load_error_strings();

    if (!(c->ssl_ctx = SSL_CTX_new(TLS_client_method()))) {
      log_error("nntp: SSL_CTX_new failed");
      goto fail;
    }
    SSL_CTX_set_min_proto_version(c->ssl_ctx, TLS1_2_VERSION);
    /* No system CA store on this console; see nzb-payload-architecture.md
     * -- verification is intentionally disabled rather than shipping and
     * maintaining a CA bundle. */
    SSL_CTX_set_verify(c->ssl_ctx, SSL_VERIFY_NONE, NULL);

    if (!(c->ssl = SSL_new(c->ssl_ctx))) {
      log_error("nntp: SSL_new failed");
      goto fail;
    }
    SSL_set_fd(c->ssl, c->fd);
    SSL_set_tlsext_host_name(c->ssl, opts->host);   /* SNI */

    log_debug("nntp: starting TLS handshake ...");
    if (SSL_connect(c->ssl) != 1) {
      log_error("nntp: TLS handshake FAILED");
      log_ssl_errors();
      goto fail;
    }
    log_debug("nntp: TLS established: %s / %s", SSL_get_version(c->ssl), SSL_get_cipher(c->ssl));
  }

  if (raw_read_line(c, line, sizeof line) < 0) {
    log_error("nntp: no greeting received from %s", opts->host);
    goto fail;
  }
  log_debug("nntp: S: %s", line);

  if (greeting_out && greeting_out_size) {
    snprintf(greeting_out, greeting_out_size, "%s", line);
  }

  return c;

fail:
  nntp_conn_close(c);
  return NULL;
}

int
nntp_conn_authenticate(nntp_conn_t *c, const char *user, const char *pass) {
  char line[4096];
  int code;

  log_debug("nntp: authenticating as '%s' ...", user);

  if (nntp_conn_cmd(c, "AUTHINFO USER %s", user) < 0) return -1;
  if (raw_read_line(c, line, sizeof line) < 0) {
    log_error("nntp: AUTHINFO USER: no response");
    return -1;
  }
  log_debug("nntp: S: %s", line);
  code = atoi(line);

  if (code == 281) {
    log_info("nntp: authentication OK (no password required)");
    return 0;
  }
  if (code != 381) {
    log_error("nntp: authentication FAILED (unexpected code %d after USER)", code);
    return -1;
  }

  if (nntp_conn_cmd(c, "AUTHINFO PASS %s", pass) < 0) return -1;
  if (raw_read_line(c, line, sizeof line) < 0) {
    log_error("nntp: AUTHINFO PASS: no response");
    return -1;
  }
  log_debug("nntp: S: %s", line);
  code = atoi(line);

  if (code == 281) {
    log_info("nntp: authentication OK");
    return 0;
  }

  log_error("nntp: authentication FAILED (code %d after PASS)", code);
  return -1;
}

/* Reads one body line with dot-unstuffing applied, storing the resulting
 * length (after unstuffing) in *out_len. Returns 1 for a normal line
 * (stored in buf), 0 for the terminating bare ".", -1 on transport
 * failure. */
static int
read_body_line(nntp_conn_t *c, char *buf, size_t size, size_t *out_len) {
  int n = raw_read_line(c, buf, size);

  if (n < 0) return -1;
  if (buf[0] == '.' && buf[1] == 0) return 0;
  if (buf[0] == '.' && buf[1] == '.') {
    memmove(buf, buf + 1, (size_t)n); /* shifts the NUL at buf[n] too */
    n -= 1;
  }

  *out_len = (size_t)n;
  return 1;
}

int
nntp_conn_fetch(nntp_conn_t *c, const char *cmd, const char *message_id,
                 nntp_body_line_cb body_cb, void *ctx) {
  char line[8192];
  int code, n, aborted = 0;

  if (nntp_conn_cmd(c, "%s %s", cmd, message_id) < 0) return -1;
  if ((n = raw_read_line(c, line, sizeof line)) < 0) {
    log_error("nntp: %s %s: no response", cmd, message_id);
    return -1;
  }
  code = atoi(line);

  /* ARTICLE (220) and BODY (222) are followed by a dot-terminated
   * multi-line block; STAT (223) and every error code are not. */
  if (code != 220 && code != 222) {
    if (code >= 400) log_debug("nntp: %s %s -> %d", cmd, message_id, code);
    return code;
  }

  for (;;) {
    size_t line_len = 0;
    int r = read_body_line(c, line, sizeof line, &line_len);
    if (r < 0) {
      log_error("nntp: %s %s: transport error mid-body", cmd, message_id);
      return -1;
    }
    if (r == 0) break;
    if (!aborted && body_cb && body_cb(ctx, line, line_len) != 0) {
      aborted = 1; /* keep draining so the connection stays in sync */
    }
  }

  return code;
}

void
nntp_conn_close(nntp_conn_t *c) {
  if (!c) return;

  if (c->fd >= 0) {
    /* Best-effort QUIT; ignore the outcome, we're tearing down regardless. */
    nntp_conn_cmd(c, "QUIT");
  }

  if (c->ssl) { SSL_shutdown(c->ssl); SSL_free(c->ssl); }
  if (c->ssl_ctx) SSL_CTX_free(c->ssl_ctx);
  if (c->fd >= 0) close(c->fd);

  free(c);
}
