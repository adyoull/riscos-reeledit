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
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "project.h"
#include "export.h"
#include "fake/screens.h"
int app_position(void); /* (viewer.c's) */
int re_test_previews(int *thumbs, int *wave_rects); /* (previews.c's) */
int re_test_previews_stand_ins(void);
int re_test_previews_pending(void);
int re_test_renders(void); /* (renders.c's) */
int re_test_previews_playing(void);
int rr_stretches(int i, int *start, int *length, int *made);
static int render_starts, render_jobs, render_progress_seen, proxy_starts;
static char proxy_made[300];
const char *re_test_tl_title(void);
re_project *re_test_project(void);
static int big_item(void) /* (the big clip's item: -1 none) */
{
    for (int i = 0; i < re_project_items(re_test_project()); i++) {
        re_item it;
        if (!re_project_item(re_test_project(), i, &it) && !strcmp(it.name, "big.mp4"))
            return i;
    }
    return -1;
}
static int thumb_plots; /* (the Timeline's thumbnails plotted) */

int reeledit_main(int argc, char **argv);
int re_test_position(void);
double re_test_speed(void);
int re_test_open(void);
re_project *re_test_project(void);
int re_test_selected(void);
int re_test_overlay_selected(void);
int re_test_mark(int out);
int re_test_snap(void);
int re_test_measuring(void);
int re_test_busy(void);
const char *re_test_title(void);
void re_test_crash(void);
#define AUTOSAVE "<Choices$Write>.ReelEdit.Autosave" /* (the RISC OS name, as a file here) */
char *re_test_savebox_name(void);
char *re_test_title_line(int n);
char *re_test_export_name(void);
int re_test_clips_selected(void);
int re_test_audio_selected(int *track);
static int au_track_sel(void) { int t; re_test_audio_selected(&t); return t; }
static int au_sel(void) { int t; return re_test_audio_selected(&t); }
const char *re_test_export_status(void);

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

static const char *clip, *clip2, *clip3, *clip4;
static char typed_path[300], dragged_path[300];

/* ---- the fake machine ------------------------------------------------------- */

#define VIEWER 0x1000 /* the windows, in the order the program makes them */
#define TL     0x1100
#define INFO   0x1200 /* (made before the save box in this script) */
#define SAVEB  0x1300
#define TITLEW 0x1400 /* (the Title window: drawn by the Wimp) */
#define EXPW   0x1500 /* (the Export window: drawn by the Wimp) */
#define CLIPW  0x1600 /* (the Clips window) */
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
static fwin W[7]; /* VIEWER, TL, INFO, SAVEB, TITLEW, EXPW, CLIPW */
static int windows_made;
static fwin *win(int h)
{
    int i = (h - VIEWER) / 0x100;
    return h >= VIEWER && i < 7 && (h - VIEWER) % 0x100 == 0 ? &W[i] : NULL;
}

static unsigned fake_cs = 1000;
static unsigned slept;
static int polls, idle_polls, null_events, last_mask;
static int covered, shift_key, ctrl_key;
static char help_text[256], drag_name0[64];
static int whole_redraws;
static int starts, morites, exports_typed, mark2_reports;
static char saved_layout[256], saved_title[300];
static char start_cmd[1500];
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
static int ov_block[6], ov_redraws, ov_redraws_outside;
static int ssb_closes, ssb_playing;
static unsigned ssb_bytes, ssb_added, ssb_played, ssb_play_cs;
static double ssb_level;
static _kernel_oserror err = {1, "fake: not here"};

static void fail_quit(const char *why, ...) __attribute__((format(printf, 1, 2)));
static void fail_quit(const char *why, ...)
{
    va_list ap;
    printf("FAIL (phase %d): ", phase);
    va_start(ap, why);
    vprintf(why, ap);
    va_end(ap);
    printf("\n");
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

/* (the titles' row, over V2: shown while there are titles on their track) */
#define TL_T (re_test_project() && re_project_track_overlays(re_test_project(), 3) > 0 ? 48 + 8 : 0)
#define TL_V2_MID (48 + 8 + TL_T + 32) /* (the middle of V2's row, down from the top) */
#define TL_V1_TOP (48 + 8 + TL_T + 64 + 8) /* (the ruler, the titles' and V2's rows, the gaps: down to V1) */
static int script(int *b, int mask);
void lg_start(void);
int lg_on(void);
void lg_end(void);
void app_log(const char *fmt, ...);
#define LOG_CHOICE "<Choices$Write>.ReelEdit.Logging" /* (the RISC OS name, as a file here) */
static int choice_is(const char *want)
{
    char t[16] = "";
    FILE *f = fopen(LOG_CHOICE, "r");
    if (f) {
        if (!fgets(t, sizeof(t), f))
            t[0] = 0;
        fclose(f);
    }
    return !strncmp(t, want, strlen(want)) && t[strlen(want)] == '\n';
}

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
        case 5: S.ov_w = (int) r->r[1]; S.ov_h = (int) r->r[2]; break;         /* SetScale */
        case 7: S.ov_x = (int) r->r[1]; S.ov_y = (int) r->r[2]; break;         /* SetPosition */
        case 8: { /* RedrawWindow: given the picture's part of the rectangle only (it clears all it's given) */
            const int *rb = (const int *) r->r[1];
            ov_redraws++;
            if (rb[8] < W[0].box[1] + 2 * 64 || rb[7] < W[0].box[0] || rb[9] > W[0].box[2] || rb[10] > W[0].box[3])
                ov_redraws_outside++;
            break;
        }
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
        if (r->r[0] == 18 && r->r[2] == 0xBF6) { /* (an export made) */
            exports_typed++;
            break;
        }
        if (r->r[0] == 18) {
            filetypes_set++;
            CHECK(r->r[2] == 0xF80, "filetype &%X set, not &F80", (int) r->r[2]);
            snprintf(filetype_path, sizeof(filetype_path), "%s", (const char *) r->r[1]);
        }
        break;
    case 0x2E:
        if (!memcmp((const char *) ((int *) r->r[2] + 1), "thumbs", 6)) { /* (the Timeline's thumbnails: previews.c) */
            if ((r->r[0] & 0xFF) == 52)
                scr_sprite((const int *) r->r[2], (int) r->r[3], (int) r->r[4]);
            thumb_plots++;
            break;
        }
        plots++;
        plot_area = (int *) r->r[1];
        if ((r->r[0] & 0xFF) == 52)
            scr_sprite((const int *) r->r[2], (int) r->r[3], (int) r->r[4]);
        break;
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
    case 0x45: plots_tl++; scr_plot((int) r->r[0], (int) r->r[1], (int) r->r[2]); break; /* OS_Plot */
    case 0x46: scr_vdu((const unsigned char *) r->r[0], (int) r->r[1]); break;        /* OS_WriteN */
    case 0x65: break;
    case 0x40743:                                  /* ColourTrans_SetGCOL */
        scr_colour((unsigned) r->r[0]);
        if ((unsigned) r->r[0] == 0x2080F000u)
            colours_seen |= 1;
        if ((unsigned) r->r[0] == 0xF0C89000u) /* (a V1 clip's waveform: timeline.c's WAVE_COLOUR) */
            colours_seen |= 2;
        if ((unsigned) r->r[0] == 0x40B04000u) /* (a rendered stretch: RENDERED_COLOUR) */
            colours_seen |= 4;
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
        if (windows_made >= 7)
            fail_quit("more than seven windows made");
        scr_create_window(windows_made, (const void *) r->r[1]);
        r->r[0] = VIEWER + 0x100 * windows_made++;
        break;
    case 0x400C2: r->r[0] = BAR_ICON; break;
    case 0x400E9: break; /* Wimp_SpriteOp 24: the sprite is there */
    case 0x400DE: /* StartTask: an export's TaskWindow (the script plays its part) */
        starts++;
        snprintf(start_cmd, sizeof(start_cmd), "%s", (const char *) r->r[0]);
        break;
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
        for (int i = 0; i < 7; i++)
            if (W[i].redraw && W[i].open && i != 3 && i != 4 && i != 5) { /* (the save box is drawn by the Wimp) */
                W[i].redraw = 0;
                b[0] = VIEWER + 0x100 * i;
                r->r[0] = 1;
                fake_cs++;
                return NULL;
            }
        ev = script(b, last_mask);
        if (ev == 9 || ev == 6)
            S.menu = NULL;
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
    case 0x400C8: case 0x400C9: { /* RedrawWindow, UpdateWindow */
        int *b = (int *) r->r[1];
        rect_block(b, no == 0x400C9);
        scr_begin((b[0] - VIEWER) / 0x100, no == 0x400C8, b[7], b[8], b[9], b[10]);
        r->r[0] = 1;
        break;
    }
    case 0x400CA: r->r[0] = 0; S.cur = -1; break;
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
            W[3].box[0] = (int) r->r[2]; W[3].box[3] = (int) r->r[3]; /* (its top left) */
            W[3].box[2] = W[3].box[0] + S.w[3].vw; W[3].box[1] = W[3].box[3] - S.w[3].vh;
            S.menu = NULL;
        } else if (r->r[1] == -1) {
            W[3].open = 0;
            S.menu = NULL;
        } else {
            menus++;
            scr_menu_open((const void *) r->r[1], (int) r->r[2], (int) r->r[3]);
        }
        break;
    case 0x400D7: {                                                 /* SetExtent */
        fwin *w = win((int) r->r[0]);
        int *b = (int *) r->r[1];
        if (w)
            w->extent_w = b[2] - b[0];
        break;
    }
    case 0x400FC: /* ResizeIcon */
        if (win((int) r->r[0]))
            scr_resize_icon(((int) r->r[0] - VIEWER) / 0x100, (int) r->r[1], (int) r->r[2], (int) r->r[3],
                            (int) r->r[4], (int) r->r[5]);
        break;
    case 0x400CD: /* SetIconState */
        if (win(((int *) r->r[1])[0]))
            scr_set_icon_state((((int *) r->r[1])[0] - VIEWER) / 0x100, ((int *) r->r[1])[1], ((int *) r->r[1])[2],
                               ((int *) r->r[1])[3]);
        break;
    case 0x400D2: case 0x400DC: break;
    case 0x400F9:                                                   /* Wimp_TextOp: 2 plots */
        scr_textop((int) r->r[0], r->r[1], r->r[2], (int) r->r[4], (int) r->r[5]);
        if ((r->r[0] & 0xFF) == 1) { /* the width: 16 OS units a character */
            size_t n = strlen((const char *) r->r[1]);
            r->r[0] = 16 * (intptr_t) (r->r[2] > 0 && (size_t) r->r[2] < n ? (size_t) r->r[2] : n);
            break;
        }
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
        else if (b[4] == 0x808C4)
            morites++;
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

/* A screenshot (SCREENS=dir): the desktop as the editor has drawn it */
static int shot_box(int i, int b[5])
{
    if (!W[i].open)
        return 0;
    b[0] = W[i].box[0]; b[1] = W[i].box[1]; b[2] = W[i].box[2]; b[3] = W[i].box[3]; b[4] = W[i].scroll_x;
    return 1;
}
static void shot(const char *name)
{
    static const int order[7] = {0, 1, 2, 3, 4, 5, 6};
    int pitch = (ov_w + 31) & ~31;
    snap(name, order, 7, shot_box, ov_shown_bank >= 0 ? ov_buf[ov_shown_bank] : NULL, ov_w, ov_h, pitch);
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
        snprintf(s + strlen(s), sizeof(s) - strlen(s), "%s%c%d", i ? " " : "", it.gap ? 'g' : it.mix ? 'm' : 'c',
                 it.length);
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

/* a drop on V2's row of the Timeline, at a frame */
static void data_load_v2(int *b, int frame, int type, const char *file)
{
    data_load(b, TL, W[1].box[0] - W[1].scroll_x + 16 + (int) (frame * 4), type, file);
    b[8] = W[1].box[3] - TL_V2_MID;
}

/* A sprite file of two sprites, "first" and "second", 4x2 32-bit (new
   format, 90 dpi), the second white */
static void two_sprites(const char *file)
{
    FILE *f = fopen(file, "wb");
    uint32_t w[3 + 2 * (11 + 8)];
    int k = 0;
    w[k++] = 2;
    w[k++] = 16;
    w[k++] = 16 + 2 * (44 + 32);
    for (int s = 0; s < 2; s++) {
        w[k++] = 44 + 32;
        char name[12] = {0};
        strcpy(name, s ? "second" : "first");
        memcpy(&w[k], name, 12);
        k += 3;
        w[k++] = 3;  /* 4 words wide */
        w[k++] = 1;  /* 2 high */
        w[k++] = 0;
        w[k++] = 31;
        w[k++] = 44; /* the picture */
        w[k++] = 44; /* no mask */
        w[k++] = 6u << 27 | 90 << 14 | 90 << 1 | 1;
        for (int i = 0; i < 8; i++)
            w[k++] = s ? 0x00FFFFFF : 0x000000FF;
    }
    fwrite(w, 4, (size_t) k, f);
    fclose(f);
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

/* a click (Select: 0x400 for this window's button type) on the Timeline at a frame: the ruler (1), V1 (0),
   or V2 (2) */
#define TL_V1_H 160 /* (V1's row) */
#define TL_A_TOP(k) (TL_V1_TOP + TL_V1_H + 8 + (k) * (56 + 8)) /* (an audio track's row: below V1) */
static int click_audio(int *b, int frame, int k, int buttons)
{
    b[0] = W[1].box[0] - W[1].scroll_x + 16 + (int) (frame * tl_scale);
    b[1] = W[1].box[3] - TL_A_TOP(k) - 28;
    b[2] = buttons; b[3] = TL; b[4] = -1;
    return 6;
}
static int drop_audio(int *b, int frame, int k, const char *file)
{
    data_load(b, TL, W[1].box[0] - W[1].scroll_x + 16 + (int) (frame * tl_scale), 0xFB1, file);
    b[8] = W[1].box[3] - TL_A_TOP(k) - 28;
    return 17;
}
static int click_tl(int *b, int frame, int on_ruler, int buttons)
{
    b[0] = W[1].box[0] - W[1].scroll_x + 16 + (int) (frame * tl_scale);
    b[1] = on_ruler == 1   ? W[1].box[3] - 10
           : on_ruler == 2 ? W[1].box[3] - TL_V2_MID
           : on_ruler == 3 ? W[1].box[3] - (48 + 8 + 24) /* (the titles' row) */
                           : W[1].box[3] - TL_V1_TOP - 48;
    b[2] = buttons; b[3] = TL; b[4] = -1;
    return 6;
}

static void next(int p)
{
    phase = p;
    phase_cs = fake_cs;
}

static char saved_layout[256];
static int menus_mark;
static int plots_mark, ovd_mark, ssb_mark, closes_mark, pos_mark, pos_min = 1000, mark, scroll_mark, rec, th, wr;

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
        /* ---- logging (on here: ReelEdit$Log): the icon bar menu's Logging turns it off, then on ---- */
        CHECK(lg_on(), "logging isn't on with ReelEdit$Log set");
        remove(LOG_CHOICE);
        menus_mark = menus;
        next(3000);
        b[0] = 100; b[1] = 20; b[2] = 2; b[3] = -2; b[4] = BAR_ICON; /* Menu on the icon */
        return 6;
    case 3000:
        CHECK(menus == menus_mark + 1, "Menu on the icon: %d menus", menus - menus_mark);
        next(3001);
        b[0] = 1; b[1] = -1; /* Logging */
        return 9;
    case 3001:
        CHECK(!lg_on() && choice_is("off"), "Logging: still on (%d), or the choice not kept", lg_on());
        app_log("not to be logged");
        next(3002);
        b[0] = 100; b[1] = 20; b[2] = 2; b[3] = -2; b[4] = BAR_ICON;
        return 6;
    case 3002:
        next(3003);
        b[0] = 1; b[1] = -1; /* Logging again */
        return 9;
    case 3003: {
        CHECK(lg_on() && choice_is("on"), "Logging again: not on (%d), or the choice not kept", lg_on());
        FILE *f = fopen("editor_test.log", "r");
        char t[300];
        int off = 0, on = 0, quiet = 1, header = 0;
        while (f && fgets(t, sizeof(t), f)) {
            off += strstr(t, "logging off") != NULL;
            on += strstr(t, "logging on") != NULL;
            header += strstr(t, "ReelEdit ") != NULL;
            if (strstr(t, "not to be logged"))
                quiet = 0;
        }
        if (f)
            fclose(f);
        CHECK(off == 1 && on == 1 && quiet && header == 2, "the log: off %d, on %d, quiet %d, headers %d (kept, added to)",
              off, on, quiet, header);
        next(2);
        b[0] = 100; b[1] = 20; b[2] = 4; b[3] = -2; b[4] = BAR_ICON;
        return 6;
    }
    case 2:
        shot("01-empty");
        CHECK(W[0].open && W[1].open, "the editor's windows aren't open (%d, %d)", W[0].open, W[1].open);
        CHECK(re_test_project() && re_project_items(re_test_project()) == 0, "not an empty project");
        CHECK(!re_test_open(), "an engine with nothing to play");
        CHECK(!strcmp(re_test_title(), "<Untitled>"), "title \"%s\"", re_test_title());
        CHECK(!nulls, "null events with nothing to play");
        CHECK(W[1].box[3] + 40 == W[0].box[1] && abs((W[1].box[0] + W[1].box[2]) - (W[0].box[0] + W[0].box[2])) <= 2
                  && W[1].box[2] - W[1].box[0] > W[0].box[2] - W[0].box[0],
              "the Timeline isn't attached under the Viewer (%d,%d,%d,%d under %d,%d,%d,%d)", W[1].box[0], W[1].box[1],
              W[1].box[2], W[1].box[3], W[0].box[0], W[0].box[1], W[0].box[2], W[0].box[3]);
        next(60);
        /* the user drags the Timeline: the Viewer comes with it */
        b[0] = TL; b[1] = W[1].box[0] + 100; b[2] = W[1].box[1] - 50; b[3] = W[1].box[2] + 100; b[4] = W[1].box[3] - 50;
        b[5] = 0; b[6] = 0; b[7] = -1;
        mark = W[0].box[0];
        return 2;
    case 60:
        CHECK(W[0].box[0] == mark + 100 && W[1].box[3] + 40 == W[0].box[1]
                  && abs((W[1].box[0] + W[1].box[2]) - (W[0].box[0] + W[0].box[2])) <= 2,
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
        shot("02-first-clip");
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
            shot("03-playing");
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
            CHECK(ov_redraws > 0 && ov_redraws_outside == 0,
                  "VideoOverlay_RedrawWindow given the controls: %d of %d redraws", ov_redraws_outside, ov_redraws);
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
        CHECK(pos == 30, "a click on a clip moved the playhead (to %d)", pos);
        CHECK(colours_seen & 1, "the selected clip isn't drawn orange"); /* (the redraw came first) */
        shot("04-split-and-selected");
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
        b[5] = W[1].box[0] + 100; b[6] = W[1].box[3] - 10; /* (on the ruler) */
        b[0] = 40; b[1] = 0x555; b[2] = 56; b[4] = 0x502;
        next(86);
        return 17;
    case 86:
        CHECK(strstr(help_text, "Timeline's ruler") != NULL, "help on the Timeline's ruler: \"%s\"", help_text);
        mark = W[1].extent_w;
        ctrl_key = 1;
        ptr[0] = W[1].box[0] + 100; ptr[1] = W[1].box[3] - 60; ptr[2] = 0; ptr[3] = TL; ptr[4] = -1;
        next(87);
        goto wheel_up;
    case 87:
        next(287);
        goto wheel_up;
    case 287: /* (three notches: the Timeline is the workspace's width, wider than a short project) */
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
        next(290);
        goto wheel_down;
    case 290:
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
        next(100);
        return click_tl(b, 104, 1, 0x400); /* (the playhead at the end: its line is at the edge to be dragged) */
    /* ---- dragging on the Timeline: trimming an edge, moving a clip ---- */
    case 100:
        if (nulls)
            return 0;
        mark = drags;
        CHECK(pos == 104, "the playhead not at the end (%d)", pos);
        next(101);
        /* a drag just past the second clip's end (105), where the playhead's
           line is too: trims the end, not the playhead */
        b[0] = W[1].box[0] - W[1].scroll_x + 16 + 105 * 4 + 6; b[1] = W[1].box[3] - TL_V1_TOP - 48; b[2] = 0x40;
        b[3] = TL; b[4] = -1;
        return 6;
    case 101:
        CHECK(drags == mark + 1, "a drag at a clip's edge didn't start a drag");
        ptr[0] = W[1].box[0] - W[1].scroll_x + 16 + 105 * 4 + 6 - 5 * 4; ptr[1] = W[1].box[3] - 100; /* (5 back; in two steps) */
        next(201);
        return script(b, mask);
    case 201:
        if (strcmp(layout(), "c30 c70")) {
            if (fake_cs - phase_cs > 100)
                fail_quit("dragging the end 5 frames back: not trimmed");
            return 0;
        }
        ptr[0] = W[1].box[0] - W[1].scroll_x + 16 + 105 * 4 + 6 - 10 * 4;
        next(102);
        return script(b, mask);
    case 102:
        if (!strcmp(layout(), "c30 c65") && pos == 94 && !re_test_busy() && plots > 0) {
            CHECK(nulls, "no null events while dragging an edge");
            CHECK(abs(sprite_middle() - grey(64)) <= 2, "the Viewer shows %d at the dragged end, not its last frame (%d)",
                  sprite_middle(), grey(64));
            shot("05-trimming-an-end");
            next(103);
            return 7; /* the drag ends */
        }
        if (fake_cs - phase_cs > 100)
            fail_quit("dragging the end 10 frames back: not trimmed (or not shown)");
        return nulls ? 0 : (fail_quit("null events masked while dragging an edge"), 0);
    case 103:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 c65"), "after dragging the end: %s", layout());
        next(104);
        return key_to(b, TL, 0x188);
    case 104:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 c75"), "one F8 for the whole drag: %s", layout());
        next(105);
        return click_tl(b, 30, 0, 0x40); /* the second clip's start: trim its start */
    case 105:
        ptr[0] = W[1].box[0] - W[1].scroll_x + 16 + 40 * 4;
        next(106);
        return script(b, mask);
    case 106:
        if (!strcmp(layout(), "c30 c65") && pos == 30 && !re_test_busy()) {
            CHECK(abs(sprite_middle() - grey(10)) <= 2, "the Viewer shows %d at the dragged start, not frame 10's %d",
                  sprite_middle(), grey(10));
            next(107);
            return 7;
        }
        if (fake_cs - phase_cs > 100)
            fail_quit("dragging the start 10 frames on: not trimmed (or not shown)");
        return 0;
    case 107:
        if (nulls)
            return 0;
        next(108);
        return key_to(b, TL, 0x188);
    case 108:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 c75"), "undoing the start's trim: %s", layout());
        {
            re_item i0;
            re_project_item(re_test_project(), 0, &i0);
            snprintf(drag_name0, sizeof(drag_name0), "%s", i0.name);
        }
        next(109);
        return click_tl(b, 15, 0, 0x40); /* the first clip's middle: move it */
    case 109:
        ptr[0] = W[1].box[0] - W[1].scroll_x + 16 + 101 * 4; /* (nearest the end, 105) */
        next(110);
        return script(b, mask);
    case 110:
        if (fake_cs - phase_cs < 10)
            return 0;
        shot("06-moving-a-clip");
        next(111);
        return 7; /* dropped */
    case 111:
        if (nulls)
            return 0;
        {
            re_item i1;
            re_project_item(re_test_project(), 1, &i1);
            CHECK(!strcmp(layout(), "c75 c30") && !strcmp(i1.name, drag_name0) && re_test_selected() == 1,
                  "the first clip dragged to the end: %s, the second %s (sel %d)", layout(), i1.name,
                  re_test_selected());
            CHECK(pos == 75, "after the move, at %d, not its new start (75)", pos);
        }
        next(112);
        return key_to(b, TL, 0x188);
    case 112:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 c75"), "undoing the move: %s", layout());
        next(113);
        return click_tl(b, 50, 1, 0x400);
    /* ---- the playhead moves only from the ruler: on the track, its line is passed over ---- */
    case 113:
        if (nulls)
            return 0;
        CHECK(pos == 50, "the ruler at 50: %d", pos);
        next(114);
        return click_tl(b, 150, 0, 0x400); /* empty track, past the end */
    case 114:
        if (nulls && fake_cs - phase_cs < 20)
            return 0;
        CHECK(pos == 50 && re_test_selected() == -1, "a click on empty track: at %d (not 50), selected %d", pos,
              re_test_selected());
        mark = drags;
        ptr[0] = W[1].box[0] - W[1].scroll_x + 16 + 50 * 4; ptr[1] = W[1].box[3] - 100;
        next(115);
        return click_tl(b, 50, 0, 0x40); /* a drag on the track, on the playhead's line: the clip */
    case 115:
        CHECK(drags == mark + 1 && re_test_selected() == 1, "a drag on a clip at the playhead: drags %d, selected %d",
              drags - mark, re_test_selected());
        next(116);
        return 7; /* dropped where it was */
    case 116:
        if (nulls && fake_cs - phase_cs < 20)
            return 0;
        CHECK(pos == 50 && !strcmp(layout(), "c30 c75"), "after a drag on a clip at the playhead: at %d, %s", pos,
              layout());
        next(117);
        return key_to(b, TL, 'd');
    /* ---- dissolves: D at the cut nearest the playhead; dragged from a cut's corner; a dissolve's end dragged;
       selected and deleted ---- */
    case 117:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 m25 c50") && re_test_selected() == 1 && pos == 30,
              "D at 50: %s (from the first clip's spare frames, the length kept), selected %d, at %d", layout(),
              re_test_selected(), pos);
        shot("11-dissolve-by-key");
        next(118);
        return key_to(b, TL, 0x188);
    case 118:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 c75"), "undoing D: %s", layout());
        mark = drags;
        /* (the cut's mark: the bottom of the track, at the cut) */
        b[0] = W[1].box[0] - W[1].scroll_x + 16 + 30 * 4 + 2; b[1] = W[1].box[3] - TL_V1_TOP - TL_V1_H + 10; b[2] = 0x40;
        b[3] = TL; b[4] = -1;
        ptr[0] = b[0] + 10 * 4; ptr[1] = b[1];
        next(119);
        return 6;
    case 119: /* (the pointer resting: the Viewer shows the dissolve's middle) */
        if (strcmp(layout(), "c20 m10 c65") || pos < 20 || pos >= 30) {
            if (fake_cs - phase_cs > 100)
                fail_quit("dragging from a cut's mark 10 frames: %s, drags %d, at %d (not in the dissolve)", layout(),
                          drags - mark, pos);
            return 0;
        }
        CHECK(re_test_selected() == 1, "dragging a dissolve: selected %d", re_test_selected());
        shot("12-dissolve-by-dragging");
        next(120);
        return 7; /* dropped */
    case 120:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c20 m10 c65"), "after dragging a dissolve: %s", layout());
        mark = drags;
        b[0] = W[1].box[0] - W[1].scroll_x + 16 + 30 * 4 - 6; b[1] = W[1].box[3] - TL_V1_TOP - 48; b[2] = 0x40;
        b[3] = TL; b[4] = -1; /* (its end) */
        ptr[0] = b[0] + 5 * 4; ptr[1] = b[1];
        next(121);
        return 6;
    case 121:
        if (strcmp(layout(), "c15 m15 c60")) {
            if (fake_cs - phase_cs > 100)
                fail_quit("dragging a dissolve's end 5 frames: %s", layout());
            return 0;
        }
        next(122);
        return 7;
    case 122:
        if (nulls)
            return 0;
        next(123);
        return key_to(b, TL, 0x188);
    case 123:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c20 m10 c65"), "one F8 for dragging a dissolve's end: %s", layout());
        next(124);
        return click_tl(b, 25, 0, 0x400);
    case 124:
        CHECK(re_test_selected() == 1, "a click on a dissolve selected %d", re_test_selected());
        next(125);
        return key_to(b, TL, 0x7F);
    case 125:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 c75") && re_test_selected() == -1, "a dissolve deleted: %s", layout());
        next(126);
        return click_tl(b, 50, 1, 0x400); /* (the playhead back on the second clip, for Video information) */
    case 126:
        if (nulls)
            return 0;
        next(127);
        return click_tl(b, 60, 0, 0x400);
    /* ---- fades: Edit > Fade out on the selected clip; a fade handle dragged; one undo ---- */
    case 127:
        CHECK(re_test_selected() == 1, "fades: a click on the second clip selected %d", re_test_selected());
        next(128);
        b[0] = W[1].box[0] + 300; b[1] = W[1].box[3] - 100; b[2] = 2; b[3] = TL; b[4] = -1;
        return 6; /* Menu */
    case 128:
        next(129);
        b[0] = 1; b[1] = 11; b[2] = -1; /* Edit > Fade out */
        return 9;
    case 129: {
        if (nulls)
            return 0;
        re_item it;
        re_project_item(re_test_project(), 1, &it);
        CHECK(it.fade_out == 25 && it.fade_in == 0, "Edit > Fade out: fades %d, %d", it.fade_in, it.fade_out);
        shot("13-fade-out");
        next(140);
        menus_mark = menus;
        return click_tl(b, 15, 0, 2); /* Menu over the first clip: its context menu */
    }
    /* ---- the context menu: Fade in on the first clip; its box's edge dragged; one undo ---- */
    case 140:
        CHECK(menus == menus_mark + 1 && re_test_selected() == 0, "Menu over a clip: menus %d, selected %d",
              menus - menus_mark, re_test_selected());
        shot("15-clip-context-menu");
        next(141);
        b[0] = 0; b[1] = -1; /* Fade in */
        return 9;
    case 141: {
        if (nulls)
            return 0;
        re_item it;
        re_project_item(re_test_project(), 0, &it);
        CHECK(it.fade_in == 25, "the context menu's Fade in: %d", it.fade_in);
        mark = drags;
        /* its box's inner edge (frame 25), at the top of the track; dragged to frame 10 */
        b[0] = W[1].box[0] - W[1].scroll_x + 16 + 25 * 4 - 2; b[1] = W[1].box[3] - TL_V1_TOP - 10; b[2] = 0x40;
        b[3] = TL; b[4] = -1;
        ptr[0] = b[0] - 15 * 4; ptr[1] = b[1];
        next(130);
        return 6;
    }
    case 130: {
        re_item it;
        re_project_item(re_test_project(), 0, &it);
        if (it.fade_in != 10) {
            if (fake_cs - phase_cs > 100)
                fail_quit("dragging a fade's box's edge back 15 frames: fade %d (drags %d)", it.fade_in, drags - mark);
            return 0;
        }
        CHECK(!strcmp(layout(), "c30 c75"), "a fade dragged changed the clips: %s", layout());
        shot("14-fade-in-by-dragging");
        next(131);
        return 7;
    }
    case 131:
        if (nulls)
            return 0;
        next(132);
        return key_to(b, TL, 0x188);
    case 132: {
        if (nulls)
            return 0;
        re_item it, it1;
        re_project_item(re_test_project(), 0, &it);
        re_project_item(re_test_project(), 1, &it1);
        CHECK(it.fade_in == 25 && it1.fade_out == 25, "one F8 for a fade dragged: %d, %d", it.fade_in, it1.fade_out);
        next(142);
        return click_tl(b, 15, 0, 2);
    }
    case 142:
        next(143);
        b[0] = 0; b[1] = -1; /* Fade in again: taken away */
        return 9;
    case 143: {
        if (nulls)
            return 0;
        re_item it;
        re_project_item(re_test_project(), 0, &it);
        CHECK(it.fade_in == 0, "the context menu's Fade in again: %d (not taken away)", it.fade_in);
        next(230);
        return click_tl(b, 60, 0, 2); /* (the second clip: numbered-snd, faded out by Edit > Fade out) */
    }
    /* ---- the context menu's Sound, on the second clip: Mute; its sound's
       fade out taken away alone, then back; Mute again ---- */
    case 230:
        next(231);
        b[0] = 2; b[1] = 0; b[2] = -1; /* Sound > Mute */
        return 9;
    case 231: {
        if (nulls)
            return 0;
        re_item it;
        re_project_item(re_test_project(), 1, &it);
        CHECK(it.sound && it.muted, "Sound > Mute: sound %d, muted %d", it.sound, it.muted);
        shot("21-muted");
        next(232);
        return click_tl(b, 60, 0, 2);
    }
    case 232:
        next(233);
        b[0] = 2; b[1] = 2; b[2] = -1; /* Sound > Fade out: the sound's taken away */
        return 9;
    case 233: {
        if (nulls)
            return 0;
        re_item it;
        re_project_item(re_test_project(), 1, &it);
        CHECK(it.sound_fade_out == 0 && it.fade_out == 25, "Sound > Fade out: the sound's %d, the picture's %d",
              it.sound_fade_out, it.fade_out);
        next(234);
        return click_tl(b, 60, 0, 2);
    }
    case 234:
        next(235);
        b[0] = 2; b[1] = 2; b[2] = -1; /* Sound > Fade out: back */
        return 9;
    case 235: {
        if (nulls)
            return 0;
        re_item it;
        re_project_item(re_test_project(), 1, &it);
        CHECK(it.sound_fade_out == 25, "Sound > Fade out again: %d", it.sound_fade_out);
        next(236);
        return click_tl(b, 60, 0, 2);
    }
    case 236:
        next(237);
        b[0] = 2; b[1] = 0; b[2] = -1; /* Sound > Mute again: heard */
        return 9;
    case 237: {
        if (nulls)
            return 0;
        re_item it;
        re_project_item(re_test_project(), 1, &it);
        CHECK(!it.muted, "Sound > Mute again: muted %d", it.muted);
        shot("22-sound-fade");
        next(238);
        return click_tl(b, 60, 0, 2);
    }
    case 238:
        next(239);
        b[0] = 2; b[1] = 3; b[2] = 5; b[3] = -1; /* Sound > Volume > -6 dB */
        return 9;
    case 239: {
        if (nulls)
            return 0;
        re_item it;
        re_project_item(re_test_project(), 1, &it);
        CHECK(it.volume == -6, "Sound > Volume > -6 dB: %d", it.volume);
        next(144);
        return click_tl(b, 15, 0, 2);
    }
    case 144:
        next(145);
        b[0] = 5; b[1] = -1; /* Dissolve into the clip after */
        return 9;
    case 145:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 m25 c50") && re_test_selected() == 1, "the context menu's dissolve: %s, sel %d",
              layout(), re_test_selected());
        next(146);
        return click_tl(b, 40, 0, 2); /* Menu over the dissolve */
    case 146:
        next(147);
        b[0] = 0; b[1] = -1; /* Delete */
        return 9;
    case 147:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 c75"), "the dissolve's context menu: Delete: %s", layout());
        next(148);
        return click_tl(b, 60, 0, 2);
    case 148:
        next(149);
        b[0] = 0; b[1] = -1; /* Fade in on the second clip, where it meets the first */
        return 9;
    case 149:
        if (nulls)
            return 0;
        next(134);
        return click_tl(b, 50, 1, 0x400);
    /* the second clip's fade (where it meets the first) dragged a frame at a time: the box follows each step,
       the Viewer's picture only once the pointer rests */
    case 134: {
        if (nulls)
            return 0;
        re_item it;
        re_project_item(re_test_project(), 1, &it);
        CHECK(pos == 50 && it.fade_in == 25, "the second clip's fade in: %d, at %d", it.fade_in, pos);
        mark = drags;
        b[0] = W[1].box[0] - W[1].scroll_x + 16 + (30 + 25) * 4 - 2; b[1] = W[1].box[3] - TL_V1_TOP - 10; b[2] = 0x40;
        b[3] = TL; b[4] = -1;
        ptr[0] = b[0]; ptr[1] = b[1];
        scroll_mark = 0;
        next(135);
        return 6;
    }
    case 135: {
        re_item it;
        re_project_item(re_test_project(), 1, &it);
        if (scroll_mark > 0)
            CHECK(it.fade_in <= 25 - scroll_mark + 1 && pos == 50,
                  "a fade dragged a frame a step: step %d, fade %d, at %d (the picture made while moving)", scroll_mark,
                  it.fade_in, pos);
        if (scroll_mark < 8) {
            scroll_mark++;
            ptr[0] -= 4; /* (a frame back) */
            if (!nulls)
                fail_quit("no null events while dragging a fade");
            return 0;
        }
        CHECK(drags == mark + 1 && it.fade_in <= 18, "the second clip's fade at the cut, dragged: fade %d", it.fade_in);
        next(136);
        return 0;
    }
    case 136: { /* (resting: the fade's middle shown) */
        re_item it;
        re_project_item(re_test_project(), 1, &it);
        if (pos == 50) {
            if (fake_cs - phase_cs > 100)
                fail_quit("the pointer rested, but the Viewer didn't show the fade (at %d)", pos);
            return 0;
        }
        CHECK(pos >= 30 && pos < 30 + it.fade_in, "rested: at %d, not in the fade (30, %d frames)", pos, it.fade_in);
        next(137);
        return 7;
    }
    case 137:
        if (nulls)
            return 0;
        next(138);
        return key_to(b, TL, 0x188);
    case 138: {
        if (nulls)
            return 0;
        re_item it;
        re_project_item(re_test_project(), 1, &it);
        CHECK(it.fade_in == 25 && it.fade_out == 25, "undoing the drag of the fade at the cut: %d, %d", it.fade_in,
              it.fade_out);
        next(139);
        return key_to(b, TL, 0x188);
    }
    case 139: {
        if (nulls)
            return 0;
        re_item it;
        re_project_item(re_test_project(), 1, &it);
        CHECK(it.fade_in == 0, "undoing the context menu's fade in: %d", it.fade_in);
        next(150);
        return click_tl(b, 10, 1, 0x400);
    }
    /* ---- I/O marks: I at 10, O at 40; Return plays 10 to 40 and stops on 40; Shift-I goes to 10;
       ' (Extract) takes 10-40 out, one undo; ; (Lift) leaves a gap ---- */
    case 150:
        if (nulls)
            return 0;
        next(151);
        return key_to(b, TL, 'i');
    case 151:
        CHECK(re_test_mark(0) == 10 && re_test_mark(1) == -1, "I at 10: marks %d, %d", re_test_mark(0), re_test_mark(1));
        next(152);
        return click_tl(b, 40, 1, 0x400);
    case 152:
        if (nulls)
            return 0;
        next(153);
        return key_to(b, TL, 'o');
    case 153:
        CHECK(re_test_mark(0) == 10 && re_test_mark(1) == 40, "O at 40: marks %d, %d", re_test_mark(0), re_test_mark(1));
        shot("16-marks");
        next(154);
        return key_to(b, TL, 13); /* Return */
    case 154: /* (playing from 10; stopped on 40) */
        if (re_test_speed() != 0 || pos != 40) {
            if (fake_cs - phase_cs > 400)
                fail_quit("Return: playing the marks didn't stop on 40 (at %d, speed %g)", pos, re_test_speed());
            return 0;
        }
        CHECK(fake_cs - phase_cs >= 100, "Return: 10 to 40 played in %u cs (not about 120)", fake_cs - phase_cs);
        next(155);
        return key_to(b, TL, 'I');
    case 155:
        if (nulls)
            return 0;
        CHECK(pos == 10, "Shift-I: at %d, not the in mark (10)", pos);
        next(156);
        return key_to(b, TL, '\'');
    case 156:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c10 c64") && pos == 10 && re_test_mark(0) == -1, "' (Extract) 10-40: %s, at %d, mark %d",
              layout(), pos, re_test_mark(0));
        next(157);
        return key_to(b, TL, 0x188);
    case 157:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 c75"), "one F8 for an extract: %s", layout());
        next(158);
        return key_to(b, TL, 'i'); /* (at 10) */
    case 158:
        next(159);
        return click_tl(b, 40, 1, 0x400);
    case 159:
        if (nulls)
            return 0;
        next(160);
        return key_to(b, TL, 'o');
    case 160:
        next(161);
        return key_to(b, TL, ';');
    case 161:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c10 g31 c64"), "; (Lift) 10-40: %s", layout());
        next(162);
        return key_to(b, TL, 0x188);
    case 162:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 c75"), "undoing a lift: %s", layout());
        menus_mark = menus;
        next(170);
        data_load_v2(b, 20, 0xB60, "full.png"); /* a PNG on V2 at frame 20: an overlay */
        return 17;
    /* ---- overlays: a picture dropped on V2; its menu; dragged; deleted; a sprite file's sprite ---- */
    case 170: {
        if (nulls)
            return 0;
        re_overlay o;
        CHECK(re_project_overlays(re_test_project()) == 1 && !re_project_overlay(re_test_project(), 0, &o)
                  && o.start == 20 && o.length == 85 && o.place == RE_PLACE_FULL,
              "overlays: a PNG dropped on V2: %d, at %d for %d, place %d", re_project_overlays(re_test_project()),
              o.start, o.length, o.place);
        CHECK(re_test_overlay_selected() == 0 && re_test_selected() == -1 && !strcmp(layout(), "c30 c75"),
              "overlays: dropped: selected %d (%d), %s", re_test_overlay_selected(), re_test_selected(), layout());
        CHECK(pos == 20, "overlays: dropped: the playhead at %d, not its start", pos);
        shot("17-overlay-dropped");
        next(171);
        return click_tl(b, 30, 2, 2); /* Menu over it */
    }
    case 171:
        CHECK(menus == menus_mark + 1, "overlays: Menu over it: no menu");
        next(172);
        b[0] = 0; b[1] = 3; b[2] = -1; /* Place > Top right */
        return 9;
    case 172: {
        if (nulls)
            return 0;
        re_overlay o;
        re_project_overlay(re_test_project(), 0, &o);
        CHECK(o.place == RE_PLACE_TOP_RIGHT && o.size == 33, "overlays: Place > Top right: %d, size %d", o.place,
              o.size);
        next(173);
        b[0] = 2; b[1] = 2; b[2] = -1; /* Opacity > 50% */
        return 9;
    }
    case 173: {
        if (nulls)
            return 0;
        re_overlay o;
        re_project_overlay(re_test_project(), 0, &o);
        CHECK(o.opacity == 50, "overlays: Opacity > 50%%: %d", o.opacity);
        next(174);
        b[0] = 3; b[1] = -1; /* Fade in */
        return 9;
    }
    case 174: {
        if (nulls)
            return 0;
        re_overlay o;
        re_project_overlay(re_test_project(), 0, &o);
        CHECK(o.fade_in == 25, "overlays: Fade in: %d", o.fade_in);
        shot("18-overlay-placed");
        next(175);
        return click_tl(b, 60, 2, 0x40); /* its middle dragged */
    }
    case 175:
        ptr[0] = W[1].box[0] - W[1].scroll_x + 16 + 50 * 4; /* (10 frames back) */
        ptr[1] = W[1].box[3] - TL_V2_MID;
        next(176);
        return script(b, mask);
    case 176: {
        re_overlay o;
        re_project_overlay(re_test_project(), 0, &o);
        if (o.start == 10) {
            next(177);
            return 7; /* dropped */
        }
        if (fake_cs - phase_cs > 100)
            fail_quit("overlays: dragged 10 frames back: at %d", o.start);
        return 0;
    }
    case 177: {
        if (nulls)
            return 0;
        re_overlay o;
        re_project_overlay(re_test_project(), 0, &o);
        CHECK(o.start == 10 && o.length == 85 && o.fade_in == 25, "overlays: moved: at %d for %d, fade %d", o.start,
              o.length, o.fade_in);
        next(178);
        return key_to(b, TL, 0x188); /* F8 */
    }
    case 178: {
        if (nulls)
            return 0;
        re_overlay o;
        re_project_overlay(re_test_project(), 0, &o);
        CHECK(o.start == 20, "overlays: the move undone in one: at %d", o.start);
        help_text[0] = 0;
        memset(b, 0, 256);
        b[0] = 40; b[1] = 0x555; b[2] = 57; b[4] = 0x502;
        b[5] = W[1].box[0] + 100; b[6] = W[1].box[3] - TL_V2_MID; b[8] = TL; b[9] = -1; /* help on V2 */
        next(179);
        return 17;
    }
    case 179:
        CHECK(strstr(help_text, "V2") && strstr(help_text, "PNG"), "help on V2: \"%s\"", help_text);
        next(180);
        return click_tl(b, 30, 2, 0x400); /* selected */
    case 180:
        if (nulls)
            return 0;
        CHECK(re_test_overlay_selected() == 0, "overlays: a click on it selected %d", re_test_overlay_selected());
        next(181);
        return key_to(b, TL, 0x7F); /* Delete */
    case 181:
        if (nulls)
            return 0;
        CHECK(re_project_overlays(re_test_project()) == 0 && !strcmp(layout(), "c30 c75")
                  && re_test_overlay_selected() == -1,
              "overlays: Delete: %d left, V1 %s", re_project_overlays(re_test_project()), layout());
        two_sprites("spr2");
        remove("spr2_second.png");
        menus_mark = menus;
        next(182);
        data_load_v2(b, 0, 0xFF9, "spr2"); /* a sprite file of two: a menu of them */
        return 17;
    case 182:
        CHECK(menus == menus_mark + 1, "overlays: a sprite file of two: no menu");
        next(183);
        b[0] = 1; b[1] = -1; /* "second" */
        return 9;
    case 183: {
        if (nulls)
            return 0;
        re_overlay o;
        FILE *f = fopen("spr2_second.png", "rb");
        CHECK(f != NULL, "overlays: the sprite wasn't saved as spr2_second.png");
        if (f)
            fclose(f);
        CHECK(re_project_overlays(re_test_project()) == 1 && !re_project_overlay(re_test_project(), 0, &o)
                  && o.start == 0 && o.place == RE_PLACE_TOP_RIGHT && o.size == 20 && !strcmp(o.name, "spr2_second"),
              "overlays: the sprite: %d, at %d, place %d, size %d, \"%s\"", re_project_overlays(re_test_project()),
              o.start, o.place, o.size, o.name);
        shot("19-sprite-overlay");
        next(184);
        data_load(b, TL, W[1].box[0] - W[1].scroll_x + 16 + 104 * 4, 0xB60, "full.png"); /* on V1: a clip */
        return 17;
    }
    case 184:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 c75 c125"), "overlays: a picture on V1: %s", layout());
        next(185);
        return key_to(b, TL, 0x188);
    case 185:
        if (nulls)
            return 0;
        next(186);
        return key_to(b, TL, 0x188);
    case 186:
        if (nulls)
            return 0;
        CHECK(!strcmp(layout(), "c30 c75") && re_project_overlays(re_test_project()) == 0,
              "overlays: undone: %s, %d overlays", layout(), re_project_overlays(re_test_project()));
        next(187);
        data_load_v2(b, 5, 0xFFD, clip2); /* a video on V2: a picture in the picture */
        return 17;
    case 187: {
        if (nulls)
            return 0;
        re_overlay o;
        CHECK(re_project_overlays(re_test_project()) == 1 && !re_project_overlay(re_test_project(), 0, &o) && o.video
                  && o.start == 5 && o.length == 75 && o.place == RE_PLACE_BOTTOM_RIGHT && !strcmp(layout(), "c30 c75"),
              "overlays: a video on V2: %d, video %d, at %d for %d, place %d; V1 %s",
              re_project_overlays(re_test_project()), o.video, o.start, o.length, o.place, layout());
        shot("20-video-overlay");
        next(188);
        return click_tl(b, 6, 2, 0x40); /* by its start: trimmed */
    }
    case 188:
        ptr[0] = W[1].box[0] - W[1].scroll_x + 16 + 16 * 4; /* (10 frames on) */
        ptr[1] = W[1].box[3] - TL_V2_MID;
        next(189);
        return script(b, mask);
    case 189: {
        re_overlay o;
        re_project_overlay(re_test_project(), 0, &o);
        if (o.start == 15) {
            CHECK(o.in == 10 && o.length == 65, "overlays: the video's start trimmed: in %d, %d long", o.in, o.length);
            next(190);
            return 7;
        }
        if (fake_cs - phase_cs > 100)
            fail_quit("overlays: the video's start dragged 10 frames on: at %d", o.start);
        return 0;
    }
    case 190:
        if (nulls)
            return 0;
        next(191);
        return key_to(b, TL, 0x188);
    case 191:
        if (nulls)
            return 0;
        next(192);
        return key_to(b, TL, 0x188);
    case 192:
        if (nulls)
            return 0;
        CHECK(re_project_overlays(re_test_project()) == 0 && !strcmp(layout(), "c30 c75"),
              "overlays: the video undone: %d, %s", re_project_overlays(re_test_project()), layout());
        next(133);
        return click_tl(b, 50, 1, 0x400);
    case 133:
        if (nulls)
            return 0;
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
        shot("07-menu");
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
        shot("08-video-information");
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
        shot("09-detached-zoomed");
        next(26);
        return key(b, 0x183); /* F3 */
    /* ---- saving ---- */
    case 26:
        CHECK(savebox_opens == 1 && W[3].open, "F3 didn't open the save box");
        shot("10-save-box");
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
        mark = drags;
        b[0] = 0; b[1] = 0; b[2] = 0x40; b[3] = SAVEB; b[4] = 0; /* a drag of the file icon */
        return 6;
    case 36:
        CHECK(drags == mark + 1, "no drag started");
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
        next(240);
        return key_to(b, TL, 's'); /* a change */
    /* ---- titles: T opens the Title window; two lines typed, Size > Large, Create; the overlay
       menu's Edit title..., changed ---- */
    case 240:
        if (nulls)
            return 0;
        next(241);
        return key_to(b, TL, 't');
    case 241:
        if (nulls)
            return 0;
        CHECK(windows_made == 5 && W[4].open, "T didn't open the Title window (%d windows, open %d)", windows_made,
              W[4].open);
        strcpy(re_test_title_line(0), "Hello");
        strcpy(re_test_title_line(1), "ReelEdit");
        mark = menus;
        next(242);
        b[0] = W[4].box[0] + 600; b[1] = W[4].box[3] - 170; b[2] = 4; b[3] = TITLEW; b[4] = 6; /* Size: its menu */
        return 6;
    case 242:
        CHECK(menus == mark + 1, "the Size button gave no menu");
        next(243);
        b[0] = 3; b[1] = -1; /* Large */
        return 9;
    case 243:
        shot("23-title-window");
        mark = re_project_overlays(re_test_project());
        next(244);
        b[0] = W[4].box[2] - 50; b[1] = W[4].box[1] + 50; b[2] = 4; b[3] = TITLEW; b[4] = 16; /* Create */
        return 6;
    case 244: {
        if (nulls)
            return 0;
        re_overlay o;
        re_title t;
        int i = re_project_overlays(re_test_project()) - 1;
        CHECK(i == mark && !re_project_overlay(re_test_project(), i, &o) && o.title && !strcmp(o.name, "Hello")
                  && !re_project_title(re_test_project(), i, &t) && !strcmp(t.text, "Hello\nReelEdit") && t.size == 11
                  && o.place == RE_PLACE_LOWER_THIRD && !W[4].open,
              "Create: %d overlays (%d before), \"%s\", size %d, place %d, window open %d", i + 1, mark, o.name, t.size,
              o.place, W[4].open);
        shot("24-title");
        next(245);
        return click_tl(b, o.start + 10, 3, 2); /* Menu over the title, on the titles' row */
    }
    case 245:
        next(246);
        b[0] = 6; b[1] = -1; /* Edit title... */
        return 9;
    case 246:
        if (nulls)
            return 0;
        CHECK(W[4].open && !strcmp(re_test_title_line(0), "Hello") && !strcmp(re_test_title_line(1), "ReelEdit"),
              "Edit title...: open %d, \"%s\" / \"%s\"", W[4].open, re_test_title_line(0), re_test_title_line(1));
        strcpy(re_test_title_line(0), "Bye");
        next(247);
        return key_to(b, TITLEW, 13); /* (Return on the second line: changed) */
    case 247: {
        if (nulls)
            return 0;
        re_overlay o;
        int i = re_project_overlays(re_test_project()) - 1;
        CHECK(!re_project_overlay(re_test_project(), i, &o) && !strcmp(o.name, "Bye") && i == mark && !W[4].open,
              "Edit title... changed: \"%s\", %d overlays, open %d", o.name, i + 1, W[4].open);
        next(250);
        return key_to(b, TL, 0x18D); /* (Right: a frame on) */
    }
    /* ---- export: Ctrl-E, Size > 720 lines, a full name, Export: the TaskWindow started with
       -export; its output (progress, done) and its end shown; again, and stopped ---- */
    case 250:
        if (nulls)
            return 0;
        mark2_reports = reports;
        next(251);
        return key_to(b, TL, 5);
    case 251: {
        if (nulls)
            return 0;
        const char *nm = re_test_export_name();
        CHECK(windows_made == 6 && W[5].open && strlen(nm) > 4 && !strcmp(nm + strlen(nm) - 4, "/mp4")
                  && strstr(re_test_export_status(), "Drag"),
              "Ctrl-E: %d windows, open %d, \"%s\", \"%s\"", windows_made, W[5].open, nm, re_test_export_status());
        mark = menus;
        next(252);
        b[0] = W[5].box[0] + 200; b[1] = W[5].box[3] - 226; b[2] = 4; b[3] = EXPW; b[4] = 3; /* Size: its menu */
        return 6;
    }
    case 252:
        CHECK(menus == mark + 1, "the export's Size button gave no menu");
        next(253);
        b[0] = 2; b[1] = -1; /* 720 lines */
        return 9;
    case 253:
        shot("25-export");
        snprintf(re_test_export_name(), 256, "ADFS::Disc.$.Video/mp4");
        next(254);
        b[0] = W[5].box[2] - 50; b[1] = W[5].box[1] + 40; b[2] = 4; b[3] = EXPW; b[4] = 8; /* Export */
        return 6;
    case 254:
        CHECK(starts == 1 && strstr(start_cmd, "TaskWindow") && strstr(start_cmd, "-export")
                  && strstr(start_cmd, " ADFS::Disc.$.Video/mp4 720 1 0\"") && re_project_modified(re_test_project()),
              "Export: %d tasks started, \"%s\", modified %d", starts, start_cmd,
              re_project_modified(re_test_project()));
        next(255);
        memset(b, 0, 24);
        b[0] = 24; b[1] = 0x777; b[2] = 70; b[4] = 0x808C2; b[5] = 0x52454558; /* TaskWindow_Ego */
        return 17;
    case 255:
        next(256);
        memset(b, 0, 64);
        b[1] = 0x777; b[2] = 71; b[4] = 0x808C1;
        b[5] = (int) strlen("progress 5 10\nprog");
        memcpy(&b[6], "progress 5 10\nprog", b[5]);
        b[0] = 24 + ((b[5] + 3) & ~3);
        return 17;
    case 256:
        CHECK(strstr(re_test_export_status(), "50%") != NULL, "the export's progress: \"%s\"", re_test_export_status());
        next(257);
        memset(b, 0, 64);
        b[1] = 0x777; b[2] = 72; b[4] = 0x808C1;
        b[5] = (int) strlen("ress 10 10\ndone\n");
        memcpy(&b[6], "ress 10 10\ndone\n", b[5]);
        b[0] = 24 + ((b[5] + 3) & ~3);
        return 17;
    case 257:
        next(258);
        memset(b, 0, 24);
        b[0] = 20; b[1] = 0x777; b[2] = 73; b[4] = 0x808C3; /* TaskWindow_Morio */
        return 17;
    case 258:
        CHECK(exports_typed == 1 && !strcmp(re_test_export_status(), "Exported: Video/mp4") && W[5].open,
              "the export done: typed %d, \"%s\"", exports_typed, re_test_export_status());
        next(259);
        b[0] = W[5].box[2] - 50; b[1] = W[5].box[1] + 40; b[2] = 4; b[3] = EXPW; b[4] = 8; /* Export again */
        return 6;
    case 259:
        CHECK(starts == 2, "Export again: %d tasks started", starts);
        next(260);
        memset(b, 0, 24);
        b[0] = 24; b[1] = 0x778; b[2] = 74; b[4] = 0x808C2; b[5] = 0x52454558;
        return 17;
    case 260:
        next(261);
        b[0] = W[5].box[2] - 50; b[1] = W[5].box[1] + 40; b[2] = 4; b[3] = EXPW; b[4] = 8; /* Stop */
        return 6;
    case 261:
        CHECK(morites == 1 && strstr(re_test_export_status(), "Stopping"), "Stop: %d killed, \"%s\"", morites,
              re_test_export_status());
        next(262);
        memset(b, 0, 24);
        b[0] = 20; b[1] = 0x778; b[2] = 75; b[4] = 0x808C3;
        return 17;
    case 262:
        CHECK(!strcmp(re_test_export_status(), "Stopped.") && exports_typed == 1 && reports == mark2_reports,
              "stopped: \"%s\", typed %d, reports %d", re_test_export_status(), exports_typed, reports - mark2_reports);
        next(270);
        return key_to(b, TL, 0x18D);
    /* ---- audio tracks: a sound dropped below V1; another where it plays (stacked, a new track);
       Menu over it > Volume > -6 dB; Delete (the empty track goes) ---- */
    case 270:
        if (nulls)
            return 0;
        mark = W[1].box[3] - W[1].box[1];
        next(271);
        return drop_audio(b, 10, 0, "tone880.wav");
    case 271: {
        if (nulls)
            return 0;
        re_audio a;
        CHECK(re_project_audio_tracks(re_test_project()) == 1 && !re_project_audio(re_test_project(), 0, 0, &a)
                  && abs(a.start - 10) <= 1 && a.length == 50 && au_sel() == 0 && W[1].box[3] - W[1].box[1] > mark,
              "a sound dropped: %d tracks, at %d for %d, selected %d; the Timeline %d high (%d)",
              re_project_audio_tracks(re_test_project()), a.start, a.length, au_sel(), W[1].box[3] - W[1].box[1], mark);
        shot("26-audio");
        next(272);
        return drop_audio(b, 20, 0, "tone1320.wav");
    }
    case 272:
        if (nulls)
            return 0;
        CHECK(re_project_audio_tracks(re_test_project()) == 2 && re_project_audio_count(re_test_project(), 1) == 1
                  && au_track_sel() == 1,
              "a sound where one plays: %d tracks, %d on A2, selected on A%d", re_project_audio_tracks(re_test_project()),
              re_project_audio_count(re_test_project(), 1), au_track_sel() + 1);
        shot("27-audio-stacked");
        {
            re_audio a;
            re_project_audio(re_test_project(), 1, 0, &a);
            mark = a.start;
            click_audio(b, a.start + 25, 1, 0x40); /* its middle dragged 10 frames on */
            ptr[0] = b[0] + (int) (10 * tl_scale); ptr[1] = b[1];
        }
        next(280);
        return 6;
    case 280: {
        re_audio a;
        re_project_audio(re_test_project(), 1, 0, &a);
        if (a.start != mark + 10) {
            if (fake_cs - phase_cs > 100)
                fail_quit("a sound dragged 10 frames on: at %d (was %d)", a.start, mark);
            return 0;
        }
        next(281);
        return 7; /* dropped */
    }
    case 281:
        if (nulls)
            return 0;
        mark = menus;
        next(273);
        return click_audio(b, 40, 1, 2); /* Menu over it */
    case 273:
        CHECK(menus == mark + 1, "Menu over a sound: no menu");
        next(274);
        b[0] = 3; b[1] = 5; b[2] = -1; /* Volume > -6 dB */
        return 9;
    case 274: {
        if (nulls)
            return 0;
        re_audio a;
        CHECK(!re_project_audio(re_test_project(), 1, 0, &a) && a.volume == -6, "the sound's Volume > -6 dB: %d",
              a.volume);
        next(275);
        return key_to(b, TL, 0x7F); /* Delete: the selected sound */
    }
    case 275:
        if (nulls)
            return 0;
        CHECK(re_project_audio_tracks(re_test_project()) == 1 && au_sel() == -1,
              "Delete: %d tracks (the empty one gone), selected %d", re_project_audio_tracks(re_test_project()), au_sel());
        mark = W[0].box[2] - W[0].box[0];
        next(276);
        b[0] = W[0].box[0] + 200; b[1] = W[0].box[3] - 100; b[2] = 2; b[3] = VIEWER; b[4] = -1; /* Menu */
        return 6;
    /* ---- View > Viewer size > Large: the Viewer wider, the Timeline still under it, centred ---- */
    case 276:
        next(277);
        b[0] = 3; b[1] = 5; b[2] = 2; b[3] = -1;
        return 9;
    case 277:
        if (nulls)
            return 0;
        /* (the Timeline was detached at phase 67: it stays where it is) */
        CHECK(W[0].box[2] - W[0].box[0] > mark * 3 / 2 && W[0].open,
              "View > Viewer size > Large: %d wide (%d)", W[0].box[2] - W[0].box[0], mark);
        shot("28-viewer-large");
        next(300);
        return key_to(b, TL, 0x18D);
    /* ---- autosave: an edit half a minute on makes a copy; ReelEdit "reset"; opened again: Recover,
       the project as it was, still to be saved ---- */
    case 300: {
        if (nulls)
            return 0;
        fake_cs += 4000; /* (40 s on) */
        remove(AUTOSAVE);
        next(301);
        return key_to(b, TL, 's'); /* an edit: split */
    }
    case 301: {
        if (nulls)
            return 0;
        FILE *f = fopen(AUTOSAVE, "r");
        CHECK(f != NULL, "an edit 40 s on: no autosaved copy");
        if (f)
            fclose(f);
        snprintf(saved_layout, sizeof(saved_layout), "%s", layout());
        snprintf(saved_title, sizeof(saved_title), "%s", re_test_title());
        re_test_crash();
        answer = 3; /* Recover */
        mark = questions;
        next(302);
        b[0] = 100; b[1] = 20; b[2] = 4; b[3] = -2; b[4] = BAR_ICON; /* the icon: the editor */
        return 6;
    }
    case 302:
        if (nulls)
            return 0;
        CHECK(questions == mark + 1 && W[0].open && re_test_project() && !strcmp(layout(), saved_layout)
                  && re_project_modified(re_test_project()) && !strcmp(re_test_title(), saved_title),
              "recovered: %d questions, open %d, \"%s\" (\"%s\"), modified %d, title \"%s\" (\"%s\")",
              questions - mark, W[0].open, layout(), saved_layout,
              re_test_project() ? re_project_modified(re_test_project()) : -1, re_test_title(), saved_title);
        next(310);
        return key_to(b, TL, 0x18D);
    /* ---- the Clips window (Ctrl-B): a clip double-clicked, shown as a source; I, Shift-Right, O;
       , inserts it at the playhead; . overwrites; Escape: the project; a row dragged to the
       Timeline; Delete takes one off the list ---- */
    case 310:
        if (nulls)
            return 0;
        next(311);
        return key_to(b, TL, 2);
    case 311:
        if (nulls)
            return 0;
        CHECK(windows_made == 7 && W[6].open && re_project_clips(re_test_project()) >= 2,
              "Ctrl-B: %d windows, open %d, %d clips", windows_made, W[6].open, re_project_clips(re_test_project()));
        shot("29-clips");
        next(312);
        b[0] = W[6].box[0] + 100; b[1] = W[6].box[3] - 56 - 20; b[2] = 0x400; b[3] = CLIPW; b[4] = -1;
        return 6;
    case 312:
        CHECK(re_test_clips_selected() == 0, "a click on the first clip: selected %d", re_test_clips_selected());
        mark = re_project_length(re_test_project());
        next(313);
        b[0] = W[6].box[0] + 100; b[1] = W[6].box[3] - 56 - 20; b[2] = 4; b[3] = CLIPW; b[4] = -1; /* double */
        return 6;
    case 313:
        if (nulls)
            return 0;
        CHECK(!strncmp(re_test_title(), "Source: ", 8), "double-clicked: the Viewer's title \"%s\"", re_test_title());
        next(314);
        return key_to(b, TL, 'i');
    case 314:
        if (nulls)
            return 0;
        next(315);
        return key_to(b, TL, 0x19D); /* Shift-Right: a second on, in the source */
    case 315:
        if (nulls)
            return 0;
        next(316);
        return key_to(b, TL, 'o');
    case 316:
        if (nulls)
            return 0;
        rec = app_position();
        next(317);
        return key_to(b, TL, ','); /* insert */
    case 317:
        if (nulls)
            return 0;
        CHECK(re_project_length(re_test_project()) == mark + 26 && !strncmp(re_test_title(), "Source: ", 8),
              ", (insert): the project %d frames (%d before), title \"%.20s\"", re_project_length(re_test_project()), mark,
              re_test_title());
        CHECK(app_position() == rec + 26, ", (insert): the playhead at %d (%d before): after the clip", app_position(), rec);
        mark = re_project_length(re_test_project());
        next(322);
        return key_to(b, TL, '.'); /* overwrite, after it */
    case 322:
        if (nulls)
            return 0;
        CHECK(re_project_length(re_test_project()) == mark, ". (overwrite): the project %d frames (%d before)",
              re_project_length(re_test_project()), mark);
        mark = re_project_items(re_test_project());
        next(318);
        return key_to(b, TL, 27); /* Escape: the project again */
    case 318:
        if (nulls)
            return 0;
        CHECK(strncmp(re_test_title(), "Source: ", 8) != 0, "Escape: the Viewer's title \"%s\"", re_test_title());
        shot("30-inserted");
        next(319);
        b[0] = W[6].box[0] + 100; b[1] = W[6].box[3] - 56 - 20; b[2] = 0x40; b[3] = CLIPW; b[4] = -1; /* a drag */
        return 6;
    case 319:
        /* dropped on the Timeline's V1, at frame 0 */
        ptr[0] = W[1].box[0] - W[1].scroll_x + 16; ptr[1] = W[1].box[3] - TL_V1_TOP - 48; ptr[3] = TL; ptr[4] = -1;
        next(320);
        return 7;
    case 320:
        if (nulls)
            return 0;
        CHECK(re_project_items(re_test_project()) == mark + 1, "a clip dragged to the Timeline: %d items (%d)",
              re_project_items(re_test_project()), mark);
        mark = re_project_clips(re_test_project());
        next(321);
        return key_to(b, CLIPW, 0x7F); /* Delete: off the list */
    case 321:
        if (nulls)
            return 0;
        CHECK(re_project_clips(re_test_project()) == mark - 1, "Delete in the Clips window: %d clips (%d)",
              re_project_clips(re_test_project()), mark);
        re_test_previews(&th, &wr);
        thumb_plots = 0;
        colours_seen = 0;
        next(323);
        b[0] = TL; /* (the Timeline drawn: its thumbnails and waveforms asked for, then made) */
        return 1;
    case 323: /* thumbnails and waveforms (previews.c): made, then drawn */
        if (nulls)
            return 0;
        mark = re_test_previews(&th, &wr);
        CHECK(mark == 0 && th > 0 && thumb_plots > 0 && wr > 0 && (colours_seen & 2),
              "previews: %d still to make; %d thumbnails (%d plotted), %d waveform bars drawn", mark, th, thumb_plots, wr);
        shot("31-previews");
        next(324);
        return key_to(b, TL, ' ');
    case 324: /* playing: split, so there's a new thumbnail to make */
        CHECK(re_test_speed() != 0, "previews: not playing");
        if (re_test_position() < 4)
            return 0;
        next(325);
        return key_to(b, TL, 's');
    case 325:
        if (fake_cs - phase_cs < 30)
            return 0;
        mark = re_test_previews(&th, &wr);
        CHECK(re_test_previews_stand_ins() > 0, "previews: no stand-in for the split's new thumbnail");
        CHECK(mark > 0, "previews: made while playing (%d to make; %s at %d, speed %g)", mark, layout(), re_test_position(), re_test_speed());
        next(326);
        return key_to(b, TL, ' ');
    case 326:
        if (re_test_speed() == 0 && !re_test_busy() && re_test_previews_pending() > 0 && !nulls)
            fail_quit("previews: some to make, but null events masked");
        if (nulls)
            return 0;
        mark = re_test_previews(&th, &wr);
        CHECK(re_test_speed() == 0 && mark == 0 && th > 0 && re_test_previews_stand_ins() == 0,
              "previews: stopped, %d to make, %d drawn, %d stand-ins in the last redraw", mark, th,
              re_test_previews_stand_ins());
        /* ---- copy and paste (copypaste.c); rolls ---- */
        for (int i = 0; i < re_project_items(re_test_project()); i++) { /* (a clip 20 frames long or more) */
            re_item it;
            re_project_item(re_test_project(), i, &it);
            if (!it.gap && !it.mix && it.length >= 20) {
                rec = it.length;
                th = i;
                next(327);
                return click_tl(b, it.start + it.length / 2, 0, 0x400);
            }
        }
        fail_quit("copy: no clip 20 frames long to copy");
        return 0;
    case 327:
        if (nulls)
            return 0;
        CHECK(re_test_selected() == th, "copy: clip %d not selected (%d)", th, re_test_selected());
        mark = re_project_items(re_test_project());
        wr = re_project_length(re_test_project());
        next(328);
        return key_to(b, TL, 3); /* Ctrl-C */
    case 328:
        if (nulls)
            return 0;
        next(329);
        return key_to(b, TL, 0x1E); /* Home */
    case 329:
        if (nulls)
            return 0;
        next(330);
        return key_to(b, TL, 22); /* Ctrl-V */
    case 330: {
        if (nulls)
            return 0;
        re_item it;
        re_project_item(re_test_project(), 0, &it);
        CHECK(re_project_items(re_test_project()) == mark + 1 && re_test_selected() == 0 && it.length == rec
                  && pos == rec && re_project_length(re_test_project()) == wr + rec,
              "paste: %d items (%d), selected %d, the first %d long (%d), the playhead at %d", re_project_items(re_test_project()),
              mark + 1, re_test_selected(), it.length, rec, pos);
        shot("32-pasted");
        next(331);
        return key_to(b, TL, 24); /* Ctrl-X */
    }
    case 331:
        if (nulls)
            return 0;
        CHECK(re_project_items(re_test_project()) == mark && re_project_length(re_test_project()) == wr,
              "cut: %d items (%d)", re_project_items(re_test_project()), mark);
        next(332);
        return key_to(b, TL, 22); /* Ctrl-V: back */
    case 332:
        if (nulls)
            return 0;
        CHECK(re_project_items(re_test_project()) == mark + 1, "cut, pasted: %d items (%d)",
              re_project_items(re_test_project()), mark + 1);
        wr = re_project_length(re_test_project());
        next(333);
        return click_tl(b, rec / 2, 1, 0x400); /* (the ruler: the playhead to the pasted clip's middle) */
    case 333:
        if (nulls)
            return 0;
        CHECK(pos == rec / 2, "roll: the playhead at %d, not %d", pos, rec / 2);
        next(334);
        return key_to(b, TL, 's');
    case 334: {
        if (nulls)
            return 0;
        re_item it;
        re_project_item(re_test_project(), 0, &it);
        CHECK(it.length == rec / 2, "roll: split at %d: %d long", rec / 2, it.length);
        next(335);
        return click_tl(b, rec / 2 - 3, 1, 0x400);
    }
    case 335:
        if (nulls)
            return 0;
        next(336);
        return key_to(b, TL, 'e'); /* the cut 3 frames on rolled to the playhead */
    case 336: {
        if (nulls)
            return 0;
        re_item it;
        re_project_item(re_test_project(), 0, &it);
        CHECK(it.length == rec / 2 - 3 && re_project_length(re_test_project()) == wr
                  && re_project_items(re_test_project()) == mark + 2,
              "roll (E): the first clip %d long (%d), the project %d (%d)", it.length, rec / 2 - 3,
              re_project_length(re_test_project()), wr);
        ctrl_key = 1; /* (a Ctrl-drag at the cut: a roll) */
        next(337);
        b[0] = W[1].box[0] - W[1].scroll_x + 16 + (int) ((rec / 2 - 3) * tl_scale) - 4;
        b[1] = W[1].box[3] - TL_V1_TOP - 48;
        b[2] = 0x40; b[3] = TL; b[4] = -1;
        return 6;
    }
    case 337:
        ptr[0] = W[1].box[0] - W[1].scroll_x + 16 + (int) ((rec / 2 - 3) * tl_scale) - 4 + (int) (5 * tl_scale);
        ptr[1] = W[1].box[3] - 100;
        next(338);
        return script(b, mask);
    case 338: {
        re_item it;
        re_project_item(re_test_project(), 0, &it);
        if (it.length == rec / 2 + 2 && !re_test_busy()) {
            CHECK(re_project_length(re_test_project()) == wr && re_project_items(re_test_project()) == mark + 2,
                  "roll (Ctrl-drag): the project %d long (%d), %d items", re_project_length(re_test_project()), wr,
                  re_project_items(re_test_project()));
            ctrl_key = 0;
            next(339);
            return 7; /* the drag ends */
        }
        if (fake_cs - phase_cs > 100)
            fail_quit("roll: a Ctrl-drag 5 frames on: the first clip %d long, not %d", it.length, rec / 2 + 2);
        return nulls ? 0 : (fail_quit("null events masked while rolling"), 0);
    }
    case 339:
        if (nulls)
            return 0;
        next(340);
        return key_to(b, TL, 0x188); /* F8 */
    case 340: {
        if (nulls)
            return 0;
        re_item it;
        re_project_item(re_test_project(), 0, &it);
        CHECK(it.length == rec / 2 - 3, "roll: one F8 for the drag: %d long (%d)", it.length, rec / 2 - 3);
        /* ---- speed: the context menu's Speed > 200%, Reversed; F8 ---- */
        mark = it.length;
        wr = re_project_length(re_test_project());
        next(341);
        return click_tl(b, 2, 0, 2); /* Menu over the first clip */
    }
    case 341:
        if (nulls)
            return 0;
        next(342);
        b[0] = 3; b[1] = 1; b[2] = -1; /* Speed > 200% */
        return 9;
    case 342: {
        if (nulls)
            return 0;
        re_item it;
        re_project_item(re_test_project(), 0, &it);
        CHECK(it.speed == 200 && abs(it.length - (mark + 1) / 2) <= 1
                  && re_project_length(re_test_project()) == wr - mark + it.length,
              "Speed > 200%%: %d%%, %d long (from %d), the project %d", it.speed, it.length, mark,
              re_project_length(re_test_project()));
        shot("33-speed");
        next(343);
        return click_tl(b, 2, 0, 2);
    }
    case 343:
        if (nulls)
            return 0;
        next(344);
        b[0] = 3; b[1] = 8; b[2] = -1; /* Speed > Reversed */
        return 9;
    case 344: {
        if (nulls)
            return 0;
        re_item it;
        re_project_item(re_test_project(), 0, &it);
        CHECK(it.speed == -200, "Speed > Reversed: %d%%", it.speed);
        next(345);
        return key_to(b, TL, 0x188); /* F8 */
    }
    case 345: {
        if (nulls)
            return 0;
        re_item it;
        re_project_item(re_test_project(), 0, &it);
        CHECK(it.speed == 200, "Speed: F8: %d%%", it.speed);
        /* ---- render previews: Ctrl-R; each stretch's TaskWindow played by
           the script (the export made here, from the command); played from them ---- */
        render_starts = starts;
        colours_seen = 0;
        next(346);
        return key_to(b, TL, 18); /* Ctrl-R */
    }
    case 346: { /* a render started: made here; the TaskWindow's messages */
        char proj[300], outf[300];
        int h = 0, q = 0, hw = 0, in = 0, out = 0;
        const char *x = strstr(start_cmd, "-export ");
        CHECK(starts == render_starts + 1 && x && sscanf(x, "-export %299s %299s %d %d %d %d %d", proj, outf, &h, &q, &hw, &in, &out) == 7
                  && q == 2 && out >= in,
              "render: %d started (%d before): \"%s\"", starts, render_starts, start_cmd);
        if (!x)
            fail_quit("render: not started");
        re_export_settings es = {h, q, 0, "mpeg4", 1, in, out};
        char err[200];
        CHECK(re_export(proj, outf, &es, NULL, NULL, NULL, err, sizeof(err)) == 0, "render: frames %d-%d: %s", in, out, err);
        render_jobs++;
        render_starts = starts;
        next(347);
        memset(b, 0, 24);
        b[0] = 24; b[1] = 0x780 + render_jobs; b[2] = 80; b[4] = 0x808C2; b[5] = 0x52455056; /* TaskWindow_Ego */
        return 17;
    }
    case 347: {
        int s0, l0, m0;
        CHECK(rr_stretches(0, &s0, &l0, &m0) >= 0, "render: no stretches");
        next(348);
        memset(b, 0, 64);
        b[1] = 0x780 + render_jobs; b[2] = 81; b[4] = 0x808C1;
        b[5] = (int) strlen("progress 1 2\ndone\n");
        memcpy(&b[6], "progress 1 2\ndone\n", b[5]);
        b[0] = 24 + ((b[5] + 3) & ~3);
        return 17;
    }
    case 348:
        for (int i = 0, s0, l0, m0, st; (st = rr_stretches(i, &s0, &l0, &m0)) >= 0; i++)
            render_progress_seen |= st == 2 && m0 == 1;
        next(349);
        memset(b, 0, 24);
        b[0] = 20; b[1] = 0x780 + render_jobs; b[2] = 82; b[4] = 0x808C3; /* TaskWindow_Morio */
        return 17;
    case 349:
        CHECK(re_test_renders() == render_jobs, "render: %d previews kept after %d jobs", re_test_renders(), render_jobs);
        if (starts > render_starts && render_jobs < 20) { /* (the next stretch started) */
            render_starts = starts - 1;
            next(346);
            return script(b, mask);
        }
        {
            int all = 1, n = 0;
            for (int i = 0, s0, l0, m0, st; (st = rr_stretches(i, &s0, &l0, &m0)) >= 0; i++, n++)
                all &= st == 1;
            CHECK(all && n == render_jobs && render_progress_seen, "render: %d stretches, %d rendered, all %d, progress %d",
                  n, render_jobs, all, render_progress_seen);
        }
        CHECK(colours_seen & 4, "render: the rendered stretches not drawn green");
        shot("34-rendered");
        next(350);
        return key_to(b, TL, ' '); /* play: from the previews */
    case 350:
        if (fake_cs - phase_cs < 20)
            return 0;
        CHECK(re_test_speed() != 0 && re_test_previews_playing(), "render: playing from the previews: %d",
              re_test_previews_playing());
        next(351);
        return key_to(b, TL, ' ');
    case 351:
        if (nulls)
            return 0;
        CHECK(!re_test_previews_playing(), "render: stopped, still the previews");
        next(352);
        return key_to(b, TL, ' ');
    case 352: /* an edit while playing: the project again */
        if (fake_cs - phase_cs < 10)
            return 0;
        next(353);
        return key_to(b, TL, 's');
    case 353:
        CHECK(!re_test_previews_playing(), "render: edited while playing, still the previews");
        next(354);
        return key_to(b, TL, ' ');
    case 354:
        if (nulls)
            return 0;
        /* ---- proxies: a big clip dropped at the end; its proxy's TaskWindow
           played by the script (made here, from the command); used; View >
           Use proxies off and on ---- */
        proxy_starts = starts;
        next(355);
        data_load(b, TL, W[1].box[0] - W[1].scroll_x + 16 + (int) ((re_project_length(re_test_project()) + 5) * tl_scale),
                  0xBF6, clip4);
        b[8] = W[1].box[3] - TL_V1_TOP - 48;
        return 17;
    case 355: {
        if (nulls)
            return 0;
        char file[300], outf[300];
        int hw = -1;
        const char *x = strstr(start_cmd, "-proxy ");
        CHECK(starts == proxy_starts + 1 && x && sscanf(x, "-proxy %299s %299s %d", file, outf, &hw) == 3
                  && strstr(file, "big.mp4") && strstr(re_test_tl_title(), "making proxies"),
              "proxy: %d started (%d before): \"%s\"; the Timeline \"%s\"", starts, proxy_starts, start_cmd,
              re_test_tl_title());
        if (!x)
            fail_quit("proxy: not started");
        char err[200];
        CHECK(re_export_proxy(file, outf, 0, NULL, NULL, NULL, err, sizeof(err)) == 0, "proxy: %s", err);
        snprintf(proxy_made, sizeof(proxy_made), "%s", outf);
        next(356);
        memset(b, 0, 24);
        b[0] = 24; b[1] = 0x790; b[2] = 90; b[4] = 0x808C2; b[5] = 0x52455058; /* TaskWindow_Ego */
        return 17;
    }
    case 356:
        next(357);
        memset(b, 0, 64);
        b[1] = 0x790; b[2] = 91; b[4] = 0x808C1;
        b[5] = (int) strlen("done\n");
        memcpy(&b[6], "done\n", b[5]);
        b[0] = 24 + ((b[5] + 3) & ~3);
        return 17;
    case 357:
        next(358);
        memset(b, 0, 24);
        b[0] = 20; b[1] = 0x790; b[2] = 92; b[4] = 0x808C3; /* TaskWindow_Morio */
        return 17;
    case 358: {
        if (nulls)
            return 0;
        int i = big_item();
        const char *f = i >= 0 ? re_project_item_file(re_test_project(), i) : NULL;
        CHECK(f && !strcmp(f, proxy_made) && !strcmp(re_test_tl_title(), "Timeline") && starts == proxy_starts + 1,
              "proxy: the clip's file %s (%s), the Timeline \"%s\"", f ? f : "-", proxy_made, re_test_tl_title());
        shot("35-proxy");
        next(359);
        b[0] = W[0].box[0] + 50; b[1] = W[0].box[3] - 50; b[2] = 2; b[3] = VIEWER; b[4] = -1;
        return 6; /* Menu */
    }
    case 359:
        next(360);
        b[0] = 3; b[1] = 7; b[2] = -1; /* View > Use proxies: off */
        return 9;
    case 360: {
        if (nulls)
            return 0;
        int i = big_item();
        const char *f = i >= 0 ? re_project_item_file(re_test_project(), i) : NULL;
        CHECK(f && strstr(f, "big.mp4"), "proxy: off: the clip's file %s", f ? f : "-");
        next(361);
        b[0] = W[0].box[0] + 50; b[1] = W[0].box[3] - 50; b[2] = 2; b[3] = VIEWER; b[4] = -1;
        return 6;
    }
    case 361:
        next(362);
        b[0] = 3; b[1] = 7; b[2] = -1; /* on */
        return 9;
    case 362: {
        if (nulls)
            return 0;
        int i = big_item();
        const char *f = i >= 0 ? re_project_item_file(re_test_project(), i) : NULL;
        CHECK(f && !strcmp(f, proxy_made), "proxy: on again: the clip's file %s", f ? f : "-");
        next(45);
        return key_to(b, TL, 0x18D);
    }
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
    if (argc != 5) {
        fprintf(stderr, "usage: editor_test numbered-snd.mp4 numbered.mp4 numbered.m2v big.mp4\n");
        return 2;
    }
    clip4 = argv[4];
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
    scr_init();
    remove(AUTOSAVE); /* (a copy left by a run before: none) */
    int r = reeledit_main(0, NULL);
    CHECK(r == 0, "reeledit_main returned %d", r);
    CHECK(phase == 46, "the script stopped at phase %d", phase);
    FILE *left = proxy_made[0] ? fopen(proxy_made, "rb") : NULL;
    CHECK(proxy_made[0] && !left, "proxy: %s left after quitting", proxy_made);
    if (left)
        fclose(left);
    CHECK(questions == mark + 1 && prequit_acks == 1, "Message_PreQuit: %d questions, %d acknowledgements",
          questions - mark, prequit_acks);
    CHECK(key_sent == 0x1FC, "the shutdown not started again (key &%X sent)", key_sent);
    CHECK(closedown == 1, "Wimp_CloseDown not called");
    FILE *as = fopen(AUTOSAVE, "r");
    CHECK(!as, "quit: the autosaved copy left");
    if (as)
        fclose(as);
    /* ---- logging off as standard: no ReelEdit$Log, no choice; then chosen ---- */
    lg_end();
    unsetenv("ReelEdit$Log");
    remove(LOG_CHOICE);
    lg_start();
    CHECK(!lg_on(), "logging on with nothing asking for it");
    FILE *ch = fopen(LOG_CHOICE, "w");
    if (ch) {
        fputs("on\n", ch);
        fclose(ch);
    }
    lg_start();
    CHECK(lg_on(), "logging off though chosen");
    lg_end();
    remove(LOG_CHOICE);
    remove("<Wimp$ScrapDir>.ReelEditLog");
    printf("editor_test: %d polls, %d null events, %d Wimp_PollIdle, %u cs asleep; overlay %d pictures; "
           "%u bytes of sound\n", polls, null_events, idle_polls, slept, ov_displays, ssb_bytes);
    if (S.on)
        printf("editor_test: %d screenshots in %s\n", S.shots, S.dir);
    printf("editor_test: %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
}
