/* audio.c - audio tracks (project.h): music and sound effects under the
 * video, A1, A2 and on, below V1.
 *
 * Each is a playlist of sounds and gaps (track.c), a track of the tractor
 * marked as Shotcut marks its audio tracks ("shotcut:audio" 1, its name
 * "shotcut:name" A1...), its pictures hidden ("hide" 1: an audio file
 * gives black ones, a video's are left out). Its sound is mixed with V1's
 * by one core mix transition per track (summed, always active; marked
 * "shotcut:reeledit_audio", made again after every edit), unless the
 * project has a mix of its own into that track (Shotcut's from its
 * background).
 *
 * Sounds stack: a sound put where another is playing goes on the next
 * track with room for all of it there, a new track if none has. Each is
 * moved and trimmed as an overlay is (its in moves with its start, kept
 * within its file); muted, given a level and faded as a clip's sound is
 * (clipsound.c's filters on its cut).
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TRACKS 16

/* The audio tracks, in order, and their places in the tractor. Their number. */
static int tracks(re_project *p, mlt_playlist *pl, int *index)
{
    int n = 0;
    if (!p->tractor)
        return 0;
    mlt_multitrack mt = mlt_tractor_multitrack(p->tractor);
    for (int i = 0; i < mlt_multitrack_count(mt) && n < MAX_TRACKS; i++) {
        mlt_producer t = mlt_multitrack_track(mt, i);
        if (t && mlt_service_identify(MLT_PRODUCER_SERVICE(t)) == mlt_service_playlist_type
            && mlt_properties_get_int(MLT_PRODUCER_PROPERTIES(t), "shotcut:audio")) {
            if (pl)
                pl[n] = (mlt_playlist) t;
            if (index)
                index[n] = i;
            n++;
        }
    }
    return n;
}

static mlt_playlist track(re_project *p, int k)
{
    mlt_playlist pl[MAX_TRACKS];
    int n = tracks(p, pl, NULL);
    return k >= 0 && k < n ? pl[k] : NULL;
}

int re_project_audio_tracks(re_project *p)
{
    return tracks(p, NULL, NULL);
}

int re_project_audio_count(re_project *p, int k)
{
    return rp_track_items(track(p, k));
}

/* A new audio track, after the others (the tractor's last). NULL if not. */
static mlt_playlist new_track(re_project *p)
{
    int n = tracks(p, NULL, NULL);
    if (!p->tractor || n >= MAX_TRACKS)
        return NULL;
    mlt_playlist pl = mlt_playlist_new(p->profile);
    if (!pl)
        return NULL;
    mlt_multitrack mt = mlt_tractor_multitrack(p->tractor);
    int count = mlt_multitrack_count(mt);
    char id[32], name[16];
    for (int k = 1;; k++) { /* (an id no track has) */
        snprintf(id, sizeof(id), "playlist%d", k);
        int used = 0;
        for (int i = 0; i < count; i++) {
            const char *t = mlt_properties_get(MLT_PRODUCER_PROPERTIES(mlt_multitrack_track(mt, i)), "id");
            used |= t && !strcmp(t, id);
        }
        if (!used)
            break;
    }
    snprintf(name, sizeof(name), "A%d", n + 1);
    mlt_properties pp = MLT_PLAYLIST_PROPERTIES(pl);
    mlt_properties_set(pp, "id", id);
    mlt_properties_set(pp, "shotcut:name", name);
    mlt_properties_set_int(pp, "shotcut:audio", 1);
    mlt_properties_set_int(pp, "hide", 1); /* (its pictures not shown) */
    mlt_tractor_set_track(p->tractor, MLT_PLAYLIST_PRODUCER(pl), count);
    mlt_playlist_close(pl); /* (the tractor keeps it) */
    return pl;
}

int re_project_audio(re_project *p, int k, int i, re_audio *o)
{
    mlt_playlist_clip_info info;
    mlt_playlist pl = track(p, k);
    memset(o, 0, sizeof(*o));
    int e = rp_track_entry(pl, i);
    if (e < 0 || mlt_playlist_get_clip_info(pl, &info, e))
        return -1;
    o->track = k;
    o->start = info.start;
    o->length = info.frame_count;
    o->in = info.frame_in;
    o->source_length = info.length;
    o->muted = rp_cut_muted(info.cut);
    o->volume = rp_cut_volume(info.cut);
    o->fade_in = rp_cut_fade(info.cut, RP_SOUND, 0);
    o->fade_out = rp_cut_fade(info.cut, RP_SOUND, 1);
    const char *leaf = info.resource ? info.resource : "";
    for (const char *c = leaf; *c; c++)
        if (*c == '/' || *c == '\\')
            leaf = c + 1;
    snprintf(o->name, sizeof(o->name), "%.63s", leaf);
    return 0;
}

mlt_playlist rp_audio_track(re_project *p, int k)
{
    return track(p, k);
}

const char *re_project_audio_file(re_project *p, int k, int i)
{
    mlt_playlist_clip_info info;
    mlt_playlist pl = track(p, k);
    int e = rp_track_entry(pl, i);
    if (e < 0 || mlt_playlist_get_clip_info(pl, &info, e))
        return NULL;
    return info.resource && *info.resource ? info.resource : NULL; /* (MLT keeps a picture's without "qimage:") */
}

int re_project_audio_at(re_project *p, int k, int frame)
{
    re_audio o;
    for (int i = 0; !re_project_audio(p, k, i, &o); i++)
        if (frame >= o.start && frame < o.start + o.length)
            return i;
    return -1;
}

/* The cut of sound i on track k, or NULL */
static mlt_producer cut_of(re_project *p, int k, int i)
{
    mlt_playlist pl = track(p, k);
    int e = rp_track_entry(pl, i);
    return e >= 0 ? mlt_playlist_get_clip(pl, e) : NULL;
}

int re_project_audio_add(re_project *p, const char *file, int frame, int length, int *at_track, int *at)
{
    if (!p->tractor || re_project_length(p) <= 0)
        return -1;
    mlt_producer prod = mlt_factory_producer(p->profile, NULL, file);
    if (!prod)
        return RE_SOUND_CANT_OPEN;
    int len = mlt_producer_get_length(prod);
    if (len >= INT_MAX / 2 || len < 1 || !rp_cut_has_sound(prod)) {
        mlt_producer_close(prod);
        return len >= INT_MAX / 2 || len < 1 ? RE_SOUND_CANT_OPEN : RE_SOUND_NO_SOUND;
    }
    if (length <= 0 || length > len)
        length = len;
    return rp_audio_put(p, prod, 0, frame, length, at_track, at);
}

int rp_audio_put(re_project *p, mlt_producer prod, int in, int frame, int length, int *at_track, int *at)
{
    if (frame < 0)
        frame = 0;
    int end = re_project_length(p);
    if (frame >= end) {
        mlt_producer_close(prod);
        return -1;
    }
    int want = length < end - frame ? length : end - frame; /* (all of it, or to the project's end) */
    /* the first track with room for all of it there */
    mlt_playlist pl[MAX_TRACKS];
    int n = tracks(p, pl, NULL), k = 0;
    rp_slot *s = NULL;
    int items = 0;
    for (k = 0; k < n; k++) {
        int st = frame, ln = want;
        items = rp_track_read(pl[k], &s);
        if (items >= 0 && !rp_track_room(end, s, items, -1, &st, &ln) && st == frame && ln == want)
            break;
        if (items >= 0)
            rp_track_free(s, items);
        s = NULL;
    }
    rp_checkpoint(p);
    mlt_playlist target = k < n ? pl[k] : new_track(p);
    if (!target) {
        rp_no_edit(p);
        mlt_producer_close(prod);
        return -1;
    }
    if (!s) {
        items = 0;
        s = calloc(1, sizeof(rp_slot));
        if (!s) {
            rp_no_edit(p);
            mlt_producer_close(prod);
            return -1;
        }
    } else {
        rp_slot *more = realloc(s, (items + 1) * sizeof(rp_slot));
        if (!more) {
            rp_track_free(s, items);
            rp_no_edit(p);
            mlt_producer_close(prod);
            return -1;
        }
        s = more;
    }
    s[items].cut = mlt_producer_cut(prod, in, in + want - 1);
    mlt_producer_close(prod);
    s[items].start = frame;
    s[items].length = want;
    s[items].in = in;
    rp_track_sort(s, items + 1);
    rp_track_write(target, s, items + 1);
    rp_changed(p);
    *at_track = k;
    *at = re_project_audio_at(p, k, frame);
    return 0;
}

int re_project_audio_span(re_project *p, int k, int i, int start, int length)
{
    re_audio o;
    if (re_project_audio(p, k, i, &o))
        return -1;
    return rp_track_span(p, track(p, k), i, start, length, 1, o.source_length);
}

int re_project_audio_remove(re_project *p, int k, int i)
{
    mlt_playlist pl = track(p, k);
    int e = rp_track_entry(pl, i);
    if (e < 0)
        return 0;
    rp_checkpoint(p);
    mlt_producer gone = mlt_playlist_replace_with_blank(pl, e);
    if (gone)
        mlt_producer_close(gone);
    mlt_playlist_consolidate_blanks(pl, 1);
    int n = mlt_playlist_count(pl);
    if (n > 0 && mlt_playlist_is_blank(pl, n - 1))
        mlt_playlist_remove(pl, n - 1);
    /* an empty track at the end goes (the tractor's last: no other
       track's place changes) */
    int index[MAX_TRACKS], nt = tracks(p, NULL, index);
    mlt_multitrack mt = mlt_tractor_multitrack(p->tractor);
    if (k == nt - 1 && rp_track_items(pl) == 0 && index[k] == mlt_multitrack_count(mt) - 1)
        mlt_tractor_remove_track(p->tractor, index[k]);
    rp_changed(p);
    return 1;
}

int re_project_audio_mute(re_project *p, int k, int i, int on)
{
    mlt_producer cut = cut_of(p, k, i);
    if (!cut || rp_cut_muted(cut) == !!on)
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    rp_cut_mute(p, cut, on);
    rp_changed(p);
    return 1;
}

int re_project_audio_volume(re_project *p, int k, int i, int db)
{
    mlt_producer cut = cut_of(p, k, i);
    db = db < -60 ? -60 : db > 12 ? 12 : db;
    if (!cut || rp_cut_volume(cut) == db)
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    rp_cut_set_volume(p, cut, db);
    rp_changed(p);
    return 1;
}

int re_project_audio_fade(re_project *p, int k, int i, int end, int length)
{
    mlt_playlist_clip_info info;
    mlt_playlist pl = track(p, k);
    int e = rp_track_entry(pl, i);
    if (e < 0 || mlt_playlist_get_clip_info(pl, &info, e))
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

/* ---- the mixes ------------------------------------------------------------------------------ */

#define MARK "shotcut:reeledit_audio"

void rp_fit_audio(re_project *p)
{
    if (!p->tractor)
        return;
    mlt_field field = mlt_tractor_field(p->tractor);
    /* ours taken away (walking the transitions from the tractor's end,
       again after each: disconnecting one changes the chain) */
    int index[MAX_TRACKS], n = tracks(p, NULL, index), mixed[MAX_TRACKS] = {0};
    for (;;) {
        mlt_service found = NULL;
        for (mlt_service s = mlt_service_producer(MLT_TRACTOR_SERVICE(p->tractor)); s && !found;
             s = mlt_service_producer(s)) {
            if (mlt_service_identify(s) != mlt_service_transition_type)
                break;
            if (mlt_properties_get_int(MLT_SERVICE_PROPERTIES(s), MARK))
                found = s;
        }
        if (!found)
            break;
        mlt_field_disconnect_service(field, found);
    }
    /* the project's own mixes into a track */
    for (mlt_service s = mlt_service_producer(MLT_TRACTOR_SERVICE(p->tractor));
         s && mlt_service_identify(s) == mlt_service_transition_type; s = mlt_service_producer(s)) {
        mlt_properties tp = MLT_SERVICE_PROPERTIES(s);
        const char *svc = mlt_properties_get(tp, "mlt_service");
        for (int k = 0; k < n; k++)
            if (svc && !strcmp(svc, "mix") && mlt_properties_get_int(tp, "b_track") == index[k])
                mixed[k] = 1;
    }
    for (int k = 0; k < n; k++) {
        if (mixed[k])
            continue;
        mlt_transition t = mlt_factory_transition(p->profile, "mix", NULL);
        if (!t)
            return;
        mlt_properties tp = MLT_TRANSITION_PROPERTIES(t);
        mlt_properties_set_int(tp, "always_active", 1);
        mlt_properties_set_int(tp, "sum", 1);
        mlt_properties_set_int(tp, MARK, 1);
        mlt_field_plant_transition(field, t, p->v1_track, index[k]);
        mlt_transition_close(t);
    }
}
