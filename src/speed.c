/* speed.c - a clip's speed (project.h): faster, slower, reversed.
 *
 * A clip at another speed is a cut of MLT's timewarp producer ("2:file",
 * "-1:file"), as Shotcut makes one: its frames are the file's at the new
 * rate (a 200% clip has half as many), its sound sped up or slowed down
 * with them (and its pitch: MLT's pitch keeping needs Rubber Band, which
 * isn't built in). Back to 100%, it's the file again.
 *
 * Changing a clip's speed keeps the part of the file it shows (its in and
 * out, in the file's time) and where it starts; what follows moves along
 * as it gets longer or shorter. Its fades, mute and level are kept. Not a
 * picture, nor a clip in a dissolve (take the dissolve out first). One
 * edit.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project_internal.h"
#include "still.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A producer's speed (1: as it is) and its file */
static double speed_of(mlt_producer parent, const char **file)
{
    mlt_properties pp = MLT_PRODUCER_PROPERTIES(parent);
    const char *service = mlt_properties_get(pp, "mlt_service");
    if (service && !strcmp(service, "timewarp")) {
        if (file)
            *file = mlt_properties_get(pp, "warp_resource");
        double s = mlt_properties_get_double(pp, "warp_speed");
        return s != 0 ? s : 1;
    }
    if (file)
        *file = mlt_properties_get(pp, "resource");
    return 1;
}

int rp_speed_percent(mlt_producer parent)
{
    return parent ? (int) lround(speed_of(parent, NULL) * 100) : 100;
}

const char *rp_speed_file(mlt_producer parent, const char *resource)
{
    const char *file = NULL;
    if (parent)
        speed_of(parent, &file);
    return file && *file ? file : resource && *resource ? resource : NULL;
}

int re_item_file_frame(const re_item *it, int f)
{
    if (it->held)
        return it->held_frame;
    double s = it->speed / 100.0;
    if (s == 0)
        s = 1;
    return (int) floor(s > 0 ? f * s : (it->source_length - 1 - f) * -s);
}

int re_project_speed(re_project *p, int item, int percent)
{
    re_item it, n;
    mlt_playlist_clip_info info;
    if (percent == 0 || abs(percent) < 10 || abs(percent) > 2000 || re_project_item(p, item, &it) || it.gap
        || it.mix || it.speed == percent || (item > 0 && !re_project_item(p, item - 1, &n) && n.mix)
        || (!re_project_item(p, item + 1, &n) && n.mix) || mlt_playlist_get_clip_info(p->v1, &info, item)
        || it.picture || it.countdown)
        return 0;
    const char *file = rp_speed_file(info.producer, info.resource);
    if (!file)
        return 0;
    /* the part of the file it shows: file frames f0 to f1 (f1 not shown) */
    double s = it.speed / 100.0, a = fabs(s);
    double f0 = s > 0 ? it.in * a : (it.source_length - 1 - it.out) * a;
    double f1 = s > 0 ? (it.out + 1) * a : (it.source_length - it.in) * a;
    /* the file at the new speed */
    double s2 = percent / 100.0, a2 = fabs(s2);
    char res[1100];
    mlt_producer prod;
    if (percent == 100) {
        prod = rp_open_clip(p, file);
    } else {
        snprintf(res, sizeof(res), "timewarp:%g:%s", s2, file); /* (by the loader: its resampler too) */
        prod = mlt_factory_producer(p->profile, NULL, res);
    }
    if (!prod)
        return 0;
    int len = mlt_producer_get_length(prod);
    if (len < 1 || len >= INT_MAX / 2) {
        mlt_producer_close(prod);
        return 0;
    }
    int in = s2 > 0 ? (int) lround(f0 / a2) : len - (int) lround(f1 / a2);
    int out = s2 > 0 ? (int) lround(f1 / a2) - 1 : len - 1 - (int) lround(f0 / a2);
    in = in < 0 ? 0 : in > len - 1 ? len - 1 : in;
    out = out < in ? in : out > len - 1 ? len - 1 : out;
    mlt_producer cut = mlt_producer_cut(prod, in, out);
    mlt_producer_close(prod); /* (the cut has it) */
    re_colour colour;
    re_project_colour(p, item, &colour);
    re_sound_fx fx;
    re_project_sound_fx(p, item, &fx);
    re_motion motion;
    re_project_motion(p, item, &motion);
    int effect = re_project_effect(p, item);
    re_project_begin(p); /* (one undo: the clip taken out, put back at its speed, its effects) */
    int removed = re_project_remove(p, item, 1), ok = 0;
    if (removed)
        ok = rp_insert_cut(p, cut, it.start) == item;
    else
        mlt_producer_close(cut);
    if (ok) {
        for (int end = 0; end < 2; end++) {
            int pic = end ? it.fade_out : it.fade_in, snd = end ? it.sound_fade_out : it.sound_fade_in;
            if (pic)
                re_project_fade(p, item, end, pic);
            if (snd != pic)
                re_project_sound_fade(p, item, end, snd);
        }
        if (it.muted)
            re_project_mute(p, item, 1);
        if (it.volume)
            re_project_volume(p, item, it.volume);
        re_project_set_colour(p, item, &colour);
        re_project_set_sound_fx(p, item, &fx);
        if (motion.kind > 0)
            re_project_set_motion(p, item, &motion);
        re_project_set_effect(p, item, effect);
    }
    re_project_end(p);
    if (!ok && removed) /* (half done: undone) */
        re_project_undo(p);
    return ok;
}
