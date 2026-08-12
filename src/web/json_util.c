#include <stdlib.h>
#include <string.h>

#include "../log/log.h"
#include "httpd.h"
#include "json_util.h"

enum MHD_Result
json_respond(struct MHD_Connection *conn, unsigned int status, cJSON *body) {
  char *text = cJSON_PrintUnformatted(body);
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response *resp;

  cJSON_Delete(body);

  if (!text) {
    log_error("json_respond: cJSON_PrintUnformatted failed");
    return MHD_NO;
  }

  /* MHD_RESPMEM_MUST_FREE: MHD frees text (allocated by cJSON, which
   * uses the same malloc/free as us) once the response is destroyed. */
  if ((resp = MHD_create_response_from_buffer(strlen(text), text, MHD_RESPMEM_MUST_FREE))) {
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "application/json");
    ret = httpd_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
  } else {
    free(text);
  }

  return ret;
}

enum MHD_Result
json_respond_error(struct MHD_Connection *conn, unsigned int status, const char *message) {
  cJSON *body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "error", message);
  return json_respond(conn, status, body);
}
