/* savebox.c - the Save as box (app.h), as every RISC OS program has: the
 * project's file icon to drag to a directory display (or another
 * program), its name to type, and Save. Opened as a menu (F3, or the
 * menu's File > Save as). Dragging: Wimp_DragBox, then Message_DataSave
 * to wherever it was dropped; the Filer replies with Message_DataSaveAck
 * and the full name; saved there, and Message_DataLoad sent back.
 * Typed: a full name (with a '.' or ':') saves there at once.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "app.h"

#include <stdio.h>
#include <string.h>

#define Wimp_DragBox        0x400D0
#define Wimp_GetPointerInfo 0x400CF
#define OS_File             0x08

#define MSG_DATASAVE    1
#define MSG_DATASAVEACK 2
#define MSG_DATALOAD    3

enum { S_FILE, S_NAME, S_SAVE, S_N };

static struct
{
    int win;
    char name[256];
    char sprite[16];
    int dragging;
    int my_ref;      /* our Message_DataSave */
} B;

int sb_window(void)
{
    return B.win;
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
        icon_t icon[S_N];
    } w;
    static char save_text[] = "Save";
    _kernel_swi_regs r;
    memset(&w, 0, sizeof(w));
    w.vis.x1 = 520; w.vis.y1 = 232;
    w.behind = -1;
    w.flags = (int) 0x84000012u; /* new format, moveable, title, not a "pane" */
    w.tfg = 7; w.tbg = 2; w.wfg = 7; w.wbg = 1; w.sofg = 3; w.sibg = 1; w.tfocus = 12;
    w.ext.x0 = 0; w.ext.y0 = -232; w.ext.x1 = 520; w.ext.y1 = 0;
    w.tflags = 0x1 | 0x8 | 0x10; /* text, centred */
    w.sprites = 1;
    memcpy(w.title, "Save as", 8);
    w.nicons = S_N;
    /* the file icon: the project's filetype's sprite from the Wimp's pool,
       by name (not indirected); click or drag */
    snprintf(B.sprite, sizeof(B.sprite), "file_%03x", FILETYPE_MLT);
    w.icon[S_FILE].x0 = 196; w.icon[S_FILE].y0 = -124; w.icon[S_FILE].x1 = 324; w.icon[S_FILE].y1 = -20;
    w.icon[S_FILE].flags = 0x2 | 0x8 | 0x10 | (6 << 12) | (7 << 24) | (1 << 28); /* sprite, centred, click/drag */
    memcpy(&w.icon[S_FILE].ind, B.sprite, strlen(B.sprite) + 1);
    /* the name: writable */
    w.icon[S_NAME].x0 = 16; w.icon[S_NAME].y0 = -196; w.icon[S_NAME].x1 = 372; w.icon[S_NAME].y1 = -144;
    w.icon[S_NAME].flags = 0x1 | 0x4 | 0x10 | 0x20 | 0x100 | (15 << 12) | (7 << 24) | (0 << 28);
    w.icon[S_NAME].ind.text = B.name;
    w.icon[S_NAME].ind.valid = "Pptr_write;Ktar";
    w.icon[S_NAME].ind.len = sizeof(B.name);
    /* Save: the default action (R6,3 border) */
    w.icon[S_SAVE].x0 = 388; w.icon[S_SAVE].y0 = -204; w.icon[S_SAVE].x1 = 504; w.icon[S_SAVE].y1 = -136;
    w.icon[S_SAVE].flags = 0x1 | 0x4 | 0x8 | 0x10 | 0x20 | 0x100 | (3 << 12) | (7 << 24) | (1 << 28);
    w.icon[S_SAVE].ind.text = save_text;
    w.icon[S_SAVE].ind.valid = "R6,3";
    w.icon[S_SAVE].ind.len = sizeof(save_text);
    r.r[1] = (intptr_t) &w;
    if (ro_swi(Wimp_CreateWindow, &r))
        return -1;
    B.win = (int) r.r[0];
    return 0;
}

void sb_open(int x, int y)
{
    _kernel_swi_regs r;
    if (!A.proj || !re_project_producer(A.proj)) {
        app_report("There's nothing to save yet: drag a clip from a directory display to the Timeline first.");
        return;
    }
    if (!B.win && create() < 0)
        return;
    snprintf(B.name, sizeof(B.name), "%s", A.path[0] ? A.path : "Project");
    r.r[1] = B.win;
    r.r[2] = x - 64;
    r.r[3] = y + 64;
    ro_swi(Wimp_CreateMenu, &r);
    /* the caret at the end of the name */
    r.r[0] = B.win; r.r[1] = S_NAME; r.r[2] = 0; r.r[3] = 0; r.r[4] = -1; r.r[5] = (int) strlen(B.name);
    ro_swi(Wimp_SetCaretPosition, &r);
}

static void close_box(void)
{
    _kernel_swi_regs r;
    r.r[1] = -1;
    ro_swi(Wimp_CreateMenu, &r); /* (closes the menu tree, the box with it) */
}

/* Saves to a full RISC OS name: 0, or -1 (said) */
static int save_to(const char *path, int safe)
{
    char unix_name[512];
    _kernel_swi_regs r;
    if (re_project_save(A.proj, app_mlt_name(path, unix_name, sizeof(unix_name))) < 0) {
        char t[300];
        snprintf(t, sizeof(t), "The project couldn't be saved as %s.", path);
        app_report(t);
        return -1;
    }
    r.r[0] = 18; /* OS_File 18: set the filetype */
    r.r[1] = (intptr_t) path;
    r.r[2] = FILETYPE_MLT;
    ro_swi(OS_File, &r);
    app_log("saved as %s", path);
    if (safe)
        app_saved(path);
    return 0;
}

static void save_typed(void)
{
    if (!strchr(B.name, '.') && !strchr(B.name, ':')) {
        app_report("To save, drag the icon to a directory display.");
        return;
    }
    if (!save_to(B.name, 1))
        close_box();
}

void sb_click(int *b)
{
    _kernel_swi_regs r;
    if (b[4] == S_SAVE && (b[2] & 5)) {
        char typed[sizeof(B.name)];
        snprintf(typed, sizeof(typed), "%s", B.name);
        save_typed();
        if (b[2] & 1) { /* (Adjust: the box stays, with what was typed) */
            sb_open(b[0], b[1]);
            snprintf(B.name, sizeof(B.name), "%s", typed);
        }
    } else if (b[4] == S_FILE && (b[2] & (0x40 | 0x10))) {
        /* a drag of the file icon: a box the icon's size, anywhere on the screen */
        int st[9], d[10];
        st[0] = B.win;
        r.r[1] = (intptr_t) st;
        ro_swi(Wimp_GetWindowState, &r);
        int ox = st[1] - st[5], oy = st[4] - st[6];
        d[0] = B.win;
        d[1] = 5; /* drag a fixed size box */
        d[2] = ox + 196; d[3] = oy - 124; d[4] = ox + 324; d[5] = oy - 20;
        d[6] = -0x10000; d[7] = -0x10000; d[8] = 0x10000; d[9] = 0x10000;
        r.r[1] = (intptr_t) d;
        if (!ro_swi(Wimp_DragBox, &r))
            B.dragging = 1;
    }
}

void sb_key(int *b)
{
    _kernel_swi_regs r;
    if (b[6] == 13) {
        save_typed();
        return;
    }
    r.r[0] = b[6];
    ro_swi(Wimp_ProcessKey, &r);
}

static const char *leaf_of(const char *path)
{
    const char *l = strrchr(path, '.'), *c = strrchr(path, ':');
    if (c && (!l || c > l))
        l = c;
    return l ? l + 1 : path;
}

void sb_drag_end(int *b)
{
    int p[5], m[64];
    _kernel_swi_regs r;
    (void) b;
    if (!B.dragging)
        return;
    B.dragging = 0;
    r.r[1] = (intptr_t) p;
    if (ro_swi(Wimp_GetPointerInfo, &r))
        return;
    if (p[3] == B.win || p[3] == tl_window()) /* dropped back on the box, or the Timeline: nothing */
        return;
    /* Message_DataSave to the window it was dropped on */
    memset(m, 0, sizeof(m));
    const char *leaf = leaf_of(B.name);
    m[3] = 0;
    m[4] = MSG_DATASAVE;
    m[5] = p[3]; m[6] = p[4]; m[7] = p[0]; m[8] = p[1];
    m[9] = 4096;           /* estimated size */
    m[10] = FILETYPE_MLT;
    snprintf((char *) &m[11], 212, "%.211s", leaf);
    m[0] = (44 + (int) strlen((char *) &m[11]) + 1 + 3) & ~3;
    r.r[0] = 17;
    r.r[1] = (intptr_t) m;
    r.r[2] = p[3];
    r.r[3] = p[4];
    if (!ro_swi(Wimp_SendMessage, &r))
        B.my_ref = m[2];
}

void sb_forget(void)
{
    B.my_ref = 0;
    B.dragging = 0;
}

int sb_message(int *b)
{
    _kernel_swi_regs r;
    if (b[4] != MSG_DATASAVEACK || !B.my_ref || b[3] != B.my_ref)
        return 0;
    B.my_ref = 0;
    if (!A.proj)
        return 1;
    char path[256];
    snprintf(path, sizeof(path), "%s", (const char *) &b[11]);
    int safe = b[9] != -1; /* (-1: a scrap file, another program's: not where it lives) */
    if (save_to(path, safe))
        return 1;
    /* Message_DataLoad back: the receiver loads it */
    b[3] = b[2];
    b[4] = MSG_DATALOAD;
    r.r[0] = 18; /* recorded */
    r.r[1] = (intptr_t) b;
    r.r[2] = b[1];
    ro_swi(Wimp_SendMessage, &r);
    close_box();
    return 1;
}

#ifdef RE_TEST
char *re_test_savebox_name(void) { return B.name; }
#endif
