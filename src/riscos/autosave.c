/* autosave.c - the project kept safe while it's edited (app.h): after an
 * edit, if it has changes not saved and the last copy is half a minute
 * old (or there's none), a copy of it is saved to
 * <Choices$Write>.ReelEdit.Autosave (MLT XML), with where it was saved
 * from in AutosaveFor beside it. Saving, closing the editor (keeping or
 * discarding its changes) and quitting take the copy away: a copy left is
 * a project whose changes were lost (Reel Edit stopped, the computer
 * reset).
 *
 * Opening the editor with a copy there asks whether to recover it: the
 * project as it was, still to be saved (where it came from, if it had a
 * file).
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "app.h"

#include <stdio.h>
#include <string.h>

#define OS_File 0x08
#define EVERY_CS 3000 /* (half a minute) */
#define DIR  "<Choices$Write>.ReelEdit"
#define COPY DIR ".Autosave"
#define FOR  DIR ".AutosaveFor"

static struct
{
    unsigned last_cs;  /* when the last copy was saved */
    int saved;         /* there's a copy (of this session's) */
} S = {0, 0};

void as_edited(void)
{
    char unix_name[512];
    _kernel_swi_regs r;
    if (!A.proj || !re_project_modified(A.proj) || !re_project_producer(A.proj))
        return;
    unsigned now = ro_now_cs();
    if (S.saved && (int) (now - S.last_cs) < EVERY_CS)
        return;
    r.r[0] = 8; /* OS_File 8: the directory (there already: no harm) */
    r.r[1] = (intptr_t) DIR;
    r.r[4] = 0;
    ro_swi(OS_File, &r);
    if (re_project_save_copy(A.proj, app_mlt_name(COPY, unix_name, sizeof(unix_name)))) {
        app_log("autosave: couldn't save to %s", COPY);
        return;
    }
    FILE *f = fopen(app_mlt_name(FOR, unix_name, sizeof(unix_name)), "w");
    if (f) {
        fprintf(f, "%s\n", A.path);
        fclose(f);
    }
    S.saved = 1;
    S.last_cs = now;
    app_log("autosave: saved");
}

void as_forget(void)
{
    char unix_name[512];
    remove(app_mlt_name(COPY, unix_name, sizeof(unix_name)));
    remove(app_mlt_name(FOR, unix_name, sizeof(unix_name)));
    S.saved = 0;
}

int as_offer(void)
{
    char unix_name[512], path[256] = "";
    FILE *f = fopen(app_mlt_name(COPY, unix_name, sizeof(unix_name)), "r");
    if (!f)
        return 0;
    fclose(f);
    f = fopen(app_mlt_name(FOR, unix_name, sizeof(unix_name)), "r");
    if (f) {
        if (fgets(path, sizeof(path), f))
            path[strcspn(path, "\r\n")] = 0;
        fclose(f);
    }
    char t[400];
    snprintf(t, sizeof(t),
             "Reel Edit didn't close properly, and %.200s had changes that weren't saved. Recover them?",
             path[0] ? path : "a project");
    if (app_ask(t, "Recover,Discard") != 3) {
        app_log("autosave: discarded");
        as_forget();
        return 0;
    }
    re_project *p = re_project_open(app_mlt_name(COPY, unix_name, sizeof(unix_name)), 1);
    if (!p) {
        app_report("The project's copy couldn't be read.");
        as_forget();
        return 0;
    }
    re_project_set_modified(p);
    A.proj = p;
    snprintf(A.path, sizeof(A.path), "%s", path);
    app_log("autosave: recovered (%s)", path[0] ? path : "no file");
    S.saved = 1;
    S.last_cs = ro_now_cs();
    return 1;
}
