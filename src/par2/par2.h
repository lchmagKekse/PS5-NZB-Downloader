/* PAR2 recovery-set parsing, verification, and repair. Pure C, no
 * third-party PAR2 library -- packets are parsed directly per the PAR 2.0
 * format, cross-checked against par2cmdline's struct layouts. MD5 via
 * OpenSSL; CRC32 via ../util/crc32.h (same variant zlib/yEnc/PAR2 use). */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
  char           name[512];        /* file name as declared in its File Description packet */
  unsigned char  fileid[16];
  unsigned char  hash_full[16];    /* MD5 of the whole file */
  unsigned char  hash_16k[16];     /* MD5 of the first min(16384, length) bytes -- parsed but currently unused by par2_verify_file() */
  long long      length;           /* declared file length */

  unsigned char (*block_md5)[16];  /* one per block, from the matching IFSC packet -- NULL if none found */
  uint32_t      *block_crc32;      /* parallel to block_md5 */
  size_t         block_count;      /* from the matching IFSC packet if found, else ceil(length/block_size) once the layout is finalized (see par2_scan_dir) */

  /* Filled in by par2_scan_dir() from the Main packet's fileid array --
   * that array's order (not scan/name order) sets each file's position in
   * the recovery set's global block numbering (see rs.h's gf16_next_base).
   * in_recovery_set is 0 if a FileDesc packet exists but the Main packet
   * doesn't list it (e.g. .par2/.nfo/sample, or no Main packet found). */
  int            in_recovery_set;
  long long      global_block_start;  /* only meaningful if in_recovery_set */
} par2_file_t;

/* Location of one Recovery Slice packet's data on disk -- the (potentially
 * huge) data itself is never read until par2_repair_set() actually uses
 * this slice. hash/header_tail let par2_repair_set() verify a candidate
 * slice's data before trusting it, without hashing every found slice at
 * scan time (most never get used). */
typedef struct {
  char           path[900];
  long long      data_offset;  /* file offset where this slice's raw recovery data begins, right after its 4-byte exponent field */
  long long      data_len;     /* declared length of that data (packet body length minus 4) -- checked against block_size before use, since a scan can find a slice before the Main packet defining block_size has been parsed */
  uint32_t       exponent;
  unsigned char  hash[16];        /* packet header's declared integrity hash */
  unsigned char  header_tail[32]; /* packet header bytes 32..63 (setid + type) -- the hash covers this plus the full body (exponent + data) */
} par2_slice_t;

typedef struct {
  long long     block_size;  /* from the Main packet; 0 if none was found */
  par2_file_t  *files;
  size_t        file_count;
  size_t        file_capacity;

  par2_slice_t *slices;       /* every Recovery Slice packet found, sorted ascending by exponent once par2_scan_dir() returns; may contain duplicate exponents (first one found wins -- see par2_repair_set()) */
  size_t        slice_count;
  size_t        slice_capacity;

  long long     source_block_count;  /* total blocks across every recoverable file, i.e. the Vandermonde matrix's input-block count; 0 if no Main packet was found */

  /* Internal scratch used only while par2_scan_dir() resolves file layout;
   * freed before it returns. Not meant for callers -- exposed only because
   * par2_set_t isn't opaque. */
  unsigned char (*main_fileids)[16];
  size_t         main_fileid_count;
  size_t         main_recoverable_count;
} par2_set_t;

/* Scans dir (non-recursive) for *.par2 files, parsing every Main/File
 * Description/IFSC packet across all of them into one merged set --
 * critical packets are typically duplicated across every volume, so this
 * works even if only a repair volume is present. A packet with a bad
 * integrity hash is skipped; slice bodies are never read into memory.
 * Returns an empty set (not NULL) when no usable PAR2 data is found --
 * normal, not an error. NULL only on allocation failure. */
par2_set_t *par2_scan_dir(const char *dir);

void par2_set_free(par2_set_t *set);

/* Finds the file in set whose declared name exactly matches filename, or
 * NULL if the recovery set doesn't cover a file by that name (normal --
 * not every file in an NZB is necessarily part of the PAR2 set, e.g. the
 * .par2 files themselves, or an .nfo/sample file). */
const par2_file_t *par2_find_file(const par2_set_t *set, const char *filename);

typedef enum {
  PAR2_VERIFY_OK,       /* whole-file MD5 matched -- definitely good */
  PAR2_VERIFY_DAMAGED,  /* mismatched, and block-level detail was available -- see report */
  PAR2_VERIFY_UNKNOWN,  /* mismatched, but no IFSC/block data to localize which part is bad */
} par2_verify_result_t;

typedef struct {
  par2_verify_result_t result;
  size_t bad_blocks;    /* only meaningful when result == PAR2_VERIFY_DAMAGED */
  size_t block_count;   /* ditto */
} par2_verify_report_t;

/* Progress callback invoked as bytes are read during par2_verify_file() --
 * may fire once per read() chunk, keep it cheap. bytes_done is cumulative
 * across calls sharing one par2_progress_t. May be NULL. Return nonzero to
 * abort: par2_verify_file()/par2_repair_set() stop at the next opportunity
 * and return -1 (used by api_system_eject() to cut a long verify/repair
 * pass short). */
typedef int (*par2_progress_cb)(void *ctx, long long bytes_done, long long bytes_total);

/* Shared across every file verified in one job's pass so bytes_done
 * keeps accumulating from one file to the next rather than resetting.
 * Caller fills in total (and cb/cb_ctx) once before verifying any file;
 * done should start at 0. */
typedef struct {
  long long done;
  long long total;
  par2_progress_cb cb;
  void *cb_ctx;
} par2_progress_t;

/* Verifies path against pf's whole-file MD5 (one streaming read, the fast
 * path). Only on a mismatch does it re-read in set->block_size chunks
 * against pf's per-block IFSC entries (if any) to report which blocks are
 * bad. *report is always filled in on a 0 return; pg may be NULL. Returns
 * 0 on success, -1 on local I/O error (already logged). */
int par2_verify_file(const par2_set_t *set, const par2_file_t *pf, const char *path,
                      par2_progress_t *pg, par2_verify_report_t *report);

/* One job file's on-disk location for par2_repair_set(); name must match
 * a par2_file_t's declared name. Every file the job has -- not just
 * damaged ones -- must be included: repair needs present files' data too,
 * as Reed-Solomon input. */
typedef struct {
  char name[512];
  char path[900];
} par2_repair_file_t;

/* Reconstructs every damaged/missing block across set's files using
 * Recovery Slice packets (Reed-Solomon over GF(2^16), ported from
 * par2cmdline -- see rs.h). Writes via pwrite() at each block's offset,
 * then truncates to the declared file length. Each slice actually used is
 * hash-checked first (not every slice found); a bad one is skipped for the
 * next by ascending exponent. Does not re-verify the result -- caller must
 * re-run par2_verify_file() after. A file the set covers but files[] omits
 * fails the whole attempt (err explains which). Returns 0 on full
 * reconstruction, -1 on failure (err filled); pg may be NULL. */
int par2_repair_set(const par2_set_t *set, const par2_repair_file_t *files, size_t file_count,
                     par2_progress_t *pg, char *err, size_t err_size);
