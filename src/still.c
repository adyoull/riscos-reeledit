/* still.c - still pictures (PNG, JPEG) as MLT producers (still.h): a logo
 * or a title over the video, or a picture on V1.
 *
 * MLT reads a picture with its avformat producer, which decodes the file
 * again for every frame and converts it again (RGBA to the composite's
 * YUV, through swscale): fine on a PC, 200 ms a frame on a Pi. This one
 * decodes the file once and keeps the last picture asked for, at the size
 * and in the form asked for, alpha and all; each frame is a copy of it.
 * It knows a file by its first bytes, not its name: a RISC OS file has a
 * type, not an extension (and avformat finds pictures by extension).
 *
 * It's registered as "qimage", the name of MLT's Qt picture producer (not
 * in this build: no Qt): MLT's loader takes pictures to qimage first (its
 * loader.dict), and projects from Shotcut name their pictures qimage, so
 * pictures come here whoever made the project, and projects saved here
 * open in Shotcut.
 *
 * Pictures made rather than read (a title: title.c) are the same, made
 * again when what they're made from (the producer's properties) changes.
 *
 * Also: RGBA written as a PNG (by FFmpeg's encoder), for sprites.c.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "still.h"
#include "scale.h"

#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LENGTH 15000 /* (as MLT's qimage: frames; a clip of it is trimmed to what's wanted) */

typedef struct
{
    uint8_t *rgba; /* the file's picture */
    int width, height;
    /* the last picture handed out: at this size, in this form */
    uint8_t *image, *alpha;
    int image_size, alpha_size;
    mlt_image_format format, asked; /* (the form it's in; the form asked for, if it couldn't be made) */
    int w, h;
    /* a picture made, not read (a title: re_still_made): how, and from what
       it was last made */
    const re_still_maker *maker;
    char *made_from;
    /* for previews (consumer.fast_scale): the picture in yuv420p at its own
       size (made even), its alpha; other sizes scaled from it (scaled()) */
    uint8_t *base, *base_alpha;
    int bw, bh;
    int fast; /* (the picture handed out was scaled so: not for a frame that isn't a preview's) */
} still;

static void still_free(still *s)
{
    if (!s)
        return;
    free(s->rgba);
    free(s->image);
    free(s->alpha);
    free(s->made_from);
    free(s->base);
    free(s->base_alpha);
    free(s);
}

/* A whole file in memory (with FFmpeg's padding after it). NULL, or the
   bytes (av_free them) and their number. */
static uint8_t *slurp(const char *file, int *size)
{
    FILE *f = fopen(file, "rb");
    if (!f)
        return NULL;
    uint8_t *data = NULL;
    long n = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
    if (n > 0 && n < (64 << 20) && !fseek(f, 0, SEEK_SET)
        && (data = av_malloc(n + AV_INPUT_BUFFER_PADDING_SIZE))) {
        if (fread(data, 1, n, f) != (size_t) n) {
            av_free(data);
            data = NULL;
        } else {
            memset(data + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            *size = (int) n;
        }
    }
    fclose(f);
    return data;
}

/* What a file's first bytes say it is: a PNG, a JPEG, or neither (the
   name isn't asked: a RISC OS file has its type, not an extension) */
static enum AVCodecID kind(const uint8_t *d, int n)
{
    if (n > 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8))
        return AV_CODEC_ID_PNG;
    if (n > 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
        return AV_CODEC_ID_MJPEG;
    return AV_CODEC_ID_NONE;
}

/* The file decoded (by FFmpeg's decoders, not its demuxers), as RGBA. 0,
   or -1. */
static int decode(still *s, const char *file)
{
    int n = 0, ok = -1;
    uint8_t *data = slurp(file, &n);
    enum AVCodecID id = data ? kind(data, n) : AV_CODEC_ID_NONE;
    const AVCodec *codec = id != AV_CODEC_ID_NONE ? avcodec_find_decoder(id) : NULL;
    AVCodecContext *c = codec ? avcodec_alloc_context3(codec) : NULL;
    AVPacket *pkt = c ? av_packet_alloc() : NULL;
    AVFrame *fr = pkt ? av_frame_alloc() : NULL;
    if (fr && avcodec_open2(c, codec, NULL) >= 0) {
        pkt->data = data;
        pkt->size = n;
        if (avcodec_send_packet(c, pkt) >= 0)
            avcodec_send_packet(c, NULL);
        if (avcodec_receive_frame(c, fr) >= 0 && fr->width > 0 && fr->height > 0
            && (s->rgba = malloc((size_t) fr->width * fr->height * 4))) {
            struct SwsContext *sws = sws_getContext(fr->width, fr->height, fr->format, fr->width, fr->height,
                                                    AV_PIX_FMT_RGBA, SWS_POINT | SWS_ACCURATE_RND, NULL, NULL, NULL);
            if (sws) {
                uint8_t *out[4] = {s->rgba};
                int out_stride[4] = {fr->width * 4};
                sws_scale(sws, (const uint8_t *const *) fr->data, fr->linesize, 0, fr->height, out, out_stride);
                sws_freeContext(sws);
                s->width = fr->width;
                s->height = fr->height;
                ok = 0;
            }
        }
    }
    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&c);
    av_free(data);
    return ok;
}

/* The picture at w x h, in the form asked for, made from the file's (and
   kept). 0, or -1. */
static int make(still *s, mlt_frame frame, mlt_image_format format, int w, int h)
{
    uint8_t *scaled = malloc((size_t) w * h * 4);
    if (!scaled)
        return -1;
    if (w == s->width && h == s->height) {
        memcpy(scaled, s->rgba, (size_t) w * h * 4);
    } else {
        int flags = (w < s->width ? SWS_AREA : SWS_BICUBIC) | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
        struct SwsContext *sws = sws_getContext(s->width, s->height, AV_PIX_FMT_RGBA, w, h, AV_PIX_FMT_RGBA, flags,
                                                NULL, NULL, NULL);
        if (!sws) {
            free(scaled);
            return -1;
        }
        const uint8_t *in[4] = {s->rgba};
        int in_stride[4] = {s->width * 4};
        uint8_t *out[4] = {scaled};
        int out_stride[4] = {w * 4};
        sws_scale(sws, in, in_stride, 0, s->height, out, out_stride);
        sws_freeContext(sws);
    }
    /* into the form asked for, by the frame's converters (the loader's
       filters put them on it; they make the alpha plane for a form without
       alpha in it): the frame's picture for now, a copy kept */
    mlt_properties fp = MLT_FRAME_PROPERTIES(frame);
    mlt_frame_set_image(frame, scaled, w * h * 4, free);
    mlt_frame_set_alpha(frame, NULL, 0, NULL);
    mlt_properties_set_int(fp, "format", mlt_image_rgba);
    mlt_properties_set_int(fp, "width", w);
    mlt_properties_set_int(fp, "height", h);
    uint8_t *img = scaled;
    mlt_image_format fmt = mlt_image_rgba;
    if (format != mlt_image_rgba && format != mlt_image_none && format != mlt_image_movit
        && mlt_frame_has_convert_image(frame) && mlt_frame_convert_image(frame, &img, &fmt, format)) {
        img = mlt_properties_get_data(fp, "image", NULL); /* (not converted: RGBA it stays) */
        fmt = mlt_image_rgba;
    }
    struct mlt_image_s mi;
    mlt_image_set_values(&mi, NULL, fmt, w, h);
    int size = mlt_image_calculate_size(&mi), asize = 0;
    uint8_t *alpha = fmt == mlt_image_rgba ? NULL : mlt_frame_get_alpha_size(frame, &asize);
    free(s->image);
    free(s->alpha);
    s->image = img ? malloc(size) : NULL;
    s->alpha = alpha && asize >= w * h ? malloc(w * h) : NULL;
    if (s->image)
        memcpy(s->image, img, size);
    if (s->alpha)
        memcpy(s->alpha, alpha, w * h);
    if (!s->image) {
        free(s->alpha);
        s->alpha = NULL;
        return -1;
    }
    s->image_size = size;
    s->alpha_size = s->alpha ? w * h : 0;
    s->format = fmt;
    s->asked = format;
    s->w = w;
    s->h = h;
    return 0;
}

/* A preview's yuv420p at w x h (chroma w/2 x h/2, as MLT lays out an odd
   size) scaled from the picture in yuv420p
   at its own size (made once: RGBA to yuv420p by the frame's converters,
   as make() does), not scaled and converted again from RGBA for every
   size: a logo or title whose size is keyframed changes size every
   frame. 0, or -1 (make() then). */
static int scaled(still *s, mlt_frame frame, int w, int h)
{
    if (w < 2 || h < 2)
        return -1;
    if (!s->base) {
        int bw = s->width & ~1, bh = s->height & ~1;
        if (bw < 2 || bh < 2 || make(s, frame, mlt_image_yuv420p, bw, bh) || s->format != mlt_image_yuv420p)
            return -1;
        s->base = s->image;
        s->base_alpha = s->alpha;
        s->image = s->alpha = NULL;
        s->bw = bw;
        s->bh = bh;
    }
    int bw = s->bw, bh = s->bh, size = w * h * 3 / 2;
    uint8_t *img = malloc(size), *a = s->base_alpha ? malloc((size_t) w * h) : NULL;
    if (!img || (s->base_alpha && !a)) {
        free(img);
        free(a);
        return -1;
    }
    const uint8_t *bu = s->base + bw * bh, *bv = bu + (bw / 2) * (bh / 2);
    uint8_t *u = img + w * h, *v = u + (w / 2) * (h / 2);
    if (re_scale_fit(s->base, bw, bh, img, w, h) || re_scale_fit(bu, bw / 2, bh / 2, u, w / 2, h / 2)
        || re_scale_fit(bv, bw / 2, bh / 2, v, w / 2, h / 2) || (a && re_scale_fit(s->base_alpha, bw, bh, a, w, h))) {
        free(img);
        free(a);
        return -1;
    }
    free(s->image);
    free(s->alpha);
    s->image = img;
    s->alpha = a;
    s->image_size = size;
    s->alpha_size = a ? w * h : 0;
    s->format = s->asked = mlt_image_yuv420p;
    s->w = w;
    s->h = h;
    return 0;
}

/* A made picture made again if what it's made from has changed (the
   producer's properties). 0, or -1 (none). */
static int ensure(mlt_producer producer, still *s)
{
    if (!s->maker)
        return s->rgba ? 0 : -1;
    mlt_properties pp = MLT_PRODUCER_PROPERTIES(producer);
    char key[2048];
    s->maker->key(pp, key, sizeof(key));
    if (s->rgba && s->made_from && !strcmp(s->made_from, key))
        return 0;
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    if (s->maker->make(pp, &rgba, &w, &h) || !rgba || w < 1 || h < 1) {
        free(rgba);
        return -1;
    }
    free(s->rgba);
    free(s->image);
    free(s->alpha);
    free(s->made_from);
    s->rgba = rgba;
    s->width = w;
    s->height = h;
    s->image = s->alpha = NULL;
    free(s->base);
    free(s->base_alpha);
    s->base = s->base_alpha = NULL;
    s->made_from = strdup(key);
    mlt_properties_set_int(pp, "meta.media.width", w);
    mlt_properties_set_int(pp, "meta.media.height", h);
    return 0;
}

static int get_image(mlt_frame frame, uint8_t **buffer, mlt_image_format *format, int *width, int *height,
                     int writable)
{
    (void) writable;
    mlt_producer producer = mlt_frame_pop_service(frame);
    still *s = mlt_properties_get_data(MLT_PRODUCER_PROPERTIES(producer), "_still", NULL);
    if (!s)
        return 1;
    if (!s->rgba)
        return 1;
    int w = *width > 0 ? *width : s->width, h = *height > 0 ? *height : s->height;
    /* (the loader's rescale filter asks for the picture at its own size and
       says here the size it wants: made at that size, kept, it scales
       nothing each frame) */
    mlt_properties fp0 = MLT_FRAME_PROPERTIES(frame);
    const char *interp = mlt_properties_get(fp0, "consumer.rescale");
    int rw = mlt_properties_get_int(fp0, "rescale_width"), rh = mlt_properties_get_int(fp0, "rescale_height");
    if (rw > 0 && rh > 0 && !(interp && !strcmp(interp, "none"))) {
        w = rw;
        h = rh;
    }
    mlt_image_format want = *format == mlt_image_none ? mlt_image_rgba : *format;
    mlt_service_lock(MLT_PRODUCER_SERVICE(producer));
    int fast = want == mlt_image_yuv420p && mlt_properties_get_int(fp0, "consumer.fast_scale")
               && (w != s->width || h != s->height);
    if (!s->image || s->w != w || s->h != h || s->asked != want || !s->rgba || (s->fast && !fast)) {
        s->fast = fast && !scaled(s, frame, w, h);
        if (!s->fast && make(s, frame, want, w, h)) {
            mlt_service_unlock(MLT_PRODUCER_SERVICE(producer));
            return 1;
        }
    }
    uint8_t *copy = mlt_pool_alloc(s->image_size);
    if (copy)
        memcpy(copy, s->image, s->image_size);
    if (copy && s->alpha) {
        uint8_t *a = mlt_pool_alloc(s->alpha_size);
        if (a) {
            memcpy(a, s->alpha, s->alpha_size);
            mlt_frame_set_alpha(frame, a, s->alpha_size, mlt_pool_release);
        }
    }
    *format = s->format;
    mlt_service_unlock(MLT_PRODUCER_SERVICE(producer));
    if (!copy)
        return 1;
    mlt_frame_set_image(frame, copy, s->image_size, mlt_pool_release);
    *buffer = copy;
    *width = w;
    *height = h;
    mlt_properties fp = MLT_FRAME_PROPERTIES(frame);
    mlt_properties_set_int(fp, "format", *format);
    mlt_properties_set_int(fp, "width", w);
    mlt_properties_set_int(fp, "height", h);
    return 0;
}

static int get_frame(mlt_producer producer, mlt_frame_ptr frame, int index)
{
    (void) index;
    still *s = mlt_properties_get_data(MLT_PRODUCER_PROPERTIES(producer), "_still", NULL);
    if (s)
        ensure(producer, s); /* (mlt_service_get_frame holds the producer's lock) */
    *frame = mlt_frame_init(MLT_PRODUCER_SERVICE(producer));
    if (!*frame)
        return 1;
    mlt_properties fp = MLT_FRAME_PROPERTIES(*frame);
    mlt_frame_set_position(*frame, mlt_producer_position(producer));
    mlt_properties_set_int(fp, "progressive", 1);
    mlt_properties_set_double(fp, "aspect_ratio", 1.0);
    mlt_properties_set_int(fp, "meta.media.width", s ? s->width : 0);
    mlt_properties_set_int(fp, "meta.media.height", s ? s->height : 0);
    mlt_properties_set_int(fp, "width", s ? s->width : 0);
    mlt_properties_set_int(fp, "height", s ? s->height : 0);
    mlt_frame_push_service(*frame, producer);
    mlt_frame_push_get_image(*frame, get_image);
    mlt_producer_prepare_next(producer);
    return 0;
}

/* A producer of a still picture, s (its own from here) */
static mlt_producer producer_of(mlt_profile profile, still *s, const char *service, const char *resource)
{
    mlt_producer p = mlt_producer_new(profile);
    if (!p) {
        still_free(s);
        return NULL;
    }
    mlt_properties pp = MLT_PRODUCER_PROPERTIES(p);
    mlt_properties_set(pp, "resource", resource);
    mlt_properties_set(pp, "mlt_service", service);
    mlt_properties_set_position(pp, "length", LENGTH);
    mlt_properties_set_position(pp, "out", LENGTH - 1);
    mlt_properties_set_int(pp, "meta.media.width", s->width);
    mlt_properties_set_int(pp, "meta.media.height", s->height);
    mlt_properties_set_int(pp, "progressive", 1);
    mlt_properties_set_double(pp, "aspect_ratio", 1.0);
    mlt_properties_set_int(pp, "seekable", 1);
    mlt_properties_set_data(pp, "_still", s, 0, (mlt_destructor) still_free, NULL);
    p->get_frame = get_frame;
    return p;
}

static mlt_producer still_init(mlt_profile profile, mlt_service_type type, const char *id, char *arg)
{
    (void) type;
    (void) id;
    if (!arg || !*arg)
        return NULL;
    still *s = calloc(1, sizeof(*s));
    if (!s || decode(s, arg)) {
        still_free(s);
        return NULL;
    }
    return producer_of(profile, s, "qimage", arg);
}

mlt_producer re_still_made(mlt_profile profile, const char *service, const char *resource, const re_still_maker *maker)
{
    still *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->maker = maker;
    return producer_of(profile, s, service, resource ? resource : "");
}

int re_still_size(mlt_producer producer, int *width, int *height)
{
    mlt_producer parent = producer ? mlt_producer_cut_parent(producer) : NULL;
    if (!parent)
        parent = producer;
    still *s = parent ? mlt_properties_get_data(MLT_PRODUCER_PROPERTIES(parent), "_still", NULL) : NULL;
    if (!s)
        return -1;
    mlt_service_lock(MLT_PRODUCER_SERVICE(parent));
    int bad = ensure(parent, s);
    *width = s->width;
    *height = s->height;
    mlt_service_unlock(MLT_PRODUCER_SERVICE(parent));
    return bad;
}

void re_still_register(void)
{
    mlt_repository r = mlt_factory_repository();
    if (r)
        mlt_repository_register(r, mlt_service_producer_type, "qimage", (mlt_register_callback) still_init);
}

const char *re_still_kind[4] = {"qimage"};
int re_still_kinds = 1;

int re_still_is(mlt_producer producer)
{
    if (!producer)
        return 0;
    mlt_producer parent = mlt_producer_cut_parent(producer);
    const char *s = mlt_properties_get(MLT_PRODUCER_PROPERTIES(parent ? parent : producer), "mlt_service");
    if (!s)
        return 0;
    for (int i = 0; i < re_still_kinds; i++)
        if (!strcmp(s, re_still_kind[i]))
            return 1;
    return 0;
}

int re_still_open(const char *file, int *width, int *height)
{
    still s = {0};
    if (decode(&s, file))
        return -1;
    free(s.rgba);
    *width = s.width;
    *height = s.height;
    return 0;
}

int re_still_write_png(const char *file, const uint8_t *rgba, int width, int height)
{
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    AVCodecContext *c = codec ? avcodec_alloc_context3(codec) : NULL;
    AVFrame *fr = c ? av_frame_alloc() : NULL;
    AVPacket *pkt = fr ? av_packet_alloc() : NULL;
    int ok = -1;
    if (pkt) {
        c->width = width;
        c->height = height;
        c->pix_fmt = AV_PIX_FMT_RGBA;
        c->time_base = (AVRational){1, 25};
        fr->format = AV_PIX_FMT_RGBA;
        fr->width = width;
        fr->height = height;
        fr->data[0] = (uint8_t *) rgba;
        fr->linesize[0] = width * 4;
        if (avcodec_open2(c, codec, NULL) >= 0 && avcodec_send_frame(c, fr) >= 0 && avcodec_send_frame(c, NULL) >= 0
            && avcodec_receive_packet(c, pkt) >= 0) {
            FILE *f = fopen(file, "wb");
            if (f) {
                ok = fwrite(pkt->data, 1, pkt->size, f) == (size_t) pkt->size ? 0 : -1;
                if (fclose(f))
                    ok = -1;
                if (ok)
                    remove(file);
            }
        }
    }
    av_packet_free(&pkt);
    av_frame_free(&fr);
    avcodec_free_context(&c);
    return ok;
}
