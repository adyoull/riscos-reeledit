/* neon.h - ReelEdit's own NEON loops (neon.c), each standing in for a C
 * loop, used only when the CPU has NEON (re_neon_on, from the engine's
 * start): each returns 1 done, or 0 (no NEON: the caller's C does it).
 * neon.c alone is built with -mfpu=neon (build-riscos.sh); on a machine
 * without NEON (and on Linux, for the tests) each returns 0.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef RE_NEON_H
#define RE_NEON_H
#include <stdint.h>

void re_neon_on(int on);
int re_neon_is_on(void);

/* scale.c's: a row mixed from two, out[i] = (a[i] * (256 - w) + b[i] * w + 128) >> 8 (w 1..255); and a row
   sampled, out[i] = (row[xi[i]] * (256 - xw[i]) + row[xi[i] + 1] * xw[i] + 128) >> 8 */
int re_neon_mix(const uint8_t *a, const uint8_t *b, uint8_t *out, int n, int w);
int re_neon_row(const uint8_t *row, const int32_t *xi, const uint16_t *xw, uint8_t *out, int n);

/* chroma.c's: n chroma samples' (Cb, Cr) each within var of (ku, kv) made see-through: the two rows of alpha
   below them (a0, a1: 2n each) set to 0 there, left as they are elsewhere */
int re_neon_key(const uint8_t *u, const uint8_t *v, uint8_t *a0, uint8_t *a1, int n, int ku, int kv, int var);

/* looks.c's charcoal, n pixels of a row whose neighbours (xs either side; rows above and below given) are all
   in the picture: each lut[q] for its 3x3's q = sum1^2 + sum2^2 (plus's sums), sat past the table's n entries.
   Returns how many it did (a multiple of 8; 0: none, the C does them). */
int re_neon_charcoal(const uint8_t *above, const uint8_t *row, const uint8_t *below, uint8_t *out, int n, int xs,
                     const uint8_t *lut, int lut_n, uint8_t sat);

/* looks.c's Negative: n bytes in place, p[i] = clamp(base - p[i] + add, lo, hi) (base 0..255, add 0 or 1:
   luma inv - y, chroma 256 - c), the same as the C */
int re_neon_invert(uint8_t *p, int n, int base, int add, int lo, int hi);

#endif
