/* scale.c - part of a picture plane scaled into part of another (scale.h).
 *
 * Bilinear, in 8-bit fixed point: for each column of the destination, its
 * source column and weight worked out once; for each row, the two source
 * rows it falls between mixed into a row (NEON: re_neon_mix, neon.h), then
 * each destination pixel made from its two neighbours in that row.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "scale.h"
#include "neon.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int re_scale_plane(const uint8_t *src, int sw, int sh, int sstride, uint8_t *dst, int dstride, int x0, int y0,
                   int x1, int y1, double ox, double oy, double fx, double fy)
{
    int n = x1 - x0;
    if (n <= 0 || y1 <= y0 || sw < 1 || sh < 1)
        return 0;
    int32_t *xi = malloc(sizeof(int32_t) * n);
    uint16_t *xw = malloc(sizeof(uint16_t) * n);
    uint8_t *row = malloc((size_t) sw + 16);
    if (!xi || !xw || !row) {
        free(xi);
        free(xw);
        free(row);
        return -1;
    }
    /* (the columns: the source's span they use, lo..hi) */
    int lo = sw, hi = 0;
    for (int i = 0; i < n; i++) {
        double sx = (x0 + i + 0.5 - ox) * fx - 0.5;
        if (sx < 0)
            sx = 0;
        if (sx > sw - 1)
            sx = sw - 1;
        int ix = (int) sx, w = (int) lround((sx - ix) * 256);
        if (w == 256) {
            ix++;
            w = 0;
        }
        if (ix >= sw - 1) { /* (the last column: no pixel after it) */
            ix = sw - 1;
            w = 0;
        }
        xi[i] = ix;
        xw[i] = (uint16_t) w;
        if (ix < lo)
            lo = ix;
        if (ix + (w ? 1 : 0) > hi)
            hi = ix + (w ? 1 : 0);
    }
    for (int i = 0; i < n; i++) /* (indices into the mixed row, which starts at lo) */
        xi[i] -= lo;
    int span = hi - lo + 1;
    for (int y = y0; y < y1; y++) {
        double sy = (y + 0.5 - oy) * fy - 0.5;
        if (sy < 0)
            sy = 0;
        if (sy > sh - 1)
            sy = sh - 1;
        int iy = (int) sy, w = (int) lround((sy - iy) * 256);
        if (w == 256) {
            iy++;
            w = 0;
        }
        if (iy >= sh - 1) {
            iy = sh - 1;
            w = 0;
        }
        const uint8_t *a = src + (size_t) iy * sstride + lo, *b = w ? a + sstride : a;
        if (!w)
            memcpy(row, a, span);
        else if (!re_neon_mix(a, b, row, span, w))
            for (int x = 0; x < span; x++)
                row[x] = (uint8_t) ((a[x] * (256 - w) + b[x] * w + 128) >> 8);
        row[span] = row[span - 1]; /* (one past: read with a weight of 0) */
        uint8_t *out = dst + (size_t) y * dstride + x0;
        if (!re_neon_row(row, xi, xw, out, n))
            for (int i = 0; i < n; i++)
                out[i] = (uint8_t) ((row[xi[i]] * (256 - xw[i]) + row[xi[i] + 1] * xw[i] + 128) >> 8);
    }
    free(xi);
    free(xw);
    free(row);
    return 0;
}
