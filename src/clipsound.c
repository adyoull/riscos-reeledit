/* clipsound.c - a clip's sound on its own (project.h): muted, or faded in or
 * out without its picture. For a picture in the picture whose sound isn't
 * wanted, or a clip's sound brought in under its picture.
 *
 * Muted: a volume filter on the clip's cut, its gain 0, named
 * "shotcut:filter" reeleditMute (Shotcut has no mute of a clip's own: not
 * tried there). A sound fade alone is the volume filter of a clip's fade
 * (fade.c) without its brightness one.
 *
 * A dissolve's two clips' ends are cuts of their own (MLT's mix: a tractor
 * of them); rp_fit_sound mutes each as its clip is, after every edit and
 * when a project is read, so a muted clip stays silent through its
 * dissolves. V2's videos' (overlay.c) are kept on their cuts too.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project_internal.h"
#include "still.h"

#include <stdlib.h>
#include <string.h>

#define MUTE "reeleditMute"

int rp_cut_has_sound(mlt_producer cut)
{
    if (!cut || re_still_is(cut))
        return 0;
    mlt_producer parent = mlt_producer_cut_parent(cut);
    const char *a = mlt_properties_get(MLT_PRODUCER_PROPERTIES(parent ? parent : cut), "audio_index");
    return !a || atoi(a) >= 0; /* (avformat's -1: none; not avformat: assumed) */
}

int rp_cut_muted(mlt_producer cut)
{
    return cut && rp_cut_filter(cut, MUTE) != NULL;
}

void rp_cut_mute(re_project *p, mlt_producer cut, int on)
{
    mlt_filter f = rp_cut_filter(cut, MUTE);
    if (!on && f)
        mlt_service_detach(MLT_PRODUCER_SERVICE(cut), f);
    if (!on || f)
        return;
    f = mlt_factory_filter(re_project_profile(p), "volume", NULL);
    if (!f)
        return;
    mlt_properties_set(MLT_FILTER_PROPERTIES(f), "shotcut:filter", MUTE);
    mlt_properties_set(MLT_FILTER_PROPERTIES(f), "gain", "0");
    mlt_service_attach(MLT_PRODUCER_SERVICE(cut), f);
    mlt_filter_close(f);
}

void rp_fit_sound(re_project *p)
{
    int n = p->v1 ? mlt_playlist_count(p->v1) : 0;
    for (int i = 0; i < n; i++) {
        mlt_tractor t = rp_mix_of(p, i);
        if (!t)
            continue;
        mlt_multitrack mt = mlt_tractor_multitrack(t);
        for (int k = 0; k < 2 && k < mlt_multitrack_count(mt); k++) {
            int clip = k ? i + 1 : i - 1;
            mlt_producer end = mlt_multitrack_track(mt, k);
            /* (only a cut of its own: never the file's producer, which every
               clip of that file shares) */
            if (end && mlt_producer_is_cut(end))
                rp_cut_mute(p, end, rp_is_clip(p, clip) && rp_cut_muted(mlt_playlist_get_clip(p->v1, clip)));
        }
    }
}

/* ---- edits ---------------------------------------------------------------------------------- */

int re_project_mute(re_project *p, int item, int on)
{
    if (!rp_is_clip(p, item))
        return 0;
    mlt_producer cut = mlt_playlist_get_clip(p->v1, item);
    if (!rp_cut_has_sound(cut) || rp_cut_muted(cut) == !!on)
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    rp_cut_mute(p, cut, on);
    rp_changed(p);
    return 1;
}

int re_project_sound_fade(re_project *p, int item, int end, int length)
{
    if (!rp_is_clip(p, item) || !rp_cut_has_sound(mlt_playlist_get_clip(p->v1, item)))
        return 0;
    return rp_fade(p, item, RP_SOUND, end, length);
}
