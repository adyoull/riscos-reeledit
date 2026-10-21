/* project.h - a Reel Edit project: MLT's model (a tractor whose video track
 * V1 is a playlist of clips and gaps), edited by the Timeline and played by
 * the engine. Saved and loaded as MLT XML, as Shotcut and Kdenlive keep
 * theirs. No RISC OS calls, so it's tested on Linux.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef RE_PROJECT_H
#define RE_PROJECT_H
#include <framework/mlt.h>

typedef struct re_project re_project;

/* An empty project: its profile (size, frame rate) is taken from the first
   clip added. */
re_project *re_project_new(void);
/* A project from a file: an MLT XML project (its first video track is V1;
   the others are kept, not yet shown), or a clip (a project of that clip).
   is_xml: it's MLT XML whatever its name (MLT's loader goes by the name's
   extension: a RISC OS file of type XML has none). */
re_project *re_project_open(const char *file, int is_xml);
void re_project_close(re_project *p);

/* What the engine plays: the whole project (NULL while it's empty). The
   project keeps it; its contents change as the project is edited. */
mlt_producer re_project_producer(re_project *p);
mlt_profile re_project_profile(re_project *p);

int re_project_length(re_project *p);   /* frames */
double re_project_fps(re_project *p);
int re_project_modified(re_project *p); /* changed since made, opened or saved */
/* Each edit adds 1: the engine and the windows compare it with what they
   last saw. */
unsigned re_project_changes(re_project *p);

/* V1's items: clips and gaps, in order. */
typedef struct
{
    int start, length;   /* in the project, frames */
    int in, out;         /* in the clip */
    int gap;             /* 1: a gap (blank), not a clip */
    char name[64];       /* the clip's file's leaf name */
} re_item;
int re_project_items(re_project *p);
int re_project_item(re_project *p, int i, re_item *item);
/* The item at a frame, -1 past the end. */
int re_project_item_at(re_project *p, int frame);

/* Adds a clip (a video file) to V1 at a frame: at the end (-1, or past the
   end), else at the start of the item there (or, if the frame is nearer
   that item's end, after it). The item index it became, or -1 if the file
   can't be opened. */
int re_project_add(re_project *p, const char *file, int frame);
/* Splits the item at a frame in two (nothing at either end of an item).
   1 split, 0 not. */
int re_project_split(re_project *p, int frame);
/* Removes an item. ripple: what follows moves up (else a gap is left).
   A gap removed is always closed up. 1 done, 0 not. */
int re_project_remove(re_project *p, int item, int ripple);

/* Saves as MLT XML. 0, or -1. Saved: not modified. */
int re_project_save(re_project *p, const char *file);

#endif
