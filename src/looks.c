/* looks.c - effect.c's special effects for this build: the "plus"
 * module's invert, threshold and charcoal filters (the same properties,
 * the same sums), registered over plus's.
 *
 * Plus's ask for yuv422: a clip in ReelEdit's yuv420p pipeline was turned
 * into yuv422 and back for them, and charcoal works out a square root by
 * a loop for every pixel. These work in yuv420p when that's what's asked
 * for (the luma of each pixel; the chroma at its own size), charcoal's
 * edges eight at a time with NEON where there is (neon.h); asked for
 * another form, they do as plus's do, in yuv422.
 *
 * From MLT's plus module (filter_invert.c, filter_threshold.c and
 * filter_charcoal.c: LGPL 2.1 or later, Copyright (C) 2003-2022 Meltytech,
 * LLC), reworked.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project.h"
#include "neon.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { INVERT, THRESHOLD, CHARCOAL };

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* The largest whole number whose square is at most n (charcoal's) */
static int isqrt(int n)
{
    int s = (int) sqrt((double) n);
    while (s * s > n)
        s--;
    while ((s + 1) * (s + 1) <= n)
        s++;
    return s;
}

typedef struct
{
    int min, max_luma, max_chroma, invert_luma;
    int x_scatter, y_scatter, invert;
    float scale, mix;
} charcoal_settings;

/* charcoal's value for one pixel from its 3x3 (outside the picture: max_luma) */
static uint8_t charcoal_pixel(const charcoal_settings *c, int m00, int m01, int m02, int m10, int m12, int m20,
                              int m21, int m22)
{
    int sum1 = (m20 - m00) + ((m21 - m01) << 1) + (m22 - m20);
    int sum2 = (m02 - m00) + ((m12 - m10) << 1) + (m22 - m20);
    float sum = c->scale * isqrt(sum1 * sum1 + sum2 * sum2);
    return (uint8_t) (!c->invert ? (sum >= c->min && sum <= c->max_luma ? c->invert_luma - sum
                                    : sum < c->min                      ? c->max_luma
                                                                        : c->min)
                                 : (sum >= c->min && sum <= c->max_luma ? sum
                                    : sum < c->min                      ? c->min
                                                                        : c->max_luma));
}

/* charcoal's values for each q = sum1^2 + sum2^2 below lut_n (the rest all *sat): a table, so no square root for
   a pixel (sum = scale * isqrt(q) is past max once isqrt(q) is past max / scale) */
static uint8_t *charcoal_table(const charcoal_settings *c, int *lut_n, uint8_t *sat)
{
    double most = c->scale > 0 ? c->max_luma / c->scale + 2 : 65536;
    int n = most * most < 65536 ? (int) (most * most) : 65536;
    uint8_t *t = malloc((size_t) n);
    if (!t)
        return NULL;
    for (int q = 0; q < n; q++) {
        float sum = c->scale * isqrt(q);
        t[q] = (uint8_t) (!c->invert ? (sum >= c->min && sum <= c->max_luma ? c->invert_luma - sum
                                        : sum < c->min                      ? c->max_luma
                                                                            : c->min)
                                     : (sum >= c->min && sum <= c->max_luma ? sum
                                        : sum < c->min                      ? c->min
                                                                            : c->max_luma));
    }
    /* (past the table: past max if it was big enough to hold everything below it, else worked out each time) */
    *sat = (uint8_t) (!c->invert ? c->min : c->max_luma);
    *lut_n = n;
    return t;
}

/* A plane of luma (step: from a pixel to the next one along, 1 in yuv420p, 2 in yuv422; pitch: from a row to
   the next) made a charcoal drawing, into out (the same layout) */
static void charcoal_luma(const charcoal_settings *c, const uint8_t *in, uint8_t *out, int w, int h, int step,
                          int pitch)
{
    int xs = c->x_scatter, ys = c->y_scatter, edge = c->max_luma, lut_n = 0;
    uint8_t sat = 0, *lut = charcoal_table(c, &lut_n, &sat);
    int whole = lut && lut_n < 65536; /* (every q past the table saturates) */
#define Y(x, y) ((x) < 0 || (x) >= w || (y) < 0 || (y) >= h ? edge : in[(y) * pitch + (x) * step])
    for (int y = 0; y < h; y++) {
        int inside = y - ys >= 0 && y + ys < h;
        for (int x = 0; x < w; x++) {
            if (inside && x == xs && whole && x < w - xs) { /* (the middle of a row: no edges to check) */
                const uint8_t *a0 = in + (y - ys) * pitch, *a1 = in + y * pitch, *a2 = in + (y + ys) * pitch;
                int done = step == 1 ? re_neon_charcoal(a0 + xs, a1 + xs, a2 + xs, out + y * pitch + xs, w - 2 * xs, xs,
                                                        lut, lut_n, sat)
                                     : 0;
                for (x = xs + done; x < w - xs; x++) {
                    int o = x * step, d = xs * step;
                    int m00 = a0[o - d], m01 = a0[o], m02 = a0[o + d], m10 = a1[o - d], m12 = a1[o + d];
                    int m20 = a2[o - d], m21 = a2[o], m22 = a2[o + d];
                    int sum1 = (m20 - m00) + ((m21 - m01) << 1) + (m22 - m20);
                    int sum2 = (m02 - m00) + ((m12 - m10) << 1) + (m22 - m20);
                    int q = sum1 * sum1 + sum2 * sum2;
                    out[y * pitch + o] = q < lut_n ? lut[q] : sat;
                }
                if (x >= w)
                    break;
            }
            out[y * pitch + x * step] =
                charcoal_pixel(c, Y(x - xs, y - ys), Y(x, y - ys), Y(x + xs, y - ys), Y(x - xs, y), Y(x + xs, y),
                               Y(x - xs, y + ys), Y(x, y + ys), Y(x + xs, y + ys));
        }
    }
#undef Y
    free(lut);
}

static int get_image(mlt_frame frame, uint8_t **image, mlt_image_format *format, int *width, int *height,
                     int writable)
{
    mlt_filter filter = mlt_frame_pop_service(frame);
    mlt_properties fp = MLT_FILTER_PROPERTIES(filter);
    int kind = mlt_properties_get_int(fp, "_reeledit_look");
    mlt_position pos = mlt_filter_get_position(filter, frame), len = mlt_filter_get_length2(filter, frame);
    if (*format != mlt_image_yuv420p)
        *format = mlt_image_yuv422;
    int err = mlt_frame_get_image(frame, image, format, width, height, kind == CHARCOAL ? 0 : 1);
    if (err || !*image)
        return err;
    int w = *width, h = *height, planar = *format == mlt_image_yuv420p;
    if (*format != mlt_image_yuv420p && *format != mlt_image_yuv422)
        return 0;
    mlt_properties frp = MLT_FRAME_PROPERTIES(frame);
    int full = mlt_properties_get_int(frp, "full_range");
    int min = full ? 0 : 16, max_luma = full ? 255 : 235, max_chroma = full ? 255 : 240, inv = full ? 255 : 251;
    uint8_t *p = *image;
    if (kind == INVERT) {
        if (planar) {
            if (!re_neon_invert(p, w * h, inv, 0, min, max_luma))
                for (int i = 0; i < w * h; i++)
                    p[i] = (uint8_t) clampi(inv - p[i], min, max_luma);
            if (!re_neon_invert(p + w * h, (w / 2) * (h / 2) * 2, 255, 1, min, max_chroma))
                for (int i = w * h; i < w * h + (w / 2) * (h / 2) * 2; i++)
                    p[i] = (uint8_t) clampi(256 - p[i], min, max_chroma);
        } else {
            for (int i = 0; i < w * h * 2; i += 2) {
                p[i] = (uint8_t) clampi(inv - p[i], min, max_luma);
                p[i + 1] = (uint8_t) clampi(256 - p[i + 1], min, max_chroma);
            }
        }
        int mask = mlt_properties_get_int(fp, "alpha");
        if (mask) {
            uint8_t *alpha = mlt_pool_alloc(w * h);
            if (alpha) {
                memset(alpha, mask, (size_t) w * h);
                mlt_frame_set_alpha(frame, alpha, w * h, mlt_pool_release);
            }
        }
    } else if (kind == THRESHOLD) {
        int midpoint = mlt_properties_anim_get_int(fp, "midpoint", pos, len);
        int use_alpha = mlt_properties_get_int(fp, "use_alpha"), invert = mlt_properties_get_int(fp, "invert");
        int full_luma = mlt_properties_get_int(frp, "full_luma");
        uint8_t white = full_luma ? 255 : 235, black = full_luma ? 0 : 16;
        uint8_t a_hi = invert ? black : white, a_lo = invert ? white : black; /* (plus's B and A) */
        uint8_t *alpha = use_alpha ? mlt_frame_get_alpha(frame) : NULL;
        int step = planar ? 1 : 2;
        for (int i = 0; i < w * h; i++) {
            int on = use_alpha ? !alpha || alpha[i] >= midpoint : p[i * step] >= midpoint;
            p[i * step] = on ? a_hi : a_lo;
            if (!planar)
                p[i * 2 + 1] = 128;
        }
        if (planar)
            memset(p + w * h, 128, (size_t) (w / 2) * (h / 2) * 2);
    } else {
        charcoal_settings c = {min, max_luma, max_chroma, inv, 0, 0, 0, 0, 0};
        c.x_scatter = (int) mlt_properties_anim_get_double(fp, "x_scatter", pos, len);
        c.y_scatter = (int) mlt_properties_anim_get_double(fp, "y_scatter", pos, len);
        mlt_profile profile = mlt_service_profile(MLT_FILTER_SERVICE(filter));
        double sx = mlt_profile_scale_width(profile, w), sy = mlt_profile_scale_height(profile, h);
        if (sx > 0.0 || sy > 0.0) {
            c.x_scatter = (int) lrint(c.x_scatter * sx) < 1 ? 1 : (int) lrint(c.x_scatter * sx);
            c.y_scatter = (int) lrint(c.y_scatter * sy) < 1 ? 1 : (int) lrint(c.y_scatter * sy);
        }
        c.invert = mlt_properties_anim_get_int(fp, "invert", pos, len);
        c.scale = (float) mlt_properties_anim_get_double(fp, "scale", pos, len);
        c.mix = (float) mlt_properties_anim_get_double(fp, "mix", pos, len);
        int size = planar ? w * h + (w / 2) * (h / 2) * 2 : w * h * 2;
        uint8_t *out = mlt_pool_alloc(size);
        if (!out)
            return 0;
        charcoal_luma(&c, p, out, w, h, planar ? 1 : 2, planar ? w : w * 2);
        if (planar) {
            for (int i = w * h; i < size; i++)
                out[i] = (uint8_t) clampi((int) (128 + c.mix * (p[i] - 128)), c.min, c.max_chroma);
        } else {
            for (int i = 1; i < size; i += 2)
                out[i] = (uint8_t) clampi((int) (128 + c.mix * (p[i] - 128)), c.min, c.max_chroma);
        }
        *image = out;
        mlt_frame_set_image(frame, out, size, mlt_pool_release);
    }
    return 0;
}

static mlt_frame process(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, get_image);
    return frame;
}

static mlt_filter look_init(mlt_profile profile, mlt_service_type type, const char *id, char *arg)
{
    (void) profile;
    (void) type;
    (void) arg;
    mlt_filter f = mlt_filter_new();
    if (!f)
        return NULL;
    mlt_properties fp = MLT_FILTER_PROPERTIES(f);
    int kind = !strcmp(id, "invert") ? INVERT : !strcmp(id, "threshold") ? THRESHOLD : CHARCOAL;
    mlt_properties_set_int(fp, "_reeledit_look", kind);
    if (kind == THRESHOLD) {
        mlt_properties_set_int(fp, "midpoint", 128);
        mlt_properties_set_int(fp, "use_alpha", 0);
        mlt_properties_set_int(fp, "invert", 0);
    } else if (kind == CHARCOAL) {
        mlt_properties_set_int(fp, "x_scatter", 1);
        mlt_properties_set_int(fp, "y_scatter", 1);
        mlt_properties_set_double(fp, "scale", 1.5);
        mlt_properties_set_double(fp, "mix", 0.0);
    }
    f->process = process;
    return f;
}

void re_looks_register(void)
{
    mlt_repository r = mlt_factory_repository();
    if (!r)
        return;
    mlt_repository_register(r, mlt_service_filter_type, "invert", (mlt_register_callback) look_init);
    mlt_repository_register(r, mlt_service_filter_type, "threshold", (mlt_register_callback) look_init);
    mlt_repository_register(r, mlt_service_filter_type, "charcoal", (mlt_register_callback) look_init);
}
