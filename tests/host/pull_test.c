/* pull_test - a project played the way ReelEdit plays it, with no threads.
 *
 * Usage: pull_test project.mlt a.mp4 b.mp4
 * project.mlt: a.mp4 (75 frames) then b.mp4 (75 frames), with a 25-frame
 * luma crossfade between them (frames 50 to 74 of the project).
 *
 * Checks:
 * - frames before, during and after the crossfade, in any order (each a
 *   seek): the clips' own colours outside it, a blend that moves from A to
 *   B inside it, the same picture when a position is asked for again;
 * - MLT starts no thread doing it (MLT_NO_THREADS built in): pthread_create
 *   is wrapped (-Wl,--wrap=pthread_create) and counted.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "mltt.h"
#include <pthread.h>

static int threads_created = 0;
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *arg)
{
    threads_created++;
    return __real_pthread_create(t, a, f, arg);
}

static int near(int a, int b, int tolerance)
{
    return abs(a - b) <= tolerance;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: pull_test project.mlt a.mp4 b.mp4\n");
        return 2;
    }
    int a[3], b[3], p[3];
    mltt clip;
    if (!mltt_open(&clip, "hdv_720_25p", "avformat", argv[2]))
        return 1;
    CHECK(mltt_centre(&clip, 10, a) >= 0, "no picture from %s", argv[2]);
    mltt_close(&clip);
    if (!mltt_open(&clip, "hdv_720_25p", "avformat", argv[3]))
        return 1;
    CHECK(mltt_centre(&clip, 10, b) >= 0, "no picture from %s", argv[3]);
    mltt_close(&clip);
    CHECK(!near(a[0], b[0], 10) || !near(a[1], b[1], 10), "the two clips' colours are too alike");

    mltt project;
    if (!mltt_open(&project, "hdv_720_25p", "xml", argv[1]))
        return 1;
    CHECK(mlt_producer_get_length(project.producer) == 125,
          "project length %d, not 125",
          mlt_producer_get_length(project.producer));

    /* outside the crossfade: each clip's own colour */
    static const int a_frames[] = {0, 10, 49};
    static const int b_frames[] = {75, 100, 124};
    for (int i = 0; i < 3; i++) {
        CHECK(mltt_centre(&project, a_frames[i], p) >= 0, "no picture at %d", a_frames[i]);
        CHECK(near(p[0], a[0], 2) && near(p[1], a[1], 2) && near(p[2], a[2], 2),
              "frame %d is %d,%d,%d, not clip A's %d,%d,%d",
              a_frames[i], p[0], p[1], p[2], a[0], a[1], a[2]);
        CHECK(mltt_centre(&project, b_frames[i], p) >= 0, "no picture at %d", b_frames[i]);
        CHECK(near(p[0], b[0], 2) && near(p[1], b[1], 2) && near(p[2], b[2], 2),
              "frame %d is %d,%d,%d, not clip B's %d,%d,%d",
              b_frames[i], p[0], p[1], p[2], b[0], b[1], b[2]);
    }

    /* inside it: a blend, moving steadily from A towards B (U is the
       channel that differs most between red and blue) */
    int previous_distance = abs(a[1] - b[1]) + 1, mid[3] = {0};
    for (int f = 52; f <= 72; f += 4) {
        CHECK(mltt_centre(&project, f, p) >= 0, "no picture at %d", f);
        int distance = abs(p[1] - b[1]);
        CHECK(distance < previous_distance,
              "frame %d (U %d) is no nearer clip B (U %d) than frame %d",
              f, p[1], b[1], f - 4);
        CHECK(!near(p[1], a[1], 3) && !near(p[1], b[1], 3), "frame %d (U %d) isn't a blend", f, p[1]);
        previous_distance = distance;
        if (f == 60)
            memcpy(mid, p, sizeof(mid));
    }

    /* the crossfade drawn as yuv420p (the fast path) and as yuv422 (the
       original path) is the same picture */
    for (int f = 54; f <= 70; f += 8) {
        int p420[3], p422[3];
        CHECK(mltt_centre_as(&project, f, mlt_image_yuv420p, p420) >= 0, "no yuv420p picture at %d", f);
        CHECK(mltt_centre_as(&project, f, mlt_image_yuv422, p422) >= 0, "no yuv422 picture at %d", f);
        CHECK(near(p420[0], p422[0], 1) && near(p420[1], p422[1], 1) && near(p420[2], p422[2], 1),
              "frame %d: yuv420p %d,%d,%d but yuv422 %d,%d,%d",
              f, p420[0], p420[1], p420[2], p422[0], p422[1], p422[2]);
    }

    /* random access: back into the crossfade, then forward, then back */
    int again[3];
    mltt_centre(&project, 110, p);
    CHECK(mltt_centre(&project, 60, again) >= 0, "no picture at 60 again");
    CHECK(near(again[0], mid[0], 1) && near(again[1], mid[1], 1) && near(again[2], mid[2], 1),
          "frame 60 asked for again is %d,%d,%d, not %d,%d,%d",
          again[0], again[1], again[2], mid[0], mid[1], mid[2]);
    mltt_centre(&project, 5, p);
    CHECK(near(p[1], a[1], 2), "frame 5 after a seek back is U %d, not A's %d", p[1], a[1]);

    mltt_close(&project);
    CHECK(threads_created == 0, "MLT created %d threads", threads_created);
    printf("pull_test: %s\n", mltt_failures ? "FAILED" : "passed");
    return mltt_failures != 0;
}
