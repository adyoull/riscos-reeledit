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
static void show(void *ctx, const re_picture *p)
{
    (void) ctx;
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

int main(int argc, char **argv)
{
    if (argc != 10) {
        fprintf(stderr, "usage: project_test a.mp4 b.mp4 numbered.mp4 out.mlt numbered.m2v four3-709.mp4 big.mp4 "
                        "wide-dvd.mp4 numbered-snd.mp4\n");
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

    printf("project_test: %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
}
