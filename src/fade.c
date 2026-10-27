/* fade.c - fades from and to black on V1's clips (project.h): a clip's
 * start fading in, its end fading out, the picture and the sound together.
 *
 * A fade is two MLT filters on the clip's cut (the playlist entry), as
 * Shotcut makes them, named the way Shotcut names its own
 * ("shotcut:filter"):
 *   - brightness, its level going 0 to 1 (in) or 1 to 0 (out);
 *   - volume, its level going -60 dB to 0 dB (in) or back (out).
 * A filter on a cut counts frames of the clip's file, so a fade in starts
 * at the clip's in and a fade out ends at its out; rp_fit_fades keeps them
 * there as the clip is trimmed (after every edit), and to the clip's length.
 * The fade's length is the brightness filter's.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project_internal.h"

#include <stdio.h>
#include <string.h>

static const char *const NAMES[2][2] = {{"fadeInBrightness", "fadeInVolume"},
                                         {"fadeOutBrightness", "fadeOutVolume"}};

/* The clip's filter named so, or NULL */
static mlt_filter find(mlt_producer cut, const char *name)
{
    mlt_service s = MLT_PRODUCER_SERVICE(cut);
    for (int i = 0; i < mlt_service_filter_count(s); i++) {
        mlt_filter f = mlt_service_filter(s, i);
        const char *n = mlt_properties_get(MLT_FILTER_PROPERTIES(f), "shotcut:filter");
        if (n && !strcmp(n, name))
            return f;
    }
    return NULL;
}

static int length_of(mlt_producer cut, int end)
{
    mlt_filter f = find(cut, NAMES[end][0]);
    return f ? mlt_filter_get_out(f) - mlt_filter_get_in(f) + 1 : 0;
}

/* The fade at one end of a clip made again (0: none), over frames in..out
   of its file */
static void set(re_project *p, mlt_producer cut, int end, int length, int in, int out)
{
    for (int k = 0; k < 2; k++) {
        mlt_filter old = find(cut, NAMES[end][k]);
        if (old)
            mlt_service_detach(MLT_PRODUCER_SERVICE(cut), old);
    }
    if (length > out - in + 1)
        length = out - in + 1;
    if (length < 1)
        return;
    int f0 = end ? out - length + 1 : in, f1 = f0 + length - 1;
    char level[64];
    for (int k = 0; k < 2; k++) {
        mlt_filter f = mlt_factory_filter(re_project_profile(p), k ? "volume" : "brightness", NULL);
        if (!f)
            continue;
        mlt_properties fp = MLT_FILTER_PROPERTIES(f);
        mlt_properties_set(fp, "shotcut:filter", NAMES[end][k]);
        double lo = k ? -60 : 0, hi = k ? 0 : 1; /* (dB; brightness) */
        snprintf(level, sizeof(level), "0=%g;%d=%g", end ? hi : lo, length - 1, end ? lo : hi);
        mlt_properties_set(fp, "level", level);
        if (!k)
            mlt_properties_set_double(fp, "alpha", 1);
        mlt_filter_set_in_and_out(f, f0, f1);
        mlt_service_attach(MLT_PRODUCER_SERVICE(cut), f);
        mlt_filter_close(f); /* (the clip keeps its reference) */
    }
}

static mlt_producer cut_of(re_project *p, int item, re_item *it)
{
    if (!rp_is_clip(p, item) || re_project_item(p, item, it))
        return NULL;
    return mlt_playlist_get_clip(p->v1, item);
}

int rp_fade_of(re_project *p, int item, int end)
{
    /* (not cut_of: re_project_item asks this) */
    return rp_is_clip(p, item) ? length_of(mlt_playlist_get_clip(p->v1, item), end) : 0;
}

void rp_set_fade(re_project *p, int item, int end, int length)
{
    re_item it;
    mlt_producer cut = cut_of(p, item, &it);
    if (cut)
        set(p, cut, end, length, it.in, it.out);
}

void rp_fit_fades(re_project *p)
{
    int n = p->v1 ? mlt_playlist_count(p->v1) : 0;
    for (int i = 0; i < n; i++) {
        re_item it;
        mlt_producer cut = cut_of(p, i, &it);
        for (int end = 0; cut && end < 2; end++) {
            mlt_filter f = find(cut, NAMES[end][0]);
            if (!f)
                continue;
            int length = length_of(cut, end), want = length < it.length ? length : it.length;
            int f0 = end ? it.out - want + 1 : it.in;
            if (want != length || mlt_filter_get_in(f) != f0)
                set(p, cut, end, want, it.in, it.out);
        }
    }
}

int re_project_fade(re_project *p, int item, int end, int length)
{
    re_item it;
    if (!cut_of(p, item, &it) || rp_mix_of(p, end ? item + 1 : item - 1))
        return 0; /* (that end is in a dissolve) */
    if (length < 0)
        length = 0;
    if (length > it.length)
        length = it.length;
    if (length == rp_fade_of(p, item, end))
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    rp_set_fade(p, item, end, length);
    rp_changed(p);
    return 1;
}
