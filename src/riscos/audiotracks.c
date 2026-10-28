/* audiotracks.c - the audio tracks' sounds (app.h): dropped on the rows
 * below V1, and their menu. What they do to the project is project.h's
 * (audio.c); this is the desktop's side.
 *
 * A file dropped on an audio row (an audio file: WAV, MP3, AAC, Ogg,
 * FLAC; or a video, its sound alone) goes on the first track with room
 * for all of it at the frame it was dropped at: where another sound
 * plays, on the next track, a new one if need be, so sound effects
 * stack.
 *
 * The menu, over a sound ("Sound"):
 *   Mute; Fade in, Fade out (a second; ticked when it has one, chosen
 *   again it goes); Volume > +12 dB to -36 dB; Delete
 * Adjust keeps the menu open.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "app.h"
#include "menu.h"

#include <stdio.h>
#include <string.h>

#define Wimp_CreateMenu     0x400D4
#define Wimp_GetPointerInfo 0x400CF

static struct
{
    int track, i, x, y;
} U = {-1, -1, 0, 0};

static const char *leaf(const char *path)
{
    const char *l = strrchr(path, '.');
    return l ? l + 1 : path;
}

void au_load(const char *path, int frame)
{
    char unix_name[512], t[400];
    if (!A.proj || re_project_length(A.proj) <= 0) {
        app_report("Put some video on V1 first: sounds go under it.");
        return;
    }
    int k = -1, i = -1;
    int r = re_project_audio_add(A.proj, app_mlt_name(path, unix_name, sizeof(unix_name)), frame < 0 ? 0 : frame, 0,
                                 &k, &i);
    app_log("sound %s at %d: %d (A%d, %d)", path, frame, r, k + 1, i);
    if (r == RE_SOUND_CANT_OPEN || r == RE_SOUND_NO_SOUND) {
        snprintf(t, sizeof(t), "%.200s has no sound ReelEdit can play.", leaf(path));
        app_report(t);
        return;
    }
    if (r < 0) {
        app_report("There's no room for a sound there: drop it before the project's end.");
        return;
    }
    tl_select_audio(k, i);
    app_edited();
}

void au_delete(int track, int i)
{
    if (!A.proj || !re_project_audio_remove(A.proj, track, i))
        return;
    app_log("sound %d on A%d deleted", i, track + 1);
    A.au_sel = -1;
    app_edited();
}

enum { U_MUTE, U_FADE_IN, U_FADE_OUT, U_VOLUME, U_DELETE };
static MENU(5) m_audio = {"Sound", 7, 2, 7, 0, 200, 44, 0,
                          {{2, -1, IT("Mute")}, {0, -1, IT("Fade in")}, {2, -1, IT("Fade out")},
                           {2, 0, IT("Volume")}, {0x80, -1, IT("Delete")}}};

void au_menu_open(int x, int y, int track, int i)
{
    re_audio a;
    _kernel_swi_regs r;
    if (!A.proj || re_project_audio(A.proj, track, i, &a))
        return;
    U.track = track;
    U.i = i;
    U.x = x;
    U.y = y;
    tl_select_audio(track, i);
    menu_tick(&m_audio.item[U_MUTE].flags, a.muted);
    menu_tick(&m_audio.item[U_FADE_IN].flags, a.fade_in > 0);
    menu_tick(&m_audio.item[U_FADE_OUT].flags, a.fade_out > 0);
    m_audio.item[U_VOLUME].sub = fx_volume_menu(a.volume);
    r.r[1] = (intptr_t) &m_audio;
    r.r[2] = x - 64;
    r.r[3] = y;
    _kernel_oserror *e = ro_swi(Wimp_CreateMenu, &r);
    app_log("sound menu for %d on A%d at %d,%d%s%s", i, track + 1, x, y, e ? ": " : "", e ? e->errmess : "");
    app_audio_menu_opened();
}

void au_menu_select(int *b)
{
    re_audio a;
    int k = U.track, i = U.i, again = 0;
    if (!A.proj || b[0] < 0 || re_project_audio(A.proj, k, i, &a))
        return;
    int second = re_project_frames(A.proj, 1.0);
    switch (b[0]) {
    case U_MUTE: again = re_project_audio_mute(A.proj, k, i, !a.muted); break;
    case U_FADE_IN: again = re_project_audio_fade(A.proj, k, i, 0, a.fade_in ? 0 : second); break;
    case U_FADE_OUT: again = re_project_audio_fade(A.proj, k, i, 1, a.fade_out ? 0 : second); break;
    case U_VOLUME:
        if (b[1] >= 0 && b[1] < FX_N_LEVELS)
            again = re_project_audio_volume(A.proj, k, i, FX_LEVELS[b[1]]);
        break;
    case U_DELETE: au_delete(k, i); return;
    }
    if (again) {
        app_log("sound %d on A%d: menu item %d,%d", i, k + 1, b[0], b[1]);
        app_edited();
    }
    int p[5];
    _kernel_swi_regs r;
    r.r[1] = (intptr_t) p;
    if (again && !ro_swi(Wimp_GetPointerInfo, &r) && (p[2] & 1))
        au_menu_open(U.x, U.y, k, i);
}

#ifdef RE_TEST
int re_test_audio_selected(int *track)
{
    *track = A.au_track;
    return A.au_sel;
}
#endif
