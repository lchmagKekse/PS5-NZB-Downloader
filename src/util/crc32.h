/* CRC-32 (IEEE 802.3 polynomial, the same variant zlib/yEnc/PAR2 use).
 * Streaming/incremental: feed it chunks as they arrive rather than
 * requiring the whole buffer up front, matching the yEnc decoder's
 * one-line/one-chunk-at-a-time output.
 */
#pragma once

#include <stddef.h>

/* Starting value for a new checksum; pass to the first crc32_update(). */
unsigned long crc32_init(void);

/* Folds len bytes of data into crc (the previous crc32_init()/
 * crc32_update() return value) and returns the updated running value.
 * The result is NOT the final CRC-32 -- call crc32_final() for that. */
unsigned long crc32_update(unsigned long crc, const unsigned char *data, size_t len);

/* Finalizes a running value from crc32_update() into the actual CRC-32. */
unsigned long crc32_final(unsigned long crc);
