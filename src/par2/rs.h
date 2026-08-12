/* GF(2^16) arithmetic and Reed-Solomon matrix solving for PAR2 repair --
 * ported line-for-line from par2cmdline's galois.h/reedsolomon.h
 * (generator 0x1100B, Vandermonde matrix + Gauss-Jordan elimination) so
 * reconstructed bytes are bit-identical to a conforming PAR2 tool's
 * output; a subtly wrong base/matrix choice would silently corrupt a
 * "successfully repaired" file rather than fail loudly. Repair-side only
 * -- no support for creating new recovery blocks. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef uint16_t gf16_t;

/* Builds the log/antilog tables. Not thread-safe -- call once from
 * main() before any thread can reach par2_repair_set(), same pattern as
 * yenc_global_init(). A second call is a cheap no-op (idempotent). */
void gf16_init(void);

gf16_t gf16_mul(gf16_t a, gf16_t b);
gf16_t gf16_div(gf16_t a, gf16_t b);  /* caller guarantees b != 0 */
gf16_t gf16_pow(gf16_t base, uint16_t exponent);

/* dst[i] ^= gf16_mul(src[i], factor) over 16-bit little-endian values
 * packed in the len-byte buffers (len must be even). Bytes are
 * assembled/disassembled manually rather than cast through a uint16_t*,
 * avoiding alignment/strict-aliasing issues. No-op if factor == 0. */
void gf16_mul_acc(unsigned char *dst, const unsigned char *src, size_t len, gf16_t factor);

/* Assigns the Vandermonde "base" value for global input-block index,
 * using par2cmdline's log-stepping (SetInput). *base_state carries the
 * running counter across calls -- zero it before index 0, then call once
 * per index in ascending order. Returns 0 (unset *out) only if input
 * blocks exceed the field's 65535 limit. */
int gf16_next_base(unsigned int *base_state, gf16_t *out);

/* Solves for unknown_count missing blocks via Gauss-Jordan elimination
 * over GF(2^16) (pure repair: unknown_count equations for unknown_count
 * unknowns). present_bases/missing_bases are per-column database[] values
 * in ascending block-index order; exponents holds the selected slices'
 * values, ascending and distinct.
 *
 * On success, coeffs is row-major unknown_count x (present_count+
 * unknown_count): coeffs[r*(present_count+unknown_count)+c] is the GF
 * factor for column c (< present_count: present block at present_bases[c];
 * else: recovery slice exponents[c-present_count]) accumulated into
 * missing block r (missing_bases[r]). Needs unknown_count*(present_count+
 * unknown_count) gf16_t slots. Returns -1 on a singular matrix. */
int gf16_solve(size_t present_count, const gf16_t *present_bases,
               size_t unknown_count, const gf16_t *missing_bases,
               const uint16_t *exponents,
               gf16_t *coeffs);
