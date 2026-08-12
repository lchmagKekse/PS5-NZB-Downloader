/* Streaming NZB (XML) parser, built on expat. Reads in bounded chunks (see
 * NZB_PARSE_CHUNK_SIZE) rather than slurping the whole file into memory.
 */
#pragma once

#include "../queue/job.h"

/* Parses the NZB file at path into a newly created job_t (name taken from
 * path's basename). Segment message-ids are normalized to always include
 * the enclosing angle brackets NNTP expects. Returns the job on success,
 * or NULL on error (missing file, malformed XML; already logged via
 * log_error()). Caller owns the returned job and must job_free() it. */
job_t *nzb_parse_file(const char *path);
