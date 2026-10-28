/* bench - how long MLT takes to make each picture of a clip or project, as
 * ReelEdit's player asks for them (yuv420p at the profile's size, one
 * after another, no threads). For profiling on Linux, and on RISC OS.
 *
 *   bench FILE [FRAMES] [SCALE]
 *   bench -raw FILE [FRAMES]
 * -raw decodes the file with FFmpeg alone (h264_vchiq or hevc_hwdec first,
 * as MLT would choose, else FFmpeg's own decoder), and times one copy of a
 * picture: the least MLT could cost, to compare with MLT's figure.
 * SCALE: the preview size against the project's (default 1; 0.5 half).
 * It prints which decoder each clip got (MLT's info messages), then the
 * time a picture took on average, the middle time (the median: what a
 * picture usually costs, unmoved by a few slow ones such as a VideoCore
 * decoder being handed over) and the slowest, and the first picture
 * separately (opening and the first decode).
 */
#include "../src/still.h"
#include <framework/mlt.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/cpu.h>
#include <libavutil/imgutils.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int by_time(const void *a, const void *b)
{
    double x = *(const double *) a, y = *(const double *) b;
    return x < y ? -1 : x > y;
}

static int raw(const char *file, int frames)
{
    AVFormatContext *fc = NULL;
    if (avformat_open_input(&fc, file, NULL, NULL) < 0 || avformat_find_stream_info(fc, NULL) < 0) {
        fprintf(stderr, "bench: can't open %s\n", file);
        return 1;
    }
    int vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vi < 0)
        return 1;
    AVCodecParameters *par = fc->streams[vi]->codecpar;
    const char *names[] = {par->codec_id == AV_CODEC_ID_H264 ? "h264_vchiq"
                           : par->codec_id == AV_CODEC_ID_HEVC ? "hevc_hwdec" : NULL,
                           NULL};
    const char *prefer = getenv("MLT_AVFORMAT_PREFER");
    const AVCodec *codec = NULL;
    AVCodecContext *c = NULL;
    for (int pass = 0; pass < 2 && !c; pass++) {
        codec = pass == 0 ? (names[0] && !(prefer && !strstr(prefer, names[0])) ? avcodec_find_decoder_by_name(names[0]) : NULL)
                          : avcodec_find_decoder(par->codec_id);
        if (!codec)
            continue;
        c = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(c, par);
        c->thread_count = 1;
        if (avcodec_open2(c, codec, NULL) < 0)
            avcodec_free_context(&c);
    }
    if (!c)
        return 1;
    printf("bench -raw: %s, decoder %s, %dx%d\n", file, codec->name, par->width, par->height);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    uint8_t *copy = NULL;
    int copy_size = 0, n = 0, eof = 0;
    double start = now(), first = 0, copying = 0;
    while (n < frames) {
        int r = avcodec_receive_frame(c, f);
        if (r == 0) {
            double t0 = now();
            if (!copy) {
                copy_size = av_image_get_buffer_size(f->format, f->width, f->height, 1);
                copy = av_malloc(copy_size);
            }
            uint8_t *d[4];
            int ls[4];
            av_image_fill_arrays(d, ls, copy, f->format, f->width, f->height, 1);
            av_image_copy(d, ls, (const uint8_t **) f->data, f->linesize, f->format, f->width, f->height);
            copying += now() - t0;
            av_frame_unref(f);
            if (n++ == 0) {
                first = now() - start;
                start = now();
                copying = 0;
            }
            continue;
        }
        if (r == AVERROR_EOF || (r != AVERROR(EAGAIN)))
            break;
        if (eof)
            break;
        while ((r = av_read_frame(fc, pkt)) >= 0 && pkt->stream_index != vi)
            av_packet_unref(pkt);
        if (r < 0) {
            avcodec_send_packet(c, NULL);
            eof = 1;
        } else {
            avcodec_send_packet(c, pkt);
            av_packet_unref(pkt);
        }
    }
    double total = now() - start;
    if (n > 1)
        printf("bench -raw: the first picture %.0f ms; then %.1f ms a picture (%.1f a second), "
               "of which copying it %.1f ms\n",
               first * 1000, total * 1000 / (n - 1), (n - 1) / total, copying * 1000 / (n - 1));
    av_free(copy);
    av_frame_free(&f);
    av_packet_free(&pkt);
    avcodec_free_context(&c);
    avformat_close_input(&fc);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "-raw"))
        return raw(argv[2], argc > 3 ? atoi(argv[3]) : 250);
    if (argc < 2) {
        fprintf(stderr, "usage: bench FILE [FRAMES] [SCALE]\n");
        return 2;
    }
#ifdef MLT_STATIC_DEFINE
    mlt_static_modules_register();
#endif
    mlt_factory_init(NULL);
    re_still_register(); /* (pictures as ReelEdit reads them) */
    /* (as ReelEdit: NEON where there is, quick scaling; BENCH_C or
       BENCH_ACCURATE set to any value leaves either out, to compare) */
    int fast = !getenv("BENCH_ACCURATE");
    if ((av_get_cpu_flags() & AV_CPU_FLAG_NEON) && !getenv("BENCH_C"))
        mlt_environment_set("MLT_NEON", "1");
    fprintf(stderr, "bench: NEON %s, scaling %s\n", mlt_environment("MLT_NEON") ? "on" : "off",
            fast ? "fast" : "accurate");
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
    double *times = calloc(frames > 0 ? frames : 1, sizeof *times);
    for (int i = 0; i < frames; i++) {
        double t0 = now();
        mlt_producer_seek(producer, i);
        mlt_frame frame = mlt_consumer_get_frame(consumer);
        mlt_properties_set_int(MLT_FRAME_PROPERTIES(frame), "consumer.source_colours", 1); /* (as ReelEdit's player) */
        mlt_properties_set_int(MLT_FRAME_PROPERTIES(frame), "consumer.fast_scale", fast);
        mlt_image_format format = mlt_image_yuv420p;
        int w = w0, h = h0;
        uint8_t *image = NULL;
        mlt_frame_get_image(frame, &image, &format, &w, &h, 0);
        if (i == 0)
            printf("bench: %s, %d frames, %dx%d %s\n", argv[1], frames, w, h, mlt_image_format_name(format));
        mlt_frame_close(frame);
        double t = now() - t0;
        times[i] = t;
        if (i == 0) {
            first = t;
            start = now();
        } else if (t > worst) {
            worst = t;
        }
    }
    double total = now() - start;
    double median = 0;
    if (frames > 1) {
        qsort(times + 1, frames - 1, sizeof *times, by_time);
        median = times[1 + (frames - 1) / 2];
    }
    free(times);
    printf("bench: the first picture %.0f ms; then %.1f ms a picture (%.1f a second), usually %.1f ms, "
           "the slowest %.1f ms\n",
           first * 1000, total * 1000 / (frames > 1 ? frames - 1 : 1), (frames - 1) / total, median * 1000,
           worst * 1000);
    mlt_consumer_close(consumer);
    mlt_producer_close(producer);
    return 0;
}
