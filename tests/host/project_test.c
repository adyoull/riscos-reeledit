/* project_test - src/project.c, the editing model, and the engine playing
 * it as it's edited.
 *
 *   project_test a.mp4 b.mp4 numbered.mp4 out.mlt
 * a.mp4 red, b.mp4 blue, numbered.mp4 frame n a grey of luma 16 + 2n; each
 * 320x180, 25 fps, 75 frames. out.mlt: written and read back.
 *
 * An empty project takes the first clip's profile; clips go at the end or
 * before/after the item at a frame; split at a frame (not at an item's
 * ends); removing ripples (or leaves a gap, which removing closes; a gap
 * at the end goes); saved as MLT XML and opened again the same; and the
 * engine, playing the project, shows each edit after re_engine_refresh.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "engine.h"
#include "project.h"
#include "measure.h"
#include "sprites.h"
#include "still.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...)                                                                                              \
    do {                                                                                                           \
        if (!(c)) {                                                                                                \
            fails++;                                                                                               \
            printf("FAIL %d: ", __LINE__);                                                                         \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
        }                                                                                                          \
    } while (0)

static int shown_y = -1, shown_pos = -1, shown_w, shown_h, shown_cs;
static double shown_dar;
static uint8_t shown_luma[320 * 180]; /* (the picture's luma, when it's 320x180) */
static uint8_t shown_cb[160 * 90], shown_cr[160 * 90]; /* (and its chroma) */
static void show(void *ctx, const re_picture *p)
{
    (void) ctx;
    if (p->width == 320 && p->height == 180)
        for (int y = 0; y < 180; y++) {
            memcpy(shown_luma + y * 320, p->planes[0] + y * p->strides[0], 320);
            if (y < 90) {
                memcpy(shown_cb + y * 160, p->planes[1] + y * p->strides[1], 160);
                memcpy(shown_cr + y * 160, p->planes[2] + y * p->strides[2], 160);
            }
        }
    shown_w = p->width;
    shown_h = p->height;
    shown_cs = p->colorspace;
    shown_dar = p->dar;
    shown_y = p->planes[0][(p->height / 2) * p->strides[0] + p->width / 2];
    shown_pos = p->position;
}
static unsigned clk(void *ctx)
{
    (void) ctx;
    return 0;
}

/* items as "c75 g10 m5 c40": clips (c), gaps (g) and dissolves (m) with their lengths */
static const char *layout(re_project *p)
{
    static char s[256];
    s[0] = 0;
    for (int i = 0; i < re_project_items(p); i++) {
        re_item it;
        re_project_item(p, i, &it);
        snprintf(s + strlen(s), sizeof(s) - strlen(s), "%s%c%d", i ? " " : "", it.gap ? 'g' : it.mix ? 'm' : 'c',
                 it.length);
    }
    return s;
}

static const char *name_of(re_project *p, int i)
{
    static re_item it;
    re_project_item(p, i, &it);
    return it.name;
}

static int luma_at(re_engine *e, int frame)
{
    re_engine_seek(e, frame);
    re_engine_poll(e);
    return shown_pos == frame ? shown_y : -1;
}

/* ---- dissolves: made from the clips' spare frames or by overlapping them;
   lengthened, shortened, removed; kept through undo, saving, opening; the
   engine plays them (the picture between the two clips'), also between
   clips of different sizes */
static void dissolves(const char *a, const char *b, const char *four3, const char *out)
{
    re_engine_io io = {show, NULL, clk, NULL, 48000, 2, NULL};
    re_project *p = re_project_new();
    re_project_add(p, a, -1);
    re_project_add(p, b, -1);
    re_engine *e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    int red = luma_at(e, 10), blue = luma_at(e, 100);

    /* no frames beyond the cut: overlapped, the project 25 shorter */
    CHECK(re_project_dissolve(p, 0, 25, 0) == 1 && !strcmp(layout(p), "c50 m25 c50") && re_project_length(p) == 125,
          "dissolves: overlapped: %s, %d", layout(p), re_project_length(p));
    re_item it;
    re_project_item(p, 1, &it);
    CHECK(it.mix && it.in == 0 && it.out == 24 && !strcmp(it.name, "Dissolve"), "dissolves: the item: %d %d-%d %s",
          it.mix, it.in, it.out, it.name);
    re_engine_refresh(e);
    int mid = luma_at(e, 62), first = luma_at(e, 50), last = luma_at(e, 74);
    CHECK(abs(first - red) <= 6 && abs(last - blue) <= 6 && mid > blue + 10 && mid < red - 10,
          "dissolves: played: luma %d, %d, %d (red %d, blue %d)", first, mid, last, red, blue);
    CHECK(re_project_dissolve_max(p, 1) == 25 + 49, "dissolves: the longest %d, not 74", re_project_dissolve_max(p, 1));
    CHECK(re_project_dissolve_length(p, 1, 35) == 1 && !strcmp(layout(p), "c40 m35 c40"), "dissolves: longer: %s",
          layout(p));
    CHECK(re_project_dissolve_length(p, 1, 500) == 1 && !strcmp(layout(p), "c1 m74 c1"), "dissolves: kept to the clips: %s",
          layout(p));
    CHECK(re_project_dissolve_length(p, 1, 10) == 1 && !strcmp(layout(p), "c65 m10 c65"), "dissolves: shorter: %s",
          layout(p));
    re_engine_refresh(e);
    mid = luma_at(e, 70);
    CHECK(mid > blue + 10 && mid < red - 10, "dissolves: played after resizing: luma %d", mid);

    /* what's refused: splitting it, trimming or moving into it */
    CHECK(re_project_split(p, 70) == 0, "dissolves: split inside one");
    CHECK(re_project_trim(p, 30, 1) == 0 && re_project_trim(p, 100, 0) == 0, "dissolves: Q or W into one: %s", layout(p));
    CHECK(re_project_move(p, 0, 3) == -1 && re_project_move(p, 1, 0) == -1, "dissolves: moved");
    CHECK(re_project_set_in_out(p, 0, 0, 70) == 0 && !strcmp(layout(p), "c65 m10 c65"),
          "dissolves: an end in one trimmed: %s", layout(p));

    /* undo and redo: XML again, the links made again (resizing works) */
    CHECK(re_project_undo(p) && !strcmp(layout(p), "c1 m74 c1"), "dissolves: undo: %s", layout(p));
    CHECK(re_project_redo(p) && !strcmp(layout(p), "c65 m10 c65"), "dissolves: redo: %s", layout(p));
    CHECK(re_project_dissolve_length(p, 1, 20) == 1 && !strcmp(layout(p), "c55 m20 c55"),
          "dissolves: resized after redo: %s", layout(p));

    /* saved and opened again: still a dissolve, that resizes, and is removed */
    CHECK(re_project_save(p, out) == 0, "dissolves: not saved");
    re_engine_close(e);
    re_project_close(p);
    p = re_project_open(out, 1);
    CHECK(p && !strcmp(layout(p), "c55 m20 c55"), "dissolves: opened: %s", p ? layout(p) : "-");
    if (!p)
        return;
    CHECK(re_project_dissolve_length(p, 1, 30) == 1 && !strcmp(layout(p), "c45 m30 c45"),
          "dissolves: opened, resized: %s", layout(p));
    e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    mid = luma_at(e, 60);
    CHECK(mid > blue + 10 && mid < red - 10, "dissolves: opened, played: luma %d", mid);
    CHECK(re_project_remove(p, 1, 1) == 1 && !strcmp(layout(p), "c75 c75"), "dissolves: removed: %s", layout(p));

    /* from spare frames: the length kept (the first clip's after its out; then the second's before its in) */
    CHECK(re_project_set_in_out(p, 0, 0, 49) == 1 && !strcmp(layout(p), "c50 c75"), "dissolves: a trimmed: %s",
          layout(p));
    CHECK(re_project_dissolve(p, 0, 10, 0) == 1 && !strcmp(layout(p), "c50 m10 c65"),
          "dissolves: a's spare frames: %s", layout(p));
    re_engine_refresh(e);
    int at_cut = luma_at(e, 50);
    CHECK(abs(at_cut - red) <= 6, "dissolves: a's spare frames: the dissolve starts red (%d)", at_cut);
    CHECK(re_project_remove(p, 1, 1) == 1 && !strcmp(layout(p), "c50 c75"), "dissolves: removed again: %s", layout(p));
    CHECK(re_project_set_in_out(p, 0, 0, 74) == 1 && re_project_set_in_out(p, 1, 20, 74) == 1
              && !strcmp(layout(p), "c75 c55"),
          "dissolves: b trimmed: %s", layout(p));
    CHECK(re_project_dissolve(p, 0, 10, 0) == 1 && !strcmp(layout(p), "c65 m10 c55"),
          "dissolves: b's spare frames: %s", layout(p));
    /* a clip removed takes its dissolve: the other clip as it was */
    CHECK(re_project_remove(p, 0, 1) == 1 && !strcmp(layout(p), "c55"), "dissolves: the clip before removed: %s",
          layout(p));
    re_project_item(p, 0, &it);
    CHECK(it.in == 20, "dissolves: the clip left starts at %d, not 20", it.in);
    re_engine_close(e);
    re_project_close(p);

    /* clips of different sizes: played at the project's size */
    p = re_project_new();
    re_project_add(p, a, -1);
    re_project_add(p, four3, -1);
    CHECK(re_project_dissolve(p, 0, 20, 1) == 1, "dissolves: different sizes: %s", layout(p));
    e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    mid = luma_at(e, 65);
    CHECK(mid > 0 && shown_w == 320 && shown_h == 180, "dissolves: different sizes: %dx%d, luma %d", shown_w, shown_h,
          mid);
    mid = luma_at(e, 90);
    CHECK(shown_w == 480 && shown_h == 360, "dissolves: after it, the 4:3 clip as it is: %dx%d", shown_w, shown_h);
    re_engine_close(e);
    re_project_close(p);
}

/* The loudest sample of the project's sound at a frame (MLT's, as the engine takes it) */
static int peak_at(re_project *p, int frame)
{
    mlt_producer prod = re_project_producer(p);
    mlt_frame f = NULL;
    mlt_producer_seek(prod, frame);
    if (mlt_service_get_frame(MLT_PRODUCER_SERVICE(prod), &f, 0) || !f)
        return -1;
    mlt_audio_format fmt = mlt_audio_s16;
    int freq = 48000, ch = 2, n = mlt_audio_calculate_frame_samples(25, freq, frame), peak = 0;
    int16_t *pcm = NULL;
    if (!mlt_frame_get_audio(f, (void **) &pcm, &fmt, &freq, &ch, &n) && pcm)
        for (int i = 0; i < n * ch; i++)
            if (abs(pcm[i]) > peak)
                peak = abs(pcm[i]);
    mlt_frame_close(f);
    return peak;
}

/* ---- fades: from black at a clip's start, to black at its end, the sound
   with the picture; kept to the clip as it's trimmed and split; kept
   through undo, saving and opening; taken away; not where a dissolve is */
static void fades(const char *a, const char *b, const char *snd, const char *out)
{
    re_engine_io io = {show, NULL, clk, NULL, 48000, 2, NULL};
    re_project *p = re_project_new();
    re_project_add(p, a, -1);
    re_project_add(p, b, -1);
    re_engine *e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    int red = luma_at(e, 30), blue = luma_at(e, 100);
    CHECK(re_project_fade(p, 0, 0, 25) == 1 && re_project_fade(p, 1, 1, 25) == 1, "fades: not made");
    re_item it, it2;
    re_project_item(p, 0, &it);
    re_project_item(p, 1, &it2);
    CHECK(it.fade_in == 25 && it.fade_out == 0 && it2.fade_in == 0 && it2.fade_out == 25, "fades: %d %d, %d %d",
          it.fade_in, it.fade_out, it2.fade_in, it2.fade_out);
    re_engine_refresh(e);
    int y0 = luma_at(e, 0), y12 = luma_at(e, 12), y30 = luma_at(e, 30), y149 = luma_at(e, 149), y100 = luma_at(e, 100);
    CHECK(y0 <= 20 && y12 > 25 && y12 < red - 10 && abs(y30 - red) <= 4, "fades: in: luma %d, %d, %d (red %d)", y0,
          y12, y30, red);
    CHECK(y149 <= 20 && abs(y100 - blue) <= 4, "fades: out: luma %d at the end, %d before (blue %d)", y149, y100,
          blue);
    CHECK(re_project_fade(p, 0, 0, 25) == 0, "fades: the same again changed it");

    /* trimmed: the fade stays at the start; kept to the clip's length; undo */
    CHECK(re_project_set_in_out(p, 0, 10, 74) == 1, "fades: trim");
    re_engine_refresh(e);
    re_project_item(p, 0, &it);
    CHECK(it.fade_in == 25 && luma_at(e, 0) <= 20, "fades: trimmed: fade %d, luma %d at the start", it.fade_in,
          luma_at(e, 0));
    CHECK(re_project_set_in_out(p, 0, 10, 24) == 1, "fades: trimmed short");
    re_project_item(p, 0, &it);
    CHECK(it.fade_in == 15, "fades: a 15-frame clip's fade: %d", it.fade_in);
    CHECK(re_project_undo(p) && re_project_undo(p), "fades: undo");
    re_project_item(p, 0, &it);
    CHECK(it.fade_in == 25 && it.in == 0, "fades: undone: fade %d, in %d", it.fade_in, it.in);

    /* split: the fade out goes with the second part */
    CHECK(re_project_split(p, 110) == 1, "fades: split");
    re_project_item(p, 1, &it);
    re_project_item(p, 2, &it2);
    CHECK(it.fade_out == 0 && it2.fade_out == 25, "fades: split: fade outs %d, %d", it.fade_out, it2.fade_out);

    /* saved and opened */
    CHECK(re_project_save(p, out) == 0, "fades: not saved");
    re_engine_close(e);
    re_project_close(p);
    p = re_project_open(out, 1);
    CHECK(p != NULL, "fades: not opened");
    if (!p)
        return;
    re_project_item(p, 0, &it);
    re_project_item(p, 2, &it2);
    CHECK(it.fade_in == 25 && it2.fade_out == 25, "fades: opened: %d, %d", it.fade_in, it2.fade_out);
    e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    CHECK(luma_at(e, 0) <= 20, "fades: opened: luma %d at the start", luma_at(e, 0));

    /* taken away; and none where a dissolve is */
    CHECK(re_project_fade(p, 0, 0, 0) == 1, "fades: not taken away");
    re_engine_refresh(e);
    re_project_item(p, 0, &it);
    CHECK(it.fade_in == 0 && abs(luma_at(e, 0) - red) <= 4, "fades: taken away: %d, luma %d", it.fade_in,
          luma_at(e, 0));
    CHECK(re_project_fade(p, 0, 1, 10) == 1 && re_project_fade(p, 1, 0, 10) == 1, "fades: either side of the cut");
    CHECK(re_project_dissolve(p, 0, 10, 1) == 1, "fades: the dissolve: %s", layout(p));
    re_project_item(p, 0, &it);
    re_project_item(p, 2, &it2);
    CHECK(it.fade_out == 0 && it2.fade_in == 0, "fades: a dissolve kept the fades beside it: %d, %d", it.fade_out,
          it2.fade_in);
    CHECK(re_project_fade(p, 0, 1, 10) == 0 && re_project_fade(p, 2, 0, 10) == 0, "fades: made in a dissolve");
    re_engine_close(e);
    re_project_close(p);

    /* the sound fades too */
    p = re_project_new();
    re_project_add(p, snd, -1);
    int loud = peak_at(p, 40);
    re_project_fade(p, 0, 0, 25);
    int quiet = peak_at(p, 0), half = peak_at(p, 20);
    CHECK(loud > 1000 && quiet < loud / 50 && half > quiet && half < loud, "fades: the sound: %d, %d, %d (%d unfaded)",
          quiet, half, peak_at(p, 40), loud);
    re_project_close(p);
}

/* ---- a range taken out (I/O marks: Extract, Lift): clips split at both
   ends; what follows moves up or a gap is left; fades kept at the ends
   left; not across a dissolve */
static void ranges(const char *a, const char *b)
{
    re_project *p = re_project_new();
    re_project_add(p, a, -1);
    re_project_add(p, b, -1);
    re_item it;
    CHECK(re_project_fade(p, 1, 1, 10) == 1, "ranges: fade");
    CHECK(re_project_remove_range(p, 50, 99, 1) == 1 && !strcmp(layout(p), "c50 c50") && re_project_length(p) == 100,
          "ranges: extract 50-99: %s, %d", layout(p), re_project_length(p));
    re_project_item(p, 1, &it);
    CHECK(it.in == 25 && it.fade_out == 10, "ranges: what's left of b: in %d, fade out %d", it.in, it.fade_out);
    CHECK(re_project_undo(p) && !strcmp(layout(p), "c75 c75"), "ranges: one undo: %s", layout(p));
    CHECK(re_project_remove_range(p, 50, 99, 0) == 1 && !strcmp(layout(p), "c50 g50 c50") && re_project_length(p) == 150,
          "ranges: lift 50-99: %s", layout(p));
    re_project_undo(p);
    CHECK(re_project_remove_range(p, 75, 500, 0) == 1 && !strcmp(layout(p), "c75"), "ranges: lift to the end: %s",
          layout(p));
    re_project_undo(p);
    CHECK(re_project_remove_range(p, 0, 74, 1) == 1 && !strcmp(layout(p), "c75"), "ranges: a whole clip: %s", layout(p));
    re_project_undo(p);
    CHECK(re_project_remove_range(p, 80, 70, 1) == 0 && re_project_remove_range(p, 200, 210, 1) == 0,
          "ranges: out before in, or past the end");
    CHECK(re_project_dissolve(p, 0, 10, 1) == 1 && re_project_remove_range(p, 60, 80, 1) == 0
              && !strcmp(layout(p), "c65 m10 c65"),
          "ranges: across a dissolve: %s", layout(p));
    re_project_close(p);
}

/* ---- overlays: pictures over the video on V2 ---------------------------------------------- */

/* The luma at x, y of the picture at a frame (320x180), or -1 */
static int luma_xy(re_engine *e, int frame, int x, int y)
{
    re_engine_seek(e, frame);
    re_engine_poll(e);
    return shown_pos == frame && shown_w == 320 ? shown_luma[y * 320 + x] : -1;
}

static int count(const char *file, const char *what)
{
    FILE *f = fopen(file, "r");
    int n = 0, c, k = 0, len = (int) strlen(what);
    while (f && (c = fgetc(f)) != EOF) {
        k = c == what[k] ? k + 1 : c == what[0];
        if (k == len) {
            n++;
            k = 0;
        }
    }
    if (f)
        fclose(f);
    return n;
}

/* A sprite file of one sprite, its pixels given as rows of words */
static void put32(FILE *f, uint32_t v)
{
    uint8_t b[4] = {v & 255, v >> 8 & 255, v >> 16 & 255, v >> 24};
    fwrite(b, 1, 4, f);
}
static void sprite_file(const char *file, const char *name, int wwords, int h, int lbit, int rbit, uint32_t mode,
                        const uint32_t *palette, int pal_words, const uint32_t *image, const uint32_t *mask,
                        int mask_words)
{
    FILE *f = fopen(file, "wb");
    int image_at = 44 + pal_words * 4, mask_at = mask ? image_at + wwords * h * 4 : image_at;
    int size = (mask ? mask_at + mask_words * 4 : image_at + wwords * h * 4);
    put32(f, 1);
    put32(f, 16);
    put32(f, 16 + size);
    put32(f, size);
    char n[12] = {0};
    strncpy(n, name, 12);
    fwrite(n, 1, 12, f);
    put32(f, wwords - 1);
    put32(f, h - 1);
    put32(f, lbit);
    put32(f, rbit);
    put32(f, image_at);
    put32(f, mask_at);
    put32(f, mode);
    for (int i = 0; i < pal_words; i++)
        put32(f, palette[i]);
    for (int i = 0; i < wwords * h; i++)
        put32(f, image[i]);
    for (int i = 0; mask && i < mask_words; i++)
        put32(f, mask[i]);
    fclose(f);
}

/* The RGBA of a sprite's pixel x, y as &RRGGBBAA, or 1 if it can't be read */
static uint32_t pixel(const char *file, int x, int y, int *w, int *h)
{
    uint8_t *rgba;
    if (re_sprite_rgba(file, 0, &rgba, w, h))
        return 1;
    uint8_t *q = rgba + (y * *w + x) * 4;
    uint32_t v = (uint32_t) q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3];
    free(rgba);
    return v;
}

static void sprites(const char *dir)
{
    char f[600];
    int w, h;
    /* new, 32 bits (90 dpi square), a 1-bit mask: red, then green see-through */
    snprintf(f, sizeof(f), "%s/spr32", dir);
    uint32_t img32[2] = {0x000000FF, 0x0000FF00}, mask1[1] = {1};
    sprite_file(f, "logo", 2, 1, 0, 31, 6u << 27 | 90 << 14 | 90 << 1 | 1, NULL, 0, img32, mask1, 1);
    char names[4][13];
    CHECK(re_sprites_names(f, names, 4) == 1 && !strcmp(names[0], "logo"), "sprites: names");
    CHECK(pixel(f, 0, 0, &w, &h) == 0xFF0000FF && pixel(f, 1, 0, &w, &h) == 0x00FF0000 && w == 2 && h == 1,
          "sprites: 32 bits, masked: %08x %08x, %dx%d", pixel(f, 0, 0, &w, &h), pixel(f, 1, 0, &w, &h), w, h);
    /* RISC OS 5's 32 bits with alpha, RGB order */
    uint32_t img32a[2] = {0x80FF0000, 0xFF0000FF};
    sprite_file(f, "a", 2, 1, 0, 31, 15u << 27 | 6 << 20 | 3u << 14 | 1 << 6 | 1 << 4 | 1, NULL, 0, img32a, NULL, 0);
    CHECK(pixel(f, 0, 0, &w, &h) == 0xFF000080 && pixel(f, 1, 0, &w, &h) == 0x0000FFFF,
          "sprites: RISC OS 5's alpha: %08x %08x", pixel(f, 0, 0, &w, &h), pixel(f, 1, 0, &w, &h));
    /* old, mode 12 (4 bits, 90 x 45 dpi: rows doubled), the Wimp's colours, an old mask; 8 pixels, 7 used */
    uint32_t img4[1] = {0x0B0A0987u}, mask4[1] = {0x0FFFFFF0u};
    sprite_file(f, "old", 1, 1, 0, 27, 12, NULL, 0, img4, mask4, 1);
    CHECK(pixel(f, 0, 0, &w, &h) == 0x00000000 && pixel(f, 1, 1, &w, &h) == 0x004499FF
              && pixel(f, 4, 0, &w, &h) == 0x00CC00FF && w == 7 && h == 2,
          "sprites: mode 12: %08x %08x %08x, %dx%d", pixel(f, 0, 0, &w, &h), pixel(f, 1, 1, &w, &h),
          pixel(f, 4, 0, &w, &h), w, h);
    /* old, mode 28 (8 bits, square): its own palette */
    uint32_t pal[512], img8[1] = {0x00000201};
    for (int i = 0; i < 256; i++)
        pal[i * 2] = pal[i * 2 + 1] = (uint32_t) i << 8 | (255u - i) << 24; /* (&BBGGRR00) */
    sprite_file(f, "pal", 1, 1, 0, 23, 28, pal, 512, img8, NULL, 0);
    CHECK(pixel(f, 1, 0, &w, &h) == 0x0200FDFF && w == 3, "sprites: a palette: %08x, %d wide", pixel(f, 1, 0, &w, &h),
          w);
    /* written as a PNG, read back */
    uint8_t *rgba;
    snprintf(f, sizeof(f), "%s/spr32", dir);
    sprite_file(f, "logo", 2, 1, 0, 31, 6u << 27 | 90 << 14 | 90 << 1 | 1, NULL, 0, img32, mask1, 1);
    char png[600];
    snprintf(png, sizeof(png), "%s/spr32-png", dir); /* (no extension: known by its bytes) */
    CHECK(!re_sprite_rgba(f, 0, &rgba, &w, &h) && !re_still_write_png(png, rgba, w, h)
              && !re_still_open(png, &w, &h) && w == 2 && h == 1,
          "sprites: the PNG: %dx%d", w, h);
    free(rgba);
    CHECK(re_sprites_names(png, names, 4) == -1 && re_sprite_rgba(png, 0, &rgba, &w, &h) == -1,
          "sprites: a PNG taken for a sprite file");
}

/* ---- videos on V2: a picture in the picture, its sound mixed with V1's */
static void video_overlays(const char *a, const char *b, const char *snd, const char *numbered, const char *m2v,
                           const char *out)
{
    re_engine_io io = {show, NULL, clk, NULL, 48000, 2, NULL};
    re_project *p = re_project_new();
    re_project_add(p, a, -1);
    re_project_add(p, b, -1);
    re_engine *e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    int red = luma_xy(e, 20, 100, 90);
    re_overlay o;
    CHECK(re_project_overlay_add(p, numbered, 10, 0) == 0, "videos: not added");
    re_project_overlay(p, 0, &o);
    CHECK(o.video && o.start == 10 && o.length == 75 && o.in == 0 && o.source_length == 75
              && o.place == RE_PLACE_BOTTOM_RIGHT && o.size == 33 && !strcmp(o.name, strrchr(numbered, '/') ? strrchr(numbered, '/') + 1 : numbered),
          "videos: added: video %d, at %d for %d, in %d of %d, place %d size %d, \"%s\"", o.video, o.start, o.length,
          o.in, o.source_length, o.place, o.size, o.name);
    re_engine_refresh(e);
    int in_box = luma_xy(e, 15, 250, 140), outside = luma_xy(e, 15, 100, 90);
    CHECK(abs(in_box - (16 + 2 * 5)) <= 4 && abs(outside - red) <= 4,
          "videos: shown bottom right: %d in its box (frame 5's %d), %d outside it (red %d)", in_box, 16 + 2 * 5,
          outside, red);
    CHECK(abs(luma_xy(e, 40, 250, 140) - (16 + 2 * 30)) <= 4, "videos: played on: %d at 40 (frame 30's %d)",
          luma_xy(e, 40, 250, 140), 16 + 2 * 30);

    /* trimmed at its start: its in moves, what's left stays where it was */
    CHECK(re_project_overlay_span(p, 0, 15, 70) == 1, "videos: start trimmed");
    re_project_overlay(p, 0, &o);
    re_engine_refresh(e);
    CHECK(o.start == 15 && o.length == 70 && o.in == 5 && abs(luma_xy(e, 15, 250, 140) - (16 + 2 * 5)) <= 4,
          "videos: start trimmed: at %d for %d, in %d; luma %d at 15 (frame 5's %d)", o.start, o.length, o.in,
          luma_xy(e, 15, 250, 140), 16 + 2 * 5);
    CHECK(re_project_overlay_span(p, 0, 0, 85) == 1, "videos: start trimmed back");
    re_project_overlay(p, 0, &o);
    CHECK(o.start == 10 && o.length == 75 && o.in == 0, "videos: start trimmed back past the file's start: %d %d, in %d",
          o.start, o.length, o.in);
    /* its end: kept to its file */
    CHECK(re_project_overlay_span(p, 0, 10, 40) == 1 && re_project_overlay_span(p, 0, 10, 200) == 1,
          "videos: end trimmed");
    re_project_overlay(p, 0, &o);
    CHECK(o.length == 75, "videos: end trimmed past the file's end: %d", o.length);
    CHECK(re_project_overlay_span(p, 0, 10, 200) == 0, "videos: trimmed past its file's end again: a change");
    /* moved: its in kept */
    re_project_overlay_span(p, 0, 15, 70);
    CHECK(re_project_overlay_span(p, 0, 40, 70) == 1, "videos: moved");
    re_project_overlay(p, 0, &o);
    re_engine_refresh(e);
    CHECK(o.start == 40 && o.in == 5 && abs(luma_xy(e, 40, 250, 140) - (16 + 2 * 5)) <= 4,
          "videos: moved: at %d, in %d, luma %d", o.start, o.in, luma_xy(e, 40, 250, 140));
    CHECK(re_project_overlay_add(p, m2v, 0, 0) == RE_OVERLAY_NO_LENGTH, "videos: raw video, no length, added");

    /* saved, opened: the same; one mix (made when there's a video) */
    CHECK(re_project_save(p, out) == 0 && count(out, ">mix<") == 1, "videos: saved: %d mixes", count(out, ">mix<"));
    re_engine_close(e);
    re_project_close(p);
    p = re_project_open(out, 1);
    CHECK(p && re_project_overlays(p) == 1 && !re_project_overlay(p, 0, &o) && o.video && o.in == 5 && o.start == 40,
          "videos: opened: in %d, at %d", o.in, o.start);
    if (!p)
        return;
    char again[600];
    snprintf(again, sizeof(again), "%s-again", out);
    CHECK(re_project_save(p, again) == 0 && count(again, ">mix<") == 1, "videos: saved again: %d mixes",
          count(again, ">mix<"));
    re_project_close(p);

    /* the sound: V2's heard (V1 has none), both summed, V1's kept under a silent video */
    p = re_project_new();
    re_project_add(p, a, -1);
    re_project_overlay_add(p, snd, 0, 0);
    int v2_only = peak_at(p, 40);
    re_project_close(p);
    p = re_project_new();
    re_project_add(p, snd, -1);
    int loud = peak_at(p, 40);
    re_project_overlay_add(p, snd, 0, 0);
    int both = peak_at(p, 40);
    re_project_close(p);
    p = re_project_new();
    re_project_add(p, snd, -1);
    re_project_overlay_add(p, numbered, 0, 0);
    int under = peak_at(p, 40);
    re_project_close(p);
    CHECK(loud > 1000 && abs(v2_only - loud) < loud / 10, "videos: V2's sound alone: %d (%d)", v2_only, loud);
    CHECK(both > loud * 3 / 2, "videos: both sounds: %d (one: %d)", both, loud);
    CHECK(abs(under - loud) < loud / 10, "videos: V1's sound under a silent video: %d (%d)", under, loud);
}

/* A still picture's producer, asked for its picture in one form, then
   another, then the first again: each as asked, the alpha kept */
static void stills(const char *full)
{
    mlt_profile prof = mlt_profile_init(NULL);
    char res[600];
    snprintf(res, sizeof(res), "qimage:%s", full);
    mlt_producer pic = mlt_factory_producer(prof, NULL, res);
    CHECK(pic && re_still_is(pic) && mlt_producer_get_length(pic) == 15000, "stills: not opened");
    for (int k = 0; pic && k < 3; k++) {
        mlt_frame f = NULL;
        mlt_producer_seek(pic, k * 10);
        mlt_service_get_frame(MLT_PRODUCER_SERVICE(pic), &f, 0);
        mlt_image_format fmt = k == 1 ? mlt_image_yuv422 : mlt_image_rgba;
        uint8_t *img = NULL;
        int w = 320, h = 180;
        mlt_frame_get_image(f, &img, &fmt, &w, &h, 0);
        if (k == 1) {
            uint8_t *alpha = mlt_frame_get_alpha(f);
            CHECK(img && fmt == mlt_image_yuv422 && abs(img[(90 * 320 + 240) * 2] - 235) <= 4 && alpha && !alpha[90 * 320 + 80]
                      && alpha[90 * 320 + 240] == 255,
                  "stills: YUV: format %d, luma %d", fmt, img ? img[(90 * 320 + 240) * 2] : -1);
        } else {
            CHECK(img && fmt == mlt_image_rgba && img[(90 * 320 + 80) * 4 + 3] == 0 && img[(90 * 320 + 240) * 4 + 3] == 255
                      && img[(90 * 320 + 240) * 4] == 255,
                  "stills: RGBA (%d): format %d", k, fmt);
        }
        mlt_frame_close(f);
    }
    /* a smaller picture asked for: made so */
    mlt_frame f = NULL;
    mlt_service_get_frame(MLT_PRODUCER_SERVICE(pic), &f, 0);
    mlt_image_format fmt = mlt_image_rgba;
    uint8_t *img = NULL;
    int w = 160, h = 90;
    mlt_frame_get_image(f, &img, &fmt, &w, &h, 0);
    CHECK(img && w == 160 && h == 90 && img[(45 * 160 + 40) * 4 + 3] == 0 && img[(45 * 160 + 120) * 4 + 3] == 255,
          "stills: smaller: %dx%d", w, h);
    mlt_frame_close(f);
    /* the rescale filter's way: asked at its own size, told the size wanted (made so: nothing scaled after) */
    mlt_producer bare = mlt_factory_producer(prof, "qimage", (char *) full); /* (no loader: no filters) */
    f = NULL;
    mlt_service_get_frame(MLT_PRODUCER_SERVICE(bare), &f, 0);
    mlt_properties_set_int(MLT_FRAME_PROPERTIES(f), "rescale_width", 80);
    mlt_properties_set_int(MLT_FRAME_PROPERTIES(f), "rescale_height", 46);
    fmt = mlt_image_rgba;
    w = 320, h = 180;
    mlt_frame_get_image(f, &img, &fmt, &w, &h, 0);
    CHECK(w == 80 && h == 46, "stills: rescale_width asked for 80x46: %dx%d", w, h);
    mlt_frame_close(f);
    mlt_producer_close(bare);
    mlt_producer_close(pic);
    mlt_profile_close(prof);
}

static void overlays(const char *a, const char *b, const char *snd, const char *out, const char *full,
                     const char *logo, const char *jpg)
{
    re_engine_io io = {show, NULL, clk, NULL, 48000, 2, NULL};
    re_project *p = re_project_new();
    re_overlay o;
    CHECK(re_project_overlay_add(p, full, 0, 0) == -1, "overlays: one in an empty project");
    re_project_add(p, a, -1);
    re_project_add(p, b, -1);
    re_engine *e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    int red = luma_xy(e, 20, 80, 90), blue = luma_xy(e, 100, 80, 90);
    {
        char junk[600];
        snprintf(junk, sizeof(junk), "%s-junk", out);
        FILE *f = fopen(junk, "w");
        fputs("not a picture, not a video\n", f);
        fclose(f);
        CHECK(re_project_overlay_add(p, junk, 0, 0) == RE_OVERLAY_CANT_OPEN, "overlays: a text file opened");
    }

    /* a picture as big as the frame: all of it, its alpha kept (left half clear, right white) */
    const int white = 235;
    CHECK(re_project_overlay_add(p, full, 10, 0) == 0 && re_project_overlays(p) == 1, "overlays: not added");
    re_project_overlay(p, 0, &o);
    CHECK(o.start == 10 && o.length == 125 && o.place == RE_PLACE_FULL && o.opacity == 100 && !strcmp(o.name, "full"),
          "overlays: added: %d %d, place %d, opacity %d, \"%s\"", o.start, o.length, o.place, o.opacity, o.name);
    CHECK(re_project_length(p) == 150, "overlays: the project's length: %d", re_project_length(p));
    re_engine_refresh(e);
    luma_xy(e, 5, 80, 90);
    int cb0 = shown_cb[45 * 160 + 40], cr0 = shown_cr[45 * 160 + 40]; /* (red's colour, no overlay) */
    int l = luma_xy(e, 20, 80, 90), r = luma_xy(e, 20, 240, 90), before = luma_xy(e, 5, 240, 90),
        after = luma_xy(e, 140, 240, 90);
    luma_xy(e, 20, 80, 90);
    CHECK(abs(shown_cb[45 * 160 + 40] - cb0) <= 3 && abs(shown_cr[45 * 160 + 40] - cr0) <= 3
              && abs(shown_cb[45 * 160 + 120] - 128) <= 3 && abs(shown_cr[45 * 160 + 120] - 128) <= 3
              && abs(shown_luma[90 * 320 + 81] - red) <= 4,
          "overlays: colours: red's %d,%d under the clear half (%d,%d without), %d,%d under the white; luma %d at "
          "an odd x",
          shown_cb[45 * 160 + 40], shown_cr[45 * 160 + 40], cb0, cr0, shown_cb[45 * 160 + 120],
          shown_cr[45 * 160 + 120], shown_luma[90 * 320 + 81]);
    CHECK(abs(l - red) <= 4 && abs(r - white) <= 4 && abs(before - red) <= 4,
          "overlays: shown: luma %d left, %d right (red %d), %d before it", l, r, red, before);
    CHECK(abs(after - blue) <= 4, "overlays: after it (over b): %d", after);
    CHECK(abs(luma_xy(e, 21, 80, 90) - red) <= 4 && abs(luma_xy(e, 22, 80, 90) - red) <= 4,
          "overlays: the next frames: %d, %d", luma_xy(e, 21, 80, 90), luma_xy(e, 22, 80, 90));
    CHECK(re_project_overlay_at(p, 9) == -1 && re_project_overlay_at(p, 10) == 0 && re_project_overlay_at(p, 134) == 0
              && re_project_overlay_at(p, 135) == -1,
          "overlays: at");

    /* half opaque */
    CHECK(re_project_overlay_set(p, 0, -1, -1, 50) == 1, "overlays: opacity");
    re_engine_refresh(e);
    r = luma_xy(e, 20, 240, 90);
    CHECK(r > red + 40 && r < white - 40, "overlays: half opaque: %d (red %d)", r, red);
    CHECK(re_project_undo(p) && (re_engine_close(e), e = re_engine_open_producer(re_project_profile(p),
                                                                                  re_project_producer(p), 1, &io))
              && abs(luma_xy(e, 20, 240, 90) - white) <= 4,
          "overlays: opacity undone: %d", luma_xy(e, 20, 240, 90));

    /* faded in: none at its first frame, half way at the middle */
    CHECK(re_project_overlay_fade(p, 0, 0, 20) == 1 && re_project_overlay_fade(p, 0, 1, 20) == 1, "overlays: fades");
    re_engine_refresh(e);
    int f0 = luma_xy(e, 10, 240, 90), f10 = luma_xy(e, 20, 240, 90), f30 = luma_xy(e, 40, 240, 90),
        f125 = luma_xy(e, 125, 240, 90), fend = luma_xy(e, 134, 240, 90);
    CHECK(abs(f0 - red) <= 4 && f10 > red + 40 && f10 < white - 40 && abs(f30 - white) <= 4,
          "overlays: faded in: %d, %d, %d", f0, f10, f30);
    CHECK(f125 > blue + 40 && f125 < white - 40 && abs(fend - blue) <= 4,
          "overlays: faded out (over b): %d, %d at the end (blue %d)", f125, fend, blue);
    re_project_overlay(p, 0, &o);
    CHECK(o.fade_in == 20 && o.fade_out == 20, "overlays: fades: %d %d", o.fade_in, o.fade_out);

    /* a logo where there's room: before the first (shortened to fit), top right */
    CHECK(re_project_overlay_add(p, logo, 0, 0) == 0 && re_project_overlays(p) == 2, "overlays: the logo");
    re_project_overlay(p, 0, &o);
    CHECK(o.start == 0 && o.length == 10 && o.place == RE_PLACE_TOP_RIGHT && o.size == 20,
          "overlays: the logo: %d %d, place %d, size %d", o.start, o.length, o.place, o.size);
    re_engine_refresh(e);
    int in = luma_xy(e, 5, 290, 20), out_of = luma_xy(e, 5, 200, 20), below = luma_xy(e, 5, 290, 60);
    luma_xy(e, 5, 0, 0);
    int edge_in = shown_luma[20 * 320 + 241], edge_out = shown_luma[20 * 320 + 237];
    CHECK(edge_in > 200 && abs(edge_out - red) <= 4, "overlays: the logo's left edge (x 240): %d at 241, %d at 237",
          edge_in, edge_out);
    CHECK(in > 200 && abs(out_of - red) <= 4 && abs(below - red) <= 4,
          "overlays: the logo: luma %d in its box, %d left of it, %d below (red %d)", in, out_of, below, red);
    CHECK(re_project_overlay_set(p, 0, RE_PLACE_BOTTOM_LEFT, 30, -1) == 1, "overlays: placed bottom left");
    re_engine_refresh(e);
    CHECK(luma_xy(e, 5, 30, 150) > 200 && abs(luma_xy(e, 5, 290, 20) - red) <= 4, "overlays: bottom left: %d, %d",
          luma_xy(e, 5, 30, 150), luma_xy(e, 5, 290, 20));
    /* one at a frame inside another: after it */
    CHECK(re_project_overlay_add(p, jpg, 50, 0) == 2, "overlays: the JPEG");
    re_project_overlay(p, 2, &o);
    CHECK(o.start == 135 && o.length == 15, "overlays: the JPEG after the full one: %d %d", o.start, o.length);
    CHECK(re_project_overlay_add(p, logo, 140, 0) == -1, "overlays: added with no room");

    /* moved and trimmed: kept clear of the others, within the project */
    CHECK(re_project_overlay_remove(p, 2) == 1 && re_project_overlays(p) == 2, "overlays: removed");
    CHECK(re_project_overlay_span(p, 1, 20, 125) == 1, "overlays: moved");
    re_project_overlay(p, 1, &o);
    CHECK(o.start == 20 && o.length == 125 && o.fade_in == 20, "overlays: moved: %d %d, fade %d", o.start, o.length,
          o.fade_in);
    CHECK(re_project_overlay_span(p, 1, 40, 125) == 1, "overlays: moved to the end");
    re_project_overlay(p, 1, &o);
    CHECK(o.start == 25 && o.length == 125, "overlays: moved past the end: %d %d", o.start, o.length);
    CHECK(re_project_overlay_span(p, 1, 0, 125) == 1, "overlays: moved onto the logo");
    re_project_overlay(p, 1, &o);
    CHECK(o.start == 10, "overlays: moved onto the logo: at %d", o.start);
    CHECK(re_project_overlay_span(p, 1, 10, 500) == 1, "overlays: trimmed longer");
    re_project_overlay(p, 1, &o);
    CHECK(o.start == 10 && o.length == 140, "overlays: trimmed past the end: %d %d", o.start, o.length);
    CHECK(re_project_overlay_span(p, 0, 0, 30) == 0 || (re_project_overlay(p, 0, &o), o.length == 10),
          "overlays: the logo trimmed over the next");

    /* saved, opened: as they were, played; saved again: no more composites than overlays */
    CHECK(re_project_save(p, out) == 0, "overlays: not saved");
    CHECK(count(out, "composite") == 2 && count(out, "qimage") == 2, "overlays: saved: %d composites, %d qimages",
          count(out, "composite"), count(out, "qimage"));
    re_engine_close(e);
    re_project_close(p);
    p = re_project_open(out, 1);
    CHECK(p && re_project_overlays(p) == 2, "overlays: opened: %d", p ? re_project_overlays(p) : -1);
    if (!p)
        return;
    re_project_overlay(p, 1, &o);
    CHECK(o.start == 10 && o.length == 140 && o.fade_in == 20 && o.place == RE_PLACE_FULL,
          "overlays: opened: %d %d, fade %d, place %d", o.start, o.length, o.fade_in, o.place);
    e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    CHECK(abs(luma_xy(e, 40, 240, 90) - white) <= 4 && abs(luma_xy(e, 40, 80, 90) - red) <= 4,
          "overlays: opened: played: %d, %d", luma_xy(e, 40, 240, 90), luma_xy(e, 40, 80, 90));
    char again[600];
    snprintf(again, sizeof(again), "%s-again", out);
    CHECK(re_project_save(p, again) == 0 && count(again, "composite") == 2, "overlays: saved again: %d composites",
          count(again, "composite"));

    /* a project whose composites have gone (made elsewhere): made again when it's opened */
    {
        FILE *f = fopen(again, "r");
        static char xml[65536], cut[65536];
        size_t n = f ? fread(xml, 1, sizeof(xml) - 1, f) : 0;
        if (f)
            fclose(f);
        xml[n] = 0;
        char *w = cut, *r = xml, *t;
        while ((t = strstr(r, "<transition"))) {
            char *end = strstr(t, "</transition>");
            int ours = end && strstr(t, "reeledit_overlay") && strstr(t, "reeledit_overlay") < end;
            size_t keep = (size_t) (ours ? t - r : (end ? end + 13 : t + 11) - r);
            memcpy(w, r, keep);
            w += keep;
            r = ours ? end + 13 : r + keep;
        }
        strcpy(w, r);
        snprintf(again, sizeof(again), "%s-bare", out);
        f = fopen(again, "w");
        fputs(cut, f);
        fclose(f);
        re_engine_close(e);
        re_project_close(p);
        p = re_project_open(again, 1);
        CHECK(p && count(again, "composite") == 0 && re_project_overlays(p) == 2, "overlays: no composites: opened");
        if (!p)
            return;
        e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
        CHECK(abs(luma_xy(e, 40, 240, 90) - white) <= 4 && abs(luma_xy(e, 40, 80, 90) - red) <= 4,
              "overlays: no composites: played: %d, %d", luma_xy(e, 40, 240, 90), luma_xy(e, 40, 80, 90));
    }

    /* V1 edited: the overlays stay where they are */
    CHECK(re_project_remove(p, 0, 1) == 1 && re_project_overlays(p) == 2, "overlays: V1 edited");
    re_project_overlay(p, 1, &o);
    CHECK(o.start == 10, "overlays: V1 edited: the overlay moved to %d", o.start);
    re_engine_close(e);
    re_project_close(p);

    /* the sound is still V1's */
    p = re_project_new();
    re_project_add(p, snd, -1);
    int loud = peak_at(p, 40);
    re_project_overlay_add(p, full, 0, 0);
    CHECK(loud > 1000 && peak_at(p, 40) == loud, "overlays: the sound: %d, %d without", peak_at(p, 40), loud);
    /* a picture on V1: 5 seconds of it */
    CHECK(re_project_add(p, full, -1) == 1, "overlays: a picture on V1");
    re_item it;
    re_project_item(p, 1, &it);
    CHECK(it.length == 125, "overlays: a picture on V1: %d frames", it.length);
    re_project_close(p);
}

int main(int argc, char **argv)
{
    if (argc != 13) {
        fprintf(stderr, "usage: project_test a.mp4 b.mp4 numbered.mp4 out.mlt numbered.m2v four3-709.mp4 big.mp4 "
                        "wide-dvd.mp4 numbered-snd.mp4 full.png logo.png pic.jpg\n");
        return 2;
    }
    const char *a = argv[1], *b = argv[2], *n = argv[3], *out = argv[4];
    re_project *p = re_project_new();
    CHECK(p && re_project_items(p) == 0 && re_project_length(p) == 0 && !re_project_producer(p), "not empty");
    CHECK(re_project_add(p, "no-such-file.mp4", -1) < 0, "a missing file added");

    CHECK(re_project_add(p, a, -1) == 0, "a not added");
    mlt_profile pr = re_project_profile(p);
    CHECK(pr->width == 320 && pr->height == 180 && re_project_fps(p) == 25, "profile %dx%d %g fps", pr->width,
          pr->height, re_project_fps(p));
    CHECK(re_project_add(p, b, -1) == 1, "b not at the end");
    CHECK(!strcmp(layout(p), "c75 c75") && re_project_length(p) == 150, "%s, %d", layout(p), re_project_length(p));
    CHECK(re_project_modified(p), "not modified");

    /* the engine plays it; edits are seen after a refresh */
    re_engine_io io = {show, NULL, clk, NULL, 48000, 2, NULL};
    re_engine *e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    CHECK(e && re_engine_length(e) == 150, "engine: length %d", e ? re_engine_length(e) : -1);
    re_engine_seek(e, 10);
    re_engine_poll(e);
    int red_y = shown_y;
    CHECK(red_y > 70 && red_y < 90, "frame 10: luma %d, not red's", red_y);

    /* numbered at frame 10: nearer a's start, so before it */
    unsigned ch = re_project_changes(p);
    CHECK(re_project_add(p, n, 10) == 0, "numbered not first");
    CHECK(re_project_changes(p) > ch, "no change counted");
    CHECK(!strcmp(layout(p), "c75 c75 c75"), "%s", layout(p));
    CHECK(!strcmp(name_of(p, 0), "numbered.mp4") && !strcmp(name_of(p, 1), "a.mp4"), "order %s, %s",
          name_of(p, 0), name_of(p, 1));
    re_engine_refresh(e);
    re_engine_poll(e);
    CHECK(re_engine_length(e) == 225, "engine: length %d after adding", re_engine_length(e));
    CHECK(shown_pos == 10 && abs(shown_y - 36) <= 1, "after the edit frame 10: luma %d, not numbered's 36", shown_y);
    /* at frame 140 (b's 65th... no: a is 75-149; 140 nearer a's end): after a */
    CHECK(re_project_add(p, n, 140) == 2, "numbered at 140 not after a");
    CHECK(!strcmp(layout(p), "c75 c75 c75 c75"), "%s", layout(p));

    /* split: inside an item, not at its ends */
    CHECK(re_project_split(p, 30) == 1, "no split at 30");
    CHECK(!strcmp(layout(p), "c30 c45 c75 c75 c75"), "%s", layout(p));
    re_item it;
    re_project_item(p, 1, &it);
    CHECK(it.in == 30 && it.out == 74 && it.start == 30, "the second half: in %d out %d start %d", it.in, it.out,
          it.start);
    CHECK(re_project_split(p, 75) == 0 && re_project_split(p, 0) == 0, "split at an item's end");
    CHECK(re_project_split(p, 100000) == 0, "split past the end");

    /* remove: ripple, then leaving a gap, then the gap */
    CHECK(re_project_remove(p, 1, 1) == 1, "not removed");
    CHECK(!strcmp(layout(p), "c30 c75 c75 c75"), "ripple: %s", layout(p));
    CHECK(re_project_remove(p, 1, 0) == 1, "not removed (gap)");
    CHECK(!strcmp(layout(p), "c30 g75 c75 c75"), "gap: %s", layout(p));
    CHECK(re_project_item_at(p, 50) == 1, "item at 50: %d", re_project_item_at(p, 50));
    re_engine_refresh(e);
    re_engine_seek(e, 50);
    re_engine_poll(e);
    CHECK(shown_y < 20, "in the gap: luma %d, not black", shown_y);
    CHECK(re_project_remove(p, 1, 0) == 1 && !strcmp(layout(p), "c30 c75 c75"), "the gap: %s", layout(p));
    CHECK(re_project_remove(p, 2, 0) == 1 && !strcmp(layout(p), "c30 c75"), "a gap at the end: %s", layout(p));
    CHECK(re_project_remove(p, 9, 1) == 0, "removed an item that isn't there");

    /* saved and opened again */
    CHECK(re_project_save(p, out) == 0, "not saved");
    CHECK(!re_project_modified(p), "modified after saving");
    char want[256];
    snprintf(want, sizeof(want), "%s", layout(p));
    re_engine_close(e);
    re_project_close(p);
    p = re_project_open(out, 1);
    CHECK(p != NULL, "can't open %s", out);
    if (p) {
        CHECK(!strcmp(layout(p), want), "opened again: %s, not %s", layout(p), want);
        CHECK(!re_project_modified(p), "modified when just opened");
        CHECK(re_project_profile(p)->width == 320 && re_project_fps(p) == 25, "opened: profile");
        re_project_item(p, 0, &it);
        CHECK(it.in == 0 && it.out == 29 && !strcmp(it.name, "numbered.mp4"), "opened: first item %s %d-%d",
              it.name, it.in, it.out);
        /* edited after opening */
        CHECK(re_project_add(p, b, -1) == 2 && re_project_length(p) == 180, "opened, then added: %s",
              layout(p));
        re_project_close(p);
    }

    /* not a project: refused, no harm done (as MLT XML, and by name) */
    {
        FILE *f = fopen("not-a-project", "w");
        if (f) {
            fputs("this isn't XML\n", f);
            fclose(f);
        }
        CHECK(re_project_open("not-a-project", 1) == NULL, "a text file opened as a project");
        CHECK(re_project_open("no-such-file.mlt", 1) == NULL, "a missing file opened as a project");
        CHECK(re_project_open(a, 1) == NULL, "a video opened as MLT XML");
    }

    /* a clip opened as a project */
    p = re_project_open(n, 0);
    CHECK(p && !strcmp(layout(p), "c75") && !re_project_modified(p), "a clip as a project: %s",
          p ? layout(p) : "none");
    re_project_close(p);

    /* ripple trims, cuts, undo and redo */
    p = re_project_new();
    re_project_add(p, a, -1);
    re_project_add(p, b, -1);
    re_project_split(p, 30);
    CHECK(!strcmp(layout(p), "c30 c45 c75"), "undo set-up: %s", layout(p));
    CHECK(re_project_trim(p, 40, 0) == 1 && !strcmp(layout(p), "c30 c35 c75"), "Q at 40: %s", layout(p));
    re_project_item(p, 1, &it);
    CHECK(it.in == 40, "Q: the piece starts at %d in the clip, not 40", it.in);
    CHECK(re_project_trim(p, 100, 1) == 1 && !strcmp(layout(p), "c30 c35 c35"), "W at 100: %s", layout(p));
    CHECK(re_project_trim(p, 30, 0) == 0 && re_project_trim(p, 500, 1) == 0, "a trim at a cut or past the end");
    CHECK(re_project_cut(p, 50, -1) == 30 && re_project_cut(p, 50, 1) == 65 && re_project_cut(p, 65, 1) == 100
              && re_project_cut(p, 0, -1) == -1 && re_project_cut(p, 100, 1) == -1,
          "cuts: %d %d %d", re_project_cut(p, 50, -1), re_project_cut(p, 50, 1), re_project_cut(p, 65, 1));
    CHECK(re_project_save(p, out) == 0 && !re_project_modified(p), "saved");
    CHECK(re_project_undo(p) && !strcmp(layout(p), "c30 c35 c75") && re_project_modified(p), "undo W: %s",
          layout(p));
    CHECK(re_project_redo(p) && !strcmp(layout(p), "c30 c35 c35") && !re_project_modified(p),
          "redo W: %s (modified %d)", layout(p), re_project_modified(p));
    CHECK(re_project_undo(p) && re_project_undo(p) && re_project_undo(p) && !strcmp(layout(p), "c75 c75"),
          "undo three: %s", layout(p));
    e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    re_engine_seek(e, 80);
    re_engine_poll(e);
    CHECK(shown_y < 50, "after undoing, frame 80 is b's (blue), luma %d", shown_y);
    re_engine_close(e);
    CHECK(re_project_undo(p) && re_project_undo(p) && re_project_items(p) == 0 && !re_project_producer(p),
          "undone to empty: %s", layout(p));
    CHECK(!re_project_undo(p) && !re_project_can_undo(p) && re_project_can_redo(p), "undo past the start");
    for (int i = 0; i < 5; i++)
        re_project_redo(p);
    CHECK(!strcmp(layout(p), "c30 c35 c35") && !re_project_modified(p) && !re_project_can_redo(p),
          "redone all: %s", layout(p));
    re_project_undo(p);
    re_project_split(p, 10);
    CHECK(!re_project_can_redo(p), "an edit after undo left something to redo");
    re_project_close(p);

    /* ---- a file that doesn't say how long it is (raw MPEG-2): measured */
    const char *m2v = argv[5];
    p = re_project_new();
    CHECK(re_project_add(p, m2v, -1) == RE_ADD_MEASURE && re_project_items(p) == 0,
          "a raw .m2v not sent to be measured (%s)", layout(p));
    re_measure *ms = re_measure_open(m2v);
    CHECK(ms != NULL, "can't measure %s", m2v);
    int r = 0, steps = 0;
    while (ms && (r = re_measure_step(ms, 10)) == 0)
        steps++;
    CHECK(r == 1 && steps >= 5, "measuring: %d after %d steps (of 10 packets)", r, steps);
    int len = ms ? re_project_frames(p, re_measure_seconds(ms)) : 0;
    CHECK(len == 75 && (!ms || re_measure_progress(ms) > 0.99), "measured %d frames (%.3f s), not 75", len,
          ms ? re_measure_seconds(ms) : 0);
    re_measure_close(ms);
    CHECK(re_measure_open("no-such-file.m2v") == NULL, "measured a missing file");
    CHECK(re_project_add_length(p, m2v, -1, len) == 0 && !strcmp(layout(p), "c75") && re_project_length(p) == 75,
          "added measured: %s", layout(p));
    e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    CHECK(e && re_engine_length(e) == 75, "engine: length %d", e ? re_engine_length(e) : -1);
    re_engine_seek(e, 74);
    re_engine_poll(e);
    CHECK(shown_pos == 74 && abs(shown_y - (16 + 2 * 74)) <= 3, "the .m2v's last frame: %d, luma %d", shown_pos,
          shown_y);
    re_engine_close(e);
    CHECK(re_project_save(p, out) == 0, "saving with the .m2v");
    re_project_close(p);
    p = re_project_open(out, 1);
    CHECK(p && !strcmp(layout(p), "c75"), "the .m2v's length not kept: %s", p ? layout(p) : "(not opened)");
    re_project_close(p);

    /* ---- drags: trimming an edge in many steps (one undo for all), and
       moving a clip */
    p = re_project_new();
    re_project_add(p, a, -1);
    re_project_add(p, b, -1);
    CHECK(re_project_save(p, out) == 0 && !re_project_modified(p), "drags: not saved");
    re_project_begin(p);
    CHECK(re_project_set_in_out(p, 1, 10, 74) == 1 && !strcmp(layout(p), "c75 c65"), "drags: in 10: %s",
          layout(p));
    CHECK(re_project_set_in_out(p, 1, 20, 74) == 1 && !strcmp(layout(p), "c75 c55"), "drags: in 20: %s",
          layout(p));
    re_project_end(p);
    CHECK(re_project_modified(p), "drags: trimmed, but not modified");
    CHECK(re_project_undo(p) && !strcmp(layout(p), "c75 c75"), "drags: one undo for the drag: %s", layout(p));
    CHECK(!re_project_modified(p), "drags: undone to the saved state, but modified");
    CHECK(re_project_redo(p) && !strcmp(layout(p), "c75 c55"), "drags: redo: %s", layout(p));
    /* back where it was before the end: no undo point, not modified */
    re_project_undo(p);
    int could_undo = re_project_can_undo(p);
    re_project_begin(p);
    re_project_set_in_out(p, 1, 30, 74);
    re_project_set_in_out(p, 1, 0, 74);
    re_project_end(p);
    CHECK(re_project_can_undo(p) == could_undo && !re_project_modified(p),
          "drags: a drag back to where it began left an undo point (%d) or modified (%d)", re_project_can_undo(p),
          re_project_modified(p));
    /* kept to the file, and at least a frame */
    CHECK(re_project_set_in_out(p, 0, 0, 500) == 0 && !strcmp(layout(p), "c75 c75"), "drags: past the file's end: %s",
          layout(p));
    CHECK(re_project_set_in_out(p, 0, 40, 30) == 1 && !strcmp(layout(p), "c1 c75"), "drags: in past out: %s",
          layout(p));
    re_project_undo(p);
    /* moving: a's clip after b's; and back */
    char n0[64], n1[64];
    snprintf(n0, sizeof(n0), "%s", name_of(p, 0));
    snprintf(n1, sizeof(n1), "%s", name_of(p, 1));
    CHECK(re_project_move(p, 0, 1) == -1 && re_project_move(p, 0, 0) == -1, "drags: a move to where it is");
    CHECK(re_project_move(p, 0, 2) == 1 && !strcmp(name_of(p, 0), n1) && !strcmp(name_of(p, 1), n0),
          "drags: moved to the end: %s, %s", name_of(p, 0), name_of(p, 1));
    CHECK(re_project_move(p, 1, 0) == 0 && !strcmp(name_of(p, 0), n0), "drags: moved to the start: %s",
          name_of(p, 0));
    CHECK(re_project_undo(p) && !strcmp(name_of(p, 0), n1), "drags: undoing a move: %s", name_of(p, 0));
    re_project_close(p);

    /* ---- clips of other sizes and shapes: shown as they are, in their own
       colours, up to 1080p (MLT's fitting converted them several times) */
    p = re_project_new();
    re_project_add(p, a, -1);                /* 320x180, BT.601: the profile */
    re_project_add(p, argv[6], -1);          /* 480x360 (4:3), BT.709 */
    re_project_add(p, argv[7], -1);          /* 2560x1440: bigger than the overlay takes */
    re_project_add(p, argv[8], -1);          /* 720x576, anamorphic 16:9 */
    e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    re_engine_seek(e, 10);
    re_engine_poll(e);
    CHECK(shown_w == 320 && shown_h == 180 && fabs(shown_dar - 16.0 / 9) < 0.01 && shown_cs == 601,
          "the profile's own clip: %dx%d, shape %.3f, BT.%d", shown_w, shown_h, shown_dar, shown_cs);
    re_engine_seek(e, 80);
    re_engine_poll(e);
    CHECK(shown_w == 480 && shown_h == 360 && fabs(shown_dar - 4.0 / 3) < 0.01,
          "a 4:3 480x360 clip shown at %dx%d, shape %.3f (not as it is)", shown_w, shown_h, shown_dar);
    CHECK(shown_cs == 709, "the BT.709 clip shown as BT.%d (converted)", shown_cs);
    re_engine_seek(e, 75 + 75 + 5);
    re_engine_poll(e);
    CHECK(shown_w == 320 && shown_h == 180, "a 2560x1440 clip shown at %dx%d, not fitted to 320x180", shown_w,
          shown_h);
    re_engine_seek(e, 75 + 75 + 25 + 5);
    re_engine_poll(e);
    CHECK(shown_w == 720 && shown_h == 576 && fabs(shown_dar - 16.0 / 9) < 0.01,
          "an anamorphic 720x576 16:9 clip shown at %dx%d, shape %.3f", shown_w, shown_h, shown_dar);
    re_engine_close(e);
    re_project_close(p);

    dissolves(a, b, argv[6], out);
    fades(a, b, argv[9], out);
    ranges(a, b);
    stills(argv[10]);
    overlays(a, b, argv[9], out, argv[10], argv[11], argv[12]);
    video_overlays(a, b, argv[9], n, argv[5], out);
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", out);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = 0;
    else
        strcpy(dir, ".");
    sprites(dir);

    printf("project_test: %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
}
