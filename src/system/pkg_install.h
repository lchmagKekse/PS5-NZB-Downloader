/* Installs a .pkg file already sitting on disk via the console's own
 * libSceAppInstUtil -- the same API ps5-payload-dev/websrv's PKGInstall
 * homebrew uses (see homebrew/PKGInstall/pkg_install.c in that repo, and
 * sdk/samples/install_app here for the sibling AppInstallTitleDir call).
 * Exists because NZB releases for jailbroken-PS5 titles often ship the
 * .pkg loose alongside (or instead of) a param.json title layout, so
 * extraction alone doesn't get it onto the console -- see web/api_jobs.c's
 * install-pkgs action.
 */
#pragma once

#include <stddef.h>

/* Installs the .pkg at path (an absolute filesystem path, not a URL --
 * sceAppInstUtilInstallByPackage accepts either, but every caller here
 * already has a local path from job_ensure_pkg_scanned()). Returns 0 on
 * success, -1 on failure with a short reason written to err. Safe to call
 * repeatedly across the process lifetime: sceAppInstUtilInitialize() only
 * actually runs once, on the first call. */
int pkg_install_file(const char *path, char *err, size_t err_size);
