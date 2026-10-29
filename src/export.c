/* export.c - a project made into a video file (export.h).
 *
 * The project file is opened as the editor opens it (re_project_open: its
 * pictures, titles, fades and dissolves as they play), and played through
 * MLT's avformat consumer, which with MLT_NO_THREADS makes every frame on
 * this thread and returns at the end (mlt_consumer_start). The size is
 * the consumer's (MLT scales); the frame rate the project's.
 *
 * Video: H.264 by the VideoCore (h264_encvchiq: about three times real time
 * at 1080p on a Pi 4) when asked and FFmpeg has it, at a bit rate from the
 * picture's size (bits a pixel: 0.07, 0.10, 0.15, as !FFmpeg's Convert
 * window); else libx264 (veryfast, rate factor 28, 23, 19); else mpeg4. If
 * a codec makes no video (MLT writes the sound alone when the video codec
 * won't open: the VideoCore's without VCHIQ), the next is tried. Sound:
 * AAC, 48 kHz stereo. MP4, its index at the start (faststart).
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "export.h"
#include "engine.h"
#include "project.h"
#include "proxy.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* (patches/mlt 0020) */
extern void mlt_avformat_prefer_limit(const char *spec);

int re_export_bitrate(int width, int height, double fps, int quality)
{
    static const double BPP[3] = {0.07, 0.10, 0.15};
    double b = BPP[quality < 0 ? 0 : quality > 2 ? 2 : quality] * width * height * (fps > 0 ? fps : 25);
    int k = (int) (b / 50000 + 0.5) * 50000;
    return k < 300000 ? 300000 : k > 20000000 ? 20000000 : k;
}

static char report[200];

const char *re_export_report(void)
{
    return report;
}

/* Proxies: each frame scaled by the previews' scaler (patches/mlt 0014's,
   halving then bilinear, NEON rows) instead of swscale's general bilinear,
   which at 1080p to 360 lines was half of all a proxy's work on Linux, and
   nearly all of what's left to the ARM when the VideoCore decodes and
   encodes. (Only a yuv420p picture with nothing but its size to change.) */
static mlt_frame fast_process(mlt_filter filter, mlt_frame frame)
{
    (void) filter;
    mlt_properties_set_int(MLT_FRAME_PROPERTIES(frame), "consumer.fast_scale", 1);
    return frame;
}

typedef struct
{
    mlt_producer producer;
    int done, total, stopped;
    void (*progress)(void *ctx, int done, int total);
    int (*stop)(void *ctx);
    void *ctx;
} run;

static void on_show(mlt_properties owner, run *r, mlt_event_data data)
{
    (void) owner;
    (void) data;
    r->done++;
    if (r->progress)
        r->progress(r->ctx, r->done, r->total);
    if (!r->stopped && r->stop && r->stop(r->ctx)) {
        r->stopped = 1;
        mlt_producer_set_speed(r->producer, 0); /* (terminate_on_pause: the consumer ends) */
    }
}

static int has_encoder(const char *name)
{
    return avcodec_find_encoder_by_name(name) != NULL;
}

/* One try with a video codec: frames made (or -1) */
static int encode(re_project *p, const char *out, const re_export_settings *s, const char *vcodec, run *r)
{
    mlt_profile profile = re_project_profile(p);
    mlt_producer prod = re_project_producer(p);
    mlt_consumer c = mlt_factory_consumer(profile, "avformat", out);
    if (!c || !prod)
        return -1;
    mlt_properties cp = MLT_CONSUMER_PROPERTIES(c);
    int h = s->height > 0 && s->height < profile->height ? s->height : profile->height;
    int w = (int) ((double) profile->width * h / profile->height + 0.5);
    double dar = (double) profile->display_aspect_num / (profile->display_aspect_den ? profile->display_aspect_den : 1);
    if (dar > 0)
        w = (int) (h * dar + 0.5);
    w &= ~1;
    h &= ~1;
    double fps = (double) profile->frame_rate_num / (profile->frame_rate_den ? profile->frame_rate_den : 1);
    char v[32];
    mlt_properties_set_int(cp, "width", w);
    mlt_properties_set_int(cp, "height", h);
    mlt_properties_set(cp, "f", "mp4");
    mlt_properties_set(cp, "movflags", "+faststart");
    mlt_properties_set(cp, "vcodec", vcodec);
    mlt_properties_set(cp, "pix_fmt", "yuv420p");
    mlt_properties_set_int(cp, "g", s->gop > 0 ? s->gop : (int) (fps * 2 + 0.5));
    if (!strcmp(vcodec, "libx264")) {
        static const int CRF[3] = {28, 23, 19};
        mlt_properties_set(cp, "vpreset", "veryfast");
        mlt_properties_set_int(cp, "crf", CRF[s->quality < 0 ? 0 : s->quality > 2 ? 2 : s->quality]);
    } else {
        snprintf(v, sizeof(v), "%dk", re_export_bitrate(w, h, fps, s->quality) / 1000);
        mlt_properties_set(cp, "vb", v);
        if (!strcmp(vcodec, "h264_encvchiq"))
            mlt_properties_set(cp, "vprofile", s->quality == RE_EXPORT_SMALL ? "77" : "100");
    }
    mlt_properties_set(cp, "acodec", "aac");
    mlt_properties_set(cp, "ab", s->quality == RE_EXPORT_HIGH ? "192k" : "160k");
    mlt_properties_set_int(cp, "frequency", 48000);
    mlt_properties_set_int(cp, "channels", 2);
    mlt_properties_set(cp, "rescale", "bilinear");
    mlt_properties_set_int(cp, "progressive", 1);
    mlt_properties_set_int(cp, "terminate_on_pause", 1);
    mlt_properties_set_int(cp, "real_time", -1);
    mlt_producer cut = NULL; /* (frames in to out: a cut of them, played from its start) */
    if (s->range) {
        cut = mlt_producer_cut(prod, s->in, s->out);
        if (!cut) {
            mlt_consumer_close(c);
            return -1;
        }
        prod = cut;
    }
    mlt_filter fast = NULL;
    if (s->fast && (fast = mlt_filter_new())) {
        fast->process = fast_process;
        mlt_service_attach(MLT_PRODUCER_SERVICE(prod), fast);
    }
    mlt_consumer_connect(c, MLT_PRODUCER_SERVICE(prod));
    mlt_producer_seek(prod, 0);
    mlt_producer_set_speed(prod, 1);
    r->producer = prod;
    r->done = 0;
    r->total = mlt_producer_get_length(prod);
    mlt_events_listen(cp, r, "consumer-frame-show", (mlt_listener) on_show); /* (closed with the consumer) */
    mlt_consumer_start(c); /* (no threads: returns at the end) */
    mlt_consumer_stop(c);
    mlt_consumer_close(c);
    mlt_producer_set_speed(prod, 0);
    if (fast) {
        mlt_service_detach(MLT_PRODUCER_SERVICE(prod), fast);
        mlt_filter_close(fast);
    }
    if (cut)
        mlt_producer_close(cut);
    return r->done;
}

/* The file has a video stream (MLT leaves out a video codec that won't
   open, and writes the sound alone) */
static int has_video(const char *file)
{
    AVFormatContext *fc = NULL;
    int v = 0;
    if (avformat_open_input(&fc, file, NULL, NULL) == 0) {
        if (avformat_find_stream_info(fc, NULL) >= 0)
            for (unsigned i = 0; i < fc->nb_streams; i++)
                v |= fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
        avformat_close_input(&fc);
    }
    return v;
}

static long size_of(const char *file)
{
    FILE *f = fopen(file, "rb");
    if (!f)
        return -1;
    long n = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
    fclose(f);
    return n;
}

static int export_open(re_project *p, const char *out, const re_export_settings *s,
                       void (*progress)(void *ctx, int done, int total), int (*stop)(void *ctx), void *ctx, char *err,
                       int errsize);

/* Why a project (or a clip, for a proxy) couldn't be exported: which of
   its checks failed, said short (the Export box shows about 100
   characters); more of it printed, for the editor's log. NULL: it can go
   on. */
static const char *unreadable(const char *file, re_project *p, char *err, int errsize)
{
    const char *leaf = strrchr(file, '/');
    leaf = leaf ? leaf + 1 : file;
    long size = size_of(file);
    if (size < 0)
        snprintf(err, errsize, "%s isn't there (or can't be opened)", leaf);
    else if (!p)
        snprintf(err, errsize, "%s (%ld bytes) couldn't be loaded", leaf, size);
    else if (re_project_length(p) <= 0)
        snprintf(err, errsize, "%s has no frames in it", leaf);
    else
        return NULL;
    printf("couldn't read %s: %s\n", file, err); /* (the whole name, for the log) */
    if (size > 0) { /* (how it starts: XML, or not) */
        char head[81] = "";
        FILE *f = fopen(file, "rb");
        size_t n = f ? fread(head, 1, sizeof(head) - 1, f) : 0;
        if (f)
            fclose(f);
        head[n] = 0;
        for (size_t i = 0; i < n; i++)
            if (head[i] < 32 || head[i] > 126)
                head[i] = ' ';
        printf("it starts: %s\n", head);
    }
    fflush(stdout);
    return err;
}

int re_export(const char *project, const char *out, const re_export_settings *s,
              void (*progress)(void *ctx, int done, int total), int (*stop)(void *ctx), void *ctx, char *err,
              int errsize)
{
    snprintf(err, errsize, "%s", "");
    re_project *p = re_project_open(project, 1);
    if (unreadable(project, p, err, errsize)) {
        re_project_close(p);
        return -1;
    }
    return export_open(p, out, s, progress, stop, ctx, err, errsize);
}

int re_export_proxy(const char *file, const char *out, int hardware, void (*progress)(void *ctx, int done, int total),
                    int (*stop)(void *ctx), void *ctx, char *err, int errsize)
{
    re_export_settings s = {RE_PROXY_HEIGHT, RE_EXPORT_SMALL, hardware, NULL, 0, 0, 0, 5, 1};
    snprintf(err, errsize, "%s", "");
    re_project *p = re_project_open(file, 0); /* (the clip as a project of it: its own size and rate) */
    if (unreadable(file, p, err, errsize)) {
        re_project_close(p);
        return -1;
    }
    if (re_project_profile(p)->height <= RE_PROXY_HEIGHT)
        s.height = 0; /* (as it is: small already, but not H.264) */
    return export_open(p, out, &s, progress, stop, ctx, err, errsize);
}

/* A project opened (or NULL), exported, closed */
static int export_open(re_project *p, const char *out, const re_export_settings *s,
                       void (*progress)(void *ctx, int done, int total), int (*stop)(void *ctx), void *ctx, char *err,
                       int errsize)
{
    if (!p || re_project_length(p) <= 0) {
        snprintf(err, errsize, "the project couldn't be read");
        if (p)
            re_project_close(p);
        return -1;
    }
    run r = {NULL, 0, 0, 0, progress, stop, ctx};
    const char *codecs[3];
    int n = 0;
    if (s->vcodec) {
        codecs[n++] = s->vcodec;
    } else {
        if (s->hardware && has_encoder("h264_encvchiq"))
            codecs[n++] = "h264_encvchiq";
        if (has_encoder("libx264"))
            codecs[n++] = "libx264";
        codecs[n++] = "mpeg4";
    }
    int len = re_project_length(p);
    if (s->range && (s->in < 0 || s->out < s->in || s->out >= len)) {
        snprintf(err, errsize, "frames %d to %d aren't in the project", s->in, s->out);
        re_project_close(p);
        return -1;
    }
    int made = -1, total = s->range ? s->out - s->in + 1 : len;
    const char *used = codecs[0];
    clock_t t0 = clock();
    for (int i = 0; i < n && !r.stopped; i++) {
        used = codecs[i];
        remove(out);
        made = encode(p, out, s, used, &r);
        if (r.stopped || (made > 0 && size_of(out) > 0 && has_video(out)))
            break; /* (made: done, or stopped) */
    }
    double secs = (double) (clock() - t0) / CLOCKS_PER_SEC;
    const char *dec = re_project_decoder(p, s->range ? s->in : 0);
    snprintf(report, sizeof(report), "decoder %s, encoder %s, %d frames in %.1f s", dec ? dec : "?", used,
             made < 0 ? 0 : made, secs);
    re_project_close(p);
    if (r.stopped) {
        remove(out);
        return 1;
    }
    if (made < total || size_of(out) <= 0 || !has_video(out)) {
        snprintf(err, errsize, "the video couldn't be made (%s: %d of %d frames)", used, made < 0 ? 0 : made, total);
        remove(out);
        return -1;
    }
    return 0;
}

/* ---- the program -------------------------------------------------------------------------- */

static void say_progress(void *ctx, int done, int total)
{
    clock_t *last = ctx, now = clock();
    if (done == total || now - *last >= CLOCKS_PER_SEC / 4) {
        *last = now;
        printf("progress %d %d\n", done, total);
        fflush(stdout);
    }
}

int re_export_args(const char *file, char ***args)
{
    FILE *f = fopen(file, "r");
    char line[2048];
    int n = 0;
    *args = NULL;
    if (!f)
        return -1;
    while (n < 16 && fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = 0;
        char **more = realloc(*args, sizeof(char *) * (n + 1));
        if (!more)
            break;
        *args = more;
        (*args)[n++] = strdup(line);
    }
    fclose(f);
    remove(file);
    return n;
}

void re_export_args_free(char **args, int n)
{
    for (int i = 0; i < n; i++)
        free(args[i]);
    free(args);
}

int re_export_main(int argc, char **argv)
{
    if (argc == 3 && argv[2][0] == '@') { /* (the arguments in a file: names with spaces) */
        char **a;
        int n = re_export_args(argv[2] + 1, &a);
        if (n < 0) {
            printf("failed: the arguments' file %s couldn't be read\n", argv[2] + 1);
            return 2;
        }
        char **all = malloc(sizeof(char *) * (n + 3));
        if (!all)
            return 2;
        all[0] = argv[0];
        all[1] = argv[1];
        for (int i = 0; i < n; i++)
            all[2 + i] = a[i];
        all[2 + n] = NULL;
        int r = re_export_main(n + 2, all);
        free(all);
        re_export_args_free(a, n);
        return r;
    }
    if (argc > 2 && !strncmp(argv[2], "-decoders ", 10)) { /* (the editor's share of the VideoCore: children.c) */
        mlt_avformat_prefer_limit(argv[2] + 10);
        printf("decoders %s\n", argv[2] + 10);
        argv[2] = argv[1];
        argv[1] = argv[0];
        return re_export_main(argc - 1, argv + 1);
    }
    int proxy = argc > 1 && !strcmp(argv[1], "-proxy");
    if (argc < 4) {
        printf("failed: usage: -export project out [height [quality [hardware [in out]]]], -proxy file out [hardware]\n"
               "  (-export ... in out 1: scaled as previews are; a first argument \"-decoders SPEC\": the hardware decoders this may open)\n");
        return 2;
    }
    re_export_settings s = {argc > 4 ? atoi(argv[4]) : 0, argc > 5 ? atoi(argv[5]) : RE_EXPORT_NORMAL,
                            argc > 6 ? atoi(argv[6]) : 0, NULL, argc > 8, argc > 8 ? atoi(argv[7]) : 0,
                            argc > 8 ? atoi(argv[8]) : 0, 0, argc > 9 && atoi(argv[9])};
    if (re_mlt_start() < 0) {
        printf("failed: MLT couldn't start\n");
        return 2;
    }
    char err[200];
    clock_t last = 0;
    printf("progress 0 0\n");
    fflush(stdout);
    int r = proxy ? re_export_proxy(argv[2], argv[3], argc > 4 && atoi(argv[4]), say_progress, NULL, &last, err,
                                    sizeof(err))
                  : re_export(argv[2], argv[3], &s, say_progress, NULL, &last, err, sizeof(err));
    if (re_export_report()[0])
        printf("used %s\n", re_export_report()); /* (the editor logs it) */
    printf(r == 0 ? "done\n" : r == 1 ? "stopped\n" : "failed: %s\n", err);
    fflush(stdout);
    return r < 0 ? 2 : r;
}
