#include <stdlib.h>
#include <string.h>

#include <rapidyenc.h>

#include "yenc.h"

void
yenc_global_init(void) {
  rapidyenc_decode_init();
}

size_t
yenc_decode_line(const void *src, size_t src_len, void *dest) {
  if (src_len == 0) return 0;

  /* is_raw=0: dot-unstuffing already happened in nntp_conn.c. state=NULL:
   * yEnc escapes never span a line break, so each line decodes independently. */
  return rapidyenc_decode_ex(0, src, dest, src_len, NULL);
}

static int
starts_with_keyword(const char *line, size_t len, const char *kw, size_t kw_len) {
  return len >= kw_len && !memcmp(line, kw, kw_len) && (len == kw_len || line[kw_len] == ' ');
}

/* Parses whitespace-separated key=value tokens, filling in whichever of
 * size=/begin=/end=/crc32=/pcrc32= are present; other tokens (line=, part=,
 * total=, =ybegin's name=) are ignored. pcrc32= (this part's checksum)
 * always wins over crc32= (only meaningful whole-file, and only reliable
 * on a single-part article) regardless of order on the line. */
static void
parse_fields(const char *s, yenc_line_t *out) {
  int have_begin = 0, have_end = 0;

  while (*s) {
    const char *tok;
    size_t tok_len;

    while (*s == ' ') s++;
    if (!*s) break;
    tok = s;
    while (*s && *s != ' ') s++;
    tok_len = (size_t)(s - tok);

    if (tok_len > 5 && !memcmp(tok, "size=", 5)) {
      out->size = strtol(tok + 5, NULL, 10);
      out->has_size = 1;
    } else if (tok_len > 6 && !memcmp(tok, "begin=", 6)) {
      out->begin = strtol(tok + 6, NULL, 10);
      have_begin = 1;
    } else if (tok_len > 4 && !memcmp(tok, "end=", 4)) {
      out->end = strtol(tok + 4, NULL, 10);
      have_end = 1;
    } else if (tok_len > 7 && !memcmp(tok, "pcrc32=", 7)) {
      out->crc32 = (uint32_t)strtoul(tok + 7, NULL, 16);
      out->has_crc32 = 1;
    } else if (tok_len > 6 && !out->has_crc32 && !memcmp(tok, "crc32=", 6)) {
      out->crc32 = (uint32_t)strtoul(tok + 6, NULL, 16);
      out->has_crc32 = 1;
    }
  }

  out->has_range = have_begin && have_end;
}

yenc_line_kind_t
yenc_classify_line(const char *line, size_t len, yenc_line_t *out) {
  memset(out, 0, sizeof *out);

  if (starts_with_keyword(line, len, "=ybegin", 7)) {
    parse_fields(line + 7, out);
    return YENC_LINE_BEGIN;
  }
  if (starts_with_keyword(line, len, "=ypart", 6)) {
    parse_fields(line + 6, out);
    return YENC_LINE_PART;
  }
  if (starts_with_keyword(line, len, "=yend", 5)) {
    parse_fields(line + 5, out);
    return YENC_LINE_END;
  }

  return YENC_LINE_DATA;
}
