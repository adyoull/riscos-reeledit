/* key.c - green screen (project.h): an overlay's backdrop of one colour
 * made see-through, so what's under it shows (V1, or V2 under a title).
 *
 * The "plus" module's chroma filter on the overlay's cut (shotcut:filter
 * "chroma", as Shotcut's Chroma Key: Simple): "key" the colour, "variance"
 * how near to it a colour has to be (0..1). It works on the picture's
 * colour (Cb and Cr), not its brightness, so shadows on the screen are
 * keyed as well. The overlay's composite then uses the alpha it makes.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define KEY "chroma"

const char *const re_keys[RE_KEYS] = {"Off", "Green", "Blue"};
static const char *const COLOURS[RE_KEYS] = {NULL, "#00ff00", "#0000ff"};

void re_key_none(re_key *k)
{
    k->kind = 0;
    k->tolerance = RE_KEY_TOLERANCE;
}

void rp_cut_key(mlt_producer cut, re_key *k)
{
    re_key_none(k);
    mlt_filter f = cut ? rp_cut_filter(cut, KEY) : NULL;
    if (!f)
        return;
    mlt_properties fp = MLT_FILTER_PROPERTIES(f);
    mlt_color c = mlt_properties_get_color(fp, "key");
    k->kind = c.g > c.b ? 1 : 2;
    k->tolerance = (int) lround(mlt_properties_get_double(fp, "variance") * 100);
}

void rp_cut_set_key(re_project *p, mlt_producer cut, const re_key *k)
{
    mlt_filter f = rp_cut_filter(cut, KEY);
    if (k->kind <= 0) {
        if (f)
            mlt_service_detach(MLT_PRODUCER_SERVICE(cut), f);
        return;
    }
    if (!f) {
        f = mlt_factory_filter(re_project_profile(p), "chroma", NULL);
        if (!f)
            return;
        mlt_properties_set(MLT_FILTER_PROPERTIES(f), "shotcut:filter", KEY);
        mlt_service_attach(MLT_PRODUCER_SERVICE(cut), f);
        mlt_filter_close(f);
    }
    mlt_properties fp = MLT_FILTER_PROPERTIES(f);
    mlt_properties_set(fp, "key", COLOURS[k->kind]);
    mlt_properties_set_double(fp, "variance", k->tolerance / 100.0);
}

static mlt_producer cut_of(re_project *p, int i)
{
    mlt_playlist pl;
    int e = rp_overlay_entry(p, i, &pl);
    return e >= 0 ? mlt_playlist_get_clip(pl, e) : NULL;
}

int re_project_overlay_key(re_project *p, int i, re_key *k)
{
    mlt_producer cut = cut_of(p, i);
    rp_cut_key(cut, k);
    return cut ? 0 : -1;
}

int re_project_overlay_set_key(re_project *p, int i, const re_key *want)
{
    re_key k = *want, now;
    k.kind = k.kind < 0 || k.kind >= RE_KEYS ? 0 : k.kind;
    k.tolerance = k.tolerance < 1 ? 1 : k.tolerance > 60 ? 60 : k.tolerance;
    if (!k.kind)
        k.tolerance = RE_KEY_TOLERANCE;
    if (re_project_overlay_key(p, i, &now) || !memcmp(&k, &now, sizeof(k)))
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    rp_cut_set_key(p, cut_of(p, i), &k);
    rp_changed(p);
    return 1;
}
