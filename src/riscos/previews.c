/* previews.c - the Timeline's thumbnails and waveforms (app.h), from
 * src/media.c: a clip's first and last frames (Premiere's "head and
 * tail"), and its sound's peaks along its bottom; a sound's on an audio
 * track along it.
 *
 * Made while nothing plays and nothing is being dragged, on null events,
 * at most 4 cs at a time: what's in view first. Until a clip's thumbnail
 * is made, the nearest one made of its file stands in (a trimmed clip's
 * new first frame: its old one). The waveform is drawn as it'll be heard:
 * at the clip's level, through its fades; none when it's muted. Its scale
 * is in dB (48 dB below full scale to full scale), so quiet sound shows.
 *
 * The thumbnails are plotted from one sprite, a grid of them, of a
 * megabyte or more: on the Pi small sprites weren't always plotted again
 * with new contents (display.c), and a cell's contents change as
 * thumbnails come and go.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "app.h"
#include "display.h"
#include "../media.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ColourTrans_SetGCOL 0x40743
#define OS_Plot             0x45
#define COLS 8
#define ROWS 8
#define CELLS (COLS * ROWS)
#define MIN_SPRITE_BYTES (1024 * 1024)
#define WORK_CS 4     /* at most, a null event */
#define FLOOR_DB 48.0 /* a waveform's scale: from this far below full scale (nothing shown) to full scale */
#define SHOW_CS 25    /* thumbnails coming in: the Timeline drawn again at most this often */
#ifdef RE_TEST
#define TICK_STEPS 2  /* (the tests' clock stands still in a null event: a few steps, so it takes several) */
#else
#define TICK_STEPS 16
#endif

static struct
{
    re_media *m;
    double fps, dar;
    int h_os;          /* the thumbnails' height, OS units */
    int tw, th;        /* their size, pixels */
    int xeig, yeig, trgb, mode;
    int *area;         /* the grid */
    int spr_rows;
    unsigned key[CELLS];  /* the thumbnail in each cell (its serial; 0: none) */
    unsigned used[CELLS]; /* when last plotted */
    unsigned clock;
    int to_show;       /* something made, not shown yet */
    unsigned shown_cs;
    int drawn;         /* (tests: thumbnails plotted, waveform rectangles drawn, since asked) */
    int wave_rects;
    int stand_ins;     /* (tests: stand-ins drawn in the Timeline's last redraw) */
} P;

static void forget(void)
{
    re_media_free(P.m);
    free(P.area);
    memset(&P, 0, sizeof(P));
}

void pv_close(void)
{
    forget();
}

/* Made for the project as it is now (its frame rate and shape, the
   screen's mode): 1 ready */
static int ready(int h_os)
{
    int xeig, yeig, trgb;
    if (!A.proj)
        return 0;
    re_display_screen(&xeig, &yeig, &trgb);
    double fps = re_project_fps(A.proj), dar = mlt_profile_dar(re_project_profile(A.proj));
    if (dar <= 0)
        dar = 16.0 / 9;
    if (P.m && (P.fps != fps || fabs(P.dar - dar) > 0.001 || P.h_os != h_os || P.xeig != xeig || P.yeig != yeig
                || P.trgb != trgb || P.mode != re_display_sprite_mode()))
        forget();
    if (P.m)
        return 1;
    P.fps = fps;
    P.dar = dar;
    P.h_os = h_os;
    P.xeig = xeig;
    P.yeig = yeig;
    P.trgb = trgb;
    P.mode = re_display_sprite_mode();
    P.th = h_os >> yeig;
    P.tw = (int) ((h_os * dar) / (1 << xeig) + 0.5);
    if (P.th < 1) P.th = 1;
    if (P.tw < 1) P.tw = 1;
    P.m = re_media_new(fps, P.tw, P.th);
    return P.m != NULL;
}

int pv_thumb_width(int h_os)
{
    return ready(h_os) ? P.tw << P.xeig : 0;
}

/* The grid sprite (made the first time) */
static int *grid(void)
{
    if (P.area)
        return P.area + 4;
    size_t w = (size_t) P.tw * COLS, rows = (size_t) P.th * ROWS;
    if (w * 4 * rows < MIN_SPRITE_BYTES)
        rows = (MIN_SPRITE_BYTES + w * 4 - 1) / (w * 4);
    size_t bytes = 16 + 44 + w * 4 * rows;
    if (!(P.area = calloc(1, bytes)))
        return NULL;
    P.area[0] = (int) bytes; /* area: size, sprites, first, free */
    P.area[1] = 1;
    P.area[2] = 16;
    P.area[3] = (int) bytes;
    int *spr = P.area + 4;
    spr[0] = (int) bytes - 16;
    memcpy(&spr[1], "thumbs\0\0\0\0\0\0", 12);
    spr[4] = (int) w - 1;
    spr[5] = (int) rows - 1;
    spr[6] = 0;
    spr[7] = 31;
    spr[8] = 44;
    spr[9] = 44;
    spr[10] = P.mode;
    P.spr_rows = (int) rows;
    return spr;
}

/* The cell holding a thumbnail: put there if it isn't (in place of the
   least recently plotted) */
static int cell_of(const unsigned char *px, unsigned serial)
{
    int c, oldest = 0;
    for (c = 0; c < CELLS; c++)
        if (P.key[c] == serial)
            break;
        else if (P.used[c] < P.used[oldest])
            oldest = c;
    int *spr = grid();
    if (!spr)
        return -1;
    if (c == CELLS) {
        c = oldest;
        P.key[c] = serial;
        unsigned char *base = (unsigned char *) spr + spr[8];
        size_t stride = (size_t) P.tw * COLS * 4;
        for (int y = 0; y < P.th; y++) {
            unsigned char *d = base + ((size_t) (c / COLS) * P.th + y) * stride + (size_t) (c % COLS) * P.tw * 4;
            const unsigned char *s = px + (size_t) y * P.tw * 4;
            if (!P.trgb) {
                memcpy(d, s, (size_t) P.tw * 4); /* (R in the low byte, as the thumbnail's) */
            } else {
                for (int x = 0; x < P.tw; x++) {
                    d[x * 4] = s[x * 4 + 2];
                    d[x * 4 + 1] = s[x * 4 + 1];
                    d[x * 4 + 2] = s[x * 4];
                    d[x * 4 + 3] = 0;
                }
            }
        }
    }
    P.used[c] = ++P.clock;
    return c;
}

static void vdu_clip(int x0, int y0, int x1, int y1) /* inclusive */
{
    unsigned char b[9] = {24, x0 & 255, (x0 >> 8) & 255, y0 & 255, (y0 >> 8) & 255,
                          x1 & 255, (x1 >> 8) & 255, y1 & 255, (y1 >> 8) & 255};
    _kernel_swi_regs r;
    r.r[0] = (intptr_t) b;
    r.r[1] = 9;
    ro_swi(OS_WriteN, &r);
}

int pv_thumb(const char *file, int frame, int x, int y1, int h_os, const ro_box *clip)
{
    unsigned serial = 0;
    _kernel_swi_regs r;
    if (!file || !ready(h_os))
        return 0;
    const unsigned char *px = re_media_thumb(P.m, file, frame, &serial);
    if (!px && (px = re_media_thumb_near(P.m, file, frame, &serial)))
        P.stand_ins++;
    if (!px)
        return 0;
    int c = cell_of(px, serial);
    if (c < 0)
        return 0;
    int w = P.tw << P.xeig, h = P.th << P.yeig;
    int c0 = x > clip->x0 ? x : clip->x0, c1 = x + w - 1 < clip->x1 ? x + w - 1 : clip->x1;
    int r0 = y1 - h > clip->y0 ? y1 - h : clip->y0, r1 = y1 - 1 < clip->y1 ? y1 - 1 : clip->y1;
    if (c1 < c0 || r1 < r0)
        return w;
    vdu_clip(c0, r0, c1, r1);
    r.r[0] = 52 + 512; /* PutSpriteScaled, by pointer */
    r.r[1] = (intptr_t) P.area;
    r.r[2] = (intptr_t) (P.area + 4);
    r.r[3] = x - ((c % COLS) * P.tw << P.xeig);
    r.r[4] = (y1 - h) - ((P.spr_rows - (c / COLS + 1) * P.th) << P.yeig);
    r.r[5] = 0;
    r.r[6] = 0;
    r.r[7] = 0;
    ro_swi(OS_SpriteOp, &r);
    vdu_clip(clip->x0, clip->y0, clip->x1, clip->y1);
    P.drawn++;
    return w;
}

static void rect(int x0, int y0, int x1, int y1)
{
    _kernel_swi_regs r;
    r.r[0] = 4; r.r[1] = x0; r.r[2] = y0;
    ro_swi(OS_Plot, &r);
    r.r[0] = 96 + 5; r.r[1] = x1; r.r[2] = y1;
    ro_swi(OS_Plot, &r);
    P.wave_rects++;
}

/* A sound's frames g0..g1 (not g1) as its file's frames k0..k1 (not k1) */
static void file_frames(const pv_sound *s, int g0, int g1, int *k0, int *k1)
{
    double sp = s->speed ? s->speed / 100.0 : 1, a = fabs(sp);
    if (sp > 0) {
        *k0 = (int) floor(g0 * a);
        *k1 = (int) ceil(g1 * a);
    } else {
        *k0 = (int) floor((s->source_length - g1) * a);
        *k1 = (int) ceil((s->source_length - g0) * a);
    }
    if (*k1 <= *k0)
        *k1 = *k0 + 1;
}

const unsigned char *pv_peaks(const char *file, int *n, int *done)
{
    *n = *done = 0;
    if (!file || !ready(P.h_os ? P.h_os : 96))
        return NULL;
    return re_media_peaks(P.m, file, n, done);
}

void pv_wave(const char *file, const pv_sound *s, int x0, double scale, int y0, int h, unsigned colour,
             const ro_box *clip)
{
    int n = 0, done = 0;
    if (!file || s->muted || s->length <= 0 || !ready(P.h_os ? P.h_os : 96))
        return;
    const unsigned char *pk = re_media_peaks(P.m, file, &n, &done);
    if (!pk)
        return;
    _kernel_swi_regs r;
    r.r[0] = (int) colour; r.r[3] = 0; r.r[4] = 0;
    ro_swi(ColourTrans_SetGCOL, &r);
    int step = 1 << P.xeig, mid = y0 + h / 2, half = h / 2;
    int xa = clip->x0, xe = clip->x1 + 1; /* (a clip runs on under a dissolve: its frames beyond its ends) */
    xa = x0 + (int) floor((double) (xa - x0) / step) * step; /* (columns on the pixels) */
    double level = pow(10, s->volume / 20.0);
    int run_x = -1, run_h = 0; /* (columns of the same height: one rectangle) */
    for (int x = xa; x < xe + step; x += step) {
        int hh = 0;
        if (x < xe) {
            double a = (x - x0) / scale, b = (x + step - x0) / scale; /* (the frames under this column) */
            int f0 = (int) floor(a), f1 = (int) floor(b);
            if (f1 <= f0)
                f1 = f0 + 1;
            int top = 0, k0, k1; /* (the file's frames under them: at another speed, more or fewer) */
            file_frames(s, s->in + f0, s->in + f1, &k0, &k1);
            for (int k = k0 < 0 ? 0 : k0; k < k1 && k < n; k++)
                if (pk[k] > top)
                    top = pk[k];
            double g = level, t = a < 0 ? 0 : a > s->length ? s->length : a;
            if (s->fade_in > 0 && t < s->fade_in)
                g *= t / s->fade_in;
            if (s->fade_out > 0 && t > s->length - s->fade_out)
                g *= (s->length - t) / s->fade_out;
            double v = top / 255.0 * g; /* (shown in dB, as Audacity's and Premiere's are: quiet sounds seen) */
            v = v > 0 ? (20 * log10(v) + FLOOR_DB) / FLOOR_DB : 0;
            hh = (int) ((v < 0 ? 0 : v > 1 ? 1 : v) * half + 0.5);
            if (top > 0 && hh < 1)
                hh = 1;
        }
        if (run_x >= 0 && hh != run_h) {
            if (run_h > 0)
                rect(run_x, mid - run_h, x - 1, mid + run_h - 1);
            run_x = -1;
        }
        if (run_x < 0) {
            run_x = x;
            run_h = hh;
        }
    }
}

void pv_redraw_begin(void)
{
    P.stand_ins = 0;
}

int pv_busy(void)
{
    return P.m && (re_media_pending(P.m) > 0 || P.to_show) && !(A.e && re_engine_speed(A.e) != 0)
           && !tl_scrubbing(); /* (any drag on the Timeline) */
}

void pv_tick(void)
{
    unsigned t0 = ro_now_cs();
    int n = 0, r = 0;
    while (n++ < TICK_STEPS && ro_now_cs() - t0 < WORK_CS && (r = re_media_step(P.m)) >= 0)
        if (r == 1)
            P.to_show = 1;
    if (P.to_show && (ro_now_cs() - P.shown_cs >= SHOW_CS || re_media_pending(P.m) == 0)) {
        P.to_show = 0;
        P.shown_cs = ro_now_cs();
        tl_refresh();
    }
}

#ifdef RE_TEST
int re_test_previews_stand_ins(void) { return P.stand_ins; }
int re_test_previews_pending(void) { return P.m ? re_media_pending(P.m) : 0; }
int re_test_previews(int *thumbs, int *wave_rects)
{
    *thumbs = P.drawn;
    *wave_rects = P.wave_rects;
    P.drawn = P.wave_rects = 0;
    return P.m ? re_media_pending(P.m) : -1;
}
#endif
