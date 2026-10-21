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
    unsigned state, next_state, saved_state; /* each edit a new state; modified: not the saved one */
    unsigned changes;
    /* undo and redo: the project as MLT XML before each edit */
    struct snap
    {
        char *xml;         /* "" for an empty project */
        unsigned state;
    } *undo, *redo;
    int n_undo, n_redo;
};

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

static void changed(re_project *p)
{
    fit_tractor(p);
    p->state = ++p->next_state;
    p->changes++;
}

/* ---- undo ---------------------------------------------------------------- */

/* The project as MLT XML in memory (the xml consumer keeps a document in
   a property when its name has no '.'); paths relative to the current
   directory, which is what the xml producer resolves them against too */
static char *snapshot(re_project *p)
{
    if (!p->tractor)
        return strdup("");
    mlt_consumer c = mlt_factory_consumer(p->profile, "xml", "string");
    if (!c)
        return NULL;
    mlt_properties_set_int(MLT_CONSUMER_PROPERTIES(c), "no_meta", 1);
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
static void checkpoint(re_project *p)
{
    push(&p->undo, &p->n_undo, snapshot(p), p->state);
    drop_snaps(p->redo, &p->n_redo);
}

/* An edit that didn't happen: its checkpoint dropped */
static void no_edit(re_project *p)
{
    if (p->n_undo > 0)
        free(p->undo[--p->n_undo].xml);
}

static int adopt(re_project *p, mlt_producer prod);

/* The project made again from a snapshot. 0, or -1 (left as it was). */
static int restore(re_project *p, const char *xml)
{
    mlt_tractor old = p->tractor;
    mlt_playlist old_v1 = p->v1;
    int old_own = p->own_v1;
    if (!*xml) {
        p->tractor = NULL;
        p->v1 = NULL;
        p->own_v1 = 0;
    } else {
        setenv("MLT_XML_DEEP", "1", 1);
        mlt_producer prod = mlt_factory_producer(p->profile, "xml-string", (char *) xml);
        unsetenv("MLT_XML_DEEP");
        p->tractor = NULL;
        p->v1 = NULL;
        p->own_v1 = 0;
        if (!prod || adopt(p, prod) < 0) {
            p->tractor = old;
            p->v1 = old_v1;
            p->own_v1 = old_own;
            return -1;
        }
        fit_tractor(p);
    }
    if (old)
        mlt_tractor_close(old);
    if (old_own)
        mlt_playlist_close(old_v1);
    p->changes++;
    return 0;
}

int re_project_undo(re_project *p)
{
    if (p->n_undo == 0)
        return 0;
    char *now = snapshot(p);
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
    char *now = snapshot(p);
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
    /* V1: the first playlist that isn't Shotcut's black background */
    mlt_multitrack mt = mlt_tractor_multitrack(p->tractor);
    p->v1 = NULL;
    p->own_v1 = 0;
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
        mlt_tractor_close(p->tractor);
        p->tractor = NULL;
        return -1;
    }
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
    mlt_profile_close(p->profile);
    drop_snaps(p->undo, &p->n_undo);
    drop_snaps(p->redo, &p->n_redo);
    free(p->undo);
    free(p->redo);
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
    checkpoint(p);
    if (make_tracks(p) < 0) {
        no_edit(p);
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
    checkpoint(p);
    if (mlt_playlist_split(p->v1, i, frame - it.start - 1)) {
        no_edit(p);
        return 0;
    }
    changed(p);
    return 1;
}

int re_project_remove(re_project *p, int item, int ripple)
{
    if (!p->v1 || item < 0 || item >= mlt_playlist_count(p->v1))
        return 0;
    checkpoint(p);
    if (ripple || mlt_playlist_is_blank(p->v1, item)) {
        if (mlt_playlist_remove(p->v1, item)) {
            no_edit(p);
            return 0;
        }
    } else {
        mlt_producer gone = mlt_playlist_replace_with_blank(p->v1, item);
        if (!gone) {
            no_edit(p);
            return 0;
        }
        mlt_producer_close(gone);
    }
    /* (a gap left at the end does nothing: gone) */
    int n = mlt_playlist_count(p->v1);
    if (n > 0 && mlt_playlist_is_blank(p->v1, n - 1))
        mlt_playlist_remove(p->v1, n - 1);
    changed(p);
    return 1;
}

int re_project_trim(re_project *p, int frame, int end)
{
    int i = re_project_item_at(p, frame);
    re_item it;
    if (i < 0 || re_project_item(p, i, &it) || it.gap || frame <= it.start || frame >= it.start + it.length)
        return 0;
    int in = it.in, out = it.out, k = frame - it.start;
    if (end)
        out = it.in + k - 1; /* the end: the frame before the playhead the last */
    else
        in = it.in + k;      /* the start: the playhead's frame the first */
    checkpoint(p);
    if (mlt_playlist_resize_clip(p->v1, i, in, out)) {
        no_edit(p);
        return 0;
    }
    changed(p);
    return 1;
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
    return n;
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
    p->saved_state = p->state;
    return 0;
}
