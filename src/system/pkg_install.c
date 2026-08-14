#include <stdio.h>

#include "pkg_install.h"

/* AppInstUtil's real implementation lives inside libSceIpmi.so on the
 * console -- same forced NEEDED entry as ps5-payload-dev/websrv's
 * PKGInstall homebrew (its Makefile links -lSceIpmi -lSceAppInstUtil for
 * the same reason; see the matching pragma in
 * sdk/sce_stubs/libSceAppInstUtil.c). */
#pragma comment(lib, "libSceIpmi.so")

/* Struct layouts and the two calls below are copied from
 * ps5-payload-dev/websrv's homebrew/PKGInstall/pkg_install.c, the one
 * confirmed-working reference for this call on this SDK -- only .uri is
 * ever populated there (a local path or an http(s) URL), everything else
 * is passed zeroed/empty, so that's all this wrapper sets too. */
typedef struct pkg_metadata {
  const char *uri;
  const char *ex_uri;
  const char *playgo_scenario_id;
  const char *content_id;
  const char *content_name;
  const char *icon_url;
} pkg_metadata_t;

typedef struct pkg_info {
  char content_id[48];
  int  type;
  int  platform;
} pkg_info_t;

typedef struct playgo_info {
  char lang[8][30];
  char scenario_ids[3][64];
  char content_ids[64];
  long unknown[810];
} playgo_info_t;

int sceAppInstUtilInitialize(void);
int sceAppInstUtilInstallByPackage(const pkg_metadata_t *, pkg_info_t *, playgo_info_t *);

static int g_initialized = 0;

int
pkg_install_file(const char *path, char *err, size_t err_size) {
  pkg_metadata_t meta = {
    .uri = path, .ex_uri = "", .playgo_scenario_id = "",
    .content_id = "", .content_name = "", .icon_url = "",
  };
  pkg_info_t info = {0};
  playgo_info_t playgo = {0};
  int rc;

  if (!g_initialized) {
    if ((rc = sceAppInstUtilInitialize()) != 0) {
      snprintf(err, err_size, "sceAppInstUtilInitialize failed (0x%x)", rc);
      return -1;
    }
    g_initialized = 1;
  }

  if ((rc = sceAppInstUtilInstallByPackage(&meta, &info, &playgo)) != 0) {
    snprintf(err, err_size, "install failed (0x%x)", rc);
    return -1;
  }

  return 0;
}
