/* colour.c - a clip's colour (project.h): brightness, contrast, saturation,
 * warmth and midtones, and looks made of them (black and white, sepia...).
 *
 * One filter on the clip's cut: FFmpeg's lutyuv (through MLT's avformat
 * module, "avfilter.lutyuv"), its tables made from the settings, so each
 * pixel costs a table look-up in yuv420p, the format the engine asks for
 * (no conversion to RGB). Marked "shotcut:filter" reeleditColour, its
 * settings kept with it as shotcut:reeledit_brightness and so on, the
 * tables' expressions made from them:
 *   Y: gamma (midtones), then contrast about the middle, then brightness,
 *      on the video range (16..235)
 *   U, V: saturation about the middle, then warmth (U down and V up: more
 *      red, less blue; the other way: cooler)
 *
 * A dissolve's ends are cuts of their own (as clipsound.c's): rp_fit_colour
 * gives each its clip's colour after every edit and when a project is
 * read, so a clip keeps its colour through its dissolves.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project_internal.h"
#include "still.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define COLOUR "reeleditColour"

static const char *const KEYS[5] = {"shotcut:reeledit_brightness", "shotcut:reeledit_contrast",
                                    "shotcut:reeledit_saturation", "shotcut:reeledit_warmth",
                                    "shotcut:reeledit_midtones"};

void re_colour_none(re_colour *c)
{
    c->brightness = 0;
    c->contrast = 100;
    c->saturation = 100;
    c->warmth = 0;
    c->midtones = 0;
}

int re_colour_is_none(const re_colour *c)
{
    return c->brightness == 0 && c->contrast == 100 && c->saturation == 100 && c->warmth == 0 && c->midtones == 0;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void clamp(re_colour *c)
{
    c->brightness = clampi(c->brightness, -100, 100);
    c->contrast = clampi(c->contrast, 0, 200);
    c->saturation = clampi(c->saturation, 0, 200);
    c->warmth = clampi(c->warmth, -100, 100);
    c->midtones = clampi(c->midtones, -100, 100);
}

const char *const re_colour_looks[RE_LOOKS] = {"None",  "Vivid",          "Warm",  "Cool",
                                               "Faded", "Black and white", "Sepia", "Night"};

void re_colour_look(int look, re_colour *c)
{
    re_colour_none(c);
    switch (look) {
    case 1: c->contrast = 110; c->saturation = 140; break;                     /* Vivid */
    case 2: c->warmth = 30; c->saturation = 110; break;                        /* Warm */
    case 3: c->warmth = -30; break;                                            /* Cool */
    case 4: c->contrast = 80; c->brightness = 8; c->saturation = 70; break;    /* Faded */
    case 5: c->saturation = 0; c->contrast = 110; break;                       /* Black and white */
    case 6: c->saturation = 0; c->warmth = 55; break;                          /* Sepia */
    case 7: c->brightness = -20; c->saturation = 60; c->warmth = -45; break;   /* Night */
    }
}

int re_colour_look_of(const re_colour *c)
{
    re_colour l;
    for (int i = 0; i < RE_LOOKS; i++) {
        re_colour_look(i, &l);
        if (!memcmp(&l, c, sizeof(l)))
            return i;
    }
    return -1;
}

/* The settings read from a cut's filter (none: as it is) */
static void read_filter(mlt_filter f, re_colour *c)
{
    re_colour_none(c);
    if (!f)
        return;
    mlt_properties fp = MLT_FILTER_PROPERTIES(f);
    int *v[5] = {&c->brightness, &c->contrast, &c->saturation, &c->warmth, &c->midtones};
    for (int k = 0; k < 5; k++)
        if (mlt_properties_get(fp, KEYS[k]))
            *v[k] = mlt_properties_get_int(fp, KEYS[k]);
}

/* The filter's tables made from the settings */
static void write_filter(mlt_filter f, const re_colour *c)
{
    mlt_properties fp = MLT_FILTER_PROPERTIES(f);
    int *v[5] = {(int *) &c->brightness, (int *) &c->contrast, (int *) &c->saturation, (int *) &c->warmth,
                 (int *) &c->midtones};
    for (int k = 0; k < 5; k++)
        mlt_properties_set_int(fp, KEYS[k], *v[k]);
    double g = pow(2.0, -c->midtones / 100.0), k = c->contrast / 100.0, b = c->brightness / 200.0;
    double s = c->saturation / 100.0, w = c->warmth * 0.2;
    char y[200], u[100], vv[100];
    snprintf(y, sizeof(y), "clip(16+219*((pow(clip((val-16)/219,0,1),%.4f)-0.5)*%.4f+0.5+%.4f),0,255)", g, k, b);
    snprintf(u, sizeof(u), "clip(128+(val-128)*%.4f-(%.3f),0,255)", s, w);
    snprintf(vv, sizeof(vv), "clip(128+(val-128)*%.4f+(%.3f),0,255)", s, w);
    mlt_properties_set(fp, "av.y", y);
    mlt_properties_set(fp, "av.u", u);
    mlt_properties_set(fp, "av.v", vv);
}

int rp_cut_colour(mlt_producer cut, re_colour *c)
{
    mlt_filter f = cut ? rp_cut_filter(cut, COLOUR) : NULL;
    read_filter(f, c);
    return f != NULL;
}

void rp_cut_set_colour(re_project *p, mlt_producer cut, const re_colour *c)
{
    mlt_filter f = rp_cut_filter(cut, COLOUR);
    if (re_colour_is_none(c)) {
        if (f)
            mlt_service_detach(MLT_PRODUCER_SERVICE(cut), f);
        return;
    }
    if (!f) {
        f = mlt_factory_filter(re_project_profile(p), "avfilter.lutyuv", NULL);
        if (!f)
            return;
        mlt_properties_set(MLT_FILTER_PROPERTIES(f), "shotcut:filter", COLOUR);
        mlt_service_attach(MLT_PRODUCER_SERVICE(cut), f);
        mlt_filter_close(f);
    }
    write_filter(f, c);
}

void rp_fit_colour(re_project *p)
{
    int n = p->v1 ? mlt_playlist_count(p->v1) : 0;
    for (int i = 0; i < n; i++) {
        mlt_tractor t = rp_mix_of(p, i);
        if (!t)
            continue;
        mlt_multitrack mt = mlt_tractor_multitrack(t);
        for (int k = 0; k < 2 && k < mlt_multitrack_count(mt); k++) {
            int clip = k ? i + 1 : i - 1;
            mlt_producer end = mlt_multitrack_track(mt, k);
            if (!end || !mlt_producer_is_cut(end)) /* (never the file's producer: every clip of it shares it) */
                continue;
            re_colour want, now;
            if (rp_is_clip(p, clip))
                rp_cut_colour(mlt_playlist_get_clip(p->v1, clip), &want);
            else
                re_colour_none(&want);
            rp_cut_colour(end, &now);
            if (memcmp(&want, &now, sizeof(now)))
                rp_cut_set_colour(p, end, &want);
        }
    }
}

int re_project_colour(re_project *p, int item, re_colour *c)
{
    if (!rp_is_clip(p, item)) {
        re_colour_none(c);
        return -1;
    }
    rp_cut_colour(mlt_playlist_get_clip(p->v1, item), c);
    return 0;
}

int re_project_set_colour(re_project *p, int item, const re_colour *c)
{
    re_colour want = *c, now;
    clamp(&want);
    if (re_project_colour(p, item, &now) || !memcmp(&now, &want, sizeof(now)))
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    rp_cut_set_colour(p, mlt_playlist_get_clip(p->v1, item), &want);
    rp_changed(p);
    return 1;
}
