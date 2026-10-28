/* screens.h - screenshots from the fake Wimp (editor_test): what !ReelEdit
 * draws is recorded (the windows it makes, their icons, its OS_Plot
 * rectangles, Wimp_TextOp text, the picture sprite it plots, the overlay's
 * picture, the menu it opens), and snap() writes it out as a display list
 * that tests/host/screens.py turns into a PNG, as the desktop would show it.
 *
 * Only when SCREENS names a directory (tests/host/run.sh --screens). The
 * window and menu blocks are read as !ReelEdit lays them out on this host
 * (pointers 8 bytes): the same structs as src/riscos's.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef FAKE_SCREENS_H
#define FAKE_SCREENS_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *text; const char *valid; int len; } scr_ind;
typedef struct { int x0, y0, x1, y1, flags; union { char name[12]; scr_ind ind; } d; } scr_icon;
typedef struct
{
    int x0, y0, x1, y1;
    int sx, sy, behind, flags;
    unsigned char tfg, tbg, wfg, wbg, sofg, sibg, tfocus, xflags;
    int ex0, ey0, ex1, ey1;
    int tflags, wbutton, sprites;
    short minw, minh;
    scr_ind title;
    int nicons;
    scr_icon icon[];
} scr_wblock;
typedef struct { int flags; intptr_t sub; int iflags; const char *text; const char *valid; int len; } scr_mitem;
typedef struct
{
    char title[12];
    unsigned char tfg, tbg, wfg, wbg;
    int w, h, gap;
    scr_mitem item[];
} scr_menu;

#define SCR_WINS 6
#define SCR_ICONS 32
#define SCR_OPS 6000

typedef struct
{
    char type;         /* R rect, P triangle, T text, S sprite, C clip */
    int x0, y0, x1, y1, x2, y2;
    unsigned colour, bg;
    int right;
    char *text;        /* T: the text; S: the sprite's pixels (x1 = width, y1 = rows) */
} scr_op;

static struct
{
    int on;
    const char *dir;
    struct
    {
        int made;
        unsigned flags;
        unsigned char tbg, wbg;
        int vw, vh; /* the visible area's size when made */
        char *title, title_text[13];
        int nicons;
        scr_icon icon[SCR_ICONS];
        scr_op *ops;
        int nops;
    } w[SCR_WINS];
    int cur;              /* the window being drawn (-1: none) */
    unsigned colour;
    int px, py, qx, qy;   /* OS_Plot's last point, and the one before */
    unsigned tfg, tbg;
    const scr_menu *menu;
    int menu_x, menu_y;
    int ov_x, ov_y, ov_w, ov_h; /* the overlay's place (work area) and size (pixels) */
    int shots;
} S = {.cur = -1};

static void scr_init(void)
{
    S.dir = getenv("SCREENS");
    S.on = S.dir && *S.dir;
}

static void scr_add(scr_op o)
{
    if (!S.on || S.cur < 0)
        return;
    if (!S.w[S.cur].ops)
        S.w[S.cur].ops = calloc(SCR_OPS, sizeof(scr_op));
    if (S.w[S.cur].nops < SCR_OPS)
        S.w[S.cur].ops[S.w[S.cur].nops++] = o;
}

static void scr_clear(int i)
{
    for (int k = 0; k < S.w[i].nops; k++)
        free(S.w[i].ops[k].text);
    S.w[i].nops = 0;
}

static void scr_create_window(int i, const void *block)
{
    const scr_wblock *b = block;
    if (!S.on || i < 0 || i >= SCR_WINS)
        return;
    S.w[i].made = 1;
    S.w[i].flags = (unsigned) b->flags;
    S.w[i].tbg = b->tbg;
    S.w[i].wbg = b->wbg;
    S.w[i].vw = b->x1 - b->x0;
    S.w[i].vh = b->y1 - b->y0;
    if (b->tflags & 0x100) /* (indirected: the text as it is when snapped) */
        S.w[i].title = b->title.text;
    else {
        S.w[i].title = S.w[i].title_text;
        snprintf(S.w[i].title_text, sizeof(S.w[i].title_text), "%.12s", (const char *) &b->title);
    }
    /* (a title not indirected: 12 bytes there, the icons following) */
    const char *at = (const char *) &b->title + ((b->tflags & 0x100) ? sizeof(scr_ind) : 12);
    int nicons;
    memcpy(&nicons, at, sizeof(int));
    at += sizeof(int);
    at = (const char *) (((uintptr_t) at + _Alignof(scr_icon) - 1) & ~(uintptr_t) (_Alignof(scr_icon) - 1));
    S.w[i].nicons = nicons < SCR_ICONS ? nicons : SCR_ICONS;
    memcpy(S.w[i].icon, at, sizeof(scr_icon) * S.w[i].nicons);
}

static void scr_resize_icon(int i, int icon, int x0, int y0, int x1, int y1)
{
    if (!S.on || i < 0 || i >= SCR_WINS || icon < 0 || icon >= S.w[i].nicons)
        return;
    S.w[i].icon[icon].x0 = x0; S.w[i].icon[icon].y0 = y0;
    S.w[i].icon[icon].x1 = x1; S.w[i].icon[icon].y1 = y1;
}

static void scr_set_icon_state(int i, int icon, int eor, int clear)
{
    if (!S.on || i < 0 || i >= SCR_WINS || icon < 0 || icon >= S.w[i].nicons)
        return;
    S.w[i].icon[icon].flags = (S.w[i].icon[icon].flags & ~clear) ^ eor;
}

/* An update of x0,y0 to x1,y1 (exclusive) draws all of it again: what was
   drawn inside it before goes (so a window updated over and over, as the
   Timeline's playhead is, keeps a short list) */
static void scr_cover(int i, int x0, int y0, int x1, int y1)
{
    int n = 0;
    for (int k = 0; k < S.w[i].nops; k++) {
        scr_op *p = &S.w[i].ops[k];
        int b0 = p->x0, b1 = p->y0, b2 = p->x1, b3 = p->y1, drop;
        if (p->type == 'T') {
            int len = 16 * (int) strlen(p->text);
            b0 = p->right ? p->x0 - len : p->x0; b2 = p->right ? p->x0 : p->x0 + len;
            b1 = p->y0 - 8; b3 = p->y0 + 28;
        } else if (p->type == 'P') {
            b0 = p->x0 < p->x1 ? p->x0 : p->x1; b0 = b0 < p->x2 ? b0 : p->x2;
            b2 = p->x0 > p->x1 ? p->x0 : p->x1; b2 = b2 > p->x2 ? b2 : p->x2;
            b1 = p->y0 < p->y1 ? p->y0 : p->y1; b1 = b1 < p->y2 ? b1 : p->y2;
            b3 = p->y0 > p->y1 ? p->y0 : p->y1; b3 = b3 > p->y2 ? b3 : p->y2;
        } else if (p->type == 'S') {
            b2 = p->x0 + 2 * p->x1; b3 = p->y0 + 2 * p->y1;
        }
        if (p->type == 'C') /* (a clip with nothing after it before the next) */
            drop = k + 1 < S.w[i].nops && S.w[i].ops[k + 1].type == 'C';
        else
            drop = b0 >= x0 && b2 < x1 && b1 >= y0 && b3 < y1;
        if (drop)
            free(p->text);
        else
            S.w[i].ops[n++] = *p;
    }
    S.w[i].nops = n;
}

/* A redraw (full: what was drawn before goes) or an update of window i,
   clipped to a rectangle (screen, exclusive top right) */
static void scr_begin(int i, int full, int x0, int y0, int x1, int y1)
{
    if (!S.on)
        return;
    S.cur = i >= 0 && i < SCR_WINS ? i : -1;
    if (S.cur < 0)
        return;
    if (full)
        scr_clear(i);
    else
        scr_cover(i, x0, y0, x1, y1);
    scr_add((scr_op){'C', x0, y0, x1 - 1, y1 - 1, 0, 0, 0, 0, 0, NULL});
}

static void scr_colour(unsigned bbggrr00) { S.colour = bbggrr00; }

static void scr_plot(int code, int x, int y)
{
    if ((code & ~7) == 96) { /* a rectangle fill (96-103), from the last point */
        scr_add((scr_op){'R', S.px < x ? S.px : x, S.py < y ? S.py : y, S.px < x ? x : S.px, S.py < y ? y : S.py,
                         0, 0, S.colour, 0, 0, NULL});
    } else if ((code & ~7) == 80) { /* a triangle fill (80-87): the last two points and this */
        scr_add((scr_op){'P', S.qx, S.qy, S.px, S.py, x, y, S.colour, 0, 0, NULL});
    }
    S.qx = S.px;
    S.qy = S.py;
    S.px = x;
    S.py = y;
}

static void scr_vdu(const unsigned char *b, int n)
{
    if (n == 9 && b[0] == 24) /* the graphics window */
        scr_add((scr_op){'C', (short) (b[1] | b[2] << 8), (short) (b[3] | b[4] << 8), (short) (b[5] | b[6] << 8),
                         (short) (b[7] | b[8] << 8), 0, 0, 0, 0, 0, NULL});
}

static void scr_textop(int reason, intptr_t r1, intptr_t r2, int x, int y)
{
    if ((reason & 0xFF) == 0) {
        S.tfg = (unsigned) r1;
        S.tbg = (unsigned) r2;
    } else if ((reason & 0xFF) == 2) {
        scr_add((scr_op){'T', x, y, 0, 0, 0, 0, S.tfg, S.tbg, (reason >> 31) & 1, strdup((const char *) r1)});
    }
}

/* OS_SpriteOp 52 (PutSpriteScaled) of a 32bpp sprite, bottom left at x, y */
static void scr_sprite(const int *spr, int x, int y)
{
    if (!S.on || S.cur < 0)
        return;
    int w = spr[4] + 1, rows = spr[5] + 1;
    size_t n = (size_t) w * rows * 4;
    char *px = malloc(n);
    if (!px)
        return;
    memcpy(px, (const char *) spr + spr[8], n);
    scr_add((scr_op){'S', x, y, w, rows, 0, 0, 0, 0, 0, px});
}

static void scr_menu_open(const void *menu, int x, int y)
{
    S.menu = menu;
    S.menu_x = x;
    S.menu_y = y;
}

/* The desktop as it would look now: windows (boxes as fake_box gives them,
   back to front as listed), the overlay's picture, an open menu */
static void snap(const char *name, const int order[], int n, int (*box)(int i, int b[5]),
                 const uint8_t *ov_planes, int ov_fw, int ov_fh, int ov_pitch)
{
    if (!S.on)
        return;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.txt", S.dir, name);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    for (int k = 0; k < n; k++) {
        int i = order[k], b[5];
        if (!S.w[i].made || !box(i, b))
            continue;
        /* b: x0 y0 x1 y1 (visible area, screen), scroll x */
        int ox = b[0] - b[4], oy = b[3];
        fprintf(f, "W %d %d %d %d %d %u %u 0x%X %s\n", i, b[0], b[1], b[2], b[3], S.w[i].tbg, S.w[i].wbg,
                S.w[i].flags, S.w[i].title ? S.w[i].title : "");
        for (int c = 0; c < S.w[i].nicons; c++) {
            scr_icon *ic = &S.w[i].icon[c];
            const char *t = (ic->flags & 0x100) ? (ic->d.ind.text ? ic->d.ind.text : "") : ic->d.name;
            char tx[256];
            snprintf(tx, sizeof(tx), "%.*s", (ic->flags & 0x100) ? 255 : 12, (ic->flags & 3) ? t : "");
            for (char *q = tx; *q; q++)
                if (*q == '\n' || *q == '\r')
                    *q = ' ';
            fprintf(f, "I %d %d %d %d 0x%X %s\n", ox + ic->x0, oy + ic->y0, ox + ic->x1, oy + ic->y1, ic->flags,
                    tx);
        }
        for (int o = 0; o < S.w[i].nops; o++) {
            scr_op *p = &S.w[i].ops[o];
            switch (p->type) {
            case 'C': fprintf(f, "C %d %d %d %d\n", p->x0, p->y0, p->x1, p->y1); break;
            case 'R': fprintf(f, "R %d %d %d %d 0x%08X\n", p->x0, p->y0, p->x1, p->y1, p->colour); break;
            case 'P':
                fprintf(f, "P %d %d %d %d %d %d 0x%08X\n", p->x0, p->y0, p->x1, p->y1, p->x2, p->y2, p->colour);
                break;
            case 'T': fprintf(f, "T %d %d 0x%08X 0x%08X %d %s\n", p->x0, p->y0, p->colour, p->bg, p->right, p->text); break;
            case 'S': {
                char sp[512];
                snprintf(sp, sizeof(sp), "%s/%s-s%d-%d.raw", S.dir, name, i, o);
                FILE *g = fopen(sp, "wb");
                if (g) {
                    fwrite(p->text, 4, (size_t) p->x1 * p->y1, g);
                    fclose(g);
                    fprintf(f, "S %d %d %d %d %s\n", p->x0, p->y0, p->x1, p->y1, sp);
                }
                break;
            }
            }
        }
        fprintf(f, "E\n");
        /* the overlay: over everything in the Viewer's picture area */
        if (i == 0 && ov_planes && S.ov_w > 0) {
            char op[512];
            snprintf(op, sizeof(op), "%s/%s-ov.yuv", S.dir, name);
            FILE *g = fopen(op, "wb");
            if (g) {
                fwrite(ov_planes, 1, (size_t) ov_pitch * ov_fh * 3 / 2, g);
                fclose(g);
                fprintf(f, "O %d %d %d %d %d %d %d %s\n", ox + S.ov_x, oy + S.ov_y, S.ov_w, S.ov_h, ov_fw, ov_fh,
                        ov_pitch, op);
            }
        }
    }
    if (S.menu) {
        int nitems = 0;
        while (nitems < 32 && !(S.menu->item[nitems].flags & 0x80))
            nitems++;
        nitems++;
        fprintf(f, "M %d %d %d %d %.12s\n", S.menu_x, S.menu_y, S.menu->w, S.menu->h, S.menu->title);
        for (int k = 0; k < nitems; k++)
            fprintf(f, "MI 0x%X 0x%X %d %s\n", S.menu->item[k].flags, S.menu->item[k].iflags, S.menu->item[k].sub != -1,
                    S.menu->item[k].text ? S.menu->item[k].text : "");
    }
    fclose(f);
    S.shots++;
}

#endif
