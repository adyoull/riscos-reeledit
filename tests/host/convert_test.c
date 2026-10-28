/* convert_test - no picture conversion when none is needed.
 *
 * Usage: convert_test clip.mp4 project.mlt
 * clip.mp4: yuv420p video; project.mlt: two such clips with a dissolve,
 * at the clips' own size (and progressive).
 *
 * Asked for yuv420p at the video's own size, MLT should hand over the
 * decoded pictures without running swscale. In MLT 7.42 as built here it
 * didn't: the avdeinterlace normaliser (in every clip's chain when the xine
 * module isn't built) asked every frame for yuv422, so each picture was
 * converted to yuv422 and back; and the luma transition dissolved only in
 * yuv422. Profiled on Linux (2026-10-20): the conversions took more time
 * than decoding. sws_scale is wrapped (-Wl,--wrap=sws_scale) and counted.
 *
 * Also: exporting (the avformat consumer, to an encoder that takes
 * yuv420p) converts nothing either: the consumer asked for yuv422, so
 * yuv420p video went to yuv422 and back. And the colour producer (black
 * gaps, title backgrounds) makes its picture in the profile's colour
 * space: it made BT.601, which an HD (BT.709) export converted through RGB.
 *
 * And no copies of whole pictures beyond the one into MLT's buffer: the
 * producer's image cache (a deep copy in, and another out) is off, and the
 * picture kept for error concealment is kept by reference. On the Pi (engine
 * test 3) MLT took 20 ms a 1080p picture of a flat colour, which decodes in
 * almost no time; each copy of a 1080p picture is 3 MB. mlt_frame_clone and
 * mlt_frame_clone_image are wrapped and counted. A frame asked for past the
 * clip's end must still be its last picture (that's the concealment path).
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "mltt.h"
#include <libswscale/swscale.h>
#include <libavcodec/avcodec.h>

static int scales = 0, clones = 0;
/* decoding made to fail (error concealment) */
static int fail_decoding = 0;
int __real_avcodec_receive_frame(AVCodecContext *c, AVFrame *f);
int __wrap_avcodec_receive_frame(AVCodecContext *c, AVFrame *f)
{
    if (fail_decoding && c->codec_type == AVMEDIA_TYPE_VIDEO) {
        __real_avcodec_receive_frame(c, f);
        av_frame_unref(f);
        return AVERROR_INVALIDDATA;
    }
    return __real_avcodec_receive_frame(c, f);
}

mlt_frame __real_mlt_frame_clone(mlt_frame self, int is_deep);
mlt_frame __wrap_mlt_frame_clone(mlt_frame self, int is_deep)
{
    clones++;
    return __real_mlt_frame_clone(self, is_deep);
}
mlt_frame __real_mlt_frame_clone_image(mlt_frame self, int is_deep);
mlt_frame __wrap_mlt_frame_clone_image(mlt_frame self, int is_deep)
{
    clones++;
    return __real_mlt_frame_clone_image(self, is_deep);
}
int __real_sws_scale(struct SwsContext *c, const uint8_t *const src[], const int srcStride[], int y,
                     int h, uint8_t *const dst[], const int dstStride[]);
int __wrap_sws_scale(struct SwsContext *c, const uint8_t *const src[], const int srcStride[], int y,
                     int h, uint8_t *const dst[], const int dstStride[])
{
    scales++;
    return __real_sws_scale(c, src, srcStride, y, h, dst, dstStride);
}

static int frames(const char *file, int from, int to)
{
    mlt_profile profile = mlt_profile_init(NULL);
    mlt_producer producer = mlt_factory_producer(profile, NULL, file);
    if (producer && !profile->is_explicit) {
        mlt_profile_from_producer(profile, producer);
        mlt_producer_close(producer);
        producer = mlt_factory_producer(profile, NULL, file);
    }
    if (!producer) {
        fprintf(stderr, "can't open %s\n", file);
        return -1;
    }
    mlt_consumer consumer = mlt_factory_consumer(profile, "null", NULL);
    mlt_consumer_connect(consumer, MLT_PRODUCER_SERVICE(producer));
    int before = scales;
    for (int i = from; i <= to; i++) {
        mlt_producer_seek(producer, i);
        mlt_frame frame = mlt_consumer_get_frame(consumer);
        mlt_image_format format = mlt_image_yuv420p;
        int w = profile->width, h = profile->height;
        uint8_t *image = NULL;
        mlt_frame_get_image(frame, &image, &format, &w, &h, 0);
        CHECK(image && format == mlt_image_yuv420p && w == profile->width && h == profile->height,
              "%s frame %d: %s %dx%d", file, i, mlt_image_format_name(format), w, h);
        mlt_frame_close(frame);
    }
    mlt_consumer_close(consumer);
    mlt_producer_close(producer);
    mlt_profile_close(profile);
    return scales - before;
}

/* Export a producer with the avformat consumer (mpeg4, which takes yuv420p,
   as the Pi's h264_encvchiq does). Returns the sws_scale calls. */
static int export(const char *profile_name, const char *resource, const char *out)
{
    mlt_profile profile = mlt_profile_init(profile_name);
    mlt_producer producer = mlt_factory_producer(profile, NULL, resource);
    if (producer && !profile_name) {
        mlt_profile_from_producer(profile, producer);
        mlt_producer_close(producer);
        producer = mlt_factory_producer(profile, NULL, resource);
    }
    if (!producer) {
        fprintf(stderr, "can't open %s\n", resource);
        return -1;
    }
    mlt_producer_set_in_and_out(producer, 0, 9);
    mlt_consumer consumer = mlt_factory_consumer(profile, "avformat", out);
    mlt_properties_set(MLT_CONSUMER_PROPERTIES(consumer), "vcodec", "mpeg4");
    mlt_properties_set_int(MLT_CONSUMER_PROPERTIES(consumer), "an", 1);
    mlt_properties_set_int(MLT_CONSUMER_PROPERTIES(consumer), "terminate_on_pause", 1);
    mlt_consumer_connect(consumer, MLT_PRODUCER_SERVICE(producer));
    int before = scales;
    mlt_consumer_start(consumer); /* (no threads: returns when it has finished) */
    mlt_consumer_stop(consumer);
    mlt_consumer_close(consumer);
    mlt_producer_close(producer);
    mlt_profile_close(profile);
    return scales - before;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: convert_test clip.mp4 project.mlt numbered.mp4\n");
        return 2;
    }
    mlt_static_modules_register();
    mlt_factory_init(NULL);
    int c0 = clones;
    int n = frames(argv[1], 0, 19);
    CHECK(n == 0, "a clip: %d sws_scale calls for 20 frames", n);
    CHECK(clones == c0, "a clip: %d pictures cloned for 20 frames", clones - c0);
    n = frames(argv[2], 50, 74);
    CHECK(n == 0, "a dissolve: %d sws_scale calls for 25 frames", n);
    n = export(NULL, argv[2], "convert_test_export.mp4");
    CHECK(n == 0, "an export of the dissolve: %d sws_scale calls for 10 frames", n);

    /* the colour producer, in an HD profile: red in BT.709 video range is
       Y 62-63, Cb 102, Cr 240 (in BT.601 it was 81, 90, 240) */
    mltt t;
    if (!mltt_open(&t, "atsc_1080p_25", "colour", "#ff0000"))
        return 1;
    int yuv[3] = {0};
    CHECK(mltt_centre(&t, 0, yuv) >= 0, "no picture from colour");
    CHECK(abs(yuv[0] - 63) <= 1 && abs(yuv[1] - 102) <= 1 && abs(yuv[2] - 240) <= 1,
          "red in an HD profile is %d,%d,%d, not BT.709's 63,102,240", yuv[0], yuv[1], yuv[2]);
    mltt_close(&t);
    n = export("atsc_1080p_25", "colour:#ff0000", "convert_test_colour.mp4");
    CHECK(n == 0, "an export of a colour in an HD profile: %d sws_scale calls for 10 frames", n);

    /* past the end, and after a seek back and past it again: the last picture
       (numbered.mp4: 75 frames, frame n luma 16 + 2n, the last 164) */
    if (!mltt_open(&t, "hdv_720_25p", "avformat", argv[3]))
        return 1;
    static const int past[] = {74, 80, 90, 10, 99, -1};
    for (int i = 0; past[i] >= 0; i++) {
        int want = past[i] < 75 ? 16 + 2 * past[i] : 164;
        int y = mltt_centre(&t, past[i], yuv);
        CHECK(abs(y - want) <= 1, "frame %d: luma %d, not %d", past[i], y, want);
    }
    /* a picture that fails to decode: the last good one is shown instead
       (error concealment, from the picture kept by reference). (A clip
       opened afresh: the picture kept is the furthest one decoded.) */
    mltt_close(&t);
    if (!mltt_open(&t, "hdv_720_25p", "avformat", argv[3]))
        return 1;
    CHECK(abs(mltt_centre(&t, 20, yuv) - 56) <= 1, "frame 20: luma %d, not 56", yuv[0]);
    fail_decoding = 1;
    int y = mltt_centre(&t, 21, yuv);
    fail_decoding = 0;
    CHECK(abs(y - 56) <= 1, "frame 21, which failed to decode: luma %d, not frame 20's 56", y);
    mltt_close(&t);

    printf("convert_test: %s\n", mltt_failures ? "FAILED" : "passed");
    return mltt_failures != 0;
}
