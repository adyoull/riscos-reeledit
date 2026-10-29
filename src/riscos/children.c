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
 * The VideoCore is shared with it. MLT counts the hardware decoders open
 * only in its own program (patches/mlt 0004), and every child also has
 * the VideoCore's encoder open: with the editor holding two decoders, a
 * proxy's decoder and its encoder stalled ("No packet from the VideoCore
 * in 5 s"), and an export started then got none ("Input enable: ENOSPC").
 * So, while a child runs:
 *   - the editor's idle hardware decoders are closed when it starts (0022:
 *     it opens them again, within its limit, when it next plays);
 *   - all the programs together have at most two h264_vchiq (three with
 *     no encoder, 0004), leaving the encoder room;
 *   - a child is given what's free, up to what it wants (a proxy one, a
 *     render or an export two), as its first argument ("-decoders
 *     h264_vchiq=N,hevc_hwdec=M": re_export_main sets it as its limit,
 *     0020), and the editor's own limit is what's left until it ends
 *     (app_child_ended);
 *   - only one runs at a time: proxies and renders wait for any other
 *     (app_child_others), and an export pauses them and waits.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "app.h"

#include <stdio.h>
#include <string.h>

#define Wimp_StartTask 0x400DE

/* (patches/mlt 0020; the build's MLT_AVFORMAT_PREFER_MAX_DEFAULT) */
extern int mlt_avformat_prefer_held(const char *name);
extern void mlt_avformat_prefer_limit(const char *spec);
extern int mlt_avformat_prefer_release(const char *name); /* (0022) */
#define H264_MAX 2 /* (with a child's encoder: the build's three is without one) */
#define HEVC_MAX 1

#define CHILDREN 4
static struct
{
    unsigned txt; /* (0: free) */
    int h264, hevc;
} given[CHILDREN];

static char own_spec[64];

static void own_limit(void)
{
    int h = 0, e = 0, any = 0;
    for (int i = 0; i < CHILDREN; i++)
        if (given[i].txt) {
            any = 1;
            h += given[i].h264;
            e += given[i].hevc;
        }
    snprintf(own_spec, sizeof(own_spec), "h264_vchiq=%d,hevc_hwdec=%d", H264_MAX - h, HEVC_MAX - e);
    if (!any)
        own_spec[0] = 0;
    mlt_avformat_prefer_limit(any ? own_spec : NULL);
}

#ifdef RE_TEST
const char *re_test_own_decoders(void) /* (the editor's own limit now: "" none) */
{
    return own_spec;
}
#endif

int app_child_others(unsigned txt)
{
    int n = 0;
    for (int i = 0; i < CHILDREN; i++)
        n += given[i].txt && given[i].txt != txt;
    return n;
}

void app_child_ended(unsigned txt)
{
    for (int i = 0; i < CHILDREN; i++)
        if (given[i].txt == txt)
            given[i].txt = 0;
    own_limit();
}

/* What a new child may open (want at most), given the editor's and the
   other children's: its place in given[] (-1: none free) */
static int give(unsigned txt, int want, char *spec, int size)
{
    app_child_ended(txt); /* (one a kind: an old one's given back) */
    int closed = mlt_avformat_prefer_release("h264_vchiq") + mlt_avformat_prefer_release("hevc_hwdec");
    if (closed)
        app_log("the VideoCore: %d of the editor's decoders closed, for a child", closed);
    int h = mlt_avformat_prefer_held("h264_vchiq"), e = mlt_avformat_prefer_held("hevc_hwdec"), slot = -1;
    for (int i = 0; i < CHILDREN; i++)
        if (given[i].txt) {
            h += given[i].h264;
            e += given[i].hevc;
        } else if (slot < 0) {
            slot = i;
        }
    int gh = H264_MAX - h, ge = HEVC_MAX - e;
    gh = gh < 0 ? 0 : gh > want ? want : gh;
    ge = ge < 0 ? 0 : ge > 1 ? 1 : ge;
    if (slot < 0)
        gh = ge = 0;
    else
        given[slot].txt = txt, given[slot].h264 = gh, given[slot].hevc = ge;
    snprintf(spec, size, "-decoders h264_vchiq=%d,hevc_hwdec=%d", gh, ge);
    own_limit();
    return slot;
}

_kernel_oserror *app_child_start(const char *mode, const char *const *args, int n, const char *name, unsigned txt,
                                 int decoders, char *cmd, int size)
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
    char spec[64];
    give(txt, decoders, spec, sizeof(spec));
    fprintf(f, "%s\n", spec);
    for (int i = 0; i < n; i++)
        fprintf(f, "%s\n", args[i] ? args[i] : "");
    fclose(f);
    snprintf(cmd, size,
             "TaskWindow \"<ReelEdit$Dir>.!RunImage -%s @%s\" -wimpslot 40960K -name \"%s\" -task &%08X -txt &%08X -quit",
             mode, u, name, A.task, txt);
    _kernel_swi_regs r;
    r.r[0] = (intptr_t) cmd;
    _kernel_oserror *e = ro_swi(Wimp_StartTask, &r);
    if (e)
        app_child_ended(txt);
    return e;
}
