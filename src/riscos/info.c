/* info.c - the Video information window (app.h): what's known of the
 * selected clip (or the one at the playhead) and of the project, as
 * labels and values, drawn with the desktop font. Made again each time
 * it's opened, and when the project changes while it's open.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "app.h"

#include <stdio.h>
#include <string.h>

#define Wimp_TextOp 0x400F9
#define LINE_H 44
#define LABEL_W 300
#define WIDTH 1700 /* (the Video line: codec, size, rate, format, colours) */

static struct
{
    int win, lines;
    char text[4096];
    char title[64];
} I;

int info_window(void)
{
    return I.win;
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
    w.ext.x0 = 0; w.ext.y0 = -400; w.ext.x1 = WIDTH; w.ext.y1 = 0;
    w.tflags = 0x1 | 0x4 | 0x8 | 0x10 | 0x20 | 0x100 | (7 << 24) | (2 << 28);
    w.sprites = 1;
    w.minw = 400; w.minh = 100;
    w.title.text = I.title;
    w.title.valid = (const char *) -1;
    w.title.len = sizeof(I.title);
    snprintf(I.title, sizeof(I.title), "Video information");
    r.r[1] = (intptr_t) &w;
    if (ro_swi(Wimp_CreateWindow, &r))
        return -1;
    I.win = (int) r.r[0];
    return 0;
}

/* The text: the clip's, then the project's */
static void fill(void)
{
    int n = 0, item = A.sel;
    I.text[0] = 0;
    if (!A.proj)
        return;
    if (item < 0)
        item = re_project_item_at(A.proj, app_position());
    if (item >= 0) {
        int k = re_project_item_info(A.proj, item, I.text, sizeof(I.text));
        if (k > 0)
            n = k < (int) sizeof(I.text) ? k : (int) sizeof(I.text) - 1;
    }
    if (n > 0 && n < (int) sizeof(I.text) - 2)
        n += snprintf(I.text + n, sizeof(I.text) - n, "\t\n");
    re_project_info(A.proj, I.text + n, (int) sizeof(I.text) - n);
    I.lines = 0;
    for (const char *c = I.text; *c; c++)
        if (*c == '\n')
            I.lines++;
}

void info_open(void)
{
    _kernel_swi_regs r;
    int st[9];
    if (!I.win && create() < 0)
        return;
    fill();
    int h = I.lines * LINE_H + 24;
    int ext[4] = {0, -h, WIDTH, 0};
    r.r[0] = I.win;
    r.r[1] = (intptr_t) ext;
    ro_swi(Wimp_SetExtent, &r);
    st[0] = I.win;
    r.r[1] = (intptr_t) st;
    ro_swi(Wimp_GetWindowState, &r);
    if (!(st[8] & (1 << 16))) { /* not open: in the middle of the screen */
        st[1] = 400; st[3] = 400 + WIDTH; st[4] = 900; st[2] = 900 - h;
    } else {
        st[2] = st[4] - h;
    }
    st[5] = 0; st[6] = 0; st[7] = -1;
    r.r[1] = (intptr_t) st;
    ro_swi(Wimp_OpenWindow, &r);
    r.r[0] = I.win; r.r[1] = 0; r.r[2] = -h; r.r[3] = WIDTH; r.r[4] = 0;
    ro_swi(Wimp_ForceRedraw, &r);
}

void info_edited(void)
{
    int st[9];
    _kernel_swi_regs r;
    if (!I.win)
        return;
    st[0] = I.win;
    r.r[1] = (intptr_t) st;
    if (!ro_swi(Wimp_GetWindowState, &r) && (st[8] & (1 << 16)))
        info_open();
}

void info_close(void)
{
    _kernel_swi_regs r;
    if (I.win) {
        r.r[1] = (intptr_t) &I.win;
        ro_swi(Wimp_CloseWindow, &r);
    }
}

static void text(const char *s, int x, int y, int right)
{
    _kernel_swi_regs r;
    r.r[0] = 0; r.r[1] = 0x00000000; r.r[2] = (int) 0xFFFFFF00u; /* black on white */
    ro_swi(Wimp_TextOp, &r);
    r.r[0] = 2 | (right ? (1u << 31) : 0); r.r[1] = (intptr_t) s; r.r[2] = -1; r.r[3] = -1; r.r[4] = x; r.r[5] = y;
    ro_swi(Wimp_TextOp, &r);
}

void info_redraw(int *b)
{
    _kernel_swi_regs r;
    r.r[1] = (intptr_t) b;
    int more = ro_swi(Wimp_RedrawWindow, &r) ? 0 : (int) r.r[0];
    while (more) {
        int ox = b[1] - b[5], oy = b[4] - b[6];
        const char *line = I.text;
        for (int i = 0; *line; i++) {
            const char *end = strchr(line, '\n'), *tab = strchr(line, '\t');
            if (!end)
                end = line + strlen(line);
            char label[128], value[512];
            int y = oy - 12 - (i + 1) * LINE_H + 12;
            if (tab && tab < end) {
                snprintf(label, sizeof(label), "%.*s", (int) (tab - line), line);
                snprintf(value, sizeof(value), "%.*s", (int) (end - tab - 1), tab + 1);
            } else {
                label[0] = 0;
                snprintf(value, sizeof(value), "%.*s", (int) (end - line), line);
            }
            if (label[0])
                text(label, ox + LABEL_W, y, 1);
            if (value[0])
                text(value, ox + LABEL_W + 24, y, 0);
            line = *end ? end + 1 : end;
        }
        r.r[1] = (intptr_t) b;
        more = ro_swi(Wimp_GetRectangle, &r) ? 0 : (int) r.r[0];
    }
}
