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
#include "export.h"
#include <libavformat/avformat.h>
#include "project.h"
#include "measure.h"
#include "media.h"
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

/* The same, but only sound at 48 kHz counts (as the engine takes it): -1 at another rate */
static int peak_48k(re_project *p, int frame)
{
    mlt_producer prod = re_project_producer(p);
    mlt_frame f = NULL;
    mlt_producer_seek(prod, frame);
    if (mlt_service_get_frame(MLT_PRODUCER_SERVICE(prod), &f, 0) || !f)
        return -1;
    mlt_audio_format fmt = mlt_audio_s16;
    int freq = 48000, ch = 2, n = mlt_audio_calculate_frame_samples(25, freq, frame), peak = 0;
    int16_t *pcm = NULL;
    if (!mlt_frame_get_audio(f, (void **) &pcm, &fmt, &freq, &ch, &n) && pcm && freq == 48000 && fmt == mlt_audio_s16) {
        for (int i = 0; i < n * ch; i++)
            if (abs(pcm[i]) > peak)
                peak = abs(pcm[i]);
    } else {
        peak = -1;
    }
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
    /* (the colour fades towards grey with the luma: red's Cr from 128 up) */
    int cr0 = (luma_at(e, 0), shown_cr[45 * 160 + 80]), cr12 = (luma_at(e, 12), shown_cr[45 * 160 + 80]);
    int cr30 = (luma_at(e, 30), shown_cr[45 * 160 + 80]);
    CHECK(shown_w == 320 && cr30 > 180 && abs(cr0 - 128) <= 4 && cr12 > 138 && cr12 < cr30 - 10,
          "fades: in: Cr %d, %d, %d", cr0, cr12, cr30);
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

/* ---- a clip's sound alone: muted (kept through undo, saving, splitting,
   its dissolves; another clip of the same file not muted); faded without
   the picture; a fade of both, the sound's then taken away alone; a video
   on V2 muted and faded, with its picture or alone; not for clips without
   sound */
static void clip_sound(const char *silent, const char *snd, const char *out)
{
    re_project *p = re_project_new();
    re_project_add(p, snd, -1);
    re_project_add(p, snd, -1);
    re_item it, it2;
    int loud = peak_at(p, 40);
    re_project_item(p, 0, &it);
    CHECK(loud > 1000 && it.sound && !it.muted, "sound: %d unmuted, sound %d, muted %d", loud, it.sound, it.muted);
    CHECK(re_project_mute(p, 0, 1) == 1 && re_project_mute(p, 0, 1) == 0, "sound: muted (once)");
    re_project_item(p, 0, &it);
    CHECK(it.muted && peak_at(p, 40) < 30 && peak_at(p, 110) > 1000, "sound: muted: %d, %d at 40, %d in the next clip",
          it.muted, peak_at(p, 40), peak_at(p, 110));
    CHECK(re_project_undo(p) && peak_at(p, 40) > 1000 && re_project_redo(p) && peak_at(p, 40) < 30,
          "sound: undo, redo: %d", peak_at(p, 40));
    CHECK(re_project_save(p, out) == 0, "sound: not saved");
    re_project_close(p);
    p = re_project_open(out, 1);
    CHECK(p && peak_at(p, 40) < 30 && peak_at(p, 110) > 1000, "sound: opened: %d, %d", peak_at(p, 40), peak_at(p, 110));
    /* split: both parts muted */
    CHECK(re_project_sound_fade(p, 0, 1, 10) == 1 && re_project_split(p, 30) == 1, "sound: split");
    re_project_item(p, 0, &it2);
    re_project_item(p, 1, &it);
    CHECK(it.muted && peak_at(p, 20) < 30 && peak_at(p, 50) < 30 && it.sound_fade_out == 10 && it2.sound_fade_out == 0,
          "sound: split: muted %d, %d, %d; sound fades out %d, %d", it.muted, peak_at(p, 20), peak_at(p, 50),
          it2.sound_fade_out, it.sound_fade_out);
    re_project_undo(p);
    /* a dissolve between two muted clips: silent through it; one unmuted: heard */
    CHECK(re_project_mute(p, 1, 1) == 1 && re_project_dissolve(p, 0, 20, 1) == 1, "sound: dissolve: %s", layout(p));
    CHECK(peak_at(p, 65) < 30, "sound: a dissolve between muted clips: %d", peak_at(p, 65));
    CHECK(re_project_mute(p, 0, 0) == 1 && peak_at(p, 57) > 500, "sound: the first unmuted: %d in the dissolve",
          peak_at(p, 57));
    re_project_close(p);

    /* faded alone; both; the sound's taken away alone; trimmed */
    p = re_project_new();
    re_project_add(p, snd, -1);
    CHECK(re_project_sound_fade(p, 0, 0, 25) == 1 && re_project_sound_fade(p, 0, 0, 25) == 0, "sound: faded in");
    re_project_item(p, 0, &it);
    int quiet = peak_at(p, 0), half = peak_at(p, 12);
    CHECK(it.sound_fade_in == 25 && it.fade_in == 0 && quiet < loud / 50 && half > quiet && half < loud,
          "sound: faded in alone: %d (picture %d): %d, %d", it.sound_fade_in, it.fade_in, quiet, half);
    CHECK(re_project_fade(p, 0, 1, 20) == 1, "sound: a fade out of both");
    re_project_item(p, 0, &it);
    CHECK(it.fade_out == 20 && it.sound_fade_out == 20 && peak_at(p, 74) < loud / 10,
          "sound: both faded out: %d, %d, %d", it.fade_out, it.sound_fade_out, peak_at(p, 74));
    CHECK(re_project_sound_fade(p, 0, 1, 0) == 1, "sound: its fade out taken away");
    re_project_item(p, 0, &it);
    CHECK(it.fade_out == 20 && it.sound_fade_out == 0 && peak_at(p, 74) > 1000,
          "sound: the picture's fade out kept, the sound's gone: %d, %d, %d", it.fade_out, it.sound_fade_out,
          peak_at(p, 74));
    CHECK(re_project_set_in_out(p, 0, 10, 74) == 1, "sound: trimmed");
    re_project_item(p, 0, &it);
    CHECK(it.sound_fade_in == 25 && peak_at(p, 0) < loud / 50, "sound: trimmed: fade %d, %d at the start",
          it.sound_fade_in, peak_at(p, 0));
    re_project_close(p);

    /* a level: -12 dB (a quarter); kept split; through a dissolve; undone; saved */
    p = re_project_new();
    re_project_add(p, snd, -1);
    re_project_add(p, snd, -1);
    CHECK(re_project_volume(p, 0, -12) == 1 && re_project_volume(p, 0, -12) == 0, "volume: set (once)");
    re_project_item(p, 0, &it);
    int quarter = peak_at(p, 40);
    CHECK(it.volume == -12 && quarter > loud / 5 && quarter < loud / 3 && peak_at(p, 110) > loud * 9 / 10,
          "volume: -12 dB: %d, %d (%d at full), the next %d", it.volume, quarter, loud, peak_at(p, 110));
    CHECK(re_project_split(p, 30) == 1, "volume: split");
    re_project_item(p, 1, &it);
    CHECK(it.volume == -12 && peak_at(p, 50) < loud / 3, "volume: split: %d, %d", it.volume, peak_at(p, 50));
    re_project_undo(p);
    CHECK(re_project_volume(p, 1, -12) == 1 && re_project_dissolve(p, 0, 20, 1) == 1, "volume: dissolve");
    CHECK(peak_at(p, 65) < loud / 3 + 200 && peak_at(p, 65) > loud / 6, "volume: in the dissolve: %d", peak_at(p, 65));
    CHECK(re_project_undo(p) && re_project_undo(p) && re_project_volume(p, 0, 0) == 1 && peak_at(p, 40) > loud * 9 / 10,
          "volume: back to 0 dB: %d", peak_at(p, 40));
    re_project_volume(p, 0, 6);
    CHECK(re_project_save(p, out) == 0, "volume: not saved");
    re_project_close(p);
    p = re_project_open(out, 1);
    re_project_item(p, 0, &it);
    CHECK(p && it.volume == 6 && peak_at(p, 40) > loud * 3 / 2, "volume: opened: %d, %d", it.volume, peak_at(p, 40));
    re_project_close(p);

    /* without sound: nothing to mute */
    p = re_project_new();
    re_project_add(p, silent, -1);
    re_project_item(p, 0, &it);
    CHECK(!it.sound && re_project_mute(p, 0, 1) == 0 && re_project_sound_fade(p, 0, 0, 10) == 0,
          "sound: a silent clip: sound %d", it.sound);

    /* a video on V2: muted; faded with its picture, and alone */
    re_overlay o;
    CHECK(re_project_overlay_add(p, snd, 0, 0) == 0, "sound: the video on V2");
    re_project_overlay(p, 0, &o);
    CHECK(o.sound && !o.muted && peak_at(p, 40) > 1000, "sound: V2's video heard: %d %d, %d", o.sound, o.muted,
          peak_at(p, 40));
    CHECK(re_project_overlay_volume(p, 0, -12) == 1, "sound: V2's video's level");
    re_project_overlay(p, 0, &o);
    CHECK(o.volume == -12 && peak_at(p, 40) < loud / 3, "sound: V2's video at -12 dB: %d, %d", o.volume,
          peak_at(p, 40));
    re_project_overlay_volume(p, 0, 0);
    CHECK(re_project_overlay_mute(p, 0, 1) == 1, "sound: V2's video muted");
    re_project_overlay(p, 0, &o);
    CHECK(o.muted && peak_at(p, 40) < 30, "sound: V2's video muted: %d, %d", o.muted, peak_at(p, 40));
    re_project_overlay_mute(p, 0, 0);
    CHECK(re_project_overlay_fade(p, 0, 0, 25) == 1, "sound: V2's video faded in");
    re_project_overlay(p, 0, &o);
    CHECK(o.fade_in == 25 && o.sound_fade_in == 25 && peak_at(p, 0) < loud / 50, "sound: V2's fade in: %d, %d, %d",
          o.fade_in, o.sound_fade_in, peak_at(p, 0));
    CHECK(re_project_overlay_sound_fade(p, 0, 0, 0) == 1, "sound: V2's sound fade taken away");
    re_project_overlay(p, 0, &o);
    CHECK(o.fade_in == 25 && o.sound_fade_in == 0 && peak_at(p, 0) > 500, "sound: V2's sound fade gone: %d, %d, %d",
          o.fade_in, o.sound_fade_in, peak_at(p, 0));
    CHECK(re_project_overlay_sound_fade(p, 0, 1, 25) == 1 && re_project_overlay_span(p, 0, 0, 60) == 1,
          "sound: V2's fade out, its end trimmed");
    re_project_overlay(p, 0, &o);
    CHECK(o.length == 60 && o.sound_fade_out == 25 && o.fade_out == 0 && peak_at(p, 52) < loud / 10
              && peak_at(p, 30) > 1000,
          "sound: V2's fade out alone, kept at its end: %d long, %d, %d, %d at 52, %d at 30", o.length,
          o.sound_fade_out, o.fade_out, peak_at(p, 52), peak_at(p, 30));
    re_project_close(p);
    (void) it2;
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

/* ---- titles: text over the video (blocks for letters here: no Font
   Manager); its picture made by style; put on V2 at the lower third, its
   own size; changed (made again, its box with it); placed; kept through
   undo, saving and opening */
static int bright(int x0, int y0, int x1, int y1) /* (pixels of the shown picture brighter than 200) */
{
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            n += shown_luma[y * 320 + x] > 200;
    return n;
}

static void titles(const char *a, const char *out)
{
    /* the pictures */
    re_title t;
    re_title_default(&t);
    snprintf(t.text, sizeof(t.text), "Hello\nWorld!");
    t.style = RE_TITLE_PLAIN;
    t.size = 10;
    uint8_t *rgba = NULL;
    int w = 0, h = 0, w2 = 0, h2 = 0;
    CHECK(!re_title_render(&t, 200, &rgba, &w, &h) && rgba && w > 6 * 12 && h >= 2 * 25,
          "titles: rendered: %dx%d", w, h);
    int lit = 0;
    for (int i = 0; i < w * h; i++)
        lit += rgba[i * 4 + 3] == 255 && rgba[i * 4] == 255;
    CHECK(lit > 100 && rgba[3] == 0, "titles: plain: %d white pixels, the corner's alpha %d", lit, rgba[3]);
    free(rgba);
    t.style = RE_TITLE_BOX;
    CHECK(!re_title_render(&t, 200, &rgba, &w2, &h2) && w2 > w && h2 > h && rgba[3] == 160 && rgba[0] == 0,
          "titles: a box: %dx%d, its corner %d,%d", w2, h2, rgba ? rgba[0] : -1, rgba ? rgba[3] : -1);
    free(rgba);
    t.style = RE_TITLE_OUTLINE;
    CHECK(!re_title_render(&t, 200, &rgba, &w2, &h2), "titles: an outline");
    int dark = 0;
    for (int i = 0; i < w2 * h2; i++)
        dark += rgba[i * 4 + 3] == 255 && rgba[i * 4] == 0;
    CHECK(dark > 50, "titles: an outline: %d black pixels", dark);
    free(rgba);
    t.style = RE_TITLE_SHADOW;
    t.align = RE_TITLE_LEFT;
    CHECK(!re_title_render(&t, 200, &rgba, &w2, &h2) && rgba[(2 * w2 + 2) * 4 + 3] == 0, "titles: a shadow");
    dark = 0;
    for (int i = 0; i < w2 * h2; i++)
        dark += rgba[i * 4 + 3] > 100 && rgba[i * 4] == 0;
    CHECK(dark > 20, "titles: a shadow: %d dark pixels", dark);
    free(rgba);

    /* on V2 */
    re_engine_io io = {show, NULL, clk, NULL, 48000, 2, NULL};
    re_project *p = re_project_new();
    re_project_add(p, a, -1);
    re_engine *e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    re_title_default(&t);
    snprintf(t.text, sizeof(t.text), "Hello\nWorld");
    t.style = RE_TITLE_PLAIN;
    CHECK(re_project_title_add(p, &t, 10, 0) == 0, "titles: not added");
    re_overlay o;
    re_project_overlay(p, 0, &o);
    CHECK(o.title && !o.video && !o.sound && o.start == 10 && o.length == 65 && o.place == RE_PLACE_LOWER_THIRD
              && !strcmp(o.name, "Hello"),
          "titles: added: title %d video %d at %d for %d, place %d, \"%s\"", o.title, o.video, o.start, o.length,
          o.place, o.name);
    re_title g;
    CHECK(!re_project_title(p, 0, &g) && !strcmp(g.text, "Hello\nWorld") && g.size == t.size
              && !strcmp(g.font, t.font) && g.colour == 0xFFFFFF && g.style == RE_TITLE_PLAIN,
          "titles: read back: \"%s\" %s %d%% %06x style %d", g.text, g.font, g.size, g.colour, g.style);
    re_engine_refresh(e);
    luma_xy(e, 20, 0, 0);
    int bottom = bright(0, 120, 320, 180), top = bright(0, 0, 320, 120), left = bright(0, 120, 100, 180);
    int centre = bright(100, 120, 220, 180);
    CHECK(bottom > 100 && top == 0 && centre > left, "titles: shown at the lower third: %d bright below, %d above, "
          "%d centre, %d left", bottom, top, centre, left);
    luma_xy(e, 5, 0, 0);
    CHECK(bright(0, 0, 320, 180) == 0, "titles: not before it: %d", bright(0, 0, 320, 180));

    /* changed: bigger; one undo */
    t.size = 16;
    CHECK(re_project_title_set(p, 0, &t) == 1 && re_project_title_set(p, 0, &t) == 0, "titles: made bigger (once)");
    re_engine_refresh(e);
    luma_xy(e, 20, 0, 0);
    int bigger = bright(0, 0, 320, 180);
    CHECK(bigger > bottom * 2, "titles: bigger: %d bright (%d before)", bigger, bottom);
    snprintf(t.text, sizeof(t.text), "Bye");
    re_project_title_set(p, 0, &t);
    re_project_overlay(p, 0, &o);
    CHECK(!strcmp(o.name, "Bye"), "titles: renamed: \"%s\"", o.name);
    CHECK(re_project_undo(p) && !re_project_title(p, 0, &g) && !strcmp(g.text, "Hello\nWorld") && g.size == 16,
          "titles: undone: \"%s\" %d", g.text, g.size);

    /* placed top left */
    CHECK(re_project_overlay_set(p, 0, RE_PLACE_TOP_LEFT, -1, -1) == 1, "titles: placed");
    re_engine_close(e);
    e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    luma_xy(e, 20, 0, 0);
    CHECK(bright(0, 0, 160, 90) > 100 && bright(160, 90, 320, 180) == 0, "titles: top left: %d there, %d bottom right",
          bright(0, 0, 160, 90), bright(160, 90, 320, 180));

    /* saved, opened */
    CHECK(re_project_save(p, out) == 0, "titles: not saved");
    re_engine_close(e);
    re_project_close(p);
    CHECK(count(out, "qtext") >= 1 && count(out, "Hello") >= 1, "titles: saved as qtext");
    p = re_project_open(out, 1);
    CHECK(p && !re_project_overlay(p, 0, &o) && o.title && !re_project_title(p, 0, &g) && g.size == 16
              && !strcmp(g.text, "Hello\nWorld"),
          "titles: opened: title %d, \"%s\" %d", o.title, g.text, g.size);
    e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    luma_xy(e, 20, 0, 0);
    CHECK(bright(0, 0, 160, 90) > 100, "titles: opened, shown: %d", bright(0, 0, 160, 90));
    re_engine_close(e);
    re_project_close(p);
}

/* ---- export: a project (a clip faded in, a title over it, a sound clip
   after it) made into an MP4 at a smaller size: every frame, the picture's
   size, the length, sound; told each frame; stopped part way (no file
   left); bit rates by quality */
static int counted, stop_at;
static void count_progress(void *ctx, int done, int total)
{
    (void) ctx;
    (void) total;
    counted = done;
}
static int stop_when(void *ctx)
{
    (void) ctx;
    return stop_at > 0 && counted >= stop_at;
}

static void export(const char *a, const char *snd, const char *proj, const char *out)
{
    re_project *p = re_project_new();
    re_project_add(p, a, -1);
    re_project_add(p, snd, -1);
    re_project_fade(p, 0, 0, 10);
    re_title t;
    re_title_default(&t);
    snprintf(t.text, sizeof(t.text), "Export");
    re_project_title_add(p, &t, 5, 20);
    int len = re_project_length(p);
    CHECK(re_project_save(p, proj) == 0, "export: the project not saved");
    re_project_close(p);
    re_export_settings s = {90, RE_EXPORT_NORMAL, 0, "mpeg4"};
    char err[200];
    counted = 0;
    stop_at = 0;
    int r = re_export(proj, out, &s, count_progress, stop_when, NULL, err, sizeof(err));
    CHECK(r == 0 && counted == len, "export: %d (%s), told %d of %d frames", r, err, counted, len);
    AVFormatContext *fc = NULL;
    int w = 0, h = 0, audio = 0;
    double secs = 0;
    if (avformat_open_input(&fc, out, NULL, NULL) == 0 && avformat_find_stream_info(fc, NULL) >= 0) {
        for (unsigned i = 0; i < fc->nb_streams; i++) {
            AVCodecParameters *cp = fc->streams[i]->codecpar;
            if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
                w = cp->width;
                h = cp->height;
            } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO && cp->codec_id == AV_CODEC_ID_AAC) {
                audio = 1;
            }
        }
        secs = fc->duration / (double) AV_TIME_BASE;
    }
    if (fc)
        avformat_close_input(&fc);
    CHECK(w == 160 && h == 90 && audio && secs > (len - 2) / 25.0 && secs < (len + 3) / 25.0,
          "export: %dx%d, sound %d, %.2f s (%d frames)", w, h, audio, secs, len);
    /* stopped part way */
    counted = 0;
    stop_at = 20;
    r = re_export(proj, out, &s, count_progress, stop_when, NULL, err, sizeof(err));
    FILE *f = fopen(out, "rb");
    CHECK(r == 1 && counted < len && !f, "export: stopped: %d, at %d, file left %d", r, counted, f != NULL);
    if (f)
        fclose(f);
    /* not a project */
    r = re_export("no-such.mlt", out, &s, NULL, NULL, NULL, err, sizeof(err));
    CHECK(r == -1 && err[0], "export: no project: %d \"%s\"", r, err);
    CHECK(re_export_bitrate(1920, 1080, 25, RE_EXPORT_NORMAL) == 5200000
              && re_export_bitrate(640, 360, 25, RE_EXPORT_SMALL) == 400000
              && re_export_bitrate(160, 90, 25, RE_EXPORT_HIGH) == 300000,
          "export: bit rates %d %d %d", re_export_bitrate(1920, 1080, 25, RE_EXPORT_NORMAL),
          re_export_bitrate(640, 360, 25, RE_EXPORT_SMALL), re_export_bitrate(160, 90, 25, RE_EXPORT_HIGH));
}

/* ---- audio tracks: tones put on them stack (a sound where one plays goes
   on the next track; where there's room, on the first); mixed with V1's;
   muted, a level, faded; moved; removed (the empty last track with it);
   a video's sound alone (its pictures hidden); a picture refused; undo;
   saved and opened */
static void audio_tracks(const char *a, const char *snd, const char *pic, const char *out)
{
    re_engine_io io = {show, NULL, clk, NULL, 48000, 2, NULL};
    re_project *p = re_project_new();
    re_project_add(p, a, -1);
    re_project_add(p, a, -1); /* (150 frames, silent) */
    int k = -1, i = -1;
    CHECK(re_project_audio_add(p, "tone880.wav", 10, 0, &k, &i) == 0 && k == 0 && i == 0
              && re_project_audio_tracks(p) == 1,
          "audio: the first: track %d, %d; %d tracks", k, i, re_project_audio_tracks(p));
    re_audio s;
    re_project_audio(p, 0, 0, &s);
    CHECK(s.start == 10 && s.length == 50 && s.source_length == 50 && !strcmp(s.name, "tone880.wav"),
          "audio: %d for %d of %d, \"%s\"", s.start, s.length, s.source_length, s.name);
    int one = peak_at(p, 20);
    CHECK(one > 1000 && peak_at(p, 5) < 30, "audio: heard: %d at 20, %d before it", one, peak_at(p, 5));
    /* stacked: where it plays, the next track; after it, the first again */
    CHECK(re_project_audio_add(p, "tone1320.wav", 20, 0, &k, &i) == 0 && k == 1 && re_project_audio_tracks(p) == 2,
          "audio: stacked: track %d, %d tracks", k, re_project_audio_tracks(p));
    CHECK(re_project_audio_add(p, "tone880.wav", 70, 0, &k, &i) == 0 && k == 0 && i == 1,
          "audio: after it, on the first: track %d, %d", k, i);
    int both = peak_at(p, 30);
    CHECK(both > one * 3 / 2, "audio: two at once: %d (one %d)", both, one);
    /* muted, a level, a fade */
    CHECK(re_project_audio_mute(p, 1, 0, 1) == 1 && abs(peak_at(p, 30) - one) < one / 10,
          "audio: the second muted: %d (one %d)", peak_at(p, 30), one);
    CHECK(re_project_audio_volume(p, 0, 0, -12) == 1 && peak_at(p, 20) < one / 3 && peak_at(p, 20) > one / 5,
          "audio: -12 dB: %d (%d)", peak_at(p, 20), one);
    CHECK(re_project_audio_fade(p, 0, 0, 0, 10) == 1 && peak_at(p, 10) < 30, "audio: faded in: %d", peak_at(p, 10));
    re_project_audio(p, 0, 0, &s);
    CHECK(s.volume == -12 && s.fade_in == 10, "audio: read back: %d dB, fade %d", s.volume, s.fade_in);
    /* moved, its start trimmed (its in with it) */
    CHECK(re_project_audio_span(p, 1, 0, 100, 50) == 1, "audio: moved");
    re_project_audio(p, 1, 0, &s);
    CHECK(s.start == 100 && s.length == 50, "audio: moved: %d for %d", s.start, s.length);
    CHECK(re_project_audio_span(p, 1, 0, 110, 40) == 1, "audio: start trimmed");
    re_project_audio(p, 1, 0, &s);
    CHECK(s.start == 110 && s.in == 10 && s.length == 40, "audio: trimmed: %d, in %d, %d", s.start, s.in, s.length);
    /* removed: the empty last track goes; undone */
    CHECK(re_project_audio_remove(p, 1, 0) == 1 && re_project_audio_tracks(p) == 1, "audio: removed: %d tracks",
          re_project_audio_tracks(p));
    CHECK(re_project_undo(p) && re_project_audio_tracks(p) == 2 && re_project_audio_count(p, 1) == 1,
          "audio: undone: %d tracks", re_project_audio_tracks(p));
    /* a video's sound: its pictures hidden; a picture refused */
    re_engine *e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    int red = luma_at(e, 130);
    CHECK(re_project_audio_add(p, snd, 120, 0, &k, &i) == 0 && peak_at(p, 125) > 1000, "audio: a video's sound: %d",
          peak_at(p, 125));
    re_engine_refresh(e);
    CHECK(abs(luma_at(e, 130) - red) <= 4, "audio: its pictures hidden: luma %d (red %d)", luma_at(e, 130), red);
    re_engine_close(e);
    CHECK(re_project_audio_add(p, pic, 0, 0, &k, &i) == RE_SOUND_NO_SOUND, "audio: a picture not refused");
    /* saved and opened: the tracks, their sounds and mixes */
    int before = peak_at(p, 30);
    CHECK(re_project_save(p, out) == 0, "audio: not saved");
    re_project_close(p);
    p = re_project_open(out, 1);
    CHECK(p && re_project_audio_tracks(p) == 2 && re_project_audio_count(p, 0) == 3 && abs(peak_at(p, 30) - before) < 50,
          "audio: opened: %d tracks, %d on A1, %d at 30 (%d)", p ? re_project_audio_tracks(p) : -1,
          p ? re_project_audio_count(p, 0) : -1, p ? peak_at(p, 30) : -1, before);
    re_project_close(p);
}

/* ---- three-point editing: a source's marked frames inserted (what's
   there split, what follows moving on) and overwritten (what's there
   taken away for its length), past the end, never into a dissolve; the
   clips list: added once, removed, saved and opened */
static void source_editing(const char *a, const char *b, const char *numbered, const char *out)
{
    re_engine_io io = {show, NULL, clk, NULL, 48000, 2, NULL};
    re_project *p = re_project_new();
    re_project_add(p, a, -1);
    re_project_add(p, b, -1);
    re_engine *e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    int red = luma_at(e, 50), blue = luma_at(e, 100);
    CHECK(re_project_insert(p, numbered, 10, 19, 30) == 1 && !strcmp(layout(p), "c30 c10 c45 c75")
              && re_project_length(p) == 160,
          "source: inserted: %s, %d", layout(p), re_project_length(p));
    re_engine_close(e);
    e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    CHECK(abs(luma_at(e, 30) - (16 + 2 * 10)) <= 3 && abs(luma_at(e, 39) - (16 + 2 * 19)) <= 3
              && abs(luma_at(e, 40) - red) <= 3 && abs(luma_at(e, 29) - red) <= 3,
          "source: inserted frames: %d %d (frames 10, 19), %d %d either side (red %d)", luma_at(e, 30),
          luma_at(e, 39), luma_at(e, 29), luma_at(e, 40), red);
    CHECK(re_project_undo(p) && !strcmp(layout(p), "c75 c75"), "source: insert undone: %s", layout(p));
    CHECK(re_project_overwrite(p, numbered, 0, 9, 70) == 1 && !strcmp(layout(p), "c70 c10 c70")
              && re_project_length(p) == 150,
          "source: overwritten: %s, %d", layout(p), re_project_length(p));
    re_engine_close(e);
    e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    CHECK(abs(luma_at(e, 70) - 16) <= 3 && abs(luma_at(e, 79) - (16 + 18)) <= 3 && abs(luma_at(e, 80) - blue) <= 3
              && abs(luma_at(e, 69) - red) <= 3,
          "source: overwritten frames: %d %d, %d after (blue %d), %d before (red %d)", luma_at(e, 70),
          luma_at(e, 79), luma_at(e, 80), blue, luma_at(e, 69), red);
    re_engine_close(e);
    CHECK(re_project_overwrite(p, numbered, 0, 9, 160) == 4 && !strcmp(layout(p), "c70 c10 c70 g10 c10"),
          "source: past the end: %s", layout(p));
    CHECK(re_project_insert(p, numbered, 0, 4, 0) == 0 && re_project_length(p) == 175, "source: at the start: %s",
          layout(p));
    CHECK(re_project_insert(p, numbered, 5, 4, 10) == -1 && re_project_overwrite(p, numbered, 0, 500, 10) == -1,
          "source: in after out, out past the file");
    re_project_close(p);
    p = re_project_new();
    re_project_add(p, a, -1);
    re_project_add(p, b, -1);
    re_project_dissolve(p, 0, 20, 1);
    CHECK(re_project_insert(p, numbered, 0, 9, 70) == -1 && re_project_overwrite(p, numbered, 0, 9, 60) == -1,
          "source: into a dissolve: %s", layout(p));
    /* the clips list */
    re_project_save(p, out);
    CHECK(!re_project_modified(p) && re_project_clip_add(p, a) == 0 && re_project_clip_add(p, b) == 1
              && re_project_clip_add(p, a) == 0 && re_project_clips(p) == 2 && re_project_modified(p),
          "clips: added: %d, modified %d", re_project_clips(p), re_project_modified(p));
    re_project_clip_add(p, numbered);
    CHECK(re_project_clip_remove(p, 1) == 1 && re_project_clips(p) == 2 && !strcmp(re_project_clip(p, 1), numbered),
          "clips: removed: %d", re_project_clips(p));
    CHECK(re_project_save(p, out) == 0, "clips: not saved");
    re_project_close(p);
    p = re_project_open(out, 1);
    CHECK(p && re_project_clips(p) == 2 && !strcmp(re_project_clip(p, 0), a) && !strcmp(re_project_clip(p, 1), numbered),
          "clips: opened: %d", p ? re_project_clips(p) : -1);
    re_project_close(p);
}

/* ---- rolls: a cut moved, the clips either side changed, the length kept -- */
static void rolls(const char *numbered, const char *b)
{
    re_engine_io io = {show, NULL, clk, NULL, 48000, 2, NULL};
    re_project *p = re_project_new();
    re_item x, y;
    re_project_add(p, numbered, -1);
    re_project_split(p, 30);
    re_project_set_in_out(p, 1, 40, 74); /* numbered 0-29, then 40-74 */
    CHECK(!strcmp(layout(p), "c30 c35"), "rolls: set up: %s", layout(p));
    unsigned ch = re_project_changes(p);
    CHECK(re_project_roll(p, 0, 35) == 1 && !strcmp(layout(p), "c35 c30") && re_project_length(p) == 65,
          "rolls: to 35: %s, %d", layout(p), re_project_length(p));
    re_project_item(p, 0, &x);
    re_project_item(p, 1, &y);
    CHECK(x.out == 34 && y.in == 45 && y.start == 35 && re_project_changes(p) > ch, "rolls: out %d, in %d, at %d",
          x.out, y.in, y.start);
    re_engine *e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    CHECK(abs(luma_at(e, 35) - (16 + 2 * 45)) <= 2 && abs(luma_at(e, 34) - (16 + 2 * 34)) <= 2,
          "rolls: frames 34, 35 show %d, %d (not %d, %d)", luma_at(e, 34), luma_at(e, 35), 16 + 68, 16 + 90);
    re_engine_close(e);
    /* kept to the files, a frame each side */
    CHECK(re_project_roll(p, 0, 1000) == 1 && !strcmp(layout(p), "c64 c1"), "rolls: right to the end: %s",
          layout(p));
    CHECK(re_project_roll(p, 0, -50) == 1 && !strcmp(layout(p), "c1 c64"), "rolls: left to the start: %s",
          layout(p));
    re_project_item(p, 1, &y);
    CHECK(y.in == 11, "rolls: left to the start: the second's in %d, not 11", y.in);
    CHECK(re_project_roll(p, 0, 1) == 0 && re_project_roll(p, 1, 10) == 0 && re_project_roll(p, -1, 10) == 0,
          "rolls: nothing to do, or no cut");
    /* one undo */
    CHECK(re_project_undo(p) && !strcmp(layout(p), "c64 c1"), "rolls: undone: %s", layout(p));
    /* a drag's many steps: one undo */
    re_project_begin(p);
    re_project_roll(p, 0, 40);
    re_project_roll(p, 0, 50);
    re_project_end(p);
    CHECK(!strcmp(layout(p), "c50 c15") && re_project_undo(p) && !strcmp(layout(p), "c64 c1"),
          "rolls: a drag, undone: %s", layout(p));
    /* into a gap, out of one */
    re_project_add(p, b, -1);
    re_project_remove(p, 1, 0); /* c64 g1 c75 */
    CHECK(!strcmp(layout(p), "c64 g1 c75"), "rolls: a gap: %s", layout(p));
    CHECK(re_project_roll(p, 1, 55) == 0 && !strcmp(layout(p), "c64 g1 c75"),
          "rolls: the gap's end left (the clip after has nothing before it): %s", layout(p));
    CHECK(re_project_roll(p, 0, 60) == 1 && !strcmp(layout(p), "c60 g5 c75"), "rolls: into the gap: %s", layout(p));
    CHECK(re_project_roll(p, 1, 67) == 1 && !strcmp(layout(p), "c60 g7 c73") && (re_project_item(p, 2, &y), y.in == 2),
          "rolls: the gap's end right: %s, in %d", layout(p), y.in);
    CHECK(re_project_roll(p, 1, 60) == 1 && !strcmp(layout(p), "c60 g5 c75"),
          "rolls: the gap's end left, as far as the clip after has frames: %s", layout(p));
    re_project_roll(p, 1, 67);
    CHECK(re_project_roll(p, 0, 70) == 1 && !strcmp(layout(p), "c66 g1 c73"), "rolls: the gap keeps a frame: %s",
          layout(p));
    /* a clip at its file's end: not further */
    re_project_close(p);
    p = re_project_new();
    re_project_add(p, numbered, -1);
    re_project_add(p, b, -1);
    CHECK(re_project_roll(p, 0, 100) == 0 && !strcmp(layout(p), "c75 c75"), "rolls: past the file's end: %s",
          layout(p));
    /* not a dissolve */
    re_project_close(p);
    p = re_project_new();
    re_project_add(p, numbered, -1);
    re_project_split(p, 30);
    int mix = re_project_dissolve(p, 0, 10, 1);
    CHECK(mix == 1 && re_project_roll(p, 0, 25) == 0 && re_project_roll(p, 1, 25) == 0,
          "rolls: a dissolve rolled (%s)", layout(p));
    re_project_close(p);
}

/* ---- copy and paste --------------------------------------------------------- */
static void copy_paste(const char *a, const char *snd, const char *pic)
{
    re_project *p = re_project_new();
    re_item it;
    re_project_add(p, a, -1);
    re_project_add(p, snd, -1);
    re_project_set_in_out(p, 1, 5, 64); /* numbered-snd's 5-64 */
    re_project_fade(p, 1, 0, 10);
    re_project_sound_fade(p, 1, 1, 6);
    re_project_volume(p, 1, -6);
    re_project_mute(p, 1, 1);
    re_copy *c = re_project_copy(p, RE_COPY_CLIP, 0, 1);
    CHECK(c && re_copy_kind(c) == RE_COPY_CLIP && re_copy_length(c) == 60, "copy: a clip: %d",
          c ? re_copy_length(c) : -1);
    CHECK(!re_project_copy(p, RE_COPY_CLIP, 0, 7) && !re_project_copy(p, RE_COPY_OVERLAY, 0, 0)
              && !re_project_copy(p, RE_COPY_SOUND, 0, 0),
          "copy: something that isn't there");
    /* the original deleted: still pasted */
    re_project_remove(p, 1, 1);
    CHECK(!strcmp(layout(p), "c75"), "copy: set up: %s", layout(p));
    CHECK(re_project_paste(p, c, 30, NULL) == 1 && !strcmp(layout(p), "c30 c60 c45"), "paste: a clip at 30: %s",
          layout(p));
    re_project_item(p, 1, &it);
    CHECK(it.in == 5 && it.out == 64 && it.fade_in == 10 && it.sound_fade_in == 10 && it.fade_out == 0
              && it.sound_fade_out == 6 && it.volume == -6 && it.muted,
          "paste: the clip's in %d out %d, fades %d %d %d %d, %d dB", it.in, it.out, it.fade_in, it.sound_fade_in,
          it.fade_out, it.sound_fade_out, it.volume);
    CHECK(re_project_undo(p) && !strcmp(layout(p), "c75"), "paste: one undo: %s", layout(p));
    CHECK(re_project_undo(p) && !strcmp(layout(p), "c75 c60"), "paste: undone, then the delete before it: %s",
          layout(p));
    re_project_redo(p);
    re_project_redo(p);
    CHECK(re_project_paste(p, c, 1000, NULL) == 3 && !strcmp(layout(p), "c30 c60 c45 c60"), "paste: at the end: %s",
          layout(p));
    /* not into a dissolve: nothing done, no undo point */
    int mix = re_project_dissolve(p, 0, 10, 1);
    unsigned ch = re_project_changes(p);
    int len = re_project_length(p);
    re_item m;
    re_project_item(p, mix, &m);
    CHECK(mix >= 0 && re_project_paste(p, c, m.start + 2, NULL) < 0 && re_project_changes(p) == ch
              && re_project_length(p) == len,
          "paste: into a dissolve");
    re_copy_free(c);

    /* overlays: a picture, a title */
    int ov = re_project_overlay_add(p, pic, 0, 25);
    re_project_overlay_set(p, ov, RE_PLACE_TOP_LEFT, 30, 60);
    re_project_overlay_fade(p, ov, 1, 5);
    c = re_project_copy(p, RE_COPY_OVERLAY, 0, ov);
    int at = re_project_paste(p, c, 100, NULL);
    re_overlay o, q;
    CHECK(at == 1 && !re_project_overlay(p, 1, &o) && o.start == 100 && o.length == 25 && o.place == RE_PLACE_TOP_LEFT
              && o.size == 30 && o.opacity == 60 && o.fade_out == 5 && o.fade_in == 0,
          "paste: a picture: %d, at %d, %d long, place %d size %d opacity %d, fades %d %d", at, o.start, o.length,
          o.place, o.size, o.opacity, o.fade_in, o.fade_out);
    re_copy_free(c);
    re_title t, t2;
    re_title_default(&t);
    snprintf(t.text, sizeof(t.text), "Copied");
    int ti = re_project_title_add(p, &t, 40, 20);
    c = re_project_copy(p, RE_COPY_OVERLAY, 0, ti);
    at = re_project_paste(p, c, 150, NULL);
    snprintf(t2.text, sizeof(t2.text), "x");
    CHECK(at >= 0 && !re_project_title(p, at, &t2) && !strcmp(t2.text, "Copied"), "paste: a title: %d", at);
    snprintf(t2.text, sizeof(t2.text), "Changed");
    re_project_title_set(p, at, &t2);
    CHECK(!re_project_title(p, ti, &t) && !strcmp(t.text, "Copied") && !re_project_overlay(p, at, &q)
              && !strcmp(q.name, "Changed"),
          "paste: the copy's title changed the original's: %s", t.text);
    re_copy_free(c);

    /* sounds: stacked, trimmed */
    int k, i;
    re_project_audio_add(p, "tone880.wav", 10, 0, &k, &i);
    re_project_audio_span(p, 0, 0, 15, 45); /* (its start trimmed: in 5) */
    re_project_audio_volume(p, 0, 0, -12);
    re_project_audio_fade(p, 0, 0, 0, 7);
    c = re_project_copy(p, RE_COPY_SOUND, 0, 0);
    int tr = -1;
    at = re_project_paste(p, c, 20, &tr);
    re_audio s1;
    CHECK(at == 0 && tr == 1 && !re_project_audio(p, 1, 0, &s1) && s1.start == 20 && s1.length == 45 && s1.in == 5
              && s1.volume == -12 && s1.fade_in == 7 && !s1.muted,
          "paste: a sound: %d on A%d at %d, %d long, in %d, %d dB, fade %d", at, tr + 1, s1.start, s1.length, s1.in,
          s1.volume, s1.fade_in);
    re_copy_free(c);
    re_project_close(p);
}

/* ---- speeds: timewarp clips -------------------------------------------------- */
static void speeds(const char *numbered, const char *snd, const char *pic, const char *a, const char *out)
{
    re_engine_io io = {show, NULL, clk, NULL, 48000, 2, NULL};
    re_project *p = re_project_new();
    re_item it;
    re_project_add(p, numbered, -1);
    re_project_add(p, a, -1);
    CHECK(re_project_speed(p, 0, 200) == 1 && !strcmp(layout(p), "c38 c75"), "speed: 200%%: %s", layout(p));
    re_project_item(p, 0, &it);
    CHECK(it.speed == 200 && !strcmp(it.name, "numbered.mp4") && re_project_item_file(p, 0)
              && !strcmp(re_project_item_file(p, 0), numbered) && re_item_file_frame(&it, 10) == 20,
          "speed: 200%%: speed %d, name %s, file frame %d", it.speed, it.name, re_item_file_frame(&it, 10));
    re_engine *e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    CHECK(abs(luma_at(e, 10) - (16 + 2 * 20)) <= 3, "speed: 200%%: frame 10 shows %d, not file frame 20's %d",
          luma_at(e, 10), 16 + 40);
    re_engine_close(e);
    CHECK(re_project_undo(p) && !strcmp(layout(p), "c75 c75"), "speed: one undo: %s", layout(p));
    re_project_redo(p);
    CHECK(re_project_speed(p, 0, 50) == 1 && !strcmp(layout(p), "c150 c75"), "speed: 50%%: %s", layout(p));
    e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    CHECK(abs(luma_at(e, 100) - (16 + 2 * 50)) <= 3, "speed: 50%%: frame 100 shows %d, not file frame 50's %d",
          luma_at(e, 100), 16 + 100);
    re_engine_close(e);
    CHECK(re_project_speed(p, 0, -100) == 1 && !strcmp(layout(p), "c75 c75"), "speed: reversed: %s", layout(p));
    re_project_item(p, 0, &it);
    e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    CHECK(it.speed == -100 && re_item_file_frame(&it, 0) == 74 && abs(luma_at(e, 0) - (16 + 2 * 74)) <= 3
              && abs(luma_at(e, 70) - (16 + 2 * 4)) <= 3,
          "speed: reversed: frames 0, 70 show %d, %d", luma_at(e, 0), luma_at(e, 70));
    re_engine_close(e);
    /* a part of the file, kept through speeds */
    CHECK(re_project_speed(p, 0, 100) == 1 && re_project_set_in_out(p, 0, 20, 59) && !strcmp(layout(p), "c40 c75"),
          "speed: trimmed: %s", layout(p));
    re_project_speed(p, 0, -100);
    re_project_item(p, 0, &it);
    e = re_engine_open_producer(re_project_profile(p), re_project_producer(p), 1, &io);
    CHECK(it.length == 40 && re_item_file_frame(&it, it.in) == 59 && abs(luma_at(e, 0) - (16 + 2 * 59)) <= 3
              && abs(luma_at(e, 39) - (16 + 2 * 20)) <= 3,
          "speed: trimmed, reversed: %d long, file frame %d; frames 0, 39 show %d, %d", it.length,
          re_item_file_frame(&it, it.in), luma_at(e, 0), luma_at(e, 39));
    re_engine_close(e);
    re_project_speed(p, 0, 200);
    re_project_speed(p, 0, 100);
    re_project_item(p, 0, &it);
    CHECK(it.in == 20 && it.out == 59 && it.speed == 100, "speed: there and back: in %d out %d, %d%%", it.in, it.out,
          it.speed);
    /* saved, opened */
    re_project_speed(p, 0, 200);
    CHECK(!re_project_save(p, out), "speed: not saved");
    re_project *q = re_project_open(out, 1);
    CHECK(q && !re_project_item(q, 0, &it) && it.speed == 200 && it.length == 20 && it.in == 10,
          "speed: opened: %d%%, %d long, in %d", q ? it.speed : -1, q ? it.length : -1, q ? it.in : -1);
    re_project_close(q);
    /* refused: no change, out of range, beside a dissolve */
    CHECK(re_project_speed(p, 0, 200) == 0 && re_project_speed(p, 0, 0) == 0 && re_project_speed(p, 0, 5) == 0
              && re_project_speed(p, 0, 3000) == 0 && re_project_speed(p, 9, 200) == 0,
          "speed: a change that isn't one");
    int mix = re_project_dissolve(p, 0, 5, 1);
    CHECK(mix == 1 && re_project_speed(p, 0, 100) == 0 && re_project_speed(p, 2, 50) == 0, "speed: beside a dissolve");
    re_project_close(p);
    /* its sound's level and fades kept; a picture refused */
    p = re_project_new();
    re_project_add(p, snd, -1);
    re_project_volume(p, 0, -6);
    re_project_fade(p, 0, 0, 10);
    char q2[300];
    snprintf(q2, sizeof(q2), "qimage:%s", pic);
    re_project_add(p, q2, -1);
    CHECK(re_project_speed(p, 0, 200) == 1 && !re_project_item(p, 0, &it) && it.volume == -6 && it.fade_in == 10
              && it.sound,
          "speed: the sound's level %d, fade %d", it.volume, it.fade_in);
    CHECK(peak_48k(p, 20) > 1000, "speed: 200%%: the sound at 48 kHz: %d", peak_48k(p, 20));
    re_project_speed(p, 0, -50);
    CHECK(peak_48k(p, 40) > 1000, "speed: -50%%: the sound at 48 kHz: %d", peak_48k(p, 40));
    re_project_save(p, out);
    re_project *q3 = re_project_open(out, 1);
    CHECK(q3 && peak_48k(q3, 40) > 1000, "speed: opened: the sound at 48 kHz: %d", q3 ? peak_48k(q3, 40) : -2);
    re_project_close(q3);
    re_project_undo(p);
    CHECK(peak_48k(p, 20) > 1000, "speed: undone: the sound at 48 kHz: %d", peak_48k(p, 20));
    CHECK(re_project_speed(p, 1, 200) == 0, "speed: a picture sped up");
    re_project_close(p);
}

/* ---- media: thumbnails and waveforms, made a step at a time ---------------- */

/* Steps until there's nothing to do: how many (or -1, too many) */
static int media_run(re_media *m)
{
    for (int n = 0; n < 200000; n++)
        if (re_media_step(m) < 0)
            return n;
    return -1;
}

/* numbered's frame f is grey 16 + 2f (video range): as RGB, full range */
static int grey_of(int f)
{
    return (2 * f) * 255 / 219;
}

static void media(const char *numbered, const char *snd, const char *pic, const char *a)
{
    re_media *m = re_media_new(25, 64, 36);
    unsigned s1 = 0, s2 = 0, s3 = 0;
    CHECK(!re_media_thumb(m, numbered, 37, &s1) && re_media_pending(m) == 1, "media: a thumbnail made at once");
    int steps = media_run(m);
    const unsigned char *t = re_media_thumb(m, numbered, 37, &s1);
    CHECK(steps > 0 && t && re_media_pending(m) == 0, "media: no thumbnail of 37 (%d steps)", steps);
    if (t)
        CHECK(abs(t[(18 * 64 + 32) * 4] - grey_of(37)) <= 4 && abs(t[(18 * 64 + 32) * 4 + 1] - grey_of(37)) <= 4
                  && t[(18 * 64 + 32) * 4 + 3] == 255 && abs(t[(1 * 64 + 1) * 4] - grey_of(37)) <= 4,
              "media: frame 37's thumbnail %d,%d (not %d), alpha %d", t[(18 * 64 + 32) * 4], t[(18 * 64 + 32) * 4 + 1],
              grey_of(37), t[(18 * 64 + 32) * 4 + 3]);
    re_media_thumb(m, numbered, 0, NULL);
    re_media_thumb(m, numbered, 60, NULL);
    re_media_thumb(m, numbered, 500, NULL); /* (past the end: the last frame) */
    media_run(m);
    const unsigned char *t0 = re_media_thumb(m, numbered, 0, &s2), *t60 = re_media_thumb(m, numbered, 60, &s3);
    const unsigned char *tend = re_media_thumb(m, numbered, 500, NULL);
    CHECK(t0 && t60 && tend && s2 != s1 && s3 != s2, "media: thumbnails 0, 60, past the end not made");
    if (t0 && t60 && tend)
        CHECK(t0[(18 * 64 + 32) * 4] <= 4 && abs(t60[(18 * 64 + 32) * 4] - grey_of(60)) <= 4
                  && abs(tend[(18 * 64 + 32) * 4] - grey_of(74)) <= 4,
              "media: frames 0, 60, the end: %d, %d (%d), %d (%d)", t0[(18 * 64 + 32) * 4], t60[(18 * 64 + 32) * 4],
              grey_of(60), tend[(18 * 64 + 32) * 4], grey_of(74));
    unsigned sn = 0;
    CHECK(re_media_thumb_near(m, numbered, 40, &sn) == t && sn == s1 && !re_media_thumb_near(m, a, 40, NULL),
          "media: the nearest thumbnail to 40 isn't 37's");
    /* a file that isn't one: not made, not tried again */
    re_media_thumb(m, "no-such-file.mp4", 0, NULL);
    media_run(m);
    CHECK(!re_media_thumb(m, "no-such-file.mp4", 0, NULL) && !re_media_thumb(m, "no-such-file.mp4", 5, NULL)
              && re_media_pending(m) == 0,
          "media: a missing file's thumbnail, or asked for again (%d waiting)", re_media_pending(m));
    /* a picture, in a box of another shape: bars */
    re_media *sq = re_media_new(25, 40, 40);
    re_media_thumb(sq, pic, 0, NULL);
    media_run(sq);
    const unsigned char *g = re_media_thumb(sq, pic, 0, NULL);
    CHECK(g && g[(20 * 40 + 20) * 4 + 1] > 100 && g[(20 * 40 + 20) * 4] < 60 && g[(2 * 40 + 20) * 4 + 1] < 10,
          "media: the picture's thumbnail (green, black bars above): %d,%d / %d", g ? g[(20 * 40 + 20) * 4] : -1,
          g ? g[(20 * 40 + 20) * 4 + 1] : -1, g ? g[(2 * 40 + 20) * 4 + 1] : -1);
    re_media_free(sq);

    /* peaks: a second of tone (an eighth of full scale: 32), a second of silence */
    int n = 0, done = 0;
    CHECK(!re_media_peaks(m, "half.wav", &n, &done) && !done, "media: peaks made at once");
    media_run(m);
    const unsigned char *pk = re_media_peaks(m, "half.wav", &n, &done);
    CHECK(pk && done && n >= 49 && n <= 51, "media: half.wav's peaks: %d (done %d)", n, done);
    if (pk && n >= 49)
        CHECK(abs(pk[5] - 32) <= 2 && abs(pk[23] - 32) <= 2 && pk[26] <= 1 && pk[45] <= 1,
              "media: half.wav's peaks %d %d %d %d", pk[5], pk[23], pk[26], pk[45]);
    re_media_peaks(m, snd, &n, &done);
    re_media_peaks(m, numbered, &n, &done);
    media_run(m);
    pk = re_media_peaks(m, snd, &n, &done);
    CHECK(pk && done && n >= 74 && n <= 76 && abs(pk[37] - 32) <= 3, "media: numbered-snd's peaks: %d, %d", n,
          pk && n > 37 ? pk[37] : -1);
    CHECK(!re_media_peaks(m, numbered, &n, &done) && done && n == 0, "media: a silent video's peaks");

    /* thumbnails before peaks: one asked for after is made first */
    re_media_peaks(m, "tone880.wav", &n, &done);
    re_media_thumb(m, numbered, 20, NULL);
    for (int k = 0; k < 400 && !re_media_thumb(m, numbered, 20, NULL); k++)
        re_media_step(m);
    re_media_peaks(m, "tone880.wav", &n, &done);
    CHECK(re_media_thumb(m, numbered, 20, NULL) && !done, "media: the thumbnail wasn't first (peaks done %d)", done);
    media_run(m);

    /* too many: the least recently asked for go */
    re_media_free(m);
    m = re_media_new(25, 64, 36);
    for (int f = 0; f < 110; f++) {
        re_media_thumb(m, numbered, f, NULL);
        media_run(m);
    }
    CHECK(!re_media_thumb(m, numbered, 3, NULL) && re_media_thumb(m, numbered, 109, NULL),
          "media: 110 asked for: the first not let go, or the last not kept");

    /* the project's files */
    re_project *p = re_project_new();
    int k, i;
    re_project_add(p, numbered, -1);
    re_project_add(p, numbered, -1);
    re_project_dissolve(p, 0, 10, 1);
    re_project_audio_add(p, "half.wav", 0, 0, &k, &i);
    CHECK(re_project_item_file(p, 0) && !strcmp(re_project_item_file(p, 0), numbered) && !re_project_item_file(p, 1)
              && re_project_audio_file(p, 0, 0) && !strcmp(re_project_audio_file(p, 0, 0), "half.wav"),
          "media: the project's files: %s, %s, %s", re_project_item_file(p, 0) ? re_project_item_file(p, 0) : "-",
          re_project_item_file(p, 1) ? re_project_item_file(p, 1) : "-",
          re_project_audio_file(p, 0, 0) ? re_project_audio_file(p, 0, 0) : "-");
    char q[300];
    snprintf(q, sizeof(q), "qimage:%s", pic); /* (a picture, as the desktop adds one) */
    int at = re_project_add(p, q, -1);
    CHECK(at >= 0 && re_project_item_file(p, at) && !strcmp(re_project_item_file(p, at), pic),
          "media: a picture's file: %s", at >= 0 && re_project_item_file(p, at) ? re_project_item_file(p, at) : "-");
    re_project_close(p);
    re_media_free(m);
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
    clip_sound(a, argv[9], out);
    stills(argv[10]);
    titles(a, out);
    export(a, argv[9], out, "export-test.mp4");
    audio_tracks(a, argv[9], argv[11], out);
    source_editing(a, b, n, out);
    media(n, argv[9], argv[12], a);
    rolls(n, b);
    copy_paste(a, argv[9], argv[11]);
    speeds(n, argv[9], argv[12], a, out);
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
