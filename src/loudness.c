/* loudness.c - the whole project's sound evened out (project.h): the
 * "plus" module's dynamic_loudness filter on the project (its tractor),
 * as it plays and as it's exported. It measures the loudness as it goes
 * (EBU R128, over the last few seconds) and turns the sound up or down
 * towards a target, by no more than 3 dB a second and 15 dB in all, so
 * quiet and loud clips come out alike without pumping.
 *
 * Marked shotcut:filter "dynamicLoudness" (as Shotcut's Loudness:
 * Dynamic). Saved with the project: a filter on the tractor.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project_internal.h"

#include <math.h>
#include <string.h>

#define TAG "dynamicLoudness"

static mlt_filter loudness_filter(re_project *p)
{
    if (!p->tractor)
        return NULL;
    mlt_service s = MLT_TRACTOR_SERVICE(p->tractor);
    for (int i = 0; mlt_service_filter(s, i); i++) {
        mlt_filter f = mlt_service_filter(s, i);
        const char *t = mlt_properties_get(MLT_FILTER_PROPERTIES(f), "shotcut:filter");
        if (t && !strcmp(t, TAG))
            return f;
    }
    return NULL;
}

int re_project_loudness(re_project *p)
{
    mlt_filter f = loudness_filter(p);
    return f ? (int) lround(mlt_properties_get_double(MLT_FILTER_PROPERTIES(f), "target_loudness")) : 0;
}

int re_project_set_loudness(re_project *p, int lufs)
{
    if (!p->tractor || (lufs != 0 && (lufs > -5 || lufs < -40)))
        return 0;
    if (re_project_loudness(p) == lufs)
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    mlt_filter f = loudness_filter(p);
    if (!lufs) {
        mlt_service_detach(MLT_TRACTOR_SERVICE(p->tractor), f);
    } else {
        if (!f) {
            f = mlt_factory_filter(p->profile, "dynamic_loudness", NULL);
            if (!f) {
                if (!p->in_edit)
                    rp_no_edit(p);
                return 0;
            }
            mlt_properties_set(MLT_FILTER_PROPERTIES(f), "shotcut:filter", TAG);
            mlt_properties_set_int(MLT_FILTER_PROPERTIES(f), "discontinuity_reset", 1);
            mlt_service_attach(MLT_TRACTOR_SERVICE(p->tractor), f);
            mlt_filter_close(f);
        }
        mlt_properties_set_double(MLT_FILTER_PROPERTIES(f), "target_loudness", lufs);
    }
    rp_changed(p);
    return 1;
}
