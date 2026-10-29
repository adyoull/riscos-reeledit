/* scale.h - part of a picture plane scaled into part of another, bilinear
 * (scale.c), with NEON where the CPU has it (neon.h): affine.c's
 * pan and zoom, one plane of a yuv420p picture at a time.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef RE_SCALE_H
#define RE_SCALE_H
#include <stdint.h>

/* The destination's pixels x0..x1-1, y0..y1-1 made from the source (sw x
   sh): destination pixel (u, v) shows source point ((u + 0.5 - ox) * fx -
   0.5, (v + 0.5 - oy) * fy - 0.5), each between its four nearest pixels
   (kept within the source). 0, or -1 (no memory). */
int re_scale_plane(const uint8_t *src, int sw, int sh, int sstride, uint8_t *dst, int dstride, int x0, int y0,
                   int x1, int y1, double ox, double oy, double fx, double fy);

/* A whole plane (sw x sh, rows sw long) scaled to dw x dh (rows dw long):
   halved by 2x2 means while it's twice the size or more, so a big
   reduction doesn't alias, then as re_scale_plane. 0, or -1 (no memory). */
int re_scale_fit(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh);

#endif
