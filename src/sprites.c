/* sprites.c - RISC OS sprite files read (sprites.h): a sprite as RGBA,
 * to be written as a PNG for MLT. Plain C, no RISC OS calls (the file is
 * read as bytes), so it's tested on Linux.
 *
 * A sprite file is a sprite area without its first word: the number of
 * sprites, the offset of the first and of the free space (each 4 more
 * than in the file), then the sprites, each:
 *   +0 the offset of the next, +4 its name (12 bytes), +16 its width in
 *   words - 1, +20 its height - 1, +24 the first and +28 the last bit used
 *   of each row, +32 the offset of the picture and +36 of the mask (the
 *   picture's: none), +40 its mode, +44 its palette (pairs of words) if
 *   the picture is further on.
 * Rows are whole words, the leftmost pixel in a word's lowest bits.
 *
 * Read:
 *   - old sprites (a mode number: the mode's colours and pixel shape; a
 *     mask of the picture's depth, a pixel all ones where it's solid)
 *   - new ones (a mode word: type, dpi; a mask of 1 bit a pixel), and
 *     RISC OS 5's (type in bits 20-26: its 4:4:4:4 and 5:6:5, RGB order
 *     and alpha flags), and Select's alpha masks (8 bits a pixel: bit 31)
 *   - 1, 2, 4 and 8 bits a pixel, with their palette or the Wimp's (the
 *     standard 256 for 8), 16 (1:5:5:5, 5:6:5, 4:4:4:4), 24 and 32
 * Pixels that aren't square (90 x 45 dpi) are made square: rows (or
 * columns) repeated.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "sprites.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SIZE (64 << 20)
#define MAX_PIXELS (16 << 20)

/* The whole file. NULL, or its bytes (free them) and their number. */
static uint8_t *slurp(const char *file, long *size)
{
    FILE *f = fopen(file, "rb");
    if (!f)
        return NULL;
    uint8_t *d = NULL;
    long n = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
    if (n >= 12 && n < MAX_SIZE && !fseek(f, 0, SEEK_SET) && (d = malloc(n))) {
        if (fread(d, 1, n, f) != (size_t) n) {
            free(d);
            d = NULL;
        } else {
            *size = n;
        }
    }
    fclose(f);
    return d;
}

static uint32_t word(const uint8_t *d, long at)
{
    return d[at] | d[at + 1] << 8 | d[at + 2] << 16 | (uint32_t) d[at + 3] << 24;
}

/* The sprites' offsets in the file, in order; their number, or -1 if it
   isn't a sprite file. */
static int find(const uint8_t *d, long size, long *at, int max)
{
    long n = word(d, 0), first = (long) word(d, 4) - 4, free_at = (long) word(d, 8) - 4;
    if (n < 1 || n > 10000 || first < 12 || first >= size || free_at > size || free_at < first)
        return -1;
    long o = first;
    int k = 0;
    for (long i = 0; i < n; i++) {
        if (o < 0 || o + 44 > size)
            return -1;
        long next = word(d, o);
        if (next < 44 || o + next > size)
            return -1;
        if (k < max)
            at[k++] = o;
        o += next;
    }
    return k;
}

int re_sprites_names(const char *file, char names[][13], int max)
{
    long size = 0, at[RE_SPRITES_MAX];
    uint8_t *d = slurp(file, &size);
    int n = d ? find(d, size, at, max < RE_SPRITES_MAX ? max : RE_SPRITES_MAX) : -1;
    for (int i = 0; i < n; i++) {
        int j = 0;
        for (; j < 12 && d[at[i] + 4 + j] > ' '; j++)
            names[i][j] = (char) d[at[i] + 4 + j];
        names[i][j] = 0;
    }
    free(d);
    return n;
}

/* ---- a sprite's pixels ----------------------------------------------------------------------- */

/* Old modes 0-49: log2 bits a pixel, and the pixels' shape (x and y eig) */
static const uint8_t OLD_MODES[50][3] = {
    {0, 1, 2}, {1, 2, 2}, {2, 3, 2}, {0, 1, 2}, {0, 2, 2}, {1, 3, 2}, {0, 2, 2}, {2, 2, 2}, {1, 1, 2}, {2, 2, 2},
    {3, 3, 2}, {1, 1, 2}, {2, 1, 2}, {3, 2, 2}, {2, 1, 2}, {3, 1, 2}, {2, 1, 2}, {2, 1, 2}, {0, 1, 1}, {1, 1, 1},
    {2, 1, 1}, {3, 1, 1}, {2, 0, 1}, {0, 1, 1}, {3, 1, 2}, {0, 1, 1}, {1, 1, 1}, {2, 1, 1}, {3, 1, 1}, {0, 1, 1},
    {1, 1, 1}, {2, 1, 1}, {3, 1, 1}, {0, 1, 2}, {1, 1, 2}, {2, 1, 2}, {3, 1, 2}, {0, 1, 1}, {1, 1, 1}, {2, 1, 1},
    {3, 1, 1}, {0, 1, 2}, {1, 1, 2}, {2, 1, 2}, {0, 1, 2}, {1, 1, 2}, {2, 1, 2}, {3, 2, 2}, {2, 2, 1}, {3, 2, 1}};

/* The Wimp's 16 colours (&RRGGBB) */
static const uint32_t WIMP16[16] = {0xFFFFFF, 0xDDDDDD, 0xBBBBBB, 0x999999, 0x777777, 0x555555, 0x333333, 0x000000,
                                    0x004499, 0xEEEE00, 0x00CC00, 0xDD0000, 0xEEEEBB, 0x558800, 0xFFBB00, 0x00BBFF};

enum { K_PALETTE, K_1555, K_565, K_4444, K_888, K_8888 };

typedef struct
{
    int bpp;           /* bits a pixel */
    int kind;          /* K_ */
    int rgb, alpha;    /* RISC OS 5's flags: red in the high bits; the top bits alpha */
    int xmul, ymul;    /* each pixel repeated, across and down (to square them) */
    int new_mask;      /* the mask: 1 bit a pixel (new sprites), or the picture's depth */
    int alpha_mask;    /* the mask: 8 bits a pixel, alpha (Select's) */
} form;

/* The sprite's form from its mode. 0, or -1 (one not known). */
static int form_of(uint32_t mode, form *f)
{
    static const int TYPE_BPP[17] = {0, 1, 2, 4, 8, 16, 32, 0, 24, 0, 16, 0, 0, 0, 0, 0, 16};
    memset(f, 0, sizeof(*f));
    f->xmul = f->ymul = 1;
    int xeig = 1, yeig = 1, type = 0;
    if (mode < 256) {
        if (mode >= 50)
            return -1;
        f->bpp = 1 << OLD_MODES[mode][0];
        xeig = OLD_MODES[mode][1];
        yeig = OLD_MODES[mode][2];
        f->kind = K_PALETTE;
    } else if ((mode >> 27) == 15) { /* RISC OS 5: type in bits 20-26, flags in 8-15, eigs in 4-7 */
        type = (mode >> 20) & 127;
        if (type < 1 || type > 16 || !TYPE_BPP[type])
            return -1;
        f->bpp = TYPE_BPP[type];
        f->rgb = (mode >> 14) & 1;
        f->alpha = (mode >> 15) & 1;
        xeig = (mode >> 4) & 3;
        yeig = (mode >> 6) & 3;
        f->new_mask = 1;
    } else {
        if (!(mode & 1))
            return -1;
        type = (mode >> 27) & 15;
        if (type < 1 || type > 16 || !TYPE_BPP[type])
            return -1;
        f->bpp = TYPE_BPP[type];
        int xdpi = (mode >> 1) & 0x1FFF, ydpi = (mode >> 14) & 0x1FFF;
        if (xdpi > 0 && ydpi > 0) { /* (as eigs: 180 dpi is 0, 90 is 1, 45 is 2) */
            xeig = xdpi >= 180 ? 0 : xdpi >= 90 ? 1 : 2;
            yeig = ydpi >= 180 ? 0 : ydpi >= 90 ? 1 : 2;
        }
        f->new_mask = 1;
        f->alpha_mask = (mode >> 31) & 1;
    }
    switch (f->bpp) {
    case 16: f->kind = type == 10 ? K_565 : type == 16 ? K_4444 : K_1555; break;
    case 24: f->kind = K_888; break;
    case 32: f->kind = K_8888; break;
    default: f->kind = K_PALETTE;
    }
    if (yeig > xeig)
        f->ymul = 1 << (yeig - xeig);
    else if (xeig > yeig)
        f->xmul = 1 << (xeig - yeig);
    return 0;
}

/* A palette of 2^bpp colours (&RRGGBB): the sprite's, or the Wimp's */
static void palette_of(const uint8_t *d, long s, long image, int bpp, uint32_t *pal)
{
    int n = 1 << bpp, have = (int) ((image - 44) / 8);
    if (have >= n) {
        for (int i = 0; i < n; i++) {
            uint32_t w = word(d, s + 44 + i * 8); /* &BBGGRR00 */
            pal[i] = ((w >> 8) & 255) << 16 | ((w >> 16) & 255) << 8 | (w >> 24);
        }
        return;
    }
    if (bpp == 8) { /* the standard 256: B3 G3 G2 R3 B2 R2 T1 T0 */
        for (int i = 0; i < 256; i++) {
            int t = i & 3, r = (i >> 4 & 1) << 3 | (i >> 2 & 1) << 2 | t, g = (i >> 6 & 1) << 3 | (i >> 5 & 1) << 2 | t,
                b = (i >> 7 & 1) << 3 | (i >> 3 & 1) << 2 | t;
            pal[i] = (uint32_t) (r * 17) << 16 | (g * 17) << 8 | b * 17;
        }
        return;
    }
    static const int WIMP2[4] = {0, 2, 4, 7}, WIMP1[2] = {0, 7};
    for (int i = 0; i < n; i++)
        pal[i] = WIMP16[bpp == 1 ? WIMP1[i] : bpp == 2 ? WIMP2[i] : i];
}

/* Bits x .. x + bits - 1 of a row */
static uint32_t bits_at(const uint8_t *row, long x, int bits)
{
    if (bits >= 8) {
        const uint8_t *p = row + x / 8;
        switch (bits) {
        case 8: return p[0];
        case 16: return p[0] | p[1] << 8;
        case 24: return p[0] | p[1] << 8 | p[2] << 16;
        default: return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24;
        }
    }
    return (row[x / 8] >> (x % 8)) & ((1 << bits) - 1);
}

static int scale(uint32_t v, int bits)
{
    return (int) (v * 255 / ((1u << bits) - 1));
}

int re_sprite_rgba(const char *file, int index, uint8_t **rgba, int *width, int *height)
{
    long size = 0, at[RE_SPRITES_MAX];
    uint8_t *d = slurp(file, &size);
    int n = d ? find(d, size, at, RE_SPRITES_MAX) : -1, ok = -1;
    *rgba = NULL;
    if (index < 0 || index >= n) {
        free(d);
        return -1;
    }
    long s = at[index];
    long wwords = (long) word(d, s + 16) + 1, h = (long) word(d, s + 20) + 1;
    long lbit = word(d, s + 24), rbit = word(d, s + 28), image = word(d, s + 32), mask = word(d, s + 36);
    uint32_t mode = word(d, s + 40);
    form f;
    long stride = wwords * 4;
    if (form_of(mode, &f) || wwords < 1 || wwords > 65536 || h < 1 || h > 65536 || lbit > 31 || rbit > 31
        || image < 44 || s + image + stride * h > size) {
        free(d);
        return -1;
    }
    long w = (wwords * 32 - lbit - (31 - rbit)) / f.bpp;
    if (f.new_mask)
        lbit = 0; /* (new sprites have none) */
    int has_mask = mask != image && mask >= 44;
    long mstride = !has_mask ? 0 : f.alpha_mask ? (w + 3) / 4 * 4 : f.new_mask ? (w + 31) / 32 * 4 : stride;
    if (has_mask && s + mask + mstride * h > size)
        has_mask = 0;
    long ow = w * f.xmul, oh = h * f.ymul;
    if (w < 1 || ow * oh > MAX_PIXELS || !(*rgba = malloc(ow * oh * 4))) {
        free(d);
        return -1;
    }
    uint32_t pal[256];
    if (f.kind == K_PALETTE)
        palette_of(d, s, image, f.bpp, pal);
    for (long y = 0; y < h; y++) {
        const uint8_t *row = d + s + image + y * stride, *mrow = has_mask ? d + s + mask + y * mstride : NULL;
        uint8_t *out = *rgba + (y * f.ymul) * ow * 4;
        for (long x = 0; x < w; x++) {
            uint32_t v = bits_at(row, lbit + x * f.bpp, f.bpp);
            int r, g, b, a = 255;
            switch (f.kind) {
            case K_PALETTE:
                r = pal[v] >> 16 & 255, g = pal[v] >> 8 & 255, b = pal[v] & 255;
                break;
            case K_1555:
                r = scale(v & 31, 5), g = scale(v >> 5 & 31, 5), b = scale(v >> 10 & 31, 5);
                if (f.alpha)
                    a = v >> 15 ? 255 : 0;
                break;
            case K_565:
                r = scale(v & 31, 5), g = scale(v >> 5 & 63, 6), b = scale(v >> 11 & 31, 5);
                break;
            case K_4444:
                r = scale(v & 15, 4), g = scale(v >> 4 & 15, 4), b = scale(v >> 8 & 15, 4);
                if (f.alpha)
                    a = scale(v >> 12 & 15, 4);
                break;
            case K_888:
                r = v & 255, g = v >> 8 & 255, b = v >> 16 & 255;
                break;
            default:
                r = v & 255, g = v >> 8 & 255, b = v >> 16 & 255;
                if (f.alpha)
                    a = v >> 24;
            }
            if (f.rgb) { /* (red in the high bits) */
                int t = r;
                r = b;
                b = t;
            }
            if (mrow) {
                if (f.alpha_mask)
                    a = mrow[x];
                else if (f.new_mask)
                    a = mrow[x / 8] >> (x % 8) & 1 ? 255 : 0;
                else
                    a = bits_at(mrow, lbit + x * f.bpp, f.bpp) ? 255 : 0;
            }
            for (int k = 0; k < f.xmul; k++) {
                uint8_t *p = out + (x * f.xmul + k) * 4;
                p[0] = (uint8_t) r, p[1] = (uint8_t) g, p[2] = (uint8_t) b, p[3] = (uint8_t) a;
            }
        }
        for (int k = 1; k < f.ymul; k++)
            memcpy(out + k * ow * 4, out, ow * 4);
    }
    *width = (int) ow;
    *height = (int) oh;
    ok = 0;
    free(d);
    return ok;
}
