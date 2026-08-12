#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>

#include "../log/log.h"
#include "../util/crc32.h"
#include "par2.h"
#include "rs.h"

#define PACKET_HEADER_SIZE 64
#define TYPE_SIZE 16

/* PAR2 fields are little-endian; bodies are read as raw bytes, never cast
 * through a struct, to avoid packing/alignment portability issues. */
static const unsigned char PACKET_MAGIC[8]      = {'P','A','R','2','\0','P','K','T'};
static const unsigned char TYPE_MAIN[TYPE_SIZE]     = {'P','A','R',' ','2','.','0','\0','M','a','i','n','\0','\0','\0','\0'};
static const unsigned char TYPE_FILEDESC[TYPE_SIZE] = {'P','A','R',' ','2','.','0','\0','F','i','l','e','D','e','s','c'};
static const unsigned char TYPE_IFSC[TYPE_SIZE]     = {'P','A','R',' ','2','.','0','\0','I','F','S','C','\0','\0','\0','\0'};
static const unsigned char TYPE_RECOVERY[TYPE_SIZE] = {'P','A','R',' ','2','.','0','\0','R','e','c','v','S','l','i','c'};

/* Guards against a corrupt length field driving a huge allocation --
 * Main/FileDesc/IFSC packets are always small in practice. Recovery slice
 * packets, which can legitimately be large, are never read into memory. */
#define MAX_CRITICAL_PACKET_BODY (16 * 1024 * 1024)

#define VERIFY_READ_CHUNK 65536

static uint64_t
read_le64(const unsigned char *p) {
  uint64_t v = 0;
  int i;
  for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

static uint32_t
read_le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* MD5 of a || b (b may be NULL/0-length) into out[16]. Used both for a
 * packet's own integrity hash (header tail || body, two separate
 * buffers) and for a single-buffer hash (pass b=NULL). */
static void
md5_of_two(const unsigned char *a, size_t alen, const unsigned char *b, size_t blen, unsigned char out[16]) {
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  unsigned int outlen = 0;

  if (!ctx) {
    memset(out, 0, 16);
    return;
  }

  EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
  EVP_DigestUpdate(ctx, a, alen);
  if (blen > 0) EVP_DigestUpdate(ctx, b, blen);
  EVP_DigestFinal_ex(ctx, out, &outlen);
  EVP_MD_CTX_free(ctx);
}

static par2_file_t *
set_find_or_add_file(par2_set_t *set, const unsigned char fileid[16]) {
  size_t i;

  for (i = 0; i < set->file_count; i++) {
    if (!memcmp(set->files[i].fileid, fileid, 16)) return &set->files[i];
  }

  if (set->file_count == set->file_capacity) {
    size_t new_cap = set->file_capacity ? set->file_capacity * 2 : 8;
    par2_file_t *grown = realloc(set->files, new_cap * sizeof *grown);
    if (!grown) return NULL;
    set->files = grown;
    set->file_capacity = new_cap;
  }

  memset(&set->files[set->file_count], 0, sizeof set->files[0]);
  memcpy(set->files[set->file_count].fileid, fileid, 16);
  return &set->files[set->file_count++];
}

/* Main packet body: blocksize (leu64) + recoverablefilecount (leu32) +
 * fileid[] (totalfilecount entries, derived from packet length). The
 * first recoverablefilecount ids are the recovery set, in the exact
 * order repair's global block numbering must follow -- see
 * finalize_layout(). */
static void
handle_main_packet(par2_set_t *set, const unsigned char *body, size_t body_len) {
  size_t recoverable, total, i;
  unsigned char (*ids)[16];

  if (body_len < 12) return;
  if (set->block_size == 0) {
    long long bs = (long long)read_le64(body);
    /* Per spec, blocksize must be a non-zero multiple of 4: repair's
     * GF(2^16) accumulation processes two bytes at a time. A value that
     * fails this is treated as "no Main packet found" (block_size stays 0). */
    if (bs > 0 && (bs & 3) == 0) set->block_size = bs;
  }

  if (set->main_fileids) return; /* first Main packet found wins -- duplicates across volumes are identical per spec */

  recoverable = (size_t)read_le32(body + 8);
  total = (body_len - 12) / 16;
  if (recoverable > total) return; /* malformed -- ignore, layout stays unresolved */

  if (!(ids = malloc(total > 0 ? total * sizeof *ids : 1))) return;
  for (i = 0; i < total; i++) memcpy(ids[i], body + 12 + i * 16, 16);

  set->main_fileids = ids;
  set->main_fileid_count = total;
  set->main_recoverable_count = recoverable;
}

/* File Description packet body: fileid(16) + hashfull(16) + hash16k(16)
 * + length(leu64,8) + name[] (zero-padded to a multiple of 4, actual
 * length determined by packet length minus the fixed 56-byte prefix). */
static void
handle_filedesc_packet(par2_set_t *set, const unsigned char *body, size_t body_len) {
  par2_file_t *pf;
  size_t name_len;

  if (body_len < 56) return;
  if (!(pf = set_find_or_add_file(set, body))) return;

  memcpy(pf->hash_full, body + 16, 16);
  memcpy(pf->hash_16k, body + 32, 16);
  pf->length = (long long)read_le64(body + 48);

  name_len = body_len - 56;
  if (name_len >= sizeof pf->name) name_len = sizeof pf->name - 1;
  memcpy(pf->name, body + 56, name_len);
  pf->name[name_len] = 0;
}

/* Input File Slice Checksum packet body: fileid(16) + entries[], each
 * entry hash(16) + crc(leu32,4) = 20 bytes, one per block of that file
 * in block order. Entry count derived from packet length. */
static void
handle_ifsc_packet(par2_set_t *set, const unsigned char *body, size_t body_len) {
  par2_file_t *pf;
  size_t entry_count, i;
  unsigned char (*block_md5)[16];
  uint32_t *block_crc32;

  if (body_len < 16) return;
  entry_count = (body_len - 16) / 20;
  if (entry_count == 0) return;
  if (!(pf = set_find_or_add_file(set, body))) return;

  if (!(block_md5 = malloc(entry_count * sizeof *block_md5))) return;
  if (!(block_crc32 = malloc(entry_count * sizeof *block_crc32))) {
    free(block_md5);
    return;
  }

  for (i = 0; i < entry_count; i++) {
    const unsigned char *entry = body + 16 + i * 20;
    memcpy(block_md5[i], entry, 16);
    block_crc32[i] = read_le32(entry + 16);
  }

  free(pf->block_md5);
  free(pf->block_crc32);
  pf->block_md5 = block_md5;
  pf->block_crc32 = block_crc32;
  pf->block_count = entry_count;
}

static int
append_slice(par2_set_t *set, const char *path, long long data_offset, long long data_len, uint32_t exponent,
             const unsigned char hash[16], const unsigned char header_tail[32]) {
  if (set->slice_count == set->slice_capacity) {
    size_t new_cap = set->slice_capacity ? set->slice_capacity * 2 : 16;
    par2_slice_t *grown = realloc(set->slices, new_cap * sizeof *grown);
    if (!grown) return -1;
    set->slices = grown;
    set->slice_capacity = new_cap;
  }

  {
    par2_slice_t *s = &set->slices[set->slice_count++];
    snprintf(s->path, sizeof s->path, "%s", path);
    s->data_offset = data_offset;
    s->data_len = data_len;
    s->exponent = exponent;
    memcpy(s->hash, hash, 16);
    memcpy(s->header_tail, header_tail, 32);
  }

  return 0;
}

/* Recovery Slice packet body: exponent (leu32) + recovery data
 * (block_size bytes). Only the data's location is recorded (see
 * par2_slice_t), never read into memory here -- it can be large, and most
 * found slices never end up used. On return f is positioned at the next
 * packet. Returns 0 on success, -1 if seeking failed (caller stops
 * parsing this file). */
static int
handle_recovery_packet(par2_set_t *set, const char *path, FILE *f, size_t body_len,
                        const unsigned char hash[16], const unsigned char header_tail[32]) {
  unsigned char expbuf[4];
  long long data_offset, data_len;

  if (body_len < 4) {
    return body_len == 0 || fseek(f, (long)body_len, SEEK_CUR) == 0 ? 0 : -1;
  }

  if (fread(expbuf, 1, 4, f) != 4) return -1;

  data_offset = ftell(f);
  if (data_offset < 0) return -1;
  data_len = (long long)(body_len - 4);

  if (append_slice(set, path, data_offset, data_len, read_le32(expbuf), hash, header_tail) < 0) return -1;

  return data_len == 0 || fseek(f, (long)data_len, SEEK_CUR) == 0 ? 0 : -1;
}

/* Parses every packet in one *.par2 file into set. Packets are expected
 * back-to-back with no gaps; if the magic isn't found where the previous
 * packet's length says the next one starts, parsing stops there rather
 * than resyncing, keeping whatever was already parsed. Unrecognized/
 * recovery-slice bodies are skipped via fseek, never read into memory. */
static void
parse_par2_file(par2_set_t *set, const char *path) {
  FILE *f;
  unsigned char header[PACKET_HEADER_SIZE];

  if (!(f = fopen(path, "rb"))) {
    log_warn("par2: fopen(%s): %s", path, strerror(errno));
    return;
  }

  for (;;) {
    uint64_t length;
    size_t body_len;
    unsigned char *body;
    unsigned char computed_hash[16];

    if (fread(header, 1, PACKET_HEADER_SIZE, f) != PACKET_HEADER_SIZE) break; /* EOF -- done */

    if (memcmp(header, PACKET_MAGIC, 8) != 0) {
      log_debug("par2: %s: packet magic mismatch at this position, stopping (kept what came before)", path);
      break;
    }

    length = read_le64(header + 8);
    if (length < PACKET_HEADER_SIZE) {
      log_warn("par2: %s: implausible packet length, stopping", path);
      break;
    }
    body_len = (size_t)(length - PACKET_HEADER_SIZE);

    if (!memcmp(header + 48, TYPE_RECOVERY, TYPE_SIZE)) {
      if (handle_recovery_packet(set, path, f, body_len, header + 16, header + 32) < 0) {
        log_warn("par2: %s: truncated/unseekable recovery packet, stopping", path);
        break;
      }
      continue;
    }

    if (memcmp(header + 48, TYPE_MAIN, TYPE_SIZE) != 0 &&
        memcmp(header + 48, TYPE_FILEDESC, TYPE_SIZE) != 0 &&
        memcmp(header + 48, TYPE_IFSC, TYPE_SIZE) != 0) {
      if (body_len > 0 && fseek(f, (long)body_len, SEEK_CUR) != 0) break;
      continue;
    }

    if (body_len > (size_t)MAX_CRITICAL_PACKET_BODY) {
      log_warn("par2: %s: critical packet body implausibly large (%zu bytes), stopping", path, body_len);
      break;
    }

    if (!(body = malloc(body_len > 0 ? body_len : 1))) break;
    if (body_len > 0 && fread(body, 1, body_len, f) != body_len) {
      log_warn("par2: %s: truncated packet body, stopping", path);
      free(body);
      break;
    }

    /* Packet's own integrity hash covers everything from setid onward
     * (header bytes 32..64) through the end of the body -- NOT magic/
     * length/hash itself. See PACKET_HEADER in the reference
     * implementation's par2fileformat.h. */
    md5_of_two(header + 32, PACKET_HEADER_SIZE - 32, body, body_len, computed_hash);
    if (memcmp(computed_hash, header + 16, 16) != 0) {
      log_warn("par2: %s: packet integrity hash mismatch, skipping one packet", path);
      free(body);
      continue;
    }

    if (!memcmp(header + 48, TYPE_MAIN, TYPE_SIZE)) {
      handle_main_packet(set, body, body_len);
    } else if (!memcmp(header + 48, TYPE_FILEDESC, TYPE_SIZE)) {
      handle_filedesc_packet(set, body, body_len);
    } else {
      handle_ifsc_packet(set, body, body_len);
    }

    free(body);
  }

  fclose(f);
}

static par2_file_t *
find_file_by_id(par2_set_t *set, const unsigned char fileid[16]) {
  size_t i;

  for (i = 0; i < set->file_count; i++) {
    if (!memcmp(set->files[i].fileid, fileid, 16)) return &set->files[i];
  }

  return NULL;
}

static int
slice_cmp(const void *a, const void *b) {
  const par2_slice_t *sa = a, *sb = b;

  if (sa->exponent < sb->exponent) return -1;
  if (sa->exponent > sb->exponent) return 1;
  return 0;
}

/* Resolves each recoverable file's position in the global block numbering
 * from the Main packet's fileid array -- must run after every *.par2 file
 * has been scanned, since the Main packet and a file's FileDesc/IFSC
 * packets can live in different physical files. A fileid with no matching
 * FileDesc is left with in_recovery_set == 0. */
static void
finalize_layout(par2_set_t *set) {
  long long running = 0;
  size_t i;

  if (set->main_fileids && set->block_size > 0) {
    for (i = 0; i < set->main_recoverable_count && i < set->main_fileid_count; i++) {
      par2_file_t *pf = find_file_by_id(set, set->main_fileids[i]);
      long long bc;

      if (!pf) continue;

      bc = (long long)pf->block_count;
      if (bc <= 0) bc = (pf->length + set->block_size - 1) / set->block_size;
      if (bc <= 0) continue; /* zero-length file -- contributes no blocks */

      pf->in_recovery_set = 1;
      pf->global_block_start = running;
      if (pf->block_count == 0) pf->block_count = (size_t)bc;
      running += bc;
    }
    set->source_block_count = running;
  }

  free(set->main_fileids);
  set->main_fileids = NULL;
  set->main_fileid_count = 0;
  set->main_recoverable_count = 0;
}

par2_set_t *
par2_scan_dir(const char *dir) {
  par2_set_t *set;
  DIR *d;
  struct dirent *ent;

  if (!(set = calloc(1, sizeof *set))) return NULL;

  if (!(d = opendir(dir))) {
    /* Not fatal -- indistinguishable here from "no PAR2 files present",
     * which is the normal case for most jobs. */
    return set;
  }

  while ((ent = readdir(d))) {
    char full[900];
    size_t len = strlen(ent->d_name);

    if (len < 5 || strcasecmp(ent->d_name + len - 5, ".par2") != 0) continue;

    snprintf(full, sizeof full, "%s/%s", dir, ent->d_name);
    parse_par2_file(set, full);
  }

  closedir(d);

  finalize_layout(set);
  if (set->slice_count > 1) qsort(set->slices, set->slice_count, sizeof *set->slices, slice_cmp);

  return set;
}

void
par2_set_free(par2_set_t *set) {
  size_t i;

  if (!set) return;

  for (i = 0; i < set->file_count; i++) {
    free(set->files[i].block_md5);
    free(set->files[i].block_crc32);
  }
  free(set->files);
  free(set->slices);
  free(set->main_fileids);
  free(set);
}

const par2_file_t *
par2_find_file(const par2_set_t *set, const char *filename) {
  size_t i;

  for (i = 0; i < set->file_count; i++) {
    if (!strcmp(set->files[i].name, filename)) return &set->files[i];
  }

  return NULL;
}

static void
report_progress(par2_progress_t *pg, size_t n) {
  if (!pg) return;
  pg->done += (long long)n;
  if (pg->cb) pg->cb(pg->cb_ctx, pg->done, pg->total);
}

int
par2_verify_file(const par2_set_t *set, const par2_file_t *pf, const char *path,
                  par2_progress_t *pg, par2_verify_report_t *report) {
  FILE *f;
  EVP_MD_CTX *ctx;
  unsigned char buf[VERIFY_READ_CHUNK];
  unsigned char computed_full[16];
  size_t n;

  memset(report, 0, sizeof *report);

  if (!(f = fopen(path, "rb"))) {
    log_warn("par2: verify: fopen(%s): %s", path, strerror(errno));
    return -1;
  }

  if (!(ctx = EVP_MD_CTX_new())) {
    fclose(f);
    return -1;
  }
  EVP_DigestInit_ex(ctx, EVP_md5(), NULL);

  while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
    EVP_DigestUpdate(ctx, buf, n);
    report_progress(pg, n);
  }
  if (ferror(f)) {
    log_warn("par2: verify: read error on %s: %s", path, strerror(errno));
    EVP_MD_CTX_free(ctx);
    fclose(f);
    return -1;
  }
  {
    unsigned int outlen = 0;
    EVP_DigestFinal_ex(ctx, computed_full, &outlen);
  }
  EVP_MD_CTX_free(ctx);

  if (!memcmp(computed_full, pf->hash_full, 16)) {
    report->result = PAR2_VERIFY_OK;
    fclose(f);
    return 0;
  }

  if (!pf->block_md5 || pf->block_count == 0 || set->block_size <= 0) {
    report->result = PAR2_VERIFY_UNKNOWN;
    fclose(f);
    return 0;
  }

  /* Whole-file hash mismatched -- fall back to block-by-block IFSC checks
   * to report how much is wrong. A short/missing final block is
   * zero-padded to block_size first, matching how PAR2 creators compute
   * these checksums. */
  {
    unsigned char *block = malloc((size_t)set->block_size);
    size_t bi;

    if (!block) {
      report->result = PAR2_VERIFY_UNKNOWN;
      fclose(f);
      return 0;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
      free(block);
      report->result = PAR2_VERIFY_UNKNOWN;
      fclose(f);
      return 0;
    }

    report->block_count = pf->block_count;

    for (bi = 0; bi < pf->block_count; bi++) {
      size_t got = fread(block, 1, (size_t)set->block_size, f);
      unsigned char block_md5[16];
      unsigned long crc;
      uint32_t block_crc;

      if (got < (size_t)set->block_size) {
        memset(block + got, 0, (size_t)set->block_size - got);
      }
      report_progress(pg, got);

      crc = crc32_init();
      crc = crc32_update(crc, block, (size_t)set->block_size);
      block_crc = (uint32_t)crc32_final(crc);

      md5_of_two(block, (size_t)set->block_size, NULL, 0, block_md5);

      if (block_crc != pf->block_crc32[bi] || memcmp(block_md5, pf->block_md5[bi], 16) != 0) {
        report->bad_blocks++;
      }
    }

    free(block);
    report->result = PAR2_VERIFY_DAMAGED;
  }

  fclose(f);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Repair: Reed-Solomon reconstruction over GF(2^16), see rs.h.        */
/* ------------------------------------------------------------------ */

/* Combined output-buffer budget across all missing blocks being
 * reconstructed at once -- bounds memory for a badly-damaged release. */
#define REPAIR_MEMORY_BUDGET (32 * 1024 * 1024)

static long long
ll_clamp(long long value, long long lo, long long hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

/* Whole-file MD5, for the (expected to be rare) case of a covered file
 * with no IFSC packet -- no block-level granularity is available, so
 * repair can only treat the file as entirely-good or entirely-missing. */
static int
hash_file_md5(const char *path, unsigned char out[16]) {
  FILE *f;
  EVP_MD_CTX *ctx;
  unsigned char buf[VERIFY_READ_CHUNK];
  size_t n;
  unsigned int outlen = 0;

  if (!(f = fopen(path, "rb"))) return -1;
  if (!(ctx = EVP_MD_CTX_new())) {
    fclose(f);
    return -1;
  }
  EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) EVP_DigestUpdate(ctx, buf, n);
  if (ferror(f)) {
    EVP_MD_CTX_free(ctx);
    fclose(f);
    return -1;
  }
  EVP_DigestFinal_ex(ctx, out, &outlen);
  EVP_MD_CTX_free(ctx);
  fclose(f);
  return 0;
}

/* Verifies a candidate recovery slice's on-disk data against its own
 * packet's declared integrity hash -- done lazily here (not at scan time)
 * since most found slices never get used, and hashing costs a full
 * block_size read either way. Needed because a slice's header can parse
 * cleanly while its data is silently wrong (e.g. an NZB missing articles
 * for that .par2 file, observed on real hardware) -- the caller skips a
 * bad slice and tries the next by ascending exponent instead of failing
 * the whole repair. Returns 1 if the data matches, 0 on mismatch or I/O
 * error. */
static int
slice_hash_ok(const par2_slice_t *s) {
  FILE *f;
  EVP_MD_CTX *ctx;
  unsigned char chunk[VERIFY_READ_CHUNK];
  unsigned char computed[16];
  long long remaining;
  unsigned int outlen = 0;
  int ok;

  if (!(f = fopen(s->path, "rb"))) return 0;
  if (fseek(f, s->data_offset - 4, SEEK_SET) != 0) {
    fclose(f);
    return 0;
  }

  if (!(ctx = EVP_MD_CTX_new())) {
    fclose(f);
    return 0;
  }
  EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
  EVP_DigestUpdate(ctx, s->header_tail, sizeof s->header_tail);

  remaining = 4 + s->data_len; /* exponent field + recovery data, matching what the hash covers */
  ok = 1;
  while (remaining > 0) {
    size_t want = (remaining < (long long)sizeof chunk) ? (size_t)remaining : sizeof chunk;
    size_t got = fread(chunk, 1, want, f);
    if (got != want) {
      ok = 0;
      break;
    }
    EVP_DigestUpdate(ctx, chunk, got);
    remaining -= (long long)got;
  }

  if (ok) {
    EVP_DigestFinal_ex(ctx, computed, &outlen);
    ok = memcmp(computed, s->hash, 16) == 0;
  }

  EVP_MD_CTX_free(ctx);
  fclose(f);
  return ok;
}

/* Fills bad[bi]=1 for every block of pf that doesn't match its IFSC
 * hash/CRC -- mirrors par2_verify_file()'s block-level fallback (short/
 * missing data reads as zero-padded). Returns 0 on success (even if every
 * block is bad); -1 only on a real I/O error (a missing file is not an
 * error -- its blocks are simply all bad). */
static int
scan_block_damage(const par2_set_t *set, const par2_file_t *pf, const char *path, unsigned char *bad) {
  FILE *f;
  unsigned char *block;
  size_t bi;

  if (!(f = fopen(path, "rb"))) {
    memset(bad, 1, pf->block_count);
    return 0;
  }

  if (!(block = malloc((size_t)set->block_size))) {
    fclose(f);
    return -1;
  }

  for (bi = 0; bi < pf->block_count; bi++) {
    size_t got = fread(block, 1, (size_t)set->block_size, f);
    unsigned char md5[16];
    unsigned long crc;
    uint32_t block_crc;

    if (got < (size_t)set->block_size) memset(block + got, 0, (size_t)set->block_size - got);

    crc = crc32_init();
    crc = crc32_update(crc, block, (size_t)set->block_size);
    block_crc = (uint32_t)crc32_final(crc);
    md5_of_two(block, (size_t)set->block_size, NULL, 0, md5);

    bad[bi] = (block_crc != pf->block_crc32[bi] || memcmp(md5, pf->block_md5[bi], 16) != 0) ? 1 : 0;
  }

  free(block);
  fclose(f);
  return 0;
}

/* Maps a global block index to its owning file and byte offset within it
 * -- set->files[] ranges tile [0, source_block_count) with no gaps
 * (guaranteed by finalize_layout()). NULL only means an internal
 * bookkeeping bug, not bad data. */
static const par2_file_t *
locate_block(const par2_set_t *set, char **paths, long long global_idx,
             const char **path_out, long long *block_base_off) {
  size_t i;

  for (i = 0; i < set->file_count; i++) {
    const par2_file_t *pf = &set->files[i];

    if (!pf->in_recovery_set) continue;
    if (global_idx >= pf->global_block_start && global_idx < pf->global_block_start + (long long)pf->block_count) {
      *path_out = paths[i];
      *block_base_off = (global_idx - pf->global_block_start) * set->block_size;
      return pf;
    }
  }

  return NULL;
}

typedef struct {
  char     path[900];
  long long data_offset;
  uint16_t exponent;
} accepted_slice_t;

int
par2_repair_set(const par2_set_t *set, const par2_repair_file_t *files, size_t file_count,
                par2_progress_t *pg, char *err, size_t err_size) {
  size_t nfiles = set->file_count;
  char **paths = NULL;
  unsigned char *present = NULL;
  gf16_t *database = NULL;
  uint32_t *datapresentindex = NULL, *datamissingindex = NULL;
  size_t datapresent = 0, datamissing = 0;
  accepted_slice_t *accepted = NULL;
  gf16_t *coeffs = NULL, *present_bases = NULL, *missing_bases = NULL;
  uint16_t *exponents16 = NULL;
  unsigned char *inbuf = NULL, **outbufs = NULL;
  size_t i;
  long long total_blocks;
  int rc = -1;

  err[0] = 0;
  gf16_init();

  if (set->block_size <= 0 || set->source_block_count <= 0) {
    snprintf(err, err_size, "PAR2 repair: no usable PAR2 layout (Main packet missing or invalid)");
    return -1;
  }
  total_blocks = set->source_block_count;

  /* Resolve each recoverable file's on-disk path once, up front. A
   * recoverable file with no entry in files[] is a hard failure -- nowhere
   * to read/write it; should not happen in practice. */
  if (!(paths = calloc(nfiles, sizeof *paths))) goto oom;
  for (i = 0; i < nfiles; i++) {
    const par2_file_t *pf = &set->files[i];
    size_t j;

    if (!pf->in_recovery_set) continue;

    for (j = 0; j < file_count; j++) {
      if (!strcmp(files[j].name, pf->name)) {
        if (!(paths[i] = strdup(files[j].path))) goto oom;
        break;
      }
    }

    if (!paths[i]) {
      snprintf(err, err_size, "PAR2 repair: %s: not part of this job -- recovery set references a file this job doesn't have", pf->name);
      goto done;
    }
  }

  /* Determine present vs. missing across every file at once -- one
   * recovery slice's linear combination spans every file's blocks, so
   * repair is inherently whole-set, not per-file. Defaults to "missing"
   * (fail-safe): an unresolved index just costs an extra "not enough
   * recovery blocks" failure rather than silently using unverified bytes. */
  if (!(present = malloc((size_t)total_blocks))) goto oom;
  memset(present, 0, (size_t)total_blocks);

  for (i = 0; i < nfiles; i++) {
    const par2_file_t *pf = &set->files[i];
    unsigned char *bad;
    size_t bi;

    if (!pf->in_recovery_set) continue;

    if (pf->block_md5 && pf->block_count > 0) {
      if (!(bad = malloc(pf->block_count))) goto oom;
      if (scan_block_damage(set, pf, paths[i], bad) < 0) {
        free(bad);
        snprintf(err, err_size, "PAR2 repair: could not read %s", pf->name);
        goto done;
      }
      for (bi = 0; bi < pf->block_count; bi++) present[pf->global_block_start + bi] = bad[bi] ? 0 : 1;
      free(bad);
    } else {
      unsigned char full[16];
      int ok = hash_file_md5(paths[i], full) == 0 && !memcmp(full, pf->hash_full, 16);
      for (bi = 0; bi < pf->block_count; bi++) present[pf->global_block_start + bi] = ok ? 1 : 0;
    }
  }

  /* database[] base values must be assigned in strict ascending global
   * index order across every block, present or missing alike -- the
   * matrix depends on every input block having a correctly-ordered
   * base value, exactly matching ReedSolomon<Galois16>::SetInput(). */
  if (!(database = malloc((size_t)total_blocks * sizeof *database))) goto oom;
  {
    unsigned int base_state = 0;
    for (i = 0; i < (size_t)total_blocks; i++) {
      if (!gf16_next_base(&base_state, &database[i])) {
        snprintf(err, err_size, "PAR2 repair: too many blocks in recovery set for GF(2^16) (%lld)", total_blocks);
        goto done;
      }
    }
  }

  if (!(datapresentindex = malloc((size_t)total_blocks * sizeof *datapresentindex))) goto oom;
  if (!(datamissingindex = malloc((size_t)total_blocks * sizeof *datamissingindex))) goto oom;
  for (i = 0; i < (size_t)total_blocks; i++) {
    if (present[i]) datapresentindex[datapresent++] = (uint32_t)i;
    else datamissingindex[datamissing++] = (uint32_t)i;
  }

  if (datamissing == 0) {
    rc = 0;
    goto done;
  }

  /* Select datamissing usable slices by ascending exponent (set->slices
   * already sorted), deduplicating by exponent (first found wins). A
   * candidate is skipped if its data length doesn't match block_size, or
   * if slice_hash_ok() fails -- letting one bad volume be skipped for the
   * next rather than sinking the whole repair. Reports a precise "need N
   * more" if the scan runs out before collecting enough. */
  if (!(accepted = malloc(datamissing * sizeof *accepted))) goto oom;
  {
    size_t got = 0;
    long long prev_exp = -1;
    int have_prev = 0;

    for (i = 0; i < set->slice_count && got < datamissing; i++) {
      const par2_slice_t *s = &set->slices[i];
      if (have_prev && (long long)s->exponent == prev_exp) continue;
      prev_exp = s->exponent;
      have_prev = 1;
      if (s->data_len != set->block_size) continue;
      if (!slice_hash_ok(s)) {
        log_warn("par2: repair: %s: recovery slice (exponent %u) failed its own integrity hash, skipping -- trying another",
                 s->path, s->exponent);
        continue;
      }
      snprintf(accepted[got].path, sizeof accepted[got].path, "%s", s->path);
      accepted[got].data_offset = s->data_offset;
      accepted[got].exponent = (uint16_t)s->exponent;
      got++;
    }

    if (got < datamissing) {
      snprintf(err, err_size, "PAR2 repair: need %zu more recovery block(s) (have %zu usable, need %zu)",
               datamissing - got, got, datamissing);
      goto done;
    }
  }

  if (!(present_bases = malloc(datapresent * sizeof *present_bases))) goto oom;
  if (!(missing_bases = malloc(datamissing * sizeof *missing_bases))) goto oom;
  if (!(exponents16 = malloc(datamissing * sizeof *exponents16))) goto oom;
  for (i = 0; i < datapresent; i++) present_bases[i] = database[datapresentindex[i]];
  for (i = 0; i < datamissing; i++) {
    missing_bases[i] = database[datamissingindex[i]];
    exponents16[i] = accepted[i].exponent;
  }

  if (!(coeffs = malloc(datamissing * (datapresent + datamissing) * sizeof *coeffs))) goto oom;
  if (gf16_solve(datapresent, present_bases, datamissing, missing_bases, exponents16, coeffs) < 0) {
    snprintf(err, err_size, "PAR2 repair: internal Reed-Solomon computation error (singular matrix)");
    goto done;
  }

  /* Reconstruction: one pass over block_size in chunks, not one per
   * missing block, since all missing blocks share the same coefficients
   * at every byte offset. Each chunk reads every input column, GF(2^16)-
   * accumulates into every missing block's output, then writes clamped to
   * the file's declared length (the zero-padded tail is math-only, never
   * written). */
  {
    size_t incount = datapresent + datamissing;
    size_t chunksize;
    long long chunk_off;
    size_t row, col;

    if ((long long)datamissing * set->block_size > REPAIR_MEMORY_BUDGET) {
      chunksize = (size_t)(REPAIR_MEMORY_BUDGET / datamissing);
      chunksize &= ~(size_t)1;
      if (chunksize < 4) chunksize = 4;
    } else {
      chunksize = (size_t)set->block_size;
    }

    if (!(inbuf = malloc(chunksize))) goto oom;
    if (!(outbufs = calloc(datamissing, sizeof *outbufs))) goto oom;
    for (row = 0; row < datamissing; row++) {
      if (!(outbufs[row] = malloc(chunksize))) goto oom;
    }

    for (chunk_off = 0; chunk_off < set->block_size; chunk_off += (long long)chunksize) {
      long long remain = set->block_size - chunk_off;
      size_t len = (remain < (long long)chunksize) ? (size_t)remain : chunksize;

      for (row = 0; row < datamissing; row++) memset(outbufs[row], 0, len);

      for (col = 0; col < incount; col++) {
        if (col < datapresent) {
          long long gi = (long long)datapresentindex[col];
          const par2_file_t *pf;
          const char *path;
          long long block_base, valid;

          if (!(pf = locate_block(set, paths, gi, &path, &block_base))) {
            snprintf(err, err_size, "PAR2 repair: internal error: could not locate block %lld", gi);
            goto done;
          }

          valid = ll_clamp(pf->length - block_base - chunk_off, 0, (long long)len);
          if (valid > 0) {
            FILE *f = fopen(path, "rb");
            if (!f || fseek(f, block_base + chunk_off, SEEK_SET) != 0 ||
                fread(inbuf, 1, (size_t)valid, f) != (size_t)valid) {
              if (f) fclose(f);
              snprintf(err, err_size, "PAR2 repair: I/O error reading %s", pf->name);
              goto done;
            }
            fclose(f);
          }
          if ((size_t)valid < len) memset(inbuf + valid, 0, len - (size_t)valid);
        } else {
          size_t k = col - datapresent;
          FILE *f = fopen(accepted[k].path, "rb");
          if (!f || fseek(f, accepted[k].data_offset + chunk_off, SEEK_SET) != 0 ||
              fread(inbuf, 1, len, f) != len) {
            if (f) fclose(f);
            snprintf(err, err_size, "PAR2 repair: I/O error reading recovery data (exponent %u)", accepted[k].exponent);
            goto done;
          }
          fclose(f);
        }

        for (row = 0; row < datamissing; row++) {
          gf16_t factor = coeffs[row * incount + col];
          if (factor) gf16_mul_acc(outbufs[row], inbuf, len, factor);
        }

        if (pg) {
          pg->done += (long long)len;
          if (pg->cb) pg->cb(pg->cb_ctx, pg->done, pg->total);
        }
      }

      for (row = 0; row < datamissing; row++) {
        long long gi = (long long)datamissingindex[row];
        const par2_file_t *pf;
        const char *path;
        long long block_base, write_len;
        int fd;

        if (!(pf = locate_block(set, paths, gi, &path, &block_base))) {
          snprintf(err, err_size, "PAR2 repair: internal error: could not locate block %lld", gi);
          goto done;
        }

        write_len = ll_clamp(pf->length - block_base - chunk_off, 0, (long long)len);
        if (write_len <= 0) continue;

        fd = open(path, O_WRONLY | O_CREAT, 0644);
        if (fd < 0 || pwrite(fd, outbufs[row], (size_t)write_len, block_base + chunk_off) != write_len) {
          if (fd >= 0) close(fd);
          snprintf(err, err_size, "PAR2 repair: I/O error writing %s", pf->name);
          goto done;
        }
        close(fd);
      }
    }
  }

  /* Defensive: truncate every repaired file down to its declared true
   * length, mirroring par2_verify_job()'s own shrink-only fixup for the
   * same reason -- should already be exact given every write above is
   * clamped to pf->length, but costs nothing to double check. */
  for (i = 0; i < nfiles; i++) {
    const par2_file_t *pf = &set->files[i];
    struct stat st;

    if (!pf->in_recovery_set || !paths[i]) continue;
    if (stat(paths[i], &st) == 0 && (long long)st.st_size != pf->length) {
      if (truncate(paths[i], pf->length) != 0) {
        log_warn("par2: repair: truncate(%s, %lld): %s", paths[i], pf->length, strerror(errno));
      }
    }
  }

  rc = 0;
  goto done;

oom:
  snprintf(err, err_size, "PAR2 repair: out of memory");

done:
  if (paths) {
    for (i = 0; i < nfiles; i++) free(paths[i]);
    free(paths);
  }
  free(present);
  free(database);
  free(datapresentindex);
  free(datamissingindex);
  free(accepted);
  free(coeffs);
  free(present_bases);
  free(missing_bases);
  free(exponents16);
  free(inbuf);
  if (outbufs) {
    for (i = 0; i < datamissing; i++) free(outbufs[i]);
    free(outbufs);
  }
  return rc;
}
