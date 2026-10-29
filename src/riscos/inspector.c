/* inspector.c - the Inspector (app.h): the selected item's settings on
 * screen, as Shotcut's Properties and Filters panels, beside the Viewer.
 * View > Inspector (Ctrl-I) opens and closes it; it opens with the editor
 * and, while the Timeline is attached, stays to the right of the Viewer
 * (part of the workspace).
 *
 *   a clip     Colour: Look (a menu), Brightness, Contrast, Saturation,
 *              Warmth, Midtones (sliders), Reset; Sound: Volume, Mute
 *   an overlay Place (a menu), Size, Opacity; a video's Volume and Mute;
 *              a title's Edit title...
 *   a sound    Volume, Mute
 *
 * A slider is set by clicking on it or dragging along it: the Viewer shows
 * the change as it's dragged, and the whole drag is one step for Undo.
 * Each control is a row of the window, drawn here (no icons).
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "app.h"
#include "menu.h"

#include <stdio.h>
#include <string.h>

#define WIDTH   600
#define HEAD_H  64
#define ROW_H   52
#define BAR_X0  220
#define BAR_X1  470
#define Wimp_DragBox        0x400D0
#define Wimp_GetPointerInfo 0x400CF
#define Wimp_TextOp         0x400F9
#define ColourTrans_SetGCOL 0x40743
#define OS_Plot             0x45

enum { K_HEADING, K_SLIDER, K_BUTTON, K_TOGGLE, K_TEXT, K_TABS }; /* (tabs: label and text the two names) */
enum { C_LOOK = 1, C_BRIGHTNESS, C_CONTRAST, C_SATURATION, C_WARMTH, C_MIDTONES, C_RESET, C_VOLUME, C_MUTE,
       O_PLACE, O_SIZE, O_OPACITY, O_TITLE, O_VOLUME, O_MUTE, A_VOLUME, A_MUTE,
       F_PAN, F_RUMBLE, F_VOICE, F_GATE, F_NORMAL, /* (F_: the sound's clean-up, of whatever is selected) */
       T_TABS };

typedef struct
{
    int kind, id;
    const char *label;
    int lo, hi, value;
    char text[48];   /* a button's, a toggle's or a value's words */
} control;

#define MAX_CONTROLS 26

static struct
{
    int win;
    char title[16];
    int drag_id;     /* the slider being dragged (0: none) */
    int drag_value;  /* its value when last set */
    int menu;        /* the menu open: C_LOOK or O_PLACE */
    int closed;      /* closed by the user: not opened with the editor */
    int tab;         /* the page shown: 0 Colour (or Overlay), 1 Sound */
} I = {0, "Inspector", 0, 0, 0, 0, 0};

static const char *const PLACE_NAMES[RE_PLACES] = {"Full frame", "Centre", "Top left", "Top right", "Bottom left",
                                                   "Bottom right", "Lower third"};

int in_window(void)
{
    return I.win;
}

/* ---- the controls for what's selected ----------------------------------------------------- */

static int add(control *c, int n, int kind, int id, const char *label, int lo, int hi, int value, const char *text)
{
    if (n >= MAX_CONTROLS)
        return n;
    c[n].kind = kind;
    c[n].id = id;
    c[n].label = label;
    c[n].lo = lo;
    c[n].hi = hi;
    c[n].value = value < lo ? lo : value > hi ? hi : value;
    snprintf(c[n].text, sizeof(c[n].text), "%s", text ? text : "");
    return n + 1;
}

/* The selection's sound clean-up: 0, or -1 (none: nothing with sound selected) */
static int sel_fx(re_sound_fx *fx)
{
    if (A.sel >= 0)
        return re_project_sound_fx(A.proj, A.sel, fx);
    if (A.ov_sel >= 0)
        return re_project_overlay_sound_fx(A.proj, A.ov_sel, fx);
    if (A.au_sel >= 0)
        return re_project_audio_sound_fx(A.proj, A.au_track, A.au_sel, fx);
    return -1;
}

static int sel_set_fx(const re_sound_fx *fx)
{
    if (A.sel >= 0)
        return re_project_set_sound_fx(A.proj, A.sel, fx);
    if (A.ov_sel >= 0)
        return re_project_overlay_set_sound_fx(A.proj, A.ov_sel, fx);
    if (A.au_sel >= 0)
        return re_project_audio_set_sound_fx(A.proj, A.au_track, A.au_sel, fx);
    return 0;
}

/* The sound clean-up's rows (after Volume and Mute) */
static int fx_rows(control *c, int n)
{
    re_sound_fx fx;
    if (sel_fx(&fx))
        return n;
    n = add(c, n, K_SLIDER, F_PAN, "Pan", -100, 100, fx.pan, NULL);
    n = add(c, n, K_TOGGLE, F_RUMBLE, "Rumble", 0, 1, fx.rumble, fx.rumble ? "Cut" : "As it is");
    n = add(c, n, K_TOGGLE, F_VOICE, "Voices", 0, 1, fx.voice, fx.voice ? "Clearer" : "As they are");
    n = add(c, n, K_TOGGLE, F_GATE, "Background", 0, 1, fx.gate, fx.gate ? "Quieter between sounds" : "As it is");
    n = add(c, n, K_BUTTON, F_NORMAL, "", 0, 0, 0, "Normalise the level");
    return n;
}

/* What's selected, in words (for the window's head) */
static void what(char *t, int size)
{
    re_item it;
    re_overlay o;
    re_audio a;
    if (!A.proj)
        snprintf(t, size, "No project");
    else if (A.sel >= 0 && !re_project_item(A.proj, A.sel, &it) && !it.gap && !it.mix)
        snprintf(t, size, "Clip: %s", it.name);
    else if (A.ov_sel >= 0 && !re_project_overlay(A.proj, A.ov_sel, &o))
        snprintf(t, size, "%s: %s", o.title ? "Title" : o.video ? "Picture in picture" : "Overlay", o.name);
    else if (A.au_sel >= 0 && !re_project_audio(A.proj, A.au_track, A.au_sel, &a))
        snprintf(t, size, "Sound: %s", a.name);
    else
        snprintf(t, size, "Select a clip, overlay, title or sound");
}

static int controls(control *c)
{
    int n = 0;
    re_item it;
    re_overlay o;
    re_audio a;
    char v[48];
    if (!A.proj)
        return 0;
    if (A.sel >= 0 && !re_project_item(A.proj, A.sel, &it) && !it.gap && !it.mix) {
        n = add(c, n, K_TABS, T_TABS, "Colour", 0, 1, I.tab, "Sound");
        if (I.tab == 0) {
            re_colour col;
            re_project_colour(A.proj, A.sel, &col);
            int look = re_colour_look_of(&col);
            n = add(c, n, K_BUTTON, C_LOOK, "Look", 0, 0, 0, look >= 0 ? re_colour_looks[look] : "Your own");
            n = add(c, n, K_SLIDER, C_BRIGHTNESS, "Brightness", -100, 100, col.brightness, NULL);
            n = add(c, n, K_SLIDER, C_CONTRAST, "Contrast", 0, 200, col.contrast, NULL);
            n = add(c, n, K_SLIDER, C_SATURATION, "Saturation", 0, 200, col.saturation, NULL);
            n = add(c, n, K_SLIDER, C_WARMTH, "Warmth", -100, 100, col.warmth, NULL);
            n = add(c, n, K_SLIDER, C_MIDTONES, "Midtones", -100, 100, col.midtones, NULL);
            n = add(c, n, K_BUTTON, C_RESET, "", 0, 0, 0, "Reset the colour");
            if (it.speed != 100) {
                snprintf(v, sizeof(v), it.speed < 0 ? "Speed: reversed, %d%%" : "Speed: %d%%",
                         it.speed < 0 ? -it.speed : it.speed);
                n = add(c, n, K_TEXT, 0, "", 0, 0, 0, v);
            }
        } else if (it.sound) {
            n = add(c, n, K_SLIDER, C_VOLUME, "Volume", -36, 12, it.volume, NULL);
            n = add(c, n, K_TOGGLE, C_MUTE, "Mute", 0, 1, it.muted, it.muted ? "Muted" : "Not muted");
            n = fx_rows(c, n);
        } else {
            n = add(c, n, K_TEXT, 0, "", 0, 0, 0, "This clip has no sound.");
        }
    } else if (A.ov_sel >= 0 && !re_project_overlay(A.proj, A.ov_sel, &o)) {
        int sound = o.video && o.sound;
        if (sound)
            n = add(c, n, K_TABS, T_TABS, o.title ? "Title" : "Overlay", 0, 1, I.tab, "Sound");
        else
            n = add(c, n, K_HEADING, 0, o.title ? "Title" : "Overlay", 0, 0, 0, NULL);
        if (!sound || I.tab == 0) {
            if (o.title)
                n = add(c, n, K_BUTTON, O_TITLE, "Text", 0, 0, 0, "Edit title...");
            n = add(c, n, K_BUTTON, O_PLACE, "Place", 0, 0, 0,
                    PLACE_NAMES[o.place >= 0 && o.place < RE_PLACES ? o.place : 0]);
            if (!o.title && o.place != RE_PLACE_FULL)
                n = add(c, n, K_SLIDER, O_SIZE, "Size", 10, 100, o.size, NULL);
            n = add(c, n, K_SLIDER, O_OPACITY, "Opacity", 0, 100, o.opacity, NULL);
        } else {
            n = add(c, n, K_SLIDER, O_VOLUME, "Volume", -36, 12, o.volume, NULL);
            n = add(c, n, K_TOGGLE, O_MUTE, "Mute", 0, 1, o.muted, o.muted ? "Muted" : "Not muted");
            n = fx_rows(c, n);
        }
    } else if (A.au_sel >= 0 && !re_project_audio(A.proj, A.au_track, A.au_sel, &a)) {
        n = add(c, n, K_HEADING, 0, "Sound", 0, 0, 0, NULL);
        n = add(c, n, K_SLIDER, A_VOLUME, "Volume", -36, 12, a.volume, NULL);
        n = add(c, n, K_TOGGLE, A_MUTE, "Mute", 0, 1, a.muted, a.muted ? "Muted" : "Not muted");
        n = fx_rows(c, n);
    }
    return n;
}

static void value_text(const control *c, char *t, int size)
{
    switch (c->id) {
    case C_CONTRAST: case C_SATURATION: case O_SIZE: case O_OPACITY:
        snprintf(t, size, "%d%%", c->value);
        break;
    case C_VOLUME: case O_VOLUME: case A_VOLUME:
        snprintf(t, size, "%+d dB", c->value);
        break;
    case F_PAN:
        if (c->value == 0)
            snprintf(t, size, "Middle");
        else
            snprintf(t, size, "%s %d", c->value < 0 ? "L" : "R", c->value < 0 ? -c->value : c->value);
        break;
    default:
        snprintf(t, size, "%+d", c->value);
    }
}

/* ---- the window ---------------------------------------------------------------------------- */

static int height(void)
{
    control c[MAX_CONTROLS];
    int h = HEAD_H + controls(c) * ROW_H + 24;
    return h < 400 ? 400 : h;
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
    w.vis.x1 = WIDTH; w.vis.y1 = 600;
    w.behind = -1;
    w.flags = (int) 0xBF000002u; /* new format, back, close, title, toggle, vertical scroll, adjust size, moveable */
    w.tfg = 7; w.tbg = 2; w.wfg = 7; w.wbg = 1; w.sofg = 3; w.sibg = 1; w.tfocus = 12;
    w.ext.x0 = 0; w.ext.y0 = -1200; w.ext.x1 = WIDTH; w.ext.y1 = 0;
    w.tflags = 0x1 | 0x4 | 0x8 | 0x10 | 0x20 | 0x100 | (7 << 24) | (2 << 28);
    w.wbutton = 10 << 12; /* click, drag, double click */
    w.sprites = 1;
    w.minw = WIDTH; w.minh = 200;
    w.title.text = I.title;
    w.title.valid = (const char *) -1;
    w.title.len = sizeof(I.title);
    r.r[1] = (intptr_t) &w;
    if (ro_swi(Wimp_CreateWindow, &r))
        return -1;
    I.win = (int) r.r[0];
    return 0;
}

static int is_open(void)
{
    int st[9];
    _kernel_swi_regs r;
    if (!I.win)
        return 0;
    st[0] = I.win;
    r.r[1] = (intptr_t) st;
    return !ro_swi(Wimp_GetWindowState, &r) && (st[8] & (1 << 16));
}

int in_is_open(void)
{
    return is_open();
}

static void redraw_all(void)
{
    _kernel_swi_regs r;
    if (!I.win)
        return;
    int h = height(), ext[4] = {0, -(h > 1200 ? h : 1200), WIDTH, 0};
    r.r[0] = I.win;
    r.r[1] = (intptr_t) ext;
    ro_swi(Wimp_SetExtent, &r);
    r.r[0] = I.win; r.r[1] = 0; r.r[2] = ext[1]; r.r[3] = WIDTH; r.r[4] = 0;
    ro_swi(Wimp_ForceRedraw, &r);
}

void in_edited(void)
{
    if (is_open())
        redraw_all();
}

int in_width(void)
{
    return I.closed ? 0 : WIDTH + 8;
}

void in_place(int x1, int y0, int y1, int behind)
{
    int st[9];
    _kernel_swi_regs r;
    if (!is_open() || !A.attached)
        return;
    st[0] = I.win;
    r.r[1] = (intptr_t) st;
    if (ro_swi(Wimp_GetWindowState, &r))
        return;
    int sw = app_screen_w(), x0 = x1 + 8;
    if (x0 + WIDTH > sw) /* (no room to the right: at the screen's edge) */
        x0 = sw - WIDTH - 40;
    if (st[1] == x0 && st[2] == y0 && st[4] == y1)
        return;
    st[1] = x0; st[3] = x0 + WIDTH; st[2] = y0; st[4] = y1;
    st[7] = behind;
    r.r[1] = (intptr_t) st;
    ro_swi(Wimp_OpenWindow, &r);
}

void in_open(void)
{
    _kernel_swi_regs r;
    int st[9];
    if (!I.win && create() < 0)
        return;
    st[0] = I.win;
    r.r[1] = (intptr_t) st;
    ro_swi(Wimp_GetWindowState, &r);
    if (!(st[8] & (1 << 16))) { /* (somewhere to start: app_place_inspector moves it beside the Viewer) */
        st[1] = app_screen_w() - WIDTH - 40; st[3] = st[1] + WIDTH; st[4] = 1400; st[2] = 800;
        st[5] = 0; st[6] = 0;
    }
    st[7] = -1;
    r.r[1] = (intptr_t) st;
    ro_swi(Wimp_OpenWindow, &r);
    app_place_inspector();
    redraw_all();
}

void in_close(void)
{
    _kernel_swi_regs r;
    I.drag_id = 0;
    if (I.win) {
        r.r[1] = (intptr_t) &I.win;
        ro_swi(Wimp_CloseWindow, &r);
    }
}

void in_toggle(void)
{
    if (is_open()) {
        in_close();
        I.closed = 1;
    } else {
        I.closed = 0;
        in_open();
    }
    app_place_inspector();
    app_log("inspector %s", is_open() ? "open" : "closed");
}

/* ---- drawing ------------------------------------------------------------------------------- */

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
    ro_swi(OS_Plot, &r);
    r.r[0] = 96 + 5; r.r[1] = x1; r.r[2] = y1;
    ro_swi(OS_Plot, &r);
}

#define GREY   0xDDDDDD00u
#define DARK   0x55555500u
#define BLUE   0xC0702000u /* (32, 112, 192) */
#define WHITE  0xFFFFFF00u
#define BLACK  0x00000000u

static void button(int x0, int y0, int x1, int y1, const char *t, unsigned bg)
{
    rect(x0, y0, x1, y1, DARK);
    rect(x0 + 2, y0 + 2, x1 - 2, y1 - 2, bg);
    text(t, x0 + 16, y0 + 10, BLACK, bg);
}

static int slider_x(const control *c)
{
    return BAR_X0 + (BAR_X1 - BAR_X0) * (c->value - c->lo) / (c->hi > c->lo ? c->hi - c->lo : 1);
}

void in_redraw(int *b)
{
    _kernel_swi_regs r;
    control c[MAX_CONTROLS];
    char head[96], v[32];
    int n = controls(c);
    what(head, sizeof(head));
    r.r[1] = (intptr_t) b;
    int more = ro_swi(Wimp_RedrawWindow, &r) ? 0 : (int) r.r[0];
    while (more) {
        int ox = b[1] - b[5], oy = b[4] - b[6];
        rect(ox, oy - HEAD_H, ox + WIDTH, oy, WHITE);
        text(head, ox + 16, oy - 44, BLACK, WHITE);
        for (int i = 0; i < n; i++) {
            int y1 = oy - HEAD_H - i * ROW_H, y0 = y1 - ROW_H;
            if (y1 < b[8] || y0 > b[10])
                continue;
            const control *k = &c[i];
            switch (k->kind) {
            case K_HEADING:
                rect(ox + 8, y0 + 8, ox + WIDTH - 8, y0 + 10, DARK);
                text(k->label, ox + 16, y0 + 18, BLACK, GREY);
                break;
            case K_SLIDER: {
                text(k->label, ox + 32, y0 + 14, BLACK, GREY);
                int mid = y0 + ROW_H / 2;
                rect(ox + BAR_X0, mid - 4, ox + BAR_X1, mid + 4, DARK);
                int zero = k->lo < 0 ? 0 : k->id == C_CONTRAST || k->id == C_SATURATION ? 100 : k->lo;
                control z = *k;
                z.value = zero;
                int zx = slider_x(&z), kx = slider_x(k);
                rect(ox + (zx < kx ? zx : kx), mid - 4, ox + (zx < kx ? kx : zx), mid + 4, BLUE);
                rect(ox + kx - 6, mid - 14, ox + kx + 6, mid + 14, k->id == I.drag_id ? BLUE : BLACK);
                value_text(k, v, sizeof(v));
                text(v, ox + BAR_X1 + 16, y0 + 14, BLACK, GREY);
                break;
            }
            case K_BUTTON:
                if (k->label[0])
                    text(k->label, ox + 32, y0 + 14, BLACK, GREY);
                button(ox + BAR_X0, y0 + 4, ox + WIDTH - 24, y1 - 4, k->text, WHITE);
                break;
            case K_TOGGLE:
                text(k->label, ox + 32, y0 + 14, BLACK, GREY);
                button(ox + BAR_X0, y0 + 4, ox + WIDTH - 24, y1 - 4, k->text, k->value ? 0x8080F000u : WHITE);
                break;
            case K_TEXT:
                text(k->text, ox + 32, y0 + 14, DARK, GREY);
                break;
            case K_TABS: {
                int mid = ox + WIDTH / 2;
                button(ox + 16, y0 + 6, mid - 4, y1 - 2, k->label, k->value == 0 ? 0xE0C09000u : WHITE);
                button(mid + 4, y0 + 6, ox + WIDTH - 24, y1 - 2, k->text, k->value == 1 ? 0xE0C09000u : WHITE);
                break;
            }
            }
        }
        r.r[1] = (intptr_t) b;
        more = ro_swi(Wimp_GetRectangle, &r) ? 0 : (int) r.r[0];
    }
}

/* ---- settings changed ----------------------------------------------------------------------- */

/* A control set to a value: 1 changed */
static int set(int id, int value)
{
    re_colour col;
    re_item it;
    re_overlay o;
    re_audio a;
    int done = 0;
    if (!A.proj)
        return 0;
    switch (id) {
    case C_BRIGHTNESS: case C_CONTRAST: case C_SATURATION: case C_WARMTH: case C_MIDTONES:
        if (re_project_colour(A.proj, A.sel, &col))
            return 0;
        if (id == C_BRIGHTNESS) col.brightness = value;
        if (id == C_CONTRAST) col.contrast = value;
        if (id == C_SATURATION) col.saturation = value;
        if (id == C_WARMTH) col.warmth = value;
        if (id == C_MIDTONES) col.midtones = value;
        done = re_project_set_colour(A.proj, A.sel, &col);
        break;
    case C_VOLUME:
        done = re_project_volume(A.proj, A.sel, value);
        break;
    case C_MUTE:
        done = !re_project_item(A.proj, A.sel, &it) && re_project_mute(A.proj, A.sel, !it.muted);
        break;
    case O_SIZE:
        done = re_project_overlay_set(A.proj, A.ov_sel, -1, value, -1);
        break;
    case O_OPACITY:
        done = re_project_overlay_set(A.proj, A.ov_sel, -1, -1, value);
        break;
    case O_VOLUME:
        done = re_project_overlay_volume(A.proj, A.ov_sel, value);
        break;
    case O_MUTE:
        done = !re_project_overlay(A.proj, A.ov_sel, &o) && re_project_overlay_mute(A.proj, A.ov_sel, !o.muted);
        break;
    case A_VOLUME:
        done = re_project_audio_volume(A.proj, A.au_track, A.au_sel, value);
        break;
    case F_PAN: case F_RUMBLE: case F_VOICE: case F_GATE: {
        re_sound_fx fx;
        if (sel_fx(&fx))
            return 0;
        if (id == F_PAN) fx.pan = value;
        if (id == F_RUMBLE) fx.rumble = !fx.rumble;
        if (id == F_VOICE) fx.voice = !fx.voice;
        if (id == F_GATE) fx.gate = !fx.gate;
        done = sel_set_fx(&fx);
        break;
    }
    case A_MUTE:
        done = !re_project_audio(A.proj, A.au_track, A.au_sel, &a)
               && re_project_audio_mute(A.proj, A.au_track, A.au_sel, !a.muted);
        break;
    }
    return done;
}

/* The control at a pointer height (work area), or -1 */
static int row_at(int wy, int n)
{
    int i = (-wy - HEAD_H) / ROW_H;
    return -wy >= HEAD_H && i >= 0 && i < n ? i : -1;
}

static int value_at(const control *k, int wx)
{
    int x = wx < BAR_X0 ? BAR_X0 : wx > BAR_X1 ? BAR_X1 : wx;
    int v = k->lo + (int) ((long) (x - BAR_X0) * (k->hi - k->lo) * 2 + (BAR_X1 - BAR_X0)) / (2 * (BAR_X1 - BAR_X0));
    return v < k->lo ? k->lo : v > k->hi ? k->hi : v;
}

static void open_menu(int which, int x, int y)
{
    static MENU(RE_PLACES > RE_LOOKS ? RE_PLACES : RE_LOOKS) m;
    re_colour col;
    re_overlay o;
    int n = which == C_LOOK ? RE_LOOKS : RE_PLACES, ticked = -1;
    if (which == C_LOOK && !re_project_colour(A.proj, A.sel, &col))
        ticked = re_colour_look_of(&col);
    if (which == O_PLACE && !re_project_overlay(A.proj, A.ov_sel, &o))
        ticked = o.place;
    memset(&m, 0, sizeof(m));
    snprintf(m.title, sizeof(m.title), "%s", which == C_LOOK ? "Look" : "Place");
    m.tfg = 7, m.tbg = 2, m.wfg = 7, m.wbg = 0;
    m.w = 320, m.h = 44;
    for (int i = 0; i < n; i++) {
        menu_item *it = &m.item[i];
        const char *name = which == C_LOOK ? re_colour_looks[i] : PLACE_NAMES[i];
        it->flags = (i == n - 1 ? 0x80 : 0) | (i == ticked ? 1 : 0);
        it->sub = -1;
        it->iflags = ITEM_FLAGS;
        it->text = name;
        it->valid = (const char *) -1;
        it->len = (int) strlen(name) + 1;
    }
    I.menu = which;
    _kernel_swi_regs r;
    r.r[1] = (intptr_t) &m;
    r.r[2] = x - 64;
    r.r[3] = y + 20;
    ro_swi(Wimp_CreateMenu, &r);
    app_inspector_menu_opened();
}

void in_menu_select(int *b)
{
    int k = b[0];
    if (k < 0 || !A.proj)
        return;
    int done = 0;
    if (I.menu == C_LOOK && k < RE_LOOKS) {
        re_colour col;
        re_colour_look(k, &col);
        done = re_project_set_colour(A.proj, A.sel, &col);
        app_log("inspector: look %s", re_colour_looks[k]);
    } else if (I.menu == O_PLACE && k < RE_PLACES) {
        done = re_project_overlay_set(A.proj, A.ov_sel, k, -1, -1);
        app_log("inspector: place %s", PLACE_NAMES[k]);
    }
    if (done)
        app_edited();
}

/* An item's normalising level (kind 0 a clip, 1 an overlay, 2 a sound on
   track k) from its file's peaks over the part used (the waveform's, made
   in the background): 0, 1 (its waveform isn't made yet), 2 (too little
   sound) or 3 (no sound) */
static int level_of(int kind, int k, int i, int *db)
{
    const char *file = NULL;
    int f0 = 0, f1 = 0, n = 0, done = 0;
    re_item it;
    re_overlay o;
    re_audio a;
    if (kind == 0 && !re_project_item(A.proj, i, &it) && !it.gap && !it.mix && it.sound) {
        file = re_project_item_file(A.proj, i);
        f0 = re_item_file_frame(&it, it.in);
        f1 = re_item_file_frame(&it, it.out) + 1;
        if (f1 < f0) { /* (reversed) */
            int t = f0;
            f0 = f1 - 1;
            f1 = t + 1;
        }
    } else if (kind == 1 && !re_project_overlay(A.proj, i, &o) && o.video && o.sound) {
        file = re_project_overlay_file(A.proj, i);
        f0 = o.in;
        f1 = o.in + o.length;
    } else if (kind == 2 && !re_project_audio(A.proj, k, i, &a)) {
        file = re_project_audio_file(A.proj, k, i);
        f0 = a.in;
        f1 = a.in + a.length;
    }
    if (!file)
        return 3;
    const unsigned char *pk = pv_peaks(file, &n, &done);
    if (!pk || !done)
        return 1;
    return re_normal_level(pk, n, f0, f1, db) ? 2 : 0;
}

static int set_level(int kind, int k, int i, int db)
{
    return kind == 0 ? re_project_volume(A.proj, i, db)
           : kind == 1 ? re_project_overlay_volume(A.proj, i, db)
                       : re_project_audio_volume(A.proj, k, i, db);
}

/* The selection's sound made as loud as the rest */
static void normalise(void)
{
    int kind = A.sel >= 0 ? 0 : A.ov_sel >= 0 ? 1 : 2, i = kind == 0 ? A.sel : kind == 1 ? A.ov_sel : A.au_sel;
    int db = 0, r = level_of(kind, A.au_track, i, &db);
    if (r == 1)
        app_report("Its waveform is still being made: try again in a moment.");
    else if (r)
        app_report("There's too little sound there to normalise.");
    if (r)
        return;
    app_log("inspector: normalised to %+d dB", db);
    if (set_level(kind, A.au_track, i, db))
        app_edited();
}

void in_normalise_all(void)
{
    int done = 0, waiting = 0, db;
    if (!A.proj)
        return;
    re_project_begin(A.proj); /* (one undo for all of it) */
    for (int i = 0; i < re_project_items(A.proj); i++) {
        int r = level_of(0, 0, i, &db);
        if (!r)
            done += set_level(0, 0, i, db) >= 0;
        waiting += r == 1;
    }
    for (int i = 0; i < re_project_overlays(A.proj); i++) {
        int r = level_of(1, 0, i, &db);
        if (!r)
            done += set_level(1, 0, i, db) >= 0;
        waiting += r == 1;
    }
    for (int k = 0; k < re_project_audio_tracks(A.proj); k++)
        for (int i = 0; i < re_project_audio_count(A.proj, k); i++) {
            int r = level_of(2, k, i, &db);
            if (!r)
                done += set_level(2, k, i, db) >= 0;
            waiting += r == 1;
        }
    re_project_end(A.proj);
    app_log("normalised: %d, %d waiting for their waveforms", done, waiting);
    if (waiting) {
        char t[160];
        snprintf(t, sizeof(t), "%d of the sounds have been normalised; %d are still having their waveforms made: "
                 "choose this again in a moment for those.", done, waiting);
        app_report(t);
    }
    app_edited();
}

void in_click(int *b)
{
    int st[9];
    _kernel_swi_regs r;
    control c[MAX_CONTROLS];
    st[0] = I.win;
    r.r[1] = (intptr_t) st;
    if (ro_swi(Wimp_GetWindowState, &r) || !A.proj)
        return;
    if (b[2] & 2) { /* Menu: the editor's */
        app_menu(b[0], b[1]);
        return;
    }
    int n = controls(c), wx = b[0] - (st[1] - st[5]), wy = b[1] - (st[4] - st[6]);
    int i = row_at(wy, n);
    if (i < 0)
        return;
    control *k = &c[i];
    if (k->kind == K_SLIDER && wx >= BAR_X0 - 16 && wx <= BAR_X1 + 16) {
        if (b[2] & 0x40) { /* a drag: followed on null events, one undo */
            int d[10];
            d[0] = I.win; d[1] = 7; /* (a drag the program follows) */
            d[2] = d[4] = b[0]; d[3] = d[5] = b[1];
            d[6] = -0x10000; d[7] = -0x10000; d[8] = 0x10000; d[9] = 0x10000;
            r.r[1] = (intptr_t) d;
            if (!ro_swi(Wimp_DragBox, &r)) {
                re_project_begin(A.proj);
                I.drag_id = k->id;
                I.drag_value = k->value;
                app_log("inspector: dragging %s", k->label);
            }
            return;
        }
        if (b[2] & 0x500) { /* a click (its first report): set there, one undo */
            int v = value_at(k, wx);
            if (set(k->id, v)) {
                app_log("inspector: %s %d", k->label, v);
                app_edited();
            }
        }
        return;
    }
    if (!(b[2] & 0x500))
        return;
    switch (k->id) {
    case T_TABS:
        I.tab = wx < WIDTH / 2 ? 0 : 1;
        redraw_all();
        break;
    case C_LOOK: case O_PLACE:
        open_menu(k->id, b[0], b[1]);
        break;
    case C_RESET: {
        re_colour none;
        re_colour_none(&none);
        if (re_project_set_colour(A.proj, A.sel, &none)) {
            app_log("inspector: colour reset");
            app_edited();
        }
        break;
    }
    case O_TITLE:
        ti_open(A.ov_sel);
        break;
    case F_NORMAL:
        normalise();
        break;
    case C_MUTE: case O_MUTE: case A_MUTE: case F_RUMBLE: case F_VOICE: case F_GATE:
        if (set(k->id, 0)) {
            app_log("inspector: %s", k->label);
            app_edited();
        }
        break;
    }
}

int in_dragging(void)
{
    return I.drag_id != 0;
}

void in_drag_tick(void)
{
    int p[5], st[9];
    _kernel_swi_regs r;
    control c[MAX_CONTROLS];
    if (!I.drag_id || !A.proj)
        return;
    r.r[1] = (intptr_t) p;
    st[0] = I.win;
    if (ro_swi(Wimp_GetPointerInfo, &r))
        return;
    r.r[1] = (intptr_t) st;
    if (ro_swi(Wimp_GetWindowState, &r))
        return;
    int n = controls(c);
    for (int i = 0; i < n; i++) {
        if (c[i].id != I.drag_id)
            continue;
        int v = value_at(&c[i], p[0] - (st[1] - st[5]));
        if (v != I.drag_value) {
            I.drag_value = v;
            if (set(I.drag_id, v))
                app_edited_dragging();
            redraw_all();
        }
        return;
    }
}

void in_drag_end(int *b)
{
    (void) b;
    if (!I.drag_id)
        return;
    in_drag_tick();
    I.drag_id = 0;
    if (A.proj)
        re_project_end(A.proj);
    app_log("inspector: dragged to %d", I.drag_value);
    app_edited();
    redraw_all();
}

void in_key(int *b)
{
    _kernel_swi_regs r;
    if (app_key(b[6]))
        return;
    r.r[0] = b[6];
    ro_swi(Wimp_ProcessKey, &r);
}

const char *in_help(void)
{
    return "This is the Inspector: the settings of what's selected on the Timeline.|MClick or drag along a slider "
           "to set it (the Viewer shows it as you go); click Look or Place for a menu.|MCtrl-I closes or opens it.";
}

#ifdef RE_TEST
int re_test_inspector_controls(void)
{
    control c[MAX_CONTROLS];
    return controls(c);
}
#endif
