/* viewer.c - Reel Edit's desktop front end (app.h): the task, the Viewer
 * window and the editor's menus, keys and messages.
 *
 * Select on the icon bar icon opens the editor: the Viewer (the picture,
 * the transport and a toolbar) with the Timeline under it, on a new
 * project or the one open. Clips are dragged from directory displays to
 * the Timeline (where they're dropped), the Viewer or the icon (at the
 * end); an MLT project dropped is opened. Everything else is on the
 * toolbar, the menu (Menu over either window) and the keys:
 *   Space play/stop; J K L backwards, stop, forwards (again: x2, x4);
 *   Left/Right a frame (Shift: a second); Home, Copy the start, the end;
 *   S split at the playhead; Delete the selected clip, the rest closing up
 *   (Shift-Delete: leaving a gap); + and - zoom; ^Z clear the selection;
 *   F3 save; Escape stop.
 *
 * Keeping the desktop running (docs/DESIGN.md): pictures are made by
 * src/engine.c on null events, at most one each time, asked for with
 * Wimp_PollIdle at the next picture's time; stopped, null events are
 * masked out. Playing, pictures go through the hardware overlay when
 * nothing is over them; stopped, they're drawn in the window (display.c).
 * The sound goes to SharedSoundBuffer (sound.c) and is the clock.
 *
 * Log: <Wimp$ScrapDir>.ReelEditLog (ReelEdit$Log names another file, or
 * "off"), started afresh each time.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "app.h"
#include "../measure.h"
#include "display.h"
#include "sound.h"

#include <stdarg.h>
#include <stdio.h>
#ifdef __riscos__
#include <unixlib/local.h>
#endif
#include <stdlib.h>
#include <string.h>

#ifndef RE_VERSION
#define RE_VERSION "0.2"
#endif

/* the heap in a dynamic area (UnixLib adds more areas when it needs them) */
const char *const __dynamic_da_name = "ReelEdit Heap";
int __dynamic_da_max_size = 128 << 20;

#define MSG_QUIT        0
#define MSG_DATASAVEACK 2
#define MSG_DATALOAD    3
#define MSG_DATALOADACK 4
#define MSG_PREQUIT     8
#define MSG_MODECHANGE  0x400C1
#define MSG_HELPREQUEST 0x502
#define MSG_HELPREPLY   0x503

#define SOUND_RATE 48000

/* the Viewer's rows of controls, OS units */
#define CH     64  /* a row's height */
#define GAP    4
#define W_BTN  72
#define W_PLAY 112
#define W_TIME 360
#define W_TOOL 136
#define MIN_W  1400 /* (the toolbar's width) */

enum { I_BAR, I_START, I_BACK, I_PLAY, I_FWD, I_END, I_TRACK, I_FILL, I_TIME,
       I_SPLIT, I_DELETE, I_ZOOMOUT, I_ZOOMIN, I_SAVE, I_UNDO, I_REDO, I_SNAP, I_ZLABEL, I_FIT, N_ICONS };

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

re_app A = {0, NULL, NULL, "", -1, 1, 1};

static struct
{
    int bar_icon, win, quit;
    re_sound *snd;
    char title[300], time_text[64], play_text[8], snap_text[16];
    int vis_w, vis_h;
    ro_box pic;
    int fill_x1;
    int shuttle;          /* J/L presses: the speed is +-2^(n-1) */
    int idle_cs;          /* from the engine: -1 nothing to do */
    unsigned poll_cs;     /* when the engine was last polled */
    unsigned last_ctl_cs, log_cs, slept_cs, log_slept;
    re_engine_stats log_st;
    int log_nulls;
    char log_decoder[32];
    unsigned changes;     /* the project's, last seen */
    FILE *log;
    /* A clip that doesn't say how long it is, being measured (null events),
       and the files dropped meanwhile, to be loaded after it */
    re_measure *ms;
    char ms_path[256], ms_name[512];
    int ms_frame;
    unsigned ms_cs;
    struct { char path[256]; int filetype, frame; } queue[16];
    int queued;
} V;

/* (&88 and &89: the left and right arrows of RISC OS's character set) */
static char empty_text[] = "", start_text[] = "|\x88", back_text[] = "\x88", fwd_text[] = "\x89",
            end_text[] = "\x89|", split_text[] = "Split", delete_text[] = "Delete", zoomout_text[] = "-",
            zoomin_text[] = "+", undo_text[] = "Undo", redo_text[] = "Redo", zlabel_text[] = "Timeline zoom",
            fit_text[] = "Fit", save_text[] = "Save";

/* ---- the log, reports ---------------------------------------------------------- */

void app_log(const char *fmt, ...)
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
    app_log("%s", line);
}

static void log_open(void)
{
    const char *path = getenv("ReelEdit$Log");
    if (path && !strcmp(path, "off"))
        return;
    V.log = fopen(path && *path ? path : "<Wimp$ScrapDir>.ReelEditLog", "w");
    app_log(APP " %s", RE_VERSION);
}

/* An error box: buttons (NULL: OK). Returns Wimp_ReportError's R1: 1 OK,
   3 the first of the buttons, 4 the second ... */
static int ask(const char *text, const char *buttons)
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
    r.r[1] = buttons ? (16 | (1 << 8) | (4 << 9)) : (1 | 16); /* (a question: new style, no OK) */
    r.r[2] = (intptr_t) APP;
    r.r[3] = (intptr_t) "!reeledit";
    r.r[4] = 1;
    r.r[5] = (intptr_t) buttons;
    app_log("%s: %s", buttons ? "ask" : "report", text);
    re_display_hide_overlay();
    ro_swi(Wimp_ReportError, &r);
    return (int) r.r[1];
}

void app_report(const char *text)
{
    ask(text, NULL);
}

static int mode_var(int var)
{
    _kernel_swi_regs r;
    r.r[0] = -1;
    r.r[1] = var;
    return ro_swi(OS_ReadModeVariable, &r) ? 0 : (int) r.r[2];
}

static int shift_down(void)
{
    _kernel_swi_regs r;
    r.r[0] = 121; /* OS_Byte 121: is Shift (key 0) down? */
    r.r[1] = 0 ^ 0x80;
    return !ro_swi(OS_Byte, &r) && (r.r[1] & 0xFF) == 0xFF;
}

void app_timecode(char *buf, int size, int frame)
{
    double fps = A.proj ? re_project_fps(A.proj) : 25;
    int f = frame < 0 ? 0 : frame, rate = (int) (fps + 0.5);
    if (rate < 1)
        rate = 1;
    int s = f / rate;
    snprintf(buf, size, "%02d:%02d:%02d:%02d", s / 3600, s / 60 % 60, s % 60, f % rate);
}

/* MLT wants a Unix-style name: it chooses the xml producer by ".mlt", and
   finds a project's clips from its directory, split at '/'. */
const char *app_mlt_name(const char *path, char *buf, int size)
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

static const char *leaf(const char *path)
{
    const char *l = strrchr(path, '.');
    return l ? l + 1 : path;
}

/* ---- the Viewer window ------------------------------------------------------------- */

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

static void set_caret(int w)
{
    _kernel_swi_regs r;
    r.r[0] = w; r.r[1] = -1; r.r[2] = 0; r.r[3] = 0;
    r.r[4] = 1 << 25; /* invisible */
    r.r[5] = -1;
    ro_swi(Wimp_SetCaretPosition, &r);
}

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
    w.minw = MIN_W; w.minh = 2 * CH + 96;
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
    icon_def(&w.icon[I_SPLIT], button, split_text, "R5,3", sizeof(split_text));
    icon_def(&w.icon[I_DELETE], button, delete_text, "R5,3", sizeof(delete_text));
    icon_def(&w.icon[I_ZOOMOUT], button, zoomout_text, "R5,3", sizeof(zoomout_text));
    icon_def(&w.icon[I_ZOOMIN], button, zoomin_text, "R5,3", sizeof(zoomin_text));
    icon_def(&w.icon[I_SAVE], button, save_text, "R5,3", sizeof(save_text));
    icon_def(&w.icon[I_UNDO], button, undo_text, "R5,3", sizeof(undo_text));
    icon_def(&w.icon[I_REDO], button, redo_text, "R5,3", sizeof(redo_text));
    icon_def(&w.icon[I_SNAP], button, V.snap_text, "R5,3", sizeof(V.snap_text));
    /* (filled, in the bar's grey: unfilled, the Wimp anti-aliases the text
       against the window's background, black here, and it looks ragged) */
    icon_def(&w.icon[I_ZLABEL], IF_TEXT | IF_VCENT | IF_FILLED | IF_INDIR | 0x200 | IF_COL(7, 1),
             zlabel_text, (const char *) -1, sizeof(zlabel_text));
    icon_def(&w.icon[I_FIT], button, fit_text, "R5,3", sizeof(fit_text));
    r.r[1] = (intptr_t) &w;
    if (ro_swi(Wimp_CreateWindow, &r))
        return -1;
    V.win = (int) r.r[0];
    return 0;
}

static void track_x(int *x0, int *x1)
{
    *x0 = GAP * 6 + W_BTN * 4 + W_PLAY;
    *x1 = V.vis_w - GAP - W_TIME - GAP;
}

/* The two rows of controls for a visible size; the picture above them. */
static void layout(int vw, int vh)
{
    int y0 = -vh + CH + GAP, y1 = -vh + 2 * CH - GAP, x = GAP, tx0, tx1;
    V.vis_w = vw;
    V.vis_h = vh;
    V.pic.x0 = 0; V.pic.x1 = vw; V.pic.y1 = 0; V.pic.y0 = -vh + 2 * CH;
    resize_icon(I_BAR, 0, -vh, vw, -vh + 2 * CH);
    /* the transport */
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
    /* the toolbar */
    y0 = -vh + GAP;
    y1 = -vh + CH - GAP;
    x = GAP;
    /* edit tools | Timeline zoom - + Fit | ... Save */
    static const struct { int icon, w, gap_after; } tools[] = {
        {I_SPLIT, W_TOOL, 0}, {I_DELETE, W_TOOL, 0}, {I_UNDO, W_TOOL, 0}, {I_REDO, W_TOOL, 0},
        {I_SNAP, W_TOOL + 24, 24}, {I_ZLABEL, 200, 8}, {I_ZOOMOUT, 64, 0}, {I_ZOOMIN, 64, 0}, {I_FIT, 80, 0}};
    for (unsigned i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        resize_icon(tools[i].icon, x, y0, x + tools[i].w, y1);
        x += tools[i].w + GAP + tools[i].gap_after;
    }
    resize_icon(I_SAVE, vw - GAP - W_TOOL, y0, vw - GAP, y1);
    re_display_place(V.win, V.pic);
}

static void update_title(void)
{
    char t[sizeof(V.title)];
    _kernel_swi_regs r;
    snprintf(t, sizeof(t), "%s%s", A.path[0] ? A.path : "<Untitled>",
             A.proj && re_project_modified(A.proj) ? " *" : "");
    if (!strcmp(t, V.title) || !V.win)
        return;
    snprintf(V.title, sizeof(V.title), "%s", t);
    r.r[0] = V.win;
    r.r[1] = 0x4B534154; /* "TASK": R2 says what */
    r.r[2] = 3;          /* the title bar */
    ro_swi(Wimp_ForceRedraw, &r);
}

static void update_controls(int force)
{
    char pos[24], len[24], text[64];
    int y0 = -V.vis_h + CH + GAP, y1 = -V.vis_h + 2 * CH - GAP, tx0, tx1, fill;
    if (!V.win)
        return;
    int p = A.e ? re_engine_position(A.e) : 0, n = A.e ? re_engine_length(A.e) : 0;
    double sp = A.e ? re_engine_speed(A.e) : 0;
    const char *snap = A.snap ? "Snap: on" : "Snap: off";
    if (force || strcmp(snap, V.snap_text)) {
        snprintf(V.snap_text, sizeof(V.snap_text), "%s", snap);
        icon_refresh(I_SNAP);
    }
    const char *play = sp != 0 ? "Stop" : "Play";
    if (force || strcmp(play, V.play_text)) {
        snprintf(V.play_text, sizeof(V.play_text), "%s", play);
        icon_refresh(I_PLAY);
    }
    app_timecode(pos, sizeof(pos), p);
    app_timecode(len, sizeof(len), n);
    if (V.ms)
        snprintf(text, sizeof(text), "Measuring: %d%%", (int) (re_measure_progress(V.ms) * 100));
    else if (sp != 0 && sp != 1)
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
    tl_playhead(p);
}

#define Wimp_GetWindowOutline 0x400E0

static void open_window(int *b);

/* The Timeline attached: kept under the Viewer, as wide, just behind it
   in the window stack */
static void place_timeline(void)
{
    int vs[9], vo[5], ts[9], to[5];
    _kernel_swi_regs r;
    if (!A.attached || !tl_window() || !V.win)
        return;
    window_state(V.win, vs);
    window_state(tl_window(), ts);
    if (!(vs[8] & (1 << 16)) || !(ts[8] & (1 << 16)))
        return;
    vo[0] = V.win;
    r.r[1] = (intptr_t) vo;
    ro_swi(Wimp_GetWindowOutline, &r);
    to[0] = tl_window();
    r.r[1] = (intptr_t) to;
    ro_swi(Wimp_GetWindowOutline, &r);
    int top = to[4] - ts[4], h = ts[4] - ts[2]; /* (its title bar, and its height) */
    int b[9] = {tl_window(), vs[1], vo[2] - top - h, vs[3], vo[2] - top, ts[5], ts[6], V.win, 0};
    if (b[1] == ts[1] && b[2] == ts[2] && b[3] == ts[3] && b[4] == ts[4] && ts[7] == V.win)
        return;
    tl_open_request(b);
}

/* The Timeline moved or resized by the user while attached: the Viewer
   comes with it (the same width, its own height, just above) */
static void timeline_moved(int *b)
{
    int vs[9], vo[5], to[5];
    _kernel_swi_regs r;
    tl_open_request(b);
    if (!A.attached || !V.win)
        return;
    window_state(V.win, vs);
    vo[0] = V.win;
    r.r[1] = (intptr_t) vo;
    ro_swi(Wimp_GetWindowOutline, &r);
    to[0] = tl_window();
    r.r[1] = (intptr_t) to;
    ro_swi(Wimp_GetWindowOutline, &r);
    int ts[9];
    window_state(tl_window(), ts);
    int bottom = vs[2] - vo[2];          /* the Viewer's border below its work area */
    int y0 = to[4] + bottom, h = vs[4] - vs[2];
    int nb[9] = {V.win, ts[1], y0, ts[3], y0 + h, vs[5], vs[6], b[7] == V.win ? -1 : b[7], 0};
    if (nb[1] != vs[1] || nb[2] != vs[2] || nb[3] != vs[3] || nb[4] != vs[4] || b[7] != ts[7])
        open_window(nb);
    place_timeline();
}

static void open_window(int *b)
{
    _kernel_swi_regs r;
    int vw = b[3] - b[1], vh = b[4] - b[2], resized = 0;
    if (vw != V.vis_w || vh != V.vis_h) {
        resized = 1;
        layout(vw, vh);
        if (A.e && re_engine_speed(A.e) == 0)
            re_engine_seek(A.e, re_engine_position(A.e)); /* the picture for the new size */
        update_controls(1);
    }
    r.r[1] = (intptr_t) b;
    ro_swi(Wimp_OpenWindow, &r);
    /* (the Wimp may have opened it smaller than asked: laid out as it is) */
    int st[9];
    window_state(V.win, st);
    if (st[3] - st[1] != V.vis_w || st[4] - st[2] != V.vis_h) {
        layout(st[3] - st[1], st[4] - st[2]);
        update_controls(1);
        resized = 1;
    }
    /* Resized: all of it drawn again. The Wimp keeps what was on the screen
       where it can, but the controls moved with the bottom edge, and the
       picture (or the overlay's see-through area) was left where they are
       now: black. */
    if (resized)
        force_redraw(0, -V.vis_h, V.vis_w, 0);
    re_display_check_covered();
    place_timeline();
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
    re_display_show(p, re_engine_speed(A.e) != 0);
}

/* More sound queued than this (the engine keeps 0.4 s): something has gone
   wrong; thrown away, a short gap, and in step again. */
#define SOUND_BEHIND 1.0

static double sound_queued(void *ctx)
{
    (void) ctx;
    return re_sound_queued(V.snd);
}

static void sound(void *ctx, const int16_t *samples, int frames)
{
    (void) ctx;
    if (re_sound_queued(V.snd) > SOUND_BEHIND) {
        app_log("sound %.2f s behind: cleared", re_sound_queued(V.snd));
        re_sound_clear(V.snd);
        re_sound_pause(V.snd, 0);
    }
    re_sound_add(V.snd, samples, frames);
}

int app_position(void)
{
    return A.e ? re_engine_position(A.e) : 0;
}

void app_set_speed(double speed)
{
    if (!A.e)
        return;
    double was = re_engine_speed(A.e);
    if (speed == was)
        return;
    re_sound_clear(V.snd);
    re_sound_pause(V.snd, speed != 1);
    if (speed > 0 && re_engine_position(A.e) >= re_engine_length(A.e) - 1)
        re_engine_seek(A.e, 0); /* at the end: from the start */
    re_engine_play(A.e, speed);
    if (speed == 0) {
        V.shuttle = 0;
        if (re_display_hide_overlay())
            re_engine_seek(A.e, re_engine_position(A.e)); /* drawn in the window now */
    }
    app_log("speed %g at frame %d", speed, re_engine_position(A.e));
    update_controls(1);
}

void app_seek(int frame)
{
    if (!A.e)
        return;
    int n = re_engine_length(A.e);
    if (frame >= n) frame = n - 1;
    if (frame < 0) frame = 0;
    if (re_engine_speed(A.e) == 1)
        re_sound_clear(V.snd);
    re_engine_seek(A.e, frame);
    update_controls(1);
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

static void engine_close(void)
{
    if (A.e) {
        re_engine_close(A.e);
        A.e = NULL;
    }
    re_sound_close(V.snd);
    V.snd = NULL;
    re_display_close();
    if (V.win)
        force_redraw(V.pic.x0, V.pic.y0, V.pic.x1, V.pic.y1); /* black */
}

/* The engine on the project, made once it has something in it */
static void engine_open(void)
{
    static re_engine_io io;
    char why[100];
    mlt_profile pr = re_project_profile(A.proj);
    V.snd = re_sound_open(SOUND_RATE, why, sizeof(why));
    if (!V.snd)
        app_log("no sound: %s", why);
    re_sound_pause(V.snd, 1);
    io.show = show;
    io.sound = V.snd ? sound : NULL;
    io.clock_cs = clock_cs;
    io.sound_queued = V.snd ? sound_queued : NULL; /* the sound is the clock */
    io.sound_rate = SOUND_RATE;
    io.sound_channels = 2;
    double s = preview_scale(pr->width, pr->height);
    A.e = re_engine_open_producer(pr, re_project_producer(A.proj), s, &io);
    if (!A.e) {
        re_sound_close(V.snd);
        V.snd = NULL;
        app_log("the engine couldn't open the project");
        return;
    }
    if (A.e)
        app_log("project %dx%d, %.3f fps; previewed at %dx%d", pr->width, pr->height, re_project_fps(A.proj),
                re_engine_width(A.e), re_engine_height(A.e));
    V.log_cs = ro_now_cs();
    memset(&V.log_st, 0, sizeof(V.log_st));
    if (V.win) /* (re_display_close forgot the window: an undo, or a clip into an emptied project) */
        re_display_place(V.win, V.pic);
}

void app_edited(void)
{
    if (!A.proj)
        return;
    V.changes = re_project_changes(A.proj);
    if (re_project_length(A.proj) <= 0) {
        engine_close();
    } else if (!A.e) {
        engine_open();
    } else {
        re_engine_refresh(A.e);
    }
    if (A.sel >= re_project_items(A.proj))
        A.sel = -1;
    tl_edited();
    info_edited();
    update_title();
    update_controls(1);
}

void app_saved(const char *path)
{
    snprintf(A.path, sizeof(A.path), "%s", path);
    update_title();
}

/* ---- the editor: opening and closing ------------------------------------------------------ */

static void open_editor(void)
{
    if (!A.proj) {
        A.proj = re_project_new();
        A.path[0] = 0;
        A.sel = -1;
        if (!A.proj) {
            app_report("MLT couldn't start.");
            return;
        }
    }
    if (!V.win && create_window() < 0) {
        app_report("Can't make the window.");
        return;
    }
    /* the Viewer: half the screen's width, 16:9 (the project's shape once
       it has one); the Timeline under it */
    int xeig = mode_var(4), yeig = mode_var(5);
    int sw = (mode_var(11) + 1) << xeig, sh = (mode_var(12) + 1) << yeig;
    mlt_profile pr = re_project_profile(A.proj);
    double aspect = re_project_producer(A.proj) && pr->width > 0 ? (double) pr->height / pr->width : 9.0 / 16;
    int pw = sw / 2, ph = (int) (pw * aspect);
    if (pw < MIN_W)
        pw = MIN_W;
    int th = 48 + 96 + 16 + 40 + 40; /* the Timeline, its title and scroll bar */
    int b[9];
    b[0] = V.win;
    b[1] = (sw - pw) / 2; b[3] = b[1] + pw;
    b[4] = sh - 64; b[2] = b[4] - ph - 2 * CH;
    if (b[2] - th < 140) { /* (a small screen: the picture smaller) */
        b[2] = 140 + th;
    }
    b[5] = 0; b[6] = 0; b[7] = -1;
    V.vis_w = V.vis_h = 0;
    update_title();
    open_window(b);
    tl_open(b[1], b[2] - 40, pw);
    place_timeline();
    set_caret(V.win);
    if (A.e)
        re_engine_seek(A.e, re_engine_position(A.e));
    update_controls(1);
}

/* Closes the project and its windows. 0 if the user wants to keep it. */
static void measure_cancel(void);

static int close_editor(void)
{
    if (A.proj && re_project_modified(A.proj)) {
        int r = ask("This project has changes that haven't been saved. Discard them?", "Discard,Cancel");
        if (r != 3)
            return 0;
    }
    measure_cancel();
    engine_close();
    if (V.win) {
        _kernel_swi_regs r;
        r.r[1] = (intptr_t) &V.win;
        ro_swi(Wimp_CloseWindow, &r);
    }
    tl_close();
    info_close();
    sb_forget();
    re_project_close(A.proj);
    A.proj = NULL;
    A.path[0] = 0;
    A.sel = -1;
    app_log("editor closed");
    return 1;
}

static int is_project(const char *path, int filetype)
{
    size_t n = strlen(path);
    return filetype == FILETYPE_MLT || (n > 4 && (!strcmp(path + n - 4, "/mlt") || !strcmp(path + n - 4, ".mlt")));
}

static void measure_cancel(void)
{
    if (V.ms)
        app_log("measuring %s: cancelled", V.ms_path);
    re_measure_close(V.ms);
    V.ms = NULL;
    V.queued = 0;
    update_controls(1);
}

/* A clip in the project: selected, shown */
static void added(int at)
{
    A.sel = at;
    int first = !A.e;
    app_edited();
    if (first)
        open_editor(); /* (the Viewer the project's shape now) */
    re_item it;
    re_project_item(A.proj, at, &it);
    app_seek(it.start);
}

/* Measuring: a few packets each null event, for up to 4 cs; then the clip
   added, and what was dropped meanwhile loaded */
static void measure_tick(void)
{
    unsigned t0 = ro_now_cs();
    int r, n = 0;
    do
        r = re_measure_step(V.ms, 8);
    while (r == 0 && ++n < 64 && ro_now_cs() - t0 < 4);
    if (ro_now_cs() - V.ms_cs >= 25 || r != 0) {
        V.ms_cs = ro_now_cs();
        update_controls(0);
    }
    if (r == 0)
        return;
    double sec = re_measure_seconds(V.ms);
    re_measure_close(V.ms);
    V.ms = NULL;
    int frames = A.proj ? re_project_frames(A.proj, sec) : 0;
    app_log("measured %s: %.3f s, %d frames", V.ms_path, sec, frames);
    int at = r > 0 && frames > 0 ? re_project_add_length(A.proj, V.ms_name, V.ms_frame, frames) : -1;
    if (at < 0) {
        char t[300];
        snprintf(t, sizeof(t), "Reel Edit couldn't find how long %s is.", leaf(V.ms_path));
        app_report(t);
        update_controls(1);
    } else {
        added(at);
    }
    while (V.queued > 0 && !V.ms) { /* (each may start another measuring) */
        char path[256];
        int ft = V.queue[0].filetype, fr = V.queue[0].frame;
        snprintf(path, sizeof(path), "%s", V.queue[0].path);
        V.queued--;
        memmove(&V.queue[0], &V.queue[1], sizeof(V.queue[0]) * (size_t) V.queued);
        app_load(path, ft, fr);
    }
}

void app_load(const char *path, int filetype, int frame)
{
    char unix_name[512];
    if (V.ms) { /* (after the clip being measured) */
        if (V.queued < (int) (sizeof(V.queue) / sizeof(V.queue[0]))) {
            snprintf(V.queue[V.queued].path, sizeof(V.queue[0].path), "%s", path);
            V.queue[V.queued].filetype = filetype;
            V.queue[V.queued].frame = frame;
            V.queued++;
        }
        return;
    }
    const char *name = app_mlt_name(path, unix_name, sizeof(unix_name));
    if (is_project(path, filetype)) {
        if (A.proj && !close_editor())
            return;
        app_log("opening the project %s (%s)", path, name);
        A.proj = re_project_open(name, 1);
        if (!A.proj) {
            char t[300];
            snprintf(t, sizeof(t), "%s isn't an MLT project Reel Edit can open.", leaf(path));
            app_report(t);
            return;
        }
        snprintf(A.path, sizeof(A.path), "%s", path);
        A.sel = -1;
        open_editor();
        app_edited();
        return;
    }
    if (!A.proj || !V.win)
        open_editor();
    if (!A.proj)
        return;
    app_log("adding %s (%s) at %d", path, name, frame);
    int at = re_project_add(A.proj, name, frame);
    if (at == RE_ADD_MEASURE) { /* (raw MPEG-2 and the like: read through for the length) */
        V.ms = re_measure_open(name);
        if (V.ms) {
            app_log("measuring %s", path);
            snprintf(V.ms_path, sizeof(V.ms_path), "%s", path);
            snprintf(V.ms_name, sizeof(V.ms_name), "%s", name);
            V.ms_frame = frame;
            V.ms_cs = ro_now_cs();
            update_controls(1);
            return;
        }
    }
    if (at < 0) {
        char t[300];
        snprintf(t, sizeof(t), "Reel Edit can't open %s: is it a video?", leaf(path));
        app_report(t);
        return;
    }
    added(at);
}

/* ---- editing ------------------------------------------------------------------------------------ */

/* Undo and redo: the project is made again, so the engine with it */
static void undo(int redo)
{
    if (!A.proj)
        return;
    int pos = app_position();
    app_set_speed(0);
    engine_close();
    int done = redo ? re_project_redo(A.proj) : re_project_undo(A.proj);
    app_log("%s: %s", redo ? "redo" : "undo", done ? "done" : "nothing");
    A.sel = -1;
    app_edited();
    app_seek(pos);
}

/* Q and W: the clip at the playhead trimmed to it, its start or its end */
static void trim(int end)
{
    if (!A.proj)
        return;
    int pos = app_position(), i = re_project_item_at(A.proj, pos);
    re_item it;
    if (i < 0 || re_project_item(A.proj, i, &it))
        return;
    if (re_project_trim(A.proj, pos, end)) {
        app_edited();
        app_seek(end ? pos : it.start); /* (Q: the clip now starts where it did, at the frame that was here) */
    }
}

static void to_cut(int dir)
{
    if (!A.proj)
        return;
    int f = re_project_cut(A.proj, app_position(), dir);
    if (f >= 0)
        app_seek(f >= re_project_length(A.proj) ? re_project_length(A.proj) - 1 : f);
}

static void snap_toggle(void)
{
    A.snap = !A.snap;
    update_controls(1);
}

static void split(void)
{
    if (A.proj && re_project_split(A.proj, app_position()))
        app_edited();
}

static void delete_selected(int ripple)
{
    if (!A.proj || A.sel < 0)
        return;
    re_item it;
    re_project_item(A.proj, A.sel, &it);
    if (re_project_remove(A.proj, A.sel, ripple)) {
        A.sel = -1;
        app_edited();
        app_seek(it.start);
    }
}

static void save(int x, int y)
{
    if (A.proj && A.path[0] && re_project_producer(A.proj)) {
        char unix_name[512];
        if (re_project_save(A.proj, app_mlt_name(A.path, unix_name, sizeof(unix_name))) == 0) {
            _kernel_swi_regs r;
            r.r[0] = 18; /* OS_File 18: the filetype */
            r.r[1] = (intptr_t) A.path;
            r.r[2] = FILETYPE_MLT;
            ro_swi(0x08, &r);
            app_log("saved as %s", A.path);
            update_title();
            return;
        }
        char t[300];
        snprintf(t, sizeof(t), "The project couldn't be saved as %.250s.", A.path);
        app_report(t);
    }
    sb_open(x, y);
}

/* ---- the menu ------------------------------------------------------------------------------------ */

/* Menus: indirected items (the texts longer than 12 characters) */
typedef struct
{
    int flags;          /* bit 0 ticked, bit 7 the last */
    intptr_t sub;       /* a submenu, or -1 */
    int iflags;
    const char *text;
    const char *valid;
    int len;
} menu_item;
#define MENU(n)                                                                                                    \
    struct                                                                                                         \
    {                                                                                                              \
        char title[12];                                                                                            \
        unsigned char tfg, tbg, wfg, wbg;                                                                          \
        int w, h, gap;                                                                                             \
        menu_item item[n];                                                                                         \
    }
#define ITEM_FLAGS 0x07000121 /* text, filled, indirected, black on white */
#define SHADED     (1 << 22)
#define IT(t) ITEM_FLAGS, t, (const char *) -1, sizeof(t)

static MENU(3) m_file = {"File", 7, 2, 7, 0, 280, 44, 0,
                         {{0, -1, IT("New")}, {0, -1, IT("Save          F3")}, {0x80, -1, IT("Save as...")}}};
static MENU(8) m_edit = {"Edit", 7, 2, 7, 0, 520, 44, 0,
                         {{0, -1, IT("Undo                     F8")}, {2, -1, IT("Redo                     F9")},
                          {0, -1, IT("Split at the playhead     S")}, {0, -1, IT("Trim the start to it      Q")},
                          {2, -1, IT("Trim the end to it        W")}, {0, -1, IT("Delete               Delete")},
                          {0, -1, IT("Delete, leaving a gap   \x8b" "Del")}, {0x80, -1, IT("Clear the selection      ^Z")}}};
static char play_item[24] = "Play           Space";
static MENU(3) m_play = {"Play", 7, 2, 7, 0, 340, 44, 0,
                         {{0, -1, ITEM_FLAGS, play_item, (const char *) -1, sizeof(play_item)},
                          {0, -1, IT("To the start   Home")}, {0x80, -1, IT("To the end     Copy")}}};
static MENU(5) m_view = {"View", 7, 2, 7, 0, 520, 44, 0,
                         {{0, -1, IT("Zoom the Timeline in       +")}, {0, -1, IT("Zoom the Timeline out      -")},
                          {2, -1, IT("The whole project     \x8bZ")}, {0, -1, IT("Attach the Timeline")},
                          {0x80, -1, IT("Snapping                   N")}}};
static MENU(5) m_main = {"Reel Edit", 7, 2, 7, 0, 340, 44, 0,
                         {{0, 0, IT("File")}, {0, 0, IT("Edit")}, {0, 0, IT("Play")}, {0, 0, IT("View")},
                          {0x80, -1, IT("Video information...")}}};
static MENU(2) m_bar = {"Reel Edit", 7, 2, 7, 0, 160, 44, 0,
                        {{0, -1, IT("Info")}, {0x80, -1, IT("Quit")}}};
static int menu_is; /* 0 none, 1 the icon bar's, 2 the editor's */
static int menu_x, menu_y;

static void shade(int *iflags, int off)
{
    *iflags = off ? (*iflags | SHADED) : (*iflags & ~SHADED);
}

void app_menu(int x, int y)
{
    _kernel_swi_regs r;
    m_main.item[0].sub = (intptr_t) &m_file;
    m_main.item[1].sub = (intptr_t) &m_edit;
    m_main.item[2].sub = (intptr_t) &m_play;
    m_main.item[3].sub = (intptr_t) &m_view;
    int has = A.proj && re_project_length(A.proj) > 0;
    shade(&m_file.item[1].iflags, !has);
    shade(&m_file.item[2].iflags, !has);
    shade(&m_edit.item[0].iflags, !A.proj || !re_project_can_undo(A.proj));
    shade(&m_edit.item[1].iflags, !A.proj || !re_project_can_redo(A.proj));
    for (int i = 2; i <= 4; i++)
        shade(&m_edit.item[i].iflags, !has);
    for (int i = 5; i <= 7; i++)
        shade(&m_edit.item[i].iflags, A.sel < 0);
    m_view.item[4].flags = (m_view.item[4].flags & ~1) | (A.snap ? 1 : 0);
    for (int i = 0; i < 3; i++)
        shade(&m_play.item[i].iflags, !has);
    snprintf(play_item, sizeof(play_item), "%s", A.e && re_engine_speed(A.e) != 0 ? "Stop           Space"
                                                                                : "Play           Space");
    for (int i = 0; i < 3; i++)
        shade(&m_view.item[i].iflags, !has);
    m_view.item[3].flags = (m_view.item[3].flags & ~1) | (A.attached ? 1 : 0); /* ticked */
    shade(&m_main.item[4].iflags, !has);
    menu_is = 2;
    menu_x = x;
    menu_y = y;
    r.r[1] = (intptr_t) &m_main;
    r.r[2] = x - 64;
    r.r[3] = y;
    ro_swi(Wimp_CreateMenu, &r);
    re_display_check_covered();
}

static void menu_select(int *b)
{
    _kernel_swi_regs r;
    int p[5];
    if (menu_is == 1) {
        if (b[0] == 0) {
            char text[200];
            snprintf(text, sizeof(text), APP " %s: a video editor for RISC OS, on the MLT framework.", RE_VERSION);
            app_report(text);
        } else if (b[0] == 1) {
            if (close_editor())
                V.quit = 1;
        }
        return;
    }
    switch (b[0]) {
    case 0: /* File */
        if (b[1] == 0) {
            if (close_editor())
                open_editor();
        } else if (b[1] == 1) {
            save(menu_x, menu_y);
        } else if (b[1] == 2) {
            sb_open(menu_x, menu_y);
        }
        break;
    case 1: /* Edit */
        switch (b[1]) {
        case 0: undo(0); break;
        case 1: undo(1); break;
        case 2: split(); break;
        case 3: trim(0); break;
        case 4: trim(1); break;
        case 5: delete_selected(1); break;
        case 6: delete_selected(0); break;
        case 7: tl_select(-1); break;
        }
        break;
    case 2: /* Play */
        if (b[1] == 0) app_set_speed(A.e && re_engine_speed(A.e) != 0 ? 0 : 1);
        else if (b[1] == 1) app_seek(0);
        else if (b[1] == 2 && A.e) app_seek(re_engine_length(A.e) - 1);
        break;
    case 3: /* View */
        if (b[1] >= 0 && b[1] <= 2)
            tl_zoom(b[1] == 0 ? 1 : b[1] == 1 ? 0 : -1);
        else if (b[1] == 3) {
            A.attached = !A.attached;
            app_log("Timeline %s", A.attached ? "attached" : "free");
            place_timeline();
        } else if (b[1] == 4) {
            snap_toggle();
        }
        break;
    case 4: /* Video information */
        info_open();
        break;
    }
    /* Adjust: the menu stays open */
    r.r[1] = (intptr_t) p;
    if (!ro_swi(Wimp_GetPointerInfo, &r) && (p[2] & 1) && A.proj && b[0] >= 0 && b[0] != 4 && !(b[0] == 0 && b[1] >= 0))
        app_menu(menu_x, menu_y);
}

/* ---- keys and clicks ------------------------------------------------------------------------------- */

int app_key(int k)
{
    if (!A.proj)
        return 0;
    int pos = app_position(), fps1 = (int) (re_project_fps(A.proj) + 0.5);
    double sp = A.e ? re_engine_speed(A.e) : 0;
    switch (k) {
    case ' ': app_set_speed(sp != 0 ? 0 : 1); return 1;
    case 27: /* Escape: stop (and stop measuring) */
        if (V.ms)
            measure_cancel();
        app_set_speed(0);
        return 1;
    case 'k': case 'K': app_set_speed(0); return 1;
    case 'l': case 'L':
        /* (playing already at 1 counts as the first press) */
        V.shuttle = sp > 0 ? (V.shuttle < 1 ? 2 : V.shuttle < 3 ? V.shuttle + 1 : 3) : 1;
        app_set_speed(1 << (V.shuttle - 1));
        return 1;
    case 'j': case 'J':
        V.shuttle = sp < 0 ? (V.shuttle < 1 ? 2 : V.shuttle < 3 ? V.shuttle + 1 : 3) : 1;
        app_set_speed(-(1 << (V.shuttle - 1)));
        return 1;
    case 0x18C: app_seek(pos - 1); return 1;      /* Left */
    case 0x18D: app_seek(pos + 1); return 1;      /* Right */
    case 0x19C: app_seek(pos - fps1); return 1;   /* Shift-Left */
    case 0x19D: app_seek(pos + fps1); return 1;   /* Shift-Right */
    case 0x1E: app_seek(0); return 1;             /* Home */
    case 0x18B: if (A.e) app_seek(re_engine_length(A.e) - 1); return 1; /* Copy (End) */
    case 's': case 'S': split(); return 1;
    case 0x7F: case 8: delete_selected(!shift_down()); return 1; /* Delete, Backspace */
    case '+': case '=': tl_zoom(1); return 1;
    case '-': case '_': tl_zoom(0); return 1;
    case 0x1A: tl_select(-1); return 1;           /* ^Z: clear the selection */
    case 0x188: undo(0); return 1;                /* F8 */
    case 0x189: undo(1); return 1;                /* F9 */
    case 0x18F: to_cut(-1); return 1;             /* Up: the cut before */
    case 0x18E: to_cut(1); return 1;              /* Down: the cut after */
    case 'q': case 'Q': trim(0); return 1;
    case 'w': case 'W': trim(1); return 1;
    case 'n': case 'N': snap_toggle(); return 1;
    case 'Z': case '\\': tl_zoom(-1); return 1;    /* Shift-Z, \\: the whole project */
    case 0x183: {                                  /* F3: save */
        int p[5];
        _kernel_swi_regs r;
        r.r[1] = (intptr_t) p;
        ro_swi(Wimp_GetPointerInfo, &r);
        save(p[0], p[1]);
        return 1;
    }
    }
    return 0;
}

static void key(int *b)
{
    _kernel_swi_regs r;
    if (sb_window() && b[0] == sb_window()) {
        sb_key(b);
        return;
    }
    if ((b[0] == V.win || b[0] == tl_window()) && app_key(b[6]))
        return;
    r.r[0] = b[6];
    ro_swi(Wimp_ProcessKey, &r);
}

static void redraw(int *b)
{
    _kernel_swi_regs r;
    if (b[0] == tl_window()) {
        tl_redraw(b);
        return;
    }
    if (info_window() && b[0] == info_window()) {
        info_redraw(b);
        return;
    }
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
            _kernel_swi_regs r;
            menu_is = 1;
            r.r[1] = (intptr_t) &m_bar;
            r.r[2] = b[0] - 64;
            r.r[3] = 96 + 44 * 2;
            ro_swi(Wimp_CreateMenu, &r);
            re_display_check_covered();
        } else {
            open_editor();
        }
        return;
    }
    if (sb_window() && w == sb_window()) {
        sb_click(b);
        return;
    }
    if (w == tl_window()) {
        tl_click(b);
        return;
    }
    if (w != V.win)
        return;
    if (buttons & 2) {
        app_menu(b[0], b[1]);
        return;
    }
    set_caret(V.win);
    int pos = app_position(), fps1 = A.proj ? (int) (re_project_fps(A.proj) + 0.5) : 25;
    switch (i) {
    case I_START: app_seek(0); break;
    case I_BACK: app_seek(pos - ((buttons & 1) ? fps1 : 1)); break; /* Adjust: a second */
    case I_PLAY: app_set_speed(A.e && re_engine_speed(A.e) != 0 ? 0 : 1); break;
    case I_FWD: app_seek(pos + ((buttons & 1) ? fps1 : 1)); break;
    case I_END: if (A.e) app_seek(re_engine_length(A.e) - 1); break;
    case I_SPLIT: split(); break;
    case I_DELETE: delete_selected(!(buttons & 1)); break; /* Adjust: leaving a gap */
    case I_ZOOMOUT: tl_zoom(0); break;
    case I_ZOOMIN: tl_zoom(1); break;
    case I_FIT: tl_zoom(-1); break;
    case I_UNDO: undo(0); break;
    case I_REDO: undo(1); break;
    case I_SNAP: snap_toggle(); break;
    case I_SAVE: save(b[0], b[1]); break;
    case I_TRACK:
    case I_FILL: {
        int st[9], tx0, tx1;
        if (!A.e)
            break;
        window_state(V.win, st);
        track_x(&tx0, &tx1);
        int x = b[0] - (st[1] - st[5]);
        double f = (double) (x - tx0 - 4) / (tx1 - tx0 - 8);
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        app_seek((int) (f * (re_engine_length(A.e) - 1) + 0.5));
        break;
    }
    }
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

/* Interactive help: what's under the pointer */
static const char *help_for(int win, int icon)
{
    if (win == tl_window())
        return "This is the Timeline.|MClick the ruler or drag the playhead to move it; click a clip to select it "
               "(Adjust: select or not).|MCtrl and the scroll wheel zoom; N turns snapping to cuts on or off.";
    if (win == -2)
        return "This is the Reel Edit icon.|MClick Select to open the editor; drop video files on it to add them "
               "to the project, or an MLT project to open it.";
    if (win == info_window())
        return "This window shows the clip's and the project's video information.";
    if (win != V.win)
        return NULL;
    switch (icon) {
    case I_SPLIT: return "Click to split the clip at the playhead (S).";
    case I_DELETE: return "Click to delete the selected clip, closing the gap (Delete).|MAdjust leaves the gap.";
    case I_UNDO: return "Click to undo the last edit (F8).";
    case I_REDO: return "Click to redo the edit undone (F9).";
    case I_SNAP: return "Click to turn snapping on or off (N): the playhead snaps to the cuts.";
    case I_ZOOMOUT: return "Click to zoom the Timeline out (-).";
    case I_ZOOMIN: return "Click to zoom the Timeline in (+).";
    case I_FIT: return "Click to show the whole project in the Timeline (Shift-Z).";
    case I_SAVE: return "Click to save the project (F3).";
    case I_ZLABEL: return "The buttons beside this zoom the Timeline, not the picture.";
    default:
        return "This is the Viewer: the frame at the playhead.|MSpace plays and stops; J K L play backwards, "
               "stop and forwards; Left and Right move a frame; Up and Down go to the cuts.";
    }
}

static void message(int *b, int event)
{
    _kernel_swi_regs r;
    if (sb_message(b))
        return;
    switch (b[4]) {
    case MSG_QUIT:
        V.quit = 1;
        break;
    case MSG_PREQUIT:
        if (A.proj && re_project_modified(A.proj)) {
            /* object first (acknowledge), then ask */
            int to = b[1];
            b[3] = b[2];
            r.r[0] = 19;
            r.r[1] = (intptr_t) b;
            r.r[2] = to;
            ro_swi(Wimp_SendMessage, &r);
            if (ask("Reel Edit's project has changes that haven't been saved. Discard them and quit?",
                    "Discard,Cancel")
                == 3) {
                engine_close(); /* (the engine first: it plays the project) */
                re_project_close(A.proj);
                A.proj = NULL;
                V.quit = 1;
                /* a desktop shutdown (not just this task told to quit): start
                   it again, with Ctrl-Shift-F12 to the task that began it */
                if (b[0] < 24 || !(b[5] & 1)) {
                    int k[7];
                    r.r[1] = (intptr_t) k;
                    ro_swi(Wimp_GetCaretPosition, &r);
                    k[6] = 0x1FC;
                    r.r[0] = 8; /* Key_Pressed */
                    r.r[1] = (intptr_t) k;
                    r.r[2] = to;
                    ro_swi(Wimp_SendMessage, &r);
                }
            }
        }
        break;
    case MSG_DATALOAD: {
        int w = b[5];
        if (!((w == -2 && b[6] == V.bar_icon) || (V.win && w == V.win) || (tl_window() && w == tl_window())))
            break;
        if (b[3] && event == 18) /* (a reply to our own save: not a drop) */
            break;
        char file[256];
        if (b[10] == 0x1000 || b[10] == 0x2000) {
            app_report("That's a directory: drag a video or an MLT project.");
            break;
        }
        snprintf(file, sizeof(file), "%s", (const char *) &b[11]);
        int type = b[10];
        int frame = tl_window() && w == tl_window() ? tl_frame_at(b[7]) : -1;
        ack(b);
        app_load(file, type, frame);
        break;
    }
    case MSG_HELPREQUEST: {
        const char *h = help_for(b[8], b[9]);
        if (!h)
            break;
        int reply[64];
        int n = (int) strlen(h) + 1;
        if (n > 236)
            n = 236;
        memcpy(&reply[5], h, (size_t) n);
        ((char *) &reply[5])[n - 1] = 0;
        reply[0] = (20 + n + 3) & ~3;
        reply[3] = b[2];
        reply[4] = MSG_HELPREPLY;
        r.r[0] = 17;
        r.r[1] = (intptr_t) reply;
        r.r[2] = b[1];
        ro_swi(Wimp_SendMessage, &r);
        break;
    }
    case MSG_MODECHANGE:
        app_log("mode change");
        re_display_mode_changed();
        if (V.win && A.proj) {
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
    V.idle_cs = re_engine_poll(A.e);
    V.poll_cs = ro_now_cs(); /* (the wait is from now: after the picture was made) */
    if (re_engine_speed(A.e) == 0) {
        if (re_display_hide_overlay()) /* stopped by itself (the end): drawn in the window */
            re_engine_seek(A.e, re_engine_position(A.e));
        update_controls(0);
    } else if (t - V.last_ctl_cs >= 20) {
        V.last_ctl_cs = t;
        update_controls(0);
        /* a window or menu opened over the picture: the next picture (within
           a frame) is drawn in the window */
        re_display_check_covered();
    }
    if (t - V.log_cs >= 100 && re_engine_speed(A.e) != 0) {
        re_engine_stats st;
        re_display_stats ds;
        re_engine_get_stats(A.e, &st);
        re_display_get_stats(&ds);
        const char *dec = A.proj ? re_project_decoder(A.proj, re_engine_position(A.e)) : NULL;
        if (dec && strcmp(dec, V.log_decoder)) { /* (the clip on show, decoded by another) */
            snprintf(V.log_decoder, sizeof(V.log_decoder), "%s", dec);
            app_log("decoder: %s", dec);
        }
        app_log("frame %d: %d pictures, %d left out, %d slowed, %d resyncs in %.2f s; %d nulls, asleep %u%%; overlay "
                "%d, window %d, overlay waited %d, skipped %d; sound queued %.2f s, %u refused",
                re_engine_position(A.e), st.rendered - V.log_st.rendered, st.skipped - V.log_st.skipped,
                st.slowed - V.log_st.slowed, st.resyncs - V.log_st.resyncs, (t - V.log_cs) / 100.0, V.log_nulls,
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
    V.bar_icon = (int) r.r[0];
}

#ifdef RE_TEST
/* for tests/host/viewer_test.c */
int re_test_position(void) { return A.e ? re_engine_position(A.e) : -1; }
double re_test_speed(void) { return A.e ? re_engine_speed(A.e) : 0; }
int re_test_open(void) { return A.e != NULL; }
re_project *re_test_project(void) { return A.proj; }
int re_test_selected(void) { return A.sel; }
int re_test_snap(void) { return A.snap; }
int re_test_measuring(void) { return V.ms != NULL; }
int re_test_busy(void) { return A.e && re_engine_busy(A.e); }
const char *re_test_title(void) { return V.title; }
#endif

int reeledit_main(int argc, char **argv)
{
    static const int messages[] = {MSG_DATASAVEACK, MSG_DATALOAD, MSG_PREQUIT, MSG_MODECHANGE, MSG_HELPREQUEST, 0};
    int block[64];
    _kernel_swi_regs r;
    (void) argc;
    (void) argv;

    memset(&V, 0, sizeof(V));
    snprintf(V.play_text, sizeof(V.play_text), "Play");
    log_open();
    re_display_set_log(lg_line);
    if (V.log)
        re_engine_messages(lg_line);
    r.r[0] = 380;
    r.r[1] = 0x4B534154; /* "TASK" */
    r.r[2] = (intptr_t) APP;
    r.r[3] = (intptr_t) messages;
    if (ro_swi(Wimp_Initialise, &r))
        return 1;
    A.task = (int) r.r[1];
    re_display_init(A.task);
    iconbar_icon();
    V.idle_cs = -1;

    while (!V.quit) {
        /* Null events only while the engine has work: a seek at once,
           playing at the next picture's time; else none at all. */
        if (A.proj && re_project_changes(A.proj) != V.changes)
            app_edited();
        int ev, busy = (A.e && re_engine_busy(A.e)) || tl_scrubbing() || V.ms; /* (scrubbing: the pointer followed) */
        unsigned before = ro_now_cs();
        r.r[0] = busy ? 0 : 1;
        r.r[1] = (intptr_t) block;
        if (busy && re_engine_speed(A.e) != 0 && V.idle_cs > 0) {
            r.r[2] = (int) (V.poll_cs + V.idle_cs);
            if (ro_swi(Wimp_PollIdle, &r))
                break;
        } else if (ro_swi(Wimp_Poll, &r)) {
            break;
        }
        ev = (int) r.r[0];
        if (ev == 0)
            V.slept_cs += ro_now_cs() - before;
        switch (ev) {
        case 0:
            if (tl_scrubbing())
                tl_scrub_tick();
            if (V.ms)
                measure_tick();
            if (A.e)
                tick();
            break;
        case 1: redraw(block); break;
        case 2:
            if (block[0] == V.win)
                open_window(block);
            else if (block[0] == tl_window())
                timeline_moved(block);
            else {
                r.r[1] = (intptr_t) block;
                ro_swi(Wimp_OpenWindow, &r);
            }
            break;
        case 3:
            if (block[0] == V.win || block[0] == tl_window())
                close_editor();
            else {
                r.r[1] = (intptr_t) block;
                ro_swi(Wimp_CloseWindow, &r);
            }
            break;
        case 6: click(block); break;
        case 7:
            if (tl_scrubbing())
                tl_drag_end(block);
            else
                sb_drag_end(block);
            break;
        case 8: key(block); break;
        case 9: menu_select(block); break;
        case 10:
            if (block[0] == tl_window())
                tl_scroll(block);
            break;
        case 17:
        case 18: message(block, ev); break;
        }
    }
    engine_close();
    if (A.proj) {
        re_project_close(A.proj);
        A.proj = NULL;
    }
    app_log("quit");
    r.r[0] = A.task;
    r.r[1] = 0x4B534154;
    ro_swi(Wimp_CloseDown, &r);
    if (V.log)
        fclose(V.log);
    return 0;
}
