/* overlays.c - pictures and videos over the video (app.h): dropped on V2's row of the
 * Timeline (or on the Viewer: at the playhead), and their menu. What they
 * do to the project is project.h's (overlay.c); this is the desktop's
 * side: which file, which sprite, where, the menu.
 *
 * Videos dropped on V2 are overlays too: a picture in the picture, its
 * sound mixed with V1's.
 *
 * Pictures: PNG (with its alpha), JPEG, and sprite files. A sprite is made
 * a PNG first (sprites.h, still.h), beside its file ("Logos" sprite "bug":
 * Logos_bug/png), the file left as it was: MLT, and Shotcut, read PNGs. A
 * file of more sprites than one gives a menu of them to pick from.
 * Dropped on V1, a picture is a clip there (5 seconds).
 *
 * The menu, over an overlay ("Overlay"):
 *   Place      Full frame, Centre, Top left, Top right, Bottom left,
 *              Bottom right, Lower third
 *   Size       the box it's fitted in: 10% to 100% of the frame (not with
 *              Full frame)
 *   Opacity    100%, 75%, 50%, 25%
 *   Fade in, Fade out    a second; ticked when it has one, chosen again it
 *              goes (a video's sound fades with it)
 *   Sound      a video's: Mute; Fade in, Fade out (the sound alone)
 *   Delete
 * Adjust keeps the menu open, to try another.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "../sprites.h"
#include "../still.h"
#include "app.h"
#include "menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define Wimp_CreateMenu     0x400D4
#define Wimp_GetPointerInfo 0x400CF

static struct
{
    int menu;          /* 1: the overlay's menu is open, 2: the sprites' */
    int ov, x, y;      /* the overlay's menu: which, where */
    /* a sprite file's sprites, to pick one from: the file, where it goes */
    char path[256];
    int frame, overlay;
    char names[RE_SPRITES_MAX][13];
} O = {0, -1, 0, 0, "", -1, 0, {{0}}};

static int ends_with(const char *s, const char *tail)
{
    size_t n = strlen(s), t = strlen(tail);
    return n > t && !strcmp(s + n - t, tail) && (s[n - t - 1] == '/' || s[n - t - 1] == '.');
}

int ov_is_picture(const char *path, int filetype)
{
    return filetype == FILETYPE_PNG || filetype == FILETYPE_JPEG || filetype == FILETYPE_SPRITE
           || ends_with(path, "png") || ends_with(path, "jpg") || ends_with(path, "jpeg");
}

static const char *leaf(const char *path)
{
    const char *l = strrchr(path, '.');
    return l ? l + 1 : path;
}

/* ---- dropped ---------------------------------------------------------------------------------- */

/* A picture (PNG or JPEG) put in: over the video, or on V1; or a video
   over the video */
static void use(const char *path, int frame, int overlay, int picture)
{
    char unix_name[512], t[400];
    const char *name = app_mlt_name(path, unix_name, sizeof(unix_name));
    if (!overlay) {
        if (!picture) {
            app_add(path, name, frame);
            return;
        }
        char q[600];
        snprintf(q, sizeof(q), "qimage:%s", name); /* (a picture, whatever its name: still.c) */
        app_add(path, q, frame);
        return;
    }
    if (!A.proj || re_project_length(A.proj) <= 0) {
        app_report("Put some video on V1 first: overlays go over it.");
        return;
    }
    int i = re_project_overlay_add(A.proj, name, frame < 0 ? 0 : frame, 0);
    app_log("overlay %s at %d: %d", path, frame, i);
    if (i == RE_OVERLAY_CANT_OPEN) {
        snprintf(t, sizeof(t), "Reel Edit can't open %.200s: is it a picture (PNG, JPEG, sprite) or a video?",
                 leaf(path));
        app_report(t);
        return;
    }
    if (i == RE_OVERLAY_NO_LENGTH) {
        snprintf(t, sizeof(t), "%.200s doesn't say how long it is (raw video), so it can't go on V2 yet: drop it "
                 "on V1.", leaf(path));
        app_report(t);
        return;
    }
    if (i < 0) {
        app_report("There's no room on V2 there: the overlays either side, or the project's end, are in the way.");
        return;
    }
    re_overlay o;
    A.ov_sel = i;
    A.sel = -1;
    app_edited();
    if (!re_project_overlay(A.proj, i, &o))
        app_seek(o.start);
}

/* A sprite of the file made a PNG beside it, and put in */
static void use_sprite(const char *path, int index, int frame, int overlay)
{
    char unix_name[512], out[300], unix_out[512], names[RE_SPRITES_MAX][13], t[400];
    const char *name = app_mlt_name(path, unix_name, sizeof(unix_name));
    int n = re_sprites_names(name, names, RE_SPRITES_MAX);
    uint8_t *rgba = NULL;
    int w, h;
    if (index < 0 || index >= n || re_sprite_rgba(name, index, &rgba, &w, &h)) {
        snprintf(t, sizeof(t), "Reel Edit can't read %.200s's sprite: its kind of sprite isn't known.", leaf(path));
        app_report(t);
        return;
    }
    char safe[13];
    int k = 0;
    for (const char *c = names[index]; *c && k < 12; c++)
        safe[k++] = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ? *c : '_';
    safe[k] = 0;
#ifdef __riscos__
    snprintf(out, sizeof(out), "%.240s_%s/png", path, safe);
#else
    snprintf(out, sizeof(out), "%.240s_%s.png", path, safe);
#endif
    int bad = re_still_write_png(app_mlt_name(out, unix_out, sizeof(unix_out)), rgba, w, h);
    free(rgba);
    app_log("sprite %s (%s, %dx%d) as %s: %s", names[index], path, w, h, out, bad ? "not written" : "written");
    if (bad) {
        snprintf(t, sizeof(t), "Reel Edit couldn't save the sprite as a PNG beside %.200s: is the disc locked or full?",
                 leaf(path));
        app_report(t);
        return;
    }
#ifdef __riscos__
    _kernel_swi_regs r;
    r.r[0] = 18; /* OS_File 18: its filetype */
    r.r[1] = (intptr_t) out;
    r.r[2] = FILETYPE_PNG;
    ro_swi(0x08, &r);
#endif
    use(out, frame, overlay, 1);
}

static MENU(RE_SPRITES_MAX) m_sprites;

void ov_load(const char *path, int filetype, int frame, int overlay)
{
    char unix_name[512], t[400];
    int sprite = filetype == FILETYPE_SPRITE;
    if (!sprite) {
        use(path, frame, overlay, ov_is_picture(path, filetype));
        return;
    }
    int n = re_sprites_names(app_mlt_name(path, unix_name, sizeof(unix_name)), O.names, RE_SPRITES_MAX);
    if (n < 1) {
        snprintf(t, sizeof(t), "%.200s isn't a sprite file Reel Edit can read.", leaf(path));
        app_report(t);
        return;
    }
    if (n == 1) {
        use_sprite(path, 0, frame, overlay);
        return;
    }
    /* more than one: a menu of them, at the pointer */
    snprintf(O.path, sizeof(O.path), "%s", path);
    O.frame = frame;
    O.overlay = overlay;
    memset(&m_sprites, 0, sizeof(m_sprites));
    memcpy(m_sprites.title, "Sprite", 7);
    m_sprites.tfg = 7, m_sprites.tbg = 2, m_sprites.wfg = 7, m_sprites.wbg = 0;
    m_sprites.w = 200, m_sprites.h = 44;
    for (int i = 0; i < n; i++) {
        menu_item *it = &m_sprites.item[i];
        it->flags = i == n - 1 ? 0x80 : 0;
        it->sub = -1;
        it->iflags = ITEM_FLAGS;
        it->text = O.names[i];
        it->valid = (const char *) -1;
        it->len = 13;
        int wi = (int) strlen(O.names[i]) * 16 + 32;
        if (wi > m_sprites.w)
            m_sprites.w = wi;
    }
    int p[5];
    _kernel_swi_regs r;
    r.r[1] = (intptr_t) p;
    if (ro_swi(Wimp_GetPointerInfo, &r))
        p[0] = p[1] = 400;
    O.menu = 2;
    r.r[1] = (intptr_t) &m_sprites;
    r.r[2] = p[0] - 64;
    r.r[3] = p[1];
    ro_swi(Wimp_CreateMenu, &r);
    app_log("sprites in %s: %d, a menu of them", path, n);
    app_overlays_menu_opened();
}

/* ---- the overlay's menu ------------------------------------------------------------------------- */

int ov_fade_toggle(int ov, int end)
{
    re_overlay o;
    if (!A.proj || re_project_overlay(A.proj, ov, &o))
        return 0;
    int has = end ? o.fade_out : o.fade_in;
    if (!re_project_overlay_fade(A.proj, ov, end, has ? 0 : re_project_frames(A.proj, 1.0)))
        return 0;
    app_log("overlay %d: fade %s %s", ov, end ? "out" : "in", has ? "taken away" : "made");
    app_edited();
    return 1;
}

void ov_delete(int ov)
{
    re_overlay o;
    if (!A.proj || re_project_overlay(A.proj, ov, &o) || !re_project_overlay_remove(A.proj, ov))
        return;
    app_log("overlay %d deleted", ov);
    A.ov_sel = -1;
    app_edited();
}

enum { O_PLACE, O_SIZE, O_OPACITY, O_FADE_IN, O_FADE_OUT, O_SOUND, O_DELETE };
static const int SIZES[8] = {10, 15, 20, 25, 33, 50, 75, 100}, OPACITIES[4] = {100, 75, 50, 25};
static MENU(RE_PLACES) m_place = {"Place", 7, 2, 7, 0, 260, 44, 0,
                                  {{0, -1, IT("Full frame")}, {0, -1, IT("Centre")}, {0, -1, IT("Top left")},
                                   {0, -1, IT("Top right")}, {0, -1, IT("Bottom left")}, {0, -1, IT("Bottom right")},
                                   {0x80, -1, IT("Lower third")}}};
static MENU(8) m_size = {"Size", 7, 2, 7, 0, 160, 44, 0,
                         {{0, -1, IT("10%")}, {0, -1, IT("15%")}, {0, -1, IT("20%")}, {0, -1, IT("25%")},
                          {0, -1, IT("33%")}, {0, -1, IT("50%")}, {0, -1, IT("75%")}, {0x80, -1, IT("100%")}}};
static MENU(4) m_opacity = {"Opacity", 7, 2, 7, 0, 160, 44, 0,
                            {{0, -1, IT("100%")}, {0, -1, IT("75%")}, {0, -1, IT("50%")}, {0x80, -1, IT("25%")}}};
static MENU(7) m_overlay = {"Overlay", 7, 2, 7, 0, 240, 44, 0,
                            {{0, 0, IT("Place")}, {0, 0, IT("Size")}, {2, 0, IT("Opacity")}, {0, -1, IT("Fade in")},
                             {0, -1, IT("Fade out")}, {2, 0, IT("Sound")}, {0x80, -1, IT("Delete")}}};

void ov_menu_open(int x, int y, int ov)
{
    re_overlay o;
    _kernel_swi_regs r;
    if (!A.proj || re_project_overlay(A.proj, ov, &o))
        return;
    O.menu = 1;
    O.ov = ov;
    O.x = x;
    O.y = y;
    tl_select_overlay(ov);
    m_overlay.item[O_PLACE].sub = (intptr_t) &m_place;
    m_overlay.item[O_SIZE].sub = (intptr_t) &m_size;
    m_overlay.item[O_OPACITY].sub = (intptr_t) &m_opacity;
    for (int i = 0; i < RE_PLACES; i++)
        menu_tick(&m_place.item[i].flags, i == o.place);
    for (int i = 0; i < 8; i++)
        menu_tick(&m_size.item[i].flags, SIZES[i] == o.size);
    for (int i = 0; i < 4; i++)
        menu_tick(&m_opacity.item[i].flags, OPACITIES[i] == o.opacity);
    menu_shade(&m_overlay.item[O_SIZE].iflags, o.place == RE_PLACE_FULL);
    menu_tick(&m_overlay.item[O_FADE_IN].flags, o.fade_in > 0);
    menu_tick(&m_overlay.item[O_FADE_OUT].flags, o.fade_out > 0);
    m_overlay.item[O_SOUND].sub = fx_sound_menu(o.muted, o.sound_fade_in, o.sound_fade_out, 0, 0);
    menu_shade(&m_overlay.item[O_SOUND].iflags, !o.sound);
    r.r[1] = (intptr_t) &m_overlay;
    r.r[2] = x - 64;
    r.r[3] = y;
    _kernel_oserror *e = ro_swi(Wimp_CreateMenu, &r);
    app_log("overlay menu for %d at %d,%d%s%s", ov, x, y, e ? ": " : "", e ? e->errmess : "");
    app_overlays_menu_opened();
}

void ov_menu_select(int *b)
{
    if (O.menu == 2) {
        O.menu = 0;
        if (b[0] >= 0)
            use_sprite(O.path, b[0], O.frame, O.overlay);
        return;
    }
    re_overlay o;
    int i = O.ov, again = 0;
    if (!A.proj || i < 0 || re_project_overlay(A.proj, i, &o) || b[0] < 0)
        return;
    switch (b[0]) {
    case O_PLACE:
    case O_SIZE:
    case O_OPACITY: {
        int k = b[1];
        if (k < 0)
            break;
        int place = b[0] == O_PLACE && k < RE_PLACES ? k : -1;
        int size = b[0] == O_SIZE && k < 8 ? SIZES[k] : -1;
        int opacity = b[0] == O_OPACITY && k < 4 ? OPACITIES[k] : -1;
        /* (a box chosen for a full-frame picture: a size to go with it) */
        if (place > RE_PLACE_FULL && o.place == RE_PLACE_FULL && o.size >= 100)
            size = place == RE_PLACE_LOWER_THIRD ? 25 : 33;
        if (re_project_overlay_set(A.proj, i, place, size, opacity)) {
            app_log("overlay %d: place %d, size %d, opacity %d", i, place, size, opacity);
            app_edited();
            again = 1;
        }
        break;
    }
    case O_FADE_IN: again = ov_fade_toggle(i, 0); break;
    case O_FADE_OUT: again = ov_fade_toggle(i, 1); break;
    case O_SOUND: {
        int has = b[1] == S_FADE_IN ? o.sound_fade_in : o.sound_fade_out, len = re_project_frames(A.proj, 1.0);
        again = b[1] == S_MUTE ? re_project_overlay_mute(A.proj, i, !o.muted)
                : b[1] == S_FADE_IN || b[1] == S_FADE_OUT
                    ? re_project_overlay_sound_fade(A.proj, i, b[1] == S_FADE_OUT, has ? 0 : len)
                    : 0;
        if (again) {
            app_log("overlay %d: sound %s", i, b[1] == S_MUTE ? (o.muted ? "back" : "muted") : "fade toggled");
            app_edited();
        }
        break;
    }
    case O_DELETE: ov_delete(i); break;
    }
    /* Adjust: the menu stays open, for the same overlay */
    int p[5];
    _kernel_swi_regs r;
    r.r[1] = (intptr_t) p;
    if (again && !ro_swi(Wimp_GetPointerInfo, &r) && (p[2] & 1))
        ov_menu_open(O.x, O.y, i);
}
