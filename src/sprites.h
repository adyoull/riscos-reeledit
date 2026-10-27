/* sprites.h - RISC OS sprite files read (sprites.c): a sprite as RGBA, to
 * be written as a PNG (still.h) and put over the video.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef RE_SPRITES_H
#define RE_SPRITES_H
#include <stdint.h>

#define RE_SPRITES_MAX 64 /* (a file's first 64: enough to pick from) */

/* The names of a sprite file's sprites (up to max): their number, or -1
   if it isn't a sprite file */
int re_sprites_names(const char *file, char names[][13], int max);
/* A file's sprite (index from 0) as RGBA, rows of width * 4 bytes, its
   pixels made square, alpha from its mask (or alpha channel). 0 and the
   pixels (free them), or -1. */
int re_sprite_rgba(const char *file, int index, uint8_t **rgba, int *width, int *height);

#endif
