/* timeline.c - the Timeline window (app.h): a ruler with the playhead, and
 * track V1's clips, gaps and dissolves, and V2's overlays, scrolled sideways by the window's
 * own scroll bar.
 *
 * Above V1, V2's row: the overlays (pictures and videos over the video, overlays.c),
 * each a box over the frames it's shown for; a click selects one, Menu
 * gives its menu, a drag moves it (its middle) or trims it (its ends), and
 * pictures dropped on the row become overlays there.
 *
 * The ruler is the only place the playhead moves from: a click puts it
 * there, a drag scrubs. On the track, a click selects a clip or a dissolve
 * (Adjust: toggles), on empty space clears the selection; files dropped on
 * it go in at the frame they're dropped on. Dragged on the track (see
 * drag_mode_at):
 *   - a clip's edge trims it (ripple: what follows moves along; the Viewer
 *     shows the frame at the edge); with Ctrl, rolls the cut there (the
 *     clips either side trimmed together, nothing else moving);
 *   - a clip's middle moves it (a line shows where it'll go, at the cut
 *     nearest the pointer);
 *   - the white mark at the bottom of a cut makes a dissolve there, as long
 *     as the drag;
 *   - a dissolve's ends make it longer or shorter;
 *   - a fade's box's inner edge (fades are given from the context menu,
 *     effects.c) makes it longer or shorter.
 * While dragging, the Viewer shows the frame of interest once the pointer
 * rests (show_later), so each step is cheap and the drag keeps up.
 * Each drag is one undo. Drawn with ColourTrans and OS_Plot, the names with
 * the desktop font (Wimp_TextOp). A clip shows its first and last frames
 * and its waveform, a sound its waveform (previews.c).
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ColourTrans_SetGCOL 0x40743
#define OS_Plot             0x45
#define Wimp_TextOp         0x400F9
#define Wimp_DragBox        0x400D0

#define RULER_H 48   /* OS units */
#define TRACK_H 160  /* (the top for fade handles, the bottom for dissolve marks, the middle to trim and move) */
#define LEFT    16   /* frame 0's x */
#define MIN_W   640
#define LABEL_W 200  /* the playhead's timecode, beside it on the ruler */
#define MARK_TEXT_W 140 /* a ruler mark's label (m:ss.ff), at most */
#define SNAP    12   /* OS units: the playhead snaps to a cut this near */
#define EDGE    24   /* OS units: a drag this near a clip's edge trims it (a third of a narrow clip) */
/* Colours (BBGGRR00): video blue; effects (fades, dissolves) purple, so
   what's applied to a clip doesn't read as part of it */
#define CLIP_COLOUR   0xC0905000u
#define EFFECT_COLOUR 0xA0608000u /* (128, 96, 160) */
#define EFFECT_DARK   0x783C5800u /* (88, 60, 120) */
#define MARKED_COLOUR 0xF8D8B000u /* (176, 216, 248): the ruler between the I/O marks */
#define MARK_COLOUR   0xB0601800u /* (24, 96, 176): the marks' lines */
#define OVERLAY_COLOUR 0x80A03000u /* (48, 160, 128): V2's pictures */
#define SOUND_COLOUR 0x50B05000u /* (80, 176, 80): a clip's sound fades, wedges along its bottom */
#define MUTED_COLOUR 0x2828B000u /* (176, 40, 40): a muted clip's bottom edge */
#define CORNER  32   /* OS units: the track's top (fade handles) and bottom (dissolve marks) strips */
#define WAVE_H  32   /* a clip's waveform: along its bottom */
#define THUMB_H (TRACK_H - CORNER - WAVE_H) /* its thumbnails (previews.c), between */
#define WAVE_COLOUR  0xF0C89000u /* (144, 200, 240): a clip's waveform */
#define AUDIO_WAVE   0x80D8A000u /* (160, 216, 128): a sound's */
#define RENDER_H 8   /* render previews' bar, under the ruler */
#define RENDER_COLOUR   0x3030D000u /* (208, 48, 48): a stretch to render */
#define RENDERED_COLOUR 0x40B04000u /* (64, 176, 64): rendered */
#define V2_H    64   /* V2's row: the overlays (pictures over the video), above V1 */
#define V2_TOP (-RULER_H - 8) /* the rows' tops and bottoms (work area) */
#define V2_BOTTOM (V2_TOP - V2_H)
#define TRACK_TOP (V2_BOTTOM - 8)
#define TRACK_BOTTOM (TRACK_TOP - TRACK_H)
#define A_H 56        /* an audio track's row (A1, A2...: below V1), and the empty one after them */
#define AUDIO_TOP(k) (TRACK_BOTTOM - 8 - (k) * (A_H + 8))
#define AUDIO_BOTTOM(k) (AUDIO_TOP(k) - A_H)
#define AUDIO_COLOUR 0x40986000u /* (96, 152, 64): a sound on an audio track */
/* The rows of audio tracks shown: the tracks, and one more (empty: to drop
   on) */
static int audio_rows(void)
{
    return (A.proj ? re_project_audio_tracks(A.proj) : 0) + 1;
}
#define WIN_H (-AUDIO_BOTTOM(audio_rows() - 1) + 8)

/* What a drag does: the playhead (on the ruler); on the track, a clip's
   start or end trimmed, a clip moved, a dissolve made at a cut (from the
   cut's top corner), a dissolve's start or end dragged (longer, shorter),
   a clip's fade in or out made longer or shorter (from its handle at the
   top of the clip) */
enum { DRAG_NONE, DRAG_SCRUB, DRAG_IN, DRAG_OUT, DRAG_MOVE, DRAG_DISSOLVE, DRAG_MIX_IN, DRAG_MIX_OUT, DRAG_FADE_IN,
       DRAG_FADE_OUT, DRAG_OV_MOVE, DRAG_OV_IN, DRAG_OV_OUT, DRAG_AU_MOVE, DRAG_AU_IN, DRAG_AU_OUT, DRAG_ROLL };

static struct
{
    int win;
    double scale;     /* OS units a frame */
    int vis_w, vis_h, scroll_x;
    int playhead;     /* the frame it's drawn at */
    int extent_w;
    char title[64];
    int drag;         /* DRAG_: what a drag going on does */
    /* the item dragged: as it was when the drag began */
    int d_item, d_in, d_out, d_start, d_f0;
    int d_track;      /* (a sound's audio track) */
    int d_before, d_line; /* moving: where it would go (before item d_before), the line's frame (-1: none) */
    int d_mix, d_len;     /* a dissolve: the item made or dragged (-1: none yet), its length when the drag began */
    /* the Viewer while dragging: the frame to show once the pointer rests
       (-1: none), the pointer's last frame and when it got there */
    int d_show, d_raw;
    unsigned d_moved_cs;
} T = {0, 4.0, 0, 0, 0, 0, 0, "Timeline", DRAG_NONE, 0, 0, 0, 0, 0, 0, -1, -1, -1, 0, -1, -1, 0};

static int x_of(int frame)
{
    double x = LEFT + frame * T.scale; /* (kept in range: an endless clip would wrap round) */
    return x > 0x3FFFFFFF ? 0x3FFFFFFF : (int) x;
}

static int frame_of(int wx)
{
    int f = (int) ((wx - LEFT) / T.scale + 0.5);
    return f < 0 ? 0 : f;
}

int tl_window(void)
{
    return T.win;
}

/* ---- the window --------------------------------------------------------- */

static int snapped(int f);

static int create(void)
{
    struct
    {
        ro_box vis;
        int sx, sy, behind, flags;
        unsigned char tfg, tbg, wfg, wbg, sofg, sibg, tfocus, xflags;
        ro_box ext;
        int tflags, wbutton, sprites;
        short minw, minh;
        struct
        {
            char *text;
            const char *valid;
            int len;
        } title;
        int nicons;
    } w;
    _kernel_swi_regs r;
    memset(&w, 0, sizeof(w));
    w.vis.x1 = 1000; w.vis.y1 = WIN_H;
    w.behind = -1;
    /* new format, back, close, title, toggle, horizontal scroll, adjust size, moveable */
    w.flags = (int) 0xEF000102u; /* (bit 8: Scroll_Request for the arrows and the wheel) */
    w.tfg = 7; w.tbg = 2; w.wfg = 7; w.wbg = 1;
    w.sofg = 3; w.sibg = 1; w.tfocus = 12;
    w.ext.x0 = 0; w.ext.y0 = -(WIN_H); w.ext.x1 = 4096; w.ext.y1 = 0;
    w.tflags = 0x1 | 0x4 | 0x8 | 0x10 | 0x20 | 0x100 | (7 << 24) | (2 << 28);
    w.wbutton = 10 << 12; /* click (x256), drag (x16), double click */
    w.sprites = 1;
    w.minw = MIN_W; w.minh = WIN_H;
    w.title.text = T.title;
    w.title.valid = (const char *) -1;
    w.title.len = sizeof(T.title);
    r.r[1] = (intptr_t) &w;
    if (ro_swi(Wimp_CreateWindow, &r))
        return -1;
    T.win = (int) r.r[0];
    return 0;
}

static void set_extent(void)
{
    int len = A.proj ? re_project_length(A.proj) : 0;
    int w = x_of(len) + 400;
    if (w < T.vis_w)
        w = T.vis_w;
    if (w < MIN_W)
        w = MIN_W;
    int b[4] = {0, -(WIN_H), w, 0};
    _kernel_swi_regs r;
    r.r[0] = T.win;
    r.r[1] = (intptr_t) b;
    ro_swi(Wimp_SetExtent, &r);
    T.extent_w = w;
}

static void redraw_all(void)
{
    _kernel_swi_regs r;
    r.r[0] = T.win; r.r[1] = 0; r.r[2] = -(WIN_H); r.r[3] = T.extent_w; r.r[4] = 0;
    ro_swi(Wimp_ForceRedraw, &r);
}

void tl_open(int x0, int y1, int width)
{
    int b[9];
    _kernel_swi_regs r;
    if (!T.win && create() < 0)
        return;
    T.vis_w = width;
    T.vis_h = WIN_H;
    set_extent();
    b[0] = T.win;
    b[1] = x0; b[2] = y1 - T.vis_h; b[3] = x0 + width; b[4] = y1;
    b[5] = 0; b[6] = 0; b[7] = -1;
    r.r[1] = (intptr_t) b;
    ro_swi(Wimp_OpenWindow, &r);
    T.scroll_x = 0;
    redraw_all();
}

void tl_close(void)
{
    _kernel_swi_regs r;
    if (!T.win)
        return;
    r.r[1] = (intptr_t) &T.win;
    ro_swi(Wimp_CloseWindow, &r);
}

void tl_open_request(int *b)
{
    _kernel_swi_regs r;
    T.vis_w = b[3] - b[1];
    T.vis_h = b[4] - b[2];
    T.scroll_x = b[5];
    if (T.extent_w < T.vis_w)
        set_extent();
    r.r[1] = (intptr_t) b;
    ro_swi(Wimp_OpenWindow, &r);
}

void tl_edited(void)
{
    if (!T.win)
        return;
    set_extent();
    /* an audio track more or fewer: the window as tall as its rows (its top kept) */
    int st[9];
    _kernel_swi_regs r;
    st[0] = T.win;
    r.r[1] = (intptr_t) st;
    if (!ro_swi(Wimp_GetWindowState, &r) && (st[8] & (1 << 16)) && st[4] - st[2] != WIN_H && T.vis_h != WIN_H) {
        T.vis_h = WIN_H;
        st[2] = st[4] - WIN_H;
        tl_open_request(st);
    }
    redraw_all();
}

void tl_select(int item)
{
    A.sel = item;
    A.ov_sel = -1;
    A.au_sel = -1;
    redraw_all();
}

void tl_select_overlay(int ov)
{
    A.ov_sel = ov;
    A.sel = -1;
    A.au_sel = -1;
    redraw_all();
}

void tl_select_audio(int track, int i)
{
    A.au_track = track;
    A.au_sel = i;
    A.sel = A.ov_sel = -1;
    redraw_all();
}

/* The audio row at a height (work area), or -1 */
static int audio_row(int wy)
{
    int rows = audio_rows();
    for (int k = 0; k < rows; k++)
        if (wy <= AUDIO_TOP(k) + 4 && wy > AUDIO_BOTTOM(k) - 4)
            return k;
    return -1;
}

int tl_on_audio(int screen_y)
{
    int st[9];
    _kernel_swi_regs r;
    st[0] = T.win;
    r.r[1] = (intptr_t) st;
    if (!T.win || ro_swi(Wimp_GetWindowState, &r))
        return -1;
    return audio_row(screen_y - (st[4] - st[6]));
}

int tl_on_overlays(int screen_y)
{
    int st[9];
    _kernel_swi_regs r;
    st[0] = T.win;
    r.r[1] = (intptr_t) st;
    if (!T.win || ro_swi(Wimp_GetWindowState, &r))
        return 0;
    int wy = screen_y - (st[4] - st[6]);
    return wy <= V2_TOP + 8 && wy > V2_BOTTOM - 4;
}

/* Moves the view sideways (keeping the window where it is) */
static void scroll_to(int sx)
{
    int st[9];
    _kernel_swi_regs r;
    st[0] = T.win;
    r.r[1] = (intptr_t) st;
    if (ro_swi(Wimp_GetWindowState, &r) || !(st[8] & (1 << 16)))
        return;
    if (sx < 0)
        sx = 0;
    st[5] = sx;
    r.r[1] = (intptr_t) st;
    ro_swi(Wimp_OpenWindow, &r);
    r.r[1] = (intptr_t) st;
    if (!ro_swi(Wimp_GetWindowState, &r)) /* (as the Wimp opened it: kept inside the extent) */
        T.scroll_x = st[5];
}

void tl_playhead(int frame)
{
    _kernel_swi_regs r;
    if (!T.win || frame == T.playhead)
        return;
    int old = x_of(T.playhead), now = x_of(frame);
    T.playhead = frame;
    /* (the line, its handle, and the timecode to its right) */
    /* (from a label's width before it: the ruler labels it hides, or uncovers) */
    r.r[0] = T.win; r.r[1] = old - MARK_TEXT_W - 8; r.r[2] = -(WIN_H); r.r[3] = old + LABEL_W + 4;
    r.r[4] = 0;
    ro_swi(Wimp_ForceRedraw, &r);
    r.r[0] = T.win; r.r[1] = now - MARK_TEXT_W - 8; r.r[2] = -(WIN_H); r.r[3] = now + LABEL_W + 4;
    r.r[4] = 0;
    ro_swi(Wimp_ForceRedraw, &r);
    /* kept in view: a quarter of the way in when it goes off either side */
    if (now < T.scroll_x || now > T.scroll_x + T.vis_w - 16)
        scroll_to(now - T.vis_w / 4);
}

void tl_zoom(int in)
{
    double old = T.scale;
    if (in > 0)
        T.scale *= 2;
    else if (in == 0)
        T.scale /= 2;
    else {
        int len = A.proj ? re_project_length(A.proj) : 0;
        T.scale = len > 0 ? (double) (T.vis_w - LEFT - 32) / len : 4.0;
    }
    if (T.scale > 64)
        T.scale = 64;
    if (T.scale < 0.01)
        T.scale = 0.01;
    if (T.scale == old)
        return;
    set_extent();
    scroll_to(in < 0 ? 0 : x_of(T.playhead) - T.vis_w / 2);
    redraw_all();
}

int tl_frame_at(int screen_x)
{
    int st[9];
    _kernel_swi_regs r;
    st[0] = T.win;
    r.r[1] = (intptr_t) st;
    if (ro_swi(Wimp_GetWindowState, &r))
        return -1;
    return frame_of(screen_x - (st[1] - st[5]));
}

/* ---- drawing ---------------------------------------------------------------- */

static void colour(unsigned bbggrr00)
{
    _kernel_swi_regs r;
    r.r[0] = (int) bbggrr00;
    r.r[3] = 0; /* foreground, no dithering */
    r.r[4] = 0;
    ro_swi(ColourTrans_SetGCOL, &r);
}

static void plot(int code, int x, int y)
{
    _kernel_swi_regs r;
    r.r[0] = code; r.r[1] = x; r.r[2] = y;
    ro_swi(OS_Plot, &r);
}

static void rect(int x0, int y0, int x1, int y1) /* inclusive, screen */
{
    plot(4, x0, y0);
    plot(96 + 5, x1, y1);
}

static void triangle(int x0, int y0, int x1, int y1, int x2, int y2) /* filled, screen */
{
    plot(4, x0, y0);
    plot(4, x1, y1);
    plot(80 + 5, x2, y2);
}

static void text(const char *s, int x, int y, unsigned fg, unsigned bg)
{
    _kernel_swi_regs r;
    r.r[0] = 0; r.r[1] = (int) fg; r.r[2] = (int) bg;
    ro_swi(Wimp_TextOp, &r);
    r.r[0] = 2; r.r[1] = (intptr_t) s; r.r[2] = -1; r.r[3] = -1; r.r[4] = x; r.r[5] = y;
    ro_swi(Wimp_TextOp, &r);
}

/* A text as it fits a width (OS units) in the desktop font: cut short with
   an ellipsis (RISC OS's character &8C) if it doesn't */
static int text_width(const char *s)
{
    _kernel_swi_regs r;
    r.r[0] = 1; r.r[1] = (intptr_t) s; r.r[2] = 0;
    return ro_swi(Wimp_TextOp, &r) ? 0 : (int) r.r[0];
}

static void fit_text(char *out, int size, const char *s, int width)
{
    snprintf(out, size, "%s", s);
    int n = (int) strlen(out);
    if (n <= 1 || text_width(out) <= width)
        return;
    /* the most characters (then the ellipsis) that fit: found by halving,
       a few Wimp_TextOps rather than one for each character taken off (a
       long name in a short clip, each time the playhead passed) */
    char full[256];
    snprintf(full, sizeof(full), "%s", out);
    int lo = 0, hi = n - 1; /* (lo characters fit; hi + 1 don't) */
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        memcpy(out, full, (size_t) mid);
        out[mid] = (char) 0x8C;
        out[mid + 1] = 0;
        if (text_width(out) <= width)
            lo = mid;
        else
            hi = mid - 1;
    }
    memcpy(out, full, (size_t) lo);
    out[lo] = (char) 0x8C;
    out[lo + 1] = 0; /* (none fit: the ellipsis alone, as before) */
}

static void clip(int x0, int y0, int x1, int y1) /* inclusive */
{
    unsigned char b[9] = {24, x0 & 255, (x0 >> 8) & 255, y0 & 255, (y0 >> 8) & 255,
                          x1 & 255, (x1 >> 8) & 255, y1 & 255, (y1 >> 8) & 255};
    _kernel_swi_regs r;
    r.r[0] = (intptr_t) b;
    r.r[1] = 9;
    ro_swi(OS_WriteN, &r);
}

/* The ruler's marks: a step in frames giving marks far enough apart for
   their labels (Pi test: a full timecode a second ran together) */
#define MARK_GAP 180
static int ruler_step(double fps)
{
    static const double seconds[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200};
    int f1 = (int) (fps + 0.5);
    if (T.scale * 1 >= MARK_GAP)
        return 1;
    if (T.scale * 5 >= MARK_GAP && f1 > 5)
        return 5;
    for (unsigned i = 0; i < sizeof(seconds) / sizeof(seconds[0]); i++)
        if (T.scale * seconds[i] * f1 >= MARK_GAP)
            return (int) (seconds[i] * f1);
    return 7200 * f1;
}

/* A mark's label: m:ss (h:mm:ss over an hour); within a second, m:ss.ff */
static void mark_label(char *buf, int size, int f, double fps)
{
    int f1 = (int) (fps + 0.5);
    if (f1 < 1)
        f1 = 1;
    int s = f / f1;
    if (f % f1)
        snprintf(buf, size, "%d:%02d.%02d", s / 60, s % 60, f % f1);
    else if (s >= 3600)
        snprintf(buf, size, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else
        snprintf(buf, size, "%d:%02d", s / 60, s % 60);
}

static int fade_handle_x(int end, const re_item *it);

/* A clip's fades: each a box of its own over the frames it covers, along
   the top of the clip, its name in it if there's room, its inner edge
   white (drag it: longer, shorter) */
static void draw_fades(const re_item *it, int ox, int x0, int x1, int y1, int cx0, int cy0, int cx1, int cy1)
{
    for (int end = 0; end < 2; end++) {
        int length = end ? it->fade_out : it->fade_in;
        if (length <= 0)
            continue;
        int edge = ox + fade_handle_x(end, it), b0 = end ? edge : x0, b1 = end ? x1 : edge;
        if (b1 - b0 < 4)
            b1 = b0 + 4;
        colour(EFFECT_COLOUR);
        rect(b0, y1 - CORNER, b1, y1);
        colour(0xFFFFFF00);
        rect(end ? b0 : b1 - 3, y1 - CORNER, end ? b0 + 3 : b1, y1);
        if (b1 - b0 > 120) {
            int c0 = b0 > cx0 ? b0 : cx0, c1 = b1 < cx1 ? b1 : cx1, r0 = y1 - CORNER > cy0 ? y1 - CORNER : cy0;
            int r1 = y1 < cy1 ? y1 : cy1;
            if (c1 > c0 && r1 > r0) {
                clip(c0, r0, c1, r1);
                text(end ? "Fade out" : "Fade in", b0 + 10, y1 - CORNER + 6, 0xFFFFFF00, EFFECT_COLOUR);
                clip(cx0, cy0, cx1, cy1);
            }
        }
    }
}

/* A clip's sound, along its bottom (x0..x1, from y0, h high): muted, a red
   line; its fades, a green wedge each (silence to full, or back) */
static void draw_sound(int start, int length, int fade_in, int fade_out, int muted, int ox, int x0, int x1, int y0,
                       int h)
{
    if (muted) {
        colour(MUTED_COLOUR);
        rect(x0, y0, x1, y0 + 8);
        return; /* (no sound to fade) */
    }
    colour(SOUND_COLOUR);
    if (fade_in > 0) {
        int e = ox + x_of(start + fade_in);
        triangle(x0, y0, e, y0, e, y0 + h);
    }
    if (fade_out > 0) {
        int e = ox + x_of(start + length - fade_out);
        triangle(e, y0 + h, e, y0, x1, y0);
    }
}

/* A clip's name, " (muted)" after it if it is, or its sound's level if
   it's been changed (" (-6 dB)"); its speed if it isn't 100% (" 200%",
   " reversed", " -50%") */
static const char *label(char *buf, int size, const char *name, int muted, int volume, int speed)
{
    char sp[24] = "";
    if (speed == -100)
        snprintf(sp, sizeof(sp), " reversed");
    else if (speed != 100)
        snprintf(sp, sizeof(sp), " %d%%", speed);
    if (muted)
        snprintf(buf, size, "%s%s (muted)", name, sp);
    else if (volume)
        snprintf(buf, size, "%s%s (%+d dB)", name, sp, volume);
    else
        snprintf(buf, size, "%s%s", name, sp);
    return buf;
}

/* A dissolve: a box laid over the two clips where they overlap (the clips
   are drawn on through it), the first clip's colour giving way to a
   lighter one along a diagonal, a white (selected: amber) border; its name
   if there's room */
#define MIX_INSET 12 /* OS units: the clips show above and below the box */
static void draw_dissolve(const re_item *it, int selected, int x0, int x1, int y0, int y1, int cx0, int cy0, int cx1,
                          int cy1)
{
    y0 += MIX_INSET;
    y1 -= MIX_INSET;
    colour(selected ? 0x2080F000 : 0xFFFFFF00);
    rect(x0, y0, x1, y1);
    int inset = selected ? 6 : 3, a0 = x0 + inset, a1 = x1 - inset, b0 = y0 + inset, b1 = y1 - inset;
    if (a1 <= a0)
        return;
    colour(EFFECT_DARK); /* (the one clip giving way to the other along the diagonal) */
    triangle(a0, b0, a1, b0, a0, b1);
    colour(EFFECT_COLOUR);
    triangle(a1, b1, a0, b1, a1, b0);
    if (selected && x1 - x0 > 4 * inset) { /* its ends' handles: drag to lengthen or shorten */
        colour(0x2080F000);
        rect(x0, y0 + CORNER, x0 + inset + 4, y1 - CORNER);
        rect(x1 - inset - 4, y0 + CORNER, x1, y1 - CORNER);
    }
    if (a1 - a0 > 140) {
        int c0 = a0 > cx0 ? a0 : cx0, c1 = a1 < cx1 ? a1 : cx1, r0 = b0 > cy0 ? b0 : cy0, r1 = b1 < cy1 ? b1 : cy1;
        if (c1 > c0 && r1 > r0) {
            clip(c0, r0, c1, r1);
            text(it->name, a0 + 8, y0 + 12, 0xFFFFFF00, EFFECT_DARK);
            clip(cx0, cy0, cx1, cy1);
        }
    }
}

/* V2's overlays: a box each over its frames, its fades purple along its
   top, its name in it (a picture teal, a video blue); selected, an amber outline and handles at its
   ends. With none, a word on what the row is for. */
static void draw_overlays(int ox, int y0, int y1, int f0, int f1, int cx0, int cy0, int cx1, int cy1)
{
    int n = A.proj ? re_project_overlays(A.proj) : 0;
    if (n == 0) {
        if (A.proj && re_project_length(A.proj) > 0)
            text("V2: drop a picture (PNG, JPEG, sprite) or a video here to put it over the video", ox + LEFT + 8, y0 + 20,
                 0xCCCCCC00, 0x77777700);
        return;
    }
    for (int i = 0; i < n; i++) {
        re_overlay o;
        if (re_project_overlay(A.proj, i, &o) || o.start + o.length < f0 || o.start > f1)
            continue;
        int x0 = ox + x_of(o.start), x1 = ox + x_of(o.start + o.length) - 2, sel = i == A.ov_sel;
        if (x1 < x0)
            x1 = x0;
        colour(sel ? 0x2080F000 : 0x20202000);
        rect(x0, y0, x1, y1);
        int inset = sel ? 6 : 2;
        unsigned fill = o.video ? CLIP_COLOUR : OVERLAY_COLOUR; /* (a video blue, as V1's) */
        colour(fill);
        rect(x0 + inset, y0 + inset, x1 - inset, y1 - inset);
        colour(EFFECT_COLOUR); /* (its fades) */
        if (o.fade_in > 0)
            rect(x0 + inset, y1 - inset - 12, ox + x_of(o.start + o.fade_in), y1 - inset);
        if (o.fade_out > 0)
            rect(ox + x_of(o.start + o.length - o.fade_out), y1 - inset - 12, x1 - inset, y1 - inset);
        if (o.video)
            draw_sound(o.start, o.length, o.sound_fade_in, o.sound_fade_out, o.muted, ox, x0 + inset, x1 - inset,
                       y0 + inset, 9);
        if (sel && x1 - x0 > 4 * inset) {
            colour(0x2080F000);
            rect(x0, y0 + 12, x0 + inset + 4, y1 - 12);
            rect(x1 - inset - 4, y0 + 12, x1, y1 - 12);
        }
        int c0 = x0 + 2 > cx0 ? x0 + 2 : cx0, c1 = x1 - 2 < cx1 ? x1 - 2 : cx1;
        int r0 = y0 > cy0 ? y0 : cy0, r1 = y1 < cy1 ? y1 : cy1;
        if (c1 > c0 && r1 > r0) {
            char shown[64];
            clip(c0, r0, c1, r1);
            char name[80];
            fit_text(shown, sizeof(shown), label(name, sizeof(name), o.name, o.muted, o.volume, 100), x1 - x0 - 24);
            text(shown, x0 + 10, y0 + 12, 0xFFFFFF00, fill);
            clip(cx0, cy0, cx1, cy1);
        }
    }
}

/* The audio tracks' sounds: a green box each over its frames, its name
   (muted or its level after it), its fades green wedges; selected, an
   amber outline and handles at its ends. Below them, an empty row to drop
   on, with a word on what it's for. */
static void draw_audio(int ox, int oy, int f0, int f1, int cx0, int cy0, int cx1, int cy1)
{
    int rows = audio_rows();
    for (int k = 0; k < rows; k++) {
        int y1 = oy + AUDIO_TOP(k), y0 = oy + AUDIO_BOTTOM(k);
        int n = A.proj && k < rows - 1 ? re_project_audio_count(A.proj, k) : 0;
        if (k == rows - 1) {
            if (A.proj && re_project_length(A.proj) > 0)
                text(rows > 1 ? "Drop more sound here (or where a sound plays: it goes below)"
                              : "Audio: drop music or sound effects here (sounds dropped where one plays stack)",
                     ox + LEFT + 8, y0 + 12, 0xCCCCCC00, 0x77777700);
            continue;
        }
        for (int i = 0; i < n; i++) {
            re_audio a;
            if (re_project_audio(A.proj, k, i, &a) || a.start + a.length < f0 || a.start > f1)
                continue;
            int x0 = ox + x_of(a.start), x1 = ox + x_of(a.start + a.length) - 2;
            int sel = k == A.au_track && i == A.au_sel;
            if (x1 < x0)
                x1 = x0;
            colour(sel ? 0x2080F000 : 0x20202000);
            rect(x0, y0, x1, y1);
            int inset = sel ? 6 : 2;
            colour(AUDIO_COLOUR);
            rect(x0 + inset, y0 + inset, x1 - inset, y1 - inset);
            pv_sound ps = {a.in, a.length, a.fade_in, a.fade_out, a.volume, a.muted, 100, a.source_length};
            ro_box wb = {x0 + inset > cx0 ? x0 + inset : cx0, cy0, x1 - inset < cx1 ? x1 - inset : cx1, cy1};
            if (wb.x1 >= wb.x0)
                pv_wave(re_project_audio_file(A.proj, k, i), &ps, ox + x_of(a.start), T.scale, y0 + inset,
                        y1 - y0 - 2 * inset, AUDIO_WAVE, &wb);
            draw_sound(a.start, a.length, a.fade_in, a.fade_out, a.muted, ox, x0 + inset, x1 - inset, y0 + inset, 12);
            if (sel && x1 - x0 > 4 * inset) {
                colour(0x2080F000);
                rect(x0, y0 + 12, x0 + inset + 4, y1 - 12);
                rect(x1 - inset - 4, y0 + 12, x1, y1 - 12);
            }
            int c0 = x0 + 2 > cx0 ? x0 + 2 : cx0, c1 = x1 - 2 < cx1 ? x1 - 2 : cx1;
            int r0 = y0 > cy0 ? y0 : cy0, r1 = y1 < cy1 ? y1 : cy1;
            if (c1 > c0 && r1 > r0) {
                char shown[64], name[80];
                clip(c0, r0, c1, r1);
                fit_text(shown, sizeof(shown), label(name, sizeof(name), a.name, a.muted, a.volume, 100), x1 - x0 - 24);
                text(shown, x0 + 10, y0 + 14, 0xFFFFFF00, AUDIO_COLOUR);
                clip(cx0, cy0, cx1, cy1);
            }
        }
    }
}

/* The first item a redraw from frame f0 needs (the one there, and the one
   before it: a dissolve's clips run on under it), so a redraw of a strip
   (the playhead's, as it plays) looks at the items in it, not all of them */
static int first_item(int f0)
{
    int len = re_project_length(A.proj);
    if (f0 >= len)
        return re_project_items(A.proj) > 0 ? re_project_items(A.proj) - 1 : 0;
    int i = re_project_item_at(A.proj, f0 < 0 ? 0 : f0);
    return i > 0 ? i - 1 : 0;
}

static void draw(int *b)
{
    int ox = b[1] - b[5], oy = b[4] - b[6]; /* screen position of work area 0,0 */
    int cx0 = b[7], cy0 = b[8], cx1 = b[9] - 1, cy1 = b[10] - 1;
    int f0 = frame_of(cx0 - ox) - 1, f1 = frame_of(cx1 - ox) + 1;
    int ruler_y0 = oy - RULER_H, track_y1 = oy + TRACK_TOP, track_y0 = oy + TRACK_BOTTOM;
    int v2_y1 = oy + V2_TOP, v2_y0 = oy + V2_BOTTOM;
    double fps = A.proj ? re_project_fps(A.proj) : 25;

    /* the ruler, the marked stretch on it */
    colour(0xDDDDDD00);
    rect(cx0, ruler_y0, cx1, oy);
    int m0 = -1, m1 = -1; /* (the marked stretch's x: from the in mark, or the start, to the out mark, or the end) */
    if (A.proj && (A.mark_in >= 0 || A.mark_out >= 0)) {
        m0 = ox + x_of(A.mark_in >= 0 ? A.mark_in : 0);
        m1 = ox + x_of(A.mark_out >= 0 ? A.mark_out + 1 : re_project_length(A.proj));
        colour(MARKED_COLOUR);
        rect(m0, ruler_y0, m1, oy);
    }
    if (A.proj) {
        int step = ruler_step(fps), len = re_project_length(A.proj);
        colour(0x55555500);
        for (int f = (f0 < 0 ? 0 : f0) / step * step; f <= f1 && f <= len; f += step) {
            int x = ox + x_of(f);
            rect(x, ruler_y0, x + 1, ruler_y0 + 16);
            /* (a label the playhead's timecode would cover isn't drawn: half
               hidden, it read as part of the timecode) */
            int phx = ox + x_of(T.playhead);
            if (len > 0 && x + 6 < phx + LABEL_W && x + 6 + MARK_TEXT_W > phx - 2)
                continue;
            char tc[40];
            mark_label(tc, sizeof(tc), f, fps);
            text(tc, x + 6, ruler_y0 + 20, 0x00000000, 0xDDDDDD00);
        }
    }
    /* render previews (renders.c): under the ruler, each costly stretch red
       (to render), green (rendered) or amber (being rendered, as far as it's made) */
    int rs, rl, rm, st;
    for (int i = 0; A.proj && (st = rr_stretches(i, &rs, &rl, &rm)) >= 0; i++) {
        if (rs > f1 || rs + rl < f0)
            continue;
        int x0 = ox + x_of(rs), x1 = ox + x_of(rs + rl) - 1;
        colour(st == 1 ? RENDERED_COLOUR : RENDER_COLOUR);
        rect(x0, ruler_y0, x1, ruler_y0 + RENDER_H - 1);
        if (st == 2 && rm > 0) {
            colour(0x2080F000);
            rect(x0, ruler_y0, ox + x_of(rs + (rm < rl ? rm : rl)) - 1, ruler_y0 + RENDER_H - 1);
        }
    }
    /* the tracks */
    colour(0x77777700);
    rect(cx0, oy + AUDIO_BOTTOM(audio_rows() - 1) - 8, cx1, v2_y1 + 8 - 1);
    draw_audio(ox, oy, f0, f1, cx0, cy0, cx1, cy1);
    draw_overlays(ox, v2_y0, v2_y1, f0, f1, cx0, cy0, cx1, cy1);
    if (A.proj) {
        for (int i = first_item(f0); i < re_project_items(A.proj); i++) {
            re_item it;
            re_project_item(A.proj, i, &it);
            if (it.start > f1)
                break;
            if (it.gap || it.start + it.length < f0)
                continue;
            int x0 = ox + x_of(it.start), x1 = ox + x_of(it.start + it.length) - 2;
            if (x1 < x0)
                x1 = x0;
            if (it.mix)
                continue; /* (laid over the clips afterwards) */
            /* a clip runs on under a dissolve beside it: the frames it gives */
            int name_x = x0 + 10;
            re_item n;
            if (i > 0 && !re_project_item(A.proj, i - 1, &n) && n.mix)
                x0 = ox + x_of(n.start);
            if (!re_project_item(A.proj, i + 1, &n) && n.mix)
                x1 = ox + x_of(n.start + n.length) - 2;
            /* selected: the clip's own colour inside a bright outline (a fill
               hid its name, Pi test) */
            unsigned fill = CLIP_COLOUR;
            colour(i == A.sel ? 0x2080F000 : 0x20202000); /* (BBGGRR: amber) */
            rect(x0, track_y0, x1, track_y1);
            int inset = i == A.sel ? 6 : 2;
            colour(fill);
            rect(x0 + inset, track_y0 + inset, x1 - inset, track_y1 - inset);
            if (i == A.sel && x1 - x0 > 4 * inset) { /* its ends' handles: drag to trim */
                colour(0x2080F000);
                rect(x0, track_y0 + CORNER, x0 + inset + 4, track_y1 - CORNER);
                rect(x1 - inset - 4, track_y0 + CORNER, x1, track_y1 - CORNER);
            }
            /* its waveform along the bottom; its first frame, and its last
               if there's room, between that and its fades (previews.c) */
            const char *file = re_project_item_file(A.proj, i);
            ro_box in_box = {x0 + inset > cx0 ? x0 + inset : cx0, track_y0 + inset > cy0 ? track_y0 + inset : cy0,
                             x1 - inset < cx1 ? x1 - inset : cx1, track_y1 - inset < cy1 ? track_y1 - inset : cy1};
            int name_end = x1;
            if (in_box.x1 >= in_box.x0 && in_box.y1 >= in_box.y0) {
                if (it.sound) {
                    pv_sound ps = {it.in,  it.length,    it.sound_fade_in, it.sound_fade_out,
                                   it.volume, it.muted, it.speed,      it.source_length};
                    pv_wave(file, &ps, ox + x_of(it.start), T.scale, track_y0 + inset, WAVE_H - inset, WAVE_COLOUR,
                            &in_box);
                }
                int head_x = ox + x_of(it.start) + inset, tw = pv_thumb_width(THUMB_H);
                int tail_x = ox + x_of(it.start + it.length) - 2 - inset - tw;
                if (tw > 0 && x1 - x0 >= tw + 2 * inset) {
                    int w = pv_thumb(file, re_item_file_frame(&it, it.in), head_x, track_y1 - CORNER, THUMB_H, &in_box);
                    if (w > 0 && name_x < head_x + w + 10)
                        name_x = head_x + w + 10;
                    if (tail_x >= head_x + 2 * tw + 160 && pv_thumb(file, re_item_file_frame(&it, it.out), tail_x, track_y1 - CORNER, THUMB_H,
                                                                    &in_box) > 0)
                        name_end = tail_x - 4;
                }
            }
            draw_fades(&it, ox, x0, x1, track_y1, cx0, cy0, cx1, cy1);
            draw_sound(it.start, it.length, it.sound_fade_in, it.sound_fade_out, it.muted, ox, x0 + inset, x1 - inset,
                       track_y0 + inset, 24);
            /* the name, clipped to the clip */
            int c0 = x0 + 2 > cx0 ? x0 + 2 : cx0, c1 = x1 - 2 < cx1 ? x1 - 2 : cx1;
            int r0 = track_y0 > cy0 ? track_y0 : cy0, r1 = track_y1 < cy1 ? track_y1 : cy1;
            if (c1 > c0 && r1 > r0) {
                clip(c0, r0, c1, r1);
                /* the name at the start, and again along a long clip (so
                   it's in view however it's scrolled) */
                for (int nx = name_x; nx == name_x || nx < name_end - 200; nx += 1000)
                    if (nx < c1 && nx + 1000 > c0) {
                        char shown[64];
                        char name[80];
                        fit_text(shown, sizeof(shown), label(name, sizeof(name), it.name, it.muted, it.volume, it.speed),
                                 (name_end - nx < 1000 ? name_end - nx : 1000) - 12);
                        text(shown, nx, track_y1 - CORNER - 28, 0xFFFFFF00, fill); /* (below the fade handles) */
                    }
                clip(cx0, cy0, cx1, cy1);
            }
        }
    }
    /* the dissolves, over the clips */
    for (int i = A.proj ? first_item(f0) : 0; A.proj && i < re_project_items(A.proj); i++) {
        re_item it;
        re_project_item(A.proj, i, &it);
        if (it.start > f1)
            break;
        if (!it.mix || it.start + it.length < f0)
            continue;
        int x0 = ox + x_of(it.start), x1 = ox + x_of(it.start + it.length) - 2;
        draw_dissolve(&it, i == A.sel, x0, x1 < x0 ? x0 : x1, track_y0, track_y1, cx0, cy0, cx1, cy1);
    }
    /* the cuts' marks at the track's bottom: a drag there makes a dissolve */
    if (A.proj && T.drag == DRAG_NONE) {
        for (int i = first_item(f0); i + 1 < re_project_items(A.proj); i++) {
            re_item a, b;
            re_project_item(A.proj, i, &a);
            re_project_item(A.proj, i + 1, &b);
            if (b.start > f1)
                break;
            if (a.gap || a.mix || b.gap || b.mix || b.start < f0)
                continue;
            int cx = ox + x_of(b.start) - 1;
            colour(0xFFFFFF00);
            triangle(cx - 12, track_y0, cx + 12, track_y0, cx, track_y0 + 16);
        }
    }
    /* the marks: lines down through the track, with a bracket on the ruler */
    if (m0 >= 0) {
        colour(MARK_COLOUR);
        if (A.mark_in >= 0) {
            rect(m0, track_y0 - 8, m0 + 3, oy);
            rect(m0, oy - 6, m0 + 16, oy);
        }
        if (A.mark_out >= 0) {
            rect(m1 - 3, track_y0 - 8, m1, oy);
            rect(m1 - 16, oy - 6, m1, oy);
        }
    }
    /* moving a clip: where it would go */
    if (T.drag == DRAG_MOVE && T.d_line >= 0) {
        int lx = ox + x_of(T.d_line);
        colour(0xFFFFFF00);
        rect(lx - 3, track_y0 - 8, lx + 3, track_y1 + 8);
        colour(0x2080F000);
        rect(lx - 1, track_y0 - 8, lx + 1, track_y1 + 8);
    }
    /* the playhead: a line, a handle on the ruler, and its timecode beside it */
    colour(0x0000E000);
    int px = ox + x_of(T.playhead);
    rect(px - 1, track_y0 - 8, px + 1, oy);
    rect(px - 6, oy - 12, px + 6, oy);
    if (A.proj && re_project_length(A.proj) > 0) {
        char tc[24];
        app_timecode(tc, sizeof(tc), T.playhead);
        colour(0x0000E000);
        rect(px + 2, oy - 44, px + LABEL_W, oy - 14);
        text(tc, px + 8, oy - 40, 0xFFFFFF00, 0x0000E000);
    }
}

void tl_redraw(int *b)
{
    _kernel_swi_regs r;
    r.r[1] = (intptr_t) b;
    int more = ro_swi(Wimp_RedrawWindow, &r) ? 0 : (int) r.r[0];
    pv_redraw_begin();
    while (more) {
        draw(b);
        r.r[1] = (intptr_t) b;
        more = ro_swi(Wimp_GetRectangle, &r) ? 0 : (int) r.r[0];
    }
}

/* ---- dragging on the track ------------------------------------------------------ */

static void redraw_frames(int f0, int f1) /* (the track between two frames, a little either side) */
{
    _kernel_swi_regs r;
    int x0 = x_of(f0 < f1 ? f0 : f1) - 8, x1 = x_of(f0 < f1 ? f1 : f0) + 8;
    r.r[0] = T.win; r.r[1] = x0; r.r[2] = -(WIN_H); r.r[3] = x1; r.r[4] = -RULER_H;
    ro_swi(Wimp_ForceRedraw, &r);
}

/* A clip's end with a fade handle: where it doesn't meet another clip or
   a dissolve (the project's start or end, a gap), or where it has a fade */
static int fade_handle(int item, int end, const re_item *it)
{
    (void) item;
    return !it->gap && !it->mix && (end ? it->fade_out : it->fade_in) > 0;
}

/* A fade's box's inner edge (work area) */
static int fade_handle_x(int end, const re_item *it)
{
    return end ? x_of(it->start + it->length - it->fade_out) : x_of(it->start + it->fade_in);
}

/* The fade box's inner edge nearest x, within EDGE: its item and end */
static int fade_handle_at(int wx, int *item, int *end)
{
    int best = EDGE + 1;
    for (int i = 0; i < re_project_items(A.proj); i++) {
        re_item it;
        if (re_project_item(A.proj, i, &it))
            break;
        for (int e = 0; e < 2; e++) {
            int d = abs(wx - fade_handle_x(e, &it));
            if (fade_handle(i, e, &it) && d < best) {
                best = d;
                *item = i;
                *end = e;
            }
        }
    }
    return best <= EDGE;
}

/* What a drag at a work area x, y on the track would do (DRAG_NONE:
   nothing), and the item it acts on:
   - the mark at the bottom of a cut between two clips: a dissolve made
     there (DRAG_DISSOLVE; the clip before the cut);
   - a fade's box (along the top of a clip), near its inner edge: the fade
     made longer or shorter (DRAG_FADE_IN, _OUT);
   - a dissolve's ends: it made longer or shorter (DRAG_MIX_IN, _OUT); a
     clip's end in a dissolve is the dissolve's;
   - a clip's ends: trimmed (DRAG_IN, DRAG_OUT); its middle: moved.
   The item is the one under the pointer (at a cut: the one after it); just
   past the end, the last one's end. */
static int drag_mode_at(int wx, int wy, int *item)
{
    int len = re_project_length(A.proj);
    int fi = (int) ((wx - LEFT) / T.scale); /* (the frame the pointer is over, not the nearest edge) */
    int i;
    if (wy < TRACK_BOTTOM + CORNER) {
        int c = re_project_dissolve_cut(A.proj, frame_of(wx));
        re_item after;
        if (c >= 0 && !re_project_item(A.proj, c + 1, &after) && abs(wx - x_of(after.start)) <= EDGE) {
            *item = c;
            return DRAG_DISSOLVE;
        }
    }
    if (wy > TRACK_TOP - CORNER) {
        int end;
        if (fade_handle_at(wx, item, &end))
            return end ? DRAG_FADE_OUT : DRAG_FADE_IN;
    }
    if (fi >= len) {
        if (len <= 0 || wx - x_of(len) > EDGE)
            return DRAG_NONE;
        i = re_project_items(A.proj) - 1;
    } else {
        i = re_project_item_at(A.proj, fi < 0 ? 0 : fi);
    }
    re_item it, next;
    if (i < 0 || re_project_item(A.proj, i, &it) || it.gap)
        return DRAG_NONE;
    int x0 = x_of(it.start), x1 = x_of(it.start + it.length), edge = (x1 - x0) / 3;
    if (edge > EDGE)
        edge = EDGE;
    int side = wx - x0 < edge ? -1 : x1 - wx <= edge ? 1 : 0;
    *item = i;
    if (it.mix)
        return side < 0 ? DRAG_MIX_IN : side > 0 ? DRAG_MIX_OUT : DRAG_NONE;
    if (side < 0 && i > 0 && !re_project_item(A.proj, i - 1, &next) && next.mix) {
        *item = i - 1;
        return DRAG_MIX_OUT;
    }
    if (side > 0 && !re_project_item(A.proj, i + 1, &next) && next.mix) {
        *item = i + 1;
        return DRAG_MIX_IN;
    }
    return side < 0 ? DRAG_IN : side > 0 ? DRAG_OUT : DRAG_MOVE;
}

static const char *const drag_names[] = {"", "scrubbing", "trimming the start of", "trimming the end of", "moving",
                                         "making a dissolve after", "dragging the start of", "dragging the end of",
                                         "fading in", "fading out", "moving overlay", "trimming the start of overlay",
                                         "trimming the end of overlay", "moving sound", "trimming the start of sound",
                                         "trimming the end of sound", "rolling the cut after"};

static int ctrl_down(void)
{
    _kernel_swi_regs r;
    r.r[0] = 121; r.r[1] = 1 ^ 0x80; /* OS_Byte 121: Ctrl down? */
    return !ro_swi(OS_Byte, &r) && (r.r[1] & 0xFF) == 0xFF;
}

/* While dragging, the Viewer shows a frame (the edge trimmed, a fade's or
   a dissolve's middle) once the pointer rests: a picture decoded at each
   step held the desktop up (Pi, 1080p H.264), so the Timeline lagged the
   pointer */
#define REST_CS 15
static void show_later(int frame)
{
    T.d_show = frame;
}

static void show_dissolve(int mix)
{
    re_item it;
    if (mix >= 0 && !re_project_item(A.proj, mix, &it))
        show_later(it.start + it.length / 2);
}

/* The pointer kept to the window's visible part while dragging. 0, or -1. */
static int drag_box(int *b, int *st)
{
    int d[10];
    _kernel_swi_regs r;
    d[0] = T.win;
    d[1] = 7; /* a point, nothing drawn */
    d[2] = d[4] = b[0];
    d[3] = d[5] = b[1];
    d[6] = st[1]; d[7] = st[2]; d[8] = st[3] - 1; d[9] = st[4] - 1;
    r.r[1] = (intptr_t) d;
    return ro_swi(Wimp_DragBox, &r) ? -1 : 0;
}

/* A drag begun on V2: an overlay's middle moves it, its ends trim it */
static void overlay_drag_start(int f, int wx, int *b, int *st)
{
    int i = re_project_overlay_at(A.proj, (int) ((wx - LEFT) / T.scale));
    re_overlay o;
    if (i < 0 || re_project_overlay(A.proj, i, &o)) {
        app_log("timeline: a drag at %d on V2, on no overlay", wx);
        return;
    }
    int x0 = x_of(o.start), x1 = x_of(o.start + o.length), edge = (x1 - x0) / 3;
    if (edge > EDGE)
        edge = EDGE;
    int mode = wx - x0 < edge ? DRAG_OV_IN : x1 - wx <= edge ? DRAG_OV_OUT : DRAG_OV_MOVE;
    app_set_speed(0);
    T.d_item = i;
    T.d_start = o.start;
    T.d_len = o.length;
    T.d_f0 = f;
    T.d_show = -1;
    T.d_raw = f;
    T.d_moved_cs = ro_now_cs();
    tl_select_overlay(i);
    if (drag_box(b, st))
        return;
    T.drag = mode;
    app_log("timeline: %s %d (frames %d-%d), from %d", drag_names[mode], i, o.start, o.start + o.length - 1, wx);
    re_project_begin(A.proj); /* (one undo for the whole drag) */
}

/* A drag begun on an audio track: a sound's middle moves it, its ends trim it */
static void audio_drag_start(int k, int f, int wx, int *b, int *st)
{
    int i = re_project_audio_at(A.proj, k, (int) ((wx - LEFT) / T.scale));
    re_audio a;
    if (i < 0 || re_project_audio(A.proj, k, i, &a)) {
        app_log("timeline: a drag at %d on A%d, on no sound", wx, k + 1);
        return;
    }
    int x0 = x_of(a.start), x1 = x_of(a.start + a.length), edge = (x1 - x0) / 3;
    if (edge > EDGE)
        edge = EDGE;
    int mode = wx - x0 < edge ? DRAG_AU_IN : x1 - wx <= edge ? DRAG_AU_OUT : DRAG_AU_MOVE;
    app_set_speed(0);
    T.d_track = k;
    T.d_item = i;
    T.d_start = a.start;
    T.d_len = a.length;
    T.d_f0 = f;
    T.d_show = -1;
    T.d_raw = f;
    T.d_moved_cs = ro_now_cs();
    tl_select_audio(k, i);
    if (drag_box(b, st))
        return;
    T.drag = mode;
    app_log("timeline: %s %d on A%d (frames %d-%d), from %d", drag_names[mode], i, k + 1, a.start,
            a.start + a.length - 1, wx);
    re_project_begin(A.proj); /* (one undo for the whole drag) */
}

/* A drag begun on the track: see drag_mode_at */
static void drag_start(int f, int wx, int wy, int *b, int *st)
{
    int i = -1, mode = drag_mode_at(wx, wy, &i);
    re_item it, other;
    if (mode == DRAG_NONE || re_project_item(A.proj, i, &it)) {
        app_log("timeline: a drag at %d,%d, on no clip", wx, wy);
        return;
    }
    /* with Ctrl, a clip's edge: the cut there rolled (a clip or a gap the
       other side; not a dissolve) */
    if ((mode == DRAG_IN || mode == DRAG_OUT) && ctrl_down()) {
        int before = mode == DRAG_OUT ? i : i - 1;
        if (before >= 0 && !re_project_item(A.proj, before + 1, &other) && !other.mix
            && !re_project_item(A.proj, before, &it) && !it.mix) {
            mode = DRAG_ROLL;
            i = before;
        }
    }
    app_set_speed(0);
    T.d_item = i;
    T.d_in = it.in;
    T.d_out = it.out;
    T.d_start = it.start;
    T.d_f0 = f;
    T.d_before = -1;
    T.d_line = -1;
    T.d_mix = mode == DRAG_MIX_IN || mode == DRAG_MIX_OUT ? i : -1;
    T.d_len = mode == DRAG_FADE_IN ? it.fade_in : mode == DRAG_FADE_OUT ? it.fade_out : it.length;
    T.d_show = -1;
    T.d_raw = f;
    T.d_moved_cs = ro_now_cs();
    if (mode != DRAG_DISSOLVE)
        tl_select(i);
    if (drag_box(b, st))
        return;
    T.drag = mode;
    app_log("timeline: %s item %d (frames %d-%d), from %d,%d", drag_names[mode], i, it.start,
            it.start + it.length - 1, wx, wy);
    if (mode == DRAG_MOVE)
        return;
    re_project_begin(A.proj); /* (one undo for the whole drag) */
    if (mode == DRAG_IN || mode == DRAG_OUT || mode == DRAG_ROLL)
        app_seek(mode == DRAG_IN ? it.start : it.start + it.length - 1);
    else if (mode == DRAG_MIX_IN || mode == DRAG_MIX_OUT)
        show_dissolve(i);
    if (mode == DRAG_DISSOLVE)
        redraw_all(); /* (the corners go while dragging) */
}

/* Where a moved clip would go: the cut (an item's start, or the end)
   nearest the frame */
static void move_target(int f, int *before, int *at)
{
    int n = re_project_items(A.proj), best = 0, bd = -1;
    for (int k = 0; k <= n; k++) {
        re_item it;
        int c = k < n ? (re_project_item(A.proj, k, &it), it.start) : re_project_length(A.proj);
        int d = c > f ? c - f : f - c;
        if (bd < 0 || d < bd) {
            bd = d;
            best = k;
            *at = c;
        }
    }
    *before = best;
}

static void drag_tick(int f)
{
    /* (trimming isn't snapped: the cuts after the clip move with it, and the
       playhead shows the edge) */
    int delta = f - T.d_f0;
    re_item it;
    switch (T.drag) {
    case DRAG_IN:
        if (re_project_set_in_out(A.proj, T.d_item, T.d_in + delta, T.d_out)) {
            app_edited_dragging();
            show_later(T.d_start); /* (the clip's first frame now) */
        }
        break;
    case DRAG_OUT:
        if (re_project_set_in_out(A.proj, T.d_item, T.d_in, T.d_out + delta)) {
            app_edited_dragging();
            if (!re_project_item(A.proj, T.d_item, &it))
                show_later(it.start + it.length - 1); /* (its last frame now) */
        }
        break;
    case DRAG_ROLL: /* (the cut was at the clip's end; the Viewer shows the frame before it) */
        if (re_project_roll(A.proj, T.d_item, T.d_start + T.d_len + delta)) {
            app_edited_dragging();
            if (!re_project_item(A.proj, T.d_item, &it))
                show_later(it.start + it.length - 1);
        }
        break;
    case DRAG_DISSOLVE: { /* (as long as the pointer has come from the cut, either way) */
        int length = delta < 0 ? -delta : delta;
        if (length < 1)
            break;
        if (T.d_mix < 0) {
            T.d_mix = re_project_dissolve(A.proj, T.d_item, length, 1);
            if (T.d_mix >= 0)
                A.sel = T.d_mix;
        } else if (!re_project_dissolve_length(A.proj, T.d_mix, length)) {
            break;
        }
        app_edited_dragging();
        show_dissolve(T.d_mix);
        break;
    }
    case DRAG_FADE_IN: /* (dragged on, longer; the Viewer shows its middle) */
    case DRAG_FADE_OUT: {
        int in = T.drag == DRAG_FADE_IN, length = T.d_len + (in ? delta : -delta);
        if (re_project_fade(A.proj, T.d_item, !in, length < 0 ? 0 : length)) {
            app_edited_dragging();
            if (!re_project_item(A.proj, T.d_item, &it))
                show_later(in ? it.start + it.fade_in / 2 : it.start + it.length - 1 - it.fade_out / 2);
        }
        break;
    }
    case DRAG_MIX_IN: /* (its start: dragged back, longer) */
    case DRAG_MIX_OUT:
        if (re_project_dissolve_length(A.proj, T.d_mix, T.d_len + (T.drag == DRAG_MIX_IN ? -delta : delta))) {
            app_edited_dragging();
            show_dissolve(T.d_mix);
        }
        break;
    case DRAG_OV_MOVE: /* (kept clear of the others: overlay.c) */
    case DRAG_OV_IN:
    case DRAG_OV_OUT: {
        int start = T.d_start, length = T.d_len, end = T.d_start + T.d_len;
        re_overlay o;
        if (T.drag == DRAG_OV_MOVE) {
            start += delta;
        } else if (T.drag == DRAG_OV_OUT) {
            length += delta;
        } else {
            int lo = T.d_item > 0 && !re_project_overlay(A.proj, T.d_item - 1, &o) ? o.start + o.length : 0;
            start += delta;
            if (start < lo)
                start = lo;
            if (start > end - 1)
                start = end - 1;
            length = end - start; /* (its end stays) */
        }
        if (re_project_overlay_span(A.proj, T.d_item, start < 0 ? 0 : start, length < 1 ? 1 : length) == 1
            && !re_project_overlay(A.proj, T.d_item, &o)) {
            app_edited_dragging();
            show_later(T.drag == DRAG_OV_OUT ? o.start + o.length - 1 : o.start);
        }
        break;
    }
    case DRAG_AU_MOVE: /* (kept clear of the others on its track: track.c) */
    case DRAG_AU_IN:
    case DRAG_AU_OUT: {
        int start = T.d_start, length = T.d_len, end = T.d_start + T.d_len;
        re_audio a;
        if (T.drag == DRAG_AU_MOVE) {
            start += delta;
        } else if (T.drag == DRAG_AU_OUT) {
            length += delta;
        } else {
            int lo = T.d_item > 0 && !re_project_audio(A.proj, T.d_track, T.d_item - 1, &a) ? a.start + a.length : 0;
            start += delta;
            if (start < lo)
                start = lo;
            if (start > end - 1)
                start = end - 1;
            length = end - start;
        }
        if (re_project_audio_span(A.proj, T.d_track, T.d_item, start < 0 ? 0 : start, length < 1 ? 1 : length) == 1
            && !re_project_audio(A.proj, T.d_track, T.d_item, &a)) {
            app_edited_dragging();
            show_later(T.drag == DRAG_AU_OUT ? a.start + a.length - 1 : a.start);
        }
        break;
    }
    case DRAG_MOVE: {
        int before = 0, at = -1;
        move_target(f, &before, &at);
        if (before == T.d_item || before == T.d_item + 1)
            at = -1; /* (where it is: nowhere to show) */
        if (at != T.d_line) {
            if (T.d_line >= 0)
                redraw_frames(T.d_line, T.d_line);
            T.d_line = at;
            T.d_before = before;
            if (at >= 0)
                redraw_frames(at, at);
        }
        break;
    }
    }
}

/* ---- clicks -------------------------------------------------------------------- */

void tl_click(int *b)
{
    int buttons = b[2];
    int st[9];
    _kernel_swi_regs r;
    if (buttons == 2) { /* Menu: over a clip or a dissolve on the track, its context menu (its effects) */
        int item = -1, ov = -1, ak = -1, ai = -1;
        st[0] = T.win;
        r.r[1] = (intptr_t) st;
        if (A.proj && !ro_swi(Wimp_GetWindowState, &r)) {
            int wx = b[0] - (st[1] - st[5]), wy = b[1] - (st[4] - st[6]);
            int f = (int) ((wx - LEFT) / T.scale);
            if (wy <= TRACK_TOP && wy >= TRACK_BOTTOM && f >= 0)
                item = re_project_item_at(A.proj, f);
            else if (wy <= V2_TOP && wy >= V2_BOTTOM && f >= 0)
                ov = re_project_overlay_at(A.proj, f);
            else if ((ak = audio_row(wy)) >= 0 && f >= 0)
                ai = re_project_audio_at(A.proj, ak, f);
        }
        if (ai >= 0) {
            app_audio_menu(b[0], b[1], ak, ai);
            return;
        }
        if (ov >= 0) {
            app_overlay_menu(b[0], b[1], ov);
            return;
        }
        re_item it;
        if (item >= 0 && !re_project_item(A.proj, item, &it) && !it.gap)
            app_context_menu(b[0], b[1], item);
        else
            app_menu(b[0], b[1]);
        return;
    }
    /* single clicks, and drags (on the ruler); a double click's second report ignored */
    if (!(buttons & (0x400 | 0x100 | 0x40 | 0x10)))
        return;
    int adjust = buttons & (0x100 | 0x1);
    st[0] = T.win;
    r.r[1] = (intptr_t) st;
    ro_swi(Wimp_GetWindowState, &r);
    int wx = b[0] - (st[1] - st[5]), wy = b[1] - (st[4] - st[6]);
    /* the caret here, for the keys */
    r.r[0] = T.win; r.r[1] = -1; r.r[2] = 0; r.r[3] = 0; r.r[4] = 1 << 25; r.r[5] = -1;
    ro_swi(Wimp_SetCaretPosition, &r);
    if (!A.proj)
        return;
    int f = frame_of(wx);
    int drag = buttons & (0x40 | 0x10);
    if (wy > -RULER_H && app_source_mode()) /* (the ruler: the project in the Viewer again) */
        app_project_view();
    if (wy > -RULER_H) {
        /* the ruler: the only place the playhead is moved from. A click puts
           it there; a drag (on it or anywhere on the ruler) scrubs. (The
           Pi: on the track, the playhead's line took clicks and drags meant
           for a clip's edges.) */
        if (!drag)
            app_seek(snapped(f));
        else {
            /* the pointer kept to the ruler's visible part while dragging */
            int d[10];
            d[0] = T.win;
            d[1] = 7; /* a point, nothing drawn */
            d[2] = d[4] = b[0];
            d[3] = d[5] = b[1];
            d[6] = st[1]; d[7] = st[4] - RULER_H + 4; d[8] = st[3] - 1; d[9] = st[4] - 4;
            r.r[1] = (intptr_t) d;
            if (!ro_swi(Wimp_DragBox, &r))
                T.drag = DRAG_SCRUB;
        }
        return;
    }
    /* the audio tracks: sounds are selected, moved and trimmed */
    int ak = audio_row(wy);
    if (ak >= 0 && wy < TRACK_BOTTOM - 4) {
        if (drag) {
            audio_drag_start(ak, f, wx, b, st);
            return;
        }
        int ai = re_project_audio_at(A.proj, ak, (int) ((wx - LEFT) / T.scale));
        if (ai >= 0)
            tl_select_audio(ak, adjust && A.au_track == ak && A.au_sel == ai ? -1 : ai);
        else if (!adjust)
            tl_select(-1);
        return;
    }
    /* V2: overlays are selected, moved and trimmed */
    if (wy > V2_BOTTOM - 4) {
        if (drag) {
            overlay_drag_start(f, wx, b, st);
            return;
        }
        int ov = re_project_overlay_at(A.proj, (int) ((wx - LEFT) / T.scale));
        if (ov >= 0)
            tl_select_overlay(adjust && A.ov_sel == ov ? -1 : ov);
        else if (!adjust)
            tl_select(-1);
        return;
    }
    /* the track: clips are selected, trimmed and moved; the playhead stays */
    if (drag) {
        drag_start(f, wx, wy, b, st);
        return;
    }
    int i = re_project_item_at(A.proj, f);
    re_item it;
    if (i >= 0 && !re_project_item(A.proj, i, &it) && it.gap)
        i = -1;
    if (i < 0) { /* empty track space: nothing selected */
        if (!adjust)
            tl_select(-1);
        return;
    }
    if (adjust)
        tl_select(A.sel == i ? -1 : i);
    else
        tl_select(i);
}

/* Scroll_Request (the window asks for them): the scroll bar's arrows and
   the scroll wheel scroll sideways; with Ctrl, the wheel zooms about the
   pointer */
void tl_scroll(int *b)
{
    _kernel_swi_regs r;
    int dx = b[8], dy = b[9];
    if (ctrl_down() && dy) {
        int p[5];
        r.r[1] = (intptr_t) p;
        ro_swi(Wimp_GetPointerInfo, &r);
        int sx_screen = p[0] - b[1];                   /* the pointer from the window's left */
        int f = frame_of(p[0] - (b[1] - b[5]));       /* the frame under it */
        double old = T.scale;
        T.scale = dy > 0 ? T.scale * 2 : T.scale / 2;
        if (T.scale > 64) T.scale = 64;
        if (T.scale < 0.01) T.scale = 0.01;
        if (T.scale != old) {
            set_extent();
            scroll_to(x_of(f) - sx_screen);           /* (that frame kept under the pointer) */
            redraw_all();
        }
        return;
    }
    int step = 0, page = T.vis_w * 3 / 4;
    if (dx)
        step = (dx == 2 || dx == -2) ? (dx > 0 ? page : -page) : dx * 80;
    else if (dy) /* (the wheel: up is back) */
        step = (dy == 2 || dy == -2) ? (dy > 0 ? -page : page) : -dy * 120;
    if (step)
        scroll_to(T.scroll_x + step);
}

/* A frame snapped to a cut (or the start or end) within SNAP OS units,
   when snapping is on */
static int snapped(int f)
{
    if (!A.snap || !A.proj)
        return f;
    int best = f, bd = SNAP + 1;
    int c[2] = {re_project_cut(A.proj, f + 1, -1), re_project_cut(A.proj, f - 1, 1)};
    for (int k = 0; k < 2; k++) {
        if (c[k] < 0)
            continue;
        int d = (int) ((c[k] > f ? c[k] - f : f - c[k]) * T.scale);
        if (d < bd) {
            bd = d;
            best = c[k];
        }
    }
    return best;
}

const char *tl_help(int screen_y)
{
    int st[9];
    _kernel_swi_regs r;
    st[0] = T.win;
    r.r[1] = (intptr_t) st;
    int wy = ro_swi(Wimp_GetWindowState, &r) ? 0 : screen_y - (st[4] - st[6]);
    if (wy > -RULER_H)
        return "This is the Timeline's ruler.|MClick or drag here to move the playhead.|M"
               "I and O mark a stretch on it; Return plays the stretch, ' takes it out.";
    if (wy > V2_BOTTOM - 4)
        return "This is V2: pictures and videos over the video.|MDrop a PNG, JPEG, sprite or video here; "
               "drag one to move it, its ends to trim it; Menu over it to place it, size it, fade it, mute it.";
    if (wy < TRACK_BOTTOM - 4 && audio_row(wy) >= 0)
        return "These are the audio tracks: music and sound effects.|MDrop sound here (one dropped where another "
               "plays goes on the track below); drag one to move it, its ends to trim it; Menu over it for its sound.";
    return "This is V1: the clips.|MClick a clip to select it; drag its edge to trim it (with Ctrl: roll the "
           "cut), its middle to move it.|MMenu over a clip gives its effects.|MCtrl and the wheel zoom.";
}

void tl_title(void)
{
    char t[sizeof(T.title)];
    int made, all;
    if (px_making(&made, &all))
        snprintf(t, sizeof(t), "Timeline (making proxies: %d of %d made)", made, all);
    else
        snprintf(t, sizeof(t), "Timeline");
    if (!strcmp(t, T.title))
        return;
    snprintf(T.title, sizeof(T.title), "%s", t);
    if (T.win) {
        _kernel_swi_regs r;
        r.r[0] = T.win;
        r.r[1] = 0x4B534154; /* "TASK": R2 says what */
        r.r[2] = 3;          /* the title bar */
        ro_swi(Wimp_ForceRedraw, &r);
    }
}

#ifdef RE_TEST
const char *re_test_tl_title(void) { return T.title; }
#endif

void tl_refresh(void)
{
    redraw_all();
}

int tl_scrubbing(void)
{
    return T.drag != DRAG_NONE;
}

void tl_scrub_tick(void)
{
    int p[5], st[9];
    _kernel_swi_regs r;
    r.r[1] = (intptr_t) p;
    if (ro_swi(Wimp_GetPointerInfo, &r))
        return;
    st[0] = T.win;
    r.r[1] = (intptr_t) st;
    if (ro_swi(Wimp_GetWindowState, &r))
        return;
    int raw = frame_of(p[0] - (st[1] - st[5]));
    if (T.drag != DRAG_SCRUB) {
        if (raw != T.d_raw) {
            T.d_raw = raw;
            T.d_moved_cs = ro_now_cs();
            drag_tick(raw);
        } else if (T.d_show >= 0 && ro_now_cs() - T.d_moved_cs >= REST_CS) {
            app_seek(T.d_show); /* (rested: the picture) */
            T.d_show = -1;
        }
        return;
    }
    int f = snapped(raw);
    if (f != app_position())
        app_seek(f); /* (seeks before the next picture replace each other: the newest is drawn) */
}

void tl_drag_end(int *b)
{
    (void) b;
    if (T.drag == DRAG_NONE)
        return;
    tl_scrub_tick();
    int mode = T.drag;
    T.drag = DRAG_NONE;
    if (mode == DRAG_MOVE) {
        if (T.d_line >= 0)
            redraw_frames(T.d_line, T.d_line);
        T.d_line = -1;
        int j = T.d_before >= 0 ? re_project_move(A.proj, T.d_item, T.d_before) : -1;
        if (j >= 0) {
            re_item it;
            A.sel = j;
            app_edited();
            if (!re_project_item(A.proj, j, &it))
                app_seek(it.start);
        }
    } else if (mode != DRAG_SCRUB) { /* (trims, dissolves, fades, overlays: one undo) */
        re_project_end(A.proj);
        app_edited();
        if (T.d_show >= 0)
            app_seek(T.d_show);
        T.d_show = -1;
    }
}
