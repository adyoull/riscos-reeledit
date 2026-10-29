/* affine.c - the "affine" filter for this build (project.h's
 * re_motion_register; motion.c's pan and zoom use it): a clip's picture
 * put where an animated rect says ("transition.rect", in the profile's
 * pixels), as Shotcut's Size, Position & Rotate filter puts it.
 *
 * MLT's own affine (in its "plus" module: not in this build) composites
 * through a transition, in RGBA. This one does only what a rect needs:
 * the picture below asked for at the rect's size (a picture scaled once,
 * by still.c or MLT's rescale), then the frame's part of it copied out, on
 * black where it doesn't cover the frame, in the format asked for
 * (yuv420p, yuv422, RGB, RGBA; others as yuv422). Rotation and opacity
 * aren't done. Registered only if there's no other affine.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project.h"

#include <math.h>
#include <string.h>

#define BIGGEST 4 /* (a rect at most so many times the frame: memory) */

static void copy_rows(uint8_t *to, int to_stride, const uint8_t *from, int from_stride, int bytes, int rows)
{
    for (int y = 0; y < rows; y++)
        memcpy(to + y * to_stride, from + y * from_stride, bytes);
}

/* The part of a picture (sw x sh, at ox, oy in the frame) in the frame
   (w x h), on black; NULL if the format isn't one of these */
static uint8_t *place(const uint8_t *src, mlt_image_format fmt, int sw, int sh, int ox, int oy, int w, int h,
                      int *size)
{
    int bpp = fmt == mlt_image_rgba ? 4 : fmt == mlt_image_rgb ? 3 : fmt == mlt_image_yuv422 ? 2 : 0;
    if (fmt != mlt_image_yuv420p && !bpp)
        return NULL;
    if (fmt == mlt_image_yuv420p || fmt == mlt_image_yuv422) { /* (chroma in pairs) */
        ox &= ~1;
        if (fmt == mlt_image_yuv420p)
            oy &= ~1;
    }
    *size = fmt == mlt_image_yuv420p ? w * h + (w / 2) * (h / 2) * 2 : w * h * bpp;
    uint8_t *out = mlt_pool_alloc(*size);
    if (!out)
        return NULL;
    /* black */
    if (fmt == mlt_image_yuv420p) {
        memset(out, 16, (size_t) w * h);
        memset(out + w * h, 128, (size_t) (w / 2) * (h / 2) * 2);
    } else if (fmt == mlt_image_yuv422) {
        for (int i = 0; i < w * h; i++) {
            out[2 * i] = 16;
            out[2 * i + 1] = 128;
        }
    } else {
        memset(out, 0, (size_t) w * h * bpp);
        if (bpp == 4)
            for (int i = 0; i < w * h; i++)
                out[4 * i + 3] = 255;
    }
    /* the part that's in the frame: frame x0..x1, y0..y1 */
    int x0 = ox > 0 ? ox : 0, y0 = oy > 0 ? oy : 0;
    int x1 = ox + sw < w ? ox + sw : w, y1 = oy + sh < h ? oy + sh : h;
    if (x1 <= x0 || y1 <= y0)
        return out;
    int sx = x0 - ox, sy = y0 - oy;
    if (fmt == mlt_image_yuv420p) {
        copy_rows(out + y0 * w + x0, w, src + sy * sw + sx, sw, x1 - x0, y1 - y0);
        int cw = w / 2, ch = h / 2, csw = sw / 2, csh = sh / 2;
        int cx0 = x0 / 2, cy0 = y0 / 2, cx1 = x1 / 2, cy1 = y1 / 2;
        for (int k = 0; k < 2; k++)
            copy_rows(out + w * h + k * cw * ch + cy0 * cw + cx0, cw, src + sw * sh + k * csw * csh + (sy / 2) * csw + sx / 2,
                      csw, cx1 - cx0, cy1 - cy0);
    } else {
        copy_rows(out + (y0 * w + x0) * bpp, w * bpp, src + (sy * sw + sx) * bpp, sw * bpp, (x1 - x0) * bpp, y1 - y0);
    }
    return out;
}

static int get_image(mlt_frame frame, uint8_t **image, mlt_image_format *format, int *width, int *height,
                     int writable)
{
    mlt_filter filter = mlt_frame_pop_service(frame);
    mlt_properties fp = MLT_FILTER_PROPERTIES(filter);
    mlt_profile profile = mlt_service_profile(MLT_FILTER_SERVICE(filter));
    int w = *width > 0 ? *width : profile->width, h = *height > 0 ? *height : profile->height;
    mlt_position pos = mlt_filter_get_position(filter, frame), len = mlt_filter_get_length2(filter, frame);
    mlt_service_lock(MLT_FILTER_SERVICE(filter));
    mlt_rect r = mlt_properties_anim_get_rect(fp, "transition.rect", pos, len);
    mlt_service_unlock(MLT_FILTER_SERVICE(filter));
    double sx = (double) w / profile->width, sy = (double) h / profile->height;
    int rw = (int) lround(r.w * sx) & ~1, rh = (int) lround(r.h * sy) & ~1;
    int ox = (int) lround(r.x * sx), oy = (int) lround(r.y * sy);
    if (rw < 2 || rh < 2 || rw > w * BIGGEST || rh > h * BIGGEST || (ox == 0 && oy == 0 && rw == w && rh == h))
        return mlt_frame_get_image(frame, image, format, width, height, writable); /* (as it is) */
    mlt_image_format asked = *format;
    if (asked != mlt_image_yuv420p && asked != mlt_image_yuv422 && asked != mlt_image_rgb && asked != mlt_image_rgba)
        asked = mlt_image_yuv422;
    int gw = rw, gh = rh;
    mlt_image_format got = asked;
    uint8_t *src = NULL;
    mlt_properties frp = MLT_FRAME_PROPERTIES(frame);
    const char *interp = mlt_properties_get(frp, "consumer.rescale");
    if (interp && !strcmp(interp, "none")) /* (the engine's "as it comes": here, scaled to the rect's size) */
        mlt_properties_set(frp, "consumer.rescale", "bilinear");
    int err = mlt_frame_get_image(frame, &src, &got, &gw, &gh, 0);
    if (err || !src)
        return err ? err : 1;
    int size = 0;
    uint8_t *out = got == asked && gw == rw && gh == rh ? place(src, got, gw, gh, ox, oy, w, h, &size) : NULL;
    if (!out) { /* (not given at the size asked for: as it came) */
        *image = src;
        *format = got;
        *width = gw;
        *height = gh;
        return 0;
    }
    mlt_frame_set_image(frame, out, size, mlt_pool_release);
    mlt_frame_set_alpha(frame, NULL, 0, NULL); /* (the picture's was for the whole of it) */
    *image = out;
    *format = got;
    *width = w;
    *height = h;
    mlt_properties_set_int(MLT_FRAME_PROPERTIES(frame), "width", w);
    mlt_properties_set_int(MLT_FRAME_PROPERTIES(frame), "height", h);
    return 0;
}

static mlt_frame process(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, get_image);
    return frame;
}

static mlt_filter motion_init(mlt_profile profile, mlt_service_type type, const char *id, char *arg)
{
    (void) profile;
    (void) type;
    (void) id;
    (void) arg;
    mlt_filter f = mlt_filter_new();
    if (f)
        f->process = process;
    return f;
}

void re_motion_register(void)
{
    mlt_repository r = mlt_factory_repository();
    if (r && !mlt_properties_get_data(mlt_repository_filters(r), "affine", NULL))
        mlt_repository_register(r, mlt_service_filter_type, "affine", (mlt_register_callback) motion_init);
}
