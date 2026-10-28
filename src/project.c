/* project.c - a Reel Edit project (project.h).
 *
 * The model is MLT's own, as Shotcut builds it: a tractor whose tracks are
 * playlists. V1 is the first playlist (Shotcut's "background" track, black,
 * is skipped). Edits are MLT playlist calls (insert, split_at, remove,
 * replace_with_blank), so what's saved is what MLT plays.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project.h"
#include "engine.h"
#include "project_internal.h"
#include "proxy.h"
#include "still.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNDO_MAX 100

/* The tractor's length follows V1's (MLT doesn't do it for a tractor
   made without a profile; the saved project's tractor needs its out) */
static void fit_tractor(re_project *p)
{
    if (!p->tractor)
        return;
    int len = re_project_length(p);
    mlt_properties_set_position(MLT_TRACTOR_PROPERTIES(p->tractor), "length", len > 0 ? len : 1);
    mlt_producer_set_in_and_out(MLT_TRACTOR_PRODUCER(p->tractor), 0, len > 0 ? len - 1 : 0);
}

void rp_changed(re_project *p)
{
    rp_relink(p);
    rp_fit_fades(p);
    rp_fit_sound(p);
    rp_fit_overlays(p);
    rp_fit_audio(p);
    fit_tractor(p);
    p->state = ++p->next_state;
    p->changes++;
}

/* ---- undo ---------------------------------------------------------------- */

/* The project as MLT XML in memory (the xml consumer keeps a document in
   a property when its name has no '.'); paths relative to the current
   directory, which is what the xml producer resolves them against too */
char *rp_snapshot(re_project *p)
{
    if (!p->tractor)
        return strdup("");
    mlt_consumer c = mlt_factory_consumer(p->profile, "xml", "string");
    if (!c)
        return NULL;
    mlt_properties_set_int(MLT_CONSUMER_PROPERTIES(c), "no_meta", 1);
    mlt_properties_set(MLT_CONSUMER_PROPERTIES(c), "store", "shotcut"); /* (a tractor's own properties: only these are kept) */
    mlt_consumer_connect(c, MLT_PRODUCER_SERVICE(MLT_TRACTOR_PRODUCER(p->tractor)));
    mlt_consumer_start(c);
    mlt_consumer_stop(c);
    const char *xml = mlt_properties_get(MLT_CONSUMER_PROPERTIES(c), "string");
    char *copy = xml ? strdup(xml) : NULL;
    mlt_consumer_close(c);
    return copy;
}

static void drop_snaps(struct snap *v, int *n)
{
    for (int i = 0; i < *n; i++)
        free(v[i].xml);
    *n = 0;
}

static int push(struct snap **v, int *n, char *xml, unsigned state)
{
    if (!xml)
        return -1;
    if (!*v && !(*v = calloc(UNDO_MAX, sizeof(**v)))) {
        free(xml);
        return -1;
    }
    if (*n == UNDO_MAX) { /* the oldest goes */
        free((*v)[0].xml);
        memmove(*v, *v + 1, (UNDO_MAX - 1) * sizeof(**v));
        (*n)--;
    }
    (*v)[*n].xml = xml;
    (*v)[*n].state = state;
    (*n)++;
    return 0;
}

/* Before an edit: the project as it is, to go back to (and nothing to redo) */
void rp_checkpoint(re_project *p)
{
    if (p->in_edit) /* (within re_project_begin/end: its undo point is the edit's) */
        return;
    push(&p->undo, &p->n_undo, rp_snapshot(p), p->state);
    drop_snaps(p->redo, &p->n_redo);
}

/* An edit that didn't happen: its checkpoint dropped */
void rp_no_edit(re_project *p)
{
    if (p->in_edit) /* (begin's point stays: end drops it if nothing changed) */
        return;
    if (p->n_undo > 0)
        free(p->undo[--p->n_undo].xml);
}

static int adopt(re_project *p, mlt_producer prod);

/* A snapshot made again (undo, redo), with the proxies as they are now */
static int restore(re_project *p, const char *xml)
{
    char *x = rp_proxy_xml(p, strdup(xml), p->px_on);
    int r = x ? rp_restore(p, x) : -1;
    free(x);
    return r;
}

/* The project made again from a snapshot. 0, or -1 (left as it was). */
int rp_restore(re_project *p, const char *xml)
{
    re_project old = *p;
    if (!*xml) {
        p->tractor = NULL;
        p->v1 = p->v2 = NULL;
        p->own_v1 = p->own_v2 = 0;
    } else {
        setenv("MLT_XML_DEEP", "1", 1);
        mlt_producer prod = mlt_factory_producer(p->profile, "xml-string", (char *) xml);
        unsetenv("MLT_XML_DEEP");
        p->tractor = NULL;
        p->v1 = p->v2 = NULL;
        p->own_v1 = p->own_v2 = 0;
        if (!prod || adopt(p, prod) < 0) {
            *p = old;
            return -1;
        }
        fit_tractor(p);
    }
    if (old.tractor)
        mlt_tractor_close(old.tractor);
    if (old.own_v1)
        mlt_playlist_close(old.v1);
    if (old.own_v2)
        mlt_playlist_close(old.v2);
    p->changes++;
    return 0;
}

int re_project_undo(re_project *p)
{
    if (p->n_undo == 0)
        return 0;
    char *now = rp_snapshot(p);
    struct snap s = p->undo[p->n_undo - 1];
    if (!now || restore(p, s.xml) < 0) {
        free(now);
        return 0;
    }
    push(&p->redo, &p->n_redo, now, p->state);
    p->n_undo--;
    p->state = s.state;
    free(s.xml);
    return 1;
}

int re_project_redo(re_project *p)
{
    if (p->n_redo == 0)
        return 0;
    char *now = rp_snapshot(p);
    struct snap s = p->redo[p->n_redo - 1];
    if (!now || restore(p, s.xml) < 0) {
        free(now);
        return 0;
    }
    push(&p->undo, &p->n_undo, now, p->state);
    p->n_redo--;
    p->state = s.state;
    free(s.xml);
    return 1;
}

int re_project_can_undo(re_project *p)
{
    return p->n_undo > 0;
}

int re_project_can_redo(re_project *p)
{
    return p->n_redo > 0;
}

re_project *re_project_new(void)
{
    if (re_mlt_start() < 0)
        return NULL;
    re_project *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->profile = mlt_profile_init(NULL);
    return p;
}

/* The tractor and V1, made when the first clip is added (the profile
   known) */
int rp_make_tracks(re_project *p)
{
    if (p->tractor)
        return 0;
    p->tractor = mlt_tractor_new();
    p->v1 = mlt_playlist_new(p->profile);
    if (!p->tractor || !p->v1) {
        if (p->tractor)
            mlt_tractor_close(p->tractor);
        if (p->v1)
            mlt_playlist_close(p->v1);
        p->tractor = NULL;
        p->v1 = NULL;
        return -1;
    }
    /* (its positions are written in the saved file by the profile's rate) */
    mlt_service_set_profile(MLT_TRACTOR_SERVICE(p->tractor), p->profile);
    mlt_service_set_profile(MLT_MULTITRACK_SERVICE(mlt_tractor_multitrack(p->tractor)), p->profile);
    p->own_v1 = 1;
    p->v1_track = 0;
    mlt_properties_set(MLT_PLAYLIST_PROPERTIES(p->v1), "id", "playlist0");
    mlt_properties_set(MLT_PLAYLIST_PROPERTIES(p->v1), "shotcut:name", "V1");
    mlt_tractor_set_track(p->tractor, MLT_PLAYLIST_PRODUCER(p->v1), 0);
    return 0;
}

/* A clip, opened at the project's profile; the first sets the profile */
mlt_producer rp_open_clip(re_project *p, const char *file)
{
    mlt_producer clip = mlt_factory_producer(p->profile, NULL, file);
    if (clip && !p->tractor && !p->profile->is_explicit) {
        mlt_profile_from_producer(p->profile, clip);
        p->profile->is_explicit = 1;
        mlt_producer_close(clip);
        clip = mlt_factory_producer(p->profile, NULL, file);
    }
    return clip;
}

/* A project's top level from the xml producer: a tractor (or a playlist,
   given a tractor) and its V1. 0, or -1 (prod closed). */
static int adopt(re_project *p, mlt_producer prod)
{
    mlt_service_type type = mlt_service_identify(MLT_PRODUCER_SERVICE(prod));
    if (type == mlt_service_tractor_type) {
        p->tractor = (mlt_tractor) prod;
    } else if (type == mlt_service_playlist_type) {
        p->tractor = mlt_tractor_new();
        mlt_service_set_profile(MLT_TRACTOR_SERVICE(p->tractor), p->profile);
        mlt_tractor_set_track(p->tractor, prod, 0);
        mlt_producer_close(prod);
    } else {
        mlt_producer_close(prod); /* (a clip at the top: not a project) */
        return -1;
    }
    mlt_service_set_profile(MLT_TRACTOR_SERVICE(p->tractor), p->profile);
    /* V1: the first playlist that isn't Shotcut's black background (nor
       one of its audio tracks); V2 the next */
    mlt_multitrack mt = mlt_tractor_multitrack(p->tractor);
    p->v1 = p->v2 = NULL;
    p->own_v1 = p->own_v2 = 0;
    for (int i = 0; i < mlt_multitrack_count(mt) && !p->v2; i++) {
        mlt_producer t = mlt_multitrack_track(mt, i);
        mlt_properties tp = MLT_PRODUCER_PROPERTIES(t);
        const char *id = mlt_properties_get(tp, "id");
        if (mlt_service_identify(MLT_PRODUCER_SERVICE(t)) != mlt_service_playlist_type
            || (id && !strcmp(id, "background")) || mlt_properties_get_int(tp, "shotcut:audio"))
            continue;
        if (!p->v1) {
            p->v1 = (mlt_playlist) t;
            p->v1_track = i;
        } else {
            p->v2 = (mlt_playlist) t;
            p->v2_track = i;
        }
    }
    if (!p->v1) {
        mlt_tractor_close(p->tractor);
        p->tractor = NULL;
        return -1;
    }
    rp_relink(p);
    rp_fit_sound(p);
    rp_fit_overlays(p);
    rp_fit_audio(p);
    return 0;
}

re_project *re_project_open(const char *file, int is_xml)
{
    re_project *p = re_project_new();
    if (!p)
        return NULL;
    /* MLT_XML_DEEP: the xml producer gives the project's own tractor, to be
       edited and saved whole (else a tractor that saves as a reference to
       the file it came from) */
    setenv("MLT_XML_DEEP", "1", 1);
    mlt_producer prod = mlt_factory_producer(p->profile, is_xml ? "xml" : NULL, file);
    unsetenv("MLT_XML_DEEP");
    if (!prod) {
        re_project_close(p);
        return NULL;
    }
    mlt_service_type type = mlt_service_identify(MLT_PRODUCER_SERVICE(prod));
    const char *service = mlt_properties_get(MLT_PRODUCER_PROPERTIES(prod), "mlt_service");
    int from_xml = service && !strcmp(service, "xml");
    if (type == mlt_service_tractor_type || type == mlt_service_playlist_type || from_xml) {
        p->profile->is_explicit = 1;
        if (adopt(p, prod) < 0) {
            re_project_close(p);
            return NULL;
        }
        fit_tractor(p);
        rp_bin_read(p);
        return p;
    }
    /* a clip: a project of it, at its profile */
    mlt_producer_close(prod);
    if (re_project_add(p, file, -1) < 0) {
        re_project_close(p);
        return NULL;
    }
    p->saved_state = p->state;
    drop_snaps(p->undo, &p->n_undo); /* (opening isn't an edit to undo) */
    return p;
}

void re_project_close(re_project *p)
{
    if (!p)
        return;
    if (p->tractor)
        mlt_tractor_close(p->tractor);
    if (p->own_v1)
        mlt_playlist_close(p->v1);
    if (p->own_v2)
        mlt_playlist_close(p->v2);
    mlt_profile_close(p->profile);
    drop_snaps(p->undo, &p->n_undo);
    drop_snaps(p->redo, &p->n_redo);
    free(p->undo);
    free(p->redo);
    rp_bin_free(p);
    rp_proxies_free(p);
    free(p);
}

mlt_producer re_project_producer(re_project *p)
{
    return p->tractor ? MLT_TRACTOR_PRODUCER(p->tractor) : NULL;
}

mlt_profile re_project_profile(re_project *p)
{
    return p->profile;
}

int re_project_length(re_project *p)
{
    return p->v1 ? mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(p->v1)) : 0;
}

double re_project_fps(re_project *p)
{
    return mlt_profile_fps(p->profile);
}

int re_project_modified(re_project *p)
{
    return p->state != p->saved_state;
}

void re_project_set_modified(re_project *p)
{
    p->saved_state = p->state - 1;
}

unsigned re_project_changes(re_project *p)
{
    return p->changes;
}

int re_project_items(re_project *p)
{
    return p->v1 ? mlt_playlist_count(p->v1) : 0;
}

int re_project_item(re_project *p, int i, re_item *item)
{
    mlt_playlist_clip_info info;
    memset(item, 0, sizeof(*item));
    if (!p->v1 || mlt_playlist_get_clip_info(p->v1, &info, i))
        return -1;
    item->start = info.start;
    item->length = info.frame_count;
    item->in = info.frame_in;
    item->out = info.frame_out;
    item->gap = mlt_playlist_is_blank(p->v1, i);
    item->mix = rp_mix_of(p, i) != NULL;
    item->source_length = info.length;
    item->speed = 100;
    if (item->mix) {
        item->in = 0;
        item->out = item->length - 1;
        snprintf(item->name, sizeof(item->name), "Dissolve");
    } else if (!item->gap) {
        mlt_producer cut = mlt_playlist_get_clip(p->v1, i);
        item->fade_in = rp_fade_of(p, i, RP_PICTURE, 0);
        item->fade_out = rp_fade_of(p, i, RP_PICTURE, 1);
        item->sound_fade_in = rp_fade_of(p, i, RP_SOUND, 0);
        item->sound_fade_out = rp_fade_of(p, i, RP_SOUND, 1);
        item->sound = rp_cut_has_sound(cut);
        item->muted = rp_cut_muted(cut);
        item->volume = rp_cut_volume(cut);
        item->speed = rp_speed_percent(info.producer);
    }
    const char *file =
        item->gap || item->mix ? NULL : re_project_original(p, rp_speed_file(info.producer, info.resource));
    if (file) {
        const char *leaf = file;
        for (const char *c = file; *c; c++)
            if (*c == '/' || *c == '\\')
                leaf = c + 1;
        snprintf(item->name, sizeof(item->name), "%s", leaf);
    }
    return 0;
}

const char *re_project_item_file(re_project *p, int i)
{
    mlt_playlist_clip_info info;
    if (!p->v1 || mlt_playlist_get_clip_info(p->v1, &info, i) || mlt_playlist_is_blank(p->v1, i) || rp_mix_of(p, i))
        return NULL;
    return rp_speed_file(info.producer, info.resource); /* (MLT keeps a picture's without "qimage:") */
}

int re_project_item_at(re_project *p, int frame)
{
    if (!p->v1 || frame < 0 || frame >= re_project_length(p))
        return -1;
    return mlt_playlist_get_clip_index_at(p->v1, frame);
}

int re_project_add(re_project *p, const char *file, int frame)
{
    return re_project_add_length(p, file, frame, 0);
}

int re_project_frames(re_project *p, double seconds)
{
    return (int) lrint(seconds * mlt_profile_fps(p->profile));
}

int re_project_add_length(re_project *p, const char *file, int frame, int length)
{
    mlt_producer clip = rp_open_clip(p, file);
    if (!clip)
        return -1;
    mlt_properties cp = MLT_PRODUCER_PROPERTIES(clip);
    if (length > 0) {
        /* (saved with the clip: opened again, MLT takes these) */
        mlt_properties_set_position(cp, "length", length);
        mlt_properties_set_position(cp, "out", length - 1);
        mlt_properties_set(cp, "eof", "pause");
        mlt_producer_set_in_and_out(clip, 0, length - 1);
    } else if (re_still_is(clip)) { /* (a picture: 5 seconds of it) */
        length = re_project_frames(p, 5.0);
        mlt_producer_set_in_and_out(clip, 0, length - 1);
    } else if (mlt_producer_get_length(clip) >= INT_MAX / 2) {
        mlt_producer_close(clip); /* (endless to MLT: its length to be found) */
        return RE_ADD_MEASURE;
    }
    rp_checkpoint(p);
    if (rp_make_tracks(p) < 0) {
        rp_no_edit(p);
        mlt_producer_close(clip);
        return -1;
    }
    int n = mlt_playlist_count(p->v1), at = n;
    if (frame >= 0 && frame < re_project_length(p)) {
        re_item it;
        at = re_project_item_at(p, frame);
        re_project_item(p, at, &it);
        if (frame - it.start > it.length / 2)
            at++; /* nearer its end: after it */
    }
    if (at >= n)
        mlt_playlist_append(p->v1, clip);
    else
        mlt_playlist_insert(p->v1, clip, at, -1, -1);
    mlt_producer_close(clip); /* (the playlist keeps its own reference) */
    rp_changed(p);
    return at;
}

/* The item at a frame split there (not at its start, not a dissolve): 1
   split, 0 not (nothing to split there). The fade out goes with the second
   part, where the end is. No undo point of its own. */
int rp_split_at(re_project *p, int frame)
{
    int i = re_project_item_at(p, frame);
    re_item it;
    if (i < 0 || re_project_item(p, i, &it) || it.mix || frame <= it.start || frame >= it.start + it.length)
        return 0;
    int fade_out = rp_fade_of(p, i, RP_PICTURE, 1), sound_out = rp_fade_of(p, i, RP_SOUND, 1);
    int muted = rp_cut_muted(mlt_playlist_get_clip(p->v1, i));
    int volume = rp_cut_volume(mlt_playlist_get_clip(p->v1, i));
    /* (the first part: its in to the frame before; the second, a new cut,
       is given the fades out and the mute) */
    if (mlt_playlist_split(p->v1, i, frame - it.start - 1))
        return 0;
    rp_set_fade(p, i, RP_PICTURE | RP_SOUND, 1, 0);
    rp_set_fade(p, i + 1, RP_PICTURE, 1, fade_out);
    rp_set_fade(p, i + 1, RP_SOUND, 1, sound_out);
    if (muted)
        rp_cut_mute(p, mlt_playlist_get_clip(p->v1, i + 1), 1);
    if (volume)
        rp_cut_set_volume(p, mlt_playlist_get_clip(p->v1, i + 1), volume);
    return 1;
}

int re_project_split(re_project *p, int frame)
{
    rp_checkpoint(p);
    if (!rp_split_at(p, frame)) {
        rp_no_edit(p);
        return 0;
    }
    rp_changed(p);
    return 1;
}

int re_project_remove_range(re_project *p, int in, int out, int ripple)
{
    int len = re_project_length(p);
    if (!p->v1 || in < 0 || out < in || in >= len)
        return 0;
    if (out >= len)
        out = len - 1;
    for (int i = 0; i < re_project_items(p); i++) { /* (not through a dissolve: remove it first) */
        re_item it;
        if (!re_project_item(p, i, &it) && it.mix && it.start <= out && it.start + it.length - 1 >= in)
            return 0;
    }
    rp_checkpoint(p);
    rp_split_at(p, out + 1); /* (the end first: the item at the start keeps its index) */
    rp_split_at(p, in);
    for (int i = re_project_items(p) - 1; i >= 0; i--) {
        re_item it;
        if (re_project_item(p, i, &it) || it.start < in || it.start + it.length - 1 > out)
            continue;
        if (ripple || it.gap) {
            mlt_playlist_remove(p->v1, i);
        } else {
            mlt_producer gone = mlt_playlist_replace_with_blank(p->v1, i);
            if (gone)
                mlt_producer_close(gone);
        }
    }
    if (!ripple) /* (one gap where there were several; none at the end, below) */
        mlt_playlist_consolidate_blanks(p->v1, 1);
    int n = mlt_playlist_count(p->v1);
    if (n > 0 && mlt_playlist_is_blank(p->v1, n - 1))
        mlt_playlist_remove(p->v1, n - 1);
    rp_changed(p);
    return 1;
}

int re_project_remove(re_project *p, int item, int ripple)
{
    if (!p->v1 || item < 0 || item >= mlt_playlist_count(p->v1))
        return 0;
    rp_checkpoint(p);
    if (rp_is_clip(p, item)) { /* (its dissolves first: the clips beside it as they were) */
        if (rp_mix_of(p, item + 1))
            rp_unmix(p, item + 1);
        if (rp_mix_of(p, item - 1)) {
            rp_unmix(p, item - 1);
            item--;
        }
        rp_relink(p);
    }
    if (rp_mix_of(p, item)) {
        if (rp_unmix(p, item)) {
            rp_no_edit(p);
            return 0;
        }
    } else if (ripple || mlt_playlist_is_blank(p->v1, item)) {
        if (mlt_playlist_remove(p->v1, item)) {
            rp_no_edit(p);
            return 0;
        }
    } else {
        mlt_producer gone = mlt_playlist_replace_with_blank(p->v1, item);
        if (!gone) {
            rp_no_edit(p);
            return 0;
        }
        mlt_producer_close(gone);
    }
    /* (a gap left at the end does nothing: gone) */
    int n = mlt_playlist_count(p->v1);
    if (n > 0 && mlt_playlist_is_blank(p->v1, n - 1))
        mlt_playlist_remove(p->v1, n - 1);
    rp_changed(p);
    return 1;
}

int re_project_trim(re_project *p, int frame, int end)
{
    int i = re_project_item_at(p, frame);
    re_item it;
    if (i < 0 || re_project_item(p, i, &it) || it.gap || it.mix || frame <= it.start || frame >= it.start + it.length)
        return 0;
    if (rp_mix_of(p, end ? i + 1 : i - 1)) /* (that end is in a dissolve: its length is changed instead) */
        return 0;
    int in = it.in, out = it.out, k = frame - it.start;
    if (end)
        out = it.in + k - 1; /* the end: the frame before the playhead the last */
    else
        in = it.in + k;      /* the start: the playhead's frame the first */
    rp_checkpoint(p);
    if (mlt_playlist_resize_clip(p->v1, i, in, out)) {
        rp_no_edit(p);
        return 0;
    }
    rp_changed(p);
    return 1;
}

void re_project_begin(re_project *p)
{
    if (p->in_edit)
        return;
    rp_checkpoint(p);
    p->in_edit = 1;
    p->edit_state = p->state;
}

void re_project_end(re_project *p)
{
    if (!p->in_edit)
        return;
    p->in_edit = 0;
    /* (unchanged in the end: no undo point; as modified as before) */
    const char *before = p->n_undo > 0 ? p->undo[p->n_undo - 1].xml : NULL;
    char *now = rp_snapshot(p);
    if (before && now && !strcmp(before, now)) {
        rp_no_edit(p);
        p->state = p->edit_state;
    }
    free(now);
}

int re_project_roll(re_project *p, int item, int frame)
{
    re_item a, b;
    if (re_project_item(p, item, &a) || re_project_item(p, item + 1, &b) || a.mix || b.mix || (a.gap && b.gap))
        return 0;
    int d = frame - b.start;
    /* (each side keeps a frame at least, and a clip its file's frames) */
    int lo = a.gap ? -(a.length - 1) : -(a.out - a.in), hi = a.gap ? d : a.source_length - 1 - a.out;
    if (!b.gap) {
        if (-b.in > lo)
            lo = -b.in;
        if (b.out - b.in < hi)
            hi = b.out - b.in;
    } else if (b.length - 1 < hi) {
        hi = b.length - 1;
    }
    d = d < lo ? lo : d > hi ? hi : d;
    if (d == 0)
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    if (a.gap)
        mlt_playlist_resize_clip(p->v1, item, 0, a.length - 1 + d);
    else
        mlt_playlist_resize_clip(p->v1, item, a.in, a.out + d);
    if (b.gap)
        mlt_playlist_resize_clip(p->v1, item + 1, 0, b.length - 1 - d);
    else
        mlt_playlist_resize_clip(p->v1, item + 1, b.in + d, b.out);
    rp_changed(p);
    return 1;
}

int re_project_set_in_out(re_project *p, int item, int in, int out)
{
    re_item it;
    if (re_project_item(p, item, &it) || it.gap || it.mix)
        return 0;
    /* (an end in a dissolve stays: the dissolve's length is changed instead) */
    if (rp_mix_of(p, item - 1))
        in = it.in;
    if (rp_mix_of(p, item + 1))
        out = it.out;
    if (in < 0)
        in = 0;
    if (it.source_length > 0 && out > it.source_length - 1)
        out = it.source_length - 1;
    if (out < in)
        out = in;
    if (in == it.in && out == it.out)
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    if (mlt_playlist_resize_clip(p->v1, item, in, out)) {
        if (!p->in_edit)
            rp_no_edit(p);
        return 0;
    }
    rp_changed(p);
    return 1;
}

int re_project_move(re_project *p, int item, int before)
{
    int n = re_project_items(p);
    if (item < 0 || item >= n || before < 0 || before > n || before == item || before == item + 1)
        return -1;
    /* (not a dissolve, nor a clip in one, nor into one: remove the dissolve first) */
    if (rp_mix_of(p, item) || rp_mix_of(p, item - 1) || rp_mix_of(p, item + 1) || rp_mix_of(p, before)
        || rp_mix_of(p, before - 1))
        return -1;
    int dest = before > item ? before - 1 : before; /* (its index once it's out of its place) */
    if (!p->in_edit)
        rp_checkpoint(p);
    if (mlt_playlist_move(p->v1, item, dest)) {
        if (!p->in_edit)
            rp_no_edit(p);
        return -1;
    }
    rp_changed(p);
    return dest;
}

int re_project_cut(re_project *p, int frame, int dir)
{
    int best = -1, n = re_project_items(p);
    for (int i = 0; i <= n; i++) {
        int f;
        if (i < n) {
            re_item it;
            re_project_item(p, i, &it);
            f = it.start;
        } else {
            f = re_project_length(p);
        }
        if (dir < 0 && f < frame && (best < 0 || f > best))
            best = f;
        if (dir > 0 && f > frame && (best < 0 || f < best))
            best = f;
    }
    return best;
}

#define ADD(...)                                                                                                   \
    do {                                                                                                           \
        if (n < size)                                                                                              \
            n += snprintf(buf + n, size - n, __VA_ARGS__);                                                         \
    } while (0)

static void timecode(char *tc, int tsize, int frames, double fps)
{
    int rate = (int) (fps + 0.5), f = frames < 0 ? 0 : frames;
    if (rate < 1)
        rate = 1;
    int s = f / rate;
    snprintf(tc, tsize, "%02d:%02d:%02d:%02d", s / 3600, s / 60 % 60, s % 60, f % rate);
}

const char *re_project_decoder(re_project *p, int frame)
{
    mlt_playlist_clip_info info;
    int item = re_project_item_at(p, frame);
    if (!p->v1 || item < 0 || mlt_playlist_is_blank(p->v1, item) || mlt_playlist_get_clip_info(p->v1, &info, item)
        || !info.producer)
        return NULL;
    mlt_producer clip = mlt_producer_cut_parent(info.producer);
    return mlt_properties_get(MLT_PRODUCER_PROPERTIES(clip ? clip : info.producer), "meta.media.decoder");
}

int re_project_item_info(re_project *p, int item, char *buf, int size)
{
    mlt_playlist_clip_info info;
    int n = 0;
    if (size > 0)
        buf[0] = 0;
    if (!p->v1 || item < 0 || item >= mlt_playlist_count(p->v1) || mlt_playlist_is_blank(p->v1, item)
        || mlt_playlist_get_clip_info(p->v1, &info, item))
        return -1;
    mlt_producer clip = info.producer ? mlt_producer_cut_parent(info.producer) : NULL;
    if (!clip)
        clip = info.producer;
    mlt_properties pr = MLT_PRODUCER_PROPERTIES(clip);
    double fps = re_project_fps(p);
    char a[24], b[24], c[24], d[24];
    ADD("File\t%s\n", info.resource ? info.resource : "?");
    timecode(a, sizeof(a), info.frame_count, fps);
    timecode(b, sizeof(b), info.frame_in, fps);
    timecode(c, sizeof(c), mlt_producer_get_length(clip), fps);
    timecode(d, sizeof(d), info.start, fps);
    ADD("In the project\t%s long, at %s (from %s in the file)\n", a, d, b);
    ADD("The file's length\t%s\n", c);
    int streams = mlt_properties_get_int(pr, "meta.media.nb_streams");
    int vi = mlt_properties_get_int(pr, "video_index"), ai = mlt_properties_get_int(pr, "audio_index");
    char key[64];
    for (int s = 0; s < streams; s++) {
        snprintf(key, sizeof(key), "meta.media.%d.stream.type", s);
        const char *type = mlt_properties_get(pr, key);
        if (!type)
            continue;
#define META(name) (snprintf(key, sizeof(key), "meta.media.%d." name, s), mlt_properties_get(pr, key))
        const char *codec = META("codec.long_name");
        if (!codec)
            codec = META("codec.name");
        if (!strcmp(type, "video")) {
            const char *w = META("codec.width"), *h = META("codec.height"), *pix = META("codec.pix_fmt");
            const char *fr = META("stream.frame_rate"), *cs = META("codec.colorspace");
            ADD("Video%s\t%s, %sx%s, %s fps%s%s%s%s\n", s == vi ? "" : " (not used)", codec ? codec : "?", w ? w : "?",
                h ? h : "?", fr ? fr : "?", pix ? ", " : "", pix ? pix : "", cs ? ", BT." : "", cs ? cs : "");
        } else if (!strcmp(type, "audio")) {
            const char *rate = META("codec.sample_rate"), *ch = META("codec.channels");
            ADD("Sound%s\t%s, %s Hz, %s channel%s\n", s == ai ? "" : " (not used)", codec ? codec : "?", rate ? rate : "?",
                ch ? ch : "?", ch && !strcmp(ch, "1") ? "" : "s");
        }
#undef META
    }
    const char *dec = mlt_properties_get(pr, "meta.media.decoder");
    if (dec)
        ADD("Decoded by\t%s\n", !strcmp(dec, "h264_vchiq")   ? "the VideoCore (h264_vchiq)"
                                  : !strcmp(dec, "hevc_hwdec") ? "the HEVC block (hevc_hwdec)"
                                                               : dec);
    else
        ADD("Decoded by\t(not decoded yet)\n");
    return n;
}

int re_project_info(re_project *p, char *buf, int size)
{
    int n = 0;
    char a[24];
    if (size > 0)
        buf[0] = 0;
    timecode(a, sizeof(a), re_project_length(p), re_project_fps(p));
    if (!re_project_producer(p)) {
        ADD("Project\tempty\n");
        return n;
    }
    int clips = 0;
    for (int i = 0; i < re_project_items(p); i++)
        if (!mlt_playlist_is_blank(p->v1, i))
            clips++;
    ADD("Project\t%dx%d, %.3g fps, %s\n", p->profile->width, p->profile->height, re_project_fps(p), a);
    ADD("Clips\t%d on V1\n", clips);
    if (re_project_overlays(p) > 0)
        ADD("Overlays\t%d on V2\n", re_project_overlays(p));
    return n;
}

static int save(re_project *p, const char *file, int as_saved);

int re_project_save(re_project *p, const char *file)
{
    return save(p, file, 1);
}

int re_project_save_copy(re_project *p, const char *file)
{
    return save(p, file, 0);
}

static int save(re_project *p, const char *file, int as_saved)
{
    if (!p->tractor)
        return -1;
    /* written as a new file, then put in place: a save that fails leaves
       the old file, and says so */
    char tmp[1024], root[1024];
    /* (the xml consumer writes a name with no '.' in it to a property, not a file) */
    snprintf(tmp, sizeof(tmp), "%s.new", file);
    remove(tmp);
    rp_bin_write(p); /* (the Clips window's list: a property of the tractor's) */
    mlt_consumer c = mlt_factory_consumer(p->profile, "xml", tmp);
    if (!c)
        return -1;
    /* clips named from the project's own directory (else the current one) */
    snprintf(root, sizeof(root), "%s", file);
    char *slash = strrchr(root, '/');
    if (slash && slash > root) {
        *slash = 0;
        mlt_properties_set(MLT_CONSUMER_PROPERTIES(c), "root", root);
    }
    mlt_properties_set_int(MLT_CONSUMER_PROPERTIES(c), "no_meta", 1);
    mlt_properties_set(MLT_CONSUMER_PROPERTIES(c), "store", "shotcut"); /* (a tractor's own properties: only these are kept) */
    mlt_consumer_connect(c, MLT_PRODUCER_SERVICE(re_project_producer(p)));
    mlt_consumer_start(c); /* (the xml consumer writes the file here) */
    mlt_consumer_stop(c);
    mlt_consumer_close(c);
    FILE *f = fopen(tmp, "r");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size > 0 && p->n_px > 0) { /* (the files named, not their proxies) */
        char *x = malloc((size_t) size + 1);
        rewind(f);
        if (!x || fread(x, 1, (size_t) size, f) != (size_t) size) {
            free(x);
            fclose(f);
            remove(tmp);
            return -1;
        }
        x[size] = 0;
        fclose(f);
        x = rp_proxy_xml(p, x, 0);
        f = x ? fopen(tmp, "w") : NULL;
        size = f && fputs(x, f) >= 0 ? (long) strlen(x) : -1;
        free(x);
    }
    if (f)
        fclose(f);
    if (size <= 0) {
        remove(tmp);
        return -1;
    }
    remove(file);
    if (rename(tmp, file)) {
        remove(tmp);
        return -1;
    }
    if (as_saved)
        p->saved_state = p->state;
    return 0;
}
