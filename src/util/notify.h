/* PS5 OSD toast notifications -- the system-level popup shown top-of-screen,
 * visible even when the web UI isn't open. Used for key lifecycle events
 * (server up, a download's start/extract/complete).
 */
#pragma once

/* Posts a toast with a printf-formatted message. Best-effort/fire-and-forget
 * -- never worth checking a return value for or failing a download over. */
void notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
