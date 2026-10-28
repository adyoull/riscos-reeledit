/* overlay.c - overlays (project.h): pictures and videos over the video,
 * on V2.
 *
 * V2 is a playlist of pictures, videos and gaps, the tractor's track above
 * V1 (made with the first overlay; a project from Shotcut: its first video
 * track after V1). Each overlay is put over V1 by MLT's core composite
 * transition over its frames (no Qt here for Shotcut's qtblend); how is
 * kept with the picture's cut (its playlist entry), saved in the project:
 *   shotcut:reeledit_place     where: full, centre, top-left, top-right,
 *                              bottom-left, bottom-right, lower-third
 *   shotcut:reeledit_size      the box it's fitted in (not full): % of the
 *                              frame's width and height
 *   shotcut:reeledit_opacity   %
 *   shotcut:reeledit_fade_in, _fade_out   frames
 * A video's sound is faded with it (its fades' volume filters on the cut,
 * as V1's: fade.c) or on its own, or muted (clipsound.c).
 * The transitions are made again from these after every edit
 * (rp_fit_overlays, from rp_changed, and for a project read), marked
 * shotcut:reeledit_overlay; the picture is fitted in its box keeping its
 * shape (composite's fill), and fades by the transition's opacity.
 *
 * A video's sound is mixed with V1's (summed) by a core mix transition
 * from V2 to V1 (marked the same; Shotcut makes one for each track),
 * unless the project has one already. A video on V2 is trimmed as a clip
 * is: its in moves with its start, and it's kept within its file.
 *
 * V2's edits put its items back in a new order: the overlays' cuts (with
 * what's kept on them) where they're wanted, gaps between. Overlays don't
 * overlap; edits keep each where there's room, within the project.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project_internal.h"
#include "still.h"
#include "title.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MARGIN 5 /* (% of the frame: the title-safe edge, for all but full) */

static const char *const PLACES[RE_PLACES] = {"full",        "centre",       "top-left",   "top-right",
                                              "bottom-left", "bottom-right", "lower-third"};

/* ---- V2 ----------------------------------------------------------------------------------- */

int re_project_overlays(re_project *p)
{
    return rp_track_items(p->v2);
}

/* The playlist index of overlay i, or -1 */
static int entry(re_project *p, int i)
{
    return rp_track_entry(p->v2, i);
}

static int place_of(const char *s)
{
    for (int i = 0; s && i < RE_PLACES; i++)
        if (!strcmp(s, PLACES[i]))
            return i;
    return RE_PLACE_FULL;
}

int re_project_overlay(re_project *p, int i, re_overlay *o)
{
    mlt_playlist_clip_info info;
    memset(o, 0, sizeof(*o));
    int e = entry(p, i);
    if (e < 0 || mlt_playlist_get_clip_info(p->v2, &info, e))
        return -1;
    mlt_properties cp = MLT_PRODUCER_PROPERTIES(info.cut);
    o->start = info.start;
    o->length = info.frame_count;
    o->video = !re_still_is(info.cut);
    o->title = re_title_is(info.cut);
    o->in = o->video ? info.frame_in : 0;
    o->source_length = o->video ? info.length : 0;
    o->place = place_of(mlt_properties_get(cp, "shotcut:reeledit_place"));
    o->size = mlt_properties_get(cp, "shotcut:reeledit_size") ? mlt_properties_get_int(cp, "shotcut:reeledit_size") : 100;
    o->opacity = mlt_properties_get(cp, "shotcut:reeledit_opacity") ? mlt_properties_get_int(cp, "shotcut:reeledit_opacity")
                                                                     : 100;
    o->fade_in = mlt_properties_get_int(cp, "shotcut:reeledit_fade_in");
    o->fade_out = mlt_properties_get_int(cp, "shotcut:reeledit_fade_out");
    o->sound = o->video && rp_cut_has_sound(info.cut);
    o->muted = o->video && rp_cut_muted(info.cut);
    o->volume = o->video ? rp_cut_volume(info.cut) : 0;
    o->sound_fade_in = o->video ? rp_cut_fade(info.cut, RP_SOUND, 0) : 0;
    o->sound_fade_out = o->video ? rp_cut_fade(info.cut, RP_SOUND, 1) : 0;
    /* (its leaf name, a picture's without its .png or .jpg: MLT's names are Unix ones) */
    char name[256];
    snprintf(name, sizeof(name), "%s", info.resource ? info.resource : "");
    char *dot = strrchr(name, '.'), *slash = strrchr(name, '/');
    if (dot && dot > slash && !o->video && (!strcmp(dot, ".png") || !strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg")))
        *dot = 0;
    const char *leaf = name;
    for (const char *c = name; *c; c++)
        if (*c == '/' || *c == '\\')
            leaf = c + 1;
    snprintf(o->name, sizeof(o->name), "%.63s", leaf);
    re_title t;
    if (o->title && !re_title_get(info.cut, &t)) { /* (a title's name: its first line) */
        char *nl = strchr(t.text, '\n');
        if (nl)
            *nl = 0;
        snprintf(o->name, sizeof(o->name), "%.63s", t.text[0] ? t.text : "Title");
    }
    return 0;
}

int re_project_overlay_at(re_project *p, int frame)
{
    re_overlay o;
    for (int i = 0; !re_project_overlay(p, i, &o); i++)
        if (frame >= o.start && frame < o.start + o.length)
            return i;
    return -1;
}

/* V2 made (the tractor's track above the others), if it isn't there */
static int make_v2(re_project *p)
{
    if (p->v2)
        return 0;
    if (!p->tractor || !(p->v2 = mlt_playlist_new(p->profile)))
        return -1;
    mlt_multitrack mt = mlt_tractor_multitrack(p->tractor);
    int n = mlt_multitrack_count(mt);
    char id[32];
    for (int k = 1;; k++) { /* (an id no track has) */
        snprintf(id, sizeof(id), "playlist%d", k);
        int used = 0;
        for (int i = 0; i < n; i++) {
            const char *t = mlt_properties_get(MLT_PRODUCER_PROPERTIES(mlt_multitrack_track(mt, i)), "id");
            used |= t && !strcmp(t, id);
        }
        if (!used)
            break;
    }
    mlt_properties_set(MLT_PLAYLIST_PROPERTIES(p->v2), "id", id);
    mlt_properties_set(MLT_PLAYLIST_PROPERTIES(p->v2), "shotcut:name", "V2");
    mlt_tractor_set_track(p->tractor, MLT_PLAYLIST_PRODUCER(p->v2), n);
    p->own_v2 = 1;
    p->v2_track = n;
    return 0;
}

/* V2 as it is (each cut with a reference of ours; a picture's in 0).
   Their number, or -1. */
static int read_v2(re_project *p, rp_slot **s)
{
    int n = rp_track_read(p->v2, s);
    for (int i = 0; i < n; i++)
        if (re_still_is((*s)[i].cut))
            (*s)[i].in = 0;
    return n;
}

/* ---- the composites ------------------------------------------------------------------------ */

/* The box an overlay is fitted in (% of the frame), and how it sits in it */
static void box(const re_overlay *o, double *x, double *y, double *w, double *h, const char **ha, const char **va);

/* A title's box: its picture's size (as % of the frame), where its place
   says (a lower third: at the bottom, across the middle), so it's shown at
   its own size; kept within the frame (made smaller, if it must be) */
static void title_box(re_project *p, mlt_producer cut, const re_overlay *o, double *x, double *y, double *w, double *h,
                      const char **ha, const char **va)
{
    int iw = 0, ih = 0;
    if (re_still_size(cut, &iw, &ih) || iw < 1 || ih < 1) {
        box(o, x, y, w, h, ha, va);
        return;
    }
    *w = 100.0 * iw / p->profile->width;
    *h = 100.0 * ih / p->profile->height;
    double most = 100 - 2 * MARGIN;
    if (*w > most) {
        *h *= most / *w;
        *w = most;
    }
    if (*h > most) {
        *w *= most / *h;
        *h = most;
    }
    *ha = "centre";
    *va = "middle";
    double left = MARGIN, right = 100 - MARGIN - *w, top = MARGIN, bottom = 100 - MARGIN - *h;
    double mid_x = (100 - *w) / 2, mid_y = (100 - *h) / 2;
    switch (o->place) {
    case RE_PLACE_TOP_LEFT: *x = left; *y = top; break;
    case RE_PLACE_TOP_RIGHT: *x = right; *y = top; break;
    case RE_PLACE_BOTTOM_LEFT: *x = left; *y = bottom; break;
    case RE_PLACE_BOTTOM_RIGHT: *x = right; *y = bottom; break;
    case RE_PLACE_LOWER_THIRD: *x = mid_x; *y = bottom; break;
    default: *x = mid_x; *y = mid_y; break; /* (centre, full) */
    }
}

static void box(const re_overlay *o, double *x, double *y, double *w, double *h, const char **ha, const char **va)
{
    double s = o->size < 1 ? 1 : o->size > 100 ? 100 : o->size;
    *w = *h = s;
    *x = *y = MARGIN;
    *ha = "left";
    *va = "top";
    switch (o->place) {
    case RE_PLACE_FULL:
        *x = *y = 0;
        *w = *h = 100;
        *ha = "centre";
        *va = "middle";
        break;
    case RE_PLACE_CENTRE:
        *x = *y = (100 - s) / 2;
        *ha = "centre";
        *va = "middle";
        break;
    case RE_PLACE_TOP_RIGHT: *x = 100 - MARGIN - s; *ha = "right"; break;
    case RE_PLACE_BOTTOM_LEFT: *y = 100 - MARGIN - s; *va = "bottom"; break;
    case RE_PLACE_BOTTOM_RIGHT:
        *x = 100 - MARGIN - s;
        *y = 100 - MARGIN - s;
        *ha = "right";
        *va = "bottom";
        break;
    case RE_PLACE_LOWER_THIRD:
        *w = 100 - 2 * MARGIN;
        *y = 100 - MARGIN - s;
        *va = "bottom";
        break;
    }
    if (*x < 0)
        *x = 0;
    if (*y < 0)
        *y = 0;
}

/* Our transitions taken out of the tractor */
static void drop_composites(re_project *p)
{
    mlt_field field = mlt_tractor_field(p->tractor);
    for (;;) {
        mlt_service found = NULL;
        for (mlt_service s = mlt_service_producer(MLT_TRACTOR_SERVICE(p->tractor)); s && !found;
             s = mlt_service_producer(s)) {
            if (mlt_service_identify(s) != mlt_service_transition_type)
                break; /* (the multitrack: the end) */
            if (mlt_properties_get_int(MLT_SERVICE_PROPERTIES(s), "shotcut:reeledit_overlay"))
                found = s;
        }
        if (!found)
            return;
        mlt_field_disconnect_service(field, found);
    }
}

/* A mix of V2's sound into V1's not of ours (Shotcut's) */
static int foreign_mix(re_project *p)
{
    for (mlt_service s = mlt_service_producer(MLT_TRACTOR_SERVICE(p->tractor)); s; s = mlt_service_producer(s)) {
        if (mlt_service_identify(s) != mlt_service_transition_type)
            break;
        mlt_properties tp = MLT_SERVICE_PROPERTIES(s);
        const char *svc = mlt_properties_get(tp, "mlt_service");
        if (svc && !strcmp(svc, "mix") && !mlt_properties_get_int(tp, "shotcut:reeledit_overlay")
            && mlt_transition_get_b_track(MLT_TRANSITION(s)) == p->v2_track)
            return 1;
    }
    return 0;
}

void rp_fit_overlays(re_project *p)
{
    if (!p->tractor)
        return;
    drop_composites(p);
    mlt_field field = mlt_tractor_field(p->tractor);
    re_overlay o;
    for (int i = 0; p->v2 && !re_project_overlay(p, i, &o); i++) {
        mlt_transition t = mlt_factory_transition(p->profile, "composite", NULL);
        if (!t)
            return;
        mlt_properties tp = MLT_TRANSITION_PROPERTIES(t);
        double x, y, w, h;
        const char *ha, *va;
        if (o.title) {
            mlt_playlist_clip_info info;
            mlt_playlist_get_clip_info(p->v2, &info, entry(p, i));
            title_box(p, info.cut, &o, &x, &y, &w, &h, &ha, &va);
        } else {
            box(&o, &x, &y, &w, &h, &ha, &va);
        }
        double op = (o.opacity < 0 ? 0 : o.opacity > 100 ? 100 : o.opacity) / 100.0;
        char rect[96], g[400];
        snprintf(rect, sizeof(rect), "%g%%/%g%%:%g%%x%g%%", x, y, w, h);
        /* (keyframes from the transition's in: faded from 0, to 0) */
        int L = o.length, fi = o.fade_in, fo = o.fade_out;
        if (fi + fo > L)
            fo = L - fi;
        int at[4] = {0, fi, L - fo - 1, L - 1};
        double v[4] = {fi > 0 ? 0 : op, op, op, fo > 0 ? 0 : op};
        int n = 0, last = -1;
        g[0] = 0;
        for (int k = 0; k < 4; k++) {
            if (at[k] <= last || at[k] > L - 1)
                continue;
            n += snprintf(g + n, sizeof(g) - n, "%s%d=%s:%g", n ? ";" : "", at[k], rect, v[k]);
            last = at[k];
        }
        mlt_properties_set(tp, "geometry", g);
        mlt_properties_set_int(tp, "fill", 1);
        mlt_properties_set(tp, "halign", ha);
        mlt_properties_set(tp, "valign", va);
        mlt_properties_set_int(tp, "shotcut:reeledit_overlay", 1);
        mlt_transition_set_in_and_out(t, o.start, o.start + o.length - 1);
        mlt_field_plant_transition(field, t, p->v1_track, p->v2_track);
        mlt_transition_close(t); /* (the field keeps it) */
    }
    /* a video's sound, with V1's: one mix for all of V2, unless there's one */
    int videos = 0;
    for (int i = 0; p->v2 && !re_project_overlay(p, i, &o); i++)
        videos += o.video;
    if (videos && !foreign_mix(p)) {
        mlt_transition t = mlt_factory_transition(p->profile, "mix", NULL);
        if (!t)
            return;
        mlt_properties tp = MLT_TRANSITION_PROPERTIES(t);
        mlt_properties_set_int(tp, "always_active", 1);
        mlt_properties_set_int(tp, "sum", 1);
        mlt_properties_set_int(tp, "shotcut:reeledit_overlay", 1);
        mlt_field_plant_transition(field, t, p->v1_track, p->v2_track);
        mlt_transition_close(t);
    }
}

/* ---- edits -------------------------------------------------------------------------------- */

/* A picture, a video or a title (pic: ours from here) put on V2 from a
   frame, so long, where there's room, at a place and size. Its index, or
   -1. */
static int put(re_project *p, mlt_producer pic, int frame, int length, int place, int size)
{
    if (frame < 0)
        frame = 0;
    rp_slot *s;
    int n = read_v2(p, &s);
    if (n < 0 || rp_track_room(re_project_length(p), s, n, -1, &frame, &length)) {
        for (int i = 0; i < n; i++)
            mlt_producer_close(s[i].cut);
        free(s);
        mlt_producer_close(pic);
        return -1;
    }
    rp_checkpoint(p);
    if (make_v2(p) < 0) {
        rp_no_edit(p);
        for (int i = 0; i < n; i++)
            mlt_producer_close(s[i].cut);
        free(s);
        mlt_producer_close(pic);
        return -1;
    }
    mlt_producer cut = mlt_producer_cut(pic, 0, length - 1);
    mlt_producer_close(pic);
    mlt_properties cp = MLT_PRODUCER_PROPERTIES(cut);
    mlt_properties_set(cp, "shotcut:reeledit_place", PLACES[place]);
    mlt_properties_set_int(cp, "shotcut:reeledit_size", size);
    mlt_properties_set_int(cp, "shotcut:reeledit_opacity", 100);
    s[n].cut = cut;
    s[n].start = frame;
    s[n].length = length;
    s[n].in = 0;
    rp_track_sort(s, n + 1);
    rp_track_write(p->v2, s, n + 1);
    rp_changed(p);
    return re_project_overlay_at(p, frame);
}

int re_project_overlay_add(re_project *p, const char *file, int frame, int length)
{
    int pw = 0, ph = 0;
    if (!p->tractor || re_project_length(p) <= 0)
        return -1;
    int video = re_still_open(file, &pw, &ph) != 0;
    mlt_producer pic;
    if (video) { /* (not a picture: a video, as a clip is opened) */
        pic = mlt_factory_producer(p->profile, NULL, file);
        if (!pic)
            return RE_OVERLAY_CANT_OPEN;
        int len = mlt_producer_get_length(pic);
        if (len >= INT_MAX / 2 || len < 1) { /* (raw video: no length known) */
            mlt_producer_close(pic);
            return RE_OVERLAY_NO_LENGTH;
        }
        if (length <= 0 || length > len)
            length = len;
    } else {
        char res[1100];
        snprintf(res, sizeof(res), "qimage:%s", file); /* (by the loader: its filters too) */
        pic = mlt_factory_producer(p->profile, NULL, res);
        if (!pic)
            return RE_OVERLAY_CANT_OPEN;
        if (length <= 0)
            length = re_project_frames(p, 5.0);
    }
    int big = !video && (pw * 2 >= p->profile->width || ph * 2 >= p->profile->height);
    return put(p, pic, frame, length, video ? RE_PLACE_BOTTOM_RIGHT : big ? RE_PLACE_FULL : RE_PLACE_TOP_RIGHT,
               video ? 33 : big ? 100 : 20);
}

int re_project_overlay_span(re_project *p, int i, int start, int length)
{
    re_overlay o;
    if (re_project_overlay(p, i, &o))
        return -1;
    return rp_track_span(p, p->v2, i, start, length, o.video, o.source_length);
}

/* One of an overlay's settings changed (not an edit of its own if
   in_edit). 1 changed, 0 not. */
static int set_prop(re_project *p, int i, const char *name, const char *text, int value)
{
    int e = entry(p, i);
    if (e < 0)
        return 0;
    mlt_producer cut = mlt_playlist_get_clip(p->v2, e);
    mlt_properties cp = MLT_PRODUCER_PROPERTIES(cut);
    const char *now = mlt_properties_get(cp, name);
    char v[32];
    if (!text) {
        snprintf(v, sizeof(v), "%d", value);
        text = v;
    }
    if (now && !strcmp(now, text))
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    mlt_properties_set(cp, name, text);
    rp_changed(p);
    return 1;
}

int re_project_overlay_set(re_project *p, int i, int place, int size, int opacity)
{
    re_overlay o;
    if (re_project_overlay(p, i, &o))
        return 0;
    int done = 0, edit = !p->in_edit;
    if (edit)
        re_project_begin(p);
    if (place >= 0 && place < RE_PLACES)
        done |= set_prop(p, i, "shotcut:reeledit_place", PLACES[place], 0);
    if (size > 0)
        done |= set_prop(p, i, "shotcut:reeledit_size", NULL, size > 100 ? 100 : size);
    if (opacity >= 0)
        done |= set_prop(p, i, "shotcut:reeledit_opacity", NULL, opacity > 100 ? 100 : opacity);
    if (edit)
        re_project_end(p);
    return done;
}

/* A video's sound faded at an end, so long (0: none). 1 changed, 0 not. */
static int sound_fade(re_project *p, int i, int end, int length)
{
    mlt_playlist_clip_info info;
    int e = entry(p, i);
    if (e < 0 || mlt_playlist_get_clip_info(p->v2, &info, e) || !rp_cut_has_sound(info.cut))
        return 0;
    if (length < 0)
        length = 0;
    if (length > info.frame_count)
        length = info.frame_count;
    if (rp_cut_fade(info.cut, RP_SOUND, end) == length)
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    rp_cut_set_fade(p, info.cut, RP_SOUND, end, length, info.frame_in, info.frame_out);
    rp_changed(p);
    return 1;
}

int re_project_overlay_fade(re_project *p, int i, int end, int length)
{
    re_overlay o;
    if (re_project_overlay(p, i, &o))
        return 0;
    if (length < 0)
        length = 0;
    int other = end ? o.fade_in : o.fade_out;
    if (length > o.length - other)
        length = o.length - other;
    /* (a video's sound fades with it) */
    int edit = !p->in_edit;
    if (edit)
        re_project_begin(p);
    int done = set_prop(p, i, end ? "shotcut:reeledit_fade_out" : "shotcut:reeledit_fade_in", NULL, length);
    if (o.sound)
        done |= sound_fade(p, i, end, length);
    if (edit)
        re_project_end(p);
    return done;
}

int re_project_overlay_sound_fade(re_project *p, int i, int end, int length)
{
    re_overlay o;
    if (re_project_overlay(p, i, &o) || !o.sound)
        return 0;
    return sound_fade(p, i, end, length);
}

int re_project_overlay_mute(re_project *p, int i, int on)
{
    mlt_playlist_clip_info info;
    int e = entry(p, i);
    if (e < 0 || mlt_playlist_get_clip_info(p->v2, &info, e) || !rp_cut_has_sound(info.cut)
        || rp_cut_muted(info.cut) == !!on)
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    rp_cut_mute(p, info.cut, on);
    rp_changed(p);
    return 1;
}

int re_project_overlay_remove(re_project *p, int i)
{
    int e = entry(p, i);
    if (e < 0)
        return 0;
    rp_checkpoint(p);
    mlt_producer gone = mlt_playlist_replace_with_blank(p->v2, e);
    if (gone)
        mlt_producer_close(gone);
    mlt_playlist_consolidate_blanks(p->v2, 1);
    int n = mlt_playlist_count(p->v2);
    if (n > 0 && mlt_playlist_is_blank(p->v2, n - 1))
        mlt_playlist_remove(p->v2, n - 1);
    rp_changed(p);
    return 1;
}

/* ---- titles ------------------------------------------------------------------------------ */

int re_project_title_add(re_project *p, const re_title *t, int frame, int length)
{
    if (!p->tractor || re_project_length(p) <= 0)
        return -1;
    mlt_producer pic = re_title_producer(p->profile, t);
    if (!pic)
        return -1;
    if (length <= 0)
        length = re_project_frames(p, 5.0);
    return put(p, pic, frame, length, RE_PLACE_LOWER_THIRD, 100);
}

int re_project_title(re_project *p, int i, re_title *t)
{
    mlt_playlist_clip_info info;
    int e = entry(p, i);
    if (e < 0 || mlt_playlist_get_clip_info(p->v2, &info, e))
        return -1;
    return re_title_get(info.cut, t);
}

int re_project_title_set(re_project *p, int i, const re_title *t)
{
    mlt_playlist_clip_info info;
    re_title now;
    int e = entry(p, i);
    if (e < 0 || mlt_playlist_get_clip_info(p->v2, &info, e) || re_title_get(info.cut, &now))
        return 0;
    if (!memcmp(&now, t, sizeof(now)) || (!strcmp(now.text, t->text) && !strcmp(now.font, t->font)
                                           && now.size == t->size && now.colour == t->colour
                                           && now.style == t->style && now.align == t->align))
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    re_title_put(info.cut, t);
    rp_changed(p); /* (its composite's box made again: its new size) */
    return 1;
}

int re_project_overlay_volume(re_project *p, int i, int db)
{
    mlt_playlist_clip_info info;
    int e = entry(p, i);
    db = db < -60 ? -60 : db > 12 ? 12 : db;
    if (e < 0 || mlt_playlist_get_clip_info(p->v2, &info, e) || !rp_cut_has_sound(info.cut)
        || rp_cut_volume(info.cut) == db)
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    rp_cut_set_volume(p, info.cut, db);
    rp_changed(p);
    return 1;
}
