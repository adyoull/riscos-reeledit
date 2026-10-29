/* exportbox.c - the Export window (app.h): the project made into a video
 * file (MP4: H.264 and AAC), in the background while editing carries on.
 *
 * File > Export... (Ctrl-E) opens it:
 *   the file icon, to drag to a directory display (or a full name typed
 *   and Export clicked); Size (the project's, 1080, 720, 480 or 360
 *   lines); Quality (Small file, Normal, High); VideoCore encoder (the
 *   Pi's: much faster; shaded without VCHIQ); a line saying how it's going;
 *   Export, which becomes Stop while it runs.
 *
 * The project is saved to a scrap file (a copy: it stays as modified as it
 * was), and this program run again on it with -export (export.h) in a
 * TaskWindow of its own: RISC OS multitasks a TaskWindow pre-emptively, so
 * editing and playing go on. Its output comes here as TaskWindow_Output
 * messages ("progress DONE TOTAL" lines, then "done" or "failed: why");
 * TaskWindow_Morio says it has ended. Stop sends TaskWindow_Morite (killed)
 * and deletes what it had written.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "../export.h"
#include "app.h"
#include "menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define Wimp_StartTask     0x400DE
#define OS_File            0x08
#define FILETYPE_MP4       0xBF6
#define MSG_TW_OUTPUT      0x808C1
#define MSG_TW_EGO         0x808C2
#define MSG_TW_MORIO       0x808C3
#define MSG_TW_MORITE      0x808C4
#define TXT_ID             0x52454558 /* ("REEX": this program's TaskWindow) */

enum { I_FILE, I_NAME, I_SIZE_L, I_SIZE, I_QUALITY_L, I_QUALITY, I_HW, I_STATUS, I_GO, I_N };
enum { M_SIZE, M_QUALITY };

#define WIDTH  760
#define HEIGHT 380

static const int HEIGHTS[5] = {0, 1080, 720, 480, 360};
static const char *const SIZE_NAMES[5] = {"The project's", "1080 lines", "720 lines", "480 lines", "360 lines"};
static const char *const QUALITY_NAMES[3] = {"Small file", "Normal", "High"};

static struct
{
    int win;
    char name[256];       /* where it goes (a leaf until dragged, or a full name typed) */
    char sprite[16];
    int size, quality, hw, can_hw;
    int menu;
    char shown[2][24];
    char status[120];
    char go[8];
    /* the export running */
    int running, child;   /* (the TaskWindow's task, once it says so) */
    char out[256];        /* the file being written (RISC OS name) */
    char line[256];       /* output not yet ended by a newline */
    int line_len, result; /* 0 running, 1 done, -1 failed */
    int done, total;
} X = {0};

int ex_window(void)
{
    return X.win;
}

int ex_running(void)
{
    return X.running;
}

static void redraw_icons(void)
{
    _kernel_swi_regs r;
    int b[4];
    if (!X.win)
        return;
    for (int i = 0; i < I_N; i++) {
        b[0] = X.win; b[1] = i; b[2] = 0; b[3] = 0;
        r.r[1] = (intptr_t) b;
        ro_swi(Wimp_SetIconState, &r);
    }
}

static void show(void)
{
    snprintf(X.shown[M_SIZE], 24, "%s", SIZE_NAMES[X.size]);
    snprintf(X.shown[M_QUALITY], 24, "%s", QUALITY_NAMES[X.quality]);
    snprintf(X.go, sizeof(X.go), "%s", X.running ? "Stop" : "Export");
    redraw_icons();
    if (X.win) { /* the option's tick, and shaded without VCHIQ */
        _kernel_swi_regs r;
        int b[4] = {X.win, I_HW, (X.hw ? 1 << 21 : 0) | (X.can_hw ? 0 : 1 << 22), (1 << 21) | (1 << 22)};
        r.r[1] = (intptr_t) b;
        ro_swi(Wimp_SetIconState, &r);
    }
}

static void say(const char *fmt, const char *arg)
{
    snprintf(X.status, sizeof(X.status), fmt, arg ? arg : "");
    show();
}

static int create(void)
{
    typedef struct
    {
        char *text;
        const char *valid;
        int len;
    } ind_t;
    typedef struct
    {
        int x0, y0, x1, y1, flags;
        ind_t ind;
    } icon_t;
    struct
    {
        ro_box vis;
        int sx, sy, behind, flags;
        unsigned char tfg, tbg, wfg, wbg, sofg, sibg, tfocus, xflags;
        ro_box ext;
        int tflags, wbutton, sprites;
        short minw, minh;
        char title[12];
        int nicons;
        icon_t icon[I_N];
    } w;
    static char size_l[] = "Size", quality_l[] = "Quality", hw_text[] = "VideoCore encoder";
    _kernel_swi_regs r;
    memset(&w, 0, sizeof(w));
    w.vis.x1 = WIDTH; w.vis.y1 = HEIGHT;
    w.behind = -1;
    w.flags = (int) 0x86000012u; /* new format, moveable, title, close */
    w.tfg = 7; w.tbg = 2; w.wfg = 7; w.wbg = 1; w.sofg = 3; w.sibg = 1; w.tfocus = 12;
    w.ext.x0 = 0; w.ext.y0 = -HEIGHT; w.ext.x1 = WIDTH; w.ext.y1 = 0;
    w.tflags = 0x1 | 0x8 | 0x10;
    w.sprites = 1;
    memcpy(w.title, "Export", 7);
    w.nicons = I_N;
    const int LABEL = 0x1 | 0x10 | 0x100 | (7 << 24) | (1 << 28);
    const int WRITE = 0x1 | 0x4 | 0x10 | 0x20 | 0x100 | (15 << 12) | (7 << 24) | (0 << 28);
    const int BUTTON = 0x1 | 0x4 | 0x8 | 0x10 | 0x20 | 0x100 | (3 << 12) | (7 << 24) | (1 << 28);
    const int OPTION = 0x1 | 0x2 | 0x10 | 0x100 | (11 << 12) | (7 << 24) | (1 << 28); /* (text and sprite: option) */
    snprintf(X.sprite, sizeof(X.sprite), "file_%03x", FILETYPE_MP4);
    r.r[0] = 24; /* (the Wimp's sprite for MP4 files: if there's none, a plain file's) */
    r.r[2] = (intptr_t) X.sprite;
    if (ro_swi(0x400E9, &r)) /* Wimp_SpriteOp */
        snprintf(X.sprite, sizeof(X.sprite), "file_xxx");
    icon_t *ic = w.icon;
    ic[I_FILE] = (icon_t){WIDTH / 2 - 64, -124, WIDTH / 2 + 64, -20,
                          0x2 | 0x8 | 0x10 | (6 << 12) | (7 << 24) | (1 << 28), {0}};
    memcpy(&ic[I_FILE].ind, X.sprite, strlen(X.sprite) + 1);
    ic[I_NAME] = (icon_t){16, -188, WIDTH - 16, -136, WRITE, {X.name, "Pptr_write;Ktar", sizeof(X.name)}};
    ic[I_SIZE_L] = (icon_t){16, -252, 116, -200, LABEL, {size_l, (const char *) -1, sizeof(size_l)}};
    ic[I_SIZE] = (icon_t){120, -252, 370, -200, BUTTON, {X.shown[M_SIZE], "R5,3", 24}};
    ic[I_QUALITY_L] = (icon_t){390, -252, 500, -200, LABEL, {quality_l, (const char *) -1, sizeof(quality_l)}};
    ic[I_QUALITY] = (icon_t){504, -252, WIDTH - 16, -200, BUTTON, {X.shown[M_QUALITY], "R5,3", 24}};
    ic[I_HW] = (icon_t){16, -304, 380, -260, OPTION, {hw_text, "Soptoff,opton", sizeof(hw_text)}};
    ic[I_STATUS] = (icon_t){16, -364, WIDTH - 200, -312, LABEL, {X.status, (const char *) -1, sizeof(X.status)}};
    ic[I_GO] = (icon_t){WIDTH - 184, -372, WIDTH - 16, -300, BUTTON, {X.go, "R6,3", sizeof(X.go)}};
    r.r[1] = (intptr_t) &w;
    if (ro_swi(Wimp_CreateWindow, &r))
        return -1;
    X.win = (int) r.r[0];
    return 0;
}

/* Does the VideoCore's encoder have its way in (VCHIQ)? */
static int vchiq_there(void)
{
    _kernel_swi_regs r;
    r.r[1] = (intptr_t) "VCHIQ_MsgQueue";
    return !ro_swi(OS_SWINumberFromString, &r);
}

void ex_open(void)
{
    _kernel_swi_regs r;
    if (!A.proj || re_project_length(A.proj) <= 0) {
        app_report("There's nothing to export yet: drag a clip from a directory display to the Timeline first.");
        return;
    }
    if (!X.win) {
        X.can_hw = vchiq_there();
        X.hw = X.can_hw;
        X.quality = RE_EXPORT_NORMAL;
        if (create() < 0)
            return;
    }
    if (!X.running) {
        /* (the project's leaf, as a video's: "Holiday/mlt" gives "Holiday/mp4") */
        const char *leaf = A.path[0] ? A.path : "Video";
#ifdef __riscos__
        const char *sep = strrchr(leaf, '.'), *colon = strrchr(leaf, ':');
        if (colon && (!sep || colon > sep))
            sep = colon;
#else
        const char *sep = strrchr(leaf, '/'); /* (the Linux tests' names) */
#endif
        char base[64];
        snprintf(base, sizeof(base), "%.63s", sep ? sep + 1 : leaf);
        char *ext = strchr(base, '/');
        if (ext)
            *ext = 0;
        snprintf(X.name, sizeof(X.name), "%s/mp4", base[0] ? base : "Video");
        if (!X.status[0] || X.result)
            say("Drag the icon to a directory display.", NULL);
    }
    show();
    int st[9];
    st[0] = X.win;
    r.r[1] = (intptr_t) st;
    ro_swi(Wimp_GetWindowState, &r);
    if (!(st[8] & (1 << 16))) {
        st[1] = 360; st[3] = 360 + WIDTH; st[4] = 860; st[2] = 860 - HEIGHT;
    }
    st[5] = 0; st[6] = 0; st[7] = -1;
    r.r[1] = (intptr_t) st;
    ro_swi(Wimp_OpenWindow, &r);
}

/* Started, to a full RISC OS name: 0, or -1 (said) */
static int start(const char *path, int safe)
{
    char proj[300], unix_proj[512], unix_out[512], cmd[1400];
    if (!safe) {
        app_report("Drag the video to a directory display: it's made in the background.");
        return -1;
    }
    if (X.running || !A.proj)
        return -1;
    snprintf(proj, sizeof(proj), "<Wimp$ScrapDir>.ReelEdExp");
    const char *uproj = app_mlt_name(proj, unix_proj, sizeof(unix_proj));
    if (re_project_save_copy(A.proj, uproj)) {
        app_report("The project couldn't be saved to a scrap file to export it: is the scrap directory full?");
        return -1;
    }
    snprintf(X.out, sizeof(X.out), "%s", path);
    const char *uout = app_mlt_name(path, unix_out, sizeof(unix_out));
    char nums[3][16];
    snprintf(nums[0], 16, "%d", HEIGHTS[X.size]);
    snprintf(nums[1], 16, "%d", X.quality);
    snprintf(nums[2], 16, "%d", X.hw && X.can_hw);
    const char *args[5] = {uproj, uout, nums[0], nums[1], nums[2]};
    _kernel_oserror *e = app_child_start("export", args, 5, "ReelEdit export", TXT_ID, cmd, sizeof(cmd));
    app_log("export to %s: %s%s%s", path, cmd, e ? ": " : "", e ? e->errmess : "");
    if (e) {
        app_report("The export couldn't be started (TaskWindow).");
        return -1;
    }
    X.running = 1;
    X.child = 0;
    X.result = 0;
    X.line_len = 0;
    X.done = X.total = 0;
    say("Starting...", NULL);
    return 0;
}

static int dragged(const char *path, int safe)
{
    return start(path, safe);
}

static void stop(void)
{
    _kernel_swi_regs r;
    int m[6];
    if (!X.running)
        return;
    if (X.child) {
        memset(m, 0, sizeof(m));
        m[0] = 20; m[4] = MSG_TW_MORITE;
        r.r[0] = 17; r.r[1] = (intptr_t) m; r.r[2] = X.child;
        ro_swi(Wimp_SendMessage, &r);
    }
    X.result = -2; /* (stopped: when it has ended, its file goes) */
    say("Stopping...", NULL);
}

void ex_click(int *b)
{
    _kernel_swi_regs r;
    if (b[4] == I_GO && (b[2] & 5)) {
        if (X.running) {
            stop();
        } else if (!strchr(X.name, '.') && !strchr(X.name, ':')) {
            app_report("To export, drag the icon to a directory display (or type the full name).");
        } else {
            start(X.name, 1);
        }
        return;
    }
    if (b[4] == I_HW && (b[2] & 5)) {
        X.hw = X.can_hw && !X.hw;
        show();
        return;
    }
    if (b[4] == I_FILE && (b[2] & (0x40 | 0x10))) {
        int st[9];
        st[0] = X.win;
        r.r[1] = (intptr_t) st;
        ro_swi(Wimp_GetWindowState, &r);
        int ox = st[1] - st[5], oy = st[4] - st[6];
        const char *leaf = strrchr(X.name, '.');
        sb_drag_file(X.win, ox + WIDTH / 2 - 64, oy - 124, ox + WIDTH / 2 + 64, oy - 20, leaf ? leaf + 1 : X.name,
                     FILETYPE_MP4, dragged);
        return;
    }
    if ((b[4] != I_SIZE && b[4] != I_QUALITY) || !(b[2] & 5))
        return;
    static MENU(5) m;
    int which = b[4] == I_SIZE ? M_SIZE : M_QUALITY, n = which == M_SIZE ? 5 : 3;
    memset(&m, 0, sizeof(m));
    snprintf(m.title, sizeof(m.title), "%s", which == M_SIZE ? "Size" : "Quality");
    m.tfg = 7, m.tbg = 2, m.wfg = 7, m.wbg = 0;
    m.w = 280, m.h = 44;
    for (int i = 0; i < n; i++) {
        const char *t = which == M_SIZE ? SIZE_NAMES[i] : QUALITY_NAMES[i];
        m.item[i].flags = (i == n - 1 ? 0x80 : 0) | (i == (which == M_SIZE ? X.size : X.quality) ? 1 : 0);
        m.item[i].sub = -1;
        m.item[i].iflags = ITEM_FLAGS;
        m.item[i].text = t;
        m.item[i].valid = (const char *) -1;
        m.item[i].len = (int) strlen(t) + 1;
    }
    X.menu = which;
    r.r[1] = (intptr_t) &m;
    r.r[2] = b[0] - 64;
    r.r[3] = b[1] + 20;
    ro_swi(Wimp_CreateMenu, &r);
    app_export_menu_opened();
}

void ex_menu_select(int *b)
{
    if (b[0] < 0)
        return;
    if (X.menu == M_SIZE && b[0] < 5)
        X.size = b[0];
    else if (X.menu == M_QUALITY && b[0] < 3)
        X.quality = b[0];
    show();
}

void ex_key(int *b)
{
    _kernel_swi_regs r;
    if (b[6] == 13 && !X.running) {
        b[2] = 4; b[4] = I_GO;
        ex_click(b);
        return;
    }
    r.r[0] = b[6];
    ro_swi(Wimp_ProcessKey, &r);
}

/* A line the export said */
static void heard(const char *line)
{
    char t[200];
    int d, n;
    if (sscanf(line, "progress %d %d", &d, &n) == 2) {
        X.done = d;
        X.total = n;
        if (n > 0) {
            snprintf(t, sizeof(t), "Exporting: %d%% (%d of %d frames)", (int) (100.0 * d / n), d, n);
            say("%.119s", t);
        }
        return;
    }
    app_log("export: %s", line);
    if (!strcmp(line, "done")) {
        X.result = 1;
    } else if (!strncmp(line, "failed: ", 8)) {
        X.result = X.result == -2 ? -2 : -1;
        snprintf(t, sizeof(t), "The export failed: %.150s", line + 8);
        say("%.119s", t);
    }
}

/* It has ended: as it said */
static void ended(void)
{
    _kernel_swi_regs r;
    char t[300];
    X.running = 0;
    X.child = 0;
    const char *leaf = strrchr(X.out, '.');
    leaf = leaf ? leaf + 1 : X.out;
    if (X.result == 1) {
        r.r[0] = 18; r.r[1] = (intptr_t) X.out; r.r[2] = FILETYPE_MP4;
        ro_swi(OS_File, &r);
        snprintf(t, sizeof(t), "Exported: %.80s", leaf);
        say("%.119s", t);
    } else {
        char unix_out[512];
        remove(app_mlt_name(X.out, unix_out, sizeof(unix_out))); /* (a part made: gone) */
        if (X.result == -2) {
            say("Stopped.", NULL);
        } else {
            if (X.result == 0)
                say("The export ended without finishing.", NULL);
            app_report(X.status);
        }
    }
    app_log("export ended: %s", X.status);
}

int ex_message(int *b)
{
    if (b[4] == MSG_TW_EGO && X.running && b[5] == TXT_ID) {
        X.child = b[1];
        return 1;
    }
    if (b[4] == MSG_TW_OUTPUT && X.running && b[1] == X.child) {
        int n = b[5];
        const char *d = (const char *) &b[6];
        for (int i = 0; i < n && i < 232; i++) {
            if (d[i] == '\n' || d[i] == '\r') {
                X.line[X.line_len] = 0;
                if (X.line_len)
                    heard(X.line);
                X.line_len = 0;
            } else if (X.line_len < (int) sizeof(X.line) - 1) {
                X.line[X.line_len++] = d[i];
            }
        }
        return 1;
    }
    if (b[4] == MSG_TW_MORIO && X.running && b[1] == X.child) {
        if (X.line_len) {
            X.line[X.line_len] = 0;
            heard(X.line);
            X.line_len = 0;
        }
        ended();
        return 1;
    }
    return 0;
}

const char *ex_help(int icon)
{
    switch (icon) {
    case I_FILE: return "Drag this to a directory display to export the project there, as an MP4 video.";
    case I_NAME: return "The video's name: drag the icon to save it, or type a full name and click Export.";
    case I_SIZE: return "Click to choose the video's size: the project's, or fewer lines (the shape kept).";
    case I_QUALITY: return "Click to choose the video's quality: a small file, normal, or high (a bigger file).";
    case I_HW: return "Click to make the video with the Pi's VideoCore: several times faster than the ARM.|M"
                      "Shaded without the VCHIQ module.";
    case I_GO: return X.running ? "Click to stop the export (the part made is deleted)."
                                : "Click to export to the full name typed.";
    default: return "This is the Export window: the project made into an MP4 video, while you carry on editing.";
    }
}

#ifdef RE_TEST
char *re_test_export_name(void) { return X.name; }
const char *re_test_export_status(void) { return X.status; }
#endif
