/* effects.c - giving clips effects (app.h): fades and dissolves, from the
 * Timeline's context menu (Menu over a clip or a dissolve), the Edit menu
 * and D. What they do to the project is project.h's (fade.c, dissolve.c);
 * this is the desktop's side: which clip, how long, the menu.
 *
 * The context menu, over a clip ("Clip"):
 *   Fade in, Fade out        ticked when it has one; chosen again, it goes
 *   Dissolve from the clip before, Dissolve into the clip after
 *                            a second long, from the clips' frames beyond
 *                            the cut where they have them; shaded with no
 *                            clip there, or a dissolve there already
 *   Delete, Video information...
 * over a dissolve ("Dissolve"): Delete (the clips as they were).
 * Each effect is drawn on the Timeline as a box over the frames it covers;
 * dragging the box's inner edge makes it longer or shorter (timeline.c).
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "app.h"
#include "menu.h"

#include <stdio.h>
#include <string.h>

#define Wimp_CreateMenu     0x400D4
#define Wimp_GetPointerInfo 0x400CF

/* ---- the effects ---------------------------------------------------------------------------- */

int fx_fade_toggle(int item, int end)
{
    re_item it;
    if (!A.proj || item < 0 || re_project_item(A.proj, item, &it) || it.gap || it.mix)
        return 0;
    int has = end ? it.fade_out : it.fade_in;
    if (!re_project_fade(A.proj, item, end, has ? 0 : re_project_frames(A.proj, 1.0)))
        return 0;
    app_log("fade %s on item %d: %s", end ? "out" : "in", item, has ? "taken away" : "made");
    app_edited();
    return 1;
}

int fx_dissolve(int before)
{
    int m = A.proj && before >= 0 ? re_project_dissolve(A.proj, before, re_project_frames(A.proj, 1.0), 0) : -1;
    re_item it;
    if (m < 0 || re_project_item(A.proj, m, &it))
        return -1;
    app_log("dissolve: %d frames from %d", it.length, it.start);
    A.sel = m;
    app_edited();
    app_seek(it.start);
    return m;
}

int fx_dissolve_at_cut(void)
{
    return A.proj ? fx_dissolve(re_project_dissolve_cut(A.proj, app_position())) : -1;
}

/* ---- the context menu ------------------------------------------------------------------------ */

enum { C_FADE_IN, C_FADE_OUT, C_FROM, C_INTO, C_DELETE, C_INFO };
static MENU(6) m_clip = {"Clip", 7, 2, 7, 0, 440, 44, 0,
                         {{0, -1, IT("Fade in")}, {2, -1, IT("Fade out")},
                          {0, -1, IT("Dissolve from the clip before")}, {2, -1, IT("Dissolve into the clip after")},
                          {0, -1, IT("Delete")}, {0x80, -1, IT("Video information...")}}};
static MENU(1) m_mix = {"Dissolve", 7, 2, 7, 0, 200, 44, 0, {{0x80, -1, IT("Delete")}}};

static struct
{
    int item, x, y;
} C = {-1, 0, 0};

/* A dissolve can be made at the cut between items a and a + 1 */
static int can_dissolve(int a)
{
    re_item x, y;
    return a >= 0 && !re_project_item(A.proj, a, &x) && !re_project_item(A.proj, a + 1, &y) && !x.gap && !x.mix
           && !y.gap && !y.mix;
}

void fx_menu_open(int x, int y, int item)
{
    re_item it;
    _kernel_swi_regs r;
    if (!A.proj || re_project_item(A.proj, item, &it) || it.gap)
        return;
    C.item = item;
    C.x = x;
    C.y = y;
    tl_select(item);
    if (it.mix) {
        r.r[1] = (intptr_t) &m_mix;
    } else {
        menu_tick(&m_clip.item[C_FADE_IN].flags, it.fade_in > 0);
        menu_tick(&m_clip.item[C_FADE_OUT].flags, it.fade_out > 0);
        /* (an end in a dissolve has no fade) */
        re_item n;
        menu_shade(&m_clip.item[C_FADE_IN].iflags, item > 0 && !re_project_item(A.proj, item - 1, &n) && n.mix);
        menu_shade(&m_clip.item[C_FADE_OUT].iflags, !re_project_item(A.proj, item + 1, &n) && n.mix);
        menu_shade(&m_clip.item[C_FROM].iflags, !can_dissolve(item - 1));
        menu_shade(&m_clip.item[C_INTO].iflags, !can_dissolve(item));
        r.r[1] = (intptr_t) &m_clip;
    }
    r.r[2] = x - 64;
    r.r[3] = y;
    _kernel_oserror *e = ro_swi(Wimp_CreateMenu, &r);
    app_log("context menu for item %d at %d,%d%s%s", item, x, y, e ? ": " : "", e ? e->errmess : "");
}

void fx_menu_select(int *b)
{
    re_item it;
    int i = C.item, again = 0;
    if (!A.proj || i < 0 || re_project_item(A.proj, i, &it) || b[0] < 0)
        return;
    if (it.mix) {
        app_delete(i, 1);
        return;
    }
    switch (b[0]) {
    case C_FADE_IN: again = fx_fade_toggle(i, 0); break;
    case C_FADE_OUT: again = fx_fade_toggle(i, 1); break;
    case C_FROM: fx_dissolve(i - 1); break;
    case C_INTO: fx_dissolve(i); break;
    case C_DELETE: app_delete(i, 1); break;
    case C_INFO: info_open(); break;
    }
    /* Adjust: the menu stays open, for the same clip (a fade toggled) */
    int p[5];
    _kernel_swi_regs r;
    r.r[1] = (intptr_t) p;
    if (again && !ro_swi(Wimp_GetPointerInfo, &r) && (p[2] & 1))
        fx_menu_open(C.x, C.y, i);
}
