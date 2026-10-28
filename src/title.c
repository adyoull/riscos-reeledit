/* title.c - titles: text over the video (title.h).
 *
 * A title is a picture made from its text: each line drawn by the font
 * renderer (the Font Manager on RISC OS) as how much each pixel is
 * covered, the lines stacked and aligned, then coloured, over its shadow,
 * its outline or its box. The picture is just big enough for the text
 * (and its shadow, outline or box), so it's put over the video where its
 * place says, as it is: the overlay's composite is given a box its size
 * (overlay.c).
 *
 * Its producer is MLT's "qtext" (Shotcut's text producer: not in this
 * build, no Qt), with Shotcut's properties, so a project's titles open in
 * Shotcut, and Shotcut's here:
 *   text       the lines ('\n' between)
 *   family     the font (here a RISC OS font's name)
 *   size       pixels to the em, at the frame's size
 *   fgcolour   #AARRGGBB
 *   olcolour, outline   the outline's colour and width (pixels; 0: none)
 *   bgcolour, pad       the box's colour (#00000000: none) and its margin
 *   halign     left, centre, right
 *   shotcut:reeledit_shadow   1: a shadow (not Shotcut's)
 * It's a still picture (still.c), made again when any of these change.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "title.h"
#include "still.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static re_title_font_fn font_fn;

void re_title_set_font(re_title_font_fn fn)
{
    font_fn = fn;
}

/* Without a font renderer: each letter a block, a space a gap */
static int blocks(const char *font, int px, const char *line, uint8_t **mask, int *width, int *height)
{
    (void) font;
    int n = (int) strlen(line), adv = px * 6 / 10 > 1 ? px * 6 / 10 : 2, h = px * 5 / 4 > 2 ? px * 5 / 4 : 3;
    int w = n * adv + 2;
    *mask = calloc((size_t) w * h, 1);
    if (!*mask)
        return -1;
    int top = h - px * 3 / 10 - px * 7 / 10, bottom = h - px * 3 / 10; /* (cap height 0.7 em, descent 0.3) */
    for (int i = 0; i < n; i++) {
        if (line[i] == ' ')
            continue;
        int x0 = 1 + i * adv, x1 = x0 + adv * 3 / 4;
        for (int y = top < 0 ? 0 : top; y < bottom && y < h; y++)
            memset(*mask + (size_t) y * w + x0, 255, x1 - x0 > 0 ? x1 - x0 : 1);
    }
    *width = w;
    *height = h;
    return 0;
}

/* d = s over d (RGBA, not premultiplied), s's alpha times a/255 */
static void over(uint8_t *d, unsigned r, unsigned g, unsigned b, unsigned sa, unsigned a)
{
    unsigned aa = sa * a / 255;
    if (!aa)
        return;
    unsigned da = d[3], oa = aa + da * (255 - aa) / 255;
    if (!oa)
        return;
    d[0] = (uint8_t) ((r * aa + d[0] * da * (255 - aa) / 255) / oa);
    d[1] = (uint8_t) ((g * aa + d[1] * da * (255 - aa) / 255) / oa);
    d[2] = (uint8_t) ((b * aa + d[2] * da * (255 - aa) / 255) / oa);
    d[3] = (uint8_t) oa;
}

int re_title_render(const re_title *t, int frame_height, uint8_t **rgba, int *width, int *height)
{
    *rgba = NULL;
    int px = t->size * frame_height / 100;
    if (px < 4)
        px = 4;
    if (px > 400)
        px = 400;
    re_title_font_fn fn = font_fn ? font_fn : blocks;
    /* the lines' coverage */
    enum { MAX_LINES = 16 };
    uint8_t *m[MAX_LINES] = {0};
    int mw[MAX_LINES] = {0}, mh[MAX_LINES] = {0}, n = 0, w = 0, h = 0;
    const char *s = t->text;
    while (n < MAX_LINES) {
        const char *e = strchr(s, '\n');
        int len = e ? (int) (e - s) : (int) strlen(s);
        char line[512];
        snprintf(line, sizeof(line), "%.*s", len, s);
        if (fn(t->font, px, line, &m[n], &mw[n], &mh[n]) && blocks(t->font, px, line, &m[n], &mw[n], &mh[n]))
            break;
        if (mw[n] > w)
            w = mw[n];
        h += mh[n];
        n++;
        if (!e)
            break;
        s = e + 1;
    }
    /* trailing empty lines left out */
    int pad = t->style == RE_TITLE_BOX ? px / 3 : t->style != RE_TITLE_PLAIN ? (px / 8 > 2 ? px / 8 : 2) : 1;
    int W = w + 2 * pad, H = h + 2 * pad, bad = n == 0 || w < 1;
    uint8_t *cover = bad ? NULL : calloc((size_t) W * H, 1);
    uint8_t *out = cover ? calloc((size_t) W * H, 4) : NULL;
    if (out) {
        int y = pad;
        for (int i = 0; i < n; i++) {
            int x = pad + (t->align == RE_TITLE_LEFT ? 0 : t->align == RE_TITLE_RIGHT ? w - mw[i] : (w - mw[i]) / 2);
            for (int r = 0; r < mh[i]; r++)
                memcpy(cover + (size_t) (y + r) * W + x, m[i] + (size_t) r * mw[i], mw[i]);
            y += mh[i];
        }
        unsigned cr = (t->colour >> 16) & 255, cg = (t->colour >> 8) & 255, cb = t->colour & 255;
        if (t->style == RE_TITLE_BOX)
            for (int i = 0; i < W * H; i++)
                out[i * 4 + 3] = 160; /* (black, see-through a little) */
        if (t->style == RE_TITLE_SHADOW) {
            int d = px / 16 > 1 ? px / 16 : 1;
            for (int yy = d; yy < H; yy++)
                for (int xx = d; xx < W; xx++)
                    over(out + ((size_t) yy * W + xx) * 4, 0, 0, 0, cover[(size_t) (yy - d) * W + xx - d], 190);
        }
        if (t->style == RE_TITLE_OUTLINE) {
            int r = px / 16 > 1 ? px / 16 : 1;
            uint8_t *row = malloc((size_t) W * H);
            if (row) { /* (the coverage spread r each way: across, then down) */
                for (int yy = 0; yy < H; yy++)
                    for (int xx = 0; xx < W; xx++) {
                        int best = 0;
                        for (int k = xx - r; k <= xx + r; k++)
                            if (k >= 0 && k < W && cover[(size_t) yy * W + k] > best)
                                best = cover[(size_t) yy * W + k];
                        row[(size_t) yy * W + xx] = (uint8_t) best;
                    }
                for (int yy = 0; yy < H; yy++)
                    for (int xx = 0; xx < W; xx++) {
                        int best = 0;
                        for (int k = yy - r; k <= yy + r; k++)
                            if (k >= 0 && k < H && row[(size_t) k * W + xx] > best)
                                best = row[(size_t) k * W + xx];
                        over(out + ((size_t) yy * W + xx) * 4, 0, 0, 0, (unsigned) best, 255);
                    }
                free(row);
            }
        }
        for (int i = 0; i < W * H; i++)
            over(out + (size_t) i * 4, cr, cg, cb, cover[i], 255);
    }
    for (int i = 0; i < n; i++)
        free(m[i]);
    free(cover);
    if (!out)
        return -1;
    *rgba = out;
    *width = W;
    *height = H;
    return 0;
}

/* ---- the producer --------------------------------------------------------------------------- */

static const char *const ALIGNS[3] = {"left", "centre", "right"};

static unsigned colour_of(mlt_properties p, const char *name, unsigned dflt, int *alpha)
{
    if (!mlt_properties_get(p, name)) {
        if (alpha)
            *alpha = 255;
        return dflt;
    }
    mlt_color c = mlt_properties_get_color(p, name);
    if (alpha)
        *alpha = c.a;
    return ((unsigned) c.r << 16) | ((unsigned) c.g << 8) | c.b;
}

/* The settings from the properties, the size in pixels (px) */
static void from(mlt_properties p, re_title *t, int *px)
{
    re_title_default(t);
    snprintf(t->text, sizeof(t->text), "%s", mlt_properties_get(p, "text") ? mlt_properties_get(p, "text") : "");
    if (mlt_properties_get(p, "family"))
        snprintf(t->font, sizeof(t->font), "%s", mlt_properties_get(p, "family"));
    *px = mlt_properties_get(p, "size") ? mlt_properties_get_int(p, "size") : 48;
    t->colour = colour_of(p, "fgcolour", 0xFFFFFF, NULL);
    int box_alpha = 0;
    colour_of(p, "bgcolour", 0, &box_alpha);
    t->style = box_alpha > 0                                   ? RE_TITLE_BOX
               : mlt_properties_get_int(p, "outline") > 0       ? RE_TITLE_OUTLINE
               : mlt_properties_get_int(p, "shotcut:reeledit_shadow") ? RE_TITLE_SHADOW
                                                                : RE_TITLE_PLAIN;
    const char *a = mlt_properties_get(p, "halign");
    t->align = a && !strcmp(a, "left") ? RE_TITLE_LEFT : a && !strcmp(a, "right") ? RE_TITLE_RIGHT : RE_TITLE_CENTRE;
}

static void key(mlt_properties p, char *k, int size)
{
    re_title t;
    int px;
    from(p, &t, &px);
    snprintf(k, size, "%d|%s|%06x|%d|%d|%s", px, t.font, t.colour, t.style, t.align, t.text);
}

static int make(mlt_properties p, uint8_t **rgba, int *w, int *h)
{
    re_title t;
    int px;
    from(p, &t, &px);
    t.size = 100; /* (px is the size: rendered as % of a frame px high) */
    return re_title_render(&t, px, rgba, w, h);
}

static const re_still_maker MAKER = {key, make};

static mlt_producer title_init(mlt_profile profile, mlt_service_type type, const char *id, char *arg)
{
    (void) type;
    (void) id;
    mlt_producer p = re_still_made(profile, "qtext", arg, &MAKER);
    if (p && arg && *arg && !mlt_properties_get(MLT_PRODUCER_PROPERTIES(p), "text"))
        mlt_properties_set(MLT_PRODUCER_PROPERTIES(p), "text", arg);
    return p;
}

void re_title_register(void)
{
    mlt_repository r = mlt_factory_repository();
    if (r)
        mlt_repository_register(r, mlt_service_producer_type, "qtext", (mlt_register_callback) title_init);
    if (re_still_kinds < 4)
        re_still_kind[re_still_kinds++] = "qtext";
}

void re_title_default(re_title *t)
{
    memset(t, 0, sizeof(*t));
    snprintf(t->font, sizeof(t->font), "Homerton.Bold");
    t->size = 8;
    t->colour = 0xFFFFFF;
    t->style = RE_TITLE_SHADOW;
    t->align = RE_TITLE_CENTRE;
}

static mlt_producer parent_of(mlt_producer producer)
{
    mlt_producer parent = producer ? mlt_producer_cut_parent(producer) : NULL;
    return parent ? parent : producer;
}

int re_title_is(mlt_producer producer)
{
    mlt_producer p = parent_of(producer);
    const char *s = p ? mlt_properties_get(MLT_PRODUCER_PROPERTIES(p), "mlt_service") : NULL;
    return s && !strcmp(s, "qtext");
}

int re_title_get(mlt_producer producer, re_title *t)
{
    if (!re_title_is(producer))
        return -1;
    mlt_producer p = parent_of(producer);
    int px;
    from(MLT_PRODUCER_PROPERTIES(p), t, &px);
    mlt_profile profile = mlt_service_profile(MLT_PRODUCER_SERVICE(p));
    int fh = profile && profile->height > 0 ? profile->height : 576;
    t->size = (px * 100 + fh / 2) / fh;
    if (t->size < 1)
        t->size = 1;
    return 0;
}

int re_title_put(mlt_producer producer, const re_title *t)
{
    if (!re_title_is(producer))
        return -1;
    mlt_producer p = parent_of(producer);
    mlt_properties pp = MLT_PRODUCER_PROPERTIES(p);
    mlt_profile profile = mlt_service_profile(MLT_PRODUCER_SERVICE(p));
    int fh = profile && profile->height > 0 ? profile->height : 576;
    char c[16];
    mlt_properties_set(pp, "text", t->text);
    mlt_properties_set(pp, "family", t->font);
    mlt_properties_set_int(pp, "size", t->size * fh / 100 > 4 ? t->size * fh / 100 : 4);
    snprintf(c, sizeof(c), "#ff%06x", t->colour & 0xFFFFFF);
    mlt_properties_set(pp, "fgcolour", c);
    mlt_properties_set(pp, "olcolour", "#ff000000");
    mlt_properties_set_int(pp, "outline", t->style == RE_TITLE_OUTLINE ? (t->size * fh / 1600 > 1 ? t->size * fh / 1600 : 1) : 0);
    mlt_properties_set(pp, "bgcolour", t->style == RE_TITLE_BOX ? "#a0000000" : "#00000000");
    mlt_properties_set_int(pp, "pad", t->style == RE_TITLE_BOX ? t->size * fh / 300 : 0);
    mlt_properties_set_int(pp, "shotcut:reeledit_shadow", t->style == RE_TITLE_SHADOW);
    mlt_properties_set(pp, "halign", ALIGNS[t->align >= 0 && t->align < 3 ? t->align : 1]);
    mlt_properties_set(pp, "valign", "top");
    return 0;
}

mlt_producer re_title_producer(mlt_profile profile, const re_title *t)
{
    /* (by the loader, as qimage pictures are: its filters too, the
       converters among them) */
    mlt_producer p = mlt_factory_producer(profile, NULL, "qtext:");
    if (!p)
        return NULL;
    if (!re_title_is(p)) {
        mlt_producer_close(p);
        return NULL;
    }
    re_title_put(p, t);
    mlt_properties pp = MLT_PRODUCER_PROPERTIES(p);
    mlt_properties_set_position(pp, "length", 15000);
    mlt_properties_set_position(pp, "out", 14999);
    return p;
}
