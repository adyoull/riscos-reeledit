/* app.h - what the parts of !ReelEdit's front end share: the project, the
 * engine playing it, and the calls each window makes on the others.
 *
 *   viewer.c    the task: the poll loop, the Viewer window (picture,
 *               transport, toolbar), the menus, keys and messages
 *   timeline.c  the Timeline window: ruler, playhead, V1's clips, V2's overlays
 *   savebox.c   the Save as box (a draggable file icon, as every RISC OS
 *               program's)
 *   effects.c   fades and dissolves from the menus; the Timeline's context menu
 *   marks.c     the in and out marks: play them, take them out
 *   overlays.c  pictures and videos over the video (V2): dropped, their menu; sprites
 *               made PNGs
 *   audiotracks.c  sounds on the audio tracks (below V1): dropped, their menu
 *   autosave.c  a copy of the project kept while it's edited; recovery
 *   clips.c     the Clips window: the project's clips, shown as sources
 *   exportbox.c the Export window: the project made into a video, in a TaskWindow
 *   titles.c    the Title window: text over the video
 *   fonts.c     titles' text drawn by the Font Manager
 *   previews.c  the Timeline's thumbnails and waveforms (src/media.c's)
 *   copypaste.c Cut, Copy and Paste
 *   renders.c   render previews: costly stretches made into files, played from them
 *   proxies.c   proxies: small copies of big or hard-to-decode videos, played in their place
 *   info.c      the Video information window
 *   display.c, sound.c   the picture (overlay or sprite) and the sound
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef RE_APP_H
#define RE_APP_H
#include "../engine.h"
#include "../project.h"
#include "ro.h"

#define APP "ReelEdit"
#define FILETYPE_MLT 0xF80 /* (XML: an MLT project has no filetype of its own yet) */

typedef struct
{
    int task;
    re_project *proj;   /* NULL: the editor isn't open */
    re_engine *e;       /* NULL while the project is empty */
    char path[256];     /* where the project was saved or opened from ("": nowhere yet) */
    int sel;            /* the selected item on V1 (-1: none) */
    int attached;       /* the Timeline moves with the Viewer (View > Attach Timeline) */
    int snap;           /* the playhead snaps to cuts (N) */
    int mark_in, mark_out; /* the I/O marks: frames (-1: none; marks.c) */
    int ov_sel;         /* the selected overlay on V2 (-1: none; not with sel) */
    int au_track, au_sel; /* the selected sound: its audio track, its index there (au_sel -1: none) */
} re_app;
extern re_app A;

/* ---- viewer.c ---- */
void app_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* logging.c: the log, off unless chosen (icon bar menu > Logging, kept in
   Choices) or ReelEdit$Log names a file; app_log does nothing while off */
void lg_start(void);
int lg_on(void);
void lg_set(int on);
void lg_end(void);
void app_report(const char *text);
/* A question: buttons (a comma list); 3 the first, 4 the second... */
int app_ask(const char *text, const char *buttons);
/* The project was edited: the engine sees it, the windows show it. */
void app_edited(void);
/* The same, in the middle of a drag (each step of it): the Viewer's
   picture isn't made again (the Timeline asks for one when the pointer
   rests), so the desktop keeps up with the pointer. */
void app_edited_dragging(void);
/* Saved: the title loses its star. */
void app_saved(const char *path);
int app_position(void);   /* the frame on show (0 when empty) */
void app_seek(int frame);
void app_set_speed(double speed);
/* A file dropped at a frame of the project (-1: at the end): a project file
   is opened, a clip added. */
void app_load(const char *path, int filetype, int frame);
/* The keys every editor window takes (1: taken). */
int app_key(int key);
/* The editor's menu, at the pointer. */
void app_menu(int x, int y);
/* The Timeline's context menu for an item (effects.c's), at the pointer. */
void app_context_menu(int x, int y, int item);
/* An overlay's menu (overlays.c's), at the pointer. */
void app_overlay_menu(int x, int y, int ov);
/* A menu of overlays.c's open (its selections go there). */
void app_overlays_menu_opened(void);
/* A clip added to V1 at a frame (as app_load): its RISC OS name, and the
   name MLT is to open it by. 1, or 0 (reported). */
int app_add(const char *path, const char *mlt_name, int frame);
/* The selection (a clip, a dissolve or an overlay) deleted (ripple: what
   follows V1's closes up). */
void app_delete_selection(int ripple);
/* An item removed (ripple: what follows closes up; else a gap is left). */
void app_delete(int item, int ripple);
/* The Unix name MLT wants for a RISC OS one. */
const char *app_mlt_name(const char *path, char *buf, int size);
void app_timecode(char *buf, int size, int frame);

/* ---- timeline.c ---- */
void tl_open(int x0, int y1, int width);  /* (made the first time) */
void tl_close(void);
int tl_window(void);
void tl_redraw(int *block);
void tl_open_request(int *block);
void tl_click(int *block);
void tl_edited(void);          /* the project changed: extent and redraw */
void tl_playhead(int frame);   /* the playhead moved (playing: kept in view) */
int tl_frame_at(int screen_x); /* for a drop */
void tl_zoom(int in);          /* 1 in, 0 out, -1 the whole project */
void tl_select(int item);      /* -1: none (and no overlay) */
void tl_select_overlay(int ov); /* (and no item) */
void tl_select_audio(int track, int i); /* a sound (and nothing else) */
int tl_on_audio(int screen_y);  /* a drop at this height goes on the audio tracks: the row (or -1) */
int tl_on_overlays(int screen_y); /* a drop at this height goes on V2 */
const char *tl_help(int screen_y); /* interactive help: the ruler, V2 or V1 */
/* The playhead dragged along the ruler: null events while it lasts, each
   moving the playhead to the pointer */
int tl_scrubbing(void);         /* (any drag on the Timeline) */
void tl_refresh(void);          /* drawn again (its thumbnails and waveforms made) */
void tl_title(void);            /* its title again (proxies being made) */
void tl_scrub_tick(void);
void tl_drag_end(int *block);
/* Scroll_Request: the wheel scrolls the Timeline sideways; Ctrl+wheel zooms
   about the pointer */
void tl_scroll(int *block);

/* ---- effects.c: fades and dissolves, from the menus ---- */
int fx_fade_toggle(int item, int end);   /* a clip's fade (end 0 in, 1 out), a second; or taken away. 1 changed */
int fx_mute_toggle(int item);            /* a clip's sound muted, or back. 1 changed */
int fx_sound_fade_toggle(int item, int end); /* a clip's sound faded alone, a second; or taken away. 1 changed */
/* The Sound submenu (clips' and V2 videos'): ticked and shaded so; its
   items S_MUTE, S_FADE_IN, S_FADE_OUT, S_VOLUME (a submenu of FX_LEVELS, dB) */
enum { S_MUTE, S_FADE_IN, S_FADE_OUT, S_VOLUME };
#define FX_N_LEVELS 10
extern const int FX_LEVELS[FX_N_LEVELS];
intptr_t fx_sound_menu(int muted, int fade_in, int fade_out, int in_off, int out_off, int volume);
intptr_t fx_volume_menu(int volume);     /* the Volume submenu alone, its level ticked */
int fx_volume(int item, int db);         /* a clip's sound's level. 1 changed */
int fx_speed(int item, int percent);     /* a clip's speed (%; negative reversed). 1 changed (else reported) */
int fx_dissolve(int before);             /* a second's dissolve at the cut after item `before`: its index, or -1 */
int fx_dissolve_at_cut(void);            /* D: at the cut nearest the playhead */
void fx_menu_open(int x, int y, int item);
void fx_menu_select(int *block);

/* ---- marks.c: in and out marks ---- */
void mk_set(int out);     /* I, O: the mark at the playhead */
void mk_go(int out);      /* Shift-I, Shift-O: the playhead to it */
void mk_play(void);       /* Return: from in to out */
void mk_clear(void);
void mk_take(int ripple); /* ' Extract (ripple), ; Lift (a gap) */
void mk_edited(void);     /* after an edit: kept within the project */

/* ---- overlays.c: pictures over the video ---- */
#define FILETYPE_SPRITE 0xFF9
#define FILETYPE_PNG    0xB60
#define FILETYPE_JPEG   0xC85
int ov_is_picture(const char *path, int filetype);
/* A picture or a video dropped at a frame: an overlay there (V2), or a
   clip on V1; a sprite file's sprite (picked from a menu if it has more)
   made a PNG beside it first */
void ov_load(const char *path, int filetype, int frame, int overlay);
void ov_menu_open(int x, int y, int ov);
void ov_menu_select(int *block);
int ov_fade_toggle(int ov, int end); /* a second's fade, or taken away. 1 changed */
void ov_delete(int ov);

/* ---- audiotracks.c: the audio tracks' sounds ---- */
void au_load(const char *path, int frame); /* a sound dropped at a frame: on the tracks (stacked) */
void au_delete(int track, int i);
void au_menu_open(int x, int y, int track, int i);
void au_menu_select(int *block);
void app_audio_menu(int x, int y, int track, int i); /* (viewer.c: its menu, at the pointer) */
void app_audio_menu_opened(void);  /* a menu of audiotracks.c's open */

/* ---- autosave.c: a copy of the project kept while it's edited ---- */
void as_edited(void);  /* after an edit: a copy, if half a minute since the last (or none) */
void as_forget(void);  /* saved, closed, quit: the copy taken away */
int as_offer(void);    /* a copy there (ReelEdit stopped): recover it? 1: A.proj (and A.path) set */

/* ---- clips.c: the Clips window (the project's clips list) ---- */
int cl_window(void);
void cl_open(void);            /* View > Clips... (Ctrl-B) */
void cl_close(void);
void cl_edited(void);          /* the list changed */
void cl_redraw(int *block);
void cl_click(int *block);
void cl_key(int *block);
void cl_load(const char *path); /* a file dropped on it: on the list */
int cl_dragging(void);
void cl_drag_end(int *block);
const char *cl_help(void);
/* (viewer.c) A clip shown in the Viewer as a source (three-point editing):
   I and O mark it, , and . put its marked frames at the project's playhead
   (insert, overwrite); app_project_view (Escape) shows the project again */
void app_source(const char *mlt_name);
void app_project_view(void);
int app_source_mode(void);
void app_source_mark(int out);
void app_source_put(int overwrite);

/* ---- previews.c: the Timeline's thumbnails and waveforms ---- */
typedef struct
{
    int in, length;          /* its first frame in its file, its length (frames) */
    int fade_in, fade_out;   /* its sound's fades (frames) */
    int volume, muted;       /* dB; muted: no waveform */
    int speed;               /* % (re_item's: its frames' file frames) */
    int source_length;       /* (a reversed clip's frames counted from its end) */
} pv_sound;
/* A thumbnail's width (OS units) at a height (0: none can be made) */
int pv_thumb_width(int h_os);
/* A file's frame's thumbnail, top left at x, y1 (screen), within a box:
   its width, or 0 (none made yet, nor of a frame near it: asked for) */
int pv_thumb(const char *file, int frame, int x, int y1, int h_os, const ro_box *clip);
/* A sound's waveform: x0 the screen x of its first frame, scale OS units
   a frame, from y0 (h high), within a box (asked for if not made yet) */
void pv_wave(const char *file, const pv_sound *s, int x0, double scale, int y0, int h, unsigned colour,
             const ro_box *clip);
int pv_busy(void);  /* things to make, and nothing playing or being dragged: null events wanted */
void pv_tick(void); /* a null event: some made (the Timeline drawn again when there's something to see) */
void pv_close(void); /* the editor closed: all forgotten */
void pv_redraw_begin(void); /* (the Timeline's redraw starting) */

/* ---- copypaste.c: Cut, Copy and Paste ---- */
int cp_copy(int cut);  /* the selection copied (cut: then deleted). 1 done, 0 nothing selected */
void cp_paste(void);   /* at the playhead; selected, the playhead after it */
int cp_have(void);     /* there's something to paste */
void cp_close(void);   /* the editor closed: the copy forgotten */

/* ---- renders.c: render previews ---- */
void rr_start(void);   /* Play > Render previews (Ctrl-R): the stretches without one, one after another */
void rr_stop(void);
int rr_running(void);
int rr_message(int *block); /* 1: it was a render's (TaskWindow's) */
/* The i'th costly stretch (frames): -1 none; 0 to render, 1 rendered, 2
   being rendered (*made frames so far) */
int rr_stretches(int i, int *start, int *length, int *made);
/* What to play: the project with its rendered stretches from their files
   (the caller closes it), or NULL (none: the project) */
mlt_producer rr_playlist(int *rendered);
void rr_close(void);   /* the editor closed: stopped, the files deleted */

/* ---- proxies.c: proxies ---- */
void px_edited(void);  /* the project changed: files wanting proxies found, made one after another */
int px_message(int *block); /* 1: it was a proxy's (TaskWindow's) */
int px_pending(void);  /* the project to be made again (proxies made, or turned on or off) */
void px_applied(void); /* (viewer.c: done) */
int px_on(void);       /* View > Use proxies */
void px_toggle(void);
int px_making(int *made, int *all); /* 1: being made (*made of *all) */
void px_close(void);   /* the editor closed, quitting: stopped, deleted */
void px_wipe(void);    /* deleted (and anything left in their directory) */
void px_forget(void);  /* forgotten, the files left (ReelEdit stopping: tests) */
void px_recover(void); /* a project recovered: the proxies of the session that stopped, known again */
void rr_wipe(void);    /* (renders.c) previews left by a session that stopped, deleted */

/* ---- exportbox.c: the Export window ---- */
int ex_window(void);
int ex_running(void);          /* an export is being made */
void ex_open(void);            /* File > Export... */
void ex_click(int *block);
void ex_key(int *block);
void ex_menu_select(int *block);
int ex_message(int *block);    /* 1: it was the export's (TaskWindow's) */
const char *ex_help(int icon);
void app_export_menu_opened(void); /* a menu of exportbox.c's open */

/* ---- titles.c: the Title window ---- */
int ti_window(void);
void ti_open(int ov);          /* a new title (ov -1: at the playhead), or the title ov changed */
void ti_close(void);
void ti_click(int *block);
void ti_key(int *block);
void ti_menu_select(int *block);
const char *ti_help(int icon);
/* A menu of titles.c's open (its selections go there) */
void app_titles_menu_opened(void);
/* fonts.c (RISC OS only): a line of a title's text drawn by the Font
   Manager (title.h's re_title_font_fn) */
int fm_line(const char *font, int px, const char *line, uint8_t **mask, int *width, int *height);

/* ---- info.c ---- */
void info_open(void);          /* Video information: the selected clip (or the one at the playhead), the project */
void info_edited(void);        /* (made again if it's open) */
void info_close(void);
int info_window(void);
void info_redraw(int *block);

/* ---- savebox.c ---- */
void sb_open(int x, int y);    /* as a menu, at x, y */
int sb_window(void);
void sb_click(int *block);
void sb_key(int *block);
void sb_drag_end(int *block);
/* A file icon (screen box x0..y1) dragged out of a window: when it's
   dropped, where it was is found (Message_DataSave) and saved(path, safe)
   called (safe 0: a scrap file, another program's) */
void sb_drag_file(int win, int x0, int y0, int x1, int y1, const char *leaf, int filetype,
                  int (*saved)(const char *path, int safe));
int sb_message(int *block);    /* 1: it was for the save box */
void sb_forget(void);          /* the editor closed: no save to finish */

#endif
