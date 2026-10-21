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
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "engine.h"

#include <framework/mlt.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

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

re_engine *re_engine_open(const char *file, double preview_scale, const re_engine_io *io)
{
    if (!mlt_started) {
#ifdef MLT_STATIC_DEFINE
        mlt_static_modules_register();
#endif
        if (!mlt_factory_init(NULL))
            return NULL;
        mlt_started = 1;
    }
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
    e->consumer = mlt_factory_consumer(e->profile, "null", NULL);
    if (!e->consumer) {
        re_engine_close(e);
        return NULL;
    }
    mlt_consumer_connect(e->consumer, MLT_PRODUCER_SERVICE(e->producer));

    e->fps = mlt_profile_fps(e->profile);
    e->length = mlt_producer_get_length(e->producer);
    if (preview_scale <= 0 || preview_scale > 1)
        preview_scale = 1;
    e->width = even(e->profile->width * preview_scale);
    e->height = even(e->profile->height * preview_scale);
    e->shown = -1;
    e->seek_to = 0;
    return e;
}

void re_engine_close(re_engine *e)
{
    if (!e)
        return;
    if (e->consumer)
        mlt_consumer_close(e->consumer);
    if (e->producer)
        mlt_producer_close(e->producer);
    if (e->profile)
        mlt_profile_close(e->profile);
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
}

void re_engine_seek(re_engine *e, int position)
{
    e->seek_to = clamp_position(e, position);
    e->base_position = e->seek_to;
    e->base_clock = now_cs(e);
}

/* The frame's sound, at normal speed only. */
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
    mlt_producer_seek(e->producer, position);
    return mlt_consumer_get_frame(e->consumer);
}

/* Draw a frame: its picture to show(), and its sound. */
static void render(re_engine *e, int position)
{
    mlt_frame frame = frame_at(e, position);
    if (!frame)
        return;
    mlt_image_format format = mlt_image_yuv420p;
    int w = e->width, h = e->height;
    uint8_t *image = NULL;
    if (!mlt_frame_get_image(frame, &image, &format, &w, &h, 0) && image
        && format == mlt_image_yuv420p) {
        mlt_properties props = MLT_FRAME_PROPERTIES(frame);
        re_picture p;
        p.planes[0] = image;
        p.planes[1] = image + w * h;
        p.planes[2] = image + w * h + (w / 2) * (h / 2);
        p.strides[0] = w;
        p.strides[1] = p.strides[2] = w / 2;
        p.width = w;
        p.height = h;
        p.position = position;
        int cs = mlt_properties_get_int(props, "colorspace");
        p.colorspace = cs == 601 || (cs != 709 && h <= 576) ? 601 : 709; /* (unsaid: SD is 601) */
        p.full_range = mlt_properties_get_int(props, "full_range");
        e->io.show(e->io.ctx, &p);
    }
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

/* Centiseconds until the next frame is due. */
static int wait_for_next(re_engine *e)
{
    int next = e->shown + (e->speed > 0 ? 1 : -1);
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

    double elapsed = (double) (int) (now_cs(e) - e->base_clock);
    double due = e->base_position + elapsed / 100.0 * e->fps * e->speed;
    /* (a hair over, so a frame due exactly now isn't missed by rounding) */
    int target = (int) floor(due + 1e-6);
    if (target >= e->length || target < 0) {
        /* the end (or the start, backwards): the last frame, then stop */
        target = clamp_position(e, target);
        if (target != e->shown)
            render(e, target);
        e->speed = 0;
        return -1;
    }
    if (target == e->shown)
        return wait_for_next(e);

    int gap = e->speed > 0 ? target - e->shown - 1 : e->shown - target - 1;
    if (e->shown >= 0 && gap > 0) {
        if (gap > e->fps) {
            /* over a second behind: start the clock again from here */
            e->stats.skipped += gap;
            e->stats.resyncs++;
        } else if (e->speed == 1.0 && e->io.sound) {
            for (int p = e->shown + 1; p < target; p++)
                skip(e, p);
        } else {
            e->stats.skipped += gap;
        }
    }
    render(e, target);
    if (gap > e->fps) {
        e->base_position = target;
        e->base_clock = now_cs(e);
    }
    return wait_for_next(e);
}
