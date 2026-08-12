/* MHD lifecycle + routing. Uses a manual accept() loop feeding
 * MHD_add_connection rather than MHD_start_daemon binding its own
 * socket, mirroring ps5-payload-dev/websrv's websrv_listen() -- the one
 * proven-on-this-platform reference for libmicrohttpd on this SDK.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <microhttpd.h>

#include "../log/log.h"
#include "api.h"
#include "asset.h"
#include "httpd.h"
#include "json_util.h"

#define UPLOAD_FIELD_NAME "nzb"
#define DISPLAY_NAME_FIELD_NAME "display_name"
#define OUTPUT_DIR_FIELD_NAME "output_dir"
#define ADD_SHADOWMOUNT_FIELD_NAME "add_to_shadowmount"

typedef struct {
  int   is_multipart;
  struct MHD_PostProcessor *pp;

  char   *raw_body;
  size_t  raw_body_len;
  size_t  raw_body_cap;

  unsigned char *upload_data;
  size_t         upload_len;
  size_t         upload_cap;
  char           upload_filename[256];

  /* Optional multipart fields; each assumed short enough to arrive in a
   * single upload_iterator() call (off==0 handled only). */
  char           display_name[256];  /* rename-on-add modal's confirmed name */
  char           output_dir[512];    /* add-NZB modal's "Output folder" input */
  int            add_to_shadowmount; /* "1" if the modal's checkbox was checked */
} conn_ctx_t;

static volatile int g_stop = 0;

enum MHD_Result
httpd_queue_response(struct MHD_Connection *conn, unsigned int status, struct MHD_Response *resp) {
  return MHD_queue_response(conn, status, resp);
}

static int
buf_append(void **buf, size_t *len, size_t *cap, const void *data, size_t n) {
  if (*len + n > *cap) {
    size_t new_cap = *cap ? *cap * 2 : 4096;
    void *grown;

    while (new_cap < *len + n) new_cap *= 2;
    if (!(grown = realloc(*buf, new_cap))) return -1;

    *buf = grown;
    *cap = new_cap;
  }

  memcpy((char *)*buf + *len, data, n);
  *len += n;
  return 0;
}

static enum MHD_Result
upload_iterator(void *cls, enum MHD_ValueKind kind, const char *key,
                 const char *filename, const char *content_type, const char *encoding,
                 const char *value, uint64_t off, size_t size) {
  conn_ctx_t *ctx = cls;

  (void)kind; (void)content_type; (void)encoding;

  if (!strcmp(key, DISPLAY_NAME_FIELD_NAME)) {
    if (off == 0) {
      size_t n = size < sizeof(ctx->display_name) - 1 ? size : sizeof(ctx->display_name) - 1;
      memcpy(ctx->display_name, value, n);
      ctx->display_name[n] = 0;
    }
    return MHD_YES;
  }

  if (!strcmp(key, OUTPUT_DIR_FIELD_NAME)) {
    if (off == 0) {
      size_t n = size < sizeof(ctx->output_dir) - 1 ? size : sizeof(ctx->output_dir) - 1;
      memcpy(ctx->output_dir, value, n);
      ctx->output_dir[n] = 0;
    }
    return MHD_YES;
  }

  if (!strcmp(key, ADD_SHADOWMOUNT_FIELD_NAME)) {
    if (off == 0 && size > 0) ctx->add_to_shadowmount = (value[0] == '1');
    return MHD_YES;
  }

  if (strcmp(key, UPLOAD_FIELD_NAME)) return MHD_YES;

  if (off == 0 && filename) {
    snprintf(ctx->upload_filename, sizeof ctx->upload_filename, "%s", filename);
  }

  if (buf_append((void **)&ctx->upload_data, &ctx->upload_len, &ctx->upload_cap, value, size) < 0) {
    log_error("httpd: out of memory buffering upload");
    return MHD_NO;
  }

  return MHD_YES;
}

/* Splits "/api/jobs/<id>/<action>" (as encountered after the "/api/jobs/"
 * prefix has already been stripped) into id/action. Returns 0 on success,
 * -1 if rest has no '/' or id would overflow out_id. */
static int
split_id_action(const char *rest, char *out_id, size_t out_id_size, const char **out_action) {
  const char *slash = strchr(rest, '/');
  size_t id_len;

  if (!slash) return -1;

  id_len = (size_t)(slash - rest);
  if (id_len == 0 || id_len >= out_id_size) return -1;

  memcpy(out_id, rest, id_len);
  out_id[id_len] = 0;
  *out_action = slash + 1;

  return 0;
}

static enum MHD_Result
route_get(struct MHD_Connection *conn, const char *url) {
  if (!strcmp(url, "/api/status"))        return api_status_get(conn);
  if (!strcmp(url, "/api/jobs"))          return api_jobs_list(conn);
  if (!strcmp(url, "/api/config"))        return api_config_get(conn);
  if (!strcmp(url, "/api/logs"))          return api_logs_get(conn);
  if (!strcmp(url, "/api/logs/download")) return api_logs_download(conn);

  if (!strncmp(url, "/api/jobs/", 10)) {
    const char *id = url + 10;
    if (id[0] == 0) return json_respond_error(conn, MHD_HTTP_NOT_FOUND, "missing job id");
    return api_jobs_get(conn, id);
  }

  if (!strncmp(url, "/api/", 5)) {
    return json_respond_error(conn, MHD_HTTP_NOT_FOUND, "unknown API route");
  }

  if (!strcmp(url, "/") || !url[0]) return asset_request(conn, "/index.html");
  return asset_request(conn, url);
}

static enum MHD_Result
route_post(struct MHD_Connection *conn, const char *url, conn_ctx_t *ctx) {
  if (!strcmp(url, "/api/jobs") || !strcmp(url, "/api/upload")) {
    if (!ctx->upload_data) {
      return json_respond_error(conn, MHD_HTTP_BAD_REQUEST,
                                 "missing '" UPLOAD_FIELD_NAME "' file field");
    }
    return api_jobs_create(conn, ctx->upload_data, ctx->upload_len, ctx->upload_filename,
                            ctx->display_name, ctx->output_dir, ctx->add_to_shadowmount);
  }

  if (!strcmp(url, "/api/config")) {
    return api_config_post(conn, ctx->raw_body, ctx->raw_body_len);
  }

  if (!strncmp(url, "/api/jobs/", 10)) {
    char id[64];
    const char *action;

    if (split_id_action(url + 10, id, sizeof id, &action) < 0) {
      return json_respond_error(conn, MHD_HTTP_BAD_REQUEST, "malformed job action URL");
    }

    if (!strcmp(action, "pause"))  return api_jobs_pause(conn, id);
    if (!strcmp(action, "resume")) return api_jobs_resume(conn, id);
    if (!strcmp(action, "cancel")) return api_jobs_cancel(conn, id);
    if (!strcmp(action, "retry"))  return api_jobs_retry(conn, id);

    return json_respond_error(conn, MHD_HTTP_NOT_FOUND, "unknown job action");
  }

  return json_respond_error(conn, MHD_HTTP_NOT_FOUND, "unknown API route");
}

static enum MHD_Result
route_delete(struct MHD_Connection *conn, const char *url) {
  if (!strncmp(url, "/api/jobs/", 10)) {
    const char *id = url + 10;
    if (id[0] == 0) return json_respond_error(conn, MHD_HTTP_BAD_REQUEST, "missing job id");
    return api_jobs_delete(conn, id);
  }

  return json_respond_error(conn, MHD_HTTP_NOT_FOUND, "unknown API route");
}

static enum MHD_Result
on_request(void *cls, struct MHD_Connection *conn, const char *url, const char *method,
           const char *version, const char *upload_data, size_t *upload_data_size,
           void **con_cls) {
  conn_ctx_t *ctx = *con_cls;

  (void)cls; (void)version;

  if (!ctx) {
    const char *content_type;

    if (!(ctx = calloc(1, sizeof *ctx))) return MHD_NO;
    *con_cls = ctx;

    content_type = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, MHD_HTTP_HEADER_CONTENT_TYPE);
    if (!strcmp(method, MHD_HTTP_METHOD_POST) &&
        content_type && !strncmp(content_type, "multipart/form-data", sizeof("multipart/form-data") - 1)) {
      ctx->is_multipart = 1;
      ctx->pp = MHD_create_post_processor(conn, 64 * 1024, upload_iterator, ctx);
    }

    return MHD_YES;
  }

  if (!strcmp(method, MHD_HTTP_METHOD_POST) && *upload_data_size) {
    enum MHD_Result r = MHD_YES;

    if (ctx->is_multipart && ctx->pp) {
      r = MHD_post_process(ctx->pp, upload_data, *upload_data_size);
    } else if (buf_append((void **)&ctx->raw_body, &ctx->raw_body_len, &ctx->raw_body_cap,
                           upload_data, *upload_data_size) < 0) {
      r = MHD_NO;
    }

    *upload_data_size = 0;
    return r;
  }

  if (!strcmp(method, MHD_HTTP_METHOD_GET) || !strcmp(method, MHD_HTTP_METHOD_HEAD)) {
    return route_get(conn, url);
  }
  if (!strcmp(method, MHD_HTTP_METHOD_POST)) {
    return route_post(conn, url, ctx);
  }
  if (!strcmp(method, MHD_HTTP_METHOD_DELETE)) {
    return route_delete(conn, url);
  }

  return MHD_NO;
}

static void
on_request_completed(void *cls, struct MHD_Connection *conn, void **con_cls,
                      enum MHD_RequestTerminationCode toe) {
  conn_ctx_t *ctx = *con_cls;

  (void)cls; (void)conn; (void)toe;

  if (!ctx) return;

  if (ctx->pp) MHD_destroy_post_processor(ctx->pp);
  free(ctx->raw_body);
  free(ctx->upload_data);
  free(ctx);
}

void
httpd_stop(void) {
  g_stop = 1;
}

int
httpd_listen(unsigned short port) {
  struct sockaddr_in addr = {0};
  struct MHD_Daemon *httpd;
  struct timeval tv = { .tv_sec = 1 };
  int srvfd;

  if ((srvfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    log_error("httpd: socket: %s", strerror(errno));
    return -1;
  }

  setsockopt(srvfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
  setsockopt(srvfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if (bind(srvfd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    log_error("httpd: bind(:%u): %s", port, strerror(errno));
    close(srvfd);
    return -1;
  }

  if (listen(srvfd, 16) != 0) {
    log_error("httpd: listen: %s", strerror(errno));
    close(srvfd);
    return -1;
  }

  /* Explicit stack size for the same reason as nntp_pool's workers
   * (nntp_pool.c): default-attribute threads crashed with stack overflow. */
  httpd = MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION | MHD_USE_ITC |
                            MHD_USE_NO_LISTEN_SOCKET | MHD_USE_INTERNAL_POLLING_THREAD,
                            0, NULL, NULL, &on_request, NULL,
                            MHD_OPTION_THREAD_STACK_SIZE, (size_t)(1024 * 1024),
                            MHD_OPTION_NOTIFY_COMPLETED, &on_request_completed, NULL,
                            MHD_OPTION_END);
  if (!httpd) {
    log_error("httpd: MHD_start_daemon failed");
    close(srvfd);
    return -1;
  }

  log_info("httpd: listening on port %u", port);

  while (!g_stop) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof client_addr;
    int connfd = accept(srvfd, (struct sockaddr *)&client_addr, &addr_len);

    if (connfd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
      log_error("httpd: accept: %s", strerror(errno));
      break;
    }

    if (MHD_add_connection(httpd, connfd, (struct sockaddr *)&client_addr, addr_len) != MHD_YES) {
      log_warn("httpd: MHD_add_connection failed");
      close(connfd);
    }
  }

  MHD_stop_daemon(httpd);
  close(srvfd);

  log_info("httpd: stopped");
  return 0;
}
