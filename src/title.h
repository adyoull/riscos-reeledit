/* title.h - titles: text over the video, as pictures made from it
 * (title.c). Each is an MLT producer of its own ("qtext", the name of
 * Shotcut's text producer, with its properties), a still picture (still.h)
 * made again whenever its text or look changes; put on V2 as an overlay
 * (project.h's re_project_title_add).
 *
 * The text is drawn by a font renderer the application gives (the Font
 * Manager on RISC OS: riscos/fonts.c); without one, each letter is a
 * block (enough to place and test titles on Linux).
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef RE_TITLE_H
#define RE_TITLE_H
#include <framework/mlt.h>
#include <stdint.h>

enum { RE_TITLE_PLAIN, RE_TITLE_SHADOW, RE_TITLE_OUTLINE, RE_TITLE_BOX, RE_TITLE_STYLES };
enum { RE_TITLE_LEFT, RE_TITLE_CENTRE, RE_TITLE_RIGHT };

typedef struct
{
    char text[512];   /* its lines, '\n' between them */
    char font[64];    /* a RISC OS font's name: "Homerton.Bold" */
    int size;         /* the letters' size (a line's em): % of the frame's height */
    unsigned colour;  /* 0xRRGGBB */
    int style;        /* RE_TITLE_: plain, a shadow, an outline, on a box */
    int align;        /* RE_TITLE_: its lines to the left, centred, to the right */
} re_title;

/* A line of text in a font, px pixels to the em: how much each pixel is
   covered (0 to 255), malloc'd, *height its line's height. 0, or -1. */
typedef int (*re_title_font_fn)(const char *font, int px, const char *line, uint8_t **mask, int *width,
                                 int *height);
void re_title_set_font(re_title_font_fn fn); /* (NULL: blocks) */

/* "qtext" registered with MLT (re_mlt_start does it) */
void re_title_register(void);
/* A title as a picture (RGBA, malloc'd), the frame frame_height pixels
   high. 0, or -1. */
int re_title_render(const re_title *t, int frame_height, uint8_t **rgba, int *width, int *height);
/* A title's producer (for a profile; its properties as Shotcut's qtext's) */
mlt_producer re_title_producer(mlt_profile profile, const re_title *t);
/* A clip (or a cut of one) is a title */
int re_title_is(mlt_producer producer);
/* A title's settings read from its producer (or a cut of it), or written
   to it (it's made again the next time it's shown). 0, or -1. */
int re_title_get(mlt_producer producer, re_title *t);
int re_title_put(mlt_producer producer, const re_title *t);
/* The settings a new title has */
void re_title_default(re_title *t);

#endif
