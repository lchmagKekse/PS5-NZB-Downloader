#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../log/log.h"
#include "api.h"
#include "httpd.h"
#include "json_util.h"

static void
append_line_cb(void *ctx, const char *line) {
  cJSON *arr = ctx;
  cJSON_AddItemToArray(arr, cJSON_CreateString(line));
}

enum MHD_Result
api_logs_get(struct MHD_Connection *conn) {
  cJSON *arr = cJSON_CreateArray();

  log_for_each_recent(append_line_cb, arr);

  return json_respond(conn, MHD_HTTP_OK, arr);
}

enum MHD_Result
api_logs_download(struct MHD_Connection *conn) {
  const char *path = log_get_path();
  struct MHD_Response *resp;
  struct stat st;
  enum MHD_Result ret;
  char disposition[80], ts[32];
  time_t now;
  struct tm tmv;
  int fd;

  if (!path[0]) {
    return json_respond_error(conn, MHD_HTTP_NOT_FOUND, "no logfile (log_init() had no path or failed to open it)");
  }

  if ((fd = open(path, O_RDONLY)) < 0) {
    return json_respond_error(conn, MHD_HTTP_NOT_FOUND, "logfile not readable");
  }
  if (fstat(fd, &st) != 0) {
    close(fd);
    return json_respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "could not stat logfile");
  }

  /* MHD_create_response_from_fd takes ownership of fd (closes it once the
   * response is done), same idiom as asset_request()'s buffer response. */
  if (!(resp = MHD_create_response_from_fd((size_t)st.st_size, fd))) {
    close(fd);
    return json_respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "could not create response");
  }

  /* Timestamp is download time (file has per-line timestamps already) --
   * keeps successive downloads from overwriting each other by name. */
  now = time(NULL);
  localtime_r(&now, &tmv);
  strftime(ts, sizeof ts, "%Y-%m-%d_%H-%M-%S", &tmv);
  snprintf(disposition, sizeof disposition, "attachment; filename=\"nzb-%s.log\"", ts);

  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "text/plain");
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_DISPOSITION, disposition);
  ret = httpd_queue_response(conn, MHD_HTTP_OK, resp);
  MHD_destroy_response(resp);

  return ret;
}
