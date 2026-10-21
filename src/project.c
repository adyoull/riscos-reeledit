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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct re_project
{
    mlt_profile profile;
    mlt_tractor tractor;   /* NULL while empty */
    mlt_playlist v1;
    int own_v1;            /* V1 made here (a reference of ours, besides the tractor's) */
    int modified;
    unsigned changes;
};

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

static void changed(re_project *p)
{
    fit_tractor(p);
    p->modified = 1;
    p->changes++;
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
static int make_tracks(re_project *p)
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
    mlt_properties_set(MLT_PLAYLIST_PROPERTIES(p->v1), "id", "playlist0");
    mlt_properties_set(MLT_PLAYLIST_PROPERTIES(p->v1), "shotcut:name", "V1");
    mlt_tractor_set_track(p->tractor, MLT_PLAYLIST_PRODUCER(p->v1), 0);
    return 0;
}

/* A clip, opened at the project's profile; the first sets the profile */
static mlt_producer open_clip(re_project *p, const char *file)
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
        /* (the xml producer gives a tractor, or a playlist, or whatever the
           project's top level is) */
        p->profile->is_explicit = 1;
        mlt_producer top = prod;
        if (type == mlt_service_tractor_type) {
            p->tractor = (mlt_tractor) top;
        } else if (type == mlt_service_playlist_type) {
            p->tractor = mlt_tractor_new();
            mlt_service_set_profile(MLT_TRACTOR_SERVICE(p->tractor), p->profile);
            mlt_tractor_set_track(p->tractor, top, 0);
            mlt_producer_close(top);
        } else {
            /* an xml producer whose top is a clip: a project of that clip */
            mlt_producer_close(top);
            re_project_close(p);
            return NULL;
        }
        /* V1: the first playlist that isn't Shotcut's black background */
        mlt_multitrack mt = mlt_tractor_multitrack(p->tractor);
        for (int i = 0; i < mlt_multitrack_count(mt); i++) {
            mlt_producer t = mlt_multitrack_track(mt, i);
            const char *id = mlt_properties_get(MLT_PRODUCER_PROPERTIES(t), "id");
            if (mlt_service_identify(MLT_PRODUCER_SERVICE(t)) == mlt_service_playlist_type
                && !(id && !strcmp(id, "background"))) {
                p->v1 = (mlt_playlist) t;
                break;
            }
        }
        if (!p->v1) {
            re_project_close(p);
            return NULL;
        }
        return p;
    }
    /* a clip: a project of it, at its profile */
    mlt_producer_close(prod);
    if (re_project_add(p, file, -1) < 0) {
        re_project_close(p);
        return NULL;
    }
    p->modified = 0;
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
    mlt_profile_close(p->profile);
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
    return p->modified;
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
    if (!item->gap && info.resource) {
        const char *leaf = info.resource;
        for (const char *c = info.resource; *c; c++)
            if (*c == '/' || *c == '\\')
                leaf = c + 1;
        snprintf(item->name, sizeof(item->name), "%s", leaf);
    }
    return 0;
}

int re_project_item_at(re_project *p, int frame)
{
    if (!p->v1 || frame < 0 || frame >= re_project_length(p))
        return -1;
    return mlt_playlist_get_clip_index_at(p->v1, frame);
}

int re_project_add(re_project *p, const char *file, int frame)
{
    mlt_producer clip = open_clip(p, file);
    if (!clip)
        return -1;
    if (make_tracks(p) < 0) {
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
    changed(p);
    return at;
}

int re_project_split(re_project *p, int frame)
{
    int i = re_project_item_at(p, frame);
    re_item it;
    if (i < 0 || re_project_item(p, i, &it) || frame <= it.start || frame >= it.start + it.length)
        return 0;
    /* (the first part: its in to the frame before) */
    if (mlt_playlist_split(p->v1, i, frame - it.start - 1))
        return 0;
    changed(p);
    return 1;
}

int re_project_remove(re_project *p, int item, int ripple)
{
    if (!p->v1 || item < 0 || item >= mlt_playlist_count(p->v1))
        return 0;
    if (ripple || mlt_playlist_is_blank(p->v1, item)) {
        if (mlt_playlist_remove(p->v1, item))
            return 0;
    } else {
        mlt_producer gone = mlt_playlist_replace_with_blank(p->v1, item);
        if (!gone)
            return 0;
        mlt_producer_close(gone);
    }
    /* (a gap left at the end does nothing: gone) */
    int n = mlt_playlist_count(p->v1);
    if (n > 0 && mlt_playlist_is_blank(p->v1, n - 1))
        mlt_playlist_remove(p->v1, n - 1);
    changed(p);
    return 1;
}

int re_project_save(re_project *p, const char *file)
{
    if (!p->tractor)
        return -1;
    /* written as a new file, then put in place: a save that fails leaves
       the old file, and says so */
    char tmp[1024], root[1024];
    /* (the xml consumer writes a name with no '.' in it to a property, not a file) */
    snprintf(tmp, sizeof(tmp), "%s.new", file);
    remove(tmp);
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
    mlt_consumer_connect(c, MLT_PRODUCER_SERVICE(re_project_producer(p)));
    mlt_consumer_start(c); /* (the xml consumer writes the file here) */
    mlt_consumer_stop(c);
    mlt_consumer_close(c);
    FILE *f = fopen(tmp, "r");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
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
    p->modified = 0;
    return 0;
}
