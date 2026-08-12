#include <stdio.h>
#include <string.h>

#include "../log/log.h"
#include "api.h"
#include "app_state.h"
#include "json_util.h"

static cJSON *
config_to_json(const app_config_t *cfg) {
  cJSON *root = cJSON_CreateObject();
  cJSON *nntp = cJSON_CreateObject();
  cJSON *storage = cJSON_CreateObject();
  cJSON *queue = cJSON_CreateObject();

  cJSON_AddStringToObject(nntp, "host", cfg->nntp.host);
  cJSON_AddStringToObject(nntp, "port", cfg->nntp.port);
  cJSON_AddBoolToObject(nntp, "tls", cfg->nntp.tls);
  cJSON_AddStringToObject(nntp, "user", cfg->nntp.user);
  /* password intentionally omitted -- see config.h's app_config_redacted() */
  cJSON_AddNumberToObject(nntp, "max_connections", cfg->nntp.max_connections);
  /* connect_timeout_sec/read_timeout_sec/retry_count are fixed constants
   * (see app_config_set_defaults()), not part of the Settings page --
   * intentionally not exposed here. */

  cJSON_AddStringToObject(storage, "output_dir", cfg->storage.output_dir);
  cJSON_AddStringToObject(storage, "temp_dir", cfg->storage.temp_dir);

  cJSON_AddNumberToObject(queue, "max_retries", cfg->queue.max_retries);

  cJSON_AddItemToObject(root, "nntp", nntp);
  cJSON_AddItemToObject(root, "storage", storage);
  cJSON_AddItemToObject(root, "queue", queue);

  return root;
}

enum MHD_Result
api_config_get(struct MHD_Connection *conn) {
  app_config_t redacted;

  config_lock();
  redacted = app_config_redacted(&g_app.config);
  config_unlock();

  return json_respond(conn, MHD_HTTP_OK, config_to_json(&redacted));
}

static void
apply_string(cJSON *obj, const char *key, char *dest, size_t dest_size) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (cJSON_IsString(v)) snprintf(dest, dest_size, "%s", v->valuestring);
}

static void
apply_int(cJSON *obj, const char *key, int *dest) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (cJSON_IsNumber(v)) *dest = v->valueint;
  else if (cJSON_IsBool(v)) *dest = cJSON_IsTrue(v);
}

enum MHD_Result
api_config_post(struct MHD_Connection *conn, const char *body, size_t body_len) {
  cJSON *root, *nntp, *storage, *queue, *password;
  app_config_t updated, redacted;
  int nntp_changed;

  if (!body || body_len == 0) {
    return json_respond_error(conn, MHD_HTTP_BAD_REQUEST, "empty request body");
  }

  if (!(root = cJSON_ParseWithLength(body, body_len))) {
    return json_respond_error(conn, MHD_HTTP_BAD_REQUEST, "invalid JSON");
  }

  config_lock();
  updated = g_app.config;
  config_unlock();

  if ((nntp = cJSON_GetObjectItemCaseSensitive(root, "nntp"))) {
    apply_string(nntp, "host", updated.nntp.host, sizeof updated.nntp.host);
    apply_string(nntp, "port", updated.nntp.port, sizeof updated.nntp.port);
    apply_int(nntp, "tls", &updated.nntp.tls);
    apply_string(nntp, "user", updated.nntp.user, sizeof updated.nntp.user);
    apply_int(nntp, "max_connections", &updated.nntp.max_connections);
    /* connect_timeout_sec/read_timeout_sec/retry_count are fixed, not
     * settable via this endpoint. */

    /* Only overwrite the password if the client sent a non-empty one --
     * config_to_json() never returns the real value, so the form field
     * round-trips empty unless the user is deliberately changing it. */
    password = cJSON_GetObjectItemCaseSensitive(nntp, "password");
    if (cJSON_IsString(password) && password->valuestring[0]) {
      snprintf(updated.nntp.pass, sizeof updated.nntp.pass, "%s", password->valuestring);
    }
  }

  if ((storage = cJSON_GetObjectItemCaseSensitive(root, "storage"))) {
    apply_string(storage, "output_dir", updated.storage.output_dir, sizeof updated.storage.output_dir);
    apply_string(storage, "temp_dir", updated.storage.temp_dir, sizeof updated.storage.temp_dir);
  }

  if ((queue = cJSON_GetObjectItemCaseSensitive(root, "queue"))) {
    apply_int(queue, "max_retries", &updated.queue.max_retries);
  }

  cJSON_Delete(root);

  if (updated.nntp.max_connections < 1) updated.nntp.max_connections = 1;
  if (updated.queue.max_retries < 0) updated.queue.max_retries = 0;

  config_lock();
  /* memcmp is safe: nntp_config_t is plain fixed-size fields, no pointers.
   * Avoids rebuilding the pool when only storage/queue settings changed. */
  nntp_changed = memcmp(&g_app.config.nntp, &updated.nntp, sizeof updated.nntp) != 0;
  g_app.config = updated;
  app_config_save(g_app.config_path, &g_app.config);
  redacted = app_config_redacted(&g_app.config);
  if (nntp_changed) g_app.pool_reload_needed = 1;
  config_unlock();

  /* Applied by app_reload_pool_if_needed(), called between jobs (never
   * mid-download, see app_state.h). */
  if (nntp_changed) {
    log_info("api_config: NNTP settings changed -- pool will be rebuilt automatically "
             "(now if idle, otherwise once the current download finishes)");
  }

  return json_respond(conn, MHD_HTTP_OK, config_to_json(&redacted));
}
