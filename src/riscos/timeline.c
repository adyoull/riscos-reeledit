/* timeline.c - the Timeline window (app.h): a ruler with the playhead, and
 * track V1's clips and gaps, scrolled sideways by the window's own scroll
 * bar. Select on the ruler moves the playhead; on a clip selects it
 * (Adjust: toggles); files dropped on it go in at the frame they're
 * dropped on. Drawn with ColourTrans and OS_Plot, the names with the
 * desktop font (Wimp_TextOp).
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "app.h"

#include <stdio.h>
#include <string.h>

#define ColourTrans_SetGCOL 0x40743
#define OS_Plot             0x45
#define Wimp_TextOp         0x400F9
#define Wimp_DragBox        0x400D0

#define RULER_H 48   /* OS units */
#define TRACK_H 96
#define LEFT    16   /* frame 0's x */
#define MIN_W   640
#define LABEL_W 200  /* the playhead's timecode, beside it on the ruler */
#define SNAP    12   /* OS units: the playhead snaps to a cut this near */

static struct
{
    int win;
    double scale;     /* OS units a frame */
    int vis_w, vis_h, scroll_x;
    int playhead;     /* the frame it's drawn at */
    int extent_w;
    char title[32];
    int scrubbing;    /* the playhead being dragged along the ruler */
} T = {0, 4.0, 0, 0, 0, 0, 0, "Timeline", 0};

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
    w.vis.x1 = 1000; w.vis.y1 = RULER_H + TRACK_H + 16;
    w.behind = -1;
    /* new format, back, close, title, toggle, horizontal scroll, adjust size, moveable */
    w.flags = (int) 0xEF000102u; /* (bit 8: Scroll_Request for the arrows and the wheel) */
    w.tfg = 7; w.tbg = 2; w.wfg = 7; w.wbg = 1;
    w.sofg = 3; w.sibg = 1; w.tfocus = 12;
    w.ext.x0 = 0; w.ext.y0 = -(RULER_H + TRACK_H + 16); w.ext.x1 = 4096; w.ext.y1 = 0;
    w.tflags = 0x1 | 0x4 | 0x8 | 0x10 | 0x20 | 0x100 | (7 << 24) | (2 << 28);
    w.wbutton = 10 << 12; /* click (x256), drag (x16), double click */
    w.sprites = 1;
    w.minw = MIN_W; w.minh = RULER_H + TRACK_H + 16;
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
    int b[4] = {0, -(RULER_H + TRACK_H + 16), w, 0};
    _kernel_swi_regs r;
    r.r[0] = T.win;
    r.r[1] = (intptr_t) b;
    ro_swi(Wimp_SetExtent, &r);
    T.extent_w = w;
}

static void redraw_all(void)
{
    _kernel_swi_regs r;
    r.r[0] = T.win; r.r[1] = 0; r.r[2] = -(RULER_H + TRACK_H + 16); r.r[3] = T.extent_w; r.r[4] = 0;
    ro_swi(Wimp_ForceRedraw, &r);
}

void tl_open(int x0, int y1, int width)
{
    int b[9];
    _kernel_swi_regs r;
    if (!T.win && create() < 0)
        return;
    T.vis_w = width;
    T.vis_h = RULER_H + TRACK_H + 16;
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
    redraw_all();
}

void tl_select(int item)
{
    A.sel = item;
    redraw_all();
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
    r.r[0] = T.win; r.r[1] = old - 8; r.r[2] = -(RULER_H + TRACK_H + 16); r.r[3] = old + LABEL_W + 4; r.r[4] = 0;
    ro_swi(Wimp_ForceRedraw, &r);
    r.r[0] = T.win; r.r[1] = now - 8; r.r[2] = -(RULER_H + TRACK_H + 16); r.r[3] = now + LABEL_W + 4; r.r[4] = 0;
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

static void text(const char *s, int x, int y, unsigned fg, unsigned bg)
{
    _kernel_swi_regs r;
    r.r[0] = 0; r.r[1] = (int) fg; r.r[2] = (int) bg;
    ro_swi(Wimp_TextOp, &r);
    r.r[0] = 2; r.r[1] = (intptr_t) s; r.r[2] = -1; r.r[3] = -1; r.r[4] = x; r.r[5] = y;
    ro_swi(Wimp_TextOp, &r);
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

static void draw(int *b)
{
    int ox = b[1] - b[5], oy = b[4] - b[6]; /* screen position of work area 0,0 */
    int cx0 = b[7], cy0 = b[8], cx1 = b[9] - 1, cy1 = b[10] - 1;
    int f0 = frame_of(cx0 - ox) - 1, f1 = frame_of(cx1 - ox) + 1;
    int ruler_y0 = oy - RULER_H, track_y1 = oy - RULER_H - 8, track_y0 = track_y1 - TRACK_H;
    double fps = A.proj ? re_project_fps(A.proj) : 25;

    /* the ruler */
    colour(0xDDDDDD00);
    rect(cx0, ruler_y0, cx1, oy);
    if (A.proj) {
        int step = ruler_step(fps), len = re_project_length(A.proj);
        colour(0x55555500);
        for (int f = (f0 < 0 ? 0 : f0) / step * step; f <= f1 && f <= len; f += step) {
            int x = ox + x_of(f);
            rect(x, ruler_y0, x + 1, ruler_y0 + 16);
            char tc[40];
            mark_label(tc, sizeof(tc), f, fps);
            text(tc, x + 6, ruler_y0 + 20, 0x00000000, 0xDDDDDD00);
        }
    }
    /* the track */
    colour(0x77777700);
    rect(cx0, track_y0 - 8, cx1, track_y1 + 8 - 1);
    if (A.proj) {
        for (int i = 0; i < re_project_items(A.proj); i++) {
            re_item it;
            re_project_item(A.proj, i, &it);
            if (it.gap || it.start + it.length < f0 || it.start > f1)
                continue;
            int x0 = ox + x_of(it.start), x1 = ox + x_of(it.start + it.length) - 2;
            if (x1 < x0)
                x1 = x0;
            /* selected: the clip's own colour inside a bright outline (a fill
               hid its name, Pi test) */
            unsigned fill = 0xC0905000;
            colour(i == A.sel ? 0x2080F000 : 0x20202000); /* (BBGGRR: amber) */
            rect(x0, track_y0, x1, track_y1);
            int inset = i == A.sel ? 6 : 2;
            colour(fill);
            rect(x0 + inset, track_y0 + inset, x1 - inset, track_y1 - inset);
            /* the name, clipped to the clip */
            int c0 = x0 + 2 > cx0 ? x0 + 2 : cx0, c1 = x1 - 2 < cx1 ? x1 - 2 : cx1;
            int r0 = track_y0 > cy0 ? track_y0 : cy0, r1 = track_y1 < cy1 ? track_y1 : cy1;
            if (c1 > c0 && r1 > r0) {
                clip(c0, r0, c1, r1);
                /* the name at the start, and again along a long clip (so
                   it's in view however it's scrolled) */
                for (int nx = x0 + 10; nx == x0 + 10 || nx < x1 - 200; nx += 1000)
                    if (nx < c1 && nx + 1000 > c0)
                        text(it.name, nx, track_y1 - 36, 0xFFFFFF00, fill);
                clip(cx0, cy0, cx1, cy1);
            }
        }
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
    while (more) {
        draw(b);
        r.r[1] = (intptr_t) b;
        more = ro_swi(Wimp_GetRectangle, &r) ? 0 : (int) r.r[0];
    }
}

/* ---- clicks -------------------------------------------------------------------- */

void tl_click(int *b)
{
    int buttons = b[2];
    int st[9];
    _kernel_swi_regs r;
    if (buttons == 2) { /* Menu */
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
    int on_playhead = wx > x_of(T.playhead) - 12 && wx < x_of(T.playhead) + 12;
    if (wy > -RULER_H || (drag && on_playhead)) {
        /* the ruler, or the playhead line grabbed: the playhead there;
           dragged, it follows the pointer */
        if (!(drag && on_playhead))
            app_seek(snapped(f));
        if (drag) {
            /* the pointer kept to the ruler's visible part while dragging */
            int d[10];
            d[0] = T.win;
            d[1] = 7; /* a point, nothing drawn */
            d[2] = d[4] = b[0];
            d[3] = d[5] = b[1];
            d[6] = st[1]; d[7] = st[4] - RULER_H + 4; d[8] = st[3] - 1; d[9] = st[4] - 4;
            r.r[1] = (intptr_t) d;
            if (!ro_swi(Wimp_DragBox, &r))
                T.scrubbing = 1;
        }
        return;
    }
    if (drag) /* (dragging clips: to come) */
        return;
    int i = re_project_item_at(A.proj, f);
    re_item it;
    if (i >= 0 && !re_project_item(A.proj, i, &it) && it.gap)
        i = -1;
    if (i < 0) { /* empty track space: the playhead there, nothing selected */
        if (!adjust)
            tl_select(-1);
        app_seek(snapped(f));
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
    r.r[0] = 121; r.r[1] = 1 ^ 0x80; /* OS_Byte 121: Ctrl down? */
    int ctrl = !ro_swi(OS_Byte, &r) && (r.r[1] & 0xFF) == 0xFF;
    if (ctrl && dy) {
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

int tl_scrubbing(void)
{
    return T.scrubbing;
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
    int f = snapped(frame_of(p[0] - (st[1] - st[5])));
    if (f != app_position())
        app_seek(f); /* (seeks before the next picture replace each other: the newest is drawn) */
}

void tl_drag_end(int *b)
{
    (void) b;
    if (!T.scrubbing)
        return;
    tl_scrub_tick();
    T.scrubbing = 0;
}
