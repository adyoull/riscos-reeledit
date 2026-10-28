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
/* A clip (or a cut of one) is a still picture: one of these services
   (qimage, and those of pictures made: re_still_add_kind) */
int re_still_is(mlt_producer producer);
extern const char *re_still_kind[4];
extern int re_still_kinds;

/* A picture made, not read (a title): key writes what it's made from (the
   producer's properties that matter) as a string; make makes it (RGBA,
   malloc'd: 0, or -1), again whenever the key changes. */
typedef struct
{
    void (*key)(mlt_properties props, char *key, int size);
    int (*make)(mlt_properties props, uint8_t **rgba, int *width, int *height);
} re_still_maker;
/* A producer of such a picture (its service named so: register it as a
   still kind too) */
mlt_producer re_still_made(mlt_profile profile, const char *service, const char *resource, const re_still_maker *maker);
/* A still's size (a made one made now if it must be). 0, or -1. */
int re_still_size(mlt_producer producer, int *width, int *height);
/* A file is a picture this can read (a PNG or a JPEG, by its first
   bytes): 0 and its size, or -1 */
int re_still_open(const char *file, int *width, int *height);
/* RGBA (rows of width * 4 bytes) written as a PNG file. 0, or -1 (no file
   left). */
int re_still_write_png(const char *file, const uint8_t *rgba, int width, int height);

#endif
