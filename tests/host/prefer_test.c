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
 *   max     one at a time (MLT_AVFORMAT_PREFER_MAX=1, as on the Pi 4, where
 *           a second h264_vchiq never gives a picture): two clips used in
 *           turn (a dissolve) never both hold it; once the first is idle the
 *           second takes it over, the first's closed before the second's
 *           opens; the first gets it back when it's used again; closing a
 *           clip frees its place; every picture right throughout
 *   max3    three at once (MLT_AVFORMAT_PREFER_MAX="hevc_hwdec=1,h264_vchiq=3":
 *           devkit 0.2.18's direct output, where the VideoCore decodes three
 *           1080p streams at once): clips in turn each keep their own; a
 *           fourth uses the default decoder until one of the three is
 *           idle, then takes its place (that one closed first). And a
 *           clip's first picture part way in, its preferred decoder opened
 *           for it, is the right one (it was a GOP late)
 *   exit    a program that exits without closing its clips still closes the
 *           hardware decoder (on RISC OS nothing else would, and it would
 *           keep its VideoCore memory until a restart)
 * In every mode, every picture must be the right one.
 */
#include "mltt.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <unistd.h>

static int pending = 0, asked = 0, opened_preferred = 0, refused = 0, failed_late = 0;
static int packets_to_preferred = 0;
static AVCodecContext *preferred_ctx = NULL;
static int preferred_now = 0, preferred_most = 0; /* open at once, and the most */
static int opened_beside_another = 0;              /* opened while another was open */
static const char *mode = "normal";
static int64_t drop_before = INT64_MIN;
static int drop_before_sets = 0;

static int is_preferred(AVCodecContext *c)
{
    /* (the stand-in is FFmpeg's h264 opened as h264_vchiq: told apart by
       the opaque pointer set at open) */
    return c && c->opaque == (void *) &opened_preferred;
}

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
        if (preferred_now > 0)
            opened_beside_another++;
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
        CHECK(is_preferred(obj), "drop_before given to a decoder that isn't a preferred one");
        drop_before = val;
        drop_before_sets++;
        return 0;
    }
    return __real_av_opt_set_int(obj, name, val, flags);
}

static void check_closed_at_exit(void)
{
    if (preferred_now != 0) {
        fprintf(stderr, "FAIL exit: %d hardware decoder(s) still open at exit\n", preferred_now);
        _exit(1);
    }
    printf("prefer_test exit: %s\n", mltt_failures ? "FAILED" : "passed");
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
        int yuv[3] = {0};
        CHECK(abs(mltt_centre(&t, 5, yuv) - 26) <= 1, "lazy: frame 5 has luma %d, not 26", yuv[0]);
        CHECK(opened_preferred == 1, "lazy: after the first picture, opened %d times", opened_preferred);
        CHECK(abs(mltt_centre(&t, 6, yuv) - 28) <= 1, "lazy: frame 6 has luma %d, not 28", yuv[0]);
        mltt_close(&t);
        CHECK(preferred_now == 0, "lazy: %d still open after closing", preferred_now);
        printf("prefer_test %s: %s\n", mode, mltt_failures ? "FAILED" : "passed");
        return mltt_failures != 0;
    }
    if (!strcmp(mode, "exit")) {
        atexit(check_closed_at_exit); /* (runs after MLT's own, registered later) */
        mltt t;
        if (!mltt_open(&t, "hdv_720_25p", "avformat", argv[2]))
            return 1;
        int yuv[3] = {0};
        CHECK(abs(mltt_centre(&t, 4, yuv) - 24) <= 1, "exit: frame 4 has luma %d", yuv[0]);
        CHECK(preferred_now == 1, "exit: %d open before exiting", preferred_now);
        exit(mltt_failures != 0); /* without closing anything */
    }
    if (!strcmp(mode, "max3")) {
        setenv("MLT_AVFORMAT_PREFER_MAX", "hevc_hwdec=1,h264_vchiq=3", 1);
        mltt a, b, c, d;
        if (!mltt_open(&a, "hdv_720_25p", "avformat", argv[2]) || !mltt_open(&b, "hdv_720_25p", "avformat", argv[2])
            || !mltt_open(&c, "hdv_720_25p", "avformat", argv[2]) || !mltt_open(&d, "hdv_720_25p", "avformat", argv[2]))
            return 1;
        int yuv[3] = {0}, f;
#define FRAME(t, n, what) CHECK(abs(mltt_centre(&t, n, yuv) - (16 + 2 * (n))) <= 1, "max3: %s frame %d has luma %d", what, n, yuv[0])
        /* 0: a clip's first picture part way in (past a keyframe), its
           preferred decoder opened for it: the right picture, not one a
           GOP on (the stream's first pts lost with the decoder changed) */
        FRAME(d, 40, "D's first");
        FRAME(d, 41, "D");
        /* 1: three more in turn: two get places beside D; the third takes
           idle D's (D's closed first) */
        for (f = 0; f < 10; f++) {
            FRAME(a, f, "A");
            FRAME(b, 15 + f, "B");
            FRAME(c, 30 + f, "C");
        }
        CHECK(opened_preferred == 4 && preferred_most == 3 && preferred_now == 3,
              "max3: three in turn beside D: opened %d times, %d at once at most, %d now", opened_preferred,
              preferred_most, preferred_now);
        /* 2: D again with the three in use: the default decoder */
        for (f = 0; f < 3; f++) {
            FRAME(a, 10 + f, "A");
            FRAME(b, 25 + f, "B");
            FRAME(c, 40 + f, "C");
            FRAME(d, 45 + f, "D (beside three)");
        }
        CHECK(opened_preferred == 4 && preferred_now == 3, "max3: D opened one beside three (%d opened, %d now)",
              opened_preferred, preferred_now);
        /* 3: the three idle, D goes on: D takes a place back */
        for (f = 48; f < 70; f++)
            FRAME(d, f, "D (alone)");
        CHECK(opened_preferred == 5 && preferred_now == 3 && preferred_most == 3,
              "max3: D's handover: opened %d times, %d now, %d at most", opened_preferred, preferred_now,
              preferred_most);
        mltt_close(&a);
        mltt_close(&b);
        mltt_close(&c);
        mltt_close(&d);
        CHECK(preferred_now == 0, "max3: %d still open after closing everything", preferred_now);
#undef FRAME
        printf("prefer_test %s: %s\n", mode, mltt_failures ? "FAILED" : "passed");
        return mltt_failures != 0;
    }
    if (!strcmp(mode, "max")) {
        setenv("MLT_AVFORMAT_PREFER_MAX", "hevc_hwdec=3,1", 1); /* (a bare number: every decoder not named) */
        mltt a, b;
        if (!mltt_open(&a, "hdv_720_25p", "avformat", argv[2]) || !mltt_open(&b, "hdv_720_25p", "avformat", argv[2]))
            return 1;
        int yuv[3] = {0}, f;
#define FRAME(t, n, what) CHECK(abs(mltt_centre(&t, n, yuv) - (16 + 2 * (n))) <= 1, "max: %s frame %d has luma %d", what, n, yuv[0])
        /* 1: both in use, in turn (a dissolve): A gets it, B keeps the default */
        for (f = 0; f < 10; f++) {
            FRAME(a, f, "A (with B)");
            FRAME(b, 20 + f, "B (with A)");
        }
        CHECK(opened_preferred == 1 && preferred_most == 1, "max: in turn, opened %d times, %d at once",
              opened_preferred, preferred_most);
        /* 2: A idle, B goes on: B takes it over, A's closed first */
        for (f = 30; f < 45; f++)
            FRAME(b, f, "B (alone)");
        CHECK(opened_preferred == 2, "max: B didn't take it over once A was idle (opened %d times)", opened_preferred);
        CHECK(preferred_now == 1, "max: %d open after the handover", preferred_now);
        /* 3: B idle, A again (from where it was): A takes it back */
        for (f = 10; f < 25; f++)
            FRAME(a, f, "A (again)");
        CHECK(opened_preferred == 3, "max: A didn't take it back (opened %d times)", opened_preferred);
        CHECK(preferred_most == 1 && opened_beside_another == 0,
              "max: %d at once at most, %d opened while another was open", preferred_most, opened_beside_another);
        /* 4: closing the holder frees its place: a new clip gets it at once */
        mltt_close(&a);
        CHECK(preferred_now == 0, "max: %d open after A closed", preferred_now);
        mltt c;
        if (!mltt_open(&c, "hdv_720_25p", "avformat", argv[2]))
            return 1;
        FRAME(c, 3, "C");
        CHECK(opened_preferred == 4, "max: C didn't get the freed place (opened %d times)", opened_preferred);
        mltt_close(&b);
        mltt_close(&c);
        CHECK(preferred_now == 0, "max: %d still open after closing everything", preferred_now);
#undef FRAME
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
    int yuv[3] = {0};
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
