/* editor_test - !ReelEdit's editor (src/riscos: viewer.c, timeline.c,
 * savebox.c, display.c, sound.c) driven by a scripted fake Wimp, with the
 * real MLT and FFmpeg, and fake VideoOverlay, SharedSoundBuffer and clock.
 *
 *   editor_test numbered-snd.mp4 numbered.mp4
 * numbered-snd.mp4: 320x180, 25 fps, 75 frames, frame n a grey of luma
 * 16 + 2n, a 440 Hz tone at 44.1 kHz; numbered.mp4 the same, silent.
 *
 * The script:
 *   - a directory dropped on the icon is refused; Select on the icon opens
 *     the editor (the Viewer and the Timeline under it) on an empty
 *     project, with no null events;
 *   - a clip dropped on the Timeline goes in, the Viewer shows its first
 *     frame (a sprite), the title says <Untitled> *, and the Timeline
 *     draws it with its name;
 *   - playing: at the clock through the overlay, Wimp_PollIdle between
 *     pictures, the sound 0.4 s ahead; a window over the picture hides the
 *     overlay; Space stops (drawn in the window, the sound cleared); L, J,
 *     K, Escape, Home, End; Select on the icon brings the Viewer to the
 *     front; playing to the end stops there;
 *   - editing: a second clip dropped on the Timeline near the start of the
 *     first goes before it; a click
 *     on the ruler moves the playhead; S splits there; a click on a clip
 *     selects it (drawn in orange) and Delete removes it, the rest closing
 *     up; View > Zoom in (the menu) widens the Timeline;
 *   - saving: F3 opens the save box (never saved); a full name typed and
 *     Return saves there (MLT XML, filetype &F80), the title the name
 *     without a star; the file icon dragged to a directory display: the
 *     Message_DataSave / DataSaveAck / DataLoad exchange, saved there;
 *   - closing with changes asks: Cancel keeps it open, Discard closes it
 *     (the engine and the overlay with it);
 *   - the project saved by the drag (type XML, no extension) dropped on the
 *     icon opens again, the same; changed, then the desktop shutting down
 *     (Message_PreQuit): asked, Discard, the shutdown started again
 *     (Ctrl-Shift-F12 to the task that began it), quit.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include <kernel.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "project.h"

int reeledit_main(int argc, char **argv);
int re_test_position(void);
double re_test_speed(void);
int re_test_open(void);
re_project *re_test_project(void);
int re_test_selected(void);
int re_test_snap(void);
int re_test_measuring(void);
int re_test_busy(void);
const char *re_test_title(void);
char *re_test_savebox_name(void);

static int fails;
static int phase, phase_cs;
#define CHECK(c, ...)                                                                                              \
    do {                                                                                                           \
        if (!(c)) {                                                                                                \
            fails++;                                                                                               \
            printf("FAIL (phase %d): ", phase);                                                                    \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
        }                                                                                                          \
    } while (0)

static const char *clip, *clip2, *clip3;
static char typed_path[300], dragged_path[300];

/* ---- the fake machine ------------------------------------------------------- */

#define VIEWER 0x1000 /* the windows, in the order the program makes them */
#define TL     0x1100
#define INFO   0x1200 /* (made before the save box in this script) */
#define SAVEB  0x1300
#define COVER  0x2000
#define FILER  0x3000 /* a directory display */
#define TASK   0x4321
#define BAR_ICON 7
#define VO_BASE 0x5A000
static const char *const vo_names[] = {
    "VideoOverlay_Create", "VideoOverlay_Destroy", "VideoOverlay_DisplayBuffer", "VideoOverlay_MapBuffer",
    "VideoOverlay_UnmapBuffer", "VideoOverlay_SetScale", "VideoOverlay_SetWindow", "VideoOverlay_SetPosition",
    "VideoOverlay_RedrawWindow"};

typedef struct
{
    int open, box[4], scroll_x, behind, redraw, extent_w;
} fwin;
static fwin W[4]; /* VIEWER, TL, INFO, SAVEB */
static int windows_made;
static fwin *win(int h)
{
    int i = (h - VIEWER) / 0x100;
    return h >= VIEWER && i < 4 && (h - VIEWER) % 0x100 == 0 ? &W[i] : NULL;
}

static unsigned fake_cs = 1000;
static unsigned slept;
static int polls, idle_polls, null_events, last_mask;
static int covered, shift_key, ctrl_key;
static char help_text[256];
static int whole_redraws;
static int plots, reports, questions, answer = 3, closedown, acks, menus, savebox_opens, drags;
static int filetypes_set, dataloads_sent, plots_tl, prequit_acks, key_sent;
static char last_text[128], filetype_path[300], all_text[16384];
static unsigned colours_seen; /* bit 0: the selected clip's orange set */
static int ptr[5];            /* Wimp_GetPointerInfo */
static int datasave_ref, ref_next = 900;
static char datasave_leaf[64];
static int *plot_area;
static int ov_made, ov_destroyed, ov_shown_bank = -1, ov_displays, ov_w, ov_h;
static uint8_t *ov_buf[3];
static int ov_block[6];
static int ssb_closes, ssb_playing;
static unsigned ssb_bytes, ssb_added, ssb_played, ssb_play_cs;
static double ssb_level;
static _kernel_oserror err = {1, "fake: not here"};

static void fail_quit(const char *why)
{
    printf("FAIL (phase %d): %s\n", phase, why);
    exit(1);
}

static void window_state(int *b)
{
    int h = b[0];
    fwin *w = win(h);
    if (w) {
        b[1] = w->box[0]; b[2] = w->box[1]; b[3] = w->box[2]; b[4] = w->box[3];
        b[5] = w->scroll_x; b[6] = 0;
        b[7] = h == VIEWER && covered ? COVER : -1;
        b[8] = w->open ? 1 << 16 : 0;
    } else if (h == COVER) { /* over the middle of the Viewer's picture */
        int cx = (W[0].box[0] + W[0].box[2]) / 2, cy = (W[0].box[1] + W[0].box[3]) / 2;
        b[1] = cx - 100; b[2] = cy - 100; b[3] = cx + 100; b[4] = cy + 100;
        b[5] = b[6] = 0;
        b[7] = -1;
        b[8] = 1 << 16;
    }
}

/* one rectangle: the window's visible area (redraw) or the box asked for (update) */
static void rect_block(int *b, int update)
{
    fwin *w = win(b[0]);
    if (!w)
        fail_quit("a redraw of a window that isn't ours");
    int ox = w->box[0] - w->scroll_x, oy = w->box[3];
    int c[4] = {w->box[0], w->box[1], w->box[2], w->box[3]};
    if (update) {
        c[0] = ox + b[1]; c[1] = oy + b[2]; c[2] = ox + b[3]; c[3] = oy + b[4];
    }
    b[1] = w->box[0]; b[2] = w->box[1]; b[3] = w->box[2]; b[4] = w->box[3];
    b[5] = w->scroll_x; b[6] = 0;
    b[7] = c[0]; b[8] = c[1]; b[9] = c[2]; b[10] = c[3];
}

static unsigned ssb_heard(void)
{
    unsigned p = ssb_played;
    if (ssb_playing)
        p += (unsigned) ((fake_cs - ssb_play_cs) * 1920) / 8192 * 8192;
    return p < ssb_added ? p : ssb_added;
}

static int script(int *b, int mask);

_kernel_oserror *_kernel_swi(int no, _kernel_swi_regs *in, _kernel_swi_regs *out)
{
    _kernel_swi_regs *r = out;
    if (in != out)
        *out = *in;
    no &= ~0x20000;
    if (no >= VO_BASE && no < VO_BASE + 9) {
        switch (no - VO_BASE) {
        case 0:
            ov_made++;
            ov_w = ((int *) r->r[0])[1];
            ov_h = ((int *) r->r[0])[2];
            for (int i = 0; i < 3; i++) {
                if (ov_buf[i])
                    munmap(ov_buf[i], 8 << 20);
                ov_buf[i] = mmap(NULL, 8 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT,
                                 -1, 0);
            }
            r->r[0] = 77; r->r[1] = 1; r->r[2] = 16; r->r[3] = 16; r->r[4] = 4096; r->r[5] = 4096;
            break;
        case 1: ov_destroyed++; ov_shown_bank = -1; break;
        case 2: ov_displays++; ov_shown_bank = (int) r->r[1]; break;
        case 3: {
            int bank = (int) r->r[1], pitch = (ov_w + 31) & ~31;
            uint8_t *p = ov_buf[bank];
            ov_block[0] = (int) (intptr_t) p; ov_block[1] = pitch;
            ov_block[2] = (int) (intptr_t) (p + pitch * ov_h); ov_block[3] = pitch / 2;
            ov_block[4] = (int) (intptr_t) (p + pitch * ov_h * 5 / 4); ov_block[5] = pitch / 2;
            r->r[0] = (intptr_t) ov_block;
            break;
        }
        default: break;
        }
        return NULL;
    }
    switch (no) {
    case 0x06: /* OS_Byte: 176 the vsync counter (50 Hz), 121 a key down (Shift) */
        if (r->r[0] == 176)
            r->r[1] = (fake_cs / 2) & 0xFF;
        else if (r->r[0] == 121)
            r->r[1] = ((r->r[1] & 0x7F) == 0 ? shift_key : (r->r[1] & 0x7F) == 1 ? ctrl_key : 0) ? 0xFF : 0;
        break;
    case 0x08: /* OS_File 18: a filetype set */
        if (r->r[0] == 18) {
            filetypes_set++;
            CHECK(r->r[2] == 0xF80, "filetype &%X set, not &F80", (int) r->r[2]);
            snprintf(filetype_path, sizeof(filetype_path), "%s", (const char *) r->r[1]);
        }
        break;
    case 0x2E: plots++; plot_area = (int *) r->r[1]; break;
    case 0x35: {
        static const int v[13] = {0, 0, 0, 0, 1, 1, 0, 0, 0, 5, 0, 1919, 1079};
        r->r[2] = r->r[1] < 13 ? v[r->r[1]] : 0;
        break;
    }
    case 0x39: {
        const char *n = (const char *) r->r[1];
        for (int i = 0; i < 9; i++)
            if (!strcmp(n, vo_names[i])) {
                r->r[0] = VO_BASE + i;
                return NULL;
            }
        if (!strcmp(n, "SharedSoundBuffer_OpenStream") || !strcmp(n, "StreamManager_AddBlock")) {
            r->r[0] = 1;
            return NULL;
        }
        return &err;
    }
    case 0x42: r->r[0] = fake_cs; break;
    case 0x45: plots_tl++; break;                 /* OS_Plot */
    case 0x46: case 0x65: break;
    case 0x40743:                                  /* ColourTrans_SetGCOL */
        if ((unsigned) r->r[0] == 0x2080F000u)
            colours_seen |= 1;
        break;
    case 0x55FC0: r->r[0] = 5; ssb_playing = 0; ssb_added = ssb_played = 0; break;
    case 0x55FC1: ssb_closes++; ssb_playing = 0; break;
    case 0x55FC9:
        if ((r->r[1] & 1) && !ssb_playing)
            ssb_play_cs = fake_cs;
        else if (!(r->r[1] & 1) && ssb_playing)
            ssb_played = ssb_heard();
        ssb_playing = r->r[1] & 1;
        break;
    case 0x55FCE: r->r[0] = 9; break;
    case 0x55FC5: case 0x57287: break;
    case 0x57282: {
        const int16_t *smp = (const int16_t *) r->r[1];
        for (int i = 0; i < (int) r->r[2] / 2; i++)
            if (abs(smp[i]) > ssb_level)
                ssb_level = abs(smp[i]);
        ssb_bytes += (unsigned) r->r[2];
        ssb_added += (unsigned) r->r[2];
        break;
    }
    case 0x57288: r->r[0] = ssb_added; r->r[1] = ssb_heard(); break;
    case 0x400C0: r->r[0] = 380; r->r[1] = TASK; break;
    case 0x400C1:                                                   /* CreateWindow */
        if (windows_made >= 4)
            fail_quit("more than four windows made");
        r->r[0] = VIEWER + 0x100 * windows_made++;
        break;
    case 0x400C2: r->r[0] = BAR_ICON; break;
    case 0x400C5: {                                                 /* OpenWindow */
        int *b = (int *) r->r[1];
        fwin *w = win(b[0]);
        if (!w)
            break;
        w->box[0] = b[1]; w->box[1] = b[2]; w->box[2] = b[3]; w->box[3] = b[4];
        w->scroll_x = b[5];
        w->behind = b[7];
        w->open = 1;
        w->redraw = 1;
        break;
    }
    case 0x400C6: {                                                 /* CloseWindow */
        fwin *w = win(*(int *) r->r[1]);
        if (w)
            w->open = 0;
        if (w == &W[0])
            plot_area = NULL; /* (its sprite goes) */
        break;
    }
    case 0x400C7: case 0x400E1: {
        int *b = (int *) r->r[1], ev;
        last_mask = (int) r->r[0];
        if (++polls > 40000)
            fail_quit("too many polls: the script is stuck");
        for (int i = 0; i < 4; i++)
            if (W[i].redraw && W[i].open && i != 3) { /* (the save box is drawn by the Wimp) */
                W[i].redraw = 0;
                b[0] = VIEWER + 0x100 * i;
                r->r[0] = 1;
                fake_cs++;
                return NULL;
            }
        ev = script(b, last_mask);
        if (ev == 0) {
            if (last_mask & 1)
                fail_quit("the script wanted a null event, but they're masked out");
            null_events++;
            if (no == 0x400E1) {
                idle_polls++;
                if ((int) (r->r[2] - fake_cs) > 0) {
                    slept += r->r[2] - fake_cs;
                    fake_cs = (unsigned) r->r[2];
                }
            }
        }
        fake_cs++;
        r->r[0] = ev;
        break;
    }
    case 0x400C8: rect_block((int *) r->r[1], 0); r->r[0] = 1; break; /* RedrawWindow */
    case 0x400C9: rect_block((int *) r->r[1], 1); r->r[0] = 1; break; /* UpdateWindow */
    case 0x400CA: r->r[0] = 0; break;
    case 0x400CB: window_state((int *) r->r[1]); break;
    case 0x400CF: memcpy((int *) r->r[1], ptr, sizeof(ptr)); break; /* GetPointerInfo */
    case 0x400D0: drags++; break;                                   /* DragBox */
    case 0x400D1:                                                   /* ForceRedraw */
        if (r->r[1] != 0x4B534154 && win((int) r->r[0])) {
            win((int) r->r[0])->redraw = 1;
            if (r->r[0] == VIEWER) {
                fwin *w = &W[0];
                if (r->r[1] <= 0 && r->r[2] <= -(w->box[3] - w->box[1]) && r->r[3] >= w->box[2] - w->box[0] &&
                    r->r[4] >= 0)
                    whole_redraws++;
            }
        }
        break;
    case 0x400D4:                                                   /* CreateMenu */
        if (r->r[1] == SAVEB) {
            savebox_opens++;
            W[3].open = 1;
        } else if (r->r[1] == -1) {
            W[3].open = 0;
        } else {
            menus++;
        }
        break;
    case 0x400D7: {                                                 /* SetExtent */
        fwin *w = win((int) r->r[0]);
        int *b = (int *) r->r[1];
        if (w)
            w->extent_w = b[2] - b[0];
        break;
    }
    case 0x400CD: case 0x400D2: case 0x400DC: case 0x400FC: break;
    case 0x400F9:                                                   /* Wimp_TextOp: 2 plots */
        if ((r->r[0] & 0xFF) == 2) {
            snprintf(last_text, sizeof(last_text), "%s", (const char *) r->r[1]);
            if (strlen(all_text) + strlen(last_text) + 2 < sizeof(all_text)) {
                strcat(all_text, last_text);
                strcat(all_text, "|");
            }
        }
        break;
    case 0x400E0: {                                                 /* GetWindowOutline: a 40 title bar */
        int *b = (int *) r->r[1];
        fwin *w = win(b[0]);
        if (w) {
            b[1] = w->box[0]; b[2] = w->box[1] - (b[0] == TL ? 40 : 0); b[3] = w->box[2]; b[4] = w->box[3] + 40;
        }
        break;
    }
    case 0x400DD: closedown++; break;
    case 0x400DF:                                                   /* ReportError */
        if (r->r[5]) {
            questions++;
            r->r[1] = answer;
        } else {
            reports++;
            r->r[1] = 1;
        }
        break;
    case 0x400D3: memset((int *) r->r[1], 0, 24); break;            /* GetCaretPosition */
    case 0x400E7: {                                                 /* SendMessage */
        int *b = (int *) r->r[1];
        if (r->r[0] == 19) {
            prequit_acks++;
            break;
        }
        if (r->r[0] == 8) {
            key_sent = b[6];
            break;
        }
        b[2] = ++ref_next;
        if (b[4] == 0x503)
            snprintf(help_text, sizeof(help_text), "%s", (char *) &b[5]);
        else if (b[4] == 4)
            acks++;
        else if (b[4] == 1) {
            datasave_ref = b[2];
            snprintf(datasave_leaf, sizeof(datasave_leaf), "%s", (char *) &b[11]);
        } else if (b[4] == 3)
            dataloads_sent++;
        break;
    }
    default:
        printf("(fake: SWI &%X not handled)\n", no);
        break;
    }
    return NULL;
}

/* ---- what's on show ----------------------------------------------------------- */

static int sprite_middle(void)
{
    if (!plot_area)
        return -1;
    int *spr = plot_area + 4, w = spr[4] + 1;
    int ph = (W[0].box[3] - W[0].box[1] - 128) >> 1;
    uint8_t *px = (uint8_t *) spr + spr[8];
    return px[((size_t) (ph / 2) * w + w / 2) * 4];
}

static int overlay_middle(void)
{
    if (ov_shown_bank < 0)
        return -1;
    int pitch = (ov_w + 31) & ~31;
    return ov_buf[ov_shown_bank][(size_t) (ov_h / 2) * pitch + ov_w / 2];
}

static int grey(int frame)
{
    return (int) ((2.0 * frame) * 255 / 219 + 0.5);
}

static const char *layout(void)
{
    static char s[256];
    re_project *p = re_test_project();
    s[0] = 0;
    for (int i = 0; p && i < re_project_items(p); i++) {
        re_item it;
        re_project_item(p, i, &it);
        snprintf(s + strlen(s), sizeof(s) - strlen(s), "%s%c%d", i ? " " : "", it.gap ? 'g' : 'c', it.length);
    }
    return s;
}

/* ---- the script ----------------------------------------------------------------- */

static void data_load(int *b, int w, int x, int type, const char *file)
{
    memset(b, 0, 256);
    b[1] = 0x999; b[2] = 55; b[3] = 0; b[4] = 3;
    b[5] = w; b[6] = w == -2 ? BAR_ICON : -1;
    b[7] = x; b[8] = 0;
    b[10] = type;
    snprintf((char *) &b[11], 200, "%.199s", file);
    b[0] = (44 + (int) strlen(file) + 1 + 3) & ~3;
}

static int key_to(int *b, int w, int k)
{
    memset(b, 0, 28);
    b[0] = w;
    b[6] = k;
    return 8;
}
static int key(int *b, int k) { return key_to(b, VIEWER, k); }

static int click_icon(int *b, int icon)
{
    b[0] = W[0].box[0] + 10; b[1] = W[0].box[1] + 10; b[2] = 4; b[3] = VIEWER; b[4] = icon;
    return 6;
}

static double tl_scale = 4; /* the Timeline's OS units a frame, as it should be */

/* a click (Select: 0x400 for this window's button type) on the Timeline at a frame: the ruler or the track */
static int click_tl(int *b, int frame, int on_ruler, int buttons)
{
    b[0] = W[1].box[0] - W[1].scroll_x + 16 + (int) (frame * tl_scale);
    b[1] = on_ruler ? W[1].box[3] - 10 : W[1].box[3] - 48 - 8 - 48;
    b[2] = buttons; b[3] = TL; b[4] = -1;
    return 6;
}

static void next(int p)
{
    phase = p;
    phase_cs = fake_cs;
}

static char saved_layout[256];
static int plots_mark, ovd_mark, ssb_mark, closes_mark, pos_mark, pos_min = 1000, mark, scroll_mark;

static int script(int *b, int mask)
{
    int nulls = !(mask & 1), pos = re_test_position();
    switch (phase) {
    case 0: /* a directory: refused */
        data_load(b, -2, 0, 0x1000, "/tmp");
        next(1);
        return 17;
    case 1: /* Select on the icon: the editor, empty */
        CHECK(reports == 1, "a directory dropped wasn't refused");
        next(2);
        b[0] = 100; b[1] = 20; b[2] = 4; b[3] = -2; b[4] = BAR_ICON;
        return 6;
    case 2:
        CHECK(W[0].open && W[1].open, "the editor's windows aren't open (%d, %d)", W[0].open, W[1].open);
        CHECK(re_test_project() && re_project_items(re_test_project()) == 0, "not an empty project");
        CHECK(!re_test_open(), "an engine with nothing to play");
        CHECK(!strcmp(re_test_title(), "<Untitled>"), "title \"%s\"", re_test_title());
        CHECK(!nulls, "null events with nothing to play");
        CHECK(W[1].box[3] + 40 == W[0].box[1] && W[1].box[0] == W[0].box[0] && W[1].box[2] == W[0].box[2],
              "the Timeline isn't attached under the Viewer (%d,%d,%d,%d under %d,%d,%d,%d)", W[1].box[0], W[1].box[1],
              W[1].box[2], W[1].box[3], W[0].box[0], W[0].box[1], W[0].box[2], W[0].box[3]);
        next(60);
        /* the user drags the Timeline: the Viewer comes with it */
        b[0] = TL; b[1] = W[1].box[0] + 100; b[2] = W[1].box[1] - 50; b[3] = W[1].box[2] + 100; b[4] = W[1].box[3] - 50;
        b[5] = 0; b[6] = 0; b[7] = -1;
        mark = W[0].box[0];
        return 2;
    case 60:
        CHECK(W[0].box[0] == mark + 100 && W[1].box[3] + 40 == W[0].box[1] && W[1].box[0] == W[0].box[0],
              "dragging the Timeline didn't bring the Viewer (Viewer x %d, was %d)", W[0].box[0], mark);
        next(3);
        data_load(b, TL, W[1].box[0] + 100, 0xFFD, clip);
        return 17;
    case 3: /* the clip: in, its first frame drawn */
        CHECK(acks == 1, "the clip's DataLoad wasn't acknowledged (%d)", acks);
        if (plots > 0 && pos == 0) {
            CHECK(!strcmp(layout(), "c75"), "items %s", layout());
            CHECK(abs(sprite_middle() - grey(0)) <= 2, "frame 0 drawn as %d, not %d", sprite_middle(), grey(0));
            CHECK(ov_displays == 0, "the overlay was used while stopped");
            CHECK(!strcmp(re_test_title(), "<Untitled> *"), "title \"%s\"", re_test_title());
            next(4);
            return script(b, mask);
        }
        if (!nulls)
            fail_quit("no null events before the first picture");
        if (fake_cs - phase_cs > 200)
            fail_quit("no picture after 2 s");
        return 0;
    case 4: /* the Timeline drew it, with its name; stopped: no nulls */
        if (nulls) {
            if (fake_cs - phase_cs > 5)
                CHECK(0, "null events still asked for while stopped");
            return 0;
        }
        CHECK(plots_tl > 0 && strstr(all_text, "numbered") != NULL, "the Timeline didn't draw the clip (\"%s\")",
              all_text);
        CHECK(strstr(all_text, "0:00:00") != NULL, "no timecode beside the playhead (\"%s\")", all_text);
        plots_mark = plots;
        next(5);
        return key(b, 0x18D);
    case 5:
        if (plots > plots_mark && pos == 1 && (mask & 1)) {
            CHECK(abs(sprite_middle() - grey(1)) <= 2, "frame 1 drawn as %d, not %d", sprite_middle(), grey(1));
            next(6);
            ssb_mark = ssb_bytes;
            return click_icon(b, 3); /* Play */
        }
        if (fake_cs - phase_cs > 100)
            fail_quit("Right didn't draw frame 1");
        return nulls ? 0 : key(b, 0x18D);
    case 6: /* playing for 1 s */
        if (fake_cs - phase_cs >= 100) {
            CHECK(re_test_speed() == 1, "not playing (%g)", re_test_speed());
            CHECK(pos >= 24 && pos <= 28, "after 1 s at frame %d, not about 26", pos);
            CHECK(ov_made == 1 && ov_shown_bank >= 0, "the overlay isn't showing");
            CHECK(ov_displays >= 20, "%d pictures through the overlay in 1 s", ov_displays);
            CHECK(abs(overlay_middle() - (16 + 2 * pos)) <= 2, "the overlay shows luma %d at frame %d",
                  overlay_middle(), pos);
            CHECK(idle_polls >= 20, "Wimp_PollIdle used only %d times", idle_polls);
            CHECK(slept > 50, "asleep only %u cs of 100 while playing", slept);
            CHECK(ssb_bytes > (unsigned) ssb_mark && ssb_playing, "no sound given, or not playing");
            CHECK(ssb_level > 1000, "the sound given is silent (loudest %g)", ssb_level);
            covered = 1;
            plots_mark = plots;
            next(7);
        }
        return nulls ? 0 : (fail_quit("stopped playing by itself"), 0);
    case 7: /* a window over the picture: drawn in the window */
        if (fake_cs - phase_cs >= 40) {
            CHECK(ov_shown_bank < 0, "the overlay still shows with a window over it");
            CHECK(plots > plots_mark + 5, "not drawn in the window while covered (%d)", plots - plots_mark);
            covered = 0;
            next(8);
        }
        return nulls ? 0 : (fail_quit("stopped playing by itself"), 0);
    case 8: /* uncovered: the overlay again */
        if (fake_cs - phase_cs >= 30) {
            CHECK(ov_shown_bank >= 0, "the overlay didn't come back");
            double ahead = (ssb_bytes - ssb_mark) / (1920.0 * 4) - (pos - 1);
            CHECK(ahead > 5 && ahead < 14, "the sound is %.1f frames ahead of the picture, not about 10", ahead);
            closes_mark = ssb_closes;
            plots_mark = plots;
            next(9);
            return key(b, ' ');
        }
        return 0;
    case 9: /* stopped */
        if (nulls) {
            if (fake_cs - phase_cs > 20)
                fail_quit("still asking for null events after stopping");
            return 0;
        }
        CHECK(re_test_speed() == 0, "Space didn't stop it");
        CHECK(ov_shown_bank < 0, "the overlay shows while stopped");
        CHECK(plots > plots_mark && abs(sprite_middle() - grey(pos)) <= 2,
              "the stopped frame (%d) isn't drawn in the window (%d)", pos, sprite_middle());
        CHECK(ssb_closes > closes_mark && !ssb_playing, "the sound wasn't stopped and cleared");
        next(10);
        return key(b, ' ');
    case 10:
        next(11);
        return key(b, 'L');
    case 11:
        CHECK(re_test_speed() == 2, "Space then L: speed %g, not 2", re_test_speed());
        next(12);
        return key(b, 'k');
    case 12:
        CHECK(re_test_speed() == 0, "K didn't stop it");
        if (nulls)
            return 0;
        pos_mark = pos;
        next(13);
        return key(b, 'j');
    case 13:
        CHECK(re_test_speed() == -1, "J: speed %g, not -1", re_test_speed());
        if (fake_cs - phase_cs >= 80) {
            CHECK(pos < pos_mark - 2, "J: from %d to %d, not backwards", pos_mark, pos);
            next(14);
            return key(b, 27); /* Escape */
        }
        return 0;
    case 14:
        CHECK(re_test_speed() == 0, "Escape didn't stop it");
        if (nulls)
            return 0;
        next(15);
        return key_to(b, TL, 0x1E); /* Home, the caret in the Timeline */
    case 15:
        if (nulls)
            return 0;
        CHECK(pos == 0, "Home: frame %d, not 0", pos);
        covered = 1;
        next(16);
        b[0] = 100; b[1] = 20; b[2] = 4; b[3] = -2; b[4] = BAR_ICON;
        return 6;
    case 16:
        CHECK(W[0].open && W[0].behind == -1, "Select on the icon didn't bring the Viewer to the front");
        covered = 0;
        if (nulls)
            return 0;
        next(17);
        return key(b, 0x18B); /* End */
    case 17:
        if (nulls)
            return 0;
        CHECK(pos == 74, "End: frame %d, not 74", pos);
        next(18);
        return key(b, ' ');
    case 18: /* from the start to the end, stopped there */
        if (pos >= 0 && pos < pos_min)
            pos_min = pos;
        if (nulls) {
            if (fake_cs - phase_cs > 500)
                fail_quit("didn't stop at the end");
            return 0;
        }
        CHECK(pos_min < 5, "Play at the end didn't start from the beginning (from %d)", pos_min);
        CHECK(pos == 74 && re_test_speed() == 0, "stopped at %d (speed %g), not 74", pos, re_test_speed());
        CHECK(ov_shown_bank < 0, "the overlay shows after the end");
        next(19);
        /* the second clip, dropped on the Timeline at frame 10: before the first */
        data_load(b, TL, W[1].box[0] - W[1].scroll_x + 16 + 10 * 4, 0xFFD, clip2);
        return 17;
    /* ---- editing ---- */
    case 19:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c75 c75"), "after the second clip: %s", layout());
        CHECK(re_test_selected() == 0, "the new clip (dropped at frame 10) isn't first (%d)", re_test_selected());
        CHECK(pos == 0, "not at the new clip's start (%d)", pos);
        next(20);
        return click_tl(b, 30, 1, 0x400); /* the ruler at frame 30 */
    case 20:
        if (nulls)
            return 0;
        CHECK(pos == 30, "a click on the ruler at frame 30: at %d", pos);
        CHECK(abs(sprite_middle() - grey(30)) <= 2, "frame 30 drawn as %d", sprite_middle());
        next(61);
        return click_tl(b, 30, 1, 0x40); /* a drag on the ruler: the playhead follows the pointer */
    case 61:
        CHECK(drags == 1, "dragging on the ruler didn't start a drag");
        ptr[0] = W[1].box[0] - W[1].scroll_x + 16 + 60 * 4; ptr[1] = W[1].box[3] - 10;
        next(62);
        return script(b, mask);
    case 62:
        if (pos == 60 && !re_test_busy()) {
            CHECK(nulls, "no null events while dragging the playhead");
            next(63);
            return 7; /* the drag ends */
        }
        if (fake_cs - phase_cs > 100)
            fail_quit("the playhead didn't follow the pointer to frame 60");
        return nulls ? 0 : (fail_quit("null events masked while dragging the playhead"), 0);
    case 63:
        if (nulls)
            return 0;
        CHECK(pos == 60 && abs(sprite_middle() - grey(60)) <= 2, "after the drag: at %d, drawn as %d", pos,
              sprite_middle());
        next(64);
        return click_tl(b, 30, 1, 0x400); /* back to 30 */
    case 64:
        if (nulls)
            return 0;
        CHECK(pos == 30, "back to 30: %d", pos);
        next(21);
        return key_to(b, TL, 's');
    case 21:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 c45 c75"), "S at 30: %s", layout());
        colours_seen = 0;
        next(22);
        return click_tl(b, 40, 0, 0x400); /* the second piece */
    case 22:
        CHECK(re_test_selected() == 1, "a click on the second piece selected %d", re_test_selected());
        CHECK(colours_seen & 1, "the selected clip isn't drawn orange"); /* (the redraw came first) */
        shift_key = 0;
        next(23);
        return key_to(b, TL, 0x7F); /* Delete */
    case 23:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 c75"), "Delete: %s", layout());
        CHECK(re_test_selected() == -1, "still selected after Delete");
        CHECK(pos == 30, "after Delete: at %d, not 30", pos);
        plots_mark = plots;
        next(70);
        return key_to(b, TL, 0x188); /* F8: undo */
    /* ---- undo, redo, cuts, trims, snapping, help, the wheel ---- */
    case 70:
        if (nulls)
            return 0;
        if (plots == plots_mark) { /* (the picture made again: the old sprite's gone) */
            if (fake_cs - phase_cs > 100)
                fail_quit("after undo, the Viewer isn't drawn");
            return 0;
        }
        CHECK(!strcmp(layout(), "c30 c45 c75") && pos == 30, "F8 (undo the Delete): %s, at %d", layout(), pos);
        CHECK(abs(sprite_middle() - grey(30)) <= 2, "after undo, frame 30 drawn as %d", sprite_middle());
        next(71);
        return key_to(b, TL, 0x189); /* F9: redo */
    case 71:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 c75") && pos == 30, "F9 (redo the Delete): %s, at %d", layout(), pos);
        next(72);
        return key(b, 0x18F); /* Up: the cut before */
    case 72:
        if (nulls)
            return 0;
        CHECK(pos == 0, "Up from 30: at %d, not 0", pos);
        next(73);
        return key(b, 0x18E); /* Down */
    case 73:
        if (nulls)
            return 0;
        CHECK(pos == 30, "Down from 0: at %d, not 30", pos);
        next(74);
        return key(b, 0x18E);
    case 74:
        if (nulls)
            return 0;
        CHECK(pos == 104, "Down from 30: at %d, not the last frame (104)", pos);
        next(75);
        return click_tl(b, 40, 1, 0x400);
    case 75:
        if (nulls)
            return 0;
        CHECK(pos == 40, "the ruler at 40: %d", pos);
        next(76);
        return key_to(b, TL, 'q'); /* Q: the start trimmed to the playhead */
    case 76:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 c65") && pos == 30, "Q at 40: %s, at %d", layout(), pos);
        next(77);
        return key_to(b, TL, 0x188);
    case 77:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 c75"), "undo Q: %s", layout());
        next(78);
        return click_tl(b, 40, 1, 0x400);
    case 78:
        if (nulls)
            return 0;
        next(79);
        return key_to(b, TL, 'W'); /* W: the end trimmed to the playhead */
    case 79:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 c10") && (pos == 39 || pos == 40), "W at 40: %s, at %d", layout(), pos);
        next(80);
        return key_to(b, TL, 0x188);
    case 80:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 c75"), "undo W: %s", layout());
        CHECK(re_test_snap() == 1, "snapping not on to start with");
        next(81);
        return click_tl(b, 32, 1, 0x400); /* 8 OS units from the cut at 30 */
    case 81:
        if (nulls)
            return 0;
        CHECK(pos == 30, "snapping: the ruler at 32 gave %d, not the cut at 30", pos);
        next(82);
        return key_to(b, TL, 'n');
    case 82:
        CHECK(re_test_snap() == 0, "N didn't turn snapping off");
        next(83);
        return click_tl(b, 32, 1, 0x400);
    case 83:
        if (nulls)
            return 0;
        CHECK(pos == 32, "not snapping: the ruler at 32 gave %d", pos);
        next(84);
        return click_icon(b, 16); /* the Snap button */
    case 84:
        CHECK(re_test_snap() == 1, "the Snap button didn't turn snapping on");
        help_text[0] = 0;
        memset(b, 0, 256);
        b[0] = 40; b[1] = 0x555; b[2] = 55; b[4] = 0x502; b[8] = VIEWER; b[9] = 9; /* help on Split */
        next(85);
        return 17;
    case 85:
        CHECK(strstr(help_text, "split") != NULL, "help on Split: \"%s\"", help_text);
        help_text[0] = 0;
        b[8] = TL; b[9] = -1;
        b[0] = 40; b[1] = 0x555; b[2] = 56; b[4] = 0x502;
        next(86);
        return 17;
    case 86:
        CHECK(strstr(help_text, "Timeline") != NULL, "help on the Timeline: \"%s\"", help_text);
        mark = W[1].extent_w;
        ctrl_key = 1;
        ptr[0] = W[1].box[0] + 100; ptr[1] = W[1].box[3] - 60; ptr[2] = 0; ptr[3] = TL; ptr[4] = -1;
        next(87);
        goto wheel_up;
    case 87:
        next(88);
        goto wheel_up;
    case 88:
        CHECK(W[1].extent_w > mark, "Ctrl+wheel didn't zoom in (extent %d, was %d)", W[1].extent_w, mark);
        ctrl_key = 0;
        scroll_mark = W[1].scroll_x;
        next(89);
        b[0] = TL; window_state(b);
        b[8] = 0; b[9] = -1; /* the wheel down: on in time */
        return 10;
    case 89:
        CHECK(W[1].scroll_x > scroll_mark, "the wheel didn't scroll the Timeline (%d, was %d)", W[1].scroll_x,
              scroll_mark);
        ctrl_key = 1;
        next(90);
        goto wheel_down;
    case 90:
        next(91);
        goto wheel_down;
    case 91:
        ctrl_key = 0;
        CHECK(W[1].extent_w == mark, "Ctrl+wheel didn't zoom out again (extent %d, was %d)", W[1].extent_w, mark);
        next(92);
        data_load(b, VIEWER, W[0].box[0] + 50, 0xBF8, clip3); /* raw MPEG-2: no length */
        return 17;
    /* ---- a clip that doesn't say how long it is: measured, then added ---- */
    case 92:
        CHECK(re_test_measuring() && !strcmp(layout(), "c30 c75"), "the .m2v: measuring %d, %s", re_test_measuring(),
              layout());
        next(93);
        data_load(b, VIEWER, W[0].box[0] + 50, 0xFFD, clip2); /* (dropped meanwhile: after it) */
        return 17;
    case 93:
        if (re_test_measuring()) {
            if (!nulls)
                fail_quit("no null events while measuring");
            if (fake_cs - phase_cs > 300)
                fail_quit("still measuring after 3 s");
            return 0;
        }
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 c75 c75 c75"), "the .m2v measured (75 frames) and the clip dropped meanwhile: %s",
              layout());
        {
            re_item i2, i3;
            re_project *pj = re_test_project();
            CHECK(!re_project_item(pj, 2, &i2) && !re_project_item(pj, 3, &i3) && strstr(i2.name, ".m2v") &&
                      strstr(i3.name, ".mp4"),
                  "the clip dropped while measuring didn't follow the .m2v");
        }
        next(94);
        return key(b, 0x188);
    case 94:
        if (nulls)
            return 0;
        next(95);
        return key(b, 0x188);
    case 95:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 c75"), "the two undone: %s", layout());
        next(96);
        data_load(b, VIEWER, W[0].box[0] + 50, 0xBF8, clip3);
        return 17;
    case 96:
        CHECK(re_test_measuring(), "not measuring the .m2v again");
        next(97);
        return key(b, 27); /* Escape: measuring stopped */
    case 97:
        if (nulls && fake_cs - phase_cs < 20)
            return 0;
        CHECK(!re_test_measuring() && !strcmp(layout(), "c30 c75"), "Escape while measuring: %d, %s",
              re_test_measuring(), layout());
        whole_redraws = 0;
        next(98);
        b[0] = VIEWER; window_state(b);
        b[2] += 120; /* (the bottom edge up: smaller) */
        return 2;
    case 98:
        CHECK(whole_redraws == 1, "resized, the Viewer wasn't all drawn again (%d)", whole_redraws);
        whole_redraws = 0;
        next(99);
        b[0] = VIEWER; window_state(b);
        b[1] += 40; b[3] += 40; /* (moved, the same size) */
        return 2;
    case 99:
        CHECK(whole_redraws == 0, "moved, the Viewer was all drawn again");
        next(24);
        b[0] = W[0].box[0] + 50; b[1] = W[0].box[3] - 50; b[2] = 2; b[3] = VIEWER; b[4] = -1;
        return 6; /* Menu */
    wheel_up:
        b[0] = TL; window_state(b);
        b[8] = 0; b[9] = 1;
        return 10;
    wheel_down:
        b[0] = TL; window_state(b);
        b[8] = 0; b[9] = -1;
        return 10;
    case 24:
        CHECK(menus >= 1, "Menu didn't open the menu");
        next(25);
        b[0] = 3; b[1] = 0; b[2] = -1; /* View > Zoom in */
        return 9;
    case 25:
        tl_scale = 8; /* (zoomed in: the clicks from now on show it) */
        all_text[0] = 0;
        next(66);
        b[0] = 4; b[1] = -1; /* Video information */
        return 9;
    case 66:
        if (W[2].redraw)
            return nulls ? 0 : 1;
        CHECK(windows_made == 3 && W[2].open, "Video information didn't open");
        CHECK(strstr(all_text, "H.264") && strstr(all_text, "Decoded by") && strstr(all_text, "320x180") &&
                  strstr(all_text, "AAC"),
              "Video information: \"%.300s\"", all_text);
        next(67);
        b[0] = 3; b[1] = 3; b[2] = -1; /* View > Attach the Timeline: off */
        return 9;
    case 67:
        mark = W[0].box[0];
        next(68);
        b[0] = TL; b[1] = W[1].box[0] + 60; b[2] = W[1].box[1]; b[3] = W[1].box[2] + 60; b[4] = W[1].box[3];
        b[5] = W[1].scroll_x; b[6] = 0; b[7] = -1;
        return 2;
    case 68:
        CHECK(W[0].box[0] == mark, "detached, the Viewer still moved with the Timeline");
        next(26);
        return key(b, 0x183); /* F3 */
    /* ---- saving ---- */
    case 26:
        CHECK(savebox_opens == 1 && W[3].open, "F3 didn't open the save box");
        snprintf(re_test_savebox_name(), 256, "%.255s", typed_path);
        next(27);
        return key_to(b, SAVEB, 13);
    case 27: {
        CHECK(!W[3].open, "the save box still open after saving");
        FILE *f = fopen(typed_path, "r");
        char x[8192] = "";
        if (f) {
            size_t n = fread(x, 1, sizeof(x) - 1, f);
            x[n] = 0;
            fclose(f);
        }
        CHECK(f && strstr(x, "<playlist") && strstr(x, "<tractor"), "%s isn't an MLT project", typed_path);
        CHECK(filetypes_set == 1 && !strcmp(filetype_path, typed_path), "the filetype wasn't set");
        CHECK(!strcmp(re_test_title(), typed_path), "title after saving: \"%s\"", re_test_title());
        next(28);
        return click_tl(b, 50, 1, 0x400); /* (then a change: a split at 50) */
    }
    case 28:
        if (nulls)
            return 0;
        next(29);
        return key_to(b, TL, 's');
    case 29:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 c20 c55"), "S at 50 (zoomed in): %s", layout());
        CHECK(strstr(re_test_title(), " *") != NULL, "no star after a change: \"%s\"", re_test_title());
        next(30);
        return key(b, 0x183); /* F3 again: the box with the saved name */
    case 30:
        CHECK(savebox_opens == 1, "F3 opened the box with somewhere to save");
        CHECK(!strstr(re_test_title(), " *"), "F3 didn't save to %s: \"%s\"", typed_path, re_test_title());
        next(31);
        return key_to(b, TL, 's'); /* a change (a split at 50 again: none; so Right first) */
    case 31:
        next(32);
        return key_to(b, TL, 0x18D);
    case 32:
        if (nulls)
            return 0;
        next(33);
        return key_to(b, TL, 's');
    case 33:
        if (nulls)
            return 0;
        CHECK(strstr(re_test_title(), " *") != NULL, "no star after another change");
        /* File > Save as, by the menu: the box, with the name */
        next(35);
        b[0] = 0; b[1] = 2; b[2] = -1;
        return 9;
    case 35:
        CHECK(savebox_opens == 2 && W[3].open && !strcmp(re_test_savebox_name(), typed_path),
              "File > Save as: the box (%d) with \"%s\"", savebox_opens, re_test_savebox_name());
        next(36);
        b[0] = 0; b[1] = 0; b[2] = 0x40; b[3] = SAVEB; b[4] = 0; /* a drag of the file icon */
        return 6;
    case 36:
        CHECK(drags == 2, "no drag started");
        ptr[0] = 500; ptr[1] = 500; ptr[2] = 0; ptr[3] = FILER; ptr[4] = 0;
        next(37);
        return 7; /* the drag ends over a directory display */
    case 37:
        CHECK(datasave_ref != 0 && strstr(typed_path, datasave_leaf), "no Message_DataSave sent, or leaf \"%s\"",
              datasave_leaf);
        /* the Filer's Message_DataSaveAck: the full name */
        memset(b, 0, 256);
        b[1] = 0x777; b[2] = 77; b[3] = datasave_ref; b[4] = 2;
        b[5] = FILER; b[9] = 4096; b[10] = 0xF80;
        snprintf((char *) &b[11], 200, "%.199s", dragged_path);
        b[0] = (44 + (int) strlen(dragged_path) + 1 + 3) & ~3;
        next(38);
        return 17;
    case 38: {
        FILE *f = fopen(dragged_path, "r");
        CHECK(f != NULL, "not saved to %s by the drag", dragged_path);
        if (f)
            fclose(f);
        CHECK(dataloads_sent == 1, "no Message_DataLoad after saving");
        CHECK(!strcmp(re_test_title(), dragged_path), "title after the drag: \"%s\"", re_test_title());
        next(39);
        return key_to(b, TL, 0x18D);
    }
    case 39:
        if (nulls)
            return 0;
        next(40);
        return key_to(b, TL, 's'); /* a change */
    /* ---- closing ---- */
    case 40:
        if (nulls)
            return 0;
        CHECK(strstr(re_test_title(), " *") != NULL, "no star after the change");
        answer = 4; /* Cancel: a project dropped while this one has changes */
        snprintf(saved_layout, sizeof(saved_layout), "%s", layout());
        next(51);
        data_load(b, -2, 0, 0xF80, typed_path);
        return 17;
    case 51:
        CHECK(questions == 1 && re_test_project() && !strcmp(layout(), saved_layout),
              "a project dropped, Cancel: %d questions, %s", questions, layout());
        next(41);
        b[0] = VIEWER;
        return 3;
    case 41:
        CHECK(questions == 2, "closing with changes didn't ask");
        CHECK(W[0].open && re_test_project(), "Cancel closed it");
        answer = 3; /* Discard */
        ovd_mark = ov_destroyed;
        next(42);
        b[0] = TL;
        return 3;
    case 42:
        CHECK(questions == 3 && !W[0].open && !W[1].open && !re_test_project() && !re_test_open(),
              "Discard didn't close the editor");
        CHECK(!nulls, "null events with nothing open");
        mark = reports;
        next(50);
        {
            FILE *f = fopen("not-a-project", "w"); /* type XML, but not a project */
            if (f) {
                fputs("this isn't MLT\n", f);
                fclose(f);
            }
        }
        data_load(b, -2, 0, 0xF80, "not-a-project");
        return 17;
    case 50:
        CHECK(reports == mark + 1 && !re_test_project() && !W[0].open, "a file that isn't a project: not refused");
        next(43);
        data_load(b, -2, 0, 0xF80, dragged_path); /* (type XML, no extension) */
        return 17;
    case 43: /* the project saved by the drag, opened again */
        if (nulls)
            return 0;
        CHECK(W[0].open && W[1].open, "the editor isn't open for the project");
        CHECK(!strcmp(layout(), "c30 c20 c1 c54"), "opened again: %s", layout());
        CHECK(!strcmp(re_test_title(), dragged_path), "opened: title \"%s\"", re_test_title());
        CHECK(abs(sprite_middle() - grey(0)) <= 2, "opened: frame 0 drawn as %d", sprite_middle());
        next(44);
        return key_to(b, TL, 0x18D);
    case 44:
        if (nulls)
            return 0;
        next(45);
        return key_to(b, TL, 's'); /* a change */
    case 45: /* the desktop shutting down: asked, Discard; the shutdown started again; quit */
        if (nulls)
            return 0;
        answer = 3;
        mark = questions;
        next(46);
        memset(b, 0, 24);
        b[0] = 20; b[1] = 0x555; b[2] = 66; b[4] = 8; /* Message_PreQuit */
        return 17;
    case 46:
        fail_quit("still polling after Discard on Message_PreQuit");
        return 0;
    default:
        fail_quit("polled after quitting");
        return 0;
    }
}

int main(int argc, char **argv)
{
    char cwd[256];
    if (argc != 4) {
        fprintf(stderr, "usage: editor_test numbered-snd.mp4 numbered.mp4 numbered.m2v\n");
        return 2;
    }
    clip = argv[1];
    clip2 = argv[2];
    clip3 = argv[3];
    if (!getcwd(cwd, sizeof(cwd)))
        return 2;
    snprintf(typed_path, sizeof(typed_path), "%s/typed.mlt", cwd);
    snprintf(dragged_path, sizeof(dragged_path), "%s/dragged", cwd); /* (no extension: as RISC OS names are) */
    remove(typed_path);
    remove(dragged_path);
    setenv("ReelEdit$Log", "editor_test.log", 1);
    int r = reeledit_main(0, NULL);
    CHECK(r == 0, "reeledit_main returned %d", r);
    CHECK(phase == 46, "the script stopped at phase %d", phase);
    CHECK(questions == mark + 1 && prequit_acks == 1, "Message_PreQuit: %d questions, %d acknowledgements",
          questions - mark, prequit_acks);
    CHECK(key_sent == 0x1FC, "the shutdown not started again (key &%X sent)", key_sent);
    CHECK(closedown == 1, "Wimp_CloseDown not called");
    printf("editor_test: %d polls, %d null events, %d Wimp_PollIdle, %u cs asleep; overlay %d pictures; "
           "%u bytes of sound\n", polls, null_events, idle_polls, slept, ov_displays, ssb_bytes);
    printf("editor_test: %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
}
