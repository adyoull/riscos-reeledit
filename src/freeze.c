/* freeze.c - freeze frames and colour clips (project.h): clips that aren't
 * a file's frames.
 *
 * A freeze frame: a frame of a clip held, put in at that frame (the clip
 * split there, what follows moving on). Its producer is hold.c's
 * "reeleditFreeze" (the clip's video and the frame's time in it), opened
 * through MLT's loader as a picture is, so it's fitted to the frame as
 * the clip was. A still: no sound; no speed.
 *
 * A colour clip: MLT's "color" producer (resource "#AARRGGBB", as Shotcut
 * makes its colour clips), put in at a frame as a freeze frame is.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project_internal.h"
#include "proxy.h"
#include "still.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LENGTH 15000 /* (frames there are of either, as a picture has) */

const char *const re_fill_names[RE_FILLS] = {"Black", "White", "Grey", "Red", "Orange", "Yellow", "Green", "Blue",
                                             "Purple"};
const unsigned re_fill_rgbs[RE_FILLS] = {0x000000, 0xFFFFFF, 0x808080, 0xD02020, 0xF08020, 0xF0D020, 0x20A040,
                                         0x2050C0, 0x8030A0};

static mlt_producer parent_of(mlt_producer cut)
{
    mlt_producer p = cut ? mlt_producer_cut_parent(cut) : NULL;
    return p ? p : cut;
}

static int service_is(mlt_producer cut, const char *service)
{
    mlt_producer p = parent_of(cut);
    const char *s = p ? mlt_properties_get(MLT_PRODUCER_PROPERTIES(p), "mlt_service") : NULL;
    return s && !strcmp(s, service);
}

int rp_cut_fill(mlt_producer cut, unsigned *rgb)
{
    if (!cut || !(service_is(cut, "color") || service_is(cut, "colour")))
        return 0;
    const char *res = mlt_properties_get(MLT_PRODUCER_PROPERTIES(parent_of(cut)), "resource");
    const char *slash = res ? strrchr(res, '/') : NULL; /* (MLT's XML reader puts the project's directory before it) */
    mlt_properties c1 = mlt_properties_new();
    mlt_properties_set(c1, "c", slash ? slash + 1 : res ? res : "");
    mlt_color c = mlt_properties_get_color(c1, "c");
    mlt_properties_close(c1);
    *rgb = ((unsigned) c.r << 16) | ((unsigned) c.g << 8) | c.b;
    return 1;
}

int rp_cut_held(mlt_producer cut, double *time)
{
    if (!cut || !service_is(cut, "reeleditFreeze"))
        return 0;
    *time = mlt_properties_get_double(MLT_PRODUCER_PROPERTIES(parent_of(cut)), "shotcut:reeledit_time");
    return 1;
}

const char *re_fill_name(unsigned rgb)
{
    for (int i = 0; i < RE_FILLS; i++)
        if (re_fill_rgbs[i] == rgb)
            return re_fill_names[i];
    return NULL;
}

static int put(re_project *p, mlt_producer prod, int frame, int length)
{
    if (!prod)
        return -1;
    mlt_properties pp = MLT_PRODUCER_PROPERTIES(prod);
    mlt_properties_set_position(pp, "length", LENGTH);
    mlt_properties_set_position(pp, "out", LENGTH - 1);
    if (length < 1)
        length = re_project_frames(p, 2);
    if (length > LENGTH)
        length = LENGTH;
    mlt_producer cut = mlt_producer_cut(prod, 0, length - 1);
    mlt_producer_close(prod); /* (the cut has it) */
    return cut ? rp_insert_cut(p, cut, frame) : -1;
}

int re_project_freeze(re_project *p, int frame, int length)
{
    re_item it;
    int i = re_project_item_at(p, frame);
    if (i < 0 || re_project_item(p, i, &it) || it.gap || it.mix || it.picture)
        return -1;
    const char *file = re_project_original(p, re_project_item_file(p, i));
    if (!file)
        return -1;
    char res[1100];
    snprintf(res, sizeof(res), "reeleditFreeze:%s", file);
    mlt_producer prod = mlt_factory_producer(p->profile, NULL, res);
    if (!prod)
        return -1;
    int ff = re_item_file_frame(&it, it.in + frame - it.start);
    mlt_properties_set_double(MLT_PRODUCER_PROPERTIES(prod), "shotcut:reeledit_time", ff / re_project_fps(p));
    mlt_properties_set(MLT_PRODUCER_PROPERTIES(prod), "shotcut:caption", "Freeze frame");
    return put(p, prod, frame, length);
}

int re_project_add_fill(re_project *p, int frame, int length, unsigned rgb)
{
    char res[16];
    if (!p->tractor && !p->profile->is_explicit) /* (an empty project: a colour clip says nothing of its size) */
        return -1;
    snprintf(res, sizeof(res), "#ff%06x", rgb & 0xFFFFFF);
    mlt_producer prod = mlt_factory_producer(p->profile, "color", res);
    if (prod)
        mlt_properties_set(MLT_PRODUCER_PROPERTIES(prod), "shotcut:caption", "Colour");
    return put(p, prod, frame, length);
}

int re_project_set_fill(re_project *p, int item, unsigned rgb)
{
    unsigned now;
    mlt_producer cut = rp_is_clip(p, item) ? mlt_playlist_get_clip(p->v1, item) : NULL;
    if (!rp_cut_fill(cut, &now) || now == (rgb & 0xFFFFFF))
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    char res[16];
    snprintf(res, sizeof(res), "#ff%06x", rgb & 0xFFFFFF);
    mlt_properties_set(MLT_PRODUCER_PROPERTIES(parent_of(cut)), "resource", res);
    rp_changed(p);
    return 1;
}
