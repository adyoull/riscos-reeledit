/* wipe.c - wipes (project.h): a dissolve's picture changing from one clip
 * to the next by a shape (a bar across, a circle opening...) rather than
 * all over at once.
 *
 * A dissolve's luma transition (dissolve.c) given a luma map, as Shotcut
 * gives it: "resource" one of MLT's lumas by name ("%luma01.pgm": MLT
 * makes the map from the name when there's no such file, as here), its
 * "softness" (0..1: how blurred the edge is) and "invert" (the other way
 * round). No resource: a plain dissolve.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

const char *const re_wipes[RE_WIPES] = {"Dissolve", "Wipe from the left", "Wipe from the top", "Barn doors",
                                        "Diagonal", "Circle", "Box", "Clock"};
static const char *const LUMAS[RE_WIPES] = {NULL, "luma01", "luma02", "luma03", "luma07", "luma16", "luma18", "luma22"};

void re_wipe_none(re_wipe *w)
{
    w->kind = 0;
    w->softness = 0;
    w->reverse = 0;
}

/* A dissolve's luma transition, or NULL */
static mlt_transition luma_of(mlt_tractor t)
{
    mlt_service s = t ? mlt_service_producer(MLT_TRACTOR_SERVICE(t)) : NULL;
    while (s && mlt_service_identify(s) == mlt_service_transition_type) {
        const char *n = mlt_properties_get(MLT_SERVICE_PROPERTIES(s), "mlt_service");
        if (n && !strcmp(n, "luma"))
            return (mlt_transition) s;
        s = mlt_service_producer(s);
    }
    return NULL;
}

int rp_mix_wipe(mlt_tractor t, re_wipe *w)
{
    re_wipe_none(w);
    mlt_transition l = luma_of(t);
    if (!l)
        return -1;
    mlt_properties lp = MLT_TRANSITION_PROPERTIES(l);
    const char *r = mlt_properties_get(lp, "resource");
    if (r && *r) {
        w->kind = -1; /* (another map: kept as it is) */
        for (int i = 1; i < RE_WIPES; i++)
            if (strstr(r, LUMAS[i]))
                w->kind = i;
    }
    w->softness = (int) lround(mlt_properties_get_double(lp, "softness") * 100);
    w->reverse = mlt_properties_get_int(lp, "invert") != 0;
    return 0;
}

int re_project_wipe(re_project *p, int mix, re_wipe *w)
{
    return rp_mix_wipe(rp_mix_of(p, mix), w);
}

int re_project_set_wipe(re_project *p, int mix, const re_wipe *want)
{
    re_wipe w = *want, now;
    mlt_transition l = luma_of(rp_mix_of(p, mix));
    if (!l || w.kind < 0 || w.kind >= RE_WIPES || re_project_wipe(p, mix, &now))
        return 0;
    w.softness = w.softness < 0 ? 0 : w.softness > 100 ? 100 : w.softness;
    w.reverse = !!w.reverse;
    if (!memcmp(&w, &now, sizeof(w)))
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    mlt_properties lp = MLT_TRANSITION_PROPERTIES(l);
    char res[24];
    snprintf(res, sizeof(res), "%%%s.pgm", w.kind ? LUMAS[w.kind] : "");
    mlt_properties_set(lp, "resource", w.kind ? res : "");
    mlt_properties_set_double(lp, "softness", w.softness / 100.0);
    mlt_properties_set_int(lp, "invert", w.reverse);
    rp_changed(p);
    return 1;
}
