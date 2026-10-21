/* viewer.c - Reel Edit's desktop front end, first part: the icon bar icon
 * and the Viewer, which plays a project (MLT XML) or a clip dropped on the
 * icon or the window. The editing windows (Timeline, Clips, Inspector)
 * come next (docs/DESIGN.md).
 *
 * The Viewer: the picture above a row of controls (start, a frame back,
 * play/stop, a frame on, end, the position bar, the timecode). Keys, with
 * the caret in it: Space plays and stops; J, K, L shuttle back, stop and
 * forward, faster each time J or L is pressed again (x2, x4); Left and
 * Right a frame (with Shift a second); Home and End; Escape stops.
 *
 * Keeping the desktop running (docs/DESIGN.md): MLT is built without
 * threads, and pictures are made by src/engine.c on null events, at most
 * one each time, asked for with Wimp_PollIdle at the next picture's time;
 * stopped, null events are masked out (no time used at all). While playing
 * with nothing over the picture, pictures go through the hardware overlay;
 * stopped, they're drawn in the window (display.c). The sound goes to
 * SharedSoundBuffer (sound.c).
 *
 * Log: <Wimp$ScrapDir>.ReelEditLog (ReelEdit$Log names another file, or
 * "off"), started afresh each time: what was opened, the overlay, and once
 * a second while playing, pictures made and left out, the time asleep.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "../engine.h"
#include "display.h"
#include "ro.h"
#include "sound.h"

#include <stdarg.h>
#include <stdio.h>
#ifdef __riscos__
#include <unixlib/local.h>
#endif
#include <stdlib.h>
#include <string.h>

#ifndef RE_VERSION
#define RE_VERSION "0.1"
#endif
#define APP "Reel Edit"

/* the heap in a dynamic area (UnixLib adds more areas when it needs them) */
const char *const __dynamic_da_name = "ReelEdit Heap";
int __dynamic_da_max_size = 128 << 20;

#define MSG_QUIT       0
#define MSG_DATALOAD   3
#define MSG_DATALOADACK 4
#define MSG_PREQUIT    8
#define MSG_MODECHANGE 0x400C1

#define SOUND_RATE 48000

/* the controls, OS units */
#define CH     64  /* the row's height */
#define GAP    4
#define W_BTN  72
#define W_PLAY 112
#define W_TIME 360
#define MIN_W  (GAP * 8 + W_BTN * 4 + W_PLAY + 200 + W_TIME)

enum { I_BAR, I_START, I_BACK, I_PLAY, I_FWD, I_END, I_TRACK, I_FILL, I_TIME, N_ICONS };

typedef struct
{
    char *text;
    const char *valid;
    int len;
} ind_t;
typedef struct
{
    int x0, y0, x1, y1, flags;
    union {
        char name[12];
        ind_t ind;
    } d;
} icon_t;

#define IF_TEXT   0x1
#define IF_BORDER 0x4
#define IF_HCENT  0x8
#define IF_VCENT  0x10
#define IF_FILLED 0x20
#define IF_INDIR  0x100
#define IF_CLICK  (3 << 12)
#define IF_COL(fg, bg) (((fg) << 24) | ((bg) << 28))

static struct
{
    int task, bar_icon, win, menu_open;
    int quit;
    re_engine *e;
    re_sound *snd;
    char file[256], title[256], time_text[64], play_text[8];
    int vis_w, vis_h;
    ro_box pic;
    int fill_x1;
    int shuttle;          /* J/L presses: the speed is +-2^(n-1) */
    int redo_picture;     /* stopped after the overlay: the picture made again for the window */
    int idle_cs;          /* from the engine: -1 nothing to do */
    unsigned poll_cs;     /* when the engine was last polled */
    unsigned last_ctl_cs, log_cs, slept_cs, log_slept;
    re_engine_stats log_st;
    int log_nulls;
    FILE *log;
    char lastmsg[200];
} V;

static char empty_text[] = "", start_text[] = "|\x8b", back_text[] = "\x8b", fwd_text[] = "\x8a",
            end_text[] = "\x8a|";

/* ---- the log -------------------------------------------------------------- */

static void lg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void lg(const char *fmt, ...)
{
    va_list ap;
    if (!V.log)
        return;
    fprintf(V.log, "%7.2f ", ro_now_cs() / 100.0);
    va_start(ap, fmt);
    vfprintf(V.log, fmt, ap);
    va_end(ap);
    fputc('\n', V.log);
    fflush(V.log);
}

static void lg_line(const char *line)
{
    lg("%s", line);
}

static void log_open(void)
{
    const char *path = getenv("ReelEdit$Log");
    if (path && !strcmp(path, "off"))
        return;
    V.log = fopen(path && *path ? path : "<Wimp$ScrapDir>.ReelEditLog", "w");
    lg(APP " %s", RE_VERSION);
}

/* ---- helpers ------------------------------------------------------------------ */

static void report(const char *text)
{
    struct
    {
        int num;
        char mess[252];
    } err;
    _kernel_swi_regs r;
    err.num = 0;
    snprintf(err.mess, sizeof(err.mess), "%s", text);
    r.r[0] = (intptr_t) &err;
    r.r[1] = 1 | 16; /* OK; no "Error from" */
    r.r[2] = (intptr_t) APP;
    lg("report: %s", text);
    re_display_hide_overlay();
    ro_swi(Wimp_ReportError, &r);
}

static void window_state(int w, int *block)
{
    _kernel_swi_regs r;
    block[0] = w;
    r.r[1] = (intptr_t) block;
    ro_swi(Wimp_GetWindowState, &r);
}

static void force_redraw(int x0, int y0, int x1, int y1)
{
    _kernel_swi_regs r;
    r.r[0] = V.win; r.r[1] = x0; r.r[2] = y0; r.r[3] = x1; r.r[4] = y1;
    ro_swi(Wimp_ForceRedraw, &r);
}

static void icon_refresh(int i)
{
    _kernel_swi_regs r;
    int b[4] = {V.win, i, 0, 0};
    r.r[1] = (intptr_t) b;
    ro_swi(Wimp_SetIconState, &r); /* no change: redraws it */
}

static void resize_icon(int i, int x0, int y0, int x1, int y1)
{
    _kernel_swi_regs r;
    r.r[0] = V.win; r.r[1] = i;
    r.r[2] = x0; r.r[3] = y0; r.r[4] = x1; r.r[5] = y1;
    ro_swi(Wimp_ResizeIcon, &r);
}

static void set_caret(void)
{
    _kernel_swi_regs r;
    r.r[0] = V.win; r.r[1] = -1; r.r[2] = 0; r.r[3] = 0;
    r.r[4] = 1 << 25; /* invisible */
    r.r[5] = -1;
    ro_swi(Wimp_SetCaretPosition, &r);
}

static int mode_var(int var)
{
    _kernel_swi_regs r;
    r.r[0] = -1;
    r.r[1] = var;
    return ro_swi(OS_ReadModeVariable, &r) ? 0 : r.r[2];
}

/* hh:mm:ss:ff */
static void timecode(char *buf, size_t n, int frame, double fps)
{
    int f = frame < 0 ? 0 : frame, rate = (int) (fps + 0.5);
    if (rate < 1)
        rate = 1;
    int s = f / rate;
    snprintf(buf, n, "%02d:%02d:%02d:%02d", s / 3600, s / 60 % 60, s % 60, f % rate);
}

/* ---- the window ------------------------------------------------------------------- */

static void icon_def(icon_t *ic, int flags, char *text, const char *valid, int len)
{
    memset(ic, 0, sizeof(*ic));
    ic->flags = flags;
    ic->d.ind.text = text;
    ic->d.ind.valid = valid;
    ic->d.ind.len = len;
}

static int create_window(void)
{
    struct
    {
        ro_box vis;
        int sx, sy, behind, flags;
        unsigned char tfg, tbg, wfg, wbg, sofg, sibg, tfocus, xflags;
        ro_box ext;
        int tflags, wbutton, sprites;
        short minw, minh;
        ind_t title;
        int nicons;
        icon_t icon[N_ICONS];
    } w;
    _kernel_swi_regs r;
    const int button = IF_TEXT | IF_BORDER | IF_HCENT | IF_VCENT | IF_FILLED | IF_INDIR | IF_CLICK | IF_COL(7, 1);

    memset(&w, 0, sizeof(w));
    w.vis.x1 = 800; w.vis.y1 = 600;
    w.behind = -1;
    w.flags = (int) 0xAF000002u; /* new format, back, close, title, toggle, adjust size, moveable */
    w.tfg = 7; w.tbg = 2; w.wfg = 7; w.wbg = 7; /* black: around the picture */
    w.sofg = 3; w.sibg = 1; w.tfocus = 12;
    w.ext.x0 = 0; w.ext.y0 = -8192; w.ext.x1 = 8192; w.ext.y1 = 0;
    w.tflags = IF_TEXT | IF_BORDER | IF_HCENT | IF_VCENT | IF_FILLED | IF_INDIR | IF_COL(7, 2);
    w.wbutton = 3 << 12; /* clicks: for the caret */
    w.sprites = 1;
    w.minw = MIN_W; w.minh = CH + 96;
    w.title.text = V.title;
    w.title.valid = (const char *) -1;
    w.title.len = sizeof(V.title);
    w.nicons = N_ICONS;
    icon_def(&w.icon[I_BAR], IF_FILLED | IF_INDIR | IF_TEXT | IF_COL(7, 1), empty_text, (const char *) -1, 1);
    icon_def(&w.icon[I_START], button, start_text, "R5,3", sizeof(start_text));
    icon_def(&w.icon[I_BACK], button, back_text, "R5,3", sizeof(back_text));
    icon_def(&w.icon[I_PLAY], button, V.play_text, "R5,3", sizeof(V.play_text));
    icon_def(&w.icon[I_FWD], button, fwd_text, "R5,3", sizeof(fwd_text));
    icon_def(&w.icon[I_END], button, end_text, "R5,3", sizeof(end_text));
    icon_def(&w.icon[I_TRACK], IF_BORDER | IF_FILLED | IF_INDIR | IF_TEXT | IF_CLICK | IF_COL(7, 0), empty_text,
             "R2", 1);
    icon_def(&w.icon[I_FILL], IF_FILLED | IF_INDIR | IF_TEXT | IF_CLICK | IF_COL(7, 8), empty_text,
             (const char *) -1, 1);
    icon_def(&w.icon[I_TIME], IF_TEXT | IF_HCENT | IF_VCENT | IF_FILLED | IF_INDIR | IF_COL(7, 1), V.time_text,
             (const char *) -1, sizeof(V.time_text));
    r.r[1] = (intptr_t) &w;
    if (ro_swi(Wimp_CreateWindow, &r))
        return -1;
    V.win = r.r[0];
    return 0;
}

static void track_x(int *x0, int *x1)
{
    *x0 = GAP * 6 + W_BTN * 4 + W_PLAY;
    *x1 = V.vis_w - GAP - W_TIME - GAP;
}

/* The controls for a visible size; the picture area above them. */
static void layout(int vw, int vh)
{
    int y0 = -vh + GAP, y1 = -vh + CH - GAP, x = GAP, tx0, tx1;
    V.vis_w = vw;
    V.vis_h = vh;
    V.pic.x0 = 0; V.pic.x1 = vw; V.pic.y1 = 0; V.pic.y0 = -vh + CH;
    resize_icon(I_BAR, 0, -vh, vw, -vh + CH);
    resize_icon(I_START, x, y0, x + W_BTN, y1); x += W_BTN + GAP;
    resize_icon(I_BACK, x, y0, x + W_BTN, y1); x += W_BTN + GAP;
    resize_icon(I_PLAY, x, y0, x + W_PLAY, y1); x += W_PLAY + GAP;
    resize_icon(I_FWD, x, y0, x + W_BTN, y1); x += W_BTN + GAP;
    resize_icon(I_END, x, y0, x + W_BTN, y1);
    track_x(&tx0, &tx1);
    resize_icon(I_TRACK, tx0, y0 + 12, tx1, y1 - 12);
    V.fill_x1 = tx0 + 4;
    resize_icon(I_FILL, tx0 + 4, y0 + 16, V.fill_x1, y1 - 16);
    resize_icon(I_TIME, vw - GAP - W_TIME, y0, vw - GAP, y1);
    re_display_place(V.win, V.pic);
}

static void update_controls(int force)
{
    char pos[24], len[24], text[64];
    int y0 = -V.vis_h + GAP, y1 = -V.vis_h + CH - GAP, tx0, tx1, fill;
    if (!V.e || !V.win)
        return;
    int p = re_engine_position(V.e), n = re_engine_length(V.e);
    double fps = re_engine_fps(V.e), sp = re_engine_speed(V.e);
    snprintf(V.play_text, sizeof(V.play_text), "%s", sp != 0 ? "Stop" : "Play");
    icon_refresh(I_PLAY);
    timecode(pos, sizeof(pos), p, fps);
    timecode(len, sizeof(len), n, fps);
    if (sp != 0 && sp != 1)
        snprintf(text, sizeof(text), "%s / %s %dx", pos, len, (int) sp);
    else
        snprintf(text, sizeof(text), "%s / %s", pos, len);
    if (force || strcmp(text, V.time_text)) {
        snprintf(V.time_text, sizeof(V.time_text), "%s", text);
        icon_refresh(I_TIME);
    }
    track_x(&tx0, &tx1);
    tx0 += 4;
    tx1 -= 4;
    fill = n > 1 ? tx0 + (int) ((double) (tx1 - tx0) * p / (n - 1)) : tx0;
    if (force || fill != V.fill_x1) {
        int old = V.fill_x1;
        V.fill_x1 = fill;
        resize_icon(I_FILL, tx0, y0 + 16, fill, y1 - 16);
        force_redraw((old < fill ? old : fill) - 4, y0, (old > fill ? old : fill) + 4, y1);
    }
}

static void open_window(int *b)
{
    _kernel_swi_regs r;
    int vw = b[3] - b[1], vh = b[4] - b[2];
    if (vw != V.vis_w || vh != V.vis_h) {
        layout(vw, vh);
        if (V.e && re_engine_speed(V.e) == 0)
            re_engine_seek(V.e, re_engine_position(V.e)); /* the picture for the new size */
        update_controls(1);
    }
    r.r[1] = (intptr_t) b;
    ro_swi(Wimp_OpenWindow, &r);
    re_display_check_covered();
}

/* ---- playing -------------------------------------------------------------------------- */

static unsigned clock_cs(void *ctx)
{
    (void) ctx;
    return ro_now_cs();
}

static void show(void *ctx, const re_picture *p)
{
    (void) ctx;
    re_display_show(p, re_engine_speed(V.e) != 0);
}

/* More sound queued than this: it has fallen behind the picture (the
   sound of pictures left out after a stall, given after the queue had run
   dry): thrown away, a short gap, and in step again. */
#define SOUND_BEHIND 0.25

static void sound(void *ctx, const int16_t *samples, int frames)
{
    (void) ctx;
    if (re_sound_queued(V.snd) > SOUND_BEHIND) {
        lg("sound %.2f s behind: cleared", re_sound_queued(V.snd));
        re_sound_clear(V.snd);
        re_sound_pause(V.snd, 0);
    }
    re_sound_add(V.snd, samples, frames);
}

static void set_speed(double speed)
{
    if (!V.e)
        return;
    double was = re_engine_speed(V.e);
    if (speed == was)
        return;
    re_sound_clear(V.snd);
    re_sound_pause(V.snd, speed != 1);
    if (speed != 0 && re_engine_position(V.e) >= re_engine_length(V.e) - 1 && speed > 0)
        re_engine_seek(V.e, 0); /* at the end: from the start */
    re_engine_play(V.e, speed);
    if (speed == 0) {
        V.shuttle = 0;
        if (re_display_hide_overlay())
            re_engine_seek(V.e, re_engine_position(V.e)); /* drawn in the window now */
    }
    lg("speed %g at frame %d", speed, re_engine_position(V.e));
    update_controls(1);
}

static void seek(int frame)
{
    if (!V.e)
        return;
    int n = re_engine_length(V.e);
    if (frame < 0) frame = 0;
    if (frame >= n) frame = n - 1;
    if (re_engine_speed(V.e) == 1)
        re_sound_clear(V.snd);
    re_engine_seek(V.e, frame);
    update_controls(1);
}

static void close_video(void)
{
    if (V.e) {
        lg("closing %s", V.file);
        re_engine_close(V.e);
        V.e = NULL;
    }
    re_sound_close(V.snd);
    V.snd = NULL;
    re_display_close();
    if (V.win) {
        _kernel_swi_regs r;
        r.r[1] = (intptr_t) &V.win;
        ro_swi(Wimp_CloseWindow, &r);
    }
    V.file[0] = 0;
}

static const char *leaf(const char *path)
{
    const char *l = strrchr(path, '.');
    return l ? l + 1 : path;
}

/* MLT wants a Unix-style name: it chooses the xml producer by ".mlt", and
   finds a project's clips from its directory, split at '/'. */
static const char *mlt_name(const char *path, char *buf, size_t size)
{
#ifdef __riscos__
    if (__unixify_std(path, buf, size, __RISCOSIFY_FILETYPE_NOTSPECIFIED))
        return buf;
#else
    (void) buf;
    (void) size;
#endif
    return path;
}

/* The preview's size against the project's: 1, or less for pictures over
   about 1920x1088 (a full 4K overlay, 3 buffers of 12 MB, ran the Pi's
   GPU short in Reel: the screen kept going black) */
static double preview_scale(int w, int h)
{
    double s = 1;
    while ((double) w * s * h * s > 1920.0 * 1088 * 1.1 || w * s > 2048 || h * s > 2048)
        s /= 2;
    return s;
}

static void open_file(const char *path)
{
    static re_engine_io io;
    char why[100], unix_name[512];
    const char *name = mlt_name(path, unix_name, sizeof(unix_name));
    close_video();
    lg("opening %s (%s)", path, name);
    V.snd = re_sound_open(SOUND_RATE, why, sizeof(why));
    if (!V.snd)
        lg("no sound: %s", why);
    re_sound_pause(V.snd, 1);
    io.show = show;
    io.sound = V.snd ? sound : NULL; /* (none: late pictures aren't made just for their sound) */
    io.clock_cs = clock_cs;
    io.sound_rate = SOUND_RATE;
    io.sound_channels = 2;
    V.e = re_engine_open(name, 1.0, &io);
    if (V.e) {
        double s = preview_scale(re_engine_width(V.e), re_engine_height(V.e));
        if (s < 1) { /* opened again at a size the overlay can take */
            lg("%dx%d: previewed at %g of that", re_engine_width(V.e), re_engine_height(V.e), s);
            re_engine_close(V.e);
            V.e = re_engine_open(name, s, &io);
        }
    }
    if (!V.e) {
        char text[400];
        re_sound_close(V.snd);
        V.snd = NULL;
        snprintf(text, sizeof(text), "%s can't open %s: is it a video, or an MLT project?", APP, leaf(path));
        report(text);
        return;
    }
    snprintf(V.file, sizeof(V.file), "%s", path);
    snprintf(V.title, sizeof(V.title), "%s", leaf(path));
    lg("%dx%d, %.3f fps, %d frames", re_engine_width(V.e), re_engine_height(V.e), re_engine_fps(V.e),
       re_engine_length(V.e));
    if (!V.win && create_window() < 0) {
        close_video();
        report("Can't make the window.");
        return;
    }
    /* at most half the screen's width, the picture's shape */
    int xeig = mode_var(4), yeig = mode_var(5);
    int sw = (mode_var(11) + 1) << xeig, sh = (mode_var(12) + 1) << yeig;
    int pw = re_engine_width(V.e) << xeig, ph = re_engine_height(V.e) << yeig;
    if (pw > sw / 2) {
        ph = (int) ((double) ph * (sw / 2) / pw);
        pw = sw / 2;
    }
    if (pw < MIN_W) {
        ph = (int) ((double) ph * MIN_W / pw);
        pw = MIN_W;
    }
    int b[9];
    b[0] = V.win;
    b[1] = (sw - pw) / 2; b[3] = b[1] + pw;
    b[4] = sh - (sh - ph - CH) / 2; b[2] = b[4] - ph - CH;
    b[5] = 0; b[6] = 0; b[7] = -1;
    V.vis_w = V.vis_h = 0;
    open_window(b);
    set_caret();
    re_engine_seek(V.e, 0);
    V.log_cs = ro_now_cs();
}

/* ---- events ----------------------------------------------------------------------------- */

static void redraw(int *b)
{
    _kernel_swi_regs r;
    r.r[1] = (intptr_t) b;
    if (ro_swi(Wimp_RedrawWindow, &r) || !r.r[0])
        return;
    re_display_redraw(b);
}

static void click(int *b)
{
    int buttons = b[2], w = b[3], i = b[4];
    if (w == -2) { /* the icon bar */
        if (buttons & 2) {
            static struct
            {
                char title[12];
                unsigned char tfg, tbg, wfg, wbg;
                int w, h, gap;
                struct
                {
                    int flags, sub, iflags;
                    char text[12];
                } item[2];
            } m = {"Reel Edit", 7, 2, 7, 0, 160, 44, 0,
                   {{0, -1, 0x07000021, "Info"}, {0x80, -1, 0x07000021, "Quit"}}};
            _kernel_swi_regs r;
            r.r[1] = (intptr_t) &m;
            r.r[2] = b[0] - 64;
            r.r[3] = 96 + 44 * 2;
            ro_swi(Wimp_CreateMenu, &r);
            V.menu_open = 1;
            re_display_check_covered();
        } else if (V.e && V.win) {
            int st[9];
            window_state(V.win, st);
            st[7] = -1; /* to the front */
            open_window(st);
            set_caret();
        } else {
            report("Drop a video, or an MLT project (from Shotcut or Kdenlive), on the icon to view it.");
        }
        return;
    }
    if (w != V.win || !V.e)
        return;
    set_caret();
    int pos = re_engine_position(V.e), fps1 = (int) (re_engine_fps(V.e) + 0.5);
    switch (i) {
    case I_START: seek(0); break;
    case I_BACK: seek(pos - ((buttons & 1) ? fps1 : 1)); break; /* Adjust: a second */
    case I_PLAY: set_speed(re_engine_speed(V.e) != 0 ? 0 : 1); break;
    case I_FWD: seek(pos + ((buttons & 1) ? fps1 : 1)); break;
    case I_END: seek(re_engine_length(V.e) - 1); break;
    case I_TRACK:
    case I_FILL: {
        int st[9], tx0, tx1;
        window_state(V.win, st);
        track_x(&tx0, &tx1);
        int x = b[0] - (st[1] - st[5]);
        double f = (double) (x - tx0 - 4) / (tx1 - tx0 - 8);
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        seek((int) (f * (re_engine_length(V.e) - 1) + 0.5));
        break;
    }
    }
}

static void menu_select(int *b)
{
    if (b[0] == 0) {
        char text[200];
        snprintf(text, sizeof(text), APP " %s: a video editor for RISC OS, on the MLT framework. "
                 "This test build has the Viewer only.", RE_VERSION);
        report(text);
    } else if (b[0] == 1) {
        V.quit = 1;
    }
    V.menu_open = 0;
}

static void key(int *b)
{
    _kernel_swi_regs r;
    int k = b[6];
    if (V.e && b[0] == V.win) {
        int pos = re_engine_position(V.e), fps1 = (int) (re_engine_fps(V.e) + 0.5);
        double sp = re_engine_speed(V.e);
        switch (k) {
        case ' ': set_speed(sp != 0 ? 0 : 1); return;
        case 'k': case 'K': case 27: set_speed(0); return;
        case 'l': case 'L':
            /* (playing already at 1 counts as the first press) */
            V.shuttle = sp > 0 ? (V.shuttle < 1 ? 2 : V.shuttle < 3 ? V.shuttle + 1 : 3) : 1;
            set_speed(1 << (V.shuttle - 1));
            return;
        case 'j': case 'J':
            V.shuttle = sp < 0 ? (V.shuttle < 1 ? 2 : V.shuttle < 3 ? V.shuttle + 1 : 3) : 1;
            set_speed(-(1 << (V.shuttle - 1)));
            return;
        case 0x18C: seek(pos - 1); return;        /* Left */
        case 0x18D: seek(pos + 1); return;        /* Right */
        case 0x19C: seek(pos - fps1); return;     /* Shift-Left */
        case 0x19D: seek(pos + fps1); return;     /* Shift-Right */
        case 0x1E: seek(0); return;               /* Home */
        case 0x18B: seek(re_engine_length(V.e) - 1); return; /* Copy (End) */
        }
    }
    r.r[0] = k;
    ro_swi(Wimp_ProcessKey, &r);
}

static void ack(int *b)
{
    _kernel_swi_regs r;
    int to = b[1];
    b[3] = b[2];
    b[4] = MSG_DATALOADACK;
    r.r[0] = 17;
    r.r[1] = (intptr_t) b;
    r.r[2] = to;
    ro_swi(Wimp_SendMessage, &r);
}

static void message(int *b)
{
    switch (b[4]) {
    case MSG_QUIT:
        V.quit = 1;
        break;
    case MSG_DATALOAD:
        if ((b[5] == -2 && b[6] == V.bar_icon) || (V.win && b[5] == V.win)) {
            char file[256];
            if (b[10] == 0x1000 || b[10] == 0x2000) {
                report("That's a directory: drop a video or an MLT project.");
                break;
            }
            snprintf(file, sizeof(file), "%s", (const char *) &b[11]);
            ack(b);
            open_file(file);
        }
        break;
    case MSG_MODECHANGE:
        lg("mode change");
        re_display_mode_changed();
        if (V.e && V.win) {
            int st[9];
            window_state(V.win, st);
            V.vis_w = V.vis_h = 0;
            open_window(st); /* (stopped, this makes the picture again) */
        }
        break;
    }
}

/* A null event: the engine's turn */
static void tick(void)
{
    unsigned t = ro_now_cs();
    V.log_nulls++;
    V.idle_cs = re_engine_poll(V.e);
    V.poll_cs = ro_now_cs(); /* (the wait is from now: after the picture was made) */
    if (re_engine_speed(V.e) == 0) {
        if (re_display_hide_overlay()) /* stopped by itself (the end): drawn in the window */
            re_engine_seek(V.e, re_engine_position(V.e));
        update_controls(0);
    } else if (t - V.last_ctl_cs >= 20) {
        V.last_ctl_cs = t;
        update_controls(0);
        /* a window or menu opened over the picture: the next picture (within
           a frame) is drawn in the window; not made again now, which would
           give its sound twice */
        re_display_check_covered();
    }
    if (t - V.log_cs >= 100 && re_engine_speed(V.e) != 0) {
        re_engine_stats st;
        re_display_stats ds;
        re_engine_get_stats(V.e, &st);
        re_display_get_stats(&ds);
        lg("frame %d: %d pictures, %d left out, %d resyncs in %.2f s; %d nulls, asleep %u%%; overlay %d, window %d, "
           "overlay waited %d, skipped %d; sound queued %.2f s, %u refused",
           re_engine_position(V.e), st.rendered - V.log_st.rendered, st.skipped - V.log_st.skipped,
           st.resyncs - V.log_st.resyncs, (t - V.log_cs) / 100.0, V.log_nulls,
           (V.slept_cs - V.log_slept) * 100 / (t - V.log_cs), ds.overlay_pictures, ds.sprite_pictures,
           ds.overlay_waited, ds.overlay_skipped, re_sound_queued(V.snd), re_sound_refused(V.snd));
        V.log_st = st;
        V.log_cs = t;
        V.log_nulls = 0;
        V.log_slept = V.slept_cs;
    }
}

static void iconbar_icon(void)
{
    struct
    {
        int w, x0, y0, x1, y1, flags;
        char name[12];
    } b;
    _kernel_swi_regs r;
    memset(&b, 0, sizeof(b));
    b.w = -1;
    b.x1 = 68; b.y1 = 68;
    b.flags = 0x301A;
    strcpy(b.name, "!reeledit");
    r.r[0] = 0;
    r.r[1] = (intptr_t) &b;
    ro_swi(Wimp_CreateIcon, &r);
    V.bar_icon = r.r[0];
}

#ifdef RE_TEST
/* for tests/host/viewer_test.c */
int re_test_position(void) { return V.e ? re_engine_position(V.e) : -1; }
double re_test_speed(void) { return V.e ? re_engine_speed(V.e) : 0; }
int re_test_open(void) { return V.e != NULL; }
#endif

int reeledit_main(int argc, char **argv)
{
    static const int messages[] = {MSG_DATALOAD, MSG_PREQUIT, MSG_MODECHANGE, 0};
    int block[64];
    _kernel_swi_regs r;
    (void) argc;
    (void) argv;

    memset(&V, 0, sizeof(V));
    snprintf(V.play_text, sizeof(V.play_text), "Play");
    log_open();
    re_display_set_log(lg_line);
    r.r[0] = 380;
    r.r[1] = 0x4B534154; /* "TASK" */
    r.r[2] = (intptr_t) APP;
    r.r[3] = (intptr_t) messages;
    if (ro_swi(Wimp_Initialise, &r))
        return 1;
    V.task = r.r[1];
    re_display_init(V.task);
    iconbar_icon();
    V.idle_cs = -1;

    while (!V.quit) {
        /* Null events only while the engine has work: a seek at once,
           playing at the next picture's time; else none at all. */
        int ev, busy = V.e && re_engine_busy(V.e);
        unsigned before = ro_now_cs();
        r.r[0] = busy ? 0 : 1;
        r.r[1] = (intptr_t) block;
        if (busy && re_engine_speed(V.e) != 0 && V.idle_cs > 0) {
            r.r[2] = (int) (V.poll_cs + V.idle_cs);
            if (ro_swi(Wimp_PollIdle, &r))
                break;
        } else if (ro_swi(Wimp_Poll, &r)) {
            break;
        }
        ev = r.r[0];
        if (ev == 0)
            V.slept_cs += ro_now_cs() - before;
        switch (ev) {
        case 0:
            if (V.e)
                tick();
            break;
        case 1: redraw(block); break;
        case 2:
            if (block[0] == V.win)
                open_window(block);
            break;
        case 3:
            if (block[0] == V.win)
                close_video();
            break;
        case 6: click(block); break;
        case 8: key(block); break;
        case 9: menu_select(block); break;
        case 17:
        case 18: message(block); break;
        }
    }
    close_video();
    lg("quit");
    r.r[0] = V.task;
    r.r[1] = 0x4B534154;
    ro_swi(Wimp_CloseDown, &r);
    if (V.log)
        fclose(V.log);
    return 0;
}
