/* Small glue between cJSON and libmicrohttpd responses, shared by every
 * api_*.c handler.
 */
#pragma once

#include <microhttpd.h>

#include "../vendor/cjson/cJSON.h"

/* Serializes body (compact, no pretty-printing -- this is a local admin
 * API, not something meant to be hand-read over the wire) as the
 * response for conn with the given status, sets Content-Type:
 * application/json, and deletes body. Returns what MHD_queue_response
 * returned. */
enum MHD_Result json_respond(struct MHD_Connection *conn, unsigned int status, cJSON *body);

/* Convenience for the common {"error": message} shape used on every
 * non-2xx response in this API. */
enum MHD_Result json_respond_error(struct MHD_Connection *conn, unsigned int status, const char *message);
