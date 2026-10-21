/* viewer_test - !ReelEdit's Viewer (src/riscos/viewer.c, display.c,
 * sound.c) driven by a scripted fake Wimp, with the real MLT and FFmpeg,
 * and fake VideoOverlay, SharedSoundBuffer and clock.
 *
 *   viewer_test numbered-snd.mp4
 * numbered-snd.mp4: 320x180, 25 fps, 75 frames, frame n a grey of luma
 * 16 + 2n; a 440 Hz tone at 44.1 kHz (played at 48 kHz: MLT resamples).
 *
 * The script: a directory dropped on the icon bar icon is refused; the clip
 * dropped on it opens the Viewer with frame 0 drawn in the window (a
 * sprite), and then no null events (nothing used while stopped); Right
 * draws frame 1; Play plays at the clock through the overlay, with
 * Wimp_PollIdle between pictures and the sound given to StreamManager; a
 * window over the picture hides the overlay (drawn in the window instead)
 * until it's moved away; Space stops, the overlay goes and the frame is
 * drawn in the window; L L plays at twice the speed, K stops, J plays
 * backwards; End then Play starts from the beginning and plays to the end,
 * where it stops by itself; closing the window closes the video and the
 * overlay; Message_Quit quits.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include <kernel.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

int reeledit_main(int argc, char **argv);
int re_test_position(void);
double re_test_speed(void);
int re_test_open(void);

static int fails;
#define CHECK(c, ...)                                                                                              \
    do {                                                                                                           \
        if (!(c)) {                                                                                                \
            fails++;                                                                                               \
            printf("FAIL (phase %d): ", phase);                                                                    \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
        }                                                                                                          \
    } while (0)

static const char *clip;
static int phase, phase_cs;

/* ---- the fake machine ------------------------------------------------------- */

#define WIN   0x1000
#define COVER 0x2000
#define TASK  0x4321
#define BAR_ICON 7
#define VO_BASE 0x5A000 /* our numbers for VideoOverlay's SWIs */
static const char *const vo_names[] = {
    "VideoOverlay_Create", "VideoOverlay_Destroy", "VideoOverlay_DisplayBuffer", "VideoOverlay_MapBuffer",
    "VideoOverlay_UnmapBuffer", "VideoOverlay_SetScale", "VideoOverlay_SetWindow", "VideoOverlay_SetPosition",
    "VideoOverlay_RedrawWindow"};

static unsigned fake_cs = 1000;
static unsigned slept;          /* centiseconds slept in Wimp_PollIdle */
static int polls, idle_polls, null_events, last_mask;
static int win_open, win_box[4], win_behind, covered;
static int win_made, opens;
static int updates, redraws, plots, force_redraws, reports, closedown, acks;
static char last_report[256];
static int *plot_area;          /* the sprite last plotted */
static int redraw_due;
/* VideoOverlay */
static int ov_made, ov_destroyed, ov_shown_bank = -1, ov_displays, ov_w, ov_h;
static uint8_t *ov_buf[3];
static int ov_block[6];
static int ov_mapped = -1;
/* SharedSoundBuffer/StreamManager */
static int ssb_opens, ssb_closes, ssb_playing;
static unsigned ssb_bytes;
static double ssb_level;        /* the loudest sample given */
static _kernel_oserror err = {1, "fake: not here"};

static void fail_quit(const char *why)
{
    printf("FAIL: %s\n", why);
    fails++;
    exit(1);
}

static void window_state(int *b)
{
    int h = b[0];
    if (h == WIN) {
        b[1] = win_box[0]; b[2] = win_box[1]; b[3] = win_box[2]; b[4] = win_box[3];
        b[5] = 0; b[6] = 0;
        b[7] = covered ? COVER : -1;
        b[8] = win_open ? 1 << 16 : 0;
    } else if (h == COVER) { /* over the middle of our picture */
        int cx = (win_box[0] + win_box[2]) / 2, cy = (win_box[1] + win_box[3]) / 2;
        b[1] = cx - 100; b[2] = cy - 100; b[3] = cx + 100; b[4] = cy + 100;
        b[5] = b[6] = 0;
        b[7] = -1;
        b[8] = 1 << 16;
    }
}

/* Wimp_RedrawWindow / UpdateWindow: one rectangle, the window's visible
   area (redraw) or the box asked for (update), in screen coordinates */
static void rect_block(int *b, int update)
{
    int x0 = win_box[0], y1 = win_box[3];
    int c[4] = {win_box[0], win_box[1], win_box[2], win_box[3]};
    if (update) {
        c[0] = x0 + b[1]; c[1] = y1 + b[2]; c[2] = x0 + b[3]; c[3] = y1 + b[4];
    }
    b[1] = win_box[0]; b[2] = win_box[1]; b[3] = win_box[2]; b[4] = win_box[3];
    b[5] = 0; b[6] = 0;
    b[7] = c[0]; b[8] = c[1]; b[9] = c[2]; b[10] = c[3];
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
        case 0: /* Create */
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
        case 3: { /* Map: Y, Cb, Cr addresses and pitches */
            int bank = (int) r->r[1], pitch = (ov_w + 31) & ~31;
            uint8_t *p = ov_buf[bank];
            ov_block[0] = (int) (intptr_t) p; ov_block[1] = pitch;
            ov_block[2] = (int) (intptr_t) (p + pitch * ov_h); ov_block[3] = pitch / 2;
            ov_block[4] = (int) (intptr_t) (p + pitch * ov_h * 5 / 4); ov_block[5] = pitch / 2;
            ov_mapped = bank;
            r->r[0] = (intptr_t) ov_block;
            break;
        }
        default: break;
        }
        return NULL;
    }
    switch (no) {
    case 0x06: /* OS_Byte 176: the vsync counter, 50 Hz */
        r->r[1] = (fake_cs / 2) & 0xFF;
        break;
    case 0x2E: plots++; plot_area = (int *) r->r[1]; break; /* OS_SpriteOp */
    case 0x35: { /* OS_ReadModeVariable: 1920x1080, EX1 EY1, 32bpp TBGR */
        static const int v[13] = {0, 0, 0, 0, 1, 1, 0, 0, 0, 5, 0, 1919, 1079};
        r->r[2] = r->r[1] < 13 ? v[r->r[1]] : 0;
        break;
    }
    case 0x39: { /* OS_SWINumberFromString */
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
    case 0x42: r->r[0] = fake_cs; break;  /* OS_ReadMonotonicTime */
    case 0x46: case 0x65: break;          /* OS_WriteN, OS_ScreenMode */
    case 0x55FC0: ssb_opens++; r->r[0] = 5; ssb_playing = 0; break;  /* OpenStream */
    case 0x55FC1: ssb_closes++; ssb_playing = 0; break;
    case 0x55FC9: ssb_playing = r->r[1] & 1; break;                 /* Pause */
    case 0x55FCE: r->r[0] = 9; break;                               /* ReturnStreamHandle */
    case 0x55FC5: case 0x57287: break;
    case 0x57282: {                                                 /* AddBlock */
        const int16_t *smp = (const int16_t *) r->r[1];
        for (int i = 0; i < (int) r->r[2] / 2; i++)
            if (abs(smp[i]) > ssb_level)
                ssb_level = abs(smp[i]);
        ssb_bytes += (unsigned) r->r[2];
        break;
    }
    case 0x57288: r->r[0] = ssb_bytes; r->r[1] = ssb_bytes; break;  /* BufferStats: all played */
    case 0x400C0: r->r[0] = 380; r->r[1] = TASK; break;             /* Wimp_Initialise */
    case 0x400C1: win_made++; r->r[0] = WIN; break;                 /* CreateWindow */
    case 0x400C2: r->r[0] = BAR_ICON; break;                        /* CreateIcon */
    case 0x400C5: {                                                 /* OpenWindow */
        int *b = (int *) r->r[1];
        win_box[0] = b[1]; win_box[1] = b[2]; win_box[2] = b[3]; win_box[3] = b[4];
        win_behind = b[7];
        win_open = 1;
        opens++;
        redraw_due = 1;
        break;
    }
    case 0x400C6: win_open = 0; break;
    case 0x400C7: case 0x400E1: {                                   /* Wimp_Poll, Wimp_PollIdle */
        int *b = (int *) r->r[1], ev;
        last_mask = (int) r->r[0];
        if (++polls > 30000)
            fail_quit("too many polls: the script is stuck");
        if (redraw_due && win_open) {
            redraw_due = 0;
            b[0] = WIN;
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
            fake_cs++; /* (making the picture takes a moment) */
        } else {
            fake_cs++;
        }
        r->r[0] = ev;
        break;
    }
    case 0x400C8: redraws++; rect_block((int *) r->r[1], 0); r->r[0] = 1; break;  /* RedrawWindow */
    case 0x400C9: updates++; rect_block((int *) r->r[1], 1); r->r[0] = 1; break;  /* UpdateWindow */
    case 0x400CA: r->r[0] = 0; break;                                               /* GetRectangle */
    case 0x400CB: window_state((int *) r->r[1]); break;
    case 0x400D1: force_redraws++; redraw_due = 1; break;
    case 0x400CD: case 0x400D2: case 0x400D4: case 0x400DC: case 0x400FC: case 0x400D7: break;
    case 0x400DD: closedown++; break;
    case 0x400DF: reports++; snprintf(last_report, sizeof(last_report), "%s", (char *) r->r[0] + 4); break;
    case 0x400E7: if (((int *) r->r[1])[4] == 4) acks++; break;     /* SendMessage: DataLoadAck */
    default:
        printf("(fake: SWI &%X not handled)\n", no);
        break;
    }
    return NULL;
}

/* ---- what's on show ----------------------------------------------------------- */

/* the sprite's middle pixel (red) */
static int sprite_middle(void)
{
    if (!plot_area)
        return -1;
    int *spr = plot_area + 4, w = spr[4] + 1;
    int pw = (win_box[2] - win_box[0]) >> 1, ph = (win_box[3] - win_box[1] - 64) >> 1;
    (void) pw;
    uint8_t *px = (uint8_t *) spr + spr[8];
    return px[((size_t) (ph / 2) * w + w / 2) * 4];
}

/* the overlay buffer on show: its middle luma */
static int overlay_middle(void)
{
    if (ov_shown_bank < 0)
        return -1;
    int pitch = (ov_w + 31) & ~31;
    return ov_buf[ov_shown_bank][(size_t) (ov_h / 2) * pitch + ov_w / 2];
}

static int grey(int frame) /* frame n's grey as RGB (BT.601 video range) */
{
    return (int) ((2.0 * frame) * 255 / 219 + 0.5);
}

/* ---- the script ----------------------------------------------------------------- */

static void data_load(int *b, int type, const char *file)
{
    memset(b, 0, 256);
    b[1] = 0x999; b[2] = 55; b[3] = 0; b[4] = 3;
    b[5] = -2; b[6] = BAR_ICON;
    b[10] = type;
    snprintf((char *) &b[11], 200, "%s", file);
    b[0] = (44 + (int) strlen(file) + 1 + 3) & ~3;
}

static int key(int *b, int k)
{
    memset(b, 0, 28);
    b[0] = WIN;
    b[6] = k;
    return 8;
}

static int click_icon(int *b, int icon)
{
    b[0] = win_box[0] + 10; b[1] = win_box[1] + 10; b[2] = 4; b[3] = WIN; b[4] = icon;
    return 6;
}

static void next(int p)
{
    phase = p;
    phase_cs = fake_cs;
}

static int plots_mark, ovd_mark, ssb_mark, closes_mark, pos_mark, pos_min = 1000;
static unsigned play_cs;

static int script(int *b, int mask)
{
    int nulls = !(mask & 1), pos = re_test_position();
    switch (phase) {
    case 0: /* a directory: refused */
        data_load(b, 0x1000, "/tmp");
        next(1);
        return 17;
    case 1:
        CHECK(reports == 1, "a directory dropped wasn't refused");
        data_load(b, 0xFFD, clip);
        next(2);
        return 17;
    case 2: /* the first picture, in the window */
        CHECK(acks == 1, "the clip's DataLoad wasn't acknowledged (%d)", acks);
        if (plots > 0 && pos == 0) {
            CHECK(win_made == 1 && win_open, "the window isn't open");
            CHECK(abs(sprite_middle() - grey(0)) <= 2, "frame 0 drawn as %d, not %d", sprite_middle(), grey(0));
            CHECK(ov_displays == 0, "the overlay was used while stopped");
            next(3);
            return script(b, mask);
        }
        if (!nulls)
            fail_quit("no null events before the first picture");
        if (fake_cs - phase_cs > 200)
            fail_quit("no picture after 2 s");
        return 0;
    case 3: /* stopped: no null events */
        if (nulls) {
            if (fake_cs - phase_cs > 5)
                CHECK(0, "null events still asked for while stopped");
            return 0;
        }
        plots_mark = plots;
        next(4);
        return key(b, 0x18D); /* Right */
    case 4:
        if (plots > plots_mark && pos == 1 && (mask & 1)) {
            CHECK(abs(sprite_middle() - grey(1)) <= 2, "frame 1 drawn as %d, not %d", sprite_middle(), grey(1));
            next(5);
            ssb_mark = ssb_bytes;
            play_cs = fake_cs;
            return click_icon(b, 3); /* Play */
        }
        if (fake_cs - phase_cs > 100)
            fail_quit("Right didn't draw frame 1");
        return nulls ? 0 : key(b, 0x18D);
    case 5: /* playing for 1 s */
        if (fake_cs - phase_cs >= 100) {
            CHECK(re_test_speed() == 1, "not playing (%g)", re_test_speed());
            CHECK(pos >= 24 && pos <= 28, "after 1 s at frame %d, not about 26", pos);
            CHECK(ov_made == 1 && ov_shown_bank >= 0, "the overlay isn't showing");
            CHECK(ov_displays >= 20, "%d pictures through the overlay in 1 s", ov_displays);
            CHECK(abs(overlay_middle() - (16 + 2 * pos)) <= 2, "the overlay shows luma %d at frame %d",
                  overlay_middle(), pos);
            CHECK(idle_polls >= 20, "Wimp_PollIdle used only %d times", idle_polls);
            CHECK(slept > 50, "asleep only %u cs of 100 while playing", slept);
            CHECK(ssb_bytes > ssb_mark && ssb_playing, "no sound given (%u bytes), or not playing", ssb_bytes);
            CHECK(ssb_level > 1000, "the sound given is silent (loudest %g)", ssb_level);
            covered = 1;
            plots_mark = plots;
            next(6);
        }
        return nulls ? 0 : (fail_quit("stopped playing by itself"), 0);
    case 6: /* a window over the picture: drawn in the window */
        if (fake_cs - phase_cs >= 40) {
            CHECK(ov_shown_bank < 0, "the overlay still shows with a window over it");
            CHECK(plots > plots_mark + 5, "not drawn in the window while covered (%d)", plots - plots_mark);
            CHECK(abs(sprite_middle() - grey(pos)) <= 3, "frame %d drawn as %d", pos, sprite_middle());
            covered = 0;
            next(7);
        }
        return nulls ? 0 : (fail_quit("stopped playing by itself"), 0);
    case 7: /* uncovered: the overlay again */
        if (fake_cs - phase_cs >= 30) {
            CHECK(ov_shown_bank >= 0, "the overlay didn't come back");
            {
                /* the sound given keeps time with the pictures, covered or not:
                   one frame's (1920 sample frames at 48 kHz) for each frame
                   played since frame 1, no more (StreamManager gets whole
                   blocks of 2048, so up to one block's less) */
                double given = (ssb_bytes - ssb_mark) / (1920.0 * 4);
                int played = pos - 1;
                CHECK(given <= played + 0.01 && given > played - 1.1, "%.2f frames' sound given for %d frames played",
                      given, played);
                (void) play_cs;
            }
            closes_mark = ssb_closes;
            plots_mark = plots;
            next(8);
            return key(b, ' ');
        }
        return 0;
    case 8: /* stopped: the frame in the window, no overlay, no nulls */
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
        next(9);
        return key(b, ' ');
    case 9: /* playing at 1: L goes to x2 at once */
        next(10);
        return key(b, 'L');
    case 10:
        CHECK(re_test_speed() == 2, "Space then L: speed %g, not 2", re_test_speed());
        pos_mark = pos;
        next(11);
        return key(b, 'k');
    case 11:
        CHECK(re_test_speed() == 0, "K didn't stop it");
        if (nulls)
            return 0;
        pos_mark = pos;
        next(12);
        return key(b, 'j');
    case 12: /* backwards */
        CHECK(re_test_speed() == -1, "J: speed %g, not -1", re_test_speed());
        if (fake_cs - phase_cs >= 20) {
            CHECK(pos < pos_mark - 2, "J: from %d to %d, not backwards", pos_mark, pos);
            next(13);
            return key(b, 'K');
        }
        return 0;
    case 13:
        next(30);
        return key(b, 'L');
    case 30: /* Escape stops too */
        CHECK(re_test_speed() == 1, "L after stopping: speed %g, not 1", re_test_speed());
        next(31);
        return key(b, 27);
    case 31:
        CHECK(re_test_speed() == 0, "Escape didn't stop it");
        if (nulls)
            return 0;
        next(32);
        return key(b, 0x1E); /* Home */
    case 32:
        if (nulls)
            return 0;
        CHECK(pos == 0, "Home: frame %d, not 0", pos);
        covered = 1; /* the window behind another: Select on the icon brings it to the front */
        next(33);
        b[0] = 100; b[1] = 20; b[2] = 4; b[3] = -2; b[4] = BAR_ICON;
        return 6;
    case 33:
        CHECK(win_open && win_behind == -1, "Select on the icon didn't bring the Viewer to the front (%d)",
              win_behind);
        covered = 0;
        if (nulls)
            return 0;
        next(14);
        return key(b, 0x18B); /* End (Copy) */
    case 14:
        if (nulls)
            return 0;
        CHECK(pos == 74, "End: frame %d, not 74", pos);
        next(15);
        return key(b, ' ');
    case 15: /* played from the start to the end, then stopped by itself */
        if (pos >= 0 && pos < pos_min)
            pos_min = pos;
        if (nulls) {
            if (fake_cs - phase_cs > 500)
                fail_quit("didn't stop at the end");
            return 0;
        }
        CHECK(pos_min < 5, "Play at the end didn't start from the beginning (from %d)", pos_min);
        CHECK(fake_cs - phase_cs >= 280, "played to the end in %d cs, not about 3 s", fake_cs - phase_cs);
        CHECK(pos == 74 && re_test_speed() == 0, "stopped at %d (speed %g), not 74", pos, re_test_speed());
        CHECK(ov_shown_bank < 0, "the overlay shows after the end");
        CHECK(abs(sprite_middle() - grey(74)) <= 2, "the last frame drawn as %d", sprite_middle());
        ovd_mark = ov_destroyed;
        next(16);
        b[0] = WIN;
        return 3; /* Close */
    case 16:
        CHECK(!re_test_open(), "closing the window didn't close the video");
        CHECK(ov_destroyed > ovd_mark, "the overlay wasn't destroyed");
        CHECK(!win_open, "the window is still open");
        CHECK(!nulls, "null events asked for with nothing open");
        next(17);
        memset(b, 0, 20);
        b[0] = 20;
        b[4] = 0; /* Message_Quit */
        return 17;
    default:
        fail_quit("polled after Message_Quit");
        return 0;
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: viewer_test numbered-snd.mp4\n");
        return 2;
    }
    clip = argv[1];
    setenv("ReelEdit$Log", "viewer_test.log", 1);
    int r = reeledit_main(0, NULL);
    CHECK(r == 0, "reeledit_main returned %d", r);
    CHECK(phase == 17, "the script stopped at phase %d", phase);
    CHECK(closedown == 1, "Wimp_CloseDown not called");
    printf("viewer_test: %d polls, %d null events, %d Wimp_PollIdle, %u cs asleep; overlay %d pictures; "
           "%u bytes of sound\n", polls, null_events, idle_polls, slept, ov_displays, ssb_bytes);
    printf("viewer_test: %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
}
