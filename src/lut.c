/* lut.c - "avfilter.lutyuv" for this build: colour.c's tables made here.
 *
 * A clip's colour (colour.c) is FFmpeg's lutyuv filter, through MLT's
 * avformat module, so Shotcut opens it. Each time its graph is made (a
 * clip's first picture, a setting changed) FFmpeg 5.1's lutyuv works its
 * expressions out for all 65536 values of each of its tables, 8-bit or not:
 * about 400 ms on the testbed, a stall at the cut into a coloured clip and
 * at each step of a slider; and every picture was copied into an AVFrame
 * and back.
 *
 * Registered over MLT's, this one knows colour.c's own expressions (the
 * numbers in them read back), makes the three 256-entry tables from them
 * as lutyuv would (the same sums in doubles, then (int) and clipped to
 * 0..255), and looks the picture up in them where it is (yuv420p, or
 * yuv422). Any other expression (a project from Shotcut): MLT's own
 * avfilter.lutyuv does it (filter_avfilter_init, given its properties).
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (the avformat module's: the filter for any "avfilter.NAME") */
extern mlt_filter filter_avfilter_init(mlt_profile, mlt_service_type, const char *, char *);

#define LUT_Y "clip(16+219*((pow(clip((val-16)/219,0,1),%lf)-0.5)*%lf+0.5+%lf),0,255)%n"
#define LUT_U "clip(128+(val-128)*%lf-(%lf),0,255)%n"
#define LUT_V "clip(128+(val-128)*%lf+(%lf),0,255)%n"

typedef struct
{
    char *y, *u, *v;     /* the expressions the tables were made from */
    int ours;            /* colour.c's: the tables below; else MLT's filter */
    uint8_t t[3][256];
} lut_state;

static double clipd(double x, double lo, double hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

static uint8_t entry(double res)
{
    int v = (int) res;
    return (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v);
}

/* The tables for colour.c's expressions: 0, or -1 (not colour.c's) */
int re_lut_tables(const char *y, const char *u, const char *v, uint8_t t[3][256])
{
    double g, k, b, s, w, s2, w2;
    int n = -1;
    if (!y || !u || !v || sscanf(y, LUT_Y, &g, &k, &b, &n) != 3 || n < 0 || y[n])
        return -1;
    n = -1;
    if (sscanf(u, LUT_U, &s, &w, &n) != 2 || n < 0 || u[n])
        return -1;
    n = -1;
    if (sscanf(v, LUT_V, &s2, &w2, &n) != 2 || n < 0 || v[n])
        return -1;
    for (int val = 0; val < 256; val++) {
        double x = val;
        t[0][val] = entry(clipd(16 + 219 * ((pow(clipd((x - 16) / 219, 0, 1), g) - 0.5) * k + 0.5 + b), 0, 255));
        t[1][val] = entry(clipd(128 + (x - 128) * s - (w), 0, 255));
        t[2][val] = entry(clipd(128 + (x - 128) * s2 + (w2), 0, 255));
    }
    return 0;
}

static int same(const char *a, const char *b)
{
    return a && b ? !strcmp(a, b) : a == b;
}

static void state_free(lut_state *st)
{
    if (!st)
        return;
    free(st->y);
    free(st->u);
    free(st->v);
    free(st);
}

static char *dup(const char *s)
{
    return s ? strdup(s) : NULL;
}

/* The tables (made again if the expressions changed): 1 ours, 0 not */
static int tables(mlt_filter filter, lut_state *st)
{
    mlt_properties fp = MLT_FILTER_PROPERTIES(filter);
    const char *y = mlt_properties_get(fp, "av.y"), *u = mlt_properties_get(fp, "av.u"),
               *v = mlt_properties_get(fp, "av.v");
    if (!(same(y, st->y) && same(u, st->u) && same(v, st->v) && (st->y || st->u || st->v))) {
        free(st->y);
        free(st->u);
        free(st->v);
        st->y = dup(y);
        st->u = dup(u);
        st->v = dup(v);
        st->ours = !re_lut_tables(y, u, v, st->t);
    }
    return st->ours;
}

static void look_up(uint8_t *p, size_t n, const uint8_t *t)
{
    for (size_t i = 0; i < n; i++)
        p[i] = t[p[i]];
}

static int get_image(mlt_frame frame, uint8_t **image, mlt_image_format *format, int *width, int *height,
                     int writable)
{
    mlt_filter filter = mlt_frame_pop_service(frame);
    lut_state *st = mlt_properties_get_data(MLT_FILTER_PROPERTIES(filter), "_lut", NULL);
    (void) writable;
    if (*format != mlt_image_yuv420p && *format != mlt_image_yuv422)
        *format = mlt_image_yuv420p;
    int err = mlt_frame_get_image(frame, image, format, width, height, 1);
    if (err || !*image || !st)
        return err;
    int w = *width, h = *height;
    mlt_service_lock(MLT_FILTER_SERVICE(filter));
    if (*format == mlt_image_yuv420p) {
        uint8_t *u = *image + (size_t) w * h, *v = u + (size_t) (w / 2) * (h / 2);
        look_up(*image, (size_t) w * h, st->t[0]);
        look_up(u, (size_t) (w / 2) * (h / 2), st->t[1]);
        look_up(v, (size_t) (w / 2) * (h / 2), st->t[2]);
    } else if (*format == mlt_image_yuv422) { /* (Y U Y V) */
        uint8_t *p = *image;
        for (size_t i = 0; i < (size_t) w * h / 2; i++, p += 4) {
            p[0] = st->t[0][p[0]];
            p[1] = st->t[1][p[1]];
            p[2] = st->t[0][p[2]];
            p[3] = st->t[2][p[3]];
        }
    }
    mlt_service_unlock(MLT_FILTER_SERVICE(filter));
    return 0;
}

/* MLT's own avfilter.lutyuv, given this one's properties */
static mlt_filter delegate(mlt_filter filter)
{
    mlt_properties fp = MLT_FILTER_PROPERTIES(filter);
    mlt_filter d = mlt_properties_get_data(fp, "_lut_avfilter", NULL);
    if (!d) {
        mlt_profile profile = mlt_service_profile(MLT_FILTER_SERVICE(filter));
        d = filter_avfilter_init(profile, mlt_service_filter_type, "avfilter.lutyuv", NULL);
        if (!d)
            return NULL;
        mlt_service_set_profile(MLT_FILTER_SERVICE(d), profile); /* (as MLT's factory does) */
        mlt_properties_set_data(fp, "_lut_avfilter", d, 0, (mlt_destructor) mlt_filter_close, NULL);
    }
    mlt_properties dp = MLT_FILTER_PROPERTIES(d);
    for (int i = 0; i < mlt_properties_count(fp); i++) {
        const char *name = mlt_properties_get_name(fp, i), *value = mlt_properties_get_value(fp, i);
        if (name && !strncmp(name, "av.", 3) && !same(value, mlt_properties_get(dp, name)))
            mlt_properties_set(dp, name, value);
    }
    if (mlt_filter_get_in(d) != mlt_filter_get_in(filter) || mlt_filter_get_out(d) != mlt_filter_get_out(filter))
        mlt_filter_set_in_and_out(d, mlt_filter_get_in(filter), mlt_filter_get_out(filter));
    return d;
}

static mlt_frame process(mlt_filter filter, mlt_frame frame)
{
    lut_state *st = mlt_properties_get_data(MLT_FILTER_PROPERTIES(filter), "_lut", NULL);
    if (!st)
        return frame;
    mlt_service_lock(MLT_FILTER_SERVICE(filter));
    int ours = tables(filter, st);
    mlt_service_unlock(MLT_FILTER_SERVICE(filter));
    if (!ours) {
        mlt_filter d = delegate(filter);
        return d ? mlt_filter_process(d, frame) : frame;
    }
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, get_image);
    return frame;
}

static mlt_filter lut_init(mlt_profile profile, mlt_service_type type, const char *id, char *arg)
{
    (void) profile;
    (void) type;
    (void) id;
    (void) arg;
    mlt_filter f = mlt_filter_new();
    lut_state *st = calloc(1, sizeof(lut_state));
    if (!f || !st) {
        free(st);
        mlt_filter_close(f);
        return NULL;
    }
    mlt_properties fp = MLT_FILTER_PROPERTIES(f);
    mlt_properties_set_data(fp, "_lut", st, 0, (mlt_destructor) state_free, NULL);
    mlt_properties_set_int(fp, "_reeledit_lut", 1); /* (ours, not the avformat module's) */
    f->process = process;
    return f;
}

void re_lut_register(void)
{
    mlt_repository r = mlt_factory_repository();
    if (r) /* (over the avformat module's) */
        mlt_repository_register(r, mlt_service_filter_type, "avfilter.lutyuv", (mlt_register_callback) lut_init);
}
