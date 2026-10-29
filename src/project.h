/* project.h - a ReelEdit project: MLT's model (a tractor whose video track
 * V1 is a playlist of clips and gaps), edited by the Timeline and played by
 * the engine. Saved and loaded as MLT XML, as Shotcut and Kdenlive keep
 * theirs. No RISC OS calls, so it's tested on Linux.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef RE_PROJECT_H
#define RE_PROJECT_H
#include <framework/mlt.h>
#include "title.h"

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
void re_project_set_modified(re_project *p); /* (a copy recovered: still to be saved) */
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
    int sound_fade_in, sound_fade_out; /* its sound's fades from and to silence (with the picture's, or alone) */
    int sound;           /* 1: its file has sound (a picture or a silent video hasn't) */
    int muted;           /* 1: its sound muted */
    int volume;          /* its sound's level: dB (0: as it is) */
    int source_length;   /* the clip's file's length (frames of the project): out is below it; at
                            another speed, its length at that speed */
    int speed;           /* % (100: as it is; 200 twice as fast; negative: reversed) */
    int coloured;        /* 1: its colour changed (re_project_colour) */
    int moving;          /* 1: it pans or zooms (re_project_motion) */
    int picture;         /* 1: a still picture (a picture file, a freeze frame or a colour clip) */
    int held;            /* 1: a freeze frame (re_project_freeze) */
    int held_frame;      /* its frame of its file (as re_item_file_frame's) */
    int fill;            /* a colour clip's colour (0xRRGGBB), or -1: not one */
    char name[64];       /* the clip's file's leaf name */
} re_item;
int re_project_items(re_project *p);
int re_project_item(re_project *p, int i, re_item *item);
/* Its frame f (in..out) as a frame of its file (at 100%): where its
   thumbnails and waveform are */
int re_item_file_frame(const re_item *it, int f);
/* The item at a frame, -1 past the end. */
int re_project_item_at(re_project *p, int frame);
/* A clip's file (for its thumbnails and waveform, media.h): NULL for a
   gap or a dissolve. Good until the next edit. */
const char *re_project_item_file(re_project *p, int i);

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

/* The frames in to out taken out (Premiere's Extract, with ripple: what
   follows moves up; Lift, without: a gap is left). Clips are split at both
   ends. Not across a dissolve (remove it first). 1 done, 0 not. */
int re_project_remove_range(re_project *p, int in, int out, int ripple);

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
/* A roll (Premiere's rolling edit): the cut after an item moved to a
   frame, the item ending there and the next starting there, the project's
   length kept. Either may be a gap, not both; not a dissolve. Kept to the
   clips' files, each side a frame at least. 1 changed, 0 not. Between
   begin and end too. */
int re_project_roll(re_project *p, int item, int frame);
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

/* A clip's speed (speed.c): % (10 to 2000, or -10 to -2000: reversed;
   100: as it is), the part of its file it shows kept, its start kept and
   what follows moving along; its fades, mute and level kept. The sound
   goes faster or slower with it (its pitch too). Not a picture, nor a clip
   in a dissolve. One edit; 1 changed, 0 not. */
int re_project_speed(re_project *p, int item, int percent);

/* Fades from black at a clip's start (end 0) or to black at its end (end
   1), the sound with the picture; length 0 takes it away. Kept to the clip
   (trimmed, the fade stays at its end; split, the fade out goes with the
   second part); not at an end in a dissolve (a dissolve made there takes
   the fades away). 1 changed, 0 not. Between begin and end too. */
int re_project_fade(re_project *p, int item, int end, int length);

/* A clip's sound on its own (clipsound.c): muted (on 1) or not (0); faded in
   (end 0) or out (end 1) without the picture, length 0 taking the fade
   away (a clip's fade above sets both: this changes the sound's alone).
   Not for a clip without sound; a fade not at an end in a dissolve. 1
   changed, 0 not. Kept as a clip's fades and mute are: split, the second
   part is muted too and has the fade out. */
int re_project_mute(re_project *p, int item, int on);
int re_project_sound_fade(re_project *p, int item, int end, int length);
/* A clip's sound's level, dB (-60 to +12; 0: as it is): kept as its mute is.
   1 changed, 0 not. */
int re_project_volume(re_project *p, int item, int db);

/* A clip's colour (colour.c): each setting from "as it is" (re_colour_none) */
typedef struct
{
    int brightness; /* -100..100 (0: as it is) */
    int contrast;   /* 0..200 % (100) */
    int saturation; /* 0..200 % (100; 0: black and white) */
    int warmth;     /* -100 (cooler, bluer) .. 100 (warmer, redder) (0) */
    int midtones;   /* -100 (darker) .. 100 (lighter): gamma (0) */
} re_colour;
void re_colour_none(re_colour *c);
int re_colour_is_none(const re_colour *c);
/* Looks: settings by name (0 None ... ); the look a setting is, or -1 */
#define RE_LOOKS 8
extern const char *const re_colour_looks[RE_LOOKS];
void re_colour_look(int look, re_colour *c);
int re_colour_look_of(const re_colour *c);
/* A clip's (0, or -1: not a clip; as it is), and set (kept in range): 1
   changed, 0 not. Between begin and end too. */
int re_project_colour(re_project *p, int item, re_colour *c);
int re_project_set_colour(re_project *p, int item, const re_colour *c);

/* Freeze frames and colour clips (freeze.c). A frame of the clip at a
   frame held for length frames (0: 2 seconds), put in there (the clip
   split there, what follows moving on); not a picture's. A colour clip
   put in at a frame, so long (0: 2 seconds), as a freeze frame is; not in
   an empty project (it says nothing of the size). Each: its item's index,
   or -1. A colour clip's colour changed: 1, or 0 (not a colour clip, or
   that colour already; a colour clip split or copied keeps one colour for
   all its parts). Either is a still: trimmed to any length, faded and
   coloured as a clip is; no sound, no speed. */
int re_project_freeze(re_project *p, int frame, int length);
int re_project_add_fill(re_project *p, int frame, int length, unsigned rgb);
int re_project_set_fill(re_project *p, int item, unsigned rgb);
#define RE_FILLS 9
extern const char *const re_fill_names[RE_FILLS]; /* Black, White, Grey, Red... */
extern const unsigned re_fill_rgbs[RE_FILLS];
const char *re_fill_name(unsigned rgb);            /* (NULL: not one of those) */
/* The producer that makes freeze frames, registered with MLT (re_mlt_start does it) */
void re_hold_register(void);

/* Pan and zoom (motion.c): a clip's picture moving over its length. kind:
   re_motions' (0 None, Zoom in, Zoom out, Pan left to right, right to left,
   upwards, downwards; -1: a motion of Shotcut's own, kept as it is); amount:
   how far in it's zoomed at most, % (10..100: 1.1 to 2 times) */
typedef struct
{
    int kind;
    int amount;
} re_motion;
#define RE_MOTIONS 7
#define RE_MOTION_AMOUNT 30
extern const char *const re_motions[RE_MOTIONS];
void re_motion_none(re_motion *m);
/* A clip's (0, or -1: not a clip), and set: 1 changed, 0 not. Between
   begin and end too. */
int re_project_motion(re_project *p, int item, re_motion *m);
int re_project_set_motion(re_project *p, int item, const re_motion *m);
/* The filter that does it, registered with MLT (re_mlt_start does it) */
void re_motion_register(void);

/* Sound clean-up (soundfx.c): a clip's, a video overlay's or a sound's */
typedef struct
{
    int pan;    /* -100 (left) .. 100 (right); 0 the middle */
    int rumble; /* 1: low rumble cut (wind, traffic, handling) */
    int voice;  /* 1: voices made clearer */
    int gate;   /* 1: the background turned down when it's quiet */
} re_sound_fx;
void re_sound_fx_none(re_sound_fx *fx);
/* 0, or -1 (no such item, or it has no sound); set: 1 changed, 0 not.
   Between begin and end too. */
int re_project_sound_fx(re_project *p, int item, re_sound_fx *fx);
int re_project_set_sound_fx(re_project *p, int item, const re_sound_fx *fx);
int re_project_overlay_sound_fx(re_project *p, int i, re_sound_fx *fx);
int re_project_overlay_set_sound_fx(re_project *p, int i, const re_sound_fx *fx);
int re_project_audio_sound_fx(re_project *p, int track, int i, re_sound_fx *fx);
int re_project_audio_set_sound_fx(re_project *p, int track, int i, const re_sound_fx *fx);
/* The level (dB, -36..12) that brings a stretch [f0, f1) of a file's peaks
   (media.h's: a byte a frame) to -3 dB at its loud parts: 0, or -1 (too
   little sound to tell) */
int re_normal_level(const unsigned char *peaks, int n, int f0, int f1, int *db);

/* Overlays: pictures (logos, titles) and videos (a picture in the
   picture) over the video, on V2, the track above V1 (overlay.c). Each is
   a still picture (PNG, with its alpha, or JPEG: still.h) or a video, shown
   for a stretch of the project, fitted in a box where it's placed, so
   opaque, fading in and out; a video's sound mixed with V1's. They don't overlap, and are
   kept within the project (edits put each where there's room). Saved
   with the project as MLT's composite transitions, the pictures as qimage
   producers, as Shotcut saves its own (not tried in Shotcut yet). */
enum { RE_PLACE_FULL, RE_PLACE_CENTRE, RE_PLACE_TOP_LEFT, RE_PLACE_TOP_RIGHT, RE_PLACE_BOTTOM_LEFT,
       RE_PLACE_BOTTOM_RIGHT, RE_PLACE_LOWER_THIRD, RE_PLACES };
typedef struct
{
    int start, length;     /* in the project, frames */
    int place;             /* RE_PLACE_: all the frame, or a box in its middle, a corner, the lower third */
    int size;              /* the box: % of the frame's width and height (the lower third's: its height) */
    int opacity;           /* % */
    int fade_in, fade_out; /* frames (0: none) */
    int video;             /* 1: a video (else a picture) */
    int title;             /* 1: a title (a picture made from text: title.h) */
    int in;                /* a video's first frame in its file (a picture's: 0) */
    int source_length;     /* a video's file's length (frames; a picture's: 0) */
    int sound;             /* 1: a video with sound */
    int muted;             /* a video's sound muted */
    int volume;            /* a video's sound's level, dB */
    int sound_fade_in, sound_fade_out; /* a video's sound's fades (frames; 0: none) */
    char name[64];         /* the file's leaf name (a picture's without .png or .jpg) */
    int track;             /* 2: on V2; 3: on the titles' track, over V2 (titles made here) */
} re_overlay;
/* Their number, and each (in order, from 0: V2's, then the titles'
   track's, each by start). 0, or -1. */
int re_project_overlays(re_project *p);
int re_project_overlay(re_project *p, int i, re_overlay *o);
/* The overlay at a frame (on either track: V2's first), or -1 */
int re_project_overlay_at(re_project *p, int frame);
/* The one at a frame on a track (2: V2, 3: the titles'), or -1; how many
   a track has */
int re_project_overlay_at_track(re_project *p, int track, int frame);
int re_project_track_overlays(re_project *p, int track);
/* A picture or a video put over the video from a frame, so long (0: a
   picture 5 seconds, a video all of it), where there's room (after an
   overlay the frame's in; shortened to fit). A picture as big as the
   frame, or near it, fills it; a smaller one is a logo, top right; a video
   is a picture in the picture, bottom right, a third of the size. Its
   index; -1 (no room, an empty project), or: */
#define RE_OVERLAY_CANT_OPEN (-2)  /* neither a picture nor a video MLT opens */
#define RE_OVERLAY_NO_LENGTH (-3)  /* raw video, no length known (it can go on V1) */
int re_project_overlay_add(re_project *p, const char *file, int frame, int length);
/* An overlay moved (start changed, length kept) or trimmed (either end;
   its start: start changed, its end kept): kept clear of the others and
   within the project, a video within its file (trimming its start moves
   its in). 1 changed, 0 already so, -1 not. Between begin and end too. */
int re_project_overlay_span(re_project *p, int i, int start, int length);
/* Its file (NULL: a title's has none worth a name; good until the next edit) */
const char *re_project_overlay_file(re_project *p, int i);
/* Its place (RE_PLACE_), size and opacity (%; each -1: kept). 1 changed,
   0 not. */
int re_project_overlay_set(re_project *p, int i, int place, int size, int opacity);
/* Its fade in (end 0) or out (end 1), frames (0: none). 1 changed, 0 not.
   Between begin and end too. */
int re_project_overlay_fade(re_project *p, int i, int end, int length);
/* A video's sound: muted (on 1) or not; faded in (end 0) or out (end 1)
   alone (re_project_overlay_fade fades a video's sound with its picture).
   Not for a picture, nor a video without sound. 1 changed, 0 not. */
int re_project_overlay_mute(re_project *p, int i, int on);
int re_project_overlay_volume(re_project *p, int i, int db);
int re_project_overlay_sound_fade(re_project *p, int i, int end, int length);
/* Taken away. 1 done, 0 not. */
int re_project_overlay_remove(re_project *p, int i);

/* Titles (title.h): text over the video, an overlay made from it. Put on
   the titles' track (over V2, so a title goes over a logo or a picture in
   the picture) from a frame, so long (0: 5 seconds), where there's room
   on it; at the
   lower third (its place can be changed as any overlay's; not its size:
   the title's own). Its index, or -1. */
int re_project_title_add(re_project *p, const re_title *t, int frame, int length);
/* A title's settings (an overlay that's a title). 0, or -1. */
int re_project_title(re_project *p, int i, re_title *t);
/* A title changed (its text, font, size, colour, style or lines'
   alignment): made again, its box with it. 1 changed, 0 not. */
int re_project_title_set(re_project *p, int i, const re_title *t);

/* Audio tracks (audio.c): music and sound effects under the video, A1, A2
   and on (below V1), each a track of sounds with gaps between, mixed with
   V1's sound. A sound put where one is playing goes on the next track
   with room for all of it there (a new track if none has), so sounds
   stack. Each is moved and trimmed as an overlay is; muted, given a level
   and faded as a clip's sound is. Saved as Shotcut's audio tracks
   ("shotcut:audio"). */
typedef struct
{
    int track;            /* 0: A1 */
    int start, length;    /* in the project, frames */
    int in;               /* its first frame in its file */
    int source_length;    /* its file's length (frames) */
    int muted, volume;    /* (dB) */
    int fade_in, fade_out; /* frames */
    char name[64];        /* the file's leaf name */
} re_audio;
int re_project_audio_tracks(re_project *p);
int re_project_audio_count(re_project *p, int track);
int re_project_audio(re_project *p, int track, int i, re_audio *s);
int re_project_audio_at(re_project *p, int track, int frame); /* -1: none */
const char *re_project_audio_file(re_project *p, int track, int i); /* (as re_project_item_file) */
/* A sound (a file with sound: an audio file, or a video's sound) put from
   a frame, so long (0: all of it), kept to the project's end. 0 and where
   it went (*track, *i); -1 (an empty project, past its end), or: */
#define RE_SOUND_CANT_OPEN (-2)
#define RE_SOUND_NO_SOUND  (-3)
int re_project_audio_add(re_project *p, const char *file, int frame, int length, int *track, int *i);
/* Moved or trimmed (as re_project_overlay_span). 1 changed, 0 already so, -1 not. */
int re_project_audio_span(re_project *p, int track, int i, int start, int length);
/* Taken away (an empty last track with it). 1 done, 0 not. */
int re_project_audio_remove(re_project *p, int track, int i);
/* Muted; its level (dB); faded in (end 0) or out (end 1) (0: none). 1
   changed, 0 not. Between begin and end too. */
int re_project_audio_mute(re_project *p, int track, int i, int on);
int re_project_audio_volume(re_project *p, int track, int i, int db);
int re_project_audio_fade(re_project *p, int track, int i, int end, int length);

/* The clips list (the Clips window's: source.c): files the project can
   draw on (MLT names), each once, in the order they came; saved with it.
   Changing it marks the project changed (not an edit to undo). add: its
   index (there already: that one), or -1. */
int re_project_clips(re_project *p);
const char *re_project_clip(re_project *p, int i);
int re_project_clip_add(re_project *p, const char *name);
int re_project_clip_remove(re_project *p, int i);

/* Three-point editing: a source's frames in..out (of its file) put at a
   frame of V1 (past the end: there, a gap before it). Insert: what's at
   the frame split there, what follows moving on. Overwrite: over what's
   there for its length (split at both ends, taken away). Not into a
   dissolve. Its item's index, or -1. */
int re_project_insert(re_project *p, const char *file, int in, int out, int frame);
int re_project_overwrite(re_project *p, const char *file, int in, int out, int frame);

/* Copy and paste (clipboard.c): a clip on V1 (track not used), an overlay
   on V2 (not used) or a sound on an audio track, copied with its fades,
   mute, level and (an overlay) place, size and opacity; NULL if there's no
   such thing. Pasted at a frame as one edit: a clip inserted there (what's
   there split, what follows moving on; not into a dissolve), an overlay
   where there's room from there, a sound on the first audio track with
   room (*track). Its index, or -1. The copy keeps what it was copied from
   (deleted since, it can still be pasted); for this project only. */
typedef struct re_copy re_copy;
enum { RE_COPY_CLIP, RE_COPY_OVERLAY, RE_COPY_SOUND };
re_copy *re_project_copy(re_project *p, int kind, int track, int i);
int re_project_paste(re_project *p, const re_copy *c, int frame, int *track);
int re_copy_kind(const re_copy *c);   /* RE_COPY_ (-1: NULL) */
int re_copy_length(const re_copy *c); /* frames */
void re_copy_free(re_copy *c);

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
/* The same, a copy (to export from): modified as it was */
int re_project_save_copy(re_project *p, const char *file);

#endif
