/* render.h - render previews (render.c): the stretches of a project that
 * are costly to play, made into video files ahead of time, and played
 * from them instead (Premiere's "render previews").
 *
 * A stretch is costly where the picture is more than one clip decoded as
 * it is: an overlay on V2 (a picture, a title, a video in the picture), a
 * dissolve, a clip's fade, a clip at another speed. Stretches less than
 * two seconds apart are taken as one (each change of file costs a decoder
 * start).
 *
 * Each stretch has a signature: a hash of everything that makes its
 * frames and its sound (the clips, overlays and sounds in it, their
 * frames, settings and files, placed from its start). A preview is a file
 * made of a stretch, kept with its signature and length: after an edit,
 * a stretch with the same signature (moved along, say) has the same
 * preview; a changed one has none until it's rendered again.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef RE_RENDER_H
#define RE_RENDER_H
#include "project.h"

#include <stdint.h>

typedef struct
{
    int start, length; /* frames of the project */
    uint64_t sig;
} re_stretch;

/* The costly stretches, in order, up to max of them: how many */
int re_render_stretches(re_project *p, re_stretch *out, int max);
/* The signature of frames start to start + length - 1 */
uint64_t re_render_signature(re_project *p, int start, int length);

/* The previews made: files, each a stretch's (its signature and length).
   The files are the list's: forgetting one (too many: the least recently
   used go) or freeing the list deletes them. */
typedef struct re_previews re_previews;
re_previews *re_previews_new(void);
void re_previews_free(re_previews *v);
/* A file made of a stretch: kept (taking it over). 0, or -1. */
int re_previews_add(re_previews *v, uint64_t sig, int length, const char *file);
/* The file made of a stretch with this signature and length, or NULL */
const char *re_previews_find(re_previews *v, uint64_t sig, int length);
int re_previews_count(re_previews *v);

/* What to play: the project, its rendered stretches from their files. A
   playlist as long as the project (the caller closes it); NULL when no
   stretch has a preview (play the project). *rendered (if given): the
   frames from files. */
mlt_producer re_previews_producer(re_previews *v, re_project *p, int *rendered);

#endif
