/* effect.c - a clip's special effect (project.h): one of the "plus"
 * module's picture filters on its cut, as Shotcut puts them:
 *   Negative   invert (shotcut:filter "invert")
 *   Sketch     charcoal ("charcoal")
 *   Posterise  threshold: black or white, no greys ("threshold")
 * marked shotcut:reeledit_effect with which it is. One at a time; kept as
 * a clip's colour is (splits, speed, copy and paste), a dissolve's ends
 * given their clips' (rp_fit_effect).
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project_internal.h"

#include <string.h>

#define MARK "shotcut:reeledit_effect"

const char *const re_effects[RE_EFFECTS] = {"None", "Negative", "Sketch", "Posterise"};
static const char *const SERVICES[RE_EFFECTS] = {NULL, "invert", "charcoal", "threshold"};

static mlt_filter effect_filter(mlt_producer cut)
{
    for (int k = 1; cut && k < RE_EFFECTS; k++) {
        mlt_filter f = rp_cut_filter(cut, SERVICES[k]);
        if (f && mlt_properties_get(MLT_FILTER_PROPERTIES(f), MARK))
            return f;
    }
    return NULL;
}

int rp_cut_effect(mlt_producer cut)
{
    mlt_filter f = effect_filter(cut);
    int k = f ? mlt_properties_get_int(MLT_FILTER_PROPERTIES(f), MARK) : 0;
    return k > 0 && k < RE_EFFECTS ? k : 0;
}

void rp_cut_set_effect(re_project *p, mlt_producer cut, int kind)
{
    mlt_filter f = effect_filter(cut);
    if (rp_cut_effect(cut) == kind)
        return;
    if (f)
        mlt_service_detach(MLT_PRODUCER_SERVICE(cut), f);
    if (kind <= 0 || kind >= RE_EFFECTS)
        return;
    f = mlt_factory_filter(re_project_profile(p), SERVICES[kind], NULL);
    if (!f)
        return;
    mlt_properties fp = MLT_FILTER_PROPERTIES(f);
    mlt_properties_set(fp, "shotcut:filter", SERVICES[kind]);
    mlt_properties_set_int(fp, MARK, kind);
    mlt_service_attach(MLT_PRODUCER_SERVICE(cut), f);
    mlt_filter_close(f);
}

void rp_fit_effect(re_project *p)
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
            if (end && mlt_producer_is_cut(end))
                rp_cut_set_effect(p, end, rp_is_clip(p, clip) ? rp_cut_effect(mlt_playlist_get_clip(p->v1, clip)) : 0);
        }
    }
}

int re_project_effect(re_project *p, int item)
{
    return rp_is_clip(p, item) ? rp_cut_effect(mlt_playlist_get_clip(p->v1, item)) : -1;
}

int re_project_set_effect(re_project *p, int item, int kind)
{
    int now = re_project_effect(p, item);
    if (now < 0 || kind < 0 || kind >= RE_EFFECTS || now == kind)
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    rp_cut_set_effect(p, mlt_playlist_get_clip(p->v1, item), kind);
    rp_changed(p);
    return 1;
}
