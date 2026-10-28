/* engine.h - Reel Edit's player: a project (MLT XML) or a clip, played from
 * the Wimp's null events.
 *
 * It makes at most one picture each time it's called, and says how long the
 * caller may wait before calling it again, so the rest of the desktop keeps
 * running (docs/DESIGN.md, "Keeping the desktop running"). It makes no
 * RISC OS calls: the picture, the sound and the clock go through callbacks,
 * so it can be tested on Linux.
 *
 *   re_engine *e = re_engine_open("project.mlt", 0.5, &io);
 *   re_engine_play(e, 1.0);
 *   for (;;) {                    // the Wimp_Poll loop
 *       int wait = re_engine_poll(e);
 *       // wait < 0: no null events needed; else Wimp_PollIdle until now + wait cs
 *   }
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef RE_ENGINE_H
#define RE_ENGINE_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct re_engine re_engine;

/* Starts MLT (once; every module built in). 0, or -1. */
int re_mlt_start(void);
/* MLT's and FFmpeg's messages (information and worse) as lines to this,
   instead of stderr: the hardware decoders say there when they wait or
   give up. */
void re_engine_messages(void (*line)(const char *line));

/* A picture to show: yuv420p, three planes. Valid only during show(). */
typedef struct
{
    const uint8_t *planes[3];
    int strides[3];
    int width, height;
    int position; /* the frame's position in the project */
    int colorspace; /* 601 or 709 */
    int full_range; /* 1: 0-255, 0: video range (16-235) */
    double dar;     /* its shape on the screen (width over height; 0: as its pixels) */
} re_picture;

typedef struct
{
    /* Show this picture now. */
    void (*show)(void *ctx, const re_picture *picture);
    /* Sound for the frames played, interleaved signed 16-bit, at the rate and
       channels given to re_engine_open; NULL: no sound. */
    void (*sound)(void *ctx, const int16_t *samples, int frames);
    /* A monotonic clock in centiseconds (OS_ReadMonotonicTime). */
    unsigned (*clock_cs)(void *ctx);
    void *ctx;
    int sound_rate;     /* samples a second (default 48000) */
    int sound_channels; /* default 2 */
    /* Seconds of sound given and not yet heard (SharedSoundBuffer's queue).
       Given, the sound is the clock at normal speed: it's taken about
       0.4 s ahead of the picture, and the pictures follow what's heard.
       NULL: each frame's sound is given as its picture is shown. */
    double (*sound_queued)(void *ctx);
} re_engine_io;

typedef struct
{
    int rendered;       /* pictures made */
    int skipped;        /* pictures left out to keep up with the clock */
    int resyncs;        /* times it fell so far behind that the clock was moved on */
    int sound_frames;   /* sample frames given to sound() */
    int fetched;        /* frames asked of MLT (pictures made, left out, and kept for going backwards) */
    int jumps;          /* ... not straight after the one before: a seek, which decodes from a keyframe */
    int reverse_fills;  /* runs of frames decoded forwards and kept, to be shown going backwards */
    int clock_moves;    /* times the picture clock was steered to the sound heard */
    int slowed;         /* pictures the clock waited for (decoding slower than playing) */
    /* where playing's time went (clock_cs, summed: each reading's rounding
       evens out over many): MLT making a frame (decoding, the tracks, the
       transitions), its picture (converting, compositing), show(), sound */
    unsigned cs_frame, cs_picture, cs_show, cs_sound;
} re_engine_stats;

/* Open a project or clip, at the project's own size and frame rate (a
   clip's: the clip's). preview_scale: the pictures' size against that (1
   full size, 0.5 half). NULL on failure. */
re_engine *re_engine_open(const char *file, double preview_scale, const re_engine_io *io);
/* Plays a producer the caller keeps (a project's: project.h), at its
   profile. After each edit, re_engine_refresh. */
struct mlt_profile_s;
struct mlt_producer_s;
re_engine *re_engine_open_producer(struct mlt_profile_s *profile, struct mlt_producer_s *producer,
                                   double preview_scale, const re_engine_io *io);
/* The producer has changed (an edit): its length read again, the frames
   kept for going backwards forgotten, and stopped, the picture made again. */
void re_engine_refresh(re_engine *e);
/* Another producer played (the caller's, as long as it's played; the same
   length: the project's, or its render previews' playlist), from the same
   frame, playing on at the speed it was */
void re_engine_set_producer(re_engine *e, struct mlt_producer_s *producer);
/* As re_engine_refresh, but the picture on show isn't made again (unless
   it's now past the end): for edits made many times a second (a drag),
   where decoding a picture each time would hold the desktop up. */
void re_engine_invalidate(re_engine *e);
void re_engine_close(re_engine *e);

int re_engine_length(re_engine *e);  /* frames */
double re_engine_fps(re_engine *e);
int re_engine_width(re_engine *e);   /* the pictures' size (after preview_scale) */
int re_engine_height(re_engine *e);

/* Play at a speed (1 normal, 2 twice as fast, -1 backwards; 0 stops). Sound
   only at 1. Faster than 1 (2, 4) shows every second or fourth frame, so no
   more pictures a second are made than at 1.

   Backwards: a frame on its own would be decoded from its keyframe each
   time (in long-GOP video, up to hundreds of frames for one picture, the
   desktop held for a second), so runs of frames before the one due are
   decoded forwards, one a poll, kept, and shown going back; the clock waits
   while a run is decoded. */
void re_engine_play(re_engine *e, double speed);
double re_engine_speed(re_engine *e);
/* Playing forwards (after re_engine_play): stop on this frame, shown, as at
   the end (the in-to-out play of I/O marks). re_engine_play clears it. */
void re_engine_play_to(re_engine *e, int frame);

/* Go to a frame. Seeks asked for before the next poll replace each other:
   only the last one is drawn. */
void re_engine_seek(re_engine *e, int position);

/* The frame on show (or about to be, after a seek). */
int re_engine_position(re_engine *e);

/* 1 when a picture is waiting to be made (a seek, or playing): the caller
   should ask for null events (Wimp_Poll without masking them) and poll. */
int re_engine_busy(re_engine *e);

/* Do some work: at most one picture. Returns the centiseconds the caller may
   wait before calling again (0: at once), or -1 when nothing will happen
   until play or seek is called. */
int re_engine_poll(re_engine *e);

void re_engine_get_stats(re_engine *e, re_engine_stats *stats);

#ifdef __cplusplus
}
#endif
#endif
