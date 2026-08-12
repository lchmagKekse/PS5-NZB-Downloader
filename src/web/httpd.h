/* Local HTTP control API + web UI, built on libmicrohttpd (see httpd.c for
 * the connection-handling approach).
 *
 * Route handlers live in api_*.c and reach shared state through
 * app_state.h's g_app, not through parameters threaded down from here.
 */
#pragma once

#include <microhttpd.h>

/* Starts the HTTP server on port and blocks the calling thread, accepting
 * connections until httpd_stop() is called from another thread (typically
 * a signal handler). Returns 0 on a clean stop, -1 on a startup failure
 * (already logged). */
int httpd_listen(unsigned short port);

/* Causes a blocked httpd_listen() to return. Safe to call from a signal
 * handler or another thread. */
void httpd_stop(void);

/* Shared by asset.c and every api_*.c handler: queues resp as the
 * response for conn. Caller still owns resp and must MHD_destroy_response()
 * it, same as calling MHD_queue_response() directly. */
enum MHD_Result httpd_queue_response(struct MHD_Connection *conn, unsigned int status,
                                      struct MHD_Response *resp);
