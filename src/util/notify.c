#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "notify.h"

/* Layout/calling convention from the PS5 payload SDK's notify_debug sample
 * -- useless1 is undocumented header bytes the OSD toast renderer expects
 * before the message text. Declared extern (not via a header) since it's
 * the one raw kernel entry point this module wraps. */
typedef struct {
  char useless1[45];
  char message[3075];
} notify_request_t;

extern int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

void
notify(const char *fmt, ...) {
  notify_request_t req;
  va_list ap;

  memset(&req, 0, sizeof req);

  va_start(ap, fmt);
  vsnprintf(req.message, sizeof req.message, fmt, ap);
  va_end(ap);

  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}
