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

static int shown_y = -1, shown_pos = -1;
static void show(void *ctx, const re_picture *p)
{
    (void) ctx;
    shown_y = p->planes[0][(p->height / 2) * p->strides[0] + p->width / 2];
    shown_pos = p->position;
}
static unsigned clk(void *ctx)
{
    (void) ctx;
    return 0;
}

/* items as "c75 g10 c40": clips (c) and gaps (g) with their lengths */
static const char *layout(re_project *p)
{
    static char s[256];
    s[0] = 0;
    for (int i = 0; i < re_project_items(p); i++) {
        re_item it;
        re_project_item(p, i, &it);
        snprintf(s + strlen(s), sizeof(s) - strlen(s), "%s%c%d", i ? " " : "", it.gap ? 'g' : 'c', it.length);
    }
    return s;
}

static const char *name_of(re_project *p, int i)
{
    static re_item it;
    re_project_item(p, i, &it);
    return it.name;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: project_test a.mp4 b.mp4 numbered.mp4 out.mlt\n");
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

    printf("project_test: %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
}
