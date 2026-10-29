/* proxies.c - proxies on the desktop (app.h), from src/proxy.c: each video
 * in the project that wants one (taller than 540 lines, or not H.264) gets
 * a proxy made, one at a time, each in a TaskWindow of its own (as an
 * export: exportbox.c): !RunImage -proxy file proxy hardware. Once made,
 * the project is made again with the proxy in its file's place, when
 * nothing's playing or being dragged (the Viewer as it was). View > Use
 * proxies (ticked) turns them off and on. Exports, renders and saves
 * always use the files themselves.
 *
 * The Timeline's title says while they're being made. They're kept in
 * <Wimp$ScrapDir>.ReelEdPxy, with an index (Index: each proxy's name and
 * its file's) so that a project recovered after ReelEdit stopped finds
 * them again. Closing the editor and quitting delete them; so does the
 * editor opening without recovering a project (what was left of a
 * session that stopped).
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "app.h"
#include "../proxy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MSG_TW_EGO      0x808C2
#define MSG_TW_OUTPUT   0x808C1
#define MSG_TW_MORIO    0x808C3
#define MSG_TW_MORITE   0x808C4
#define TXT_ID          0x52455058 /* ("REPX": a proxy's TaskWindow) */
#define Wimp_StartTask  0x400DE
#define OS_File         0x08
#define OS_SWINumberFromString 0x39
#define DIR   "<Wimp$ScrapDir>.ReelEdPxy"
#define INDEX DIR ".Index"

enum { UNKNOWN, MAKING, NOT_WANTED, MADE, FAILED };

static struct
{
    int off;              /* View > Use proxies: off */
    re_project *proj;     /* the project they were given to */
    struct
    {
        char *file;       /* (MLT's name) */
        char *proxy;      /* (MLT's name: made) */
        int state;
    } *f;
    int n;
    int running, child, ok, at; /* a TaskWindow going (its task, once it says); for f[at] */
    char out[256];        /* (RISC OS name) */
    char line[256];
    int line_len;
    unsigned serial;
    int apply;            /* the project to be made again (with them, or without) */
    unsigned heard_cs;    /* when it last said anything (or started): px_tick's watch */
    int done, total;      /* its progress (frames) */
} X;

/* A file's name as one: MLT's names are from the root, but one given from
   where the program is (the Linux tests) is made so too, or the same clip
   had a second proxy made under its other name */
static const char *whole(const char *file, char *buf, int size)
{
    char cwd[400];
    if (file[0] == '/' || !getcwd(cwd, sizeof(cwd)))
        return file;
    snprintf(buf, size, "%s/%s", cwd, file);
    return buf;
}

static int find(const char *file)
{
    char a[600], b[600];
    file = whole(file, a, sizeof(a));
    for (int i = 0; i < X.n; i++)
        if (!strcmp(whole(X.f[i].file, b, sizeof(b)), file))
            return i;
    return -1;
}

static int add(const char *file, const char *proxy, int state)
{
    int i = find(file);
    if (i >= 0)
        return i;
    void *more = realloc(X.f, (size_t) (X.n + 1) * sizeof(*X.f));
    if (!more)
        return -1;
    X.f = more;
    X.f[X.n].file = strdup(file);
    X.f[X.n].proxy = proxy ? strdup(proxy) : NULL;
    X.f[X.n].state = state;
    return X.n++;
}

/* A RISC OS name as MLT's full Unix name (a proxy is named in the project's
   XML, which takes a name not from the root as from the project's
   directory) */
static const char *full_name(const char *ro, char *buf, int size)
{
    char whole[512];
#ifdef __riscos__
    _kernel_swi_regs r;
    r.r[0] = 37; /* OS_FSControl 37: canonicalised */
    r.r[1] = (intptr_t) ro;
    r.r[2] = (intptr_t) whole;
    r.r[3] = 0;
    r.r[4] = 0;
    r.r[5] = sizeof(whole);
    if (ro_swi(0x29, &r))
        snprintf(whole, sizeof(whole), "%s", ro);
#else
    char cwd[400];
    snprintf(whole, sizeof(whole), "%s/%s", getcwd(cwd, sizeof(cwd)) ? cwd : ".", ro);
#endif
    const char *u = app_mlt_name(whole, buf, size);
    if (u != buf)
        snprintf(buf, size, "%s", u);
    return buf;
}

static int can_hw(void)
{
    _kernel_swi_regs r;
    r.r[1] = (intptr_t) "VCHIQ_MsgQueue";
    return !ro_swi(OS_SWINumberFromString, &r);
}

int px_making(int *made, int *all)
{
    *made = *all = 0;
    for (int i = 0; i < X.n; i++)
        if (X.f[i].state == MAKING || X.f[i].state == MADE || X.f[i].state == UNKNOWN) {
            (*all)++;
            *made += X.f[i].state == MADE;
        }
    return X.running;
}

/* The next file wanting a proxy started: 1, or 0 (none) */
static int next(void)
{
    char unix_out[512], cmd[1400];
    _kernel_swi_regs r;
    for (int i = 0; i < X.n; i++) {
        if (X.f[i].state != UNKNOWN)
            continue;
        if (!re_proxy_wanted(X.f[i].file)) {
            X.f[i].state = NOT_WANTED;
            continue;
        }
        r.r[0] = 8; /* OS_File 8: the directory (there already: no harm) */
        r.r[1] = (intptr_t) DIR;
        r.r[4] = 0;
        ro_swi(OS_File, &r);
        snprintf(X.out, sizeof(X.out), DIR ".P%X%u", ro_now_cs(), ++X.serial); /* (none a recovered one's) */
        const char *uout = full_name(X.out, unix_out, sizeof(unix_out));
        char hw[4];
        snprintf(hw, sizeof(hw), "%d", can_hw());
        const char *args[3] = {X.f[i].file, uout, hw};
        _kernel_oserror *e = app_child_start("proxy", args, 3, "ReelEdit proxy", TXT_ID, 1, cmd, sizeof(cmd));
        app_log("proxy: %s: %s%s%s", X.f[i].file, cmd, e ? ": " : "", e ? e->errmess : "");
        if (e) {
            X.f[i].state = FAILED;
            continue;
        }
        X.f[i].state = MAKING;
        X.at = i;
        X.running = 1;
        X.child = X.ok = 0;
        X.line_len = 0;
        X.heard_cs = ro_now_cs();
        X.done = X.total = 0;
        tl_title();
        return 1;
    }
    tl_title();
    return 0;
}

/* The project given the proxies made (a new one: an opened project) */
static void given(void)
{
    if (!A.proj || A.proj == X.proj)
        return;
    X.proj = A.proj;
    re_project_use_proxies(A.proj, !X.off);
    for (int i = 0; i < X.n; i++)
        if (X.f[i].state == MADE)
            re_project_set_proxy(A.proj, X.f[i].file, X.f[i].proxy);
    X.apply = 1;
}

void px_edited(void)
{
    if (!A.proj)
        return;
    given();
    for (int i = 0; i < re_project_items(A.proj); i++) {
        const char *file = re_project_item_file(A.proj, i);
        if (file)
            add(re_project_original(A.proj, file), NULL, UNKNOWN);
    }
    for (int i = 0; i < re_project_overlays(A.proj); i++) {
        re_overlay o;
        const char *file = re_project_overlay(A.proj, i, &o) || !o.video ? NULL : re_project_overlay_file(A.proj, i);
        if (file)
            add(re_project_original(A.proj, file), NULL, UNKNOWN);
    }
    if (!X.running)
        next();
}

int px_pending(void)
{
    return X.apply && A.proj;
}

void px_applied(void)
{
    X.apply = 0;
}

int px_on(void)
{
    return !X.off;
}

void px_toggle(void)
{
    X.off = !X.off;
    if (A.proj)
        re_project_use_proxies(A.proj, !X.off);
    X.apply = 1;
    app_log("proxies %s", X.off ? "off" : "on");
}

/* It has ended: the proxy given to the project, or deleted */
static void ended(void)
{
    char unix_out[512];
    const char *uout = full_name(X.out, unix_out, sizeof(unix_out));
    X.running = 0;
    X.child = 0;
    app_child_ended(TXT_ID);
    if (X.at >= 0 && X.at < X.n) {
        if (X.ok) {
            X.f[X.at].state = MADE;
            free(X.f[X.at].proxy);
            X.f[X.at].proxy = strdup(uout);
            if (A.proj)
                re_project_set_proxy(A.proj, X.f[X.at].file, uout);
            X.apply = 1;
            char unix_index[512];
            FILE *f = fopen(app_mlt_name(INDEX, unix_index, sizeof(unix_index)), "a");
            if (f) {
                fprintf(f, "%s\t%s\n", uout, X.f[X.at].file);
                fclose(f);
            }
            app_log("proxy: %s made", X.f[X.at].file);
        } else {
            X.f[X.at].state = FAILED;
            remove(uout);
            app_log("proxy: %s couldn't be made", X.f[X.at].file);
        }
    }
    next();
}

static void heard(const char *line)
{
    int d, t;
    if (!strcmp(line, "done"))
        X.ok = 1;
    else if (sscanf(line, "progress %d %d", &d, &t) == 2) {
        int was = X.total > 0 ? X.done * 100 / X.total : -1;
        X.done = d;
        X.total = t;
        if ((t > 0 ? d * 100 / t : -1) != was)
            tl_title();
    } else if (!strstr(line, "Duplicate filter registration")) /* (ReelEdit's own over MLT's: as meant) */
        app_log("proxy: %s", line); /* (failed: why; anything else it said) */
}

/* Nothing said for this long: it's stuck (it says its progress four times a
   second), so stopped and the proxy given up (the clip's file used) */
#define STUCK_CS 9000

void px_tick(void)
{
    if (!X.running || ro_now_cs() - X.heard_cs < STUCK_CS)
        return;
    app_log("proxy: %s: nothing from it for %d s: stopped", X.at >= 0 && X.at < X.n ? X.f[X.at].file : "?",
            STUCK_CS / 100);
    X.heard_cs = ro_now_cs();
    if (X.child) { /* (killed: its TaskWindow_Morio ends it) */
        _kernel_swi_regs r;
        int m[6] = {20, 0, 0, 0, MSG_TW_MORITE, 0};
        r.r[0] = 17; r.r[1] = (intptr_t) m; r.r[2] = X.child;
        ro_swi(Wimp_SendMessage, &r);
    } else { /* (it never said who it was: given up here) */
        X.ok = 0;
        ended();
    }
}

int px_watching(void)
{
    return X.running;
}

int px_progress(void)
{
    return X.running && X.total > 0 ? X.done * 100 / X.total : -1;
}

int px_message(int *b)
{
    if (b[4] == MSG_TW_EGO && X.running && b[5] == TXT_ID) {
        X.child = b[1];
        X.heard_cs = ro_now_cs();
        return 1;
    }
    if (b[4] == MSG_TW_OUTPUT && X.running && b[1] == X.child) {
        X.heard_cs = ro_now_cs();
        const char *d = (const char *) &b[6];
        for (int i = 0; i < b[5] && i < 232; i++) {
            if (d[i] == '\n' || d[i] == '\r') {
                X.line[X.line_len] = 0;
                if (X.line_len)
                    heard(X.line);
                X.line_len = 0;
            } else if (X.line_len < (int) sizeof(X.line) - 1) {
                X.line[X.line_len++] = d[i];
            }
        }
        return 1;
    }
    if (b[4] == MSG_TW_MORIO && X.running && b[1] == X.child) {
        if (X.line_len) {
            X.line[X.line_len] = 0;
            heard(X.line);
            X.line_len = 0;
        }
        ended();
        return 1;
    }
    return 0;
}

/* ---- cleaning up ------------------------------------------------------------------------------- */

/* The list forgotten (the files left as they are) */
static void forget(void)
{
    for (int i = 0; i < X.n; i++) {
        free(X.f[i].file);
        free(X.f[i].proxy);
    }
    free(X.f);
    X.f = NULL;
    X.n = 0;
    X.running = X.child = 0;
    X.proj = NULL;
    X.apply = 0;
}

void px_wipe(void)
{
    char unix_name[512];
    for (int i = 0; i < X.n; i++) /* (each known one, and whatever else is there) */
        if (X.f[i].proxy)
            remove(X.f[i].proxy);
    if (X.running)
        remove(full_name(X.out, unix_name, sizeof(unix_name)));
    remove(app_mlt_name(INDEX, unix_name, sizeof(unix_name)));
    re_wipe_dir(app_mlt_name(DIR, unix_name, sizeof(unix_name)));
    forget();
}

void px_close(void)
{
    _kernel_swi_regs r;
    int m[6];
    if (X.running && X.child) { /* (killed: its file goes below) */
        memset(m, 0, sizeof(m));
        m[0] = 20; m[4] = MSG_TW_MORITE;
        r.r[0] = 17; r.r[1] = (intptr_t) m; r.r[2] = X.child;
        ro_swi(Wimp_SendMessage, &r);
    }
    px_wipe();
}

void px_forget(void)
{
    forget();
}

void px_recover(void)
{
    char unix_index[512], line[1100];
    FILE *f = fopen(app_mlt_name(INDEX, unix_index, sizeof(unix_index)), "r");
    forget();
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = 0;
        FILE *there = fopen(line, "rb");
        if (!there)
            continue;
        fclose(there);
        add(tab + 1, line, MADE);
    }
    fclose(f);
    app_log("proxy: %d recovered", X.n);
}

#ifdef RE_TEST
const char *re_test_proxy_out(void) { return X.out; }
#endif
