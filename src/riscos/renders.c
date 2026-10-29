/* renders.c - render previews on the desktop (app.h), from src/render.c:
 * Play > Render previews (Ctrl-R) makes each costly stretch of the project
 * that has no preview into a video file, one at a time, each in a
 * TaskWindow of its own (as an export is: exportbox.c), so editing and
 * playing go on meanwhile. Play > Stop rendering stops.
 *
 * Each is made from the project as it is when it starts (saved to a scrap
 * file), at the Viewer's size, by the VideoCore's encoder when there is
 * one: !RunImage -export project file height 2 hardware in out. After it,
 * the next stretch without a preview is started, until there are none.
 *
 * The Timeline shows the stretches under its ruler: red to render, green
 * rendered, amber being rendered (as much as has been made). Playing, the
 * Viewer plays the rendered stretches from their files (viewer.c asks for
 * the playlist). The files are in <Wimp$ScrapDir>.ReelEdPrv, deleted when
 * the editor closes.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "app.h"
#include "../render.h"
#include "../proxy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MSG_TW_EGO      0x808C2
#define MSG_TW_OUTPUT   0x808C1
#define MSG_TW_MORIO    0x808C3
#define MSG_TW_MORITE   0x808C4
#define TXT_ID          0x52455056 /* ("REPV": a render's TaskWindow) */
#define Wimp_StartTask  0x400DE
#define OS_File         0x08
#define OS_SWINumberFromString 0x39
#define DIR  "<Wimp$ScrapDir>.ReelEdPrv"
#define PROJ "<Wimp$ScrapDir>.ReelEdPrj"
#define MAX_STRETCHES 256

static struct
{
    re_previews *pv;
    int on;               /* rendering: one after another until none's left */
    int running, child;   /* a TaskWindow going (its task, once it says) */
    re_stretch job;       /* what it's making */
    char file[256];       /* where (RISC OS name) */
    int done, ok;         /* frames made; it said "done" */
    char line[256];
    int line_len;
    unsigned serial;
    /* the stretches, as they were when last worked out */
    re_stretch s[MAX_STRETCHES];
    int n;
    unsigned changes;     /* (worked out again when the project changes) */
    int known;
    unsigned heard_cs;    /* when it last said anything (or started): rr_tick's watch */
} R;

static void stretches(void)
{
    if (!A.proj || (R.known && re_project_changes(A.proj) == R.changes))
        return;
    R.n = re_render_stretches(A.proj, R.s, MAX_STRETCHES);
    R.changes = re_project_changes(A.proj);
    R.known = 1;
}

int rr_running(void)
{
    return R.on;
}

/* 0 none to render; 1 rendered; 2 being rendered (the frames made: *made) */
static int state(const re_stretch *s, int *made)
{
    if (R.running && s->sig == R.job.sig && s->length == R.job.length) {
        *made = R.done;
        return 2;
    }
    return re_previews_find(R.pv, s->sig, s->length) ? 1 : 0;
}

int rr_stretches(int i, int *start, int *length, int *made)
{
    stretches();
    if (!A.proj || i < 0 || i >= R.n)
        return -1;
    *start = R.s[i].start;
    *length = R.s[i].length;
    *made = 0;
    return state(&R.s[i], made);
}

static int can_hw(void)
{
    _kernel_swi_regs r;
    r.r[1] = (intptr_t) "VCHIQ_MsgQueue";
    return !ro_swi(OS_SWINumberFromString, &r);
}

/* Another child running (children.c: one at a time): the next waits (rr_tick) */
static int blocked(void)
{
    return app_child_others(TXT_ID) || ex_busy();
}

/* The next stretch without a preview started: 1, 2 (waiting: rr_tick
   starts it), or 0 (none: done) */
static int next(void)
{
    char unix_proj[512], unix_out[512], cmd[1400];
    _kernel_swi_regs r;
    if (blocked())
        return 2;
    stretches();
    int i;
    for (i = 0; i < R.n; i++)
        if (!re_previews_find(R.pv, R.s[i].sig, R.s[i].length))
            break;
    if (i == R.n || !A.proj || !A.e)
        return 0;
    R.job = R.s[i];
    const char *uproj = app_mlt_name(PROJ, unix_proj, sizeof(unix_proj));
    if (re_project_save_copy(A.proj, uproj)) {
        app_report("Previews couldn't be rendered: the project couldn't be saved to a scrap file.");
        return 0;
    }
    r.r[0] = 8; /* OS_File 8: the directory (there already: no harm) */
    r.r[1] = (intptr_t) DIR;
    r.r[4] = 0;
    ro_swi(OS_File, &r);
    snprintf(R.file, sizeof(R.file), DIR ".R%u", ++R.serial);
    const char *uout = app_mlt_name(R.file, unix_out, sizeof(unix_out));
    char nums[4][16];
    snprintf(nums[0], 16, "%d", re_engine_height(A.e));
    snprintf(nums[1], 16, "%d", can_hw());
    snprintf(nums[2], 16, "%d", R.job.start);
    snprintf(nums[3], 16, "%d", R.job.start + R.job.length - 1);
    const char *args[8] = {uproj, uout, nums[0], "2", nums[1], nums[2], nums[3], "1"}; /* (1: scaled as previews are) */
    _kernel_oserror *e = app_child_start("export", args, 8, "ReelEdit render", TXT_ID, 2, cmd, sizeof(cmd));
    app_log("render: frames %d-%d: %s%s%s", R.job.start, R.job.start + R.job.length - 1, cmd, e ? ": " : "",
            e ? e->errmess : "");
    if (e) {
        app_report("Previews couldn't be rendered (TaskWindow).");
        return 0;
    }
    R.running = 1;
    R.child = 0;
    R.done = R.ok = 0;
    R.line_len = 0;
    R.heard_cs = ro_now_cs();
    tl_refresh();
    return 1;
}

void rr_start(void)
{
    if (!A.proj || !A.e)
        return;
    if (!R.pv && !(R.pv = re_previews_new()))
        return;
    if (R.on)
        return;
    if (ex_busy()) {
        app_report("Previews can't be rendered while a video is being exported.");
        return;
    }
    R.on = 1;
    if (!next()) {
        R.on = 0;
        stretches();
        app_report(R.n ? "The previews are all rendered." :
                         "There's nothing to render: only overlays, titles, dissolves, fades and clips at another "
                         "speed are.");
    }
}

static void kill_child(void)
{
    _kernel_swi_regs r;
    int m[6];
    if (R.running && R.child) {
        memset(m, 0, sizeof(m));
        m[0] = 20; m[4] = MSG_TW_MORITE;
        r.r[0] = 17; r.r[1] = (intptr_t) m; r.r[2] = R.child;
        ro_swi(Wimp_SendMessage, &r);
    }
}

void rr_stop(void)
{
    if (!R.on)
        return;
    R.on = 0;
    kill_child(); /* (its file goes when it has ended) */
    app_log("render: stopped");
    tl_refresh();
}

/* It has ended: the file kept (as the stretch's preview), or deleted */
static void ended(void)
{
    char unix_out[512];
    R.running = 0;
    R.child = 0;
    app_child_ended(TXT_ID);
    const char *uout = app_mlt_name(R.file, unix_out, sizeof(unix_out));
    if (R.ok && R.pv && !re_previews_add(R.pv, R.job.sig, R.job.length, uout)) {
        app_log("render: frames %d-%d rendered", R.job.start, R.job.start + R.job.length - 1);
    } else {
        remove(uout);
        if (R.on && !R.ok) {
            app_report("A preview couldn't be rendered: rendering stopped.");
            R.on = 0;
        }
    }
    if (R.on && !next())
        R.on = 0;
    tl_refresh();
}

static void heard(const char *line)
{
    int d, t;
    if (sscanf(line, "progress %d %d", &d, &t) == 2) {
        R.done = d;
        tl_refresh();
    } else if (!strcmp(line, "done")) {
        R.ok = 1;
    } else if (!strncmp(line, "failed", 6)) {
        app_log("render: %s", line);
    }
}

/* Nothing said for 90 s (it says its progress four times a second): stuck,
   so stopped (the stretch left red, rendering stopped, said) */
#define STUCK_CS 9000

int rr_watching(void)
{
    return R.running || R.on;
}

void rr_tick(void)
{
    if (R.on && !R.running && !blocked()) { /* (waiting for another child: it has ended) */
        if (!next())
            R.on = 0;
        tl_refresh();
    }
    if (!R.running || ro_now_cs() - R.heard_cs < STUCK_CS)
        return;
    app_log("render: frames %d-%d: nothing from it for %d s: stopped", R.job.start, R.job.start + R.job.length - 1,
            STUCK_CS / 100);
    R.heard_cs = ro_now_cs();
    if (R.child) {
        kill_child(); /* (its TaskWindow_Morio ends it: not ok, so rendering stops and says so) */
    } else {
        R.ok = 0;
        ended();
    }
}

int rr_message(int *b)
{
    if (b[4] == MSG_TW_EGO && R.running && b[5] == TXT_ID) {
        R.child = b[1];
        R.heard_cs = ro_now_cs();
        if (!R.on)
            kill_child(); /* (stopped before it said who it was) */
        return 1;
    }
    if (b[4] == MSG_TW_OUTPUT && R.running && b[1] == R.child) {
        R.heard_cs = ro_now_cs();
        const char *d = (const char *) &b[6];
        for (int i = 0; i < b[5] && i < 232; i++) {
            if (d[i] == '\n' || d[i] == '\r') {
                R.line[R.line_len] = 0;
                if (R.line_len)
                    heard(R.line);
                R.line_len = 0;
            } else if (R.line_len < (int) sizeof(R.line) - 1) {
                R.line[R.line_len++] = d[i];
            }
        }
        return 1;
    }
    if (b[4] == MSG_TW_MORIO && R.running && b[1] == R.child) {
        if (R.line_len) {
            R.line[R.line_len] = 0;
            heard(R.line);
            R.line_len = 0;
        }
        ended();
        return 1;
    }
    return 0;
}

mlt_producer rr_playlist(int *rendered)
{
    *rendered = 0;
    return R.pv && A.proj ? re_previews_producer(R.pv, A.proj, rendered) : NULL;
}

void rr_wipe(void)
{
    char unix_dir[512];
    re_wipe_dir(app_mlt_name(DIR, unix_dir, sizeof(unix_dir)));
}

void rr_close(void)
{
    R.on = 0;
    kill_child();
    R.running = 0;
    R.child = 0;
    re_previews_free(R.pv); /* (its files deleted) */
    rr_wipe();              /* (and a part made: its TaskWindow killed) */
    R.pv = NULL;
    R.n = 0;
    R.known = 0;
}

#ifdef RE_TEST
int re_test_renders(void) { return re_previews_count(R.pv); }
const char *re_test_render_file(void) { return R.file; }
#endif
