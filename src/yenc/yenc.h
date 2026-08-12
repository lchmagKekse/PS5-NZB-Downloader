/* yEnc line parsing and decode, wrapping the vendored rapidyenc library for
 * the byte transform. NNTP dot-unstuffing has already happened by the time
 * a line reaches here (see nntp_conn.h) -- this module only tells
 * =ybegin/=ypart/=yend control lines apart from data lines and decodes them.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
  YENC_LINE_DATA,   /* not a recognized control line -- encoded data */
  YENC_LINE_BEGIN,  /* "=ybegin ..." -- starts a part */
  YENC_LINE_PART,   /* "=ypart ..." -- only present for a multi-part article */
  YENC_LINE_END,    /* "=yend ..." -- ends a part */
} yenc_line_kind_t;

typedef struct {
  long     size;        /* =ybegin: whole file's decoded size. =yend: this part's decoded size. Unset (0) on =ypart. */
  long     begin, end;  /* =ypart only: 1-based inclusive decoded byte range within the final file */
  uint32_t crc32;        /* =yend only: pcrc32= if present, else crc32= */
  int      has_size;
  int      has_range;    /* both begin= and end= were present (=ypart) */
  int      has_crc32;
} yenc_line_t;

/* Classifies line (dot-unstuffed, no trailing CRLF) and, for a recognized
 * control line, fills *out (zeroed first) with whichever fields this
 * module understands; unrecognized key=value pairs are ignored. */
yenc_line_kind_t yenc_classify_line(const char *line, size_t len, yenc_line_t *out);

/* One-time global setup for rapidyenc's decode engine (CPU feature
 * detection, lookup tables) -- not thread-safe, so call exactly once
 * before any decoding happens (see main()). */
void yenc_global_init(void);

/* Decodes one already dot-unstuffed data line (src/src_len) into dest.
 * dest must have room for at least src_len bytes -- yEnc decoding never
 * produces more bytes than its encoded input. Returns the number of
 * bytes written to dest. */
size_t yenc_decode_line(const void *src, size_t src_len, void *dest);
