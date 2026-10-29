/* hold.c - a frame of a video held as a still picture (project.h's freeze
 * frames: re_project_freeze).
 *
 * Its producer is "reeleditFreeze", a still (still.c) made from:
 *   resource                  the video
 *   shotcut:reeledit_time     the frame's time in it (seconds)
 * made by opening the video (MLT's loader, at the video's own frame
 * rate), seeking to the frame and taking its picture at the video's size,
 * once (and again only if those change). Being a still, it's shown at any
 * size and in any form, as a picture is, and has no sound.
 *
 * (MLT's own "hold" producer keeps the first picture asked for, at that
 * size and in that form, and says no more scaling is to be done: the
 * Viewer's size and an export's would differ.)
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project.h"
#include "still.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void key(mlt_properties p, char *k, int size)
{
    const char *r = mlt_properties_get(p, "resource");
    snprintf(k, size, "%.6f|%s", mlt_properties_get_double(p, "shotcut:reeledit_time"), r ? r : "");
}

static int make(mlt_properties p, uint8_t **rgba, int *w, int *h)
{
    const char *file = mlt_properties_get(p, "resource");
    if (!file || !*file)
        return -1;
    mlt_profile prof = mlt_profile_init(NULL);
    if (!prof)
        return -1;
    int bad = -1;
    mlt_producer v = mlt_factory_producer(prof, NULL, file);
    if (v) {
        mlt_profile_from_producer(prof, v); /* (its own size and rate) */
        mlt_producer_close(v);
        v = mlt_factory_producer(prof, NULL, file);
    }
    if (v) {
        double t = mlt_properties_get_double(p, "shotcut:reeledit_time");
        int pos = (int) floor(t * mlt_profile_fps(prof) + 0.5), len = mlt_producer_get_length(v);
        mlt_producer_seek(v, pos >= len ? len - 1 : pos < 0 ? 0 : pos);
        mlt_frame f = NULL;
        mlt_service_get_frame(MLT_PRODUCER_SERVICE(v), &f, 0);
        mlt_image_format fmt = mlt_image_rgba;
        uint8_t *img = NULL;
        int iw = prof->width, ih = prof->height;
        if (f && !mlt_frame_get_image(f, &img, &fmt, &iw, &ih, 0) && img && fmt == mlt_image_rgba && iw > 0 && ih > 0) {
            *rgba = malloc((size_t) iw * ih * 4);
            if (*rgba) {
                memcpy(*rgba, img, (size_t) iw * ih * 4);
                for (int i = 0; i < iw * ih; i++) /* (a frame of a video: no see-through parts) */
                    (*rgba)[4 * i + 3] = 255;
                *w = iw;
                *h = ih;
                bad = 0;
            }
        }
        if (f)
            mlt_frame_close(f);
        mlt_producer_close(v);
    }
    mlt_profile_close(prof);
    return bad;
}

static const re_still_maker MAKER = {key, make};

static mlt_producer hold_init(mlt_profile profile, mlt_service_type type, const char *id, char *arg)
{
    (void) type;
    (void) id;
    return re_still_made(profile, "reeleditFreeze", arg, &MAKER);
}

void re_hold_register(void)
{
    mlt_repository r = mlt_factory_repository();
    if (r)
        mlt_repository_register(r, mlt_service_producer_type, "reeleditFreeze", (mlt_register_callback) hold_init);
    if (re_still_kinds < 4)
        re_still_kind[re_still_kinds++] = "reeleditFreeze";
}
