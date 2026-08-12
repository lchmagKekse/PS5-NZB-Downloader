#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <expat.h>

#include "../log/log.h"
#include "../storage/paths.h"
#include "nzb_parse.h"

#define NZB_PARSE_CHUNK_SIZE 8192
#define SEG_TEXT_MAX 511

#define META_TEXT_MAX 255

typedef struct {
  job_t      *job;
  job_file_t *current_file;   /* NULL when not inside <file> */
  int         in_segment;
  long        seg_bytes;
  int         seg_number;
  char        seg_text[SEG_TEXT_MAX + 1];
  size_t      seg_text_len;

  /* <head><meta type="password">...</meta></head> -- some indexers embed
   * archive password(s) this way; collected into job->passwords for
   * extract.c to try. True only between the start and end tag (meta has
   * no children, so no stack needed). */
  int         in_password_meta;
  char        meta_text[META_TEXT_MAX + 1];
  size_t      meta_text_len;

  int         error;
} parse_ctx_t;

static const char *
find_attr(const char **attrs, const char *name) {
  int i;
  for (i = 0; attrs[i]; i += 2) {
    if (!strcmp(attrs[i], name)) return attrs[i + 1];
  }
  return NULL;
}

static void
trim_ws(char *s) {
  char *start = s;
  size_t len;

  while (*start && isspace((unsigned char)*start)) start++;
  len = strlen(start);
  while (len > 0 && isspace((unsigned char)start[len - 1])) len--;

  memmove(s, start, len);
  s[len] = 0;
}

/* Strips a trailing NNTP posting-convention part suffix -- bare "(N/M)" or
 * "yEnc (N/M)" -- from the end of s, in place. Needed because when a
 * subject lacks quotes around the real filename, extract_filename() falls
 * back to the whole subject; without this, "movie.part01.rar (1/50)" fails
 * download.c's archive-format detection (ends-with checks). */
static void
strip_trailing_part_suffix(char *s) {
  size_t len = strlen(s);
  char *paren, *p, *slash = NULL;

  if (len == 0 || s[len - 1] != ')') return;

  paren = strrchr(s, '(');
  if (!paren) return;

  /* Everything between '(' and the final ')' must be DIGITS '/' DIGITS,
   * with exactly one slash and at least one digit on each side. */
  for (p = paren + 1; p < s + len - 1; p++) {
    if (*p == '/') {
      if (slash) return;
      slash = p;
    } else if (!isdigit((unsigned char)*p)) {
      return;
    }
  }
  if (!slash || slash == paren + 1 || slash == s + len - 2) return;

  /* '(' must start the suffix -- preceded by whitespace or be the very
   * first character -- so a filename that legitimately contains "(...)"
   * earlier in the name is left alone. */
  if (paren != s && !isspace((unsigned char)paren[-1])) return;

  *paren = 0;
  trim_ws(s);

  len = strlen(s);
  if (len >= 4 && !strncasecmp(s + len - 4, "yenc", 4) &&
      (len == 4 || isspace((unsigned char)s[len - 5]))) {
    s[len - 4] = 0;
    trim_ws(s);
  }
}

/* yEnc posting convention puts the real filename in double quotes within
 * the subject, e.g. `"My.File.mkv" yEnc (1/50)`. Fall back to the whole
 * subject (minus its trailing part-count suffix, if any -- see
 * strip_trailing_part_suffix()) when that convention isn't followed. */
static void
extract_filename(const char *subject, char *out, size_t out_size) {
  const char *start = strchr(subject, '"');
  const char *end = start ? strchr(start + 1, '"') : NULL;

  if (start && end && end > start + 1) {
    size_t len = (size_t)(end - start - 1);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, start + 1, len);
    out[len] = 0;
  } else {
    snprintf(out, out_size, "%s", subject);
    strip_trailing_part_suffix(out);
  }

  path_sanitize_component(out, out_size);
}

static void XMLCALL
start_element(void *user_data, const XML_Char *name, const XML_Char **attrs) {
  parse_ctx_t *ctx = user_data;

  if (!strcmp(name, "file")) {
    const char *subject = find_attr(attrs, "subject");
    char filename[512];

    extract_filename(subject ? subject : "", filename, sizeof filename);
    ctx->current_file = job_add_file(ctx->job, filename, subject ? subject : "");
    if (!ctx->current_file) {
      log_error("nzb: out of memory adding file");
      ctx->error = 1;
    }
  } else if (!strcmp(name, "segment")) {
    const char *bytes = find_attr(attrs, "bytes");
    const char *number = find_attr(attrs, "number");

    ctx->in_segment = 1;
    ctx->seg_bytes = bytes ? atol(bytes) : 0;
    ctx->seg_number = number ? atoi(number) : 0;
    ctx->seg_text_len = 0;
    ctx->seg_text[0] = 0;
  } else if (!strcmp(name, "meta")) {
    const char *type = find_attr(attrs, "type");

    ctx->in_password_meta = (type && !strcasecmp(type, "password"));
    ctx->meta_text_len = 0;
    ctx->meta_text[0] = 0;
  }
}

static void XMLCALL
char_data(void *user_data, const XML_Char *s, int len) {
  parse_ctx_t *ctx = user_data;
  size_t remain;

  if (len <= 0) return;

  if (ctx->in_segment) {
    remain = sizeof ctx->seg_text - 1 - ctx->seg_text_len;
    if (remain == 0) return;
    if ((size_t)len > remain) len = (int)remain;

    memcpy(ctx->seg_text + ctx->seg_text_len, s, (size_t)len);
    ctx->seg_text_len += (size_t)len;
    ctx->seg_text[ctx->seg_text_len] = 0;
  } else if (ctx->in_password_meta) {
    remain = sizeof ctx->meta_text - 1 - ctx->meta_text_len;
    if (remain == 0) return;
    if ((size_t)len > remain) len = (int)remain;

    memcpy(ctx->meta_text + ctx->meta_text_len, s, (size_t)len);
    ctx->meta_text_len += (size_t)len;
    ctx->meta_text[ctx->meta_text_len] = 0;
  }
}

static void XMLCALL
end_element(void *user_data, const XML_Char *name) {
  parse_ctx_t *ctx = user_data;

  if (!strcmp(name, "file")) {
    ctx->current_file = NULL;
  } else if (!strcmp(name, "segment")) {
    trim_ws(ctx->seg_text);

    if (!ctx->current_file) {
      log_warn("nzb: <segment> outside of <file>, ignoring");
    } else if (ctx->seg_number < 1) {
      log_warn("nzb: segment with missing/invalid number, ignoring");
    } else if (ctx->seg_text[0] == 0) {
      log_warn("nzb: segment %d with empty message-id, ignoring", ctx->seg_number);
    } else {
      char msgid[SEG_TEXT_MAX + 3];

      if (ctx->seg_text[0] == '<') {
        snprintf(msgid, sizeof msgid, "%s", ctx->seg_text);
      } else {
        snprintf(msgid, sizeof msgid, "<%s>", ctx->seg_text);
      }

      if (job_file_add_segment(ctx->current_file, msgid, ctx->seg_bytes, ctx->seg_number) < 0) {
        log_error("nzb: out of memory adding segment");
        ctx->error = 1;
      }
    }

    ctx->in_segment = 0;
  } else if (!strcmp(name, "meta")) {
    if (ctx->in_password_meta) {
      trim_ws(ctx->meta_text);
      job_add_password(ctx->job, ctx->meta_text);
    }
    ctx->in_password_meta = 0;
  }
}

static const char *
basename_of(const char *path) {
  const char *slash = strrchr(path, '/');
  const char *bslash = strrchr(path, '\\');
  const char *base = path;

  if (slash && slash + 1 > base) base = slash + 1;
  if (bslash && bslash + 1 > base) base = bslash + 1;

  return base;
}

job_t *
nzb_parse_file(const char *path) {
  FILE *f;
  XML_Parser parser;
  parse_ctx_t ctx = {0};
  char buf[NZB_PARSE_CHUNK_SIZE];
  job_t *job;

  if (!(f = fopen(path, "rb"))) {
    log_error("nzb: fopen(%s): %s", path, strerror(errno));
    return NULL;
  }

  if (!(job = job_create(basename_of(path)))) {
    log_error("nzb: out of memory creating job");
    fclose(f);
    return NULL;
  }

  if (!(parser = XML_ParserCreate(NULL))) {
    log_error("nzb: XML_ParserCreate failed");
    job_free(job);
    fclose(f);
    return NULL;
  }

  ctx.job = job;
  XML_SetUserData(parser, &ctx);
  XML_SetElementHandler(parser, start_element, end_element);
  XML_SetCharacterDataHandler(parser, char_data);

  for (;;) {
    size_t n = fread(buf, 1, sizeof buf, f);
    int is_final = n < sizeof buf;

    if (is_final && ferror(f)) {
      log_error("nzb: read error on %s: %s", path, strerror(errno));
      XML_ParserFree(parser);
      job_free(job);
      fclose(f);
      return NULL;
    }

    if (XML_Parse(parser, buf, (int)n, is_final) == XML_STATUS_ERROR) {
      log_error("nzb: %s:%lu: %s", path,
                (unsigned long)XML_GetCurrentLineNumber(parser),
                XML_ErrorString(XML_GetErrorCode(parser)));
      XML_ParserFree(parser);
      job_free(job);
      fclose(f);
      return NULL;
    }

    if (ctx.error) {
      XML_ParserFree(parser);
      job_free(job);
      fclose(f);
      return NULL;
    }

    if (is_final) break;
  }

  XML_ParserFree(parser);
  fclose(f);

  log_info("nzb: parsed %s: %zu file(s)", path, job->file_count);
  return job;
}
