#include <stdlib.h>
#include <string.h>

#include "rs.h"

#define GF_BITS 16
#define GF_COUNT 65536u   /* 1 << GF_BITS */
#define GF_LIMIT 65535u   /* GF_COUNT - 1 */
#define GF_GENERATOR 0x1100Bu

static gf16_t g_log[GF_COUNT];
static gf16_t g_antilog[GF_COUNT];
static int g_initialized = 0;

void
gf16_init(void) {
  unsigned int b, l;

  if (g_initialized) return;

  b = 1;
  for (l = 0; l < GF_LIMIT; l++) {
    g_log[b] = (gf16_t)l;
    g_antilog[l] = (gf16_t)b;

    b <<= 1;
    if (b & GF_COUNT) b ^= GF_GENERATOR;
  }

  g_log[0] = (gf16_t)GF_LIMIT;
  g_antilog[GF_LIMIT] = 0;

  g_initialized = 1;
}

gf16_t
gf16_mul(gf16_t a, gf16_t b) {
  unsigned int sum;

  if (a == 0 || b == 0) return 0;

  sum = (unsigned int)g_log[a] + (unsigned int)g_log[b];
  if (sum >= GF_LIMIT) sum -= GF_LIMIT;
  return g_antilog[sum];
}

gf16_t
gf16_div(gf16_t a, gf16_t b) {
  int sum;

  if (a == 0) return 0;

  sum = (int)g_log[a] - (int)g_log[b];
  if (sum < 0) sum += (int)GF_LIMIT;
  return g_antilog[sum];
}

gf16_t
gf16_pow(gf16_t base, uint16_t exponent) {
  unsigned int sum;

  if (exponent == 0) return 1;
  if (base == 0) return 0;

  /* log[base] < GF_LIMIT (65535) and exponent <= 65535, so the product
   * fits comfortably in 32 bits (max ~4.29e9 < UINT32_MAX). The
   * shift/add is a mod-GF_LIMIT reduction (since 2^16 == 1 mod 65535);
   * one pass plus one conditional subtraction is provably enough to
   * land back under GF_LIMIT for this bounded operand range -- see
   * rs.h / the reference Galois16::pow this mirrors. */
  sum = (unsigned int)g_log[base] * (unsigned int)exponent;
  sum = (sum >> GF_BITS) + (sum & GF_LIMIT);
  if (sum >= GF_LIMIT) sum -= GF_LIMIT;
  return g_antilog[sum];
}

void
gf16_mul_acc(unsigned char *dst, const unsigned char *src, size_t len, gf16_t factor) {
  size_t i;

  if (factor == 0) return;

  for (i = 0; i < len; i += 2) {
    gf16_t sv = (gf16_t)src[i] | ((gf16_t)src[i + 1] << 8);
    gf16_t dv = (gf16_t)dst[i] | ((gf16_t)dst[i + 1] << 8);
    gf16_t out = dv ^ gf16_mul(sv, factor);
    dst[i] = (unsigned char)(out & 0xff);
    dst[i + 1] = (unsigned char)(out >> 8);
  }
}

static uint32_t
gf16_gcd(uint32_t a, uint32_t b) {
  if (a && b) {
    while (a && b) {
      if (a > b) a %= b;
      else b %= a;
    }
    return a + b;
  }
  return 0;
}

int
gf16_next_base(unsigned int *base_state, gf16_t *out) {
  unsigned int logbase = *base_state;

  while (gf16_gcd(GF_LIMIT, logbase) != 1) logbase++;
  if (logbase >= GF_LIMIT) return 0;

  *out = g_antilog[logbase];
  *base_state = logbase + 1;
  return 1;
}

/* Gauss-Jordan elimination, ported from ReedSolomon<Galois16>::GaussElim
 * restricted to the pure-repair case (parmissing == 0, i.e. rows ==
 * datamissing == unknown_count exactly -- see rs.h). leftmatrix is
 * rows x leftcols, rightmatrix is rows x rows (square); both row-major.
 * Subtraction in GF(2^n) is XOR (x - y == x + y == x ^ y). */
static int
gauss_elim(unsigned int rows, unsigned int leftcols, gf16_t *leftmatrix, gf16_t *rightmatrix) {
  unsigned int row, row2, col;

  for (row = 0; row < rows; row++) {
    gf16_t pivotvalue = rightmatrix[row * rows + row];
    if (pivotvalue == 0) return -1;

    if (pivotvalue != 1) {
      for (col = 0; col < leftcols; col++) {
        if (leftmatrix[row * leftcols + col] != 0) {
          leftmatrix[row * leftcols + col] = gf16_div(leftmatrix[row * leftcols + col], pivotvalue);
        }
      }
      rightmatrix[row * rows + row] = 1;
      for (col = row + 1; col < rows; col++) {
        if (rightmatrix[row * rows + col] != 0) {
          rightmatrix[row * rows + col] = gf16_div(rightmatrix[row * rows + col], pivotvalue);
        }
      }
    }

    for (row2 = 0; row2 < rows; row2++) {
      gf16_t scalevalue;

      if (row2 == row) continue;

      scalevalue = rightmatrix[row2 * rows + row];
      if (scalevalue == 1) {
        for (col = 0; col < leftcols; col++) {
          if (leftmatrix[row * leftcols + col] != 0) {
            leftmatrix[row2 * leftcols + col] ^= leftmatrix[row * leftcols + col];
          }
        }
        for (col = row; col < rows; col++) {
          if (rightmatrix[row * rows + col] != 0) {
            rightmatrix[row2 * rows + col] ^= rightmatrix[row * rows + col];
          }
        }
      } else if (scalevalue != 0) {
        for (col = 0; col < leftcols; col++) {
          if (leftmatrix[row * leftcols + col] != 0) {
            leftmatrix[row2 * leftcols + col] ^= gf16_mul(leftmatrix[row * leftcols + col], scalevalue);
          }
        }
        for (col = row; col < rows; col++) {
          if (rightmatrix[row * rows + col] != 0) {
            rightmatrix[row2 * rows + col] ^= gf16_mul(rightmatrix[row * rows + col], scalevalue);
          }
        }
      }
    }
  }

  return 0;
}

int
gf16_solve(size_t present_count, const gf16_t *present_bases,
           size_t unknown_count, const gf16_t *missing_bases,
           const uint16_t *exponents,
           gf16_t *coeffs) {
  unsigned int incount = (unsigned int)(present_count + unknown_count);
  unsigned int rows = (unsigned int)unknown_count;
  gf16_t *rightmatrix;
  unsigned int row, col;
  int rc;

  if (rows == 0) return 0;

  memset(coeffs, 0, (size_t)rows * incount * sizeof *coeffs);

  if (!(rightmatrix = calloc((size_t)rows * rows, sizeof *rightmatrix))) return -1;

  /* Layout matches ReedSolomon<Galois16>::Compute() with parmissing==0:
   * leftmatrix columns [0..present_count) are powers of the present
   * blocks' base values; columns [present_count..incount) start as an
   * identity block (each selected recovery slice used as its own
   * "extra input" before elimination transforms these into the real
   * coefficients on that slice's raw data). rightmatrix row r, col c is
   * missing_bases[c]^exponents[r] -- elimination drives this to the
   * identity, which is what turns leftmatrix row r into the
   * reconstruction formula for missing block r. */
  for (row = 0; row < rows; row++) {
    uint16_t exponent = exponents[row];

    for (col = 0; col < present_count; col++) {
      coeffs[row * incount + col] = gf16_pow(present_bases[col], exponent);
    }
    for (col = 0; col < rows; col++) {
      coeffs[row * incount + present_count + col] = (row == col) ? 1 : 0;
    }
    for (col = 0; col < rows; col++) {
      rightmatrix[row * rows + col] = gf16_pow(missing_bases[col], exponent);
    }
  }

  rc = gauss_elim(rows, incount, coeffs, rightmatrix);

  free(rightmatrix);
  return rc;
}
