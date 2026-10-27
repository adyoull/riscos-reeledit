/* project_internal.h - what project.c and dissolve.c share: the project's
 * insides and the steps every edit goes through (and fade.c and
 * overlay.c). Not for the editor's
 * windows or the engine: they use project.h.
 *
 * An edit: rp_checkpoint (the undo point; not within re_project_begin/end),
 * the MLT playlist calls, then rp_changed; or rp_no_edit if nothing was done.
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
};


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

/* fade.c: a clip's fade at its start (end 0) or its end (end 1): its
   length (0: none); set (0: taken away; not an edit of its own); and every
   fade kept to its clip's ends and length (after every edit) */
int rp_fade_of(re_project *p, int item, int end);
void rp_set_fade(re_project *p, int item, int end, int length);
void rp_fit_fades(re_project *p);

/* overlay.c: the composite transitions putting V2's pictures over V1 made
   again from their settings (after every edit, and a project read) */
void rp_fit_overlays(re_project *p);

#endif
