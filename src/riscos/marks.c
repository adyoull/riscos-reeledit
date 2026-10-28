/* marks.c - in and out marks on the Timeline (app.h), as every editor has
 * them: a stretch of the project to play, or to take out.
 *
 *   I, O                 the in or out mark at the playhead (the out mark's
 *                        frame is the last one in the stretch)
 *   Shift-I, Shift-O     the playhead to the mark
 *   Return               play from in to out, stopping on the out mark
 *   ' (Extract)          the stretch taken out, what follows moving up
 *   ; (Lift)             the stretch taken out, a gap left
 *   Play > Clear the marks
 * Also in the Play and Edit menus. The marks aren't part of the project
 * (not saved, not undone); after an edit they're kept within it.
 * The Timeline draws the stretch on its ruler (timeline.c). With a source
 * clip in the Viewer, I and O mark the source instead (viewer.c).
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "app.h"

static void changed(void)
{
    tl_edited(); /* (the ruler drawn again) */
}

void mk_set(int out)
{
    if (app_source_mode()) { /* (a source in the Viewer: its marks) */
        app_source_mark(out);
        return;
    }
    if (!A.proj || re_project_length(A.proj) <= 0)
        return;
    int f = app_position();
    if (out) {
        A.mark_out = f;
        if (A.mark_in > f)
            A.mark_in = -1; /* (an in mark after the out: gone) */
    } else {
        A.mark_in = f;
        if (A.mark_out >= 0 && A.mark_out < f)
            A.mark_out = -1;
    }
    app_log("mark %s at %d", out ? "out" : "in", f);
    changed();
}

void mk_go(int out)
{
    int f = out ? A.mark_out : A.mark_in;
    if (A.proj && f >= 0)
        app_seek(f);
}

/* The stretch marked: from the in mark (or the start) to the out mark (or
   the end). 0 if there are no marks. */
static int stretch(int *in, int *out)
{
    if (!A.proj || (A.mark_in < 0 && A.mark_out < 0))
        return 0;
    *in = A.mark_in >= 0 ? A.mark_in : 0;
    *out = A.mark_out >= 0 ? A.mark_out : re_project_length(A.proj) - 1;
    return *out >= *in;
}

void mk_play(void)
{
    int in, out;
    if (!A.e || !stretch(&in, &out))
        return;
    app_seek(in);
    app_set_speed(1);
    re_engine_play_to(A.e, out);
    app_log("playing the marks, %d to %d", in, out);
}

void mk_clear(void)
{
    A.mark_in = A.mark_out = -1;
    changed();
}

void mk_take(int ripple)
{
    int in, out;
    if (!stretch(&in, &out))
        return;
    if (!re_project_remove_range(A.proj, in, out, ripple)) {
        app_report("The marked part can't be taken out: it goes through a dissolve. Delete the dissolve first.");
        return;
    }
    app_log("%s %d to %d", ripple ? "extract" : "lift", in, out);
    A.mark_in = A.mark_out = -1;
    A.sel = -1;
    app_edited();
    app_seek(in);
}

void mk_edited(void)
{
    int n = A.proj ? re_project_length(A.proj) : 0;
    if (A.mark_in >= n)
        A.mark_in = -1;
    if (A.mark_out >= n)
        A.mark_out = n > 0 ? n - 1 : -1;
}
