/* Decrypts RAR5 archives with encrypted headers ("Encrypt file names" in
 * WinRAR) -- libarchive's RAR5 reader can't open these at all (no
 * decryption support of any kind, confirmed upstream:
 * libarchive/libarchive#1374). Implements the RAR5 crypto layer (per
 * rarlab's unrar crypt5.cpp/arcread.cpp) as a custom archive_read_callback
 * source, decrypting each block on demand -- one read pass, unlike an
 * earlier scratch-copy-to-disk approach. See rar5_crypt.c's top comment
 * for the on-disk layout. */
#pragma once

#include <stddef.h>

#include "../queue/job.h"

struct archive;

/* True if the volume's first block is HEAD_CRYPT (encrypted headers) --
 * the only way to detect this before a real open, since libarchive
 * discards the distinction as "Encryption is not supported". False for a
 * plain or data-only-encrypted (libarchive handles that) archive. */
int rar5_headers_encrypted(const char *first_volume_path);

/* Registers a custom read source on `a` that decrypts volumes on the fly
 * as libarchive requests data (the callback-source equivalent of
 * archive_read_open_filenames()), trying job->passwords[] against each
 * volume's embedded password-check value. *out_ctx is always set and must
 * be passed to rar5_stream_free() exactly once after the caller is done
 * with `a`. Returns ARCHIVE_OK/ARCHIVE_FATAL; archive_error_string(a)
 * has the reason on failure. */
int rar5_open_encrypted(struct archive *a, const job_t *job, const char **volumes,
                         void **out_ctx);

/* Frees everything rar5_open_encrypted() allocated for one archive_read
 * session (safe to call with ctx == NULL, e.g. when the archive being
 * opened never turned out to be header-encrypted in the first place). */
void rar5_stream_free(void *ctx);
