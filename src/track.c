/* track.c - tracks of separate items (project_internal.h): V2's overlays
 * (overlay.c) and the audio tracks' sounds (audio.c). Each is a playlist
 * of items with gaps between, put back in order after every edit of it:
 * the items' cuts (with what's kept on them: their filters and settings)
 * where they're wanted, gaps between. Items don't overlap, and edits keep
 * each where there's room, within the project.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project_internal.h"

#include <stdlib.h>
#include <string.h>

int rp_track_items(mlt_playlist pl)
{
    int n = 0, k = pl ? mlt_playlist_count(pl) : 0;
    for (int i = 0; i < k; i++)
        n += !mlt_playlist_is_blank(pl, i);
    return n;
}

int rp_track_entry(mlt_playlist pl, int i)
{
    int k = pl && i >= 0 ? mlt_playlist_count(pl) : 0;
    for (int e = 0; e < k; e++)
        if (!mlt_playlist_is_blank(pl, e) && i-- == 0)
            return e;
    return -1;
}

int rp_track_read(mlt_playlist pl, rp_slot **s)
{
    int n = rp_track_items(pl);
    *s = calloc(n + 1, sizeof(rp_slot));
    if (!*s)
        return -1;
    for (int i = 0; i < n; i++) {
        mlt_playlist_clip_info info;
        mlt_playlist_get_clip_info(pl, &info, rp_track_entry(pl, i));
        (*s)[i].cut = info.cut;
        mlt_properties_inc_ref(MLT_PRODUCER_PROPERTIES(info.cut));
        (*s)[i].start = info.start;
        (*s)[i].length = info.frame_count;
        (*s)[i].in = info.frame_in;
    }
    return n;
}

void rp_track_free(rp_slot *s, int n)
{
    for (int i = 0; i < n; i++)
        mlt_producer_close(s[i].cut);
    free(s);
}

void rp_track_write(mlt_playlist pl, rp_slot *s, int n)
{
    mlt_playlist_clear(pl);
    int at = 0;
    for (int i = 0; i < n; i++) {
        if (s[i].start > at)
            mlt_playlist_blank(pl, s[i].start - at - 1);
        int in = s[i].in, out = s[i].in + s[i].length - 1;
        mlt_playlist_append_io(pl, s[i].cut, in, out);
        mlt_playlist_resize_clip(pl, mlt_playlist_count(pl) - 1, in, out);
        mlt_producer_close(s[i].cut);
        at = s[i].start + s[i].length;
    }
    free(s);
}

int rp_track_room(int end, rp_slot *s, int n, int skip, int *start, int *length)
{
    int lo = 0, hi = end;
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
    return ((const rp_slot *) a)->start - ((const rp_slot *) b)->start;
}

void rp_track_sort(rp_slot *s, int n)
{
    qsort(s, n, sizeof(rp_slot), by_start);
}

int rp_track_span(re_project *p, mlt_playlist pl, int i, int start, int length, int media, int source_length)
{
    rp_slot *s;
    int n = rp_track_read(pl, &s);
    if (n < 0 || i < 0 || i >= n) {
        if (n >= 0)
            rp_track_free(s, n);
        return -1;
    }
    int o_start = s[i].start, o_length = s[i].length, o_in = s[i].in;
    if (length < 1)
        length = 1;
    int end = o_start + o_length;
    if (start != o_start && start + length == end) {
        /* its start trimmed (its end where it was): kept clear of the one
           before, and media to its file's start (its in moves with it) */
        int lo = i > 0 ? s[i - 1].start + s[i - 1].length : 0;
        if (media && o_start - o_in > lo)
            lo = o_start - o_in;
        if (start < lo)
            start = lo;
        if (start > end - 1)
            start = end - 1;
        length = end - start;
        s[i].in = media ? o_in + (start - o_start) : 0;
    } else if (start != o_start) {
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
        /* its end trimmed: to the room after it, media to its file's end */
        if (media && source_length > 0 && length > source_length - o_in)
            length = source_length - o_in;
        if (rp_track_room(re_project_length(p), s, n, i, &start, &length))
            length = o_length;
    }
    if (length < 1 || (start == o_start && length == o_length)) {
        rp_track_free(s, n);
        return o_start == start && o_length == length ? 0 : -1;
    }
    if (!p->in_edit)
        rp_checkpoint(p);
    s[i].start = start;
    s[i].length = length;
    rp_track_write(pl, s, n);
    rp_changed(p);
    return 1;
}
