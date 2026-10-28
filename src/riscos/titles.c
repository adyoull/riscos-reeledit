/* titles.c - the Title window (app.h): text over the video, made or
 * changed. Edit > New title... (T) opens it for a new title at the
 * playhead; the overlay menu's Edit title... for one there. What a title is and how it's drawn is title.h's;
 * putting it on the titles' track (over V2) is project.h's.
 *
 *   Text     two lines (Return moves to the next; on the second, it's
 *            made)
 *   Font, Size, Colour, Style, Lines, Place   each a button giving a
 *            menu of choices
 *   Cancel, Create (Change, for a title there already)
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "app.h"
#include "menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { I_TEXT_L, I_LINE1, I_LINE2, I_FONT_L, I_FONT, I_SIZE_L, I_SIZE, I_COLOUR_L, I_COLOUR, I_STYLE_L, I_STYLE,
       I_ALIGN_L, I_ALIGN, I_PLACE_L, I_PLACE, I_CANCEL, I_OK, I_N };
enum { M_FONT, M_SIZE, M_COLOUR, M_STYLE, M_ALIGN, M_PLACE };

#define WIDTH  800
#define HEIGHT 412

static const char *const FONTS[7] = {"Homerton.Medium", "Homerton.Bold", "Homerton.Medium.Oblique", "Trinity.Medium",
                                     "Trinity.Bold",    "Corpus.Medium", "Corpus.Bold"};
static const char *const FONT_NAMES[7] = {"Homerton", "Homerton Bold", "Homerton Italic", "Trinity", "Trinity Bold",
                                          "Corpus",   "Corpus Bold"};
static const int SIZES[5] = {4, 6, 8, 11, 15};
static const char *const SIZE_NAMES[5] = {"Tiny", "Small", "Medium", "Large", "Huge"};
static const unsigned COLOURS[6] = {0xFFFFFF, 0xFFE000, 0x000000, 0xE02020, 0x2060FF, 0x20C040};
static const char *const COLOUR_NAMES[6] = {"White", "Yellow", "Black", "Red", "Blue", "Green"};
static const char *const STYLE_NAMES[RE_TITLE_STYLES] = {"Plain", "Shadow", "Outline", "On a box"};
static const char *const ALIGN_NAMES[3] = {"Left", "Centred", "Right"};
/* (a title's places: an overlay's but all the frame, the same as the
   centre for a title; menu item k is place k + 1) */
static const char *const PLACE_NAMES[RE_PLACES - 1] = {"Centre", "Top left", "Top right", "Bottom left",
                                                       "Bottom right", "Lower third"};

static struct
{
    int win;
    int ov;                /* the title being changed (-1: a new one) */
    re_title t;
    int place;
    int menu;              /* which button's menu is open (M_) */
    char line1[256], line2[256];
    char shown[6][32];     /* the buttons' texts */
    char ok_text[8];
} T = {0, -1};

int ti_window(void)
{
    return T.win;
}

static int index_of_font(const char *f)
{
    for (int i = 0; i < 7; i++)
        if (!strcmp(f, FONTS[i]))
            return i;
    return -1;
}

static int index_of_size(int s)
{
    int best = 0;
    for (int i = 1; i < 5; i++)
        if (abs(SIZES[i] - s) < abs(SIZES[best] - s))
            best = i;
    return best;
}

static int index_of_colour(unsigned c)
{
    for (int i = 0; i < 6; i++)
        if (COLOURS[i] == c)
            return i;
    return -1;
}

/* The buttons' texts from the settings */
static void show_settings(void)
{
    int f = index_of_font(T.t.font), c = index_of_colour(T.t.colour);
    snprintf(T.shown[M_FONT], 32, "%.31s", f >= 0 ? FONT_NAMES[f] : T.t.font);
    snprintf(T.shown[M_SIZE], 32, "%s", SIZE_NAMES[index_of_size(T.t.size)]);
    if (c >= 0)
        snprintf(T.shown[M_COLOUR], 32, "%s", COLOUR_NAMES[c]);
    else
        snprintf(T.shown[M_COLOUR], 32, "#%06X", T.t.colour);
    snprintf(T.shown[M_STYLE], 32, "%s", STYLE_NAMES[T.t.style >= 0 && T.t.style < RE_TITLE_STYLES ? T.t.style : 0]);
    snprintf(T.shown[M_ALIGN], 32, "%s", ALIGN_NAMES[T.t.align >= 0 && T.t.align < 3 ? T.t.align : 1]);
    snprintf(T.shown[M_PLACE], 32, "%s", PLACE_NAMES[T.place >= 1 && T.place < RE_PLACES ? T.place - 1 : 0]);
    if (!T.win)
        return;
    _kernel_swi_regs r;
    int b[4];
    for (int i = I_TEXT_L; i < I_N; i++) { /* (each redrawn: its text may have changed) */
        b[0] = T.win; b[1] = i; b[2] = 0; b[3] = 0;
        r.r[1] = (intptr_t) b;
        ro_swi(Wimp_SetIconState, &r);
    }
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
    static char labels[6][8] = {"Font", "Size", "Colour", "Style", "Lines", "Place"};
    static char text_label[] = "Text", cancel[] = "Cancel";
    _kernel_swi_regs r;
    memset(&w, 0, sizeof(w));
    w.vis.x1 = WIDTH; w.vis.y1 = HEIGHT;
    w.behind = -1;
    w.flags = (int) 0x86000012u; /* new format, moveable, title, close */
    w.tfg = 7; w.tbg = 2; w.wfg = 7; w.wbg = 1; w.sofg = 3; w.sibg = 1; w.tfocus = 12;
    w.ext.x0 = 0; w.ext.y0 = -HEIGHT; w.ext.x1 = WIDTH; w.ext.y1 = 0;
    w.tflags = 0x1 | 0x8 | 0x10; /* text, centred */
    w.sprites = 1;
    memcpy(w.title, "Title", 6);
    w.nicons = I_N;
    const int LABEL = 0x1 | 0x10 | 0x100 | (7 << 24) | (1 << 28);                       /* text, v-centred, indirected */
    const int WRITE = 0x1 | 0x4 | 0x10 | 0x20 | 0x100 | (15 << 12) | (7 << 24) | (0 << 28);
    const int BUTTON = 0x1 | 0x4 | 0x8 | 0x10 | 0x20 | 0x100 | (3 << 12) | (7 << 24) | (1 << 28);
    icon_t *ic = w.icon;
    ic[I_TEXT_L] = (icon_t){16, -68, 140, -16, LABEL, {text_label, (const char *) -1, sizeof(text_label)}};
    ic[I_LINE1] = (icon_t){150, -68, WIDTH - 16, -16, WRITE, {T.line1, "Pptr_write;Ktar", sizeof(T.line1)}};
    ic[I_LINE2] = (icon_t){150, -124, WIDTH - 16, -72, WRITE, {T.line2, "Pptr_write;Ktar", sizeof(T.line2)}};
    for (int k = 0; k < 6; k++) {
        int row = k / 2, col = k % 2, y1 = -148 - row * 60, x0 = col ? 410 : 16;
        ic[I_FONT_L + 2 * k] = (icon_t){x0, y1 - 52, x0 + 120, y1, LABEL, {labels[k], (const char *) -1, 8}};
        ic[I_FONT + 2 * k] = (icon_t){x0 + 124, y1 - 52, x0 + 374, y1, BUTTON, {T.shown[k], "R5,3", 32}};
    }
    ic[I_CANCEL] = (icon_t){WIDTH - 360, -HEIGHT + 20, WIDTH - 196, -HEIGHT + 84, BUTTON,
                            {cancel, "R5,3", sizeof(cancel)}};
    ic[I_OK] = (icon_t){WIDTH - 184, -HEIGHT + 12, WIDTH - 16, -HEIGHT + 92, BUTTON, {T.ok_text, "R6,3", 8}};
    r.r[1] = (intptr_t) &w;
    if (ro_swi(Wimp_CreateWindow, &r))
        return -1;
    T.win = (int) r.r[0];
    return 0;
}

static void caret_to(int icon)
{
    _kernel_swi_regs r;
    r.r[0] = T.win; r.r[1] = icon; r.r[2] = 0; r.r[3] = 0; r.r[4] = -1;
    r.r[5] = (int) strlen(icon == I_LINE1 ? T.line1 : T.line2);
    ro_swi(Wimp_SetCaretPosition, &r);
}

void ti_open(int ov)
{
    _kernel_swi_regs r;
    re_overlay o;
    if (!A.proj || re_project_length(A.proj) <= 0) {
        app_report("Put some video on V1 first: titles go over it.");
        return;
    }
    T.ov = -1;
    re_title_default(&T.t);
    T.place = RE_PLACE_LOWER_THIRD;
    T.line1[0] = T.line2[0] = 0;
    if (ov >= 0 && !re_project_overlay(A.proj, ov, &o) && o.title && !re_project_title(A.proj, ov, &T.t)) {
        T.ov = ov;
        T.place = o.place == RE_PLACE_FULL ? RE_PLACE_CENTRE : o.place;
        char *nl = strchr(T.t.text, '\n');
        snprintf(T.line1, sizeof(T.line1), "%.*s", nl ? (int) (nl - T.t.text) : (int) strlen(T.t.text), T.t.text);
        snprintf(T.line2, sizeof(T.line2), "%s", nl ? nl + 1 : "");
        char *nl2 = strchr(T.line2, '\n'); /* (a third line and more: on the second) */
        while (nl2) {
            *nl2 = ' ';
            nl2 = strchr(nl2, '\n');
        }
    }
    snprintf(T.ok_text, sizeof(T.ok_text), "%s", T.ov >= 0 ? "Change" : "Create");
    if (!T.win && create() < 0)
        return;
    show_settings();
    int st[9];
    st[0] = T.win;
    r.r[1] = (intptr_t) st;
    ro_swi(Wimp_GetWindowState, &r);
    if (!(st[8] & (1 << 16))) { /* not open: in the middle of the screen */
        st[1] = 300; st[3] = 300 + WIDTH; st[4] = 800; st[2] = 800 - HEIGHT;
    }
    st[5] = 0; st[6] = 0; st[7] = -1;
    r.r[1] = (intptr_t) st;
    ro_swi(Wimp_OpenWindow, &r);
    caret_to(I_LINE1);
    app_log("title window: %s", T.ov >= 0 ? "changing a title" : "a new title");
}

void ti_close(void)
{
    _kernel_swi_regs r;
    if (T.win) {
        r.r[1] = (intptr_t) &T.win;
        ro_swi(Wimp_CloseWindow, &r);
    }
}

/* Made (or changed): 1, or 0 (said why) */
static int apply(void)
{
    re_title t = T.t;
    if (!A.proj)
        return 0;
    if (T.line2[0])
        snprintf(t.text, sizeof(t.text), "%s\n%s", T.line1, T.line2);
    else
        snprintf(t.text, sizeof(t.text), "%s", T.line1);
    if (!T.line1[0] && !T.line2[0]) {
        app_report("Type the title's text first.");
        return 0;
    }
    re_project_begin(A.proj); /* (one undo: the title and its place) */
    int i = T.ov, done = 0;
    re_overlay o;
    if (i >= 0 && !re_project_overlay(A.proj, i, &o) && o.title) {
        done = re_project_title_set(A.proj, i, &t);
    } else {
        i = re_project_title_add(A.proj, &t, app_position(), 0);
        done = i >= 0;
    }
    if (i >= 0)
        done |= re_project_overlay_set(A.proj, i, T.place, -1, -1);
    re_project_end(A.proj);
    if (i < 0) {
        app_report("There's no room for a title at the playhead: the titles either side, or the project's end, are in "
                   "the way.");
        return 0;
    }
    app_log("title %d: \"%s\" (%s, %d%%, %06x, style %d, place %d)%s", i, t.text, t.font, t.size, t.colour, t.style,
            T.place, done ? "" : ": unchanged");
    A.ov_sel = i;
    A.sel = -1;
    app_edited();
    return 1;
}

void ti_click(int *b)
{
    if (b[4] == I_CANCEL && (b[2] & 5)) {
        ti_close();
        return;
    }
    if (b[4] == I_OK && (b[2] & 5)) {
        if (apply() && !(b[2] & 1))
            ti_close();
        return;
    }
    if (b[4] < I_FONT || b[4] > I_PLACE || ((b[4] - I_FONT) & 1) || !(b[2] & 5))
        return;
    /* a setting's button: its menu */
    static MENU(RE_PLACES) m;
    int which = (b[4] - I_FONT) / 2, n = 0;
    const char *const *names = NULL;
    int ticked = -1;
    switch (which) {
    case M_FONT: names = FONT_NAMES; n = 7; ticked = index_of_font(T.t.font); break;
    case M_SIZE: names = SIZE_NAMES; n = 5; ticked = index_of_size(T.t.size); break;
    case M_COLOUR: names = COLOUR_NAMES; n = 6; ticked = index_of_colour(T.t.colour); break;
    case M_STYLE: names = STYLE_NAMES; n = RE_TITLE_STYLES; ticked = T.t.style; break;
    case M_ALIGN: names = ALIGN_NAMES; n = 3; ticked = T.t.align; break;
    case M_PLACE: names = PLACE_NAMES; n = RE_PLACES - 1; ticked = T.place - 1; break;
    }
    static const char *const TITLES[6] = {"Font", "Size", "Colour", "Style", "Lines", "Place"};
    memset(&m, 0, sizeof(m));
    snprintf(m.title, sizeof(m.title), "%s", TITLES[which]);
    m.tfg = 7, m.tbg = 2, m.wfg = 7, m.wbg = 0;
    m.w = 280, m.h = 44;
    for (int i = 0; i < n; i++) {
        menu_item *it = &m.item[i];
        it->flags = (i == n - 1 ? 0x80 : 0) | (i == ticked ? 1 : 0);
        it->sub = -1;
        it->iflags = ITEM_FLAGS;
        it->text = names[i];
        it->valid = (const char *) -1;
        it->len = (int) strlen(names[i]) + 1;
    }
    T.menu = which;
    _kernel_swi_regs r;
    r.r[1] = (intptr_t) &m;
    r.r[2] = b[0] - 64;
    r.r[3] = b[1] + 20;
    ro_swi(Wimp_CreateMenu, &r);
    app_titles_menu_opened();
}

void ti_menu_select(int *b)
{
    int k = b[0];
    if (k < 0)
        return;
    switch (T.menu) {
    case M_FONT: if (k < 7) snprintf(T.t.font, sizeof(T.t.font), "%s", FONTS[k]); break;
    case M_SIZE: if (k < 5) T.t.size = SIZES[k]; break;
    case M_COLOUR: if (k < 6) T.t.colour = COLOURS[k]; break;
    case M_STYLE: if (k < RE_TITLE_STYLES) T.t.style = k; break;
    case M_ALIGN: if (k < 3) T.t.align = k; break;
    case M_PLACE: if (k < RE_PLACES - 1) T.place = k + 1; break;
    }
    show_settings();
}

void ti_key(int *b)
{
    _kernel_swi_regs r;
    if (b[6] == 13) { /* (Return on the second line: Ktar moves from the first) */
        if (apply())
            ti_close();
        return;
    }
    if (b[6] == 27) {
        ti_close();
        return;
    }
    r.r[0] = b[6];
    ro_swi(Wimp_ProcessKey, &r);
}

const char *ti_help(int icon)
{
    switch (icon) {
    case I_LINE1: case I_LINE2: return "Type the title's text here: two lines at most.|MReturn moves to the next line; "
                                       "on the second, it makes the title.";
    case I_FONT: return "Click to choose the title's font.";
    case I_SIZE: return "Click to choose how big the letters are.";
    case I_COLOUR: return "Click to choose the letters' colour.";
    case I_STYLE: return "Click to choose a shadow, an outline or a box behind the letters, or none.";
    case I_ALIGN: return "Click to choose how the two lines line up.";
    case I_PLACE: return "Click to choose where the title goes over the video.";
    case I_OK: return "Click to put the title over the video at the playhead (or change it).|MAdjust keeps this "
                      "window open.";
    case I_CANCEL: return "Click to close this window without changing anything.";
    default: return "This is the Title window: text over the video.";
    }
}

#ifdef RE_TEST
char *re_test_title_line(int n) { return n ? T.line2 : T.line1; }
#endif
