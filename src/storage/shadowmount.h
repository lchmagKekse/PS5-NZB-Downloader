/* Registers a completed job's output with shadowmount's manual mount
 * list, for the "Add to ShadowMount list" checkbox in the add-NZB modal.
 */
#pragma once

/* Recursively searches dest_dir for a PS5 title layout: a directory
 * containing sce_sys/param.json, or failing that a loose .ffpkg/.exfat/
 * .ffpfs/.ffpfsc image file (shallowest match wins). That path is appended
 * to /data/shadowmount/manual.lst (created if needed). If nothing matches,
 * nothing is appended, just logged. job_id is only used for logging. */
void shadowmount_register(const char *job_id, const char *dest_dir);
