/* engine.c - Reel Edit's player (engine.h).
 *
 * MLT is built without threads (MLT_NO_THREADS), and the consumer here is
 * never started: each picture is made inside re_engine_poll, when it's due,
 * by mlt_consumer_get_frame. So the work happens in Reel Edit's own time
 * slice, a picture at a time, and between pictures the Wimp runs the other
 * tasks.
 *
 * Playing follows the clock: the frame due is
 *     base_position + (now - base_clock) * fps * speed
 * and when a picture is late the ones before it are left out (their sound is
 * still taken at normal speed, so it doesn't stutter). More than a second
 * behind (a long stall), the clock is moved on instead.
 *
 * Left out isn't free: MLT decodes every frame up to the one asked for (a
 * long-GOP clip can't be decoded otherwise), only the conversion is saved.
 * So a clip that decodes more slowly than it plays (1080p60 in a 24 fps
 * project, 2.5 decodes a frame) mustn't be chased: each picture further
 * on costs more, the next further still. Playing, at most MAX_LEFT_OUT
 * are left out a poll; further behind, the clock waits for the picture
 * (it plays a little slow, the sound with gaps) and every poll stays short.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "engine.h"

#include <framework/mlt.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The sound kept queued ahead of what's heard (io.sound_queued) */
#define SOUND_AHEAD 0.4
#define SOUND_TOPUP 3 /* frames' sound taken a poll at most */
#define MAX_LEFT_OUT 2 /* frames left out a poll at most, playing (see above) */

/* Going backwards: frames kept (at most this many, and this much memory) */
#define RC_SLOTS 32
#define RC_BYTES (48 << 20)

typedef struct
{
    uint8_t *buf;
    int pos;        /* -1: empty */
    int colorspace, full_range;
} rc_slot;

struct re_engine
{
    re_engine_io io;
    mlt_profile profile;
    mlt_producer producer;
    mlt_consumer consumer;
    double fps;
    int length;
    int width, height;
    double speed;
    double base_position; /* playing: where the clock started from... */
    unsigned base_clock;  /* ...and when */
    int shown;            /* the frame on show (-1: none yet) */
    int seek_to;          /* a frame to draw at the next poll (-1: none) */
    int last_fetched;     /* the last frame asked of MLT */
    int borrowed;         /* the profile and producer are the caller's (a project's) */
    int audio_next;       /* the sound clock: the next frame whose sound is taken */
    double snd_last_q;    /* io.sound_queued's last reading (-1: none yet) */
    /* going backwards (re_engine_play): frames decoded forwards and kept */
    rc_slot rc[RC_SLOTS];
    int rc_n;             /* slots used: RC_BYTES / a picture's size, 4 to RC_SLOTS */
    int rc_next, rc_end;  /* a run being decoded: the next frame, the last (-1: none) */
    re_engine_stats stats;
};

static int mlt_started = 0;

static unsigned now_cs(re_engine *e)
{
    return e->io.clock_cs(e->io.ctx);
}

static int even(double x)
{
    int n = (int) lrint(x);
    return n < 2 ? 2 : n & ~1;
}

static re_engine *setup(re_engine *e, double preview_scale);

int re_mlt_start(void)
{
    if (!mlt_started) {
#ifdef MLT_STATIC_DEFINE
        mlt_static_modules_register();
#endif
        if (!mlt_factory_init(NULL))
            return -1;
        mlt_started = 1;
    }
    return 0;
}

re_engine *re_engine_open(const char *file, double preview_scale, const re_engine_io *io)
{
    if (re_mlt_start() < 0)
        return NULL;
    re_engine *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->io = *io;
    if (e->io.sound_rate <= 0)
        e->io.sound_rate = 48000;
    if (e->io.sound_channels <= 0)
        e->io.sound_channels = 2;

    /* The project's profile comes from its file (the xml producer sets it);
       a clip's from the clip, after which the clip is opened again with it. */
    e->profile = mlt_profile_init(NULL);
    e->producer = mlt_factory_producer(e->profile, NULL, file);
    if (e->producer && !e->profile->is_explicit) {
        mlt_profile_from_producer(e->profile, e->producer);
        mlt_producer_close(e->producer);
        e->producer = mlt_factory_producer(e->profile, NULL, file);
    }
    if (!e->producer) {
        mlt_profile_close(e->profile);
        free(e);
        return NULL;
    }
    return setup(e, preview_scale);
}

re_engine *re_engine_open_producer(mlt_profile profile, mlt_producer producer, double preview_scale,
                                   const re_engine_io *io)
{
    if (!producer)
        return NULL;
    re_engine *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->io = *io;
    if (e->io.sound_rate <= 0)
        e->io.sound_rate = 48000;
    if (e->io.sound_channels <= 0)
        e->io.sound_channels = 2;
    e->profile = profile;
    e->producer = producer;
    e->borrowed = 1;
    return setup(e, preview_scale);
}

void re_engine_refresh(re_engine *e)
{
    e->length = mlt_producer_get_length(e->producer);
    for (int i = 0; i < RC_SLOTS; i++)
        e->rc[i].pos = -1; /* (what was kept may have changed) */
    e->rc_end = -1;
    int at = re_engine_position(e);
    if (e->speed == 0)
        re_engine_seek(e, at < 0 ? 0 : at); /* the picture again, as it is now */
    else if (at >= e->length)
        re_engine_seek(e, e->length - 1);
}

static re_engine *setup(re_engine *e, double preview_scale)
{
    e->consumer = mlt_factory_consumer(e->profile, "null", NULL);
    if (!e->consumer) {
        re_engine_close(e);
        return NULL;
    }
    mlt_consumer_connect(e->consumer, MLT_PRODUCER_SERVICE(e->producer));
    /* a gap in a track (a blank) is black, not MLT's checkerboard: the test
       card a started consumer would make from its test_card property (this
       one is never started) */
    {
        mlt_producer black = mlt_factory_producer(e->profile, NULL, "color:black");
        if (black) {
            mlt_properties_set(MLT_PRODUCER_PROPERTIES(black), "eof", "loop");
            mlt_properties_set_data(MLT_CONSUMER_PROPERTIES(e->consumer), "test_card_producer", black, 0,
                                    (mlt_destructor) mlt_producer_close, NULL);
        }
    }

    e->fps = mlt_profile_fps(e->profile);
    e->length = mlt_producer_get_length(e->producer);
    if (preview_scale <= 0 || preview_scale > 1)
        preview_scale = 1;
    e->width = even(e->profile->width * preview_scale);
    e->height = even(e->profile->height * preview_scale);
    e->shown = -1;
    e->seek_to = 0;
    e->last_fetched = -2;
    {
        size_t bytes = (size_t) e->width * e->height * 3 / 2;
        int n = (int) (RC_BYTES / (bytes ? bytes : 1));
        e->rc_n = n < 4 ? 4 : n > RC_SLOTS ? RC_SLOTS : n;
        for (int i = 0; i < RC_SLOTS; i++)
            e->rc[i].pos = -1;
        e->rc_end = -1;
    }
    return e;
}

void re_engine_close(re_engine *e)
{
    if (!e)
        return;
    if (e->consumer)
        mlt_consumer_close(e->consumer);
    if (e->producer && !e->borrowed)
        mlt_producer_close(e->producer);
    if (e->profile && !e->borrowed)
        mlt_profile_close(e->profile);
    for (int i = 0; i < RC_SLOTS; i++)
        free(e->rc[i].buf);
    free(e);
}

int re_engine_length(re_engine *e)
{
    return e->length;
}

double re_engine_fps(re_engine *e)
{
    return e->fps;
}

int re_engine_width(re_engine *e)
{
    return e->width;
}

int re_engine_height(re_engine *e)
{
    return e->height;
}

int re_engine_position(re_engine *e)
{
    return e->seek_to >= 0 ? e->seek_to : e->shown;
}

double re_engine_speed(re_engine *e)
{
    return e->speed;
}

void re_engine_get_stats(re_engine *e, re_engine_stats *stats)
{
    *stats = e->stats;
}

static int clamp_position(re_engine *e, int position)
{
    if (position < 0)
        return 0;
    if (position >= e->length)
        return e->length - 1;
    return position;
}

void re_engine_play(re_engine *e, double speed)
{
    if (speed == e->speed)
        return;
    int from = re_engine_position(e);
    e->speed = speed;
    e->base_position = from < 0 ? 0 : from;
    e->base_clock = now_cs(e);
    e->rc_end = -1; /* (a run half decoded: not finished) */
    e->audio_next = e->base_position + 1;
    e->snd_last_q = -1;
}

void re_engine_seek(re_engine *e, int position)
{
    e->seek_to = clamp_position(e, position);
    e->base_position = e->seek_to;
    e->base_clock = now_cs(e);
    e->rc_end = -1;
    e->audio_next = e->seek_to + 1;
    e->snd_last_q = -1;
}

static mlt_frame frame_at(re_engine *e, int position);
static void take_sound(re_engine *e, mlt_frame frame, int position);

/* Sound clock: tops up the sound (a few frames' a poll) to SOUND_AHEAD,
   and steers the picture clock towards what's being heard. StreamManager
   says what's been played a block (46 ms) at a time, so the clock is moved
   only when the reading changes, a quarter of the way (more than 0.1 s
   out, the whole way): pictures stay evenly spaced (as reelcore does). */
static void sound_tick(re_engine *e, double clock_due)
{
    int target = (int) floor(clock_due);
    if (e->audio_next < target) /* (the picture went on without it: a stall) */
        e->audio_next = target + 1;
    /* (not further ahead of the picture than that: a picture that can't keep
       up holds the sound back with it) */
    int limit = e->shown >= 0 ? e->shown + (int) (SOUND_AHEAD * e->fps) + SOUND_TOPUP + 1 : e->length;
    for (int n = 0; n < SOUND_TOPUP && e->audio_next < e->length && e->audio_next < limit
                    && e->io.sound_queued(e->io.ctx) < SOUND_AHEAD; n++) {
        mlt_frame frame = frame_at(e, e->audio_next);
        if (frame) {
            take_sound(e, frame, e->audio_next);
            mlt_frame_close(frame);
        }
        e->audio_next++;
    }
    double q = e->io.sound_queued(e->io.ctx);
    if (q > 0 && q != e->snd_last_q) {
        double heard = e->audio_next - q * e->fps, err = heard - clock_due;
        e->base_position += fabs(err) > 0.1 * e->fps ? err : 0.25 * err;
        e->stats.clock_moves++;
    }
    e->snd_last_q = q;
}

/* The frame's sound, at normal speed only. */
/* The sound taken ahead of the picture, the clock (io.sound_queued) */
static int sound_clock(re_engine *e)
{
    return e->io.sound && e->io.sound_queued && e->speed == 1.0;
}

static void take_sound(re_engine *e, mlt_frame frame, int position)
{
    if (!e->io.sound || e->speed != 1.0)
        return;
    mlt_audio_format format = mlt_audio_s16;
    int frequency = e->io.sound_rate, channels = e->io.sound_channels;
    int samples = mlt_audio_calculate_frame_samples(e->fps, frequency, position);
    void *buffer = NULL;
    if (!mlt_frame_get_audio(frame, &buffer, &format, &frequency, &channels, &samples) && buffer
        && format == mlt_audio_s16 && frequency == e->io.sound_rate
        && channels == e->io.sound_channels && samples > 0) {
        e->io.sound(e->io.ctx, buffer, samples);
        e->stats.sound_frames += samples;
    }
}

static mlt_frame frame_at(re_engine *e, int position)
{
    e->stats.fetched++;
    if (position != e->last_fetched + 1)
        e->stats.jumps++;
    e->last_fetched = position;
    mlt_producer_seek(e->producer, position);
    return mlt_consumer_get_frame(e->consumer);
}

/* The frame's picture as Reel Edit shows it: yuv420p at the preview size.
   0, or -1 when there's none. */
/* A clip's own pictures up to this size are shown as they are (the display
   fits them, the overlay in hardware); bigger ones are scaled by MLT */
#define NATIVE_MAX (1920L * 1088 * 11 / 10)

/* The frame's picture. native: a clip's picture at its own size, when it's
   no bigger than NATIVE_MAX (else, or not native, the profile's size, the
   preview scale's). Either way in the clip's own colours.

   MLT would fit a clip of another size or shape into the profile's: asked
   for yuv420p, its resize filter asks for yuv422 (for the bars), the
   producer converts, the swscale filter scales (in yuv422), the bars are
   added, and it's converted back; a BT.709 clip in a BT.601 (SD-sized)
   profile was converted through RGB as well. Profiled on Linux, 1080p60
   H.264 in a 1280x544 project: those conversions took more than the
   decoding (2.5 pictures decoded a frame); on the Pi, with the VideoCore
   decoding, nearly all the time. The display fits pictures of any size
   and shows either colour space, so none of that is needed to show it. */
static int picture_of(re_engine *e, mlt_frame frame, int position, re_picture *p, int native)
{
    mlt_image_format format = mlt_image_yuv420p;
    int w = e->width, h = e->height;
    uint8_t *image = NULL;
    mlt_properties props = MLT_FRAME_PROPERTIES(frame);
    mlt_properties_set_int(props, "consumer.source_colours", 1); /* (patches/mlt 0006) */
    int mw = mlt_properties_get_int(props, "meta.media.width"), mh = mlt_properties_get_int(props, "meta.media.height");
    if (native && mw > 0 && mh > 0 && (long) mw * mh <= NATIVE_MAX)
        mlt_properties_set(props, "consumer.rescale", "none");
    if (mlt_frame_get_image(frame, &image, &format, &w, &h, 0) || !image || format != mlt_image_yuv420p)
        return -1;
    double sar = mlt_frame_get_aspect_ratio(frame);
    p->dar = (sar > 0 ? sar : 1.0) * w / h;
    p->planes[0] = image;
    p->planes[1] = image + w * h;
    p->planes[2] = image + w * h + (w / 2) * (h / 2);
    p->strides[0] = w;
    p->strides[1] = p->strides[2] = w / 2;
    p->width = w;
    p->height = h;
    p->position = position;
    int cs = mlt_properties_get_int(props, "colorspace");
    p->colorspace = cs == 601 || (cs != 709 && h <= 576) ? 601 : 709; /* (unsaid: SD is 601) */
    p->full_range = mlt_properties_get_int(props, "full_range");
    return 0;
}

/* Draw a frame: its picture to show(), and its sound. */
static void render(re_engine *e, int position)
{
    mlt_frame frame = frame_at(e, position);
    if (!frame)
        return;
    re_picture p;
    if (!picture_of(e, frame, position, &p, 1))
        e->io.show(e->io.ctx, &p);
    if (!sound_clock(e))
        take_sound(e, frame, position);
    mlt_frame_close(frame);
    e->shown = position;
    e->stats.rendered++;
}

/* A frame left out: only its sound is taken. */
static void skip(re_engine *e, int position)
{
    mlt_frame frame = frame_at(e, position);
    if (frame) {
        take_sound(e, frame, position);
        mlt_frame_close(frame);
    }
    e->stats.skipped++;
}

/* Frames from one picture to the next: 1, or the speed when faster */
static int step_of(re_engine *e)
{
    double s = fabs(e->speed);
    return s >= 2 ? (int) s : 1;
}

/* ---- going backwards: runs of frames kept --------------------------------- */

static rc_slot *rc_find(re_engine *e, int position)
{
    for (int i = 0; i < e->rc_n; i++)
        if (e->rc[i].pos == position)
            return &e->rc[i];
    return NULL;
}

/* Decodes the next frame of the run and keeps it (in the slot of the
   frame furthest from it). */
static void rc_fill_one(re_engine *e)
{
    int position = e->rc_next++;
    size_t bytes = (size_t) e->width * e->height * 3 / 2;
    rc_slot *slot = rc_find(e, position);
    if (!slot) {
        int far = 0;
        for (int i = 0; i < e->rc_n; i++) {
            if (e->rc[i].pos < 0) {
                far = i;
                break;
            }
            if (abs(e->rc[i].pos - position) > abs(e->rc[far].pos - position))
                far = i;
        }
        slot = &e->rc[far];
        mlt_frame frame = frame_at(e, position);
        re_picture p;
        if (!frame)
            return;
        if (!picture_of(e, frame, position, &p, 0) && p.width == e->width && p.height == e->height
            && (slot->buf || (slot->buf = malloc(bytes)))) {
            memcpy(slot->buf, p.planes[0], bytes);
            slot->pos = position;
            slot->colorspace = p.colorspace;
            slot->full_range = p.full_range;
        }
        mlt_frame_close(frame);
    }
    if (e->rc_next > e->rc_end)
        e->rc_end = -1; /* the run is done */
}

static void rc_show(re_engine *e, rc_slot *slot)
{
    re_picture p;
    int w = e->width, h = e->height;
    p.planes[0] = slot->buf;
    p.planes[1] = slot->buf + w * h;
    p.planes[2] = slot->buf + w * h + (w / 2) * (h / 2);
    p.strides[0] = w;
    p.strides[1] = p.strides[2] = w / 2;
    p.width = w;
    p.height = h;
    p.position = slot->pos;
    p.colorspace = slot->colorspace;
    p.full_range = slot->full_range;
    p.dar = mlt_profile_dar(e->profile);
    e->io.show(e->io.ctx, &p);
    e->shown = slot->pos;
    e->stats.rendered++;
}

/* Centiseconds until the next frame is due. */
static int wait_for_next(re_engine *e)
{
    int next = e->shown + (e->speed > 0 ? 1 : -1) * step_of(e);
    double due = (next - e->base_position) / (e->fps * e->speed) * 100.0;
    double elapsed = (double) (int) (now_cs(e) - e->base_clock);
    double wait = ceil(due - elapsed);
    return wait < 0 ? 0 : (int) wait;
}

int re_engine_busy(re_engine *e)
{
    return e->seek_to >= 0 || e->speed != 0;
}

int re_engine_poll(re_engine *e)
{
    if (e->seek_to >= 0) {
        int position = e->seek_to;
        e->seek_to = -1;
        render(e, position);
        if (e->speed == 0)
            return -1;
        e->base_position = position;
        e->base_clock = now_cs(e);
        return wait_for_next(e);
    }
    if (e->speed == 0)
        return -1;

    if (e->speed < 0 && e->rc_end >= 0) {
        /* decoding a run to go back through: a frame a poll, the clock
           waiting; then the last of it (the one that was due) shown */
        int last = e->rc_end;
        rc_fill_one(e);
        if (e->rc_end < 0) {
            rc_slot *s = rc_find(e, last);
            if (s)
                rc_show(e, s);
            else
                render(e, last);
            e->base_position = last;
            e->base_clock = now_cs(e);
            return wait_for_next(e);
        }
        return 0;
    }

    double elapsed = (double) (int) (now_cs(e) - e->base_clock);
    double due = e->base_position + elapsed / 100.0 * e->fps * e->speed;
    if (sound_clock(e)) {
        sound_tick(e, due);
        due = e->base_position + elapsed / 100.0 * e->fps * e->speed;
    }
    /* (a hair over, so a frame due exactly now isn't missed by rounding;
       backwards, the frame due is the one not yet passed: rounded up) */
    int target = e->speed > 0 ? (int) floor(due + 1e-6) : (int) ceil(due - 1e-6);
    if (target >= e->length || target < 0) {
        /* the end (or the start, backwards): the last frame, then stop */
        target = clamp_position(e, target);
        if (target != e->shown)
            render(e, target);
        e->speed = 0;
        return -1;
    }
    if (e->shown >= 0 && abs(target - e->shown) < step_of(e))
        return wait_for_next(e);
    if (step_of(e) > 1) /* (shown on the speed's steps: 4, 8, 12 ... at 4x) */
        target = e->shown + (target - e->shown) / step_of(e) * step_of(e);

    if (e->speed < 0 && e->shown >= 0) {
        rc_slot *s = rc_find(e, target);
        if (s) {
            int gap = e->shown - target - 1;
            e->stats.skipped += gap > 0 ? gap : 0;
            rc_show(e, s);
            return wait_for_next(e);
        }
        /* a run up to it, decoded forwards from its start */
        int from = target - e->rc_n + 1;
        e->rc_next = from < 0 ? 0 : from;
        e->rc_end = target;
        e->stats.reverse_fills++;
        return re_engine_poll(e);
    }

    int gap = e->speed > 0 ? target - e->shown - 1 : e->shown - target - 1, slowed = 0;
    if (e->shown >= 0 && gap > MAX_LEFT_OUT && gap <= e->fps && e->speed == 1.0) {
        /* can't keep up: on a little, and the clock waits (see the top) */
        target = e->shown + 1 + MAX_LEFT_OUT;
        gap = MAX_LEFT_OUT;
        slowed = 1;
        e->stats.slowed++;
    }
    if (e->shown >= 0 && gap > 0) {
        if (gap > e->fps) {
            /* over a second behind: start the clock again from here */
            e->stats.skipped += gap;
            e->stats.resyncs++;
        } else if (e->speed == 1.0 && e->io.sound && !sound_clock(e)) {
            for (int p = e->shown + 1; p < target; p++)
                skip(e, p);
        } else {
            e->stats.skipped += gap;
        }
    }
    render(e, target);
    if (gap > e->fps || slowed) {
        e->base_position = target;
        e->base_clock = now_cs(e);
    }
    return wait_for_next(e);
}
