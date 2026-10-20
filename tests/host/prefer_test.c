/* prefer_test - the avformat producer's preferred decoders (patch 0004).
 *
 * Usage: prefer_test MODE numbered.mp4
 * numbered.mp4: H.264, 25 fps, no B-frames, losslessly coded, frame n a flat
 * grey of luma 16 + 2n, so each picture says which frame it is.
 *
 * On RISC OS the producer prefers h264_vchiq (the VideoCore). Here a
 * stand-in plays it: avcodec_find_decoder_by_name("h264_vchiq") gives
 * FFmpeg's h264, and the next avcodec_open2 is "h264_vchiq"'s
 * (-Wl,--wrap=...). MODE says how the stand-in behaves:
 *   normal  it decodes; after each jump (a seek, or a jump forward decoded
 *           through) it is given drop_before, a pts half a frame before the
 *           picture asked for
 *   refuse  it refuses to open (ENOSYS, as for a stream it can't take):
 *           the producer opens the default decoder instead
 *   late    it fails part way, at its 5th packet (ENOSYS): the producer
 *           opens the default decoder and seeks again to where it was
 *   off     MLT_AVFORMAT_PREFER is empty: it is never asked for
 *   lazy    it isn't opened with the clip (opening a project opens every
 *           clip), only when the first picture is decoded: the Pi ran out
 *           of VideoCore memory when every clip's decoder was opened at load
 *   max     MLT_AVFORMAT_PREFER_MAX=1 and two clips decoded at once: one
 *           gets it, the other the default decoder; when the first is
 *           closed, its place is free again
 * In every mode, every picture must be the right one.
 */
#include "mltt.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>

static int pending = 0, asked = 0, opened_preferred = 0, refused = 0, failed_late = 0;
static int packets_to_preferred = 0;
static AVCodecContext *preferred_ctx = NULL;
static int preferred_now = 0, preferred_most = 0; /* open at once, and the most */
static const char *mode = "normal";
static int64_t drop_before = INT64_MIN;
static int drop_before_sets = 0;

const AVCodec *__real_avcodec_find_decoder_by_name(const char *name);
const AVCodec *__wrap_avcodec_find_decoder_by_name(const char *name)
{
    if (!strcmp(name, "h264_vchiq")) {
        asked++;
        pending = 1;
        return avcodec_find_decoder(AV_CODEC_ID_H264);
    }
    return __real_avcodec_find_decoder_by_name(name);
}

int __real_avcodec_open2(AVCodecContext *c, const AVCodec *codec, AVDictionary **o);
int __wrap_avcodec_open2(AVCodecContext *c, const AVCodec *codec, AVDictionary **o)
{
    if (pending) {
        pending = 0;
        if (!strcmp(mode, "refuse")) {
            refused++;
            return AVERROR(ENOSYS);
        }
        opened_preferred++;
        preferred_ctx = c;
        c->opaque = &opened_preferred;
        if (++preferred_now > preferred_most)
            preferred_most = preferred_now;
        packets_to_preferred = 0;
    }
    return __real_avcodec_open2(c, codec, o);
}

int __real_avcodec_send_packet(AVCodecContext *c, const AVPacket *p);
int __wrap_avcodec_send_packet(AVCodecContext *c, const AVPacket *p)
{
    if (c == preferred_ctx && !strcmp(mode, "late") && ++packets_to_preferred == 5) {
        failed_late++;
        return AVERROR(ENOSYS);
    }
    return __real_avcodec_send_packet(c, p);
}

static int is_preferred(AVCodecContext *c)
{
    /* (the stand-in is FFmpeg's h264 opened as h264_vchiq: told apart by
       the opaque pointer set at open) */
    return c && c->opaque == (void *) &opened_preferred;
}

void __real_avcodec_free_context(AVCodecContext **c);
void __wrap_avcodec_free_context(AVCodecContext **c)
{
    if (c && is_preferred(*c))
        preferred_now--;
    if (c && *c == preferred_ctx)
        preferred_ctx = NULL;
    __real_avcodec_free_context(c);
}

int __real_av_opt_set_int(void *obj, const char *name, int64_t val, int flags);
int __wrap_av_opt_set_int(void *obj, const char *name, int64_t val, int flags)
{
    if (!strcmp(name, "drop_before")) {      /* (FFmpeg's h264 has no such option) */
        CHECK(obj == preferred_ctx, "drop_before given to a decoder that isn't the preferred one");
        drop_before = val;
        drop_before_sets++;
        return 0;
    }
    return __real_av_opt_set_int(obj, name, val, flags);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: prefer_test normal|refuse|late|off numbered.mp4\n");
        return 2;
    }
    mode = argv[1];
    if (!strcmp(mode, "off"))
        setenv("MLT_AVFORMAT_PREFER", "", 1);
    if (!strcmp(mode, "lazy")) {
        mltt t;
        if (!mltt_open(&t, "hdv_720_25p", "avformat", argv[2]))
            return 1;
        /* frames got without their pictures (as a project's load, a probe,
           or the sound alone do): no hardware decoder yet */
        for (int i = 0; i < 3; i++) {
            mlt_producer_seek(t.producer, i);
            mlt_frame f = mlt_consumer_get_frame(t.consumer);
            mlt_frame_close(f);
        }
        CHECK(asked == 0 && opened_preferred == 0, "lazy: before any picture, asked for %d times, opened %d",
              asked, opened_preferred);
        int yuv[3];
        CHECK(abs(mltt_centre(&t, 5, yuv) - 26) <= 1, "lazy: frame 5 has luma %d, not 26", yuv[0]);
        CHECK(opened_preferred == 1, "lazy: after the first picture, opened %d times", opened_preferred);
        CHECK(abs(mltt_centre(&t, 6, yuv) - 28) <= 1, "lazy: frame 6 has luma %d, not 28", yuv[0]);
        mltt_close(&t);
        CHECK(preferred_now == 0, "lazy: %d still open after closing", preferred_now);
        printf("prefer_test %s: %s\n", mode, mltt_failures ? "FAILED" : "passed");
        return mltt_failures != 0;
    }
    if (!strcmp(mode, "max")) {
        setenv("MLT_AVFORMAT_PREFER_MAX", "1", 1);
        mltt a, b;
        if (!mltt_open(&a, "hdv_720_25p", "avformat", argv[2]) || !mltt_open(&b, "hdv_720_25p", "avformat", argv[2]))
            return 1;
        int yuv[3];
        for (int i = 0; i < 6; i++) {
            CHECK(abs(mltt_centre(&a, i, yuv) - (16 + 2 * i)) <= 1, "max: clip A frame %d has luma %d", i, yuv[0]);
            CHECK(abs(mltt_centre(&b, 10 + i, yuv) - (36 + 2 * i)) <= 1, "max: clip B frame %d has luma %d", 10 + i, yuv[0]);
        }
        CHECK(opened_preferred == 1 && preferred_most == 1, "max: opened %d times, %d at once (limit 1)",
              opened_preferred, preferred_most);
        const char *da = mlt_properties_get(MLT_PRODUCER_PROPERTIES(a.producer), "meta.media.decoder");
        CHECK(preferred_now == 1, "max: %d open while both clips are", preferred_now);
        mltt_close(&a);
        CHECK(preferred_now == 0, "max: %d open after clip A closed", preferred_now);
        /* a third clip now gets the place clip A gave back */
        mltt c;
        if (!mltt_open(&c, "hdv_720_25p", "avformat", argv[2]))
            return 1;
        CHECK(abs(mltt_centre(&c, 3, yuv) - 22) <= 1, "max: clip C frame 3 has luma %d", yuv[0]);
        CHECK(opened_preferred == 2, "max: the freed place wasn't taken (opened %d times)", opened_preferred);
        (void) da;
        mltt_close(&b);
        mltt_close(&c);
        printf("prefer_test %s: %s\n", mode, mltt_failures ? "FAILED" : "passed");
        return mltt_failures != 0;
    }

    /* the clip's time base, for drop_before */
    AVFormatContext *fc = NULL;
    if (avformat_open_input(&fc, argv[2], NULL, NULL) < 0)
        return 1;
    avformat_find_stream_info(fc, NULL);
    double ticks = 1.0 / (av_q2d(fc->streams[0]->time_base) * 25);
    avformat_close_input(&fc);

    mltt t;
    if (!mltt_open(&t, "hdv_720_25p", "avformat", argv[2]))
        return 1;

    /* forward, a jump forward, back, the end, back again */
    static const int positions[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 40, 41, 42, 10, 74, 30, 31, -1};
    int yuv[3];
    for (int i = 0; positions[i] >= 0; i++) {
        int n = positions[i];
        int y = mltt_centre(&t, n, yuv);
        CHECK(abs(y - (16 + 2 * n)) <= 1, "%s: frame %d has luma %d, not %d", mode, n, y, 16 + 2 * n);
        if (i > 0 && n != positions[i - 1] + 1 && !strcmp(mode, "normal")) {
            /* a jump (decoded through, or a seek) gave drop_before: half a
               frame before the frame asked for */
            CHECK(drop_before > (n - 1) * ticks && drop_before < n * ticks,
                  "after the jump to %d, drop_before is %lld, not between frames %d and %d (%.0f, %.0f)",
                  n, (long long) drop_before, n - 1, n, (n - 1) * ticks, n * ticks);
        }
    }
    const char *decoder = mlt_properties_get(MLT_PRODUCER_PROPERTIES(t.producer), "meta.media.decoder");
    CHECK(decoder && !strcmp(decoder, "h264"), "meta.media.decoder is %s", decoder ? decoder : "(none)");
    mltt_close(&t);

    if (!strcmp(mode, "normal")) {
        CHECK(asked == 1 && opened_preferred == 1, "asked for %d times, opened %d times", asked, opened_preferred);
        CHECK(drop_before_sets >= 3, "drop_before set %d times (one a seek: expected 3 or more)", drop_before_sets);
    } else if (!strcmp(mode, "refuse")) {
        CHECK(asked == 1 && refused == 1, "asked for %d times, refused %d times (once each)", asked, refused);
        CHECK(drop_before_sets == 0, "drop_before set %d times on the default decoder", drop_before_sets);
    } else if (!strcmp(mode, "late")) {
        CHECK(asked == 1 && failed_late == 1, "asked for %d times, failed %d times (once each)", asked, failed_late);
    } else if (!strcmp(mode, "off")) {
        CHECK(asked == 0, "asked for %d times with MLT_AVFORMAT_PREFER empty", asked);
    }
    printf("prefer_test %s: %s\n", mode, mltt_failures ? "FAILED" : "passed");
    return mltt_failures != 0;
}
