/* fade.c - fades on clips (project.h): a clip's start fading in, its end
 * fading out; the picture (from and to black) and the sound (from and to
 * silence) together, or the sound on its own (clipsound.c).
 *
 * A fade is MLT filters on the clip's cut (the playlist entry), as Shotcut
 * makes them, named the way Shotcut names its own ("shotcut:filter"):
 *   - the picture's: brightness, its level going 0 to 1 (in) or 1 to 0 (out);
 *   - the sound's: volume, its level going -60 dB to 0 dB (in) or back (out).
 * A clip's "Fade in" is both, the same length; the sound's can also be had
 * (or taken away, or be another length) on its own. A clip's fade length
 * (re_item's fade_in, fade_out) is its picture's; sound_fade_in and _out
 * are its sound's.
 * A filter on a cut counts frames of the clip's file, so a fade in starts
 * at the clip's in and a fade out ends at its out; rp_fit_fades keeps them
 * there as the clip is trimmed (after every edit), and to the clip's
 * length, each kind on its own. V2's videos have sound fades the same way
 * (clipsound.c), kept by rp_fit_fades too.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project_internal.h"

#include <stdio.h>
#include <string.h>

static const char *const NAMES[2][2] = {{"fadeInBrightness", "fadeInVolume"},
                                         {"fadeOutBrightness", "fadeOutVolume"}};

mlt_filter rp_cut_filter(mlt_producer cut, const char *name)
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

int rp_cut_fade(mlt_producer cut, int kind, int end)
{
    mlt_filter f = cut ? rp_cut_filter(cut, NAMES[end][kind == RP_SOUND]) : NULL;
    return f ? mlt_filter_get_out(f) - mlt_filter_get_in(f) + 1 : 0;
}

void rp_cut_set_fade(re_project *p, mlt_producer cut, int kinds, int end, int length, int in, int out)
{
    for (int k = 0; k < 2; k++) {
        if (!(kinds & (k ? RP_SOUND : RP_PICTURE)))
            continue;
        mlt_filter old = rp_cut_filter(cut, NAMES[end][k]);
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
        if (!(kinds & (k ? RP_SOUND : RP_PICTURE)))
            continue;
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

void rp_cut_fit_fades(re_project *p, mlt_producer cut, int in, int out)
{
    int clip_length = out - in + 1;
    for (int end = 0; end < 2; end++)
        for (int k = 0; k < 2; k++) {
            mlt_filter f = rp_cut_filter(cut, NAMES[end][k]);
            if (!f)
                continue;
            int kind = k ? RP_SOUND : RP_PICTURE;
            int length = rp_cut_fade(cut, kind, end), want = length < clip_length ? length : clip_length;
            int f0 = end ? out - want + 1 : in;
            if (want != length || mlt_filter_get_in(f) != f0)
                rp_cut_set_fade(p, cut, kind, end, want, in, out);
        }
}

static mlt_producer cut_of(re_project *p, int item, re_item *it)
{
    if (!rp_is_clip(p, item) || re_project_item(p, item, it))
        return NULL;
    return mlt_playlist_get_clip(p->v1, item);
}

int rp_fade_of(re_project *p, int item, int kind, int end)
{
    /* (not cut_of: re_project_item asks this) */
    return rp_is_clip(p, item) ? rp_cut_fade(mlt_playlist_get_clip(p->v1, item), kind, end) : 0;
}

void rp_set_fade(re_project *p, int item, int kinds, int end, int length)
{
    re_item it;
    mlt_producer cut = cut_of(p, item, &it);
    if (cut)
        rp_cut_set_fade(p, cut, kinds, end, length, it.in, it.out);
}

void rp_fit_fades(re_project *p)
{
    int n = p->v1 ? mlt_playlist_count(p->v1) : 0;
    for (int i = 0; i < n; i++) {
        re_item it;
        mlt_producer cut = cut_of(p, i, &it);
        if (cut)
            rp_cut_fit_fades(p, cut, it.in, it.out);
    }
    mlt_playlist over[2] = {p->v2, p->v3}; /* (V2 and the titles' track) */
    for (int k = 0; k < 2; k++) {
        n = over[k] ? mlt_playlist_count(over[k]) : 0;
        for (int i = 0; i < n; i++) {
            mlt_playlist_clip_info info;
            if (!mlt_playlist_is_blank(over[k], i) && !mlt_playlist_get_clip_info(over[k], &info, i))
                rp_cut_fit_fades(p, info.cut, info.frame_in, info.frame_out);
        }
    }
}

int rp_fade(re_project *p, int item, int kinds, int end, int length)
{
    re_item it;
    if (!cut_of(p, item, &it) || rp_mix_of(p, end ? item + 1 : item - 1))
        return 0; /* (that end is in a dissolve) */
    if (length < 0)
        length = 0;
    if (length > it.length)
        length = it.length;
    if ((!(kinds & RP_PICTURE) || length == rp_fade_of(p, item, RP_PICTURE, end))
        && (!(kinds & RP_SOUND) || length == rp_fade_of(p, item, RP_SOUND, end)))
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    rp_set_fade(p, item, kinds, end, length);
    rp_changed(p);
    return 1;
}

int re_project_fade(re_project *p, int item, int end, int length)
{
    return rp_fade(p, item, RP_PICTURE | RP_SOUND, end, length);
}
