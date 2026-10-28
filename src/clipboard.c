/* clipboard.c - copy and paste (project.h): a clip on V1, an overlay on V2
 * (a picture, a video, a title) or a sound on an audio track, copied with
 * what's been done to it, and put at a frame of the same project.
 *
 * A copy holds the producer the original is a cut of (a reference of its
 * own: the original can be deleted), its frames, and its settings as
 * project.h gives them. Pasting makes a new cut of it there and gives it
 * those settings with project.h's calls, all as one edit (one undo). A
 * title is made afresh from its settings: two titles mustn't share one
 * producer, or changing one would change both.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project_internal.h"

#include <stdlib.h>
#include <string.h>

struct re_copy
{
    int kind;
    mlt_producer parent;  /* (NULL: a title) */
    int in, length;
    re_item item;
    re_overlay overlay;
    re_title title;
    re_audio sound;
};

int re_copy_kind(const re_copy *c)
{
    return c ? c->kind : -1;
}

int re_copy_length(const re_copy *c)
{
    return c ? c->length : 0;
}

void re_copy_free(re_copy *c)
{
    if (!c)
        return;
    mlt_producer_close(c->parent);
    free(c);
}

/* The producer a track's entry is a cut of, a reference of ours */
static mlt_producer parent_of(mlt_playlist pl, int entry)
{
    mlt_playlist_clip_info info;
    if (!pl || mlt_playlist_get_clip_info(pl, &info, entry) || !info.producer)
        return NULL;
    mlt_properties_inc_ref(MLT_PRODUCER_PROPERTIES(info.producer));
    return info.producer;
}

re_copy *re_project_copy(re_project *p, int kind, int track, int i)
{
    re_copy *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->kind = kind;
    switch (kind) {
    case RE_COPY_CLIP:
        if (re_project_item(p, i, &c->item) || c->item.gap || c->item.mix)
            break;
        c->in = c->item.in;
        c->length = c->item.length;
        c->parent = parent_of(p->v1, i);
        if (c->parent)
            return c;
        break;
    case RE_COPY_OVERLAY:
        if (re_project_overlay(p, i, &c->overlay))
            break;
        c->in = c->overlay.in;
        c->length = c->overlay.length;
        if (c->overlay.title)
            return re_project_title(p, i, &c->title) ? (free(c), NULL) : c;
        mlt_playlist opl;
        int e = rp_overlay_entry(p, i, &opl);
        c->parent = parent_of(opl, e);
        if (c->parent)
            return c;
        break;
    case RE_COPY_SOUND:
        if (re_project_audio(p, track, i, &c->sound))
            break;
        c->in = c->sound.in;
        c->length = c->sound.length;
        mlt_playlist pl = rp_audio_track(p, track);
        c->parent = pl ? parent_of(pl, rp_track_entry(pl, i)) : NULL;
        if (c->parent)
            return c;
        break;
    }
    re_copy_free(c);
    return NULL;
}

/* A picture's and its sound's fades given to a pasted thing, by the calls
   for its kind: both (fade), then the sound's own if it differs */
static void fades(re_project *p, int i, int track, int kind, int pic_in, int pic_out, int snd_in, int snd_out)
{
    for (int end = 0; end < 2; end++) {
        int pic = end ? pic_out : pic_in, snd = end ? snd_out : snd_in;
        if (kind == RE_COPY_CLIP) {
            if (pic)
                re_project_fade(p, i, end, pic);
            if (snd != pic)
                re_project_sound_fade(p, i, end, snd);
        } else if (kind == RE_COPY_OVERLAY) {
            if (pic)
                re_project_overlay_fade(p, i, end, pic);
            if (snd != pic)
                re_project_overlay_sound_fade(p, i, end, snd);
        } else if (snd) {
            re_project_audio_fade(p, track, i, end, snd);
        }
    }
}

int re_project_paste(re_project *p, const re_copy *c, int frame, int *track)
{
    int i = -1, k = 0;
    if (!c || !p->tractor || (c->kind != RE_COPY_CLIP && re_project_length(p) <= 0))
        return -1;
    if (c->parent)
        mlt_properties_inc_ref(MLT_PRODUCER_PROPERTIES(c->parent)); /* (each put takes one over) */
    re_project_begin(p); /* (one undo: the put and its settings) */
    switch (c->kind) {
    case RE_COPY_CLIP: {
        const re_item *it = &c->item;
        i = rp_insert_cut(p, mlt_producer_cut(c->parent, c->in, c->in + c->length - 1), frame);
        mlt_producer_close(c->parent); /* (the cut has its own reference) */
        if (i < 0)
            break;
        fades(p, i, 0, c->kind, it->fade_in, it->fade_out, it->sound_fade_in, it->sound_fade_out);
        if (it->muted)
            re_project_mute(p, i, 1);
        if (it->volume)
            re_project_volume(p, i, it->volume);
        break;
    }
    case RE_COPY_OVERLAY: {
        const re_overlay *o = &c->overlay;
        if (o->title)
            i = re_project_title_add(p, &c->title, frame, c->length);
        else
            i = rp_overlay_put(p, c->parent, c->in, frame, c->length, o->place, o->size);
        if (i < 0)
            break;
        re_project_overlay_set(p, i, o->place, o->title ? -1 : o->size, o->opacity);
        fades(p, i, 0, c->kind, o->fade_in, o->fade_out, o->sound_fade_in, o->sound_fade_out);
        if (o->muted)
            re_project_overlay_mute(p, i, 1);
        if (o->volume)
            re_project_overlay_volume(p, i, o->volume);
        break;
    }
    case RE_COPY_SOUND: {
        const re_audio *a = &c->sound;
        if (rp_audio_put(p, c->parent, c->in, frame, c->length, &k, &i) < 0) {
            i = -1;
            break;
        }
        fades(p, i, k, c->kind, 0, 0, a->fade_in, a->fade_out);
        if (a->muted)
            re_project_audio_mute(p, k, i, 1);
        if (a->volume)
            re_project_audio_volume(p, k, i, a->volume);
        break;
    }
    default:
        if (c->parent)
            mlt_producer_close(c->parent);
    }
    re_project_end(p);
    if (track)
        *track = k;
    return i;
}
