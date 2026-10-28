/* guide.c - the User Guide (app.h): !ReelEdit.Guide.index/html, opened in
 * whatever reads HTML on this machine (NetSurf, usually), from the icon
 * bar menu's User guide, the main menu's User guide or F1.
 *
 * It's run as the Filer would run it (Filer_Run, by its filetype, HTML
 * &FAF), so it opens in whatever opens HTML on this machine. If nothing
 * has said it reads HTML (no Alias$@RunType_FAF), or a text editor has
 * (Edit, StrongED, Zap: the guide would be shown as text), ReelEdit says
 * how to get a browser to take it instead (NetSurf, once seen, does).
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "app.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#define Wimp_StartTask 0x400DE
#define GUIDE "<ReelEdit$Dir>.Guide.index/html"

/* 1: s has t in it, whatever the case */
static int has(const char *s, const char *t)
{
    for (; *s; s++) {
        int i = 0;
        while (t[i] && s[i] && tolower((unsigned char) s[i]) == t[i])
            i++;
        if (!t[i])
            return 1;
    }
    return 0;
}

void gd_open(void)
{
    const char *run = getenv("Alias$@RunType_FAF");
    if (!run || !*run || has(run, "edit") || has(run, "stronged") || has(run, "zap")) {
        app_report("ReelEdit can't show its User Guide yet: no web browser has said it opens HTML files on this "
                   "computer. Double-click NetSurf (in Apps) once, then choose User guide again; or open "
                   "!ReelEdit.Guide.index/html in your browser.");
        app_log("guide: no browser (HTML runs as \"%s\")", run ? run : "");
        return;
    }
    /* (its full name: a browser handed "<ReelEdit$Dir>..." may not know the
       variable; on RISC OS for Linux one showed the name as it was) */
    char whole[600], cmd[700];
    snprintf(whole, sizeof(whole), "%s", GUIDE);
    _kernel_swi_regs r;
#ifdef __riscos__
    r.r[0] = 37; /* OS_FSControl 37: canonicalised */
    r.r[1] = (intptr_t) GUIDE;
    r.r[2] = (intptr_t) whole;
    r.r[3] = 0;
    r.r[4] = 0;
    r.r[5] = sizeof(whole);
    if (ro_swi(0x29, &r))
        snprintf(whole, sizeof(whole), "%s", GUIDE);
#endif
    snprintf(cmd, sizeof(cmd), "Filer_Run %s", whole);
    r.r[0] = (intptr_t) cmd;
    _kernel_oserror *e = ro_swi(Wimp_StartTask, &r);
    app_log("guide: %s%s (HTML runs as \"%s\")", e ? "not opened: " : "opened", e ? e->errmess : "", run);
    if (e)
        app_report(e->errmess);
}
