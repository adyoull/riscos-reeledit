/* clips.c - the Clips window (app.h): the project's clips list
 * (project.h's), as Premiere's bin. View > Clips... (Ctrl-B) opens it.
 *
 *   a row a clip: its name and length; click to select it
 *   double-click (or Return): the clip shown in the Viewer as a source
 *       (I and O mark it; , inserts and . overwrites its marked frames at
 *       the project's playhead; Escape, the project again)
 *   drag a row to the Timeline: the whole clip put there, as a file
 *       dropped
 *   Delete: the selected clip off the list (not off the Timeline)
 * Files dropped on the window go on the list (not on the Timeline);
 * every clip put on the Timeline comes on the list too.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "app.h"

#include <stdio.h>
#include <string.h>

#define WIDTH  760
#define ROW_H  44
#define HEAD_H 56
#define Wimp_DragBox        0x400D0
#define Wimp_GetPointerInfo 0x400CF

static struct
{
    int win;
    int sel;       /* the selected row (-1: none) */
    int dragging;  /* a row being dragged */
    char title[16];
} C = {0, -1, 0, "Clips"};

int cl_window(void)
{
    return C.win;
}

static int rows(void)
{
    return A.proj ? re_project_clips(A.proj) : 0;
}

static int height(void)
{
    int h = HEAD_H + rows() * ROW_H + 16;
    return h < 300 ? 300 : h;
}

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
    w.vis.x1 = WIDTH; w.vis.y1 = 400;
    w.behind = -1;
    w.flags = (int) 0xBF000002u; /* new format, back, close, title, toggle, vertical scroll, adjust size, moveable */
    w.tfg = 7; w.tbg = 2; w.wfg = 7; w.wbg = 0; w.sofg = 3; w.sibg = 1; w.tfocus = 12;
    w.ext.x0 = 0; w.ext.y0 = -height(); w.ext.x1 = WIDTH; w.ext.y1 = 0;
    w.tflags = 0x1 | 0x4 | 0x8 | 0x10 | 0x20 | 0x100 | (7 << 24) | (2 << 28);
    w.wbutton = 10 << 12; /* click, drag, double click */
    w.sprites = 1;
    w.minw = 300; w.minh = 150;
    w.title.text = C.title;
    w.title.valid = (const char *) -1;
    w.title.len = sizeof(C.title);
    r.r[1] = (intptr_t) &w;
    if (ro_swi(Wimp_CreateWindow, &r))
        return -1;
    C.win = (int) r.r[0];
    return 0;
}

static void redraw_all(void)
{
    _kernel_swi_regs r;
    int ext[4] = {0, -height(), WIDTH, 0};
    if (!C.win)
        return;
    r.r[0] = C.win;
    r.r[1] = (intptr_t) ext;
    ro_swi(Wimp_SetExtent, &r);
    r.r[0] = C.win; r.r[1] = 0; r.r[2] = -height(); r.r[3] = WIDTH; r.r[4] = 0;
    ro_swi(Wimp_ForceRedraw, &r);
}

void cl_open(void)
{
    _kernel_swi_regs r;
    int st[9];
    if (!A.proj) {
        app_report("Open the editor first (click Reel Edit's icon).");
        return;
    }
    if (!C.win && create() < 0)
        return;
    st[0] = C.win;
    r.r[1] = (intptr_t) st;
    ro_swi(Wimp_GetWindowState, &r);
    if (!(st[8] & (1 << 16))) { /* not open: at the screen's left */
        st[1] = 40; st[3] = 40 + WIDTH; st[4] = 1000; st[2] = 600;
    }
    st[5] = 0; st[6] = 0; st[7] = -1;
    r.r[1] = (intptr_t) st;
    ro_swi(Wimp_OpenWindow, &r);
    redraw_all();
}

void cl_close(void)
{
    _kernel_swi_regs r;
    C.sel = -1;
    if (C.win) {
        r.r[1] = (intptr_t) &C.win;
        ro_swi(Wimp_CloseWindow, &r);
    }
}

void cl_edited(void)
{
    if (C.sel >= rows())
        C.sel = -1;
    redraw_all();
}

#define Wimp_TextOp         0x400F9
#define ColourTrans_SetGCOL 0x40743
#define OS_Plot             0x45

static void text(const char *s, int x, int y, unsigned fg, unsigned bg)
{
    _kernel_swi_regs r;
    r.r[0] = 0; r.r[1] = (int) fg; r.r[2] = (int) bg; /* (Wimp_TextOp 0: its colours) */
    ro_swi(Wimp_TextOp, &r);
    r.r[0] = 2; r.r[1] = (intptr_t) s; r.r[2] = -1; r.r[3] = -1; r.r[4] = x; r.r[5] = y;
    ro_swi(Wimp_TextOp, &r);
}

static void rect(int x0, int y0, int x1, int y1, unsigned colour)
{
    _kernel_swi_regs r;
    r.r[0] = (int) colour; r.r[3] = 0; r.r[4] = 0;
    ro_swi(ColourTrans_SetGCOL, &r);
    r.r[0] = 4; r.r[1] = x0; r.r[2] = y0;
    ro_swi(OS_Plot, &r); /* move */
    r.r[0] = 96 + 5; r.r[1] = x1; r.r[2] = y1;
    ro_swi(OS_Plot, &r); /* (a rectangle, filled) */
}

static const char *leaf(const char *name)
{
    if (!strncmp(name, "qimage:", 7)) /* (a picture, by the name it was added by) */
        name += 7;
    const char *l = strrchr(name, '/');
    return l ? l + 1 : name;
}

void cl_redraw(int *b)
{
    _kernel_swi_regs r;
    r.r[1] = (intptr_t) b;
    int more = ro_swi(Wimp_RedrawWindow, &r) ? 0 : (int) r.r[0];
    while (more) {
        int ox = b[1] - b[5], oy = b[4] - b[6];
        text(rows() ? "Double-click a clip to show it; drag one to the Timeline."
                    : "Drop clips here to keep them to hand; those on the Timeline come here too.",
             ox + 16, oy - 40, 0x66666600, 0xFFFFFF00);
        for (int i = 0; i < rows(); i++) {
            int y1 = oy - HEAD_H - i * ROW_H, y0 = y1 - ROW_H + 4;
            if (y1 < b[8] || y0 > b[10])
                continue;
            unsigned bg = i == C.sel ? 0x2080F000 : 0xFFFFFF00;
            if (i == C.sel)
                rect(ox + 8, y0, ox + WIDTH - 8, y1 - 2, bg);
            text(leaf(re_project_clip(A.proj, i)), ox + 20, y0 + 10, 0x00000000, bg);
        }
        r.r[1] = (intptr_t) b;
        more = ro_swi(Wimp_GetRectangle, &r) ? 0 : (int) r.r[0];
    }
}

static int row_at(int *b, int *st)
{
    int wy = b[1] - (st[4] - st[6]);
    int i = (-wy - HEAD_H) / ROW_H;
    return -wy >= HEAD_H && i >= 0 && i < rows() ? i : -1;
}

void cl_click(int *b)
{
    int st[9];
    _kernel_swi_regs r;
    st[0] = C.win;
    r.r[1] = (intptr_t) st;
    if (ro_swi(Wimp_GetWindowState, &r))
        return;
    int i = row_at(b, st);
    if (b[2] & 2) { /* Menu: the editor's */
        app_menu(b[0], b[1]);
        return;
    }
    r.r[0] = C.win; r.r[1] = -1; r.r[2] = 0; r.r[3] = 0; r.r[4] = 1 << 25; r.r[5] = -1;
    ro_swi(Wimp_SetCaretPosition, &r); /* (the keys here) */
    if (i < 0) {
        C.sel = -1;
        redraw_all();
        return;
    }
    if (b[2] & 0x400 || b[2] & 0x4) { /* (a click's first report, or a double click) */
        C.sel = i;
        redraw_all();
    }
    if (b[2] & 0x4 && !(b[2] & 0x400)) { /* double click: shown as a source */
        app_log("clips: %s shown", re_project_clip(A.proj, i));
        app_source(re_project_clip(A.proj, i));
        return;
    }
    if (b[2] & 0x40) { /* a drag: the row's box, anywhere */
        int d[10], y1 = (st[4] - st[6]) - HEAD_H - i * ROW_H;
        d[0] = C.win; d[1] = 5;
        d[2] = st[1] + 8; d[3] = y1 - ROW_H + 4; d[4] = st[1] + WIDTH - 8; d[5] = y1;
        d[6] = -0x10000; d[7] = -0x10000; d[8] = 0x10000; d[9] = 0x10000;
        r.r[1] = (intptr_t) d;
        C.sel = i;
        if (!ro_swi(Wimp_DragBox, &r))
            C.dragging = 1;
    }
}

int cl_dragging(void)
{
    return C.dragging;
}

void cl_drag_end(int *b)
{
    int p[5];
    _kernel_swi_regs r;
    (void) b;
    C.dragging = 0;
    r.r[1] = (intptr_t) p;
    if (!A.proj || C.sel < 0 || ro_swi(Wimp_GetPointerInfo, &r) || p[3] != tl_window())
        return;
    const char *name = re_project_clip(A.proj, C.sel);
    int frame = tl_frame_at(p[0]);
    app_log("clips: %s dragged to the Timeline at %d", name, frame);
    if (tl_on_audio(p[1]) >= 0)
        au_load(name, frame);
    else
        app_add(name, name, frame);
}

void cl_key(int *b)
{
    _kernel_swi_regs r;
    if (A.proj && C.sel >= 0 && b[6] == 13) {
        app_source(re_project_clip(A.proj, C.sel));
        return;
    }
    if (A.proj && C.sel >= 0 && (b[6] == 0x7F || b[6] == 8)) {
        app_log("clips: %s off the list", re_project_clip(A.proj, C.sel));
        re_project_clip_remove(A.proj, C.sel);
        C.sel = -1;
        redraw_all();
        app_edited();
        return;
    }
    if (app_key(b[6]))
        return;
    r.r[0] = b[6];
    ro_swi(Wimp_ProcessKey, &r);
}

void cl_load(const char *path)
{
    char unix_name[512];
    if (!A.proj)
        return;
    int i = re_project_clip_add(A.proj, app_mlt_name(path, unix_name, sizeof(unix_name)));
    app_log("clips: %s on the list (%d)", path, i);
    C.sel = i;
    redraw_all();
    app_edited();
}

const char *cl_help(void)
{
    return "This is the Clips window: clips kept to hand.|MDouble-click one to show it in the Viewer: I and O "
           "mark it, , inserts and . overwrites at the playhead.|MDrag one to the Timeline; Delete takes it off "
           "the list.";
}

#ifdef RE_TEST
int re_test_clips_selected(void) { return C.sel; }
#endif
