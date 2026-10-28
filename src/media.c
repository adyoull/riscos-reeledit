/* media.c - thumbnails and waveforms for the Timeline, made a little at a
 * time (media.h).
 *
 * Two jobs, each with a file open (a reader, kept open for the next job:
 * a clip's thumbnails at its start and end come from the same file):
 *   a thumbnail: a seek, then decoding up to the frame;
 *   a file's peaks: its sound decoded from start to end, the loudest
 *     sample of each frame's worth kept.
 * Thumbnails come first (quick, and what's in view): peaks are made while
 * none is waiting.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "media.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_THUMBS  96   /* made, kept */
#define MAX_PENDING 48   /* asked for, not made yet */
#define MAX_PEAKS   (4 * 3600 * 60) /* frames: four hours at 60 a second */
#define THUMB_STEP 1   /* packets a step: a software-decoded HD picture takes tens of ms on the Pi */
#define PEAKS_STEP 16  /* (sound: quick) */
#define THUMB_PACKETS 400 /* read past the seek's keyframe, at most: then the nearest so far */
#define REPORT_FRAMES 125 /* peaks: shown again after this many more */

enum { PENDING, MADE, FAILED };

typedef struct
{
    char *file;
    int frame;
    int state;
    unsigned asked;   /* when last asked for (m->clock) */
    unsigned serial;
    unsigned char *px;
} thumb;

typedef struct
{
    char *file;
    int state;
    unsigned asked;
    unsigned char *peaks;
    int n, cap;
} peaks;

typedef struct
{
    char *file;
    int type; /* AVMEDIA_TYPE_VIDEO, _AUDIO */
    AVFormatContext *fc;
    AVCodecContext *cc;
    AVPacket *pkt;
    AVFrame *fr;
    int stream;
} reader;

struct re_media
{
    double fps;
    int w, h;
    unsigned clock, serials;
    thumb *th;
    int n_th, cap_th;
    peaks *pk;
    int n_pk, cap_pk;
    reader vr, ar;  /* the thumbnails' (video), the peaks' (sound) */
    struct
    {
        char *file;   /* (the entry's: found again by name after each step; NULL: no job) */
        int frame;
        int seeked, packets, have, flushed;
        int64_t ts, slack;
        AVFrame *last;
    } tj;
    struct
    {
        char *file;
        int seeked, based, reported;
        int64_t pos;  /* the next sample's number from the file's start */
    } pj;
};

/* ---- the reader ---------------------------------------------------------- */

/* The software decoder for a codec: not a hardware one (the VideoCore's
   decoders are for playing) */
static const AVCodec *software(enum AVCodecID id)
{
    const AVCodec *c;
    void *it = NULL;
    while ((c = av_codec_iterate(&it)))
        if (av_codec_is_decoder(c) && c->id == id
            && !(c->capabilities & (AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_EXPERIMENTAL)) && !strstr(c->name, "vchiq")
            && !strstr(c->name, "mmal") && !strstr(c->name, "v4l2") && !strstr(c->name, "cuvid"))
            return c;
    return NULL;
}

static void reader_close(reader *r)
{
    avcodec_free_context(&r->cc);
    avformat_close_input(&r->fc);
    av_packet_free(&r->pkt);
    av_frame_free(&r->fr);
    free(r->file);
    memset(r, 0, sizeof(*r));
}

/* The file open for its video or its sound (kept open if it is already). 0, or -1 */
static int reader_open(reader *r, const char *file, int type)
{
    if (r->fc && r->type == type && !strcmp(r->file, file))
        return 0;
    reader_close(r);
    if (avformat_open_input(&r->fc, file, NULL, NULL) < 0 || avformat_find_stream_info(r->fc, NULL) < 0)
        goto fail;
    r->stream = av_find_best_stream(r->fc, type, -1, -1, NULL, 0);
    if (r->stream < 0)
        goto fail;
    for (unsigned i = 0; i < r->fc->nb_streams; i++) /* (the others not even parsed) */
        r->fc->streams[i]->discard = (int) i == r->stream ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    AVStream *st = r->fc->streams[r->stream];
    const AVCodec *codec = software(st->codecpar->codec_id);
    if (!codec || !(r->cc = avcodec_alloc_context3(codec)) || avcodec_parameters_to_context(r->cc, st->codecpar) < 0)
        goto fail;
    r->cc->thread_count = 1;
    r->cc->pkt_timebase = st->time_base;
    if (avcodec_open2(r->cc, codec, NULL) < 0 || !(r->pkt = av_packet_alloc()) || !(r->fr = av_frame_alloc())
        || !(r->file = strdup(file)))
        goto fail;
    r->type = type;
    return 0;
fail:
    reader_close(r);
    return -1;
}

/* ---- the lists ----------------------------------------------------------- */

re_media *re_media_new(double fps, int w, int h)
{
    re_media *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->fps = fps > 0 ? fps : 25;
    m->w = w < 1 ? 1 : w;
    m->h = h < 1 ? 1 : h;
    return m;
}

static void thumb_job_end(re_media *m)
{
    av_frame_free(&m->tj.last);
    free(m->tj.file);
    memset(&m->tj, 0, sizeof(m->tj));
}

static void peaks_job_end(re_media *m)
{
    free(m->pj.file);
    memset(&m->pj, 0, sizeof(m->pj));
}

void re_media_free(re_media *m)
{
    if (!m)
        return;
    thumb_job_end(m);
    peaks_job_end(m);
    reader_close(&m->vr);
    reader_close(&m->ar);
    for (int i = 0; i < m->n_th; i++) {
        free(m->th[i].file);
        free(m->th[i].px);
    }
    for (int i = 0; i < m->n_pk; i++) {
        free(m->pk[i].file);
        free(m->pk[i].peaks);
    }
    free(m->th);
    free(m->pk);
    free(m);
}

void re_media_size(re_media *m, int *w, int *h)
{
    *w = m->w;
    *h = m->h;
}

static int is_job(re_media *m, const char *file, int frame)
{
    return m->tj.file && m->tj.frame == frame && !strcmp(m->tj.file, file);
}

static void thumb_drop(re_media *m, int i)
{
    free(m->th[i].file);
    free(m->th[i].px);
    m->th[i] = m->th[--m->n_th];
}

/* Too many made or waiting: the least recently asked for go (not the job's) */
static void thumbs_trim(re_media *m)
{
    for (int state = PENDING; state <= MADE; state++) {
        int max = state == MADE ? MAX_THUMBS : MAX_PENDING;
        for (;;) {
            int n = 0, oldest = -1;
            for (int i = 0; i < m->n_th; i++) {
                if (m->th[i].state != state)
                    continue;
                n++;
                if (!is_job(m, m->th[i].file, m->th[i].frame)
                    && (oldest < 0 || m->th[i].asked < m->th[oldest].asked))
                    oldest = i;
            }
            if (n <= max || oldest < 0)
                break;
            thumb_drop(m, oldest);
        }
    }
}

static thumb *thumb_find(re_media *m, const char *file, int frame)
{
    for (int i = 0; i < m->n_th; i++)
        if (m->th[i].frame == frame && !strcmp(m->th[i].file, file))
            return &m->th[i];
    return NULL;
}

/* Failed before (another frame of it): it isn't tried again */
static int thumbs_failed(re_media *m, const char *file)
{
    for (int i = 0; i < m->n_th; i++)
        if (m->th[i].state == FAILED && !strcmp(m->th[i].file, file))
            return 1;
    return 0;
}

const unsigned char *re_media_thumb(re_media *m, const char *file, int frame, unsigned *serial)
{
    if (!file || !*file)
        return NULL;
    if (frame < 0)
        frame = 0;
    thumb *t = thumb_find(m, file, frame);
    if (!t) {
        if (thumbs_failed(m, file))
            return NULL;
        if (m->n_th == m->cap_th) {
            int cap = m->cap_th ? m->cap_th * 2 : 32;
            thumb *more = realloc(m->th, (size_t) cap * sizeof(thumb));
            if (!more)
                return NULL;
            m->th = more;
            m->cap_th = cap;
        }
        t = &m->th[m->n_th];
        memset(t, 0, sizeof(*t));
        if (!(t->file = strdup(file)))
            return NULL;
        t->frame = frame;
        t->state = PENDING;
        m->n_th++;
    }
    t->asked = ++m->clock;
    if (t->state != MADE)
        return NULL;
    if (serial)
        *serial = t->serial;
    return t->px;
}

const unsigned char *re_media_thumb_near(re_media *m, const char *file, int frame, unsigned *serial)
{
    thumb *best = NULL;
    for (int i = 0; file && i < m->n_th; i++) {
        thumb *t = &m->th[i];
        if (t->state == MADE && !strcmp(t->file, file) && (!best || abs(t->frame - frame) < abs(best->frame - frame)))
            best = t;
    }
    if (!best)
        return NULL;
    if (serial)
        *serial = best->serial;
    return best->px;
}

static peaks *peaks_find(re_media *m, const char *file)
{
    for (int i = 0; i < m->n_pk; i++)
        if (!strcmp(m->pk[i].file, file))
            return &m->pk[i];
    return NULL;
}

const unsigned char *re_media_peaks(re_media *m, const char *file, int *n, int *done)
{
    *n = 0;
    *done = 0;
    if (!file || !*file)
        return NULL;
    peaks *p = peaks_find(m, file);
    if (!p) {
        if (m->n_pk == m->cap_pk) {
            int cap = m->cap_pk ? m->cap_pk * 2 : 16;
            peaks *more = realloc(m->pk, (size_t) cap * sizeof(peaks));
            if (!more)
                return NULL;
            m->pk = more;
            m->cap_pk = cap;
        }
        p = &m->pk[m->n_pk];
        memset(p, 0, sizeof(*p));
        if (!(p->file = strdup(file)))
            return NULL;
        p->state = PENDING;
        m->n_pk++;
    }
    p->asked = ++m->clock;
    *n = p->n;
    *done = p->state != PENDING;
    return p->n > 0 ? p->peaks : NULL;
}

int re_media_pending(re_media *m)
{
    int n = 0;
    for (int i = 0; i < m->n_th; i++)
        n += m->th[i].state == PENDING;
    for (int i = 0; i < m->n_pk; i++)
        n += m->pk[i].state == PENDING;
    return n;
}

/* ---- a thumbnail ---------------------------------------------------------- */

/* The frame scaled into the thumbnail's box (black bars): its pixels */
static unsigned char *picture(re_media *m, const AVFrame *f)
{
    unsigned char *px = calloc((size_t) m->w * m->h, 4);
    if (!px)
        return NULL;
    for (int i = 0; i < m->w * m->h; i++)
        px[i * 4 + 3] = 255;
    double sar = f->sample_aspect_ratio.num > 0 && f->sample_aspect_ratio.den > 0 ? av_q2d(f->sample_aspect_ratio) : 1;
    double dar = f->width * sar / (f->height > 0 ? f->height : 1);
    int fw = m->w, fh = m->h;
    if (dar > (double) m->w / m->h)
        fh = (int) (m->w / dar + 0.5);
    else
        fw = (int) (m->h * dar + 0.5);
    fw = fw < 1 ? 1 : fw > m->w ? m->w : fw;
    fh = fh < 1 ? 1 : fh > m->h ? m->h : fh;
    struct SwsContext *sws = sws_getContext(f->width, f->height, f->format, fw, fh, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL,
                                            NULL, NULL);
    if (!sws) {
        free(px);
        return NULL;
    }
    uint8_t *dst[4] = {px + ((size_t) ((m->h - fh) / 2) * m->w + (m->w - fw) / 2) * 4, NULL, NULL, NULL};
    int stride[4] = {m->w * 4, 0, 0, 0};
    sws_scale(sws, (const uint8_t *const *) f->data, f->linesize, 0, f->height, dst, stride);
    sws_freeContext(sws);
    for (int y = 0; y < m->h; y++) /* (a picture with alpha: as it shows on black) */
        for (int x = 0; x < m->w; x++)
            px[((size_t) y * m->w + x) * 4 + 3] = 255;
    return px;
}

/* The job's thumbnail made (from its last frame), or failed */
static int thumb_end(re_media *m, int made)
{
    thumb *t = thumb_find(m, m->tj.file, m->tj.frame);
    if (t) {
        t->px = made && m->tj.have ? picture(m, m->tj.last) : NULL;
        t->state = t->px ? MADE : FAILED;
        t->serial = ++m->serials;
    }
    thumb_job_end(m);
    thumbs_trim(m);
    return 1;
}

/* A frame decoded: kept; 1 it's the one (or past it) */
static int thumb_frame(re_media *m, AVFrame *f)
{
    int64_t pts = f->best_effort_timestamp;
    av_frame_unref(m->tj.last);
    av_frame_move_ref(m->tj.last, f);
    m->tj.have = 1;
    return pts == AV_NOPTS_VALUE || pts >= m->tj.ts - m->tj.slack;
}

static int thumb_step(re_media *m)
{
    reader *r = &m->vr;
    if (reader_open(r, m->tj.file, AVMEDIA_TYPE_VIDEO) < 0)
        return thumb_end(m, 0);
    AVStream *st = r->fc->streams[r->stream];
    if (!m->tj.seeked) {
        double tb = av_q2d(st->time_base);
        int64_t start = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
        m->tj.ts = start + (int64_t) (m->tj.frame / m->fps / tb + 0.5);
        m->tj.slack = (int64_t) (0.5 / m->fps / tb); /* (half a frame) */
        if (!(m->tj.last = av_frame_alloc()))
            return thumb_end(m, 0);
        if (avformat_seek_file(r->fc, r->stream, INT64_MIN, m->tj.ts, m->tj.ts, 0) < 0)
            avformat_seek_file(r->fc, -1, INT64_MIN, 0, 0, AVSEEK_FLAG_BYTE); /* (a picture: from its start) */
        avcodec_flush_buffers(r->cc);
        r->cc->skip_frame = AVDISCARD_NONREF;
        m->tj.seeked = 1;
    }
    for (int k = 0; k < THUMB_STEP; k++) {
        int e = m->tj.flushed ? AVERROR_EOF : av_read_frame(r->fc, r->pkt);
        if (e < 0) { /* the end: what the decoder still has */
            if (!m->tj.flushed) {
                avcodec_send_packet(r->cc, NULL);
                m->tj.flushed = 1;
            }
            while (avcodec_receive_frame(r->cc, r->fr) >= 0)
                if (thumb_frame(m, r->fr))
                    break;
            avcodec_flush_buffers(r->cc);
            return thumb_end(m, 1);
        }
        if (r->pkt->stream_index != r->stream) {
            av_packet_unref(r->pkt);
            continue;
        }
        e = avcodec_send_packet(r->cc, r->pkt);
        av_packet_unref(r->pkt);
        while (avcodec_receive_frame(r->cc, r->fr) >= 0)
            if (thumb_frame(m, r->fr))
                return thumb_end(m, 1);
        if (++m->tj.packets > THUMB_PACKETS && m->tj.have)
            return thumb_end(m, 1);
        if (e < 0 && e != AVERROR(EAGAIN) && m->tj.packets > THUMB_PACKETS)
            return thumb_end(m, m->tj.have);
    }
    return 0;
}

/* ---- peaks ---------------------------------------------------------------- */

/* The loudest of c samples from sample i, all channels: 0 to 1 */
static double loudest(const AVFrame *f, int i, int c)
{
    int ch = f->ch_layout.nb_channels > 0 ? f->ch_layout.nb_channels : 1;
    int planar = av_sample_fmt_is_planar(f->format);
    int planes = planar ? ch : 1, step = planar ? 1 : ch;
    double top = 0;
    for (int p = 0; p < planes && p < AV_NUM_DATA_POINTERS; p++) {
        /* (FFmpeg's sample buffers are aligned for their samples: av_malloc's, planes padded) */
        const void *d = f->extended_data[p];
        const uint8_t *u8 = d;
        size_t s0 = (size_t) i * step, s1 = (size_t) (i + c) * step;
        switch (av_get_packed_sample_fmt(f->format)) {
        case AV_SAMPLE_FMT_U8:
            for (size_t s = s0; s < s1; s++) {
                double v = fabs((u8[s] - 128) / 128.0);
                if (v > top) top = v;
            }
            break;
        case AV_SAMPLE_FMT_S16: {
            int big = 0;
            for (size_t s = s0; s < s1; s++) {
                int v = ((const int16_t *) d)[s];
                v = v < 0 ? -v : v;
                if (v > big) big = v;
            }
            if (big / 32768.0 > top) top = big / 32768.0;
            break;
        }
        case AV_SAMPLE_FMT_S32:
            for (size_t s = s0; s < s1; s++) {
                double v = fabs(((const int32_t *) d)[s] / 2147483648.0);
                if (v > top) top = v;
            }
            break;
        case AV_SAMPLE_FMT_FLT:
            for (size_t s = s0; s < s1; s++) {
                double v = fabs(((const float *) d)[s]);
                if (v > top) top = v;
            }
            break;
        case AV_SAMPLE_FMT_DBL:
            for (size_t s = s0; s < s1; s++) {
                double v = fabs(((const double *) d)[s]);
                if (v > top) top = v;
            }
            break;
        default: break;
        }
    }
    return top > 1 ? 1 : top;
}

static void peak_put(peaks *p, int64_t b, double v)
{
    if (b < 0 || b >= MAX_PEAKS)
        return;
    if (b >= p->cap) {
        int cap = p->cap ? p->cap : 256;
        while (cap <= b)
            cap *= 2;
        unsigned char *more = realloc(p->peaks, (size_t) cap);
        if (!more)
            return;
        memset(more + p->cap, 0, (size_t) (cap - p->cap));
        p->peaks = more;
        p->cap = cap;
    }
    if (b >= p->n)
        p->n = (int) b + 1;
    int byte = (int) (v * 255 + 0.5);
    if (byte > p->peaks[b])
        p->peaks[b] = (unsigned char) byte;
}

/* A decoded frame of sound: its samples' peaks into the frames they're in */
static void peaks_frame(re_media *m, peaks *p, const AVFrame *f)
{
    int rate = f->sample_rate > 0 ? f->sample_rate : 48000;
    if (!m->pj.based) { /* (from the first frame's time: a file whose sound starts late) */
        AVStream *st = m->ar.fc->streams[m->ar.stream];
        int64_t start = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
        int64_t t = f->best_effort_timestamp;
        m->pj.pos = t == AV_NOPTS_VALUE ? 0 : (int64_t) ((t - start) * av_q2d(st->time_base) * rate + 0.5);
        m->pj.based = 1;
    }
    for (int i = 0; i < f->nb_samples;) {
        int64_t s = m->pj.pos + i, b = (int64_t) floor(s * m->fps / rate);
        int64_t edge = (int64_t) ceil((b + 1) * rate / m->fps);
        int c = (int) (edge - s);
        if (c < 1) c = 1;
        if (c > f->nb_samples - i) c = f->nb_samples - i;
        peak_put(p, b, loudest(f, i, c));
        i += c;
    }
    m->pj.pos += f->nb_samples;
}

static int peaks_end(re_media *m, int made)
{
    peaks *p = peaks_find(m, m->pj.file);
    if (p)
        p->state = made && p->n > 0 ? MADE : FAILED;
    peaks_job_end(m);
    return 1;
}

static int peaks_step(re_media *m)
{
    reader *r = &m->ar;
    peaks *p = peaks_find(m, m->pj.file);
    if (!p)
        return peaks_end(m, 0);
    if (!m->pj.seeked) {
        if (reader_open(r, m->pj.file, AVMEDIA_TYPE_AUDIO) < 0)
            return peaks_end(m, 0);
        if (avformat_seek_file(r->fc, -1, INT64_MIN, 0, 0, AVSEEK_FLAG_BACKWARD) < 0)
            avformat_seek_file(r->fc, -1, INT64_MIN, 0, 0, AVSEEK_FLAG_BYTE);
        avcodec_flush_buffers(r->cc);
        m->pj.seeked = 1;
    } else if (reader_open(r, m->pj.file, AVMEDIA_TYPE_AUDIO) < 0) {
        return peaks_end(m, 0);
    }
    for (int k = 0; k < PEAKS_STEP; k++) {
        int e = av_read_frame(r->fc, r->pkt);
        if (e < 0) {
            avcodec_send_packet(r->cc, NULL);
            while (avcodec_receive_frame(r->cc, r->fr) >= 0) {
                peaks_frame(m, p, r->fr);
                av_frame_unref(r->fr);
            }
            avcodec_flush_buffers(r->cc);
            return peaks_end(m, 1);
        }
        if (r->pkt->stream_index == r->stream) {
            avcodec_send_packet(r->cc, r->pkt);
            while (avcodec_receive_frame(r->cc, r->fr) >= 0) {
                peaks_frame(m, p, r->fr);
                av_frame_unref(r->fr);
            }
        }
        av_packet_unref(r->pkt);
    }
    if (p->n - m->pj.reported >= REPORT_FRAMES) {
        m->pj.reported = p->n;
        return 1;
    }
    return 0;
}

/* ---- the work ------------------------------------------------------------- */

/* The most recently asked for of those waiting: a thumbnail, or peaks */
static thumb *next_thumb(re_media *m)
{
    thumb *t = NULL;
    for (int i = 0; i < m->n_th; i++)
        if (m->th[i].state == PENDING && (!t || m->th[i].asked > t->asked))
            t = &m->th[i];
    return t;
}

static peaks *next_peaks(re_media *m)
{
    peaks *p = NULL;
    for (int i = 0; i < m->n_pk; i++)
        if (m->pk[i].state == PENDING && (!p || m->pk[i].asked > p->asked))
            p = &m->pk[i];
    return p;
}

int re_media_step(re_media *m)
{
    if (m->tj.file && !thumb_find(m, m->tj.file, m->tj.frame))
        thumb_job_end(m); /* (dropped meanwhile) */
    if (!m->tj.file) {
        thumb *t = next_thumb(m);
        if (t) {
            if (!(m->tj.file = strdup(t->file)))
                return -1;
            m->tj.frame = t->frame;
        }
    }
    if (m->tj.file)
        return thumb_step(m);
    if (m->pj.file && !peaks_find(m, m->pj.file))
        peaks_job_end(m);
    if (!m->pj.file) {
        peaks *p = next_peaks(m);
        if (!p || !(m->pj.file = strdup(p->file))) {
            reader_close(&m->vr); /* (nothing to do: the decoders' memory freed; an HD one's is tens of MB) */
            reader_close(&m->ar);
            return -1;
        }
    }
    return peaks_step(m);
}
