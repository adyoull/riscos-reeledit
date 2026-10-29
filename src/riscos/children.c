/* children.c - an export, a render or a proxy started in a TaskWindow of
 * its own (app.h: app_child_start), for exportbox.c, renders.c and
 * proxies.c.
 *
 * Its arguments go in a scrap file, one a line (!RunImage -export @file,
 * export.h's re_export_args), not on the command line: a file's name may
 * hold spaces (a clip from a DOS disc: "The Simpsons Movie - Trailer.avi"),
 * which split it into several arguments there, and the child failed or
 * waited forever.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "app.h"

#include <stdio.h>

#define Wimp_StartTask 0x400DE

_kernel_oserror *app_child_start(const char *mode, const char *const *args, int n, const char *name, unsigned txt,
                                 char *cmd, int size)
{
    static unsigned serial;
    static _kernel_oserror none = {0, "the arguments couldn't be written to a scrap file"};
    char ro[64], unix_name[512];
    snprintf(ro, sizeof(ro), "<Wimp$ScrapDir>.ReelEdA%X%u", ro_now_cs(), ++serial);
    const char *u = app_mlt_name(ro, unix_name, sizeof(unix_name));
    FILE *f = fopen(u, "w");
    if (!f) {
        snprintf(cmd, size, "(%s)", ro);
        return &none;
    }
    for (int i = 0; i < n; i++)
        fprintf(f, "%s\n", args[i] ? args[i] : "");
    fclose(f);
    snprintf(cmd, size,
             "TaskWindow \"<ReelEdit$Dir>.!RunImage -%s @%s\" -wimpslot 40960K -name \"%s\" -task &%08X -txt &%08X -quit",
             mode, u, name, A.task, txt);
    _kernel_swi_regs r;
    r.r[0] = (intptr_t) cmd;
    return ro_swi(Wimp_StartTask, &r);
}
