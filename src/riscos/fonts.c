/* fonts.c - titles' text drawn by the Font Manager (app.h; title.h's font
 * renderer, given at start). Each line is painted, white on black,
 * anti-aliased, into a 32bpp sprite of its own (output switched to it),
 * and how bright each pixel came out is how much it's covered.
 *
 * The sprite is 90 dpi (a pixel is 2 OS units; a point 1.25 pixels), so a
 * font px pixels to the em is px * 0.8 points. A font that can't be found
 * is Homerton.Medium instead.
 *
 * RISC OS only (the Linux tests have no Font Manager: titles there are
 * blocks).
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "app.h"

#include <stdlib.h>
#include <string.h>

#ifdef __riscos__

#define Font_FindFont              0x40081
#define Font_LoseFont              0x40082
#define Font_Paint                 0x40086
#define Font_ScanString            0x400A1
#define ColourTrans_SetFontColours 0x4074F

#define MODE_32BPP_90DPI 0x301680B5u /* (sprite type 6, 90 x 90 dpi) */

static int find(const char *font, int px)
{
    _kernel_swi_regs r;
    int size16 = px * 64 / 5; /* (16ths of a point: px * 0.8 * 16) */
    r.r[1] = (intptr_t) font;
    r.r[2] = size16; r.r[3] = size16;
    r.r[4] = 90; r.r[5] = 90;
    if (!ro_swi(Font_FindFont, &r))
        return (int) r.r[0];
    r.r[1] = (intptr_t) "Homerton.Medium";
    r.r[2] = size16; r.r[3] = size16;
    r.r[4] = 90; r.r[5] = 90;
    return ro_swi(Font_FindFont, &r) ? -1 : (int) r.r[0];
}

int fm_line(const char *font, int px, const char *line, uint8_t **mask, int *width, int *height)
{
    _kernel_swi_regs r;
    *mask = NULL;
    int h = px * 5 / 4 + 2, base = px * 3 / 10 + 1; /* (the line's height; the baseline above its bottom) */
    if (!*line) { /* an empty line: its height, blank */
        *mask = calloc(h, 1);
        *width = 1;
        *height = h;
        return *mask ? 0 : -1;
    }
    int f = find(font, px);
    if (f < 0)
        return -1;
    /* its width: the string's advance (millipoints: 800 a pixel), a little more for slanted letters */
    r.r[0] = f;
    r.r[1] = (intptr_t) line;
    r.r[2] = 1 << 8;
    r.r[3] = 0x7FFFFFFF;
    r.r[4] = 0x7FFFFFFF;
    int w = !ro_swi(Font_ScanString, &r) ? (int) r.r[3] / 800 + px / 3 + 4 : 0;
    int ok = -1;
    size_t bytes = (size_t) w * h * 4;
    int *area = w > 0 && w < 8192 ? malloc(16 + 44 + bytes) : NULL;
    if (area) {
        area[0] = (int) (16 + 44 + bytes);
        area[1] = 0;
        area[2] = 16;
        area[3] = 16;
        r.r[0] = 256 + 9; /* (initialise the area) */
        r.r[1] = (intptr_t) area;
        ro_swi(OS_SpriteOp, &r);
        r.r[0] = 256 + 15; /* create a sprite: 32bpp, no palette */
        r.r[1] = (intptr_t) area;
        r.r[2] = (intptr_t) "line";
        r.r[3] = 0;
        r.r[4] = w;
        r.r[5] = h;
        r.r[6] = (int) MODE_32BPP_90DPI;
        if (!ro_swi(OS_SpriteOp, &r)) {
            int *spr = area + area[2] / 4; /* (the first sprite: area[2] bytes in, a word multiple) */
            uint8_t *data = (uint8_t *) spr + spr[8];
            memset(data, 0, bytes);
            r.r[0] = 256 + 60; /* output to it */
            r.r[1] = (intptr_t) area;
            r.r[2] = (intptr_t) "line";
            r.r[3] = 0;
            if (!ro_swi(OS_SpriteOp, &r)) {
                _kernel_swi_regs back = r; /* (R0-R3: to switch back) */
                r.r[0] = f;
                r.r[1] = 0x00000000;  /* the background: black */
                r.r[2] = (int) 0xFFFFFF00u; /* the letters: white */
                r.r[3] = 14;
                ro_swi(ColourTrans_SetFontColours, &r);
                r.r[0] = f;
                r.r[1] = (intptr_t) line;
                r.r[2] = (1 << 4) | (1 << 8); /* (OS units; R0 the font) */
                r.r[3] = 4;
                r.r[4] = base * 2;
                ok = ro_swi(Font_Paint, &r) ? -1 : 0;
                ro_swi(OS_SpriteOp, &back);
            }
            if (!ok && (*mask = malloc((size_t) w * h))) {
                for (int y = 0; y < h; y++) /* (row 0 at the top; a pixel's red is its brightness) */
                    for (int x = 0; x < w; x++)
                        (*mask)[(size_t) y * w + x] = data[((size_t) y * w + x) * 4];
                *width = w;
                *height = h;
            } else {
                ok = -1;
            }
        }
        free(area);
    }
    r.r[0] = f;
    ro_swi(Font_LoseFont, &r);
    return ok;
}

#endif
