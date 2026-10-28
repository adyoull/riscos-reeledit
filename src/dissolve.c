/* dissolve.c - dissolves between clips on V1 (project.h): MLT's playlist
 * mixes, made, lengthened and shortened, and kept linked to the clips
 * either side.
 *
 * A dissolve is an item of the playlist of its own: a tractor whose two
 * tracks are the end of the clip before and the start of the clip after,
 * with a luma transition (no picture: a plain dissolve) and a mix
 * transition (the sound crossfaded), marked "shotcut:transition" as
 * Shotcut marks its own (meant to let each open the other's: not yet
 * tried with Shotcut).
 *
 * MLT keeps a mix's links to its clips (what its resize and remove go by)
 * only in memory; rp_relink makes them again from what's beside each mix.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project_internal.h"

#include <stdlib.h>
#include <string.h>

/* The dissolve that item i is (MLT's mix: a tractor of two tracks, the
   clips' ends, and transitions), or NULL */
mlt_tractor rp_mix_of(re_project *p, int i)
{
    mlt_producer e = p->v1 ? mlt_playlist_get_clip(p->v1, i) : NULL;
    if (!e)
        return NULL;
    mlt_producer t = mlt_producer_cut_parent(e);
    if (!t || mlt_service_identify(MLT_PRODUCER_SERVICE(t)) != mlt_service_tractor_type
        || !mlt_properties_get(MLT_PRODUCER_PROPERTIES(t), "shotcut:transition"))
        return NULL;
    return (mlt_tractor) t;
}

int rp_is_clip(re_project *p, int i)
{
    return p->v1 && i >= 0 && i < mlt_playlist_count(p->v1) && !mlt_playlist_is_blank(p->v1, i) && !rp_mix_of(p, i);
}

/* MLT's links between a mix and the clips either side (what resizing and
   removing a mix go by) as they are now. MLT keeps them only in memory: a
   project read from XML (opened, or an undo) has none, and a split or a
   move changes which clip is beside a mix. */
void rp_relink(re_project *p)
{
    int n = p->v1 ? mlt_playlist_count(p->v1) : 0;
    for (int i = 0; i < n; i++)
        if (rp_is_clip(p, i)) {
            mlt_properties c = MLT_PRODUCER_PROPERTIES(mlt_playlist_get_clip(p->v1, i));
            mlt_properties_set_data(c, "mix_in", NULL, 0, NULL, NULL);
            mlt_properties_set_data(c, "mix_out", NULL, 0, NULL, NULL);
        }
    for (int i = 0; i < n; i++) {
        mlt_tractor t = rp_mix_of(p, i);
        if (!t)
            continue;
        mlt_properties tp = MLT_TRACTOR_PROPERTIES(t);
        mlt_properties_set_data(tp, "mlt_mix", t, 0, NULL, NULL);
        mlt_producer a = rp_is_clip(p, i - 1) ? mlt_playlist_get_clip(p->v1, i - 1) : NULL;
        mlt_producer b = rp_is_clip(p, i + 1) ? mlt_playlist_get_clip(p->v1, i + 1) : NULL;
        mlt_properties_set_data(tp, "mix_in", a, 0, NULL, NULL);
        mlt_properties_set_data(tp, "mix_out", b, 0, NULL, NULL);
        if (a)
            mlt_properties_set_data(MLT_PRODUCER_PROPERTIES(a), "mix_out", t, 0, NULL, NULL);
        if (b)
            mlt_properties_set_data(MLT_PRODUCER_PROPERTIES(b), "mix_in", t, 0, NULL, NULL);
    }
}

/* A dissolve made from spare frames: where the cut was, frames from its
   start (the clips' frames beyond it weren't in the project). Removed, the
   cut goes back there. (Unset: the clips overlapped; removed, each gets
   its frames back, as MLT's unmix does.) Saved: "shotcut:" properties are
   the only ones of a tractor MLT's XML keeps. */
#define CUT "shotcut:reeledit_cut"

/* A mix's transitions kept to its length (MLT's resize doesn't) */
static void fit_mix(mlt_tractor t, int length)
{
    mlt_service s = mlt_service_producer(MLT_TRACTOR_SERVICE(t)); /* (the last transition planted, then each before it) */
    while (s && mlt_service_identify(s) == mlt_service_transition_type) {
        mlt_transition_set_in_and_out((mlt_transition) s, 0, length - 1);
        s = mlt_service_producer(s);
    }
}

int re_project_dissolve(re_project *p, int item, int length, int overlap)
{
    re_item a, b;
    if (!rp_is_clip(p, item) || !rp_is_clip(p, item + 1) || length < 1 || re_project_item(p, item, &a)
        || re_project_item(p, item + 1, &b))
        return -1;
    int a_after = a.source_length > 0 ? a.source_length - 1 - a.out : 0; /* (frames beyond the cut) */
    int b_before = b.in;
    int e, cut = -1; /* (where the cut was in the dissolve: from spare frames) */
    if (!p->in_edit)
        rp_checkpoint(p);
    if (!overlap && a_after >= length)
        e = mlt_playlist_mix_in(p->v1, item, length), cut = 0;       /* (a's frames after its out: from the cut on) */
    else if (!overlap && b_before >= length)
        e = mlt_playlist_mix_out(p->v1, item, length), cut = length; /* (b's before its in: up to the cut) */
    else {
        int most = (a.length < b.length ? a.length : b.length) - 1; /* (each a frame at least) */
        if (length > most)
            length = most;
        e = length < 1 || mlt_playlist_mix(p->v1, item, length, NULL);
    }
    mlt_tractor t = e ? NULL : rp_mix_of(p, item + 1);
    if (!t) {
        /* (not a mix of ours yet: one MLT made has no "shotcut:transition") */
        mlt_producer m = e ? NULL : mlt_producer_cut_parent(mlt_playlist_get_clip(p->v1, item + 1));
        if (m && mlt_service_identify(MLT_PRODUCER_SERVICE(m)) == mlt_service_tractor_type)
            t = (mlt_tractor) m;
    }
    if (!t) {
        if (!p->in_edit)
            rp_no_edit(p);
        return -1;
    }
    /* as Shotcut makes them: a dissolve (luma with no picture) and the sound
       crossfaded; marked for Shotcut and for us */
    mlt_properties_set(MLT_TRACTOR_PROPERTIES(t), "shotcut:transition", "lumaMix");
    if (cut >= 0)
        mlt_properties_set_int(MLT_TRACTOR_PROPERTIES(t), CUT, cut);
    mlt_field field = mlt_tractor_field(t);
    mlt_transition luma = mlt_factory_transition(p->profile, "luma", NULL);
    mlt_transition mix = mlt_factory_transition(p->profile, "mix", NULL);
    if (luma) {
        mlt_field_plant_transition(field, luma, 0, 1);
        mlt_transition_close(luma);
    }
    if (mix) {
        mlt_properties mp = MLT_TRANSITION_PROPERTIES(mix);
        mlt_properties_set_double(mp, "start", 0.0);
        mlt_properties_set_double(mp, "end", 1.0);
        mlt_field_plant_transition(field, mix, 0, 1);
        mlt_transition_close(mix);
    }
    fit_mix(t, mlt_producer_get_playtime(MLT_TRACTOR_PRODUCER(t)));
    rp_set_fade(p, item, RP_PICTURE | RP_SOUND, 1, 0); /* (no fades at the ends dissolved) */
    rp_set_fade(p, item + 2, RP_PICTURE | RP_SOUND, 0, 0);
    rp_changed(p);
    return item + 1;
}

int re_project_dissolve_cut(re_project *p, int frame)
{
    int best = -1, bd = 0, n = p->v1 ? mlt_playlist_count(p->v1) : 0;
    for (int i = 0; i + 1 < n; i++) {
        re_item b;
        if (!rp_is_clip(p, i) || !rp_is_clip(p, i + 1) || re_project_item(p, i + 1, &b))
            continue;
        int d = b.start > frame ? b.start - frame : frame - b.start;
        if (best < 0 || d < bd) {
            best = i;
            bd = d;
        }
    }
    return best;
}

int re_project_dissolve_max(re_project *p, int mix)
{
    mlt_tractor t = rp_mix_of(p, mix);
    re_item a, b, m;
    if (!t || !rp_is_clip(p, mix - 1) || !rp_is_clip(p, mix + 1) || re_project_item(p, mix - 1, &a)
        || re_project_item(p, mix + 1, &b) || re_project_item(p, mix, &m))
        return 0;
    /* longer by d: a and b d shorter each (a frame left), track a's in d
       earlier, track b's out d later (in their files) */
    mlt_producer ta = mlt_tractor_get_track(t, 0), tb = mlt_tractor_get_track(t, 1);
    if (!ta || !tb)
        return m.length;
    int d = a.length - 1;
    if (b.length - 1 < d)
        d = b.length - 1;
    if (mlt_producer_get_in(ta) < d)
        d = mlt_producer_get_in(ta);
    int b_left = mlt_producer_get_length(mlt_producer_cut_parent(tb)) - 1 - mlt_producer_get_out(tb);
    if (b_left < d)
        d = b_left;
    return m.length + (d > 0 ? d : 0);
}

int re_project_dissolve_length(re_project *p, int mix, int length)
{
    mlt_tractor t = rp_mix_of(p, mix);
    re_item m;
    if (!t || re_project_item(p, mix, &m))
        return 0;
    int most = re_project_dissolve_max(p, mix);
    if (length > most)
        length = most;
    if (length < 1)
        length = 1;
    if (length == m.length)
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    if (mlt_playlist_resize_clip(p->v1, mix, 0, length - 1)) {
        if (!p->in_edit)
            rp_no_edit(p);
        return 0;
    }
    fit_mix(t, length);
    mlt_properties_clear(MLT_TRACTOR_PROPERTIES(t), CUT); /* (now the clips overlap: removed, both get their frames back) */
    rp_changed(p);
    return 1;
}


int rp_unmix(re_project *p, int mix)
{
    mlt_tractor t = rp_mix_of(p, mix);
    if (!t)
        return -1;
    mlt_properties tp = MLT_TRACTOR_PROPERTIES(t);
    if (!mlt_properties_get(tp, CUT) || !rp_is_clip(p, mix - 1) || !rp_is_clip(p, mix + 1))
        return mlt_playlist_remove(p->v1, mix); /* (MLT's unmix: each clip its frames back) */
    int cut = mlt_properties_get_int(tp, CUT);
    re_item a, b, m;
    re_project_item(p, mix - 1, &a);
    re_project_item(p, mix + 1, &b);
    re_project_item(p, mix, &m);
    /* (a plain remove, not MLT's unmix; then the cut where it was) */
    mlt_properties_set_data(tp, "mlt_mix", NULL, 0, NULL, NULL);
    if (mlt_playlist_remove(p->v1, mix))
        return -1;
    mlt_playlist_resize_clip(p->v1, mix - 1, a.in, a.out + cut);
    mlt_playlist_resize_clip(p->v1, mix, b.in - (m.length - cut), b.out);
    rp_relink(p);
    return 0;
}
