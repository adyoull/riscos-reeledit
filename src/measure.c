/* measure.c - a clip's length found by reading it through (measure.h).
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "measure.h"

#include <libavformat/avformat.h>
#include <stdlib.h>

struct re_measure
{
    AVFormatContext *fc;
    AVPacket *pkt;
    int stream;
    int64_t size, first, end; /* (end: the last packet's dts + duration, in the stream's time base) */
    long packets;
    double fps;
};

re_measure *re_measure_open(const char *file)
{
    re_measure *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->first = m->end = AV_NOPTS_VALUE;
    if (avformat_open_input(&m->fc, file, NULL, NULL) < 0)
        goto fail;
    if (avformat_find_stream_info(m->fc, NULL) < 0)
        goto fail;
    m->stream = av_find_best_stream(m->fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (m->stream < 0)
        goto fail;
    AVStream *st = m->fc->streams[m->stream];
    AVRational r = st->avg_frame_rate.num > 0 ? st->avg_frame_rate : st->r_frame_rate;
    m->fps = r.num > 0 && r.den > 0 ? av_q2d(r) : 25;
    m->size = m->fc->pb ? avio_size(m->fc->pb) : -1;
    m->pkt = av_packet_alloc();
    if (!m->pkt)
        goto fail;
    /* (from the start: find_stream_info read some of it) */
    if (av_seek_frame(m->fc, -1, 0, AVSEEK_FLAG_BYTE | AVSEEK_FLAG_BACKWARD) < 0 && m->fc->pb)
        avio_seek(m->fc->pb, 0, SEEK_SET);
    avformat_flush(m->fc);
    return m;
fail:
    re_measure_close(m);
    return NULL;
}

int re_measure_step(re_measure *m, int packets)
{
    while (packets-- > 0) {
        int e = av_read_frame(m->fc, m->pkt);
        if (e == AVERROR_EOF)
            return 1;
        if (e < 0)
            return m->packets > 0 ? 1 : -1; /* (a damaged end: what was read) */
        if (m->pkt->stream_index == m->stream) {
            int64_t t = m->pkt->dts != AV_NOPTS_VALUE ? m->pkt->dts : m->pkt->pts;
            m->packets++;
            if (t != AV_NOPTS_VALUE) {
                if (m->first == AV_NOPTS_VALUE || t < m->first)
                    m->first = t;
                int64_t end = t + (m->pkt->duration > 0 ? m->pkt->duration : 0);
                if (m->end == AV_NOPTS_VALUE || end > m->end)
                    m->end = end;
            }
        }
        av_packet_unref(m->pkt);
    }
    return 0;
}

double re_measure_progress(re_measure *m)
{
    if (!m->fc->pb || m->size <= 0)
        return 0;
    double p = (double) avio_tell(m->fc->pb) / (double) m->size;
    return p < 0 ? 0 : p > 1 ? 1 : p;
}

double re_measure_seconds(re_measure *m)
{
    AVStream *st = m->fc->streams[m->stream];
    if (m->first != AV_NOPTS_VALUE && m->end > m->first)
        return (double) (m->end - m->first) * av_q2d(st->time_base);
    return m->packets / m->fps; /* (no times: a frame a packet) */
}

void re_measure_close(re_measure *m)
{
    if (!m)
        return;
    av_packet_free(&m->pkt);
    avformat_close_input(&m->fc);
    free(m);
}
