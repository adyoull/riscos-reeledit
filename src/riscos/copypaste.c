/* copypaste.c - Cut, Copy and Paste (app.h): the selection (a clip on V1,
 * an overlay, a sound) copied with what's been done to it (project.h's
 * re_project_copy), and pasted at the playhead:
 *   a clip     inserted there (what follows moves on), as Final Cut's paste
 *   an overlay on V2 from there, where there's room
 *   a sound    on the audio tracks from there (stacked, as one dropped)
 * Pasted, it's selected and the playhead goes to its end, so pasting again
 * puts the next after it. Cut is Copy, then Delete (a clip's gap closed).
 * The copy is the editor's: closing it forgets the copy.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "app.h"

static re_copy *copied;

int cp_have(void)
{
    return copied != NULL;
}

int cp_copy(int cut)
{
    re_copy *c = NULL;
    if (!A.proj)
        return 0;
    if (A.au_sel >= 0)
        c = re_project_copy(A.proj, RE_COPY_SOUND, A.au_track, A.au_sel);
    else if (A.ov_sel >= 0)
        c = re_project_copy(A.proj, RE_COPY_OVERLAY, 0, A.ov_sel);
    else if (A.sel >= 0)
        c = re_project_copy(A.proj, RE_COPY_CLIP, 0, A.sel);
    if (!c) {
        app_log("copy: nothing selected to copy (a clip, an overlay or a sound)");
        return 0;
    }
    re_copy_free(copied);
    copied = c;
    app_log("%s: a %s, %d frames", cut ? "cut" : "copied",
            re_copy_kind(c) == RE_COPY_CLIP ? "clip" : re_copy_kind(c) == RE_COPY_OVERLAY ? "overlay" : "sound",
            re_copy_length(c));
    if (cut)
        app_delete_selection(1);
    return 1;
}

void cp_paste(void)
{
    if (!A.proj || !copied || app_source_mode())
        return;
    int at = app_position(), k = 0, kind = re_copy_kind(copied);
    int i = re_project_paste(A.proj, copied, at, &k);
    app_log("pasted at %d: %d (track %d)", at, i, k);
    if (i < 0) {
        app_report(kind == RE_COPY_CLIP ? "It can't be pasted into a dissolve: move the playhead off it."
                                        : "There's no room to paste it there.");
        return;
    }
    int end = at;
    if (kind == RE_COPY_CLIP) {
        re_item it;
        tl_select(i);
        if (!re_project_item(A.proj, i, &it))
            end = it.start + it.length;
    } else if (kind == RE_COPY_OVERLAY) {
        re_overlay o;
        tl_select_overlay(i);
        if (!re_project_overlay(A.proj, i, &o))
            end = o.start + o.length;
    } else {
        re_audio a;
        tl_select_audio(k, i);
        if (!re_project_audio(A.proj, k, i, &a))
            end = a.start + a.length;
    }
    app_edited();
    int len = re_project_length(A.proj);
    app_seek(end < len ? end : len - 1);
}

void cp_close(void)
{
    re_copy_free(copied);
    copied = NULL;
}
