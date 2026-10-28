/* project_internal.h - what project.c and dissolve.c share: the project's
 * insides and the steps every edit goes through (and fade.c and
 * overlay.c). Not for the editor's
 * windows or the engine: they use project.h.
 *
 * An edit: rp_checkpoint (the undo point; within re_project_begin/end,
 * none: the edit's is begin's), the MLT playlist calls, then rp_changed; or
 * rp_no_edit if nothing was done.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef RE_PROJECT_INTERNAL_H
#define RE_PROJECT_INTERNAL_H
#include "project.h"

struct re_project
{
    mlt_profile profile;
    mlt_tractor tractor;   /* NULL while empty */
    mlt_playlist v1, v2;   /* V2: NULL until there's an overlay (overlay.c) */
    int own_v1, own_v2;    /* made here (a reference of ours, besides the tractor's) */
    int v1_track, v2_track; /* their places in the tractor */
    unsigned state, next_state, saved_state; /* each edit a new state; modified: not the saved one */
    unsigned changes;
    /* undo and redo: the project as MLT XML before each edit */
    struct snap
    {
        char *xml;         /* "" for an empty project */
        unsigned state;
    } *undo, *redo;
    int n_undo, n_redo;
    /* an edit of many steps (re_project_begin): its undo point's state, and
       the state before it */
    int in_edit;
    unsigned edit_state;
    /* the Clips window's list (source.c): MLT names */
    char **bin;
    int n_bin;
};

/* project.c: V1 (and the tractor) made if the project is empty: 0, or -1;
   a file opened as a clip (at the project's profile, the first's own); the
   item at a frame split there (not at its start, not a dissolve): 1 split,
   0 not, no undo point of its own */
int rp_make_tracks(re_project *p);
mlt_producer rp_open_clip(re_project *p, const char *file);
int rp_split_at(re_project *p, int frame);

/* source.c: the Clips window's list read from the tractor (a project
   opened), written to it (before saving), given up */
void rp_bin_read(re_project *p);
void rp_bin_write(re_project *p);
void rp_bin_free(re_project *p);
/* source.c: a cut put on V1 at a frame (insert: what's there split, what
   follows moving on; past the end: at the end), taken over. Its index, or
   -1 (into a dissolve; closed) */
int rp_insert_cut(re_project *p, mlt_producer cut, int frame);
/* overlay.c: a producer (taken over) put on V2 from a frame, its frames
   in.. for length, placed so: as an overlay's added. Its index, or -1.
   audio.c: the same on the audio tracks (stacked); 0 (*track, *i), or -1 */
int rp_overlay_put(re_project *p, mlt_producer prod, int in, int frame, int length, int place, int size);
int rp_audio_put(re_project *p, mlt_producer prod, int in, int frame, int length, int *track, int *i);
/* audio.c: an audio track's playlist (NULL: none) */
mlt_playlist rp_audio_track(re_project *p, int k);


void rp_checkpoint(re_project *p); /* before an edit: the project as it is, to undo to */
void rp_no_edit(re_project *p);    /* an edit that didn't happen: its checkpoint dropped */
void rp_changed(re_project *p);    /* after an edit: the links, the length, a new state */

/* dissolve.c: the dissolve item i is (NULL: not one); a clip (not a gap,
   not a dissolve); MLT's links between dissolves and their clips made
   again (after any edit, and a project read from XML) */
mlt_tractor rp_mix_of(re_project *p, int i);
int rp_is_clip(re_project *p, int i);
void rp_relink(re_project *p);
/* A dissolve taken out, the clips either side as they were before it (not
   an edit of its own: within one). 0, or -1. */
int rp_unmix(re_project *p, int mix);

/* fade.c: fades of a clip's picture and its sound (kinds: RP_PICTURE,
   RP_SOUND, or both), at its start (end 0) or its end (end 1). On a cut
   (in, out: its frames of its file): the filter named so; a fade's length
   (0: none); set (0: taken away); kept to the cut's ends. On V1's item:
   the same; a fade made as an edit (rp_fade: 1 changed, 0 not); every
   fade (V1's and V2's) kept to its clip (after every edit). */
enum { RP_PICTURE = 1, RP_SOUND = 2 };
mlt_filter rp_cut_filter(mlt_producer cut, const char *name);
int rp_cut_fade(mlt_producer cut, int kind, int end);
void rp_cut_set_fade(re_project *p, mlt_producer cut, int kinds, int end, int length, int in, int out);
void rp_cut_fit_fades(re_project *p, mlt_producer cut, int in, int out);
int rp_fade_of(re_project *p, int item, int kind, int end);
void rp_set_fade(re_project *p, int item, int kinds, int end, int length);
int rp_fade(re_project *p, int item, int kinds, int end, int length);
void rp_fit_fades(re_project *p);

/* clipsound.c: a cut's sound: whether it has any (a picture, or a video
   without, hasn't), muted or not, muting it (not an edit of its own); the
   dissolves' clips' ends muted as their clips are (after every edit) */
int rp_cut_has_sound(mlt_producer cut);
int rp_cut_muted(mlt_producer cut);
void rp_cut_mute(re_project *p, mlt_producer cut, int on);
int rp_cut_volume(mlt_producer cut);                          /* dB (0: as it is) */
void rp_cut_set_volume(re_project *p, mlt_producer cut, int db);
void rp_fit_sound(re_project *p);

/* track.c: tracks of separate items (V2, the audio tracks): their number
   (not the gaps) and each one's playlist index (-1: none); each item's
   cut (a reference of ours), start, length and in, in order (read: their
   number, or -1; rp_track_free gives them up); the track made of them
   again (write: the slots given up); where there's room for one from
   start, so long (skip: one not counted; end: the project's length): 0,
   or -1; sorted by start; an item moved or trimmed (media: its in moves
   with a trimmed start, kept within source_length): 1 changed, 0 already
   so, -1 not */
typedef struct
{
    mlt_producer cut;
    int start, length;
    int in;
} rp_slot;
int rp_track_items(mlt_playlist pl);
int rp_track_entry(mlt_playlist pl, int i);
int rp_track_read(mlt_playlist pl, rp_slot **s);
void rp_track_free(rp_slot *s, int n);
void rp_track_write(mlt_playlist pl, rp_slot *s, int n);
int rp_track_room(int end, rp_slot *s, int n, int skip, int *start, int *length);
void rp_track_sort(rp_slot *s, int n);
int rp_track_span(re_project *p, mlt_playlist pl, int i, int start, int length, int media, int source_length);

/* audio.c: the audio tracks' mix transitions made again (after every
   edit, and a project read) */
void rp_fit_audio(re_project *p);

/* overlay.c: the composite transitions putting V2's pictures over V1 made
   again from their settings (after every edit, and a project read) */
void rp_fit_overlays(re_project *p);

#endif
