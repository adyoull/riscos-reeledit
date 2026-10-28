/* source.c - the project's clips list, and source clips put on V1 by
 * three-point editing (project.h).
 *
 * The clips list (the Clips window's, Premiere's bin): the files the
 * project can draw on, in the order they came, each once. Kept in memory
 * and saved as a property of the tractor's ("shotcut:reeledit_bin": MLT
 * names a line each; "shotcut:" properties are the tractor's only ones
 * the XML keeps), read when a project is opened. Changing it marks the
 * project changed, but isn't an edit to undo.
 *
 * A source clip's marked frames (in to out of its file) put at a frame of
 * V1 (Premiere's Insert and Overwrite):
 *   insert     the clip at that frame split there, the source put
 *              between; everything after moves on
 *   overwrite  the source over what's there, for its length (whatever it
 *              covers taken away, split at both ends); past the end, the
 *              project is made longer (a gap first, if the frame is past it)
 * Neither into a dissolve.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define BIN "shotcut:reeledit_bin"

/* ---- the clips list -------------------------------------------------------------------------- */

void rp_bin_free(re_project *p)
{
    for (int i = 0; i < p->n_bin; i++)
        free(p->bin[i]);
    free(p->bin);
    p->bin = NULL;
    p->n_bin = 0;
}

static int add(re_project *p, const char *name)
{
    for (int i = 0; i < p->n_bin; i++)
        if (!strcmp(p->bin[i], name))
            return i;
    char **more = realloc(p->bin, (p->n_bin + 1) * sizeof(char *));
    if (!more)
        return -1;
    p->bin = more;
    if (!(p->bin[p->n_bin] = strdup(name)))
        return -1;
    return p->n_bin++;
}

void rp_bin_read(re_project *p)
{
    const char *s = p->tractor ? mlt_properties_get(MLT_TRACTOR_PROPERTIES(p->tractor), BIN) : NULL;
    rp_bin_free(p);
    while (s && *s) {
        const char *e = strchr(s, '\n');
        int n = e ? (int) (e - s) : (int) strlen(s);
        char name[1024];
        if (n > 0 && n < (int) sizeof(name)) {
            memcpy(name, s, n);
            name[n] = 0;
            add(p, name);
        }
        s = e ? e + 1 : NULL;
    }
}

void rp_bin_write(re_project *p)
{
    if (!p->tractor)
        return;
    size_t n = 1;
    for (int i = 0; i < p->n_bin; i++)
        n += strlen(p->bin[i]) + 1;
    char *s = malloc(n), *at = s;
    if (!s)
        return;
    *s = 0;
    for (int i = 0; i < p->n_bin; i++)
        at += sprintf(at, "%s%s", i ? "\n" : "", p->bin[i]);
    mlt_properties_set(MLT_TRACTOR_PROPERTIES(p->tractor), BIN, p->n_bin ? s : NULL);
    free(s);
}

int re_project_clips(re_project *p)
{
    return p->n_bin;
}

const char *re_project_clip(re_project *p, int i)
{
    return i >= 0 && i < p->n_bin ? p->bin[i] : NULL;
}

int re_project_clip_add(re_project *p, const char *name)
{
    int before = p->n_bin, i = add(p, name);
    if (i >= 0 && p->n_bin != before)
        p->state = ++p->next_state; /* (changed: to be saved; not an edit to undo) */
    return i;
}

int re_project_clip_remove(re_project *p, int i)
{
    if (i < 0 || i >= p->n_bin)
        return 0;
    free(p->bin[i]);
    memmove(p->bin + i, p->bin + i + 1, (p->n_bin - i - 1) * sizeof(char *));
    p->n_bin--;
    p->state = ++p->next_state;
    return 1;
}

/* ---- three-point editing ------------------------------------------------------------------- */

/* The source opened, its frames in..out: a cut of them (ours), or NULL */
static mlt_producer source(re_project *p, const char *file, int in, int out)
{
    mlt_producer clip = rp_open_clip(p, file);
    if (!clip)
        return NULL;
    int len = mlt_producer_get_length(clip);
    if (len >= INT_MAX / 2 || in < 0 || out < in || out >= len) {
        mlt_producer_close(clip);
        return NULL;
    }
    mlt_producer cut = mlt_producer_cut(clip, in, out);
    mlt_producer_close(clip);
    return cut;
}

/* A dissolve over any of frames f0..f1 */
static int dissolved(re_project *p, int f0, int f1)
{
    for (int i = 0; i < re_project_items(p); i++) {
        re_item it;
        if (!re_project_item(p, i, &it) && it.mix && it.start <= f1 && it.start + it.length - 1 >= f0)
            return 1;
    }
    return 0;
}

/* The index of the item starting at a frame (the number of items: the end) */
static int item_starting(re_project *p, int frame)
{
    int n = re_project_items(p);
    for (int i = 0; i < n; i++) {
        re_item it;
        if (!re_project_item(p, i, &it) && it.start >= frame)
            return i;
    }
    return n;
}

int re_project_insert(re_project *p, const char *file, int in, int out, int frame)
{
    mlt_producer cut = source(p, file, in, out);
    return cut ? rp_insert_cut(p, cut, frame) : -1;
}

int rp_insert_cut(re_project *p, mlt_producer cut, int frame)
{
    if (frame < 0)
        frame = 0;
    int len = re_project_length(p);
    if (frame > len)
        frame = len;
    if (frame > 0 && frame < len && dissolved(p, frame - 1, frame)) {
        mlt_producer_close(cut);
        return -1;
    }
    rp_checkpoint(p);
    if (rp_make_tracks(p) < 0) {
        rp_no_edit(p);
        mlt_producer_close(cut);
        return -1;
    }
    rp_split_at(p, frame);
    int at = item_starting(p, frame);
    mlt_playlist_insert(p->v1, cut, at, -1, -1);
    mlt_producer_close(cut);
    rp_changed(p);
    return at;
}

int re_project_overwrite(re_project *p, const char *file, int in, int out, int frame)
{
    if (frame < 0)
        frame = 0;
    int len = re_project_length(p), n = out - in + 1;
    if (n < 1 || dissolved(p, frame, frame + n - 1))
        return -1;
    mlt_producer cut = source(p, file, in, out);
    if (!cut)
        return -1;
    rp_checkpoint(p);
    if (rp_make_tracks(p) < 0) {
        rp_no_edit(p);
        mlt_producer_close(cut);
        return -1;
    }
    if (frame > len) /* (past the end: a gap to it) */
        mlt_playlist_blank(p->v1, frame - len - 1);
    rp_split_at(p, frame + n); /* (the end first: the item at the start keeps its index) */
    rp_split_at(p, frame);
    for (int i = re_project_items(p) - 1; i >= 0; i--) { /* what it covers taken away */
        re_item it;
        if (!re_project_item(p, i, &it) && it.start >= frame && it.start + it.length <= frame + n)
            mlt_playlist_remove(p->v1, i);
    }
    int at = item_starting(p, frame);
    mlt_playlist_insert(p->v1, cut, at, -1, -1);
    mlt_producer_close(cut);
    rp_changed(p);
    return at;
}
