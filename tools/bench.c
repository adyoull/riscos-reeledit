/* bench - how long MLT takes to make each picture of a clip or project, as
 * Reel Edit's player asks for them (yuv420p at the profile's size, one
 * after another, no threads). For profiling on Linux, and on RISC OS.
 *
 *   bench FILE [FRAMES] [SCALE]
 * SCALE: the preview size against the project's (default 1; 0.5 half).
 * It prints which decoder each clip got (MLT's info messages), then the
 * time a picture took on average and at worst, and the first picture
 * separately (opening and the first decode).
 */
#include <framework/mlt.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: bench FILE [FRAMES] [SCALE]\n");
        return 2;
    }
#ifdef MLT_STATIC_DEFINE
    mlt_static_modules_register();
#endif
    mlt_factory_init(NULL);
    mlt_log_set_level(MLT_LOG_INFO);
    mlt_profile profile = mlt_profile_init(NULL);
    mlt_producer producer = mlt_factory_producer(profile, NULL, argv[1]);
    if (producer && !profile->is_explicit) {
        mlt_profile_from_producer(profile, producer);
        mlt_producer_close(producer);
        producer = mlt_factory_producer(profile, NULL, argv[1]);
    }
    if (!producer) {
        fprintf(stderr, "bench: can't open %s\n", argv[1]);
        return 1;
    }
    mlt_consumer consumer = mlt_factory_consumer(profile, "null", NULL);
    mlt_consumer_connect(consumer, MLT_PRODUCER_SERVICE(producer));
    int frames = argc > 2 ? atoi(argv[2]) : mlt_producer_get_length(producer);
    double scale = argc > 3 ? atof(argv[3]) : 1.0;
    int w0 = ((int) (profile->width * scale)) & ~1, h0 = ((int) (profile->height * scale)) & ~1;
    if (frames > mlt_producer_get_length(producer))
        frames = mlt_producer_get_length(producer);
    double start = now(), worst = 0, first = 0;
    for (int i = 0; i < frames; i++) {
        double t0 = now();
        mlt_producer_seek(producer, i);
        mlt_frame frame = mlt_consumer_get_frame(consumer);
        mlt_image_format format = mlt_image_yuv420p;
        int w = w0, h = h0;
        uint8_t *image = NULL;
        mlt_frame_get_image(frame, &image, &format, &w, &h, 0);
        if (i == 0)
            printf("bench: %s, %d frames, %dx%d %s\n", argv[1], frames, w, h, mlt_image_format_name(format));
        mlt_frame_close(frame);
        double t = now() - t0;
        if (i == 0) {
            first = t;
            start = now();
        } else if (t > worst) {
            worst = t;
        }
    }
    double total = now() - start;
    printf("bench: the first picture %.0f ms; then %.1f ms a picture (%.1f a second), the slowest %.1f ms\n",
           first * 1000, total * 1000 / (frames > 1 ? frames - 1 : 1), (frames - 1) / total, worst * 1000);
    mlt_consumer_close(consumer);
    mlt_producer_close(producer);
    return 0;
}
