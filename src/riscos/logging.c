/* logging.c - ReelEdit's log (app.h: app_log): what it did, for reports.
 *
 * Off unless asked for. The icon bar menu's Logging item turns it on or
 * off; the choice is kept in <Choices$Write>.ReelEdit.Logging (there: on).
 * The log is <Wimp$ScrapDir>.ReelEditLog, started afresh each run (on
 * again in the same run: added to). The system variable ReelEdit$Log, if
 * set, wins at start-up: a file to log to, or "off".
 *
 * While it's on, MLT's and FFmpeg's messages go in it too (the engine's
 * lines); while it's off they're dropped.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "app.h"
#include "display.h"

#include <libavutil/cpu.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef RE_VERSION
#define RE_VERSION "0.2"
#endif
#define OS_File 0x08
#define CHOICE_DIR "<Choices$Write>.ReelEdit"
#define CHOICE     CHOICE_DIR ".Logging"
#define LOG_FILE   "<Wimp$ScrapDir>.ReelEditLog"

static struct
{
    FILE *f;
    char name[256]; /* the file it goes to */
    int opened;     /* opened before this run (on again: added to) */
} L;

void app_log(const char *fmt, ...)
{
    va_list ap;
    if (!L.f)
        return;
    fprintf(L.f, "%7.2f ", ro_now_cs() / 100.0);
    va_start(ap, fmt);
    vfprintf(L.f, fmt, ap);
    va_end(ap);
    fputc('\n', L.f);
    fflush(L.f);
    /* (on the disc now, length and all: a log left open was lost, or
       empty, when the machine had to be reset with Ctrl-Break) */
    fsync(fileno(L.f));
}

static void line(const char *text)
{
    app_log("%s", text);
}

static void open_log(void)
{
    if (L.f)
        return;
    L.f = fopen(L.name, L.opened ? "a" : "w");
    if (!L.f)
        return;
    L.opened = 1;
    app_log(APP " %s", RE_VERSION);
    app_log("NEON: %s", av_get_cpu_flags() & AV_CPU_FLAG_NEON ? "yes (MLT's NEON loops used)" : "no");
    re_engine_messages(line);
}

static void close_log(void)
{
    if (!L.f)
        return;
    app_log("logging off");
    re_engine_messages(NULL);
    fclose(L.f);
    L.f = NULL;
}

static int chosen(void)
{
    char unix_name[512];
    FILE *f = fopen(app_mlt_name(CHOICE, unix_name, sizeof(unix_name)), "r");
    int on = 0;
    if (f) {
        char t[8] = "";
        on = fgets(t, sizeof(t), f) && !strncmp(t, "on", 2);
        fclose(f);
    }
    return on;
}

void lg_start(void)
{
    const char *var = getenv("ReelEdit$Log");
    int on = var && *var ? strcmp(var, "off") != 0 : chosen();
    snprintf(L.name, sizeof(L.name), "%s", var && *var && strcmp(var, "off") ? var : LOG_FILE);
    re_display_set_log(line);
    if (on)
        open_log();
    else
        re_engine_messages(NULL);
}

int lg_on(void)
{
    return L.f != NULL;
}

void lg_set(int on)
{
    char unix_name[512];
    _kernel_swi_regs r;
    r.r[0] = 8; /* OS_File 8: the directory (there already: no harm) */
    r.r[1] = (intptr_t) CHOICE_DIR;
    r.r[4] = 0;
    ro_swi(OS_File, &r);
    FILE *f = fopen(app_mlt_name(CHOICE, unix_name, sizeof(unix_name)), "w");
    if (f) {
        fputs(on ? "on\n" : "off\n", f);
        fclose(f);
    }
    if (on) {
        open_log();
        app_log("logging on");
    } else {
        close_log();
    }
}

void lg_end(void)
{
    if (L.f) {
        fclose(L.f);
        L.f = NULL;
    }
}
