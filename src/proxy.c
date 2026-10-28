/* proxy.c - proxies (proxy.h): which files want one, the project's
 * files swapped for theirs (and back), leftovers wiped.
 *
 * The swap is made on the project's MLT XML: each file's name, where it's
 * a property's whole value (or a timewarp's "2:" and the name), replaced
 * by its proxy's (or back), and the project made again from it, as undo
 * does. Saving puts the files' names back in what's written.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "proxy.h"
#include "project_internal.h"

#include <dirent.h>
#include <libavformat/avformat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int re_proxy_wanted(const char *file)
{
    AVFormatContext *fc = NULL;
    int wanted = 0;
    if (!file || avformat_open_input(&fc, file, NULL, NULL) < 0)
        return 0;
    if (avformat_find_stream_info(fc, NULL) >= 0) {
        int v = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        if (v >= 0) {
            AVStream *st = fc->streams[v];
            const char *fmt = fc->iformat ? fc->iformat->name : "";
            int picture = (st->disposition & AV_DISPOSITION_ATTACHED_PIC) || strstr(fmt, "_pipe")
                          || !strcmp(fmt, "image2") || st->nb_frames == 1;
            wanted = !picture && fc->duration > 0 /* (raw video, no length known: none can be made) */
                     && (st->codecpar->height > 540 || st->codecpar->codec_id != AV_CODEC_ID_H264);
        }
    }
    avformat_close_input(&fc);
    return wanted;
}

/* ---- the project's proxies ------------------------------------------------------------------- */

void re_project_set_proxy(re_project *p, const char *file, const char *proxy)
{
    int i;
    for (i = 0; i < p->n_px; i++)
        if (!strcmp(p->px[i].file, file))
            break;
    if (i < p->n_px) {
        free(p->px[i].proxy);
        if (!proxy) {
            free(p->px[i].file);
            p->px[i] = p->px[--p->n_px];
            return;
        }
        p->px[i].proxy = strdup(proxy);
        return;
    }
    if (!proxy)
        return;
    void *more = realloc(p->px, (size_t) (p->n_px + 1) * sizeof(*p->px));
    if (!more)
        return;
    p->px = more;
    p->px[p->n_px].file = strdup(file);
    p->px[p->n_px].proxy = strdup(proxy);
    p->n_px++;
}

void re_project_use_proxies(re_project *p, int on)
{
    p->px_on = on;
}

int re_project_proxies(re_project *p)
{
    return p->n_px;
}

const char *re_project_original(re_project *p, const char *file)
{
    for (int i = 0; file && i < p->n_px; i++)
        if (p->px[i].proxy && !strcmp(p->px[i].proxy, file))
            return p->px[i].file;
    return file;
}

void rp_proxies_free(re_project *p)
{
    for (int i = 0; i < p->n_px; i++) {
        free(p->px[i].file);
        free(p->px[i].proxy);
    }
    free(p->px);
    p->px = NULL;
    p->n_px = 0;
}

/* A name as it is in MLT's XML */
static void escaped(char *out, size_t size, const char *s)
{
    size_t n = 0;
    for (; *s && n + 7 < size; s++) {
        const char *e = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '"' ? "&quot;" : NULL;
        if (e) {
            strcpy(out + n, e);
            n += strlen(e);
        } else {
            out[n++] = *s;
        }
    }
    out[n] = 0;
}

/* Each from (a whole value: "<" after it, and ">" or ':' before) made to */
static char *swap(char *xml, const char *from, const char *to)
{
    char f[1100], t[1100];
    escaped(f, sizeof(f), from);
    escaped(t, sizeof(t), to);
    size_t fl = strlen(f), tl = strlen(t), n = 0;
    if (!fl)
        return xml;
    for (const char *c = xml; (c = strstr(c, f)); c += fl)
        if (c > xml && (c[-1] == '>' || c[-1] == ':') && c[fl] == '<')
            n++;
    if (!n)
        return xml;
    char *out = malloc(strlen(xml) + n * (tl > fl ? tl - fl : 0) + 1), *o = out;
    if (!out)
        return xml;
    const char *c = xml, *hit;
    while ((hit = strstr(c, f))) {
        int whole = hit > xml && (hit[-1] == '>' || hit[-1] == ':') && hit[fl] == '<';
        size_t k = (size_t) (hit - c) + (whole ? 0 : fl);
        memcpy(o, c, k);
        o += k;
        if (whole) {
            memcpy(o, t, tl);
            o += tl;
        }
        c = hit + fl;
    }
    strcpy(o, c);
    free(xml);
    return out;
}

/* A name as MLT's XML may have it: from the current directory, when it's in it */
static const char *from_here(const char *file)
{
    static char cwd[1024];
    if (!getcwd(cwd, sizeof(cwd)))
        return file;
    size_t n = strlen(cwd);
    return !strncmp(file, cwd, n) && file[n] == '/' ? file + n + 1 : file;
}

/* Each from (as it is, and from the current directory) made to */
static char *swap_both(char *xml, const char *from, const char *to)
{
    xml = swap(xml, from, to);
    const char *rel = from_here(from);
    return xml && rel != from ? swap(xml, rel, to) : xml;
}

char *rp_proxy_xml(re_project *p, char *xml, int to_proxies)
{
    for (int i = 0; xml && i < p->n_px; i++) /* (back to the files first: a snapshot may have either) */
        xml = swap_both(xml, p->px[i].proxy, p->px[i].file);
    for (int i = 0; xml && to_proxies && i < p->n_px; i++)
        xml = swap_both(xml, p->px[i].file, p->px[i].proxy);
    return xml;
}

int re_project_apply_proxies(re_project *p)
{
    if (!p->tractor)
        return 0;
    char *now = rp_snapshot(p);
    if (!now)
        return -1;
    char *want = rp_proxy_xml(p, strdup(now), p->px_on);
    int r = 0;
    if (want && strcmp(want, now))
        r = rp_restore(p, want) < 0 ? -1 : 1;
    free(want);
    free(now);
    return r;
}

/* ---- leftovers ------------------------------------------------------------------------------- */

int re_wipe_dir(const char *dir)
{
    DIR *d = opendir(dir);
    int r = 0;
    if (!d)
        return 0; /* (none: nothing to do) */
    struct dirent *e;
    char path[1100];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (remove(path))
            r = -1;
    }
    closedir(d);
    if (rmdir(dir))
        r = -1;
    return r;
}
