/* engine_test - src/engine.c, Reel Edit's player, driven as the Wimp would
 * drive it, against a clock this test controls.
 *
 * Usage: engine_test project.mlt
 * project.mlt: two 75 frame clips at 25 fps with a 25 frame dissolve
 * (frames 50 to 74), 125 frames, 1280x720.
 *
 * The "Wimp" here calls re_engine_poll, then moves the clock on by the wait
 * it was given (Wimp_PollIdle), plus however long showing a picture took.
 */
#include "engine.h"
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
}

static void sound(void *ctx, const int16_t *samples, int frames)
{
    (void) ctx;
    (void) samples;
    sound_frames += frames;
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
    CHECK(last_w == 1280 && last_h == 720, "picture %dx%d", last_w, last_h);

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
    CHECK(st.jumps - before.jumps <= 1 + nshown / 16, "I: %d seeks for %d pictures backwards", st.jumps - before.jumps,
          nshown);
    CHECK(st.reverse_fills > before.reverse_fills, "I: no runs decoded");

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
    CHECK(last_w == 640 && last_h == 360, "H: picture %dx%d", last_w, last_h);
    re_engine_close(e);

    printf("engine_test: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
