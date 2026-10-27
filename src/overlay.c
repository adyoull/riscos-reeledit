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

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MARGIN 5 /* (% of the frame: the title-safe edge, for all but full) */

static const char *const PLACES[RE_PLACES] = {"full",        "centre",       "top-left",   "top-right",
                                              "bottom-left", "bottom-right", "lower-third"};

/* ---- V2 ----------------------------------------------------------------------------------- */

/* V2's overlays (not its gaps): the playlist's index of each, their number */
static int list(re_project *p, int *at, int max)
{
    int n = 0, k = p->v2 ? mlt_playlist_count(p->v2) : 0;
    for (int i = 0; i < k; i++)
        if (!mlt_playlist_is_blank(p->v2, i)) {
            if (n < max && at)
                at[n] = i;
            n++;
        }
    return n;
}

int re_project_overlays(re_project *p)
{
    return list(p, NULL, 0);
}

/* The playlist index of overlay i, or -1 */
static int entry(re_project *p, int i)
{
    int n = re_project_overlays(p);
    if (i < 0 || i >= n)
        return -1;
    int *at = malloc(n * sizeof(int)), e = -1;
    if (at) {
        list(p, at, n);
        e = at[i];
        free(at);
    }
    return e;
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
    o->in = o->video ? info.frame_in : 0;
    o->source_length = o->video ? info.length : 0;
    o->place = place_of(mlt_properties_get(cp, "shotcut:reeledit_place"));
    o->size = mlt_properties_get(cp, "shotcut:reeledit_size") ? mlt_properties_get_int(cp, "shotcut:reeledit_size") : 100;
    o->opacity = mlt_properties_get(cp, "shotcut:reeledit_opacity") ? mlt_properties_get_int(cp, "shotcut:reeledit_opacity")
                                                                     : 100;
    o->fade_in = mlt_properties_get_int(cp, "shotcut:reeledit_fade_in");
    o->fade_out = mlt_properties_get_int(cp, "shotcut:reeledit_fade_out");
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

/* V2 as it's to be: each overlay's cut (a reference of ours, given up
   here), at a start, so long; in order, not overlapping. */
typedef struct
{
    mlt_producer cut;
    int start, length;
    int in; /* (a video's first frame in its file; a picture's 0) */
} slot;

static void write_v2(re_project *p, slot *s, int n)
{
    mlt_playlist_clear(p->v2);
    int at = 0;
    for (int i = 0; i < n; i++) {
        if (s[i].start > at)
            mlt_playlist_blank(p->v2, s[i].start - at - 1);
        int in = s[i].in, out = s[i].in + s[i].length - 1;
        mlt_playlist_append_io(p->v2, s[i].cut, in, out);
        mlt_playlist_resize_clip(p->v2, mlt_playlist_count(p->v2) - 1, in, out);
        mlt_producer_close(s[i].cut);
        at = s[i].start + s[i].length;
    }
}

/* V2 as it is (each cut with a reference of ours). Their number, or -1. */
static int read_v2(re_project *p, slot **s)
{
    int n = re_project_overlays(p);
    *s = calloc(n + 1, sizeof(slot));
    if (!*s)
        return -1;
    for (int i = 0; i < n; i++) {
        mlt_playlist_clip_info info;
        mlt_playlist_get_clip_info(p->v2, &info, entry(p, i));
        (*s)[i].cut = info.cut;
        mlt_properties_inc_ref(MLT_PRODUCER_PROPERTIES(info.cut));
        (*s)[i].start = info.start;
        (*s)[i].length = info.frame_count;
        (*s)[i].in = re_still_is(info.cut) ? 0 : info.frame_in;
    }
    return n;
}

/* Where there's room for an overlay from start, so long, by slot k's
   neighbours (k: its place among them; skip: a slot not counted, being
   moved) and the project's end: start and length kept within it. 0, or -1
   (no room at all). */
static int room(re_project *p, slot *s, int n, int skip, int *start, int *length)
{
    int lo = 0, hi = re_project_length(p);
    for (int i = 0; i < n; i++) {
        if (i == skip)
            continue;
        int a = s[i].start, b = a + s[i].length;
        if (b <= *start && b > lo)
            lo = b;
        else if (a >= *start && a < hi)
            hi = a;
        else if (a < *start && b > *start) /* (it's in one: after it) */
            lo = b;
    }
    if (*start < lo)
        *start = lo;
    if (hi - *start < 1)
        return -1;
    if (*length > hi - *start)
        *length = hi - *start;
    if (*length < 1)
        *length = 1;
    return 0;
}

static int by_start(const void *a, const void *b)
{
    return ((const slot *) a)->start - ((const slot *) b)->start;
}

/* ---- the composites ------------------------------------------------------------------------ */

/* The box an overlay is fitted in (% of the frame), and how it sits in it */
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
        box(&o, &x, &y, &w, &h, &ha, &va);
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
    if (frame < 0)
        frame = 0;
    slot *s;
    int n = read_v2(p, &s);
    if (n < 0 || room(p, s, n, -1, &frame, &length)) {
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
    /* (a picture as big as the frame, or near it: all of it; a smaller one
       a logo, top right; a video a picture in the picture, bottom right) */
    int big = !video && (pw * 2 >= p->profile->width || ph * 2 >= p->profile->height);
    mlt_properties_set(cp, "shotcut:reeledit_place",
                       PLACES[video ? RE_PLACE_BOTTOM_RIGHT : big ? RE_PLACE_FULL : RE_PLACE_TOP_RIGHT]);
    mlt_properties_set_int(cp, "shotcut:reeledit_size", video ? 33 : big ? 100 : 20);
    mlt_properties_set_int(cp, "shotcut:reeledit_opacity", 100);
    s[n].cut = cut;
    s[n].start = frame;
    s[n].length = length;
    s[n].in = 0;
    qsort(s, n + 1, sizeof(slot), by_start);
    write_v2(p, s, n + 1);
    free(s);
    rp_changed(p);
    return re_project_overlay_at(p, frame);
}

int re_project_overlay_span(re_project *p, int i, int start, int length)
{
    re_overlay o;
    if (re_project_overlay(p, i, &o))
        return -1;
    slot *s;
    int n = read_v2(p, &s);
    if (n < 0)
        return -1;
    if (length < 1)
        length = 1;
    int end = o.start + o.length;
    if (start != o.start && start + length == end) {
        /* its start trimmed (its end where it was): kept clear of the one
           before, and a video to its file's start (its in moves with it) */
        int lo = i > 0 ? s[i - 1].start + s[i - 1].length : 0;
        if (o.video && o.start - o.in > lo)
            lo = o.start - o.in;
        if (start < lo)
            start = lo;
        if (start > end - 1)
            start = end - 1;
        length = end - start;
        s[i].in = o.video ? o.in + (start - o.start) : 0;
    } else if (start != o.start) {
        if (start < 0)
            start = 0;
        /* a move: kept to the room either side, its length kept */
        int lo = i > 0 ? s[i - 1].start + s[i - 1].length : 0;
        int hi = i + 1 < n ? s[i + 1].start : re_project_length(p);
        if (start + length > hi)
            start = hi - length;
        if (start < lo)
            start = lo;
        if (start + length > hi)
            length = hi - start;
    } else {
        /* its end trimmed: to the room after it, a video to its file's end */
        if (o.video && o.source_length > 0 && length > o.source_length - o.in)
            length = o.source_length - o.in;
        if (room(p, s, n, i, &start, &length))
            length = o.length;
    }
    if (length < 1 || (start == o.start && length == o.length)) {
        for (int k = 0; k < n; k++)
            mlt_producer_close(s[k].cut);
        free(s);
        return o.start == start && o.length == length ? 0 : -1;
    }
    if (!p->in_edit)
        rp_checkpoint(p);
    s[i].start = start;
    s[i].length = length;
    write_v2(p, s, n);
    free(s);
    rp_changed(p);
    return 1;
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
    return set_prop(p, i, end ? "shotcut:reeledit_fade_out" : "shotcut:reeledit_fade_in", NULL, length);
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
