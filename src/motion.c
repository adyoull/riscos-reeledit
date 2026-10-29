/* motion.c - pan and zoom (project.h): a clip's picture moving over its
 * length, zooming in or out, or panning across (the "Ken Burns" effect on
 * a photo).
 *
 * As Shotcut does it: a filter on the clip's cut, MLT's "affine" service
 * with an animated "transition.rect" (where the clip's picture goes in the
 * frame, in the profile's pixels, from the first frame to the last:
 * "0=x y w h 1;-1=x y w h 1", the second at the cut's end whatever its
 * length, so a trimmed clip keeps all of its motion), marked
 * "shotcut:filter" affineSizePosition as Shotcut's Size, Position & Rotate
 * filter, so either opens the other's. Ours also keeps what the motion is
 * (shotcut:reeledit_motion, shotcut:reeledit_amount).
 *
 * The filter itself is affine.c's.
 *
 * A split clip: each part has all of the motion. A dissolve's ends hold
 * still: the clip before's at its last rect, the clip after's at its first
 * (rp_fit_motion), so nothing jumps.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOTION "affineSizePosition"

const char *const re_motions[RE_MOTIONS] = {"None",           "Zoom in",        "Zoom out", "Pan left to right",
                                            "Pan right to left", "Pan upwards", "Pan downwards"};

/* ---- motions as rects ------------------------------------------------------------------------ */

/* The rect showing a window 1/z of the frame centred at (cx, cy) (0..1) */
static void rect_of(mlt_profile prof, double z, double cx, double cy, int *r)
{
    double W = prof->width, H = prof->height;
    r[2] = (int) lround(W * z);
    r[3] = (int) lround(H * z);
    r[0] = (int) lround(W / 2 - z * cx * W);
    r[1] = (int) lround(H / 2 - z * cy * H);
}

/* A motion's first and last rects */
static void rects(mlt_profile prof, const re_motion *m, int *a, int *b)
{
    double z = 1 + m->amount / 100.0, lo = 1 / (2 * z), hi = 1 - lo;
    switch (m->kind) {
    case 1: rect_of(prof, 1, .5, .5, a); rect_of(prof, z, .5, .5, b); break;
    case 2: rect_of(prof, z, .5, .5, a); rect_of(prof, 1, .5, .5, b); break;
    case 3: rect_of(prof, z, lo, .5, a); rect_of(prof, z, hi, .5, b); break;
    case 4: rect_of(prof, z, hi, .5, a); rect_of(prof, z, lo, .5, b); break;
    case 5: rect_of(prof, z, .5, hi, a); rect_of(prof, z, .5, lo, b); break;
    case 6: rect_of(prof, z, .5, lo, a); rect_of(prof, z, .5, hi, b); break;
    default: rect_of(prof, 1, .5, .5, a); rect_of(prof, 1, .5, .5, b);
    }
}

static void clamp(re_motion *m)
{
    m->kind = m->kind < 0 || m->kind >= RE_MOTIONS ? 0 : m->kind;
    m->amount = m->amount < 10 ? 10 : m->amount > 100 ? 100 : m->amount;
    if (!m->kind)
        m->amount = RE_MOTION_AMOUNT;
}

void re_motion_none(re_motion *m)
{
    m->kind = 0;
    m->amount = RE_MOTION_AMOUNT;
}

int rp_cut_motion(mlt_producer cut, re_motion *m)
{
    re_motion_none(m);
    mlt_filter f = cut ? rp_cut_filter(cut, MOTION) : NULL;
    if (!f)
        return 0;
    mlt_properties fp = MLT_FILTER_PROPERTIES(f);
    if (mlt_properties_get(fp, "shotcut:reeledit_motion")) {
        m->kind = mlt_properties_get_int(fp, "shotcut:reeledit_motion");
        m->amount = mlt_properties_get_int(fp, "shotcut:reeledit_amount");
        clamp(m);
    } else
        m->kind = -1; /* (one of Shotcut's own) */
    return 1;
}

/* The filter on (a motion; hold: 1 the first rect throughout, 2 the last)
   or off */
static void set_rect(re_project *p, mlt_producer cut, const re_motion *m, int hold)
{
    mlt_filter f = rp_cut_filter(cut, MOTION);
    if (m->kind <= 0) {
        if (f && m->kind == 0)
            mlt_service_detach(MLT_PRODUCER_SERVICE(cut), f);
        return;
    }
    if (!f) {
        f = mlt_factory_filter(re_project_profile(p), "affine", NULL);
        if (!f)
            return;
        mlt_properties_set(MLT_FILTER_PROPERTIES(f), "shotcut:filter", MOTION);
        mlt_service_attach(MLT_PRODUCER_SERVICE(cut), f);
        mlt_filter_close(f);
    }
    int a[4], b[4];
    rects(re_project_profile(p), m, a, b);
    if (hold)
        memcpy(hold == 1 ? b : a, hold == 1 ? a : b, sizeof(a));
    char s[160];
    snprintf(s, sizeof(s), "0=%d %d %d %d 1;-1=%d %d %d %d 1", a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
    mlt_properties fp = MLT_FILTER_PROPERTIES(f);
    mlt_properties_set(fp, "transition.rect", s);
    mlt_properties_set_int(fp, "transition.fill", 1);
    mlt_properties_set_int(fp, "transition.distort", 0);
    mlt_properties_set_int(fp, "shotcut:reeledit_motion", m->kind);
    mlt_properties_set_int(fp, "shotcut:reeledit_amount", m->amount);
    mlt_properties_set_int(fp, "shotcut:reeledit_hold", hold);
}

void rp_cut_set_motion(re_project *p, mlt_producer cut, const re_motion *m)
{
    set_rect(p, cut, m, 0);
}

/* The filter's in and out its cut's (as Shotcut has them): where it is in
   the clip, and how long that is */
static void fit_span(re_project *p, mlt_producer cut)
{
    mlt_filter f = rp_cut_filter(cut, MOTION);
    if (f) { /* (ours: its rect written again, its end "-1" once more: MLT writes a rect it has read with the
                frame numbers it worked out, for the length it had then) */
        mlt_properties fp = MLT_FILTER_PROPERTIES(f);
        int hold = mlt_properties_get_int(fp, "shotcut:reeledit_hold");
        re_motion m;
        if (rp_cut_motion(cut, &m) && m.kind > 0)
            set_rect(p, cut, &m, hold);
    }
    int in = mlt_producer_get_in(cut), out = mlt_producer_get_out(cut);
    if (f && (mlt_filter_get_in(f) != in || mlt_filter_get_out(f) != out))
        mlt_filter_set_in_and_out(f, in, out);
}

void rp_fit_motion(re_project *p)
{
    int n = p->v1 ? mlt_playlist_count(p->v1) : 0;
    for (int i = 0; i < n; i++)
        if (rp_is_clip(p, i))
            fit_span(p, mlt_playlist_get_clip(p->v1, i));
    for (int i = 0; i < n; i++) {
        mlt_tractor t = rp_mix_of(p, i);
        if (!t)
            continue;
        mlt_multitrack mt = mlt_tractor_multitrack(t);
        for (int k = 0; k < 2 && k < mlt_multitrack_count(mt); k++) {
            int clip = k ? i + 1 : i - 1;
            mlt_producer end = mlt_multitrack_track(mt, k);
            if (!end || !mlt_producer_is_cut(end))
                continue;
            re_motion want, now;
            rp_cut_motion(rp_is_clip(p, clip) ? mlt_playlist_get_clip(p->v1, clip) : NULL, &want);
            if (want.kind < 0)
                continue; /* (Shotcut's own: left as it is) */
            int hold = k ? 1 : 2; /* (the clip before: its last rect; after: its first) */
            rp_cut_motion(end, &now);
            mlt_filter f = rp_cut_filter(end, MOTION);
            int held = f ? mlt_properties_get_int(MLT_FILTER_PROPERTIES(f), "shotcut:reeledit_hold") : 0;
            if (memcmp(&want, &now, sizeof(now)) || (want.kind && held != hold))
                set_rect(p, end, &want, hold); /* (held: where it is in the dissolve doesn't matter, so no in or out) */
        }
    }
}

int re_project_motion(re_project *p, int item, re_motion *m)
{
    if (!rp_is_clip(p, item)) {
        re_motion_none(m);
        return -1;
    }
    rp_cut_motion(mlt_playlist_get_clip(p->v1, item), m);
    return 0;
}

int re_project_set_motion(re_project *p, int item, const re_motion *m)
{
    re_motion want = *m, now;
    clamp(&want);
    if (re_project_motion(p, item, &now) || !memcmp(&now, &want, sizeof(now)))
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    rp_cut_set_motion(p, mlt_playlist_get_clip(p->v1, item), &want);
    rp_changed(p);
    return 1;
}
