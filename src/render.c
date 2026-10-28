/* render.c - render previews (render.h): the costly stretches, their
 * signatures, the files made of them, and the playlist that plays them.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "render.h"
#include "project_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PREVIEWS 64

/* ---- the costly stretches ------------------------------------------------------------------ */

typedef struct
{
    re_stretch *s;
    int n, max, len, join;
} spans;

/* Frames a to b - 1 costly: joined to the last stretch when they meet it
   or come within join frames of it (they come in no order: sorted after) */
static void costly(spans *v, int a, int b)
{
    if (a < 0)
        a = 0;
    if (b > v->len)
        b = v->len;
    if (b <= a || v->n >= v->max)
        return;
    v->s[v->n].start = a;
    v->s[v->n].length = b - a;
    v->n++;
}

static int by_start(const void *x, const void *y)
{
    const re_stretch *a = x, *b = y;
    return a->start - b->start;
}

int re_render_stretches(re_project *p, re_stretch *out, int max)
{
    int len = re_project_length(p);
    int cap = 4 * (re_project_items(p) + re_project_overlays(p)) + 8;
    spans v = {calloc((size_t) cap, sizeof(re_stretch)), 0, cap, len, (int) (2 * re_project_fps(p) + 0.5)};
    if (!v.s || len <= 0) {
        free(v.s);
        return 0;
    }
    for (int i = 0; i < re_project_items(p); i++) {
        re_item it;
        if (re_project_item(p, i, &it) || it.gap)
            continue;
        if (it.mix || it.speed != 100) {
            costly(&v, it.start, it.start + it.length);
            continue;
        }
        if (it.fade_in > 0)
            costly(&v, it.start, it.start + it.fade_in);
        if (it.fade_out > 0)
            costly(&v, it.start + it.length - it.fade_out, it.start + it.length);
    }
    for (int i = 0; i < re_project_overlays(p); i++) {
        re_overlay o;
        if (!re_project_overlay(p, i, &o))
            costly(&v, o.start, o.start + o.length);
    }
    qsort(v.s, (size_t) v.n, sizeof(re_stretch), by_start);
    int n = 0;
    for (int i = 0; i < v.n; i++) { /* (joined: overlapping, or near) */
        re_stretch *s = &v.s[i];
        if (n > 0 && s->start <= out[n - 1].start + out[n - 1].length + v.join) {
            int end = s->start + s->length;
            if (end > out[n - 1].start + out[n - 1].length)
                out[n - 1].length = end - out[n - 1].start;
        } else if (n < max) {
            out[n++] = *s;
        }
    }
    free(v.s);
    for (int i = 0; i < n; i++)
        out[i].sig = re_render_signature(p, out[i].start, out[i].length);
    return n;
}

/* ---- signatures ------------------------------------------------------------------------------ */

static uint64_t fnv(uint64_t h, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static uint64_t fnv(uint64_t h, const char *fmt, ...)
{
    char t[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    for (const char *c = t; *c; c++) {
        h ^= (unsigned char) *c;
        h *= 1099511628211ull;
    }
    return h;
}

static const char *resource(mlt_playlist pl, int entry)
{
    mlt_playlist_clip_info info;
    if (!pl || entry < 0 || mlt_playlist_get_clip_info(pl, &info, entry))
        return "";
    const char *r = rp_speed_file(info.producer, info.resource);
    return r ? r : "";
}

uint64_t re_render_signature(re_project *p, int start, int length)
{
    mlt_profile pr = re_project_profile(p);
    int end = start + length;
    uint64_t h = 14695981039346656037ull;
    h = fnv(h, "%d %dx%d %d/%d|", length, pr->width, pr->height, pr->frame_rate_num, pr->frame_rate_den);
    /* (each thing that's in it, placed from its start: a stretch moved along keeps its signature) */
    for (int i = 0; i < re_project_items(p); i++) {
        re_item it;
        if (re_project_item(p, i, &it) || it.start >= end || it.start + it.length <= start)
            continue;
        h = fnv(h, "V1 %d %d %d %d %d %d %d %d %d %d %d %d %d %d %s|", it.start - start, it.length, it.in, it.out,
                it.gap, it.mix, it.fade_in, it.fade_out, it.sound_fade_in, it.sound_fade_out, it.muted, it.volume,
                it.speed, it.source_length, it.gap || it.mix ? "" : resource(p->v1, i));
    }
    for (int i = 0; i < re_project_overlays(p); i++) {
        re_overlay o;
        re_title t;
        if (re_project_overlay(p, i, &o) || o.start >= end || o.start + o.length <= start)
            continue;
        h = fnv(h, "V2 %d %d %d %d %d %d %d %d %d %d %d %d %d %s|", o.start - start, o.length, o.in, o.place, o.size,
                o.opacity, o.fade_in, o.fade_out, o.video, o.muted, o.volume, o.sound_fade_in, o.sound_fade_out,
                resource(p->v2, rp_track_entry(p->v2, i)));
        if (o.title && !re_project_title(p, i, &t))
            h = fnv(h, "T %s %s %d %06X %d %d|", t.text, t.font, t.size, (unsigned) t.colour, t.style, t.align);
    }
    for (int k = 0; k < re_project_audio_tracks(p); k++)
        for (int i = 0; i < re_project_audio_count(p, k); i++) {
            re_audio a;
            if (re_project_audio(p, k, i, &a) || a.start >= end || a.start + a.length <= start)
                continue;
            h = fnv(h, "A %d %d %d %d %d %d %d %d %s|", k, a.start - start, a.length, a.in, a.muted, a.volume,
                    a.fade_in, a.fade_out, resource(rp_audio_track(p, k), rp_track_entry(rp_audio_track(p, k), i)));
        }
    return h;
}

/* ---- the previews made ---------------------------------------------------------------------- */

struct re_previews
{
    struct
    {
        uint64_t sig;
        int length;
        char *file;
        unsigned used;
    } f[MAX_PREVIEWS];
    int n;
    unsigned clock;
};

re_previews *re_previews_new(void)
{
    return calloc(1, sizeof(re_previews));
}

static void forget(re_previews *v, int i)
{
    remove(v->f[i].file);
    free(v->f[i].file);
    v->f[i] = v->f[--v->n];
}

void re_previews_free(re_previews *v)
{
    if (!v)
        return;
    while (v->n > 0)
        forget(v, v->n - 1);
    free(v);
}

int re_previews_count(re_previews *v)
{
    return v ? v->n : 0;
}

const char *re_previews_find(re_previews *v, uint64_t sig, int length)
{
    for (int i = 0; v && i < v->n; i++)
        if (v->f[i].sig == sig && v->f[i].length == length) {
            v->f[i].used = ++v->clock;
            return v->f[i].file;
        }
    return NULL;
}

int re_previews_add(re_previews *v, uint64_t sig, int length, const char *file)
{
    if (!v || !file)
        return -1;
    for (int i = 0; i < v->n; i++) /* (made again: the new one) */
        if (v->f[i].sig == sig && v->f[i].length == length) {
            if (strcmp(v->f[i].file, file))
                forget(v, i);
            else
                return 0;
            break;
        }
    if (v->n == MAX_PREVIEWS) { /* (the least recently used goes) */
        int old = 0;
        for (int i = 1; i < v->n; i++)
            if (v->f[i].used < v->f[old].used)
                old = i;
        forget(v, old);
    }
    char *copy = strdup(file);
    if (!copy)
        return -1;
    v->f[v->n].sig = sig;
    v->f[v->n].length = length;
    v->f[v->n].file = copy;
    v->f[v->n].used = ++v->clock;
    v->n++;
    return 0;
}

/* ---- playing them ------------------------------------------------------------------------------ */

mlt_producer re_previews_producer(re_previews *v, re_project *p, int *rendered)
{
    re_stretch s[256];
    int n = re_render_stretches(p, s, 256), len = re_project_length(p), from_files = 0, at = 0;
    mlt_producer whole = re_project_producer(p);
    if (rendered)
        *rendered = 0;
    if (!v || !whole || n == 0)
        return NULL;
    mlt_playlist pl = mlt_playlist_new(p->profile);
    if (!pl)
        return NULL;
    for (int i = 0; i < n; i++) {
        const char *file = re_previews_find(v, s[i].sig, s[i].length);
        mlt_producer f = file ? mlt_factory_producer(p->profile, NULL, file) : NULL;
        if (f && mlt_producer_get_length(f) < s[i].length) { /* (cut short: not used) */
            mlt_producer_close(f);
            f = NULL;
        }
        if (!f)
            continue;
        if (s[i].start > at)
            mlt_playlist_append_io(pl, whole, at, s[i].start - 1);
        mlt_playlist_append_io(pl, f, 0, s[i].length - 1);
        mlt_producer_close(f); /* (the playlist has it) */
        at = s[i].start + s[i].length;
        from_files += s[i].length;
    }
    if (from_files == 0) {
        mlt_playlist_close(pl);
        return NULL;
    }
    if (at < len)
        mlt_playlist_append_io(pl, whole, at, len - 1);
    if (rendered)
        *rendered = from_files;
    return MLT_PLAYLIST_PRODUCER(pl);
}
