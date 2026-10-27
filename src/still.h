/* still.h - still pictures (PNG, JPEG...) as MLT producers, decoded once
 * and kept (still.c): MLT's "qimage" service in this build. And PNGs
 * written.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef RE_STILL_H
#define RE_STILL_H
#include <framework/mlt.h>

/* Registered with MLT (re_mlt_start does it) */
void re_still_register(void);
/* A clip (or a cut of one) is a still picture */
int re_still_is(mlt_producer producer);
/* A file is a picture this can read (a PNG or a JPEG, by its first
   bytes): 0 and its size, or -1 */
int re_still_open(const char *file, int *width, int *height);
/* RGBA (rows of width * 4 bytes) written as a PNG file. 0, or -1 (no file
   left). */
int re_still_write_png(const char *file, const uint8_t *rgba, int width, int height);

#endif
