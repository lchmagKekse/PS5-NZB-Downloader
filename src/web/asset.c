/* Copyright (C) 2024 John Törnblom, adapted for this project.
 * See asset.h. */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <microhttpd.h>

#include "asset.h"
#include "httpd.h"

#define PAGE_404 "<html><head><title>Not found</title></head><body>Not found</body></html>"

typedef struct asset {
  const char   *path;
  const char   *mime;
  void         *data;
  size_t        size;
  struct asset *next;
} asset_t;

static asset_t *g_asset_head = 0;

static void
asset_normalize_path(const char *url, char *path) {
  char *ptr = path;
  size_t i;

  for (i = 0; i < strlen(url); i++) {
    if (url[i] == '/' && url[i + 1] == '/') continue;
    *ptr++ = url[i];
  }
  *ptr = 0;
}

void
asset_register(const char *path, void *data, size_t size, const char *mime) {
  asset_t *a = calloc(1, sizeof *a);

  a->path = path;
  a->mime = mime;
  a->data = data;
  a->size = size;
  a->next = g_asset_head;

  g_asset_head = a;
}

enum MHD_Result
asset_request(struct MHD_Connection *conn, const char *url) {
  unsigned int status = MHD_HTTP_NOT_FOUND;
  enum MHD_Result ret = MHD_NO;
  size_t size = strlen(PAGE_404);
  struct MHD_Response *resp;
  void *data = (void *)PAGE_404;
  const char *mime = "text/html";
  char path[PATH_MAX];
  asset_t *a;

  asset_normalize_path(url, path);
  for (a = g_asset_head; a; a = a->next) {
    if (!strcmp(path, a->path)) {
      data = a->data;
      size = a->size;
      mime = a->mime;
      status = MHD_HTTP_OK;
      break;
    }
  }

  if ((resp = MHD_create_response_from_buffer(size, data, MHD_RESPMEM_PERSISTENT))) {
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, mime);
    ret = httpd_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
  }

  return ret;
}
