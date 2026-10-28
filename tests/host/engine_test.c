/* engine_test - src/engine.c, ReelEdit's player, driven as the Wimp would
 * drive it, against a clock this test controls.
 *
 * Usage: engine_test project.mlt
 * project.mlt: two 75 frame clips at 25 fps with a 25 frame dissolve
 * (frames 50 to 74), 125 frames, 1280x720.
 *
 * The "Wimp" here calls re_engine_poll, then moves the clock on by the wait
 * it was given (Wimp_PollIdle), plus however long showing a picture took.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "engine.h"
#include <framework/mlt.h>
#include <libavutil/log.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, ...)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                    \
            fprintf(stderr, __VA_ARGS__);                                           \
            fputc('\n', stderr);                                                    \
            failures++;                                                             \
        }                                                                           \
    } while (0)

static unsigned fake_clock = 1000;
static int render_cost = 0;          /* centiseconds each picture takes to show */
/* tenths of a centisecond each frame takes to decode: MLT decodes every
   frame up to the one asked for, so a picture costs this for each frame
   since the last (L) */
static int decode_tenths = 0, decode_from = -1, decode_rem = 0, max_step = 0;
static int shown[4096], nshown = 0;
static int last_u = -1, last_w = 0, last_h = 0;
static long sound_frames = 0;

static void show(void *ctx, const re_picture *p)
{
    (void) ctx;
    if (nshown < 4096)
        shown[nshown++] = p->position;
    last_w = p->width;
    last_h = p->height;
    last_u = p->planes[1][(p->height / 4) * p->strides[1] + p->width / 4];
    fake_clock += render_cost;
    if (decode_tenths && decode_from >= 0 && p->position > decode_from) {
        int step = p->position - decode_from, t = step * decode_tenths + decode_rem;
        fake_clock += t / 10;
        decode_rem = t % 10;
        if (step > max_step)
            max_step = step;
    }
    decode_from = p->position;
}

static void sound(void *ctx, const int16_t *samples, int frames)
{
    (void) ctx;
    (void) samples;
    sound_frames += frames;
}

/* K's sound device: playing from snd_start at snd_rate x real time, in
   46 ms blocks as StreamManager reports it */
static unsigned snd_start;
static double snd_rate;
static double fake_queued(void *ctx)
{
    (void) ctx;
    double given = sound_frames / 48000.0;
    double played = snd_rate * (int) ((fake_clock - snd_start) / 4.6) * 0.046;
    if (played > given)
        played = given;
    return given - played;
}

static unsigned clock_cs(void *ctx)
{
    (void) ctx;
    return fake_clock;
}

/* The Wimp: poll for a while (centiseconds of the fake clock). Returns the
   number of polls; fails if a poll ever asks for a negative wait other
   than -1, or if two polls in a row both render nothing and ask for 0. */
static int run(re_engine *e, int centiseconds)
{
    unsigned end = fake_clock + centiseconds;
    int polls = 0, idle_zero = 0;
    while ((int) (end - fake_clock) > 0 && polls < 100000) {
        int before = nshown;
        re_engine_stats a, b;
        re_engine_get_stats(e, &a);
        int wait = re_engine_poll(e);
        re_engine_get_stats(e, &b);
        polls++;
        if (wait < 0)
            break;
        CHECK(wait <= 100, "asked to wait %d cs while playing", wait);
        if (wait == 0 && nshown == before && b.fetched == a.fetched) /* (no picture, nothing decoded) */
            idle_zero++;
        else
            idle_zero = 0;
        CHECK(idle_zero < 3, "polled with nothing to do and asked for no wait, %d times", idle_zero);
        fake_clock += wait;
    }
    return polls;
}

static int collected;
static char lines[2048];
static void collect(const char *line)
{
    collected++;
    if (strlen(lines) + strlen(line) + 2 < sizeof(lines)) {
        strcat(lines, line);
        strcat(lines, "|");
    }
}

static void reset_log(void)
{
    nshown = 0;
    sound_frames = 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: engine_test project.mlt\n");
        return 2;
    }
    re_engine_io io = {show, sound, clock_cs, NULL, 48000, 2};
    re_engine *e = re_engine_open(argv[1], 1.0, &io);
    CHECK(e != NULL, "can't open %s", argv[1]);
    if (!e)
        return 1;
    CHECK(re_engine_length(e) == 125, "length %d", re_engine_length(e));
    CHECK(fabs(re_engine_fps(e) - 25) < 0.01, "fps %f", re_engine_fps(e));
    re_engine_stats st;

    /* stopped: the first picture once, then nothing to do */
    CHECK(re_engine_poll(e) == -1, "stopped, but asked to be polled again");
    CHECK(nshown == 1 && shown[0] == 0, "first poll showed %d pictures", nshown);
    CHECK(re_engine_poll(e) == -1 && nshown == 1, "stopped and drawn, but drew again");
    /* (the clips' own size: 320x180 in a 1280x720 profile, fitted by the
       display, not by MLT; project_test has more of this) */
    CHECK(last_w == 320 && last_h == 180, "picture %dx%d", last_w, last_h);

    /* A: playing on a fast machine: every frame, on time, with its sound */
    reset_log();
    re_engine_play(e, 1.0);
    run(e, 400); /* 4 seconds: frames 1 to 100 */
    CHECK(nshown >= 99 && nshown <= 101, "A: %d pictures in 4 s at 25 fps", nshown);
    for (int i = 1; i < nshown; i++)
        CHECK(shown[i] == shown[i - 1] + 1, "A: frame %d after %d", shown[i], shown[i - 1]);
    re_engine_get_stats(e, &st);
    CHECK(st.skipped == 0, "A: %d pictures left out", st.skipped);
    CHECK(sound_frames == 1920L * nshown, "A: %ld sample frames for %d frames", sound_frames, nshown);

    /* B: a slow machine (each picture takes 6 cs, a frame lasts 4): the
       pictures keep to the clock, the ones in between are left out, and the
       sound has every frame */
    re_engine_play(e, 0);
    re_engine_seek(e, 0);
    re_engine_poll(e);
    reset_log();
    render_cost = 6;
    re_engine_get_stats(e, &st);
    int skipped_before = st.skipped;
    unsigned start = fake_clock;
    re_engine_play(e, 1.0);
    run(e, 300);
    int last = shown[nshown - 1];
    double expected = (fake_clock - start) * 0.25;
    CHECK(fabs(last - expected) <= 2.5, "B: at %.0f cs showing frame %d, not about %.0f",
          (double) (fake_clock - start), last, expected);
    re_engine_get_stats(e, &st);
    CHECK(st.skipped - skipped_before > 0, "B: nothing left out on a slow machine");
    CHECK(nshown < last, "B: %d pictures shown for %d frames", nshown, last);
    CHECK(sound_frames == 1920L * last, "B: %ld sample frames for frames 1 to %d (%ld)",
          sound_frames, last, 1920L * last);
    render_cost = 0;

    /* C: the desktop held up for 3 s by another task: the clock is moved
       on, not 75 frames fetched in one poll */
    re_engine_play(e, 0);
    re_engine_seek(e, 0);
    re_engine_poll(e);
    re_engine_play(e, 1.0);
    run(e, 40);
    re_engine_get_stats(e, &st);
    int resyncs = st.resyncs, rendered = st.rendered;
    reset_log();
    fake_clock += 300;
    re_engine_poll(e);
    re_engine_get_stats(e, &st);
    CHECK(st.resyncs == resyncs + 1, "C: no resync after a 3 s stall");
    CHECK(st.rendered == rendered + 1 && sound_frames == 1920, "C: %d pictures and %ld sample frames in one poll",
          st.rendered - rendered, sound_frames);
    run(e, 40);
    for (int i = 1; i < nshown; i++)
        CHECK(shown[i] == shown[i - 1] + 1, "C: frame %d after %d once going again", shown[i], shown[i - 1]);

    /* D: seeks before a poll replace each other: one picture, the last */
    re_engine_play(e, 0);
    re_engine_poll(e);
    reset_log();
    re_engine_seek(e, 10);
    re_engine_seek(e, 20);
    re_engine_seek(e, 30);
    CHECK(re_engine_position(e) == 30, "D: position %d", re_engine_position(e));
    CHECK(re_engine_poll(e) == -1, "D: stopped, but asked to be polled again");
    CHECK(nshown == 1 && shown[0] == 30, "D: %d pictures, the first %d", nshown, nshown ? shown[0] : -1);
    CHECK(re_engine_poll(e) == -1 && nshown == 1, "D: drew again");

    /* E: the dissolve: frame 62 is a blend of frames 10 and 100 */
    re_engine_seek(e, 10);
    re_engine_poll(e);
    int u_a = last_u;
    re_engine_seek(e, 100);
    re_engine_poll(e);
    int u_b = last_u;
    re_engine_seek(e, 62);
    re_engine_poll(e);
    CHECK(abs(u_a - u_b) > 40, "E: the clips' U %d and %d are too alike", u_a, u_b);
    CHECK(last_u > (u_a < u_b ? u_a : u_b) + 10 && last_u < (u_a > u_b ? u_a : u_b) - 10,
          "E: frame 62's U %d is not between %d and %d", last_u, u_a, u_b);

    /* F: playing to the end stops on the last frame */
    reset_log();
    re_engine_seek(e, 120);
    re_engine_play(e, 1.0);
    run(e, 100);
    CHECK(re_engine_speed(e) == 0, "F: still playing past the end");
    CHECK(nshown > 0 && shown[nshown - 1] == 124, "F: stopped at %d", nshown ? shown[nshown - 1] : -1);
    CHECK(re_engine_poll(e) == -1, "F: stopped, but asked to be polled again");

    /* G: backwards at double speed: frames go down, about 50 a second, silent */
    reset_log();
    re_engine_seek(e, 110);
    re_engine_poll(e);
    reset_log();
    re_engine_play(e, -2.0);
    run(e, 100);
    CHECK(nshown > 0 && abs(shown[nshown - 1] - 60) <= 2, "G: after 1 s at -2, at frame %d, not about 60",
          nshown ? shown[nshown - 1] : -1);
    for (int i = 1; i < nshown; i++)
        CHECK(shown[i] < shown[i - 1], "G: frame %d after %d going backwards", shown[i], shown[i - 1]);
    CHECK(sound_frames == 0, "G: sound at -2");

    /* I: backwards at normal speed: every frame, going down, and MLT asked
       for runs of frames forwards (one a poll), not each frame on its own
       (which in long-GOP video decodes from a keyframe each time) */
    re_engine_play(e, 0);
    re_engine_seek(e, 120);
    re_engine_poll(e);
    reset_log();
    re_engine_get_stats(e, &st);
    re_engine_stats before = st;
    re_engine_play(e, -1.0);
    {
        unsigned end = fake_clock + 200;
        int polls = 0;
        while ((int) (end - fake_clock) > 0 && polls++ < 100000) {
            re_engine_stats a, b;
            re_engine_get_stats(e, &a);
            int wait = re_engine_poll(e);
            re_engine_get_stats(e, &b);
            CHECK(b.fetched - a.fetched <= 1, "I: %d frames asked of MLT in one poll", b.fetched - a.fetched);
            if (wait < 0)
                break;
            fake_clock += wait + 1; /* (each poll takes a moment) */
        }
    }
    re_engine_get_stats(e, &st);
    CHECK(nshown >= 25, "I: %d pictures in 2 s backwards (each poll a centisecond here)", nshown);
    for (int i = 1; i < nshown; i++)
        CHECK(shown[i] == shown[i - 1] - 1, "I: frame %d after %d going backwards", shown[i], shown[i - 1]);
    CHECK(st.jumps - before.jumps <= 2 /* (runs as long as 48 MB of these small pictures allow: 64) */, "I: %d seeks for %d pictures backwards", st.jumps - before.jumps,
          nshown);
    CHECK(st.reverse_fills > before.reverse_fills, "I: no runs decoded");
    CHECK(last_w == 320 && last_h == 180, "I: backwards, pictures %dx%d, not the clips' own 320x180", last_w, last_h);

    /* K: the sound as the clock (io.sound_queued): a sound device playing 2%
       slow; the pictures follow what's heard, not the system clock, and
       the sound is kept about 0.4 s ahead */
    re_engine_close(e);
    re_engine_io io2 = io;
    io2.sound_queued = fake_queued;
    e = re_engine_open(argv[1], 1.0, &io2);
    re_engine_poll(e);
    reset_log();
    snd_start = fake_clock;
    snd_rate = 0.98;
    re_engine_play(e, 1.0);
    run(e, 300);
    {
        double heard = 1 + (fake_clock - snd_start) / 100.0 * snd_rate * 25; /* (frame 1's sound first) */
        int last = nshown ? shown[nshown - 1] : -1;
        CHECK(fabs(last - heard) <= 2, "K: after 3 s showing frame %d, the sound heard at %.1f", last, heard);
        CHECK(last < 74, "K: frame %d after 3 s: following the system clock, not the slow sound", last);
        double ahead = sound_frames / 1920.0 - (last - 1);
        CHECK(ahead > 6 && ahead < 14, "K: sound %.1f frames ahead of the picture, not about 10", ahead);
        re_engine_get_stats(e, &st);
        CHECK(st.clock_moves > 10, "K: the clock steered %d times", st.clock_moves);
    }
    snd_rate = 0;
    re_engine_play(e, 0);

    /* L: a clip that decodes slower than it plays (each frame 6 cs, a frame
       lasting 4), with the sound the clock and without: not chased (each
       picture further on, so slower, so further on...), but played slower,
       every poll short, the clock (and the sound) waiting for the picture:
       never a second behind (a resync, a long poll decoding through) */
    for (int with_sound = 1; with_sound >= 0; with_sound--) {
        re_engine_close(e);
        e = re_engine_open(argv[1], 1.0, with_sound ? &io2 : &io);
        re_engine_poll(e);
        reset_log();
        re_engine_get_stats(e, &st);
        int slowed = st.slowed, resyncs = st.resyncs;
        decode_from = 0;
        decode_tenths = 60;
        max_step = 0;
        snd_start = fake_clock;
        snd_rate = 1;
        re_engine_play(e, 1.0);
        run(e, 480);
        int last = nshown ? shown[nshown - 1] : -1;
        re_engine_get_stats(e, &st);
        CHECK(max_step <= 3, "L%d: a picture %d frames on from the one before (decoded through: a long poll)",
              with_sound, max_step);
        CHECK(last >= 65 && last <= 90, "L%d: at frame %d after 4.8 s, not about 80", with_sound, last);
        CHECK(nshown >= 22, "L%d: %d pictures in 4.8 s", with_sound, nshown);
        CHECK(st.slowed > slowed, "L%d: never slowed", with_sound);
        CHECK(st.resyncs == resyncs, "L%d: %d resyncs (the clock ran on without the picture)", with_sound,
              st.resyncs - resyncs);
        decode_tenths = 0;
        decode_from = -1;
        snd_rate = 0;
        re_engine_play(e, 0);
    }
    re_engine_close(e);
    e = re_engine_open(argv[1], 1.0, &io2);

    /* J: four times as fast: every fourth frame, about 25 pictures a second */
    re_engine_play(e, 0);
    re_engine_seek(e, 0);
    re_engine_poll(e);
    reset_log();
    re_engine_play(e, 4.0);
    run(e, 100);
    CHECK(nshown >= 20 && nshown <= 27, "J: %d pictures in 1 s at 4x", nshown);
    for (int i = 1; i < nshown; i++)
        CHECK(shown[i] - shown[i - 1] == 4, "J: frame %d after %d at 4x", shown[i], shown[i - 1]);
    CHECK(nshown > 0 && abs(shown[nshown - 1] - 100) <= 4, "J: after 1 s at 4x, at frame %d",
          nshown ? shown[nshown - 1] : -1);
    re_engine_close(e);

    /* H: preview at half size */
    e = re_engine_open(argv[1], 0.5, &io);
    CHECK(e && re_engine_width(e) == 640 && re_engine_height(e) == 360, "H: preview %dx%d",
          e ? re_engine_width(e) : 0, e ? re_engine_height(e) : 0);
    re_engine_poll(e);
    CHECK(last_w == 320 && last_h == 180, "H: picture %dx%d (a clip smaller than the preview: as it is)", last_w,
          last_h);
    re_engine_close(e);

    /* M: FFmpeg's and MLT's messages into the log, as whole lines, repeats
       counted, below information left out */
    re_engine_messages(collect);
    av_log(NULL, AV_LOG_INFO, "The format change came %d cs ", 3);
    av_log(NULL, AV_LOG_INFO, "after the first access unit\n");
    av_log(NULL, AV_LOG_WARNING, "No format change\n");
    av_log(NULL, AV_LOG_WARNING, "No format change\n");
    av_log(NULL, AV_LOG_WARNING, "No format change\n");
    av_log(NULL, AV_LOG_VERBOSE, "The format change came 1 cs\n");
    av_log(NULL, AV_LOG_DEBUG, "chatter\n");
    mlt_log(NULL, MLT_LOG_INFO, "decoder %s handed over\n", "h264_vchiq");
    mlt_log(NULL, MLT_LOG_DEBUG, "seeking\n");
    CHECK(collected == 5 && strstr(lines, "The format change came 3 cs after the first access unit|")
              && strstr(lines, "|No format change|(the line before, 2 more times)|The format change came 1 cs|mlt: decoder h264_vchiq handed over|")
              && !strstr(lines, "chatter") && !strstr(lines, "seeking"),
          "M: %d lines: %s", collected, lines);

    printf("engine_test: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
