/* clipsound.c - a clip's sound on its own (project.h): muted, or faded in or
 * out without its picture. For a picture in the picture whose sound isn't
 * wanted, or a clip's sound brought in under its picture.
 *
 * Its level: a volume filter in dB (Shotcut's Gain/Volume, "shotcut:filter"
 * audioGain, its level), 0 dB none.
 * Muted: a volume filter on the clip's cut, its gain 0, named
 * "shotcut:filter" reeleditMute (Shotcut has no mute of a clip's own: not
 * tried there). A sound fade alone is the volume filter of a clip's fade
 * (fade.c) without its brightness one.
 *
 * A dissolve's two clips' ends are cuts of their own (MLT's mix: a tractor
 * of them); rp_fit_sound mutes each (and sets its level) as its clip is, after every edit and
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
#define GAIN "audioGain" /* (Shotcut's Gain / Volume filter: volume, its level in dB) */

int rp_cut_has_sound(mlt_producer cut)
{
    if (!cut || re_still_is(cut))
        return 0;
    mlt_producer parent = mlt_producer_cut_parent(cut);
    const char *service = mlt_properties_get(MLT_PRODUCER_PROPERTIES(parent ? parent : cut), "mlt_service");
    if (service && (!strcmp(service, "color") || !strcmp(service, "colour"))) /* (a colour clip) */
        return 0;
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

int rp_cut_volume(mlt_producer cut)
{
    mlt_filter f = cut ? rp_cut_filter(cut, GAIN) : NULL;
    return f ? mlt_properties_get_int(MLT_FILTER_PROPERTIES(f), "level") : 0;
}

void rp_cut_set_volume(re_project *p, mlt_producer cut, int db)
{
    mlt_filter f = rp_cut_filter(cut, GAIN);
    if (!db) {
        if (f)
            mlt_service_detach(MLT_PRODUCER_SERVICE(cut), f);
        return;
    }
    if (!f) {
        f = mlt_factory_filter(re_project_profile(p), "volume", NULL);
        if (!f)
            return;
        mlt_properties_set(MLT_FILTER_PROPERTIES(f), "shotcut:filter", GAIN);
        mlt_service_attach(MLT_PRODUCER_SERVICE(cut), f);
        mlt_filter_close(f);
    }
    mlt_properties_set_int(MLT_FILTER_PROPERTIES(f), "level", db);
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
            if (end && mlt_producer_is_cut(end)) {
                mlt_producer c = rp_is_clip(p, clip) ? mlt_playlist_get_clip(p->v1, clip) : NULL;
                rp_cut_mute(p, end, c && rp_cut_muted(c));
                if (rp_cut_volume(end) != rp_cut_volume(c))
                    rp_cut_set_volume(p, end, rp_cut_volume(c));
            }
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

int re_project_volume(re_project *p, int item, int db)
{
    if (!rp_is_clip(p, item))
        return 0;
    mlt_producer cut = mlt_playlist_get_clip(p->v1, item);
    db = db < -60 ? -60 : db > 12 ? 12 : db;
    if (!rp_cut_has_sound(cut) || rp_cut_volume(cut) == db)
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    rp_cut_set_volume(p, cut, db);
    rp_changed(p);
    return 1;
}
