/* mltt.h - shared by the host tests: MLT driven as Reel Edit drives it on
 * RISC OS. A consumer is made and connected but never started; each frame is
 * fetched from the program's own loop (a Wimp null event, there) with
 * mlt_consumer_get_frame, after a seek, and its picture asked for as yuv420p
 * at the profile's size.
 */
#ifndef MLTT_H
#define MLTT_H
#include <framework/mlt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int mltt_failures = 0;

#define CHECK(cond, ...)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                    \
            fprintf(stderr, __VA_ARGS__);                                           \
            fputc('\n', stderr);                                                    \
            mltt_failures++;                                                        \
        }                                                                           \
    } while (0)

typedef struct
{
    mlt_profile profile;
    mlt_producer producer;
    mlt_consumer consumer;
} mltt;

/* service: "xml" for a project, "avformat" for a clip (or NULL: MLT's loader) */
static int mltt_open(mltt *t, const char *profile, const char *service, const char *resource)
{
    static int initialised = 0;
    if (!initialised) {
        mlt_static_modules_register();
        if (!mlt_factory_init(NULL)) {
            fprintf(stderr, "mlt_factory_init failed\n");
            return 0;
        }
        initialised = 1;
    }
    t->profile = mlt_profile_init(profile);
    t->producer = mlt_factory_producer(t->profile, service, resource);
    if (!t->producer) {
        fprintf(stderr, "can't open %s\n", resource);
        return 0;
    }
    t->consumer = mlt_factory_consumer(t->profile, "null", NULL);
    mlt_consumer_connect(t->consumer, MLT_PRODUCER_SERVICE(t->producer));
    return 1;
}

static void mltt_close(mltt *t)
{
    mlt_consumer_close(t->consumer);
    mlt_producer_close(t->producer);
    mlt_profile_close(t->profile);
}

/* The centre pixel's Y, U and V at a position, with the picture asked for
   as yuv420p or yuv422 (-1 if there was no picture in that format). */
static int mltt_centre_as(mltt *t, int position, mlt_image_format want, int yuv[3])
{
    mlt_producer_seek(t->producer, position);
    mlt_frame frame = mlt_consumer_get_frame(t->consumer);
    if (!frame)
        return -1;
    mlt_image_format format = want;
    int w = t->profile->width, h = t->profile->height;
    uint8_t *image = NULL;
    int error = mlt_frame_get_image(frame, &image, &format, &w, &h, 0);
    int result = -1;
    if (!error && image && format == want && want == mlt_image_yuv420p) {
        int ysize = w * h, csize = ysize / 4, c = (h / 4) * (w / 2) + w / 4;
        yuv[0] = image[(h / 2) * w + w / 2];
        yuv[1] = image[ysize + c];
        yuv[2] = image[ysize + csize + c];
        result = yuv[0];
    } else if (!error && image && format == want && want == mlt_image_yuv422) {
        uint8_t *p = image + ((h / 2) * w + (w / 2 & ~1)) * 2; /* Y0 U Y1 V */
        yuv[0] = p[0];
        yuv[1] = p[1];
        yuv[2] = p[3];
        result = yuv[0];
    }
    mlt_frame_close(frame);
    return result;
}

static int mltt_centre(mltt *t, int position, int yuv[3])
{
    return mltt_centre_as(t, position, mlt_image_yuv420p, yuv);
}
#endif
