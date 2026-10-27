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
    int mix;             /* 1: a dissolve, the clips either side mixed (its in and out: 0, length - 1) */
    int fade_in, fade_out; /* a clip's fades from and to black (frames; 0: none) */
    int source_length;   /* the clip's file's length (frames of the project): out is below it */
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
/* re_project_add's answer for a file that doesn't say how long it is (raw
   MPEG-2 or H.264 video): nothing added; measure it (measure.h) and add it
   with its length */
#define RE_ADD_MEASURE (-2)
/* A clip added, its length (frames of the project) given (0: the file's) */
int re_project_add_length(re_project *p, const char *file, int frame, int length);
/* Seconds as frames of the project */
int re_project_frames(re_project *p, double seconds);
/* Splits the item at a frame in two (nothing at either end of an item).
   1 split, 0 not. */
int re_project_split(re_project *p, int frame);
/* Removes an item. ripple: what follows moves up (else a gap is left).
   A gap removed is always closed up. A dissolve removed: the clips as they
   were before it; a clip removed takes its dissolves with it. 1 done, 0 not. */
int re_project_remove(re_project *p, int item, int ripple);

/* Ripple trims (Premiere's Q and W): the clip at a frame trimmed so it
   starts there (end 0: the frames before it go) or ends just before it
   (end 1: it and those after go); what follows moves up. 1 done, 0 not. */
int re_project_trim(re_project *p, int frame, int end);
/* The cut (an item's start, or the end) before (dir -1) or after (dir 1) a
   frame; -1 if none. */
int re_project_cut(re_project *p, int frame, int dir);

/* An edit made of many steps (a drag): one undo for all of it. Between
   begin and end the steps below change the project without undo points of
   their own; end with nothing changed leaves no undo point and the project
   as unmodified as it was. */
void re_project_begin(re_project *p);
void re_project_end(re_project *p);
/* (Between begin and end.) A clip's in and out (frames of its file), what
   follows moving up or along: ripple trimming by dragging an edge. Kept to
   the file and to one frame at least. 1 changed, 0 not. */
int re_project_set_in_out(re_project *p, int item, int in, int out);
/* Moves an item so it comes before what is now item `before` (the number
   of items: to the end); what it leaves closes up. Its new index, or -1 if
   nothing moved (a dissolve, a clip in one, or into one, isn't moved). */
int re_project_move(re_project *p, int item, int before);

/* Dissolves: an item of their own between two clips, the end of the one
   mixed into the start of the other over its length (MLT's playlist mix: a
   tractor with luma and mix transitions, marked as Shotcut marks its own).
   overlap 0: from the clips' frames beyond the cut if they have them (the
   first's after its out: the dissolve starts at the cut; or the second's
   before its in: it ends at the cut), the project's length kept; else, or
   overlap 1, the clips overlapped (the last frames of the first, the first
   of the second), the project that much shorter. item: the clip before the
   cut. The dissolve's index, or -1. */
int re_project_dissolve(re_project *p, int item, int length, int overlap);
/* A dissolve made longer or shorter (the clips overlapped more or less;
   kept to what they have, each left a frame at least). 1 changed, 0 not.
   Between begin and end, as a drag makes it, too. */
int re_project_dissolve_length(re_project *p, int mix, int length);
/* The longest a dissolve can be */
int re_project_dissolve_max(re_project *p, int mix);
/* The cut between two clips (no dissolve there yet) nearest a frame: the
   clip before it, or -1 if there's none */
int re_project_dissolve_cut(re_project *p, int frame);

/* Fades from black at a clip's start (end 0) or to black at its end (end
   1), the sound with the picture; length 0 takes it away. Kept to the clip
   (trimmed, the fade stays at its end; split, the fade out goes with the
   second part); not at an end in a dissolve (a dissolve made there takes
   the fades away). 1 changed, 0 not. Between begin and end too. */
int re_project_fade(re_project *p, int item, int end, int length);

/* Undo and redo (up to 100 edits): the project as it was before the last
   edit, and back. 1 done, 0 nothing to do. The producer changes: an
   engine playing it must be opened again. */
int re_project_undo(re_project *p);
int re_project_redo(re_project *p);
int re_project_can_undo(re_project *p);
int re_project_can_redo(re_project *p);

/* The decoder of the clip at a frame (NULL: none there, or not decoded yet) */
const char *re_project_decoder(re_project *p, int frame);

/* What's known of an item's file, for an information window: lines of
   "Label\tValue\n" (File, Length, Video: codec, size, frame rate, colours,
   the decoder in use; Sound: codec, rate, channels). Its length (as
   snprintf), or -1 for a gap or no item. */
int re_project_item_info(re_project *p, int item, char *buf, int size);
/* The project's: its size, frame rate, length, clips. */
int re_project_info(re_project *p, char *buf, int size);

/* Saves as MLT XML. 0, or -1. Saved: not modified. */
int re_project_save(re_project *p, const char *file);

#endif
