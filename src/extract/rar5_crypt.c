/* RAR5 "encrypted headers" (WinRAR's "Encrypt file names") support -- see
 * rar5_crypt.h for why. Layout (RAR 5.0, per rarlab's unrar
 * arcread.cpp/crypt5.cpp): 8-byte sig, unencrypted HEAD_CRYPT (salt+KDF
 * count), then each header block as [16-byte IV][AES-256-CBC, padded to
 * 16]; file data is separately AES-256-CBC encrypted per-file if
 * FHEXTRA_CRYPT says so. Key is per-volume; every block gets a fresh IV
 * (no CBC chaining across blocks). Decrypts on demand and feeds
 * libarchive's RAR5 reader a plaintext stream via a custom
 * archive_read_callback (rar5_open_encrypted()), instead of writing a
 * scratch copy to disk first. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <archive.h>
#include <openssl/evp.h>

#include "../log/log.h"
#include "../util/crc32.h"
#include "rar5_crypt.h"

#define RAR5_SIG "Rar!\x1A\x07\x01\x00"
#define RAR5_SIG_LEN 8

#define SIZE_SALT50 16
#define SIZE_INITV 16
#define SIZE_PSWCHECK 8
#define SIZE_PSWCHECK_CSUM 4
#define CRYPT5_KDF_LG2_COUNT_MAX 24
#define CRYPT_VERSION 0

#define HEAD_MAIN 1
#define HEAD_FILE 2
#define HEAD_SERVICE 3
#define HEAD_CRYPT 4
#define HEAD_ENDARC 5

#define HFL_EXTRA 0x0001
#define HFL_DATA 0x0002
#define HFL_SPLITBEFORE 0x0008
#define HFL_SPLITAFTER 0x0010

#define FHFL_UTIME 0x0002
#define FHFL_CRC32 0x0004
#define FHFL_UNPUNKNOWN 0x0008

#define FHEXTRA_CRYPT 0x01

#define CHFL_CRYPT_PSWCHECK 0x0001

/* Not in libarchive's public archive.h (only its internal
 * archive_platform.h, as -1) -- inlined here rather than pulling in an
 * internal header for one constant. */
#define RAR5_ARCHIVE_ERRNO_MISC (-1)

/* Matches unrar's documented 2 MB ceiling for a header's size field, plus a
 * small preamble margin. Anything larger is corrupt input. */
#define MAX_HEADER_SIZE (2 * 1024 * 1024 + 64)

#define STREAM_CHUNK (256 * 1024) /* matches extract.c's own bulk-I/O sizing */

/* --- vint decode: 7-bit groups, LSB group first, high bit = "more follows" -- --
 * matches RAR5's RawRead::GetV() exactly (see unrar's rawread.cpp). */
static uint64_t
get_vint(const unsigned char *buf, size_t len, size_t *pos) {
  uint64_t result = 0;
  unsigned shift;

  for (shift = 0; *pos < len && shift < 64; shift += 7) {
    unsigned char b = buf[(*pos)++];
    result |= (uint64_t)(b & 0x7f) << shift;
    if (!(b & 0x80)) return result;
  }
  return 0;
}

static uint32_t
crc32_of(const unsigned char *data, size_t len) {
  return (uint32_t)crc32_final(crc32_update(crc32_init(), data, len));
}

/* Encodes value as a vint in exactly `width` bytes (over-long padded) so a
 * header field can shrink in place without changing the header's total
 * length or cascading offset/CRC changes (see rar5_next_header_chunk()'s
 * PackSize patch). Caller guarantees width fits value. */
static void
put_vint_fixed_width(unsigned char *buf, size_t pos, uint64_t value, size_t width) {
  size_t i;

  for (i = 0; i < width; i++) {
    unsigned char b = (unsigned char)(value & 0x7f);

    value >>= 7;
    if (i + 1 < width) b |= 0x80;
    buf[pos + i] = b;
  }
}

/* PBKDF2-HMAC-SHA256(password, salt, 2^lg2count iters) -> 32-byte AES key.
 * If check_out is non-NULL, also derives the password-check value: PBKDF2
 * at iters+32 (matches unrar's U_i-chaining optimization), XOR-folded to
 * 8 bytes per crypt5.cpp's SetKey50(). */
static void
derive_key(const char *password, const unsigned char salt[SIZE_SALT50], unsigned lg2count,
           unsigned char key_out[32], unsigned char check_out[SIZE_PSWCHECK]) {
  int iters = 1 << lg2count;

  PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt, SIZE_SALT50, iters,
                     EVP_sha256(), 32, key_out);

  if (check_out) {
    unsigned char raw[32];
    int i;

    PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt, SIZE_SALT50, iters + 32,
                       EVP_sha256(), 32, raw);
    memset(check_out, 0, SIZE_PSWCHECK);
    for (i = 0; i < 32; i++) check_out[i % SIZE_PSWCHECK] ^= raw[i];
  }
}

/* One-shot AES-256-CBC decrypt of a whole (already fully-read, block-size
 * multiple) buffer -- used for header blocks, which are always small.
 * Returns 0 on success, -1 on an OpenSSL-level failure (should not
 * normally happen given padding is off and inlen is block-aligned). */
static int
aes256cbc_decrypt(const unsigned char key[32], const unsigned char iv[16],
                   const unsigned char *in, size_t inlen, unsigned char *out) {
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  int outlen1 = 0, outlen2 = 0;
  int ok = 0;

  if (!ctx) return -1;
  if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) == 1) {
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    if (EVP_DecryptUpdate(ctx, out, &outlen1, in, (int)inlen) == 1 &&
        EVP_DecryptFinal_ex(ctx, out + outlen1, &outlen2) == 1) {
      ok = 1;
    }
  }
  EVP_CIPHER_CTX_free(ctx);
  return ok ? 0 : -1;
}

/* Reads one header block at `in`'s current 16-byte IV position, decrypts
 * with `key`, returns malloc'd plaintext (*out_len set). *crc_ok reports
 * whether the embedded CRC32 matches -- the only way to detect a wrong
 * key, since AES-CBC decrypt never fails on bad input. Returns 0 if
 * structurally read (regardless of *crc_ok), -1 on read/truncation error. */
static int
read_and_decrypt_header(FILE *in, const unsigned char key[32],
                         unsigned char **out_pt, size_t *out_len, int *crc_ok,
                         char *err, size_t err_size) {
  unsigned char iv[16];
  unsigned char first_ct[16], first_pt[16];
  size_t pos, size_bytes;
  uint64_t block_size, total_header, aligned;
  unsigned char *ct, *pt;
  uint32_t stored_crc, calc_crc;

  *out_pt = NULL;
  *out_len = 0;
  *crc_ok = 0;

  if (fread(iv, 1, 16, in) != 16) {
    snprintf(err, err_size, "truncated archive (expected another header)");
    return -1;
  }
  if (fread(first_ct, 1, 16, in) != 16) {
    snprintf(err, err_size, "truncated archive (partial header)");
    return -1;
  }
  if (aes256cbc_decrypt(key, iv, first_ct, 16, first_pt) != 0) {
    snprintf(err, err_size, "AES decrypt failed");
    return -1;
  }

  stored_crc = (uint32_t)first_pt[0] | ((uint32_t)first_pt[1] << 8) |
               ((uint32_t)first_pt[2] << 16) | ((uint32_t)first_pt[3] << 24);
  pos = 4;
  block_size = get_vint(first_pt, 16, &pos);
  size_bytes = pos - 4;

  if (block_size == 0 || size_bytes == 0) {
    snprintf(err, err_size, "corrupt archive header (bad size field)");
    return -1;
  }

  total_header = 4 + size_bytes + block_size;
  if (total_header > MAX_HEADER_SIZE) {
    snprintf(err, err_size, "corrupt archive header (implausible size)");
    return -1;
  }
  aligned = (total_header + 15) & ~(uint64_t)15;
  if (aligned < 16) aligned = 16;

  if (!(ct = malloc((size_t)aligned)) || !(pt = malloc((size_t)aligned))) {
    free(ct);
    snprintf(err, err_size, "out of memory");
    return -1;
  }
  memcpy(ct, first_ct, 16);
  if (aligned > 16 && fread(ct + 16, 1, (size_t)(aligned - 16), in) != (size_t)(aligned - 16)) {
    free(ct);
    free(pt);
    snprintf(err, err_size, "truncated archive (partial header)");
    return -1;
  }

  if (aes256cbc_decrypt(key, iv, ct, (size_t)aligned, pt) != 0) {
    free(ct);
    free(pt);
    snprintf(err, err_size, "AES decrypt failed");
    return -1;
  }
  free(ct);

  calc_crc = crc32_of(pt + 4, (size_t)(total_header - 4));
  *crc_ok = (calc_crc == stored_crc);
  *out_pt = pt;
  *out_len = (size_t)total_header;
  return 0;
}

/* Walks a decrypted HEAD_FILE/HEAD_SERVICE body to find its FHEXTRA_CRYPT
 * record (per-file salt/IV/KDF-count for the entry's data, independent of
 * the archive-level header key). field_id_pos is the record's type-tag
 * offset: the caller must neutralize it after decrypting this entry's
 * data, since libarchive's RAR5 reader refuses to read ANY entry whose
 * header still carries an FHEXTRA_CRYPT record -- even correctly-decrypted
 * plaintext (confirmed on real hardware). unp_size_out is the entry's true
 * decompressed size, needed to fix STORED entries' padding (see
 * rar5_next_header_chunk()). method_out/name_out are diagnostic-only;
 * *have_crypt stays 0 if parsing fails. */
static void
find_file_crypt(const unsigned char *body, size_t body_len, size_t extra_size, size_t after_common_pos,
                 int *have_crypt, unsigned char salt[SIZE_SALT50], unsigned char iv[SIZE_INITV],
                 unsigned *lg2count, size_t *field_id_pos,
                 uint64_t *unp_size_out, int *unp_size_known, unsigned *method_out,
                 char *name_out, size_t name_out_cap) {
  size_t pos = after_common_pos;
  uint64_t file_flags, unp_size, file_attr, comp_info, host_os, name_size;
  size_t extra_start, extra_end, ep;

  *have_crypt = 0;
  if (name_out && name_out_cap) name_out[0] = 0;
  if (method_out) *method_out = 0xff; /* unknown -- filled below once actually parsed */
  if (extra_size == 0) return;

  file_flags = get_vint(body, body_len, &pos);
  unp_size = get_vint(body, body_len, &pos);
  file_attr = get_vint(body, body_len, &pos);
  (void)file_attr;
  if (file_flags & FHFL_UTIME) pos += 4;
  if (file_flags & FHFL_CRC32) pos += 4;
  comp_info = get_vint(body, body_len, &pos);
  if (method_out) *method_out = (unsigned)((comp_info >> 7) & 7);
  host_os = get_vint(body, body_len, &pos);
  (void)host_os;
  name_size = get_vint(body, body_len, &pos);

  if (name_out && name_out_cap && pos + name_size <= body_len) {
    size_t n = name_size < name_out_cap - 1 ? (size_t)name_size : name_out_cap - 1;

    memcpy(name_out, body + pos, n);
    name_out[n] = 0;
  }
  pos += (size_t)name_size;

  if (pos > body_len) return; /* corrupt/truncated -- nothing more we can do */

  extra_start = pos;
  extra_end = extra_start + extra_size;
  if (extra_end > body_len) return;

  ep = extra_start;
  while (ep < extra_end) {
    size_t after_size_pos;
    uint64_t field_size, field_id;

    /* field_size counts bytes from right after this size vint through the
     * end of the field (id vint + data) -- NOT including the size vint's
     * own width, matching libarchive's read_var()/consume() pairing for
     * the same extra-area format (process_head_file_extra). So the next
     * field starts at after_size_pos + field_size, not ep + field_size. */
    field_size = get_vint(body, extra_end, &ep);
    after_size_pos = ep;
    if (field_size == 0 || after_size_pos + field_size > extra_end) break; /* corrupt */

    field_id = get_vint(body, extra_end, &ep);

    if (field_id == FHEXTRA_CRYPT) {
      size_t fp = ep;
      uint64_t enc_ver, enc_flags;

      enc_ver = get_vint(body, extra_end, &fp);
      enc_flags = get_vint(body, extra_end, &fp);
      (void)enc_flags;
      if (enc_ver == CRYPT_VERSION && fp < extra_end) {
        unsigned lg2 = body[fp++];

        if (lg2 <= CRYPT5_KDF_LG2_COUNT_MAX && fp + SIZE_SALT50 + SIZE_INITV <= extra_end) {
          memcpy(salt, body + fp, SIZE_SALT50);
          fp += SIZE_SALT50;
          memcpy(iv, body + fp, SIZE_INITV);
          *lg2count = lg2;
          *field_id_pos = after_size_pos;
          *unp_size_out = unp_size;
          *unp_size_known = (file_flags & FHFL_UNPUNKNOWN) == 0;
          *have_crypt = 1;
        }
      }
      break; /* found what we came for */
    }

    ep = after_size_pos + (size_t)field_size;
  }
}

int
rar5_headers_encrypted(const char *first_volume_path) {
  FILE *f = fopen(first_volume_path, "rb");
  unsigned char buf[16];
  size_t pos;
  int is_crypt = 0;

  if (!f) return 0;
  /* Signature (8) + enough of the first block's CRC32+size+type to read
   * the type vint -- 8 more bytes is comfortably enough for any real vint
   * widths here. */
  if (fread(buf, 1, 16, f) == 16 && !memcmp(buf, RAR5_SIG, RAR5_SIG_LEN)) {
    pos = RAR5_SIG_LEN + 4; /* skip CRC32 */
    get_vint(buf, 16, &pos); /* skip size vint */
    if (pos < 16) {
      uint64_t type = get_vint(buf, 16, &pos);
      is_crypt = (type == HEAD_CRYPT);
    }
  }
  fclose(f);
  return is_crypt;
}

/* Tracks a file split across a volume boundary so its last fragment's real
 * (unpadded) size can be recovered -- see rar5_next_header_chunk(). Shared
 * across all rar5_vol_t in one rar5_stream_ctx_t; at most one file is
 * "in flight" at a time. */
typedef struct {
  int active;               /* a split file's continuation is pending */
  uint64_t unp_total;        /* that file's total UnpSize, across every fragment */
  uint64_t written_so_far;   /* sum of every earlier fragment's real (assumed
                               * padding-free) size -- see the comment where
                               * this is used */
} rar5_split_state_t;

/* Per-volume state for the streaming archive_read_callback source.
 * libarchive owns none of this (client_data is opaque to it) --
 * rar5_stream_free() frees it after the caller is done with the
 * `struct archive`. */
typedef struct {
  const job_t *job;
  const char *path;              /* not owned -- borrowed from the caller's volumes[] array,
                                   * which outlives this struct (see extract_one_archive) */
  rar5_split_state_t *split_state;

  FILE *in;
  unsigned char archive_key[32];
  const char *confirmed_password;

  int done;                       /* true once this volume has nothing left to give
                                    * libarchive (HEAD_ENDARC processed, or real EOF) */
  int wrote_sig;

  /* Sub-state for whichever phase of this volume we're currently
   * streaming: parsing/rewriting the next header block, or still working
   * through the current entry's file data. */
  int in_filedata;
  uint64_t data_remaining;         /* ciphertext/passthrough bytes not yet consumed from `in`
                                     * for the entry currently in flight */
  uint64_t write_cap;               /* UINT64_MAX == no cap, see rar5_next_header_chunk() */
  uint64_t emitted_this_entry;      /* bytes already handed to libarchive for it, capped by write_cap */
  int have_file_crypt;
  int endarc_pending;                /* this entry's header was HEAD_ENDARC -- once its
                                       * data phase (if any) finishes, mark `done` */
  EVP_CIPHER_CTX *file_ctx;          /* live only while in_filedata && have_file_crypt */

  unsigned char *inbuf;               /* STREAM_CHUNK-sized scratch, reused every call */
  unsigned char *outbuf;

  unsigned char *last_header_chunk;    /* the malloc'd header buffer handed to libarchive on
                                         * the previous call, freed on the next one -- libarchive's
                                         * read-callback contract guarantees it's done with a
                                         * returned buffer by the time it asks for the next one */
} rar5_vol_t;

typedef struct {
  rar5_vol_t **vols;
  size_t count;
  rar5_split_state_t split_state;
} rar5_stream_ctx_t;

/* Releases everything owned by one rar5_vol_t -- called from both
 * rar5_vol_close_cb() and rar5_stream_free()'s defensive final pass.
 * Safe to call twice: fields are nulled after freeing. */
static void
rar5_vol_cleanup(rar5_vol_t *v) {
  if (v->last_header_chunk) { free(v->last_header_chunk); v->last_header_chunk = NULL; }
  if (v->file_ctx) { EVP_CIPHER_CTX_free(v->file_ctx); v->file_ctx = NULL; }
  free(v->inbuf); v->inbuf = NULL;
  free(v->outbuf); v->outbuf = NULL;
  if (v->in) { fclose(v->in); v->in = NULL; }
}

/* archive_open_callback for one volume: reads its unencrypted HEAD_CRYPT
 * block, finds which of job->passwords[] opens it, and leaves `v`
 * positioned at the first real header for rar5_vol_read_cb(). */
static int
rar5_vol_open_cb(struct archive *a, void *client_data) {
  rar5_vol_t *v = client_data;
  unsigned char sig[RAR5_SIG_LEN];
  unsigned char pre[7];
  size_t pos, size_bytes;
  uint64_t block_size;
  unsigned char body[128];
  size_t bp;
  uint64_t btype, bflags, crypt_ver, enc_flags;
  unsigned lg2count;
  unsigned char archive_salt[SIZE_SALT50];
  int has_pswcheck = 0;
  unsigned char stored_pswcheck[SIZE_PSWCHECK];
  long first_header_pos;
  size_t pi;
  int found_password = 0;

  if (v->job->password_count == 0) {
    archive_set_error(a, RAR5_ARCHIVE_ERRNO_MISC,
        "archive headers are encrypted but no password is configured for this job");
    return ARCHIVE_FATAL;
  }

  if (!(v->in = fopen(v->path, "rb"))) {
    archive_set_error(a, errno, "%s: %s", v->path, strerror(errno));
    return ARCHIVE_FATAL;
  }

  if (fread(sig, 1, RAR5_SIG_LEN, v->in) != RAR5_SIG_LEN || memcmp(sig, RAR5_SIG, RAR5_SIG_LEN)) {
    archive_set_error(a, RAR5_ARCHIVE_ERRNO_MISC, "%s: not a RAR5 archive", v->path);
    goto fail;
  }

  /* HEAD_CRYPT itself is NOT encrypted -- read it straight. */
  if (fread(pre, 1, 7, v->in) != 7) {
    archive_set_error(a, RAR5_ARCHIVE_ERRNO_MISC, "%s: truncated archive", v->path);
    goto fail;
  }
  pos = 4;
  block_size = get_vint(pre, 7, &pos);
  size_bytes = pos - 4;
  if (block_size == 0 || size_bytes == 0 || block_size > sizeof body) {
    archive_set_error(a, RAR5_ARCHIVE_ERRNO_MISC, "%s: corrupt or unsupported encryption header", v->path);
    goto fail;
  }
  {
    size_t have = 7 - (4 + size_bytes);

    memcpy(body, pre + 4 + size_bytes, have);
    if (block_size > have && fread(body + have, 1, (size_t)block_size - have, v->in) != (size_t)block_size - have) {
      archive_set_error(a, RAR5_ARCHIVE_ERRNO_MISC, "%s: truncated archive", v->path);
      goto fail;
    }
  }

  bp = 0;
  btype = get_vint(body, (size_t)block_size, &bp);
  bflags = get_vint(body, (size_t)block_size, &bp);
  if (btype != HEAD_CRYPT) {
    archive_set_error(a, RAR5_ARCHIVE_ERRNO_MISC, "%s: expected an encryption header, found something else", v->path);
    goto fail;
  }
  if (bflags & HFL_EXTRA) get_vint(body, (size_t)block_size, &bp);
  if (bflags & HFL_DATA) get_vint(body, (size_t)block_size, &bp);
  crypt_ver = get_vint(body, (size_t)block_size, &bp);
  if (crypt_ver != CRYPT_VERSION) {
    archive_set_error(a, RAR5_ARCHIVE_ERRNO_MISC, "%s: unsupported RAR5 encryption version", v->path);
    goto fail;
  }
  enc_flags = get_vint(body, (size_t)block_size, &bp);
  if (bp >= block_size) {
    archive_set_error(a, RAR5_ARCHIVE_ERRNO_MISC, "%s: corrupt encryption header", v->path);
    goto fail;
  }
  lg2count = body[bp++];
  if (lg2count > CRYPT5_KDF_LG2_COUNT_MAX || bp + SIZE_SALT50 > block_size) {
    archive_set_error(a, RAR5_ARCHIVE_ERRNO_MISC, "%s: corrupt encryption header", v->path);
    goto fail;
  }
  memcpy(archive_salt, body + bp, SIZE_SALT50);
  bp += SIZE_SALT50;
  if ((enc_flags & CHFL_CRYPT_PSWCHECK) && bp + SIZE_PSWCHECK + SIZE_PSWCHECK_CSUM <= block_size) {
    unsigned char csum[SIZE_PSWCHECK_CSUM];
    unsigned char digest[32];
    unsigned int dlen = 0;
    EVP_MD_CTX *mctx = EVP_MD_CTX_new();

    memcpy(stored_pswcheck, body + bp, SIZE_PSWCHECK);
    bp += SIZE_PSWCHECK;
    memcpy(csum, body + bp, SIZE_PSWCHECK_CSUM);
    bp += SIZE_PSWCHECK_CSUM;

    if (mctx) {
      EVP_DigestInit_ex(mctx, EVP_sha256(), NULL);
      EVP_DigestUpdate(mctx, stored_pswcheck, SIZE_PSWCHECK);
      EVP_DigestFinal_ex(mctx, digest, &dlen);
      EVP_MD_CTX_free(mctx);
      has_pswcheck = (memcmp(csum, digest, SIZE_PSWCHECK_CSUM) == 0);
    }
  }

  first_header_pos = ftell(v->in);

  /* Try each password: fast path compares against the embedded check
   * value; fallback decrypts the next header and trusts its CRC32 --
   * AES-CBC never "fails" on a wrong key, it just produces garbage. */
  for (pi = 0; pi < v->job->password_count; pi++) {
    unsigned char candidate_key[32], candidate_check[SIZE_PSWCHECK];

    derive_key(v->job->passwords[pi], archive_salt, lg2count, candidate_key, candidate_check);

    if (has_pswcheck) {
      if (memcmp(candidate_check, stored_pswcheck, SIZE_PSWCHECK) != 0) continue;
      memcpy(v->archive_key, candidate_key, 32);
      v->confirmed_password = v->job->passwords[pi];
      found_password = 1;
      break;
    } else {
      unsigned char *pt;
      size_t ptlen;
      int crc_ok, rc;
      char errbuf[256];

      if (fseek(v->in, first_header_pos, SEEK_SET) != 0) {
        archive_set_error(a, RAR5_ARCHIVE_ERRNO_MISC, "%s: seek failed", v->path);
        goto fail;
      }
      rc = read_and_decrypt_header(v->in, candidate_key, &pt, &ptlen, &crc_ok, errbuf, sizeof errbuf);
      free(pt);
      if (rc != 0) {
        archive_set_error(a, RAR5_ARCHIVE_ERRNO_MISC, "%s: %s", v->path, errbuf);
        goto fail;
      }
      if (crc_ok) {
        memcpy(v->archive_key, candidate_key, 32);
        v->confirmed_password = v->job->passwords[pi];
        found_password = 1;
        break;
      }
    }
  }

  if (!found_password) {
    archive_set_error(a, RAR5_ARCHIVE_ERRNO_MISC,
        "wrong or missing password for encrypted archive (tried %zu configured password(s))",
        v->job->password_count);
    goto fail;
  }

  if (fseek(v->in, first_header_pos, SEEK_SET) != 0) {
    archive_set_error(a, RAR5_ARCHIVE_ERRNO_MISC, "%s: seek failed", v->path);
    goto fail;
  }

  if (!(v->inbuf = malloc(STREAM_CHUNK)) || !(v->outbuf = malloc(STREAM_CHUNK))) {
    archive_set_error(a, ENOMEM, "out of memory");
    goto fail;
  }

  return ARCHIVE_OK;

fail:
  fclose(v->in);
  v->in = NULL;
  return ARCHIVE_FATAL;
}

/* Reads and decrypts one more header block, handing the result straight
 * back to libarchive. If it carries file data, sets up `v`'s file-data
 * sub-state so later calls go through rar5_next_filedata_chunk() instead.
 * Returns chunk length (>0), 0 at end-of-volume, -1 on error. */
static la_ssize_t
rar5_next_header_chunk(struct archive *a, rar5_vol_t *v, const void **buffer) {
  unsigned char *pt;
  size_t ptlen;
  int crc_ok, rc;
  size_t hpos;
  uint64_t htype, hflags, extra_size = 0, data_size = 0;
  size_t data_size_pos = 0, data_size_width = 0;
  char errbuf[256];

  {
    int c = fgetc(v->in);
    if (c == EOF) { v->done = 1; return 0; }
    ungetc(c, v->in);
  }

  rc = read_and_decrypt_header(v->in, v->archive_key, &pt, &ptlen, &crc_ok, errbuf, sizeof errbuf);
  if (rc != 0) {
    archive_set_error(a, RAR5_ARCHIVE_ERRNO_MISC, "%s: %s", v->path, errbuf);
    v->done = 1;
    return -1;
  }
  if (!crc_ok) {
    free(pt);
    archive_set_error(a, RAR5_ARCHIVE_ERRNO_MISC, "%s: corrupt header (checksum mismatch)", v->path);
    v->done = 1;
    return -1;
  }

  /* hpos = offset of the Type field, i.e. right after CRC32(4) + this
   * header's own size vint (whose width can differ header to header). */
  hpos = 4;
  get_vint(pt, ptlen, &hpos);
  htype = get_vint(pt, ptlen, &hpos);
  hflags = get_vint(pt, ptlen, &hpos);
  if (hflags & HFL_EXTRA) extra_size = get_vint(pt, ptlen, &hpos);
  if (hflags & HFL_DATA) {
    data_size_pos = hpos;
    data_size = get_vint(pt, ptlen, &hpos);
    data_size_width = hpos - data_size_pos;
  }

  {
    int have_file_crypt = 0;
    unsigned char file_salt[SIZE_SALT50], file_iv[SIZE_INITV];
    unsigned file_lg2count = 0;
    size_t field_id_pos = 0;
    uint64_t entry_unp_size = 0;
    int entry_unp_known = 0;
    unsigned entry_method = 0xff;
    char entry_name[256];
    uint64_t write_cap = UINT64_MAX;
    int is_split_before, is_split_after, is_endarc;

    entry_name[0] = 0;
    if (htype == HEAD_FILE || htype == HEAD_SERVICE) {
      find_file_crypt(pt, ptlen, (size_t)extra_size, hpos,
                       &have_file_crypt, file_salt, file_iv, &file_lg2count, &field_id_pos,
                       &entry_unp_size, &entry_unp_known, &entry_method,
                       entry_name, sizeof entry_name);
    }

    /* DEBUG not INFO: fires once per entry, too noisy for INFO on a large
     * archive. The one INFO-worthy fact (headers unlocked) is logged once
     * per archive instead, in rar5_open_encrypted(). */
    if (data_size > 0 && (htype == HEAD_FILE || htype == HEAD_SERVICE)) {
      log_debug("[%s] extract: rar5: entry '%s': packed=%lld unpacked=%lld (known=%d) method=%u "
                "encrypted=%d split=%d%d",
                v->job->id, entry_name[0] ? entry_name : "(unnamed)",
                (long long)data_size, (long long)entry_unp_size, entry_unp_known,
                entry_method, have_file_crypt,
                (hflags & HFL_SPLITBEFORE) != 0, (hflags & HFL_SPLITAFTER) != 0);
    }

    is_split_before = (hflags & HFL_SPLITBEFORE) != 0;
    is_split_after = (hflags & HFL_SPLITAFTER) != 0;
    is_endarc = (htype == HEAD_ENDARC);

    if (have_file_crypt) {
      /* Encryption pads data to a 16-byte boundary. Invisible for a
       * compressed entry, but STORED entries have no decoder -- PackSize
       * must shrink to match or libarchive reads padding as content,
       * consuming the next header's IV ("Too much data" on real hardware).
       * A split file's UnpSize covers the whole file, not one fragment, but
       * only the final fragment (SplitBefore set, SplitAfter clear) can
       * carry real padding -- non-final fragments are always block-aligned
       * by construction. Recovered as UnpSize minus prior fragments'
       * PackSize, tracked via *split_state. */
      if (!is_split_before && !is_split_after) {
        if (entry_unp_known && entry_unp_size < data_size) write_cap = entry_unp_size;
      } else if (is_split_after && !is_split_before) {
        /* First fragment of a new split file. */
        v->split_state->active = 1;
        v->split_state->unp_total = entry_unp_size;
        v->split_state->written_so_far = data_size;
      } else if (is_split_after) {
        /* Middle fragment. */
        if (v->split_state->active) v->split_state->written_so_far += data_size;
      } else if (v->split_state->active) {
        /* Final fragment (SplitBefore set, SplitAfter clear). */
        uint64_t true_size = v->split_state->unp_total > v->split_state->written_so_far
                                  ? v->split_state->unp_total - v->split_state->written_so_far
                                  : 0;

        if (true_size < data_size) write_cap = true_size;
        v->split_state->active = 0;
      }
    }

    if (have_file_crypt || write_cap != UINT64_MAX) {
      uint32_t new_crc;

      if (have_file_crypt) {
        /* Neutralize FHEXTRA_CRYPT in place: its mere presence makes
         * libarchive refuse to read the (now-decrypted) data. Overwrite
         * just the type-tag byte so every other header offset stays
         * unchanged. */
        pt[field_id_pos] = 0x40; /* unused FHEXTRA_* type, single byte like the original (1) */
      }
      if (write_cap != UINT64_MAX) {
        /* Same-width in-place rewrite (see put_vint_fixed_width) --
         * the header's total length, and every offset after this
         * field, stays exactly as read_and_decrypt_header() sized it. */
        put_vint_fixed_width(pt, data_size_pos, write_cap, data_size_width);
      }

      new_crc = crc32_of(pt + 4, ptlen - 4);
      pt[0] = (unsigned char)(new_crc & 0xff);
      pt[1] = (unsigned char)((new_crc >> 8) & 0xff);
      pt[2] = (unsigned char)((new_crc >> 16) & 0xff);
      pt[3] = (unsigned char)((new_crc >> 24) & 0xff);
    }

    v->last_header_chunk = pt;
    *buffer = pt;

    if (data_size > 0) {
      v->in_filedata = 1;
      v->data_remaining = data_size;
      v->write_cap = write_cap;
      v->emitted_this_entry = 0;
      v->have_file_crypt = have_file_crypt;
      v->endarc_pending = is_endarc;

      if (have_file_crypt) {
        unsigned char file_key[32];

        /* Per-file data always uses the same password that opened the
         * archive-level headers (RAR doesn't support a different
         * password per entry) -- just a different derived key, since
         * this salt is this file's own, not the archive-level one. */
        derive_key(v->confirmed_password, file_salt, file_lg2count, file_key, NULL);
        if (!(v->file_ctx = EVP_CIPHER_CTX_new()) ||
            EVP_DecryptInit_ex(v->file_ctx, EVP_aes_256_cbc(), NULL, file_key, file_iv) != 1) {
          archive_set_error(a, RAR5_ARCHIVE_ERRNO_MISC, "%s: AES init failed", v->path);
          v->done = 1;
          return -1;
        }
        EVP_CIPHER_CTX_set_padding(v->file_ctx, 0);
      }
    } else if (is_endarc) {
      v->done = 1;
    }

    return (la_ssize_t)ptlen;
  }
}

/* Produces the next chunk of the current entry's decrypted (or
 * passed-through) file data, one STREAM_CHUNK at a time. Keeps consuming
 * (and decrypting, since CBC must advance over every byte) chunks past
 * write_cap so this never returns an empty non-EOF chunk -- that signal
 * isn't representable in this API. Once data is exhausted, finalizes the
 * cipher and falls through to the next header's chunk directly. */
static la_ssize_t
rar5_next_filedata_chunk(struct archive *a, rar5_vol_t *v, const void **buffer) {
  while (v->data_remaining > 0) {
    size_t chunk = v->data_remaining > STREAM_CHUNK ? STREAM_CHUNK : (size_t)v->data_remaining;
    size_t room;

    if (fread(v->inbuf, 1, chunk, v->in) != chunk) {
      archive_set_error(a, RAR5_ARCHIVE_ERRNO_MISC, "%s: truncated archive (file data)", v->path);
      v->done = 1;
      return -1;
    }
    v->data_remaining -= chunk;

    if (v->have_file_crypt) {
      int outlen = 0;

      if (EVP_DecryptUpdate(v->file_ctx, v->outbuf, &outlen, v->inbuf, (int)chunk) != 1) {
        archive_set_error(a, RAR5_ARCHIVE_ERRNO_MISC, "%s: decrypt failed (file data)", v->path);
        v->done = 1;
        return -1;
      }
      room = v->write_cap > v->emitted_this_entry ? v->write_cap - v->emitted_this_entry : 0;
      if ((size_t)outlen < room) room = (size_t)outlen;
      if (room == 0) continue;
      v->emitted_this_entry += room;
      *buffer = v->outbuf;
      return (la_ssize_t)room;
    } else {
      room = v->write_cap > v->emitted_this_entry ? v->write_cap - v->emitted_this_entry : 0;
      if (chunk < room) room = chunk;
      if (room == 0) continue;
      v->emitted_this_entry += room;
      *buffer = v->inbuf;
      return (la_ssize_t)room;
    }
  }

  v->in_filedata = 0;

  if (v->have_file_crypt) {
    unsigned char tail[16];
    int taillen = 0;
    int ok = EVP_DecryptFinal_ex(v->file_ctx, tail, &taillen) == 1;

    EVP_CIPHER_CTX_free(v->file_ctx);
    v->file_ctx = NULL;
    if (!ok) {
      archive_set_error(a, RAR5_ARCHIVE_ERRNO_MISC, "%s: decrypt failed (file data trailer)", v->path);
      v->done = 1;
      return -1;
    }
    if (taillen > 0) {
      size_t room = v->write_cap > v->emitted_this_entry ? v->write_cap - v->emitted_this_entry : 0;

      if ((size_t)taillen < room) room = (size_t)taillen;
      if (room > 0) {
        memcpy(v->outbuf, tail, room); /* tail is <= 16 bytes; outbuf is STREAM_CHUNK-sized scratch */
        v->emitted_this_entry += room;
        if (v->endarc_pending) v->done = 1;
        *buffer = v->outbuf;
        return (la_ssize_t)room;
      }
    }
  }

  if (v->endarc_pending) { v->done = 1; return 0; }

  return rar5_next_header_chunk(a, v, buffer);
}

static la_ssize_t
rar5_vol_read_cb(struct archive *a, void *client_data, const void **buffer) {
  rar5_vol_t *v = client_data;

  /* libarchive is done consuming whatever we returned last call by the
   * time it asks again (standard archive_read_callback contract).
   * File-data buffers (v->inbuf/outbuf) are reused in place, not tracked here. */
  if (v->last_header_chunk) { free(v->last_header_chunk); v->last_header_chunk = NULL; }

  if (v->done) return 0;

  if (!v->wrote_sig) {
    /* RAR5_SIG is a string literal (static storage duration, safe to hand
     * a pointer to past this call) -- only its first RAR5_SIG_LEN bytes
     * are the real signature, the implicit trailing NUL a string literal
     * carries is never read since the caller is only told about LEN. */
    v->wrote_sig = 1;
    *buffer = RAR5_SIG;
    return RAR5_SIG_LEN;
  }

  if (v->in_filedata) return rar5_next_filedata_chunk(a, v, buffer);
  return rar5_next_header_chunk(a, v, buffer);
}

static int
rar5_vol_close_cb(struct archive *a, void *client_data) {
  (void)a;
  rar5_vol_cleanup((rar5_vol_t *)client_data);
  return ARCHIVE_OK;
}

int
rar5_open_encrypted(struct archive *a, const job_t *job, const char **volumes, void **out_ctx) {
  size_t count = 0, i;
  rar5_stream_ctx_t *ctx;

  *out_ctx = NULL;
  while (volumes[count]) count++;

  if (!(ctx = calloc(1, sizeof *ctx)) ||
      !(ctx->vols = calloc(count, sizeof *ctx->vols))) {
    free(ctx);
    archive_set_error(a, ENOMEM, "out of memory");
    return ARCHIVE_FATAL;
  }
  ctx->count = count;
  *out_ctx = ctx;

  /* One INFO line per archive set: the one thing worth telling a normal
   * log reader (per-entry detail is DEBUG-only, see rar5_next_header_chunk()).
   * Never logs the password itself. */
  log_info("[%s] extract: rar5: %s: archive headers are encrypted, unlocking with %zu configured password(s)",
           job->id, volumes[0], job->password_count);

  for (i = 0; i < count; i++) {
    if (!(ctx->vols[i] = calloc(1, sizeof *ctx->vols[i]))) {
      archive_set_error(a, ENOMEM, "out of memory");
      return ARCHIVE_FATAL;
    }
    ctx->vols[i]->job = job;
    ctx->vols[i]->path = volumes[i];
    ctx->vols[i]->split_state = &ctx->split_state;
  }

  archive_read_set_open_callback(a, rar5_vol_open_cb);
  archive_read_set_read_callback(a, rar5_vol_read_cb);
  archive_read_set_close_callback(a, rar5_vol_close_cb);

  for (i = 0; i < count; i++) {
    if (archive_read_append_callback_data(a, ctx->vols[i]) != ARCHIVE_OK) return ARCHIVE_FATAL;
  }

  return archive_read_open1(a);
}

void
rar5_stream_free(void *ctx_) {
  rar5_stream_ctx_t *ctx = ctx_;
  size_t i;

  if (!ctx) return;
  for (i = 0; i < ctx->count; i++) {
    if (!ctx->vols[i]) continue;
    rar5_vol_cleanup(ctx->vols[i]);
    free(ctx->vols[i]);
  }
  free(ctx->vols);
  free(ctx);
}
