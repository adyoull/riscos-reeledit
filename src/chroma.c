/* chroma.c - the "chroma" filter for this build (key.c's green screen):
 * the pixels near a colour made see-through, by the frame's alpha.
 *
 * The plus module's chroma (the same properties: "key", a colour, and
 * "variance", 0..1, how near to it) asks for yuv422 and compares each
 * pixel's Cb and Cr: an overlay in ReelEdit's yuv420p pipeline was turned
 * into yuv422 and back for it. This one, registered over it, works in
 * yuv420p when that's what's asked for: each chroma sample compared
 * (NEON: neon.h's re_neon_key) and the four pixels it covers keyed
 * together. Asked for another form, it does as plus's does, in yuv422.
 *
 * From MLT's plus module (filter_chroma.c: LGPL 2.1 or later, Copyright
 * (C) 2007-2025 Meltytech, LLC), reworked.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project.h"
#include "neon.h"

#include <stdlib.h>
#include <string.h>

/* The alpha (made opaque if there's none) */
static uint8_t *alpha_of(mlt_frame frame, int w, int h)
{
    int size = 0;
    uint8_t *a = mlt_frame_get_alpha_size(frame, &size);
    if (a && size >= w * h)
        return a;
    a = mlt_pool_alloc(w * h);
    if (!a)
        return NULL;
    memset(a, 255, (size_t) w * h);
    mlt_frame_set_alpha(frame, a, w * h, mlt_pool_release);
    return a;
}

static int in_range(int x, int c, int var)
{
    return x >= c - var && x <= c + var;
}

static int get_image(mlt_frame frame, uint8_t **image, mlt_image_format *format, int *width, int *height,
                     int writable)
{
    mlt_filter filter = mlt_frame_pop_service(frame);
    mlt_properties fp = MLT_FILTER_PROPERTIES(filter);
    mlt_position pos = mlt_filter_get_position(filter, frame), len = mlt_filter_get_length2(filter, frame);
    int var = (int) (200 * mlt_properties_anim_get_double(fp, "variance", pos, len));
    mlt_color key = mlt_properties_anim_get_color(fp, "key", pos, len);
    key = mlt_color_convert_trc(key, mlt_properties_get(MLT_FRAME_PROPERTIES(frame), "color_trc"));
    int ku, kv;
    RGB2UV_601_SCALED(key.r, key.g, key.b, ku, kv);
    if (*format != mlt_image_yuv420p)
        *format = mlt_image_yuv422;
    int err = mlt_frame_get_image(frame, image, format, width, height, writable);
    if (err || !*image)
        return err;
    int w = *width, h = *height;
    uint8_t *a = alpha_of(frame, w, h);
    if (!a)
        return 0;
    if (*format == mlt_image_yuv420p) {
        const uint8_t *u = *image + w * h, *v = u + (w / 2) * (h / 2);
        for (int y = 0; y + 1 < h; y += 2) {
            const uint8_t *ur = u + (y / 2) * (w / 2), *vr = v + (y / 2) * (w / 2);
            uint8_t *a0 = a + y * w, *a1 = a0 + w;
            if (re_neon_key(ur, vr, a0, a1, w / 2, ku, kv, var))
                continue;
            for (int i = 0; i < w / 2; i++)
                if (in_range(ur[i], ku, var) && in_range(vr[i], kv, var))
                    a0[2 * i] = a0[2 * i + 1] = a1[2 * i] = a1[2 * i + 1] = 0;
        }
    } else if (*format == mlt_image_yuv422) { /* (as plus's) */
        const uint8_t *p = *image;
        for (int i = 0; i < w * h / 2; i++, p += 4) {
            if (in_range(p[1], ku, var) && in_range(p[3], kv, var))
                a[2 * i] = 0;
            int uo = i + 1 < w * h / 2 ? (p[1] + p[5]) / 2 : p[1], vo = i + 1 < w * h / 2 ? (p[3] + p[7]) / 2 : p[3];
            if (in_range(uo, ku, var) && in_range(vo, kv, var))
                a[2 * i + 1] = 0;
        }
    }
    return 0;
}

static mlt_frame process(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, get_image);
    return frame;
}

static mlt_filter chroma_init(mlt_profile profile, mlt_service_type type, const char *id, char *arg)
{
    (void) profile;
    (void) type;
    (void) id;
    mlt_filter f = mlt_filter_new();
    if (f) {
        mlt_properties fp = MLT_FILTER_PROPERTIES(f);
        mlt_properties_set(fp, "key", arg ? arg : "#0000ff");
        mlt_properties_set_double(fp, "variance", 0.15);
        mlt_properties_set_int(fp, "_reeledit_chroma", 1); /* (ours, not plus's) */
        f->process = process;
    }
    return f;
}

void re_chroma_register(void)
{
    mlt_repository r = mlt_factory_repository();
    if (r) /* (over the plus module's) */
        mlt_repository_register(r, mlt_service_filter_type, "chroma", (mlt_register_callback) chroma_init);
}
