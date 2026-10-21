/* display.c - the Viewer's picture (display.h).
 *
 * Taken from Reel (riscos-ffmpeg's player/reel.c), whose notes on the Pi 4's
 * overlay (riscos-mesa's ovltest, VideoOverlay 0.02, BCMVideo) hold here:
 *   - VideoOverlay_Vet fails for every format: probe with Create;
 *   - overlay memory is uncached (written at 2-3 GB/s, read at 152 MB/s):
 *     only ever write it, never read it back;
 *   - a buffer switch happens at the next vsync: 3 buffers (2 when the GPU
 *     has no room), and none written until a vsync has passed since the
 *     last switch, else it tears;
 *   - the overlay is "Basic": it covers everything, menus and windows too,
 *     so while anything overlaps the picture it's hidden and the picture is
 *     drawn in the window as a sprite; and while stopped (an editor is
 *     stopped most of the time: menus, dialogue boxes and drags over the
 *     picture must show);
 *   - a mode change doesn't free the old overlay: destroy it ourselves.
 * Anything going wrong with the overlay falls back to the sprite, with no
 * error box, until the picture's size or the mode changes.
 *
 * The sprite: 32bpp in the screen's colour order, the picture area's size,
 * the picture fitted in it with black bars, converted from yuv420p by
 * swscale (NEON). Plotted 1:1 with OS_SpriteOp, clipped to the picture area.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "display.h"

#include <libswscale/swscale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void (*log_fn)(const char *line);

static void lg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void lg(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    if (!log_fn)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    log_fn(line);
}

void re_display_set_log(void (*log)(const char *line))
{
    log_fn = log;
}

static struct
{
    int task;
    int xeig, yeig, log2bpp, trgb;
    int window;
    ro_box pic;           /* the picture area, work area coordinates */
    /* the sprite */
    int *area;
    int spr_w, spr_h, spr_rows;
    int have_picture;
    int last_w, last_h;   /* the picture last converted into it (its bars) */
    struct SwsContext *sws;
    re_display_stats stats;
} D;

/* ---- the screen ------------------------------------------------------- */

static int mode_var(int var)
{
    _kernel_swi_regs r;
    r.r[0] = -1;
    r.r[1] = var;
    return ro_swi(OS_ReadModeVariable, &r) ? 0 : r.r[2];
}

static void read_screen(void)
{
    D.xeig = mode_var(4);
    D.yeig = mode_var(5);
    D.log2bpp = mode_var(9);
    D.trgb = D.log2bpp == 5 && (mode_var(0) & 0x4000);
    lg("screen: %dx%d pixels, %d bpp, %s", mode_var(11) + 1, mode_var(12) + 1, 1 << D.log2bpp,
       D.trgb ? "TRGB" : "TBGR");
}

/* ---- the overlay -------------------------------------------------------- */

enum { OV_CREATE, OV_DESTROY, OV_DISPLAY, OV_MAP, OV_UNMAP, OV_SCALE, OV_WINDOW, OV_POSITION, OV_REDRAW, OV_N };
static const char *const ov_names[OV_N] = {
    "VideoOverlay_Create", "VideoOverlay_Destroy", "VideoOverlay_DisplayBuffer", "VideoOverlay_MapBuffer",
    "VideoOverlay_UnmapBuffer", "VideoOverlay_SetScale", "VideoOverlay_SetWindow", "VideoOverlay_SetPosition",
    "VideoOverlay_RedrawWindow"};
#define YV12_FOURCC 0x32315659

static struct
{
    int found;
    int swi[OV_N];
    int id, banks, next, shown, vsync;
    int minw, minh, maxw, maxh;
    int win;
    int fw, fh, colour; /* what it was made for (or failed for) */
    int failed;
    int was_covered;    /* (for the log: said once each time) */
    int placed[9];
} ov;

static int ov_allowed(void)
{
    const char *e = getenv("EGL$Overlay"); /* one switch for every program's overlays */
    if (getenv("ReelEdit$NoOverlay"))
        return 0;
    return !(e && (!strcasecmp(e, "off") || !strcasecmp(e, "no") || !strcmp(e, "0")));
}

static int ov_available(void)
{
    _kernel_swi_regs r;
    if (ov.found)
        return 1;
    for (int i = 0; i < OV_N; i++) {
        r.r[0] = 0;
        r.r[1] = (intptr_t) ov_names[i];
        if (ro_swi(OS_SWINumberFromString, &r))
            return 0;
        ov.swi[i] = r.r[0] & ~0x20000;
    }
    ov.found = 1;
    return 1;
}

static _kernel_oserror *ov_call(int which, int a, int b)
{
    _kernel_swi_regs r;
    r.r[0] = a;
    r.r[1] = b;
    return ro_swi(ov.swi[which], &r);
}

static int vsyncs(void)
{
    _kernel_swi_regs r;
    r.r[0] = 176; /* OS_Byte 176: the vsync counter */
    r.r[1] = 0;
    r.r[2] = 255;
    return ro_swi(OS_Byte, &r) ? 0 : r.r[1] & 0xFF;
}

static void ov_destroy(void)
{
    if (ov.id) {
        if (ov.shown)
            ov_call(OV_DISPLAY, ov.id, -1);
        ov_call(OV_DESTROY, ov.id, 0);
        lg("overlay: destroyed");
    }
    ov.id = ov.shown = ov.win = 0;
}

static void ov_fail(const char *why)
{
    ov_destroy();
    ov.failed = 1;
    lg("overlay: %s: drawing in the window", why);
}

int re_display_hide_overlay(void)
{
    if (!ov.shown)
        return 0;
    ov_call(OV_DISPLAY, ov.id, -1);
    ov.shown = 0;
    return 1;
}

int re_display_overlay_shown(void)
{
    return ov.shown;
}

/* A YV12 overlay of the picture's size and colours: 3 buffers, else 2, each
   mapped once now so a GPU without room shows up here. */
static int ov_create(int fw, int fh, int colour709, int full)
{
    int sel[12], b = 0, banks;
    _kernel_swi_regs r;
    for (banks = 3; banks >= 2; banks--) {
        sel[0] = 1; sel[1] = fw; sel[2] = fh; sel[3] = 7; sel[4] = -1; /* Log2BPP 7: planar */
        sel[5] = 0;                                                    /* ModeFlags: YCbCr, bit 14 video range, bit 15 BT.709 */
        sel[6] = 0x2000 | (full ? 0 : 0x4000) | (colour709 ? 0x8000 : 0);
        sel[7] = 3; sel[8] = YV12_FOURCC; /* NColour */
        sel[9] = 13; sel[10] = banks;     /* MinScreenBanks */
        sel[11] = -1;
        r.r[0] = (intptr_t) sel;
        r.r[1] = (fw << 16) | fh;
        r.r[2] = 1; /* must scale */
        r.r[3] = D.task;
        if (ro_swi(ov.swi[OV_CREATE], &r)) {
            lg("overlay: Create refused YV12 %dx%d", fw, fh);
            return 0;
        }
        ov.id = r.r[0];
        ov.minw = r.r[2]; ov.minh = r.r[3]; ov.maxw = r.r[4]; ov.maxh = r.r[5];
        for (b = 0; b < banks; b++) {
            if (ov_call(OV_MAP, ov.id, b))
                break;
            ov_call(OV_UNMAP, ov.id, b);
        }
        if (b == banks)
            break;
        lg("overlay: only %d of %d buffers (GPU memory)", b, banks);
        ov_call(OV_DESTROY, ov.id, 0);
        ov.id = 0;
    }
    if (!ov.id)
        return 0;
    ov.banks = banks;
    ov.next = 0;
    ov.win = 0;
    ov.vsync = vsyncs() - 1; /* "a vsync has passed since" */
    memset(ov.placed, 0, sizeof(ov.placed));
    lg("overlay: YV12 %dx%d, BT.%s %s range, %d buffers, scales %dx%d to %dx%d", fw, fh, colour709 ? "709" : "601",
       full ? "full" : "video", banks, ov.minw, ov.minh, ov.maxw, ov.maxh);
    return 1;
}

/* Is anything over the picture: the window not open, or a window or menu
   in front of it overlapping the picture? Each window's "behind" word is
   the window just in front of it (-1 at the front). */
static int covered(void)
{
    int st[9], o[9], n, h;
    ro_box sc;
    _kernel_swi_regs r;
    st[0] = D.window;
    r.r[1] = (intptr_t) st;
    if (ro_swi(Wimp_GetWindowState, &r) || !(st[8] & (1 << 16)))
        return 1;
    sc.x0 = st[1] - st[5] + D.pic.x0; sc.x1 = st[1] - st[5] + D.pic.x1;
    sc.y0 = st[4] - st[6] + D.pic.y0; sc.y1 = st[4] - st[6] + D.pic.y1;
    for (h = st[7], n = 0; h != -1 && n < 256; n++) {
        o[0] = h;
        r.r[1] = (intptr_t) o;
        if (ro_swi(Wimp_GetWindowState, &r))
            return 1; /* can't tell: be safe */
        if ((o[8] & (1 << 16)) && o[1] < sc.x1 && o[3] > sc.x0 && o[2] < sc.y1 && o[4] > sc.y0)
            return 1;
        h = o[7];
    }
    return 0;
}

int re_display_check_covered(void)
{
    if (ov.shown && covered())
        return re_display_hide_overlay();
    return 0;
}

/* Fits the overlay in the picture area, keeping the picture's shape. 1
   placed, 0 outside its scaling limits, -1 an error. */
static int ov_place(int pw, int ph)
{
    int bw = (D.pic.x1 - D.pic.x0) >> D.xeig, bh = (D.pic.y1 - D.pic.y0) >> D.yeig;
    int rw, rh, x, y, want[9];
    _kernel_swi_regs r;
    if (bw < 1 || bh < 1)
        return 0;
    double sx = (double) bw / pw, sy = (double) bh / ph, s = sx < sy ? sx : sy;
    rw = (int) (pw * s + 0.5);
    rh = (int) (ph * s + 0.5);
    if (rw > bw) rw = bw;
    if (rh > bh) rh = bh;
    if (rw < 1 || rh < 1 || (ov.minw && rw < ov.minw) || (ov.minh && rh < ov.minh) || (ov.maxw && rw > ov.maxw)
        || (ov.maxh && rh > ov.maxh))
        return 0;
    x = D.pic.x0 + (((bw - rw) / 2) << D.xeig); /* top left */
    y = D.pic.y1 - (((bh - rh) / 2) << D.yeig);
    want[0] = D.window; want[1] = rw; want[2] = rh; want[3] = x; want[4] = y;
    want[5] = D.pic.x0; want[6] = D.pic.y0; want[7] = D.pic.x1; want[8] = D.pic.y1;
    if (!memcmp(want, ov.placed, sizeof(want)))
        return 1;
    if (ov.win != D.window) {
        if (ov_call(OV_WINDOW, ov.id, D.window))
            return -1;
        ov.win = D.window;
    }
    r.r[0] = ov.id; r.r[1] = rw; r.r[2] = rh; r.r[3] = (rw << 16) | rh;
    if (ro_swi(ov.swi[OV_SCALE], &r))
        return -1;
    r.r[0] = ov.id; r.r[1] = x; r.r[2] = y;
    r.r[3] = D.pic.x0; r.r[4] = D.pic.y0; r.r[5] = D.pic.x1; r.r[6] = D.pic.y1;
    if (ro_swi(ov.swi[OV_POSITION], &r))
        return -1;
    memcpy(ov.placed, want, sizeof(want));
    lg("overlay: %dx%d pixels at %d,%d in window &%x", rw, rh, x, y, D.window);
    return 1;
}

static void force_redraw_pic(void)
{
    _kernel_swi_regs r;
    r.r[0] = D.window; r.r[1] = D.pic.x0; r.r[2] = D.pic.y0; r.r[3] = D.pic.x1; r.r[4] = D.pic.y1;
    ro_swi(Wimp_ForceRedraw, &r);
}

/* 1 shown, 0 not (draw it in the window), 2 left out (no vsync since the
   last switch: every buffer may still be in use; the next picture comes
   within a frame) */
static int ov_show(const re_picture *p)
{
    int fw = p->width & ~1, fh = p->height & ~1, c709 = p->colorspace == 709, full = p->full_range, placed, b;
    _kernel_swi_regs r;
    if (!D.window || !ov_allowed() || !ov_available() || fw < 2 || fh < 2) {
        static int said;
        if (!said && D.window) { /* (once: why the overlay isn't used) */
            said = 1;
            lg("overlay: %s: drawing in the window", !ov_allowed() ? "turned off (ReelEdit$NoOverlay or EGL$Overlay)"
                                                     : !ov.found ? "the VideoOverlay module isn't loaded"
                                                                 : "the picture is too small");
        }
        re_display_hide_overlay();
        return 0;
    }
    int colour = c709 | full << 1;
    if ((ov.id || ov.failed) && (ov.fw != fw || ov.fh != fh || ov.colour != colour)) {
        ov_destroy(); /* a new size or colours: start again */
        ov.failed = 0;
    }
    ov.fw = fw; ov.fh = fh; ov.colour = colour;
    if (ov.failed)
        return 0;
    if (covered()) {
        if (!ov.was_covered)
            lg("overlay: a window or menu over the picture: drawing in the window");
        ov.was_covered = 1;
        re_display_hide_overlay();
        return 0;
    }
    ov.was_covered = 0;
    if (!ov.id && !ov_create(fw, fh, c709, full)) {
        ov.failed = 1;
        return 0;
    }
    if ((placed = ov_place(fw, fh)) < 0) {
        ov_fail("placing it failed");
        return 0;
    }
    if (!placed) {
        re_display_hide_overlay();
        return 0;
    }
    if (vsyncs() == ov.vsync) {
        /* the last switch hasn't happened (every buffer may be in use):
           wait for it, up to a refresh, rather than lose the picture */
        unsigned t0 = ro_now_cs();
        while (vsyncs() == ov.vsync && ro_now_cs() - t0 < 3)
            ;
        if (vsyncs() == ov.vsync)
            return 2;
        D.stats.overlay_waited++;
    }
    b = ov.next;
    r.r[0] = ov.id;
    r.r[1] = b;
    if (ro_swi(ov.swi[OV_MAP], &r)) {
        ov_fail("MapBuffer failed");
        return 0;
    }
    {
        /* row by row, only written: the overlay's memory is uncached */
        const int *a = (const int *) (intptr_t) r.r[0];
        for (int pl = 0; pl < 3; pl++) {
            uint8_t *d = (uint8_t *) (intptr_t) a[pl * 2];
            int pitch = a[pl * 2 + 1], w = pl ? fw / 2 : fw, h = pl ? fh / 2 : fh;
            const uint8_t *s = p->planes[pl];
            for (int y = 0; y < h; y++)
                memcpy(d + (size_t) y * pitch, s + (size_t) y * p->strides[pl], w);
        }
        ov_call(OV_UNMAP, ov.id, b);
    }
    if (ov_call(OV_DISPLAY, ov.id, b)) {
        ov_fail("DisplayBuffer failed");
        return 0;
    }
    ov.vsync = vsyncs();
    ov.next = (b + 1) % ov.banks;
    if (!ov.shown) {
        ov.shown = 1;
        lg("overlay: showing");
        force_redraw_pic(); /* VideoOverlay_RedrawWindow prepares the area */
    }
    return 1;
}

/* ---- the sprite ------------------------------------------------------------- */

/* On the Pi small sprites weren't always replotted with their new contents
   (riscos-mesa found this); sprites of 1 MB or more always were, so pad
   with extra rows, which the clipping hides. */
#define MIN_SPRITE_BYTES (1024 * 1024)

static int *sprite_header(void) { return D.area + 4; }
static uint8_t *sprite_pixels(void) { return (uint8_t *) (D.area + 4 + 11); }

static void sprite_free(void)
{
    free(D.area);
    D.area = NULL;
    D.spr_w = D.spr_h = 0;
    D.have_picture = 0;
}

static int sprite_make(int w, int h)
{
    size_t rows = h, bytes;
    int *spr, mode;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (D.area && D.spr_w == w && D.spr_h == h)
        return 0;
    sprite_free();
    if ((size_t) w * 4 * rows < MIN_SPRITE_BYTES)
        rows = (MIN_SPRITE_BYTES + (size_t) w * 4 - 1) / ((size_t) w * 4);
    bytes = 16 + 44 + (size_t) w * 4 * rows;
    if (!(D.area = calloc(1, bytes))) /* black */
        return -1;
    D.area[0] = (int) bytes; /* area: size, sprites, first, free */
    D.area[1] = 1;
    D.area[2] = 16;
    D.area[3] = (int) bytes;
    spr = sprite_header();
    spr[0] = (int) bytes - 16;
    memcpy(&spr[1], "reeledit\0\0\0\0", 12);
    spr[4] = w - 1;
    spr[5] = (int) rows - 1;
    spr[6] = 0;
    spr[7] = 31;
    spr[8] = 44;
    spr[9] = 44;
    mode = 1 | ((180 >> D.xeig) << 1) | ((180 >> D.yeig) << 14) | (6 << 27); /* 32bpp TBGR */
    if (D.trgb) {
        _kernel_swi_regs r;
        r.r[0] = 1; /* the current mode's specifier */
        if (!ro_swi(OS_ScreenMode, &r))
            mode = r.r[1];
    }
    spr[10] = mode;
    D.spr_w = w;
    D.spr_h = h;
    D.spr_rows = (int) rows;
    D.last_w = D.last_h = 0;
    return 0;
}

/* The picture into the sprite, fitted, with black bars */
static int sprite_convert(const re_picture *p)
{
    int w = D.spr_w, h = D.spr_h, rw, rh, x, y;
    double sx = (double) w / p->width, sy = (double) h / p->height, s = sx < sy ? sx : sy;
    rw = (int) (p->width * s + 0.5);
    rh = (int) (p->height * s + 0.5);
    if (rw > w) rw = w;
    if (rh > h) rh = h;
    if (rw < 1 || rh < 1)
        return -1;
    x = (w - rw) / 2;
    y = (h - rh) / 2;
    if (rw != D.last_w || rh != D.last_h) {
        memset(sprite_pixels(), 0, (size_t) w * 4 * D.spr_rows); /* the bars */
        D.last_w = rw;
        D.last_h = rh;
    }
    D.sws = sws_getCachedContext(D.sws, p->width, p->height, AV_PIX_FMT_YUV420P, rw, rh,
                                 D.trgb ? AV_PIX_FMT_BGR0 : AV_PIX_FMT_RGB0, SWS_BILINEAR, NULL, NULL, NULL);
    if (!D.sws)
        return -1;
    {
        /* the picture's colours: BT.709 or 601, video or full range */
        int srcr, dstr, b, c, s2, *inv, *tab;
        if (sws_getColorspaceDetails(D.sws, &inv, &srcr, &tab, &dstr, &b, &c, &s2) >= 0) {
            const int *coefs = sws_getCoefficients(p->colorspace == 709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
            sws_setColorspaceDetails(D.sws, coefs, p->full_range, tab, 1, b, c, s2);
        }
    }
    uint8_t *dst[4] = {sprite_pixels() + ((size_t) y * w + x) * 4, NULL, NULL, NULL};
    int dst_stride[4] = {w * 4, 0, 0, 0};
    const uint8_t *src[4] = {p->planes[0], p->planes[1], p->planes[2], NULL};
    int src_stride[4] = {p->strides[0], p->strides[1], p->strides[2], 0};
    sws_scale(D.sws, src, src_stride, 0, p->height, dst, dst_stride);
    D.have_picture = 1;
    return 0;
}

static void vdu_clip(int x0, int y0, int x1, int y1) /* inclusive, OS units */
{
    unsigned char b[9] = {24, x0 & 255, (x0 >> 8) & 255, y0 & 255, (y0 >> 8) & 255,
                          x1 & 255, (x1 >> 8) & 255, y1 & 255, (y1 >> 8) & 255};
    _kernel_swi_regs r;
    r.r[0] = (intptr_t) b;
    r.r[1] = 9;
    ro_swi(OS_WriteN, &r);
}

/* The rectangles of a redraw or update: the sprite's part of each */
static void draw_rects(int *b, int more, int redraw)
{
    _kernel_swi_regs r;
    while (more) {
        int ox = b[1] - b[5], oy = b[4] - b[6]; /* screen position of work area 0,0 */
        ro_box c = {ox + D.pic.x0, oy + D.pic.y0, ox + D.pic.x1, oy + D.pic.y1};
        if (c.x0 < b[7]) c.x0 = b[7];
        if (c.y0 < b[8]) c.y0 = b[8];
        if (c.x1 > b[9]) c.x1 = b[9];
        if (c.y1 > b[10]) c.y1 = b[10];
        if (c.x0 < c.x1 && c.y0 < c.y1 && D.area && D.have_picture && !(ov.shown && ov.win == b[0])) {
            vdu_clip(c.x0, c.y0, c.x1 - 1, c.y1 - 1);
            r.r[0] = 52 + 512; /* PutSpriteScaled, by pointer */
            r.r[1] = (intptr_t) D.area;
            r.r[2] = (intptr_t) sprite_header();
            r.r[3] = ox + D.pic.x0;
            r.r[4] = oy + D.pic.y1 - (D.spr_rows << D.yeig);
            r.r[5] = 0; /* overwrite */
            r.r[6] = 0; /* no scaling: the sprite's dpi is the screen's */
            r.r[7] = 0;
            ro_swi(OS_SpriteOp, &r);
        }
        if (redraw && ov.shown && ov.win == b[0]) { /* the overlay's part of a redraw */
            r.r[0] = ov.id;
            r.r[1] = (intptr_t) b;
            ro_swi(ov.swi[OV_REDRAW], &r);
        }
        r.r[1] = (intptr_t) b;
        more = ro_swi(Wimp_GetRectangle, &r) ? 0 : r.r[0];
    }
}

void re_display_redraw(int *block)
{
    /* (block holds the first rectangle; the rest come from Wimp_GetRectangle) */
    draw_rects(block, 1, 1);
}

static void sprite_show(const re_picture *p)
{
    int b[11];
    _kernel_swi_regs r;
    if (!D.window || sprite_make((D.pic.x1 - D.pic.x0) >> D.xeig, (D.pic.y1 - D.pic.y0) >> D.yeig) < 0
        || sprite_convert(p) < 0)
        return;
    b[0] = D.window;
    b[1] = D.pic.x0; b[2] = D.pic.y0; b[3] = D.pic.x1; b[4] = D.pic.y1;
    r.r[1] = (intptr_t) b;
    if (ro_swi(Wimp_UpdateWindow, &r))
        return;
    draw_rects(b, r.r[0], 0);
}

/* ---- the interface ---------------------------------------------------------- */

void re_display_init(int task)
{
    D.task = task;
    read_screen();
}

void re_display_mode_changed(void)
{
    ov_destroy(); /* the old one outlives a mode change */
    ov.failed = 0;
    read_screen();
    sprite_free();
}

void re_display_place(int window, ro_box pic)
{
    D.window = window;
    D.pic = pic;
}

int re_display_show(const re_picture *p, int overlay_ok)
{
    if (overlay_ok) {
        int s = ov_show(p);
        if (s == 1) {
            D.stats.overlay_pictures++;
            return 1;
        }
        if (s == 2) {
            D.stats.overlay_skipped++;
            return 1;
        }
    } else {
        re_display_hide_overlay();
    }
    sprite_show(p);
    D.stats.sprite_pictures++;
    return 0;
}

void re_display_close(void)
{
    ov_destroy();
    sprite_free();
    sws_freeContext(D.sws);
    D.sws = NULL;
    D.window = 0;
}

void re_display_get_stats(re_display_stats *s)
{
    *s = D.stats;
    s->overlay_failed = ov.failed;
}
