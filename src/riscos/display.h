/* display.h - the Viewer's picture: through the hardware overlay
 * (VideoOverlay, YV12) while playing with nothing over it, else a 32bpp
 * sprite plotted in the window. As Reel (riscos-ffmpeg's player) does it;
 * docs/DESIGN.md says why.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef RE_DISPLAY_H
#define RE_DISPLAY_H
#include "../engine.h"
#include "ro.h"

/* Reads the screen mode (again after a mode change: the overlay is made
   afresh). task: our Wimp task handle. */
void re_display_init(int task);
void re_display_mode_changed(void);

/* The window and its picture area (work area coordinates, OS units). */
void re_display_place(int window, ro_box pic);

/* A new picture. overlay_ok: playing, so it may go through the overlay
   (paused, the editor draws it in the window, where menus and windows can
   be seen over it). Returns 1 through the overlay, 0 drawn in the window. */
int re_display_show(const re_picture *p, int overlay_ok);

/* A redraw: after the caller's Wimp_RedrawWindow, when it returned a
   rectangle, with its block; this does that one and gets the rest with
   Wimp_GetRectangle. */
void re_display_redraw(int *block);

/* Hides the overlay. 1 if it was showing: the caller must have the picture
   made again (it's not in the sprite). */
int re_display_hide_overlay(void);
int re_display_overlay_shown(void);

/* Something might be over the picture now (a menu, a window): if the
   overlay is showing and it is, hide it (returns 1, as hide). */
int re_display_check_covered(void);

/* A 32bpp sprite's mode word, a pixel a screen pixel, in the screen's
   colour order; the screen's eigen factors, and whether its colours are
   TRGB (else TBGR: R in the low byte) */
int re_display_sprite_mode(void);
void re_display_screen(int *xeig, int *yeig, int *trgb);

/* Frees the sprite and the overlay. */
void re_display_close(void);

typedef struct
{
    int overlay_pictures;  /* shown through the overlay */
    int sprite_pictures;   /* drawn in the window */
    int overlay_waited;    /* pictures that waited for the overlay's last switch (a vsync) */
    int overlay_skipped;   /* ... and were left out: no vsync in 3 cs */
    int overlay_failed;    /* 1: the overlay can't be used (until the size or mode changes) */
} re_display_stats;
void re_display_get_stats(re_display_stats *s);

/* For the log: a function taking one line (NULL: none). */
void re_display_set_log(void (*log)(const char *line));

#endif
