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
 *   overlays.c  pictures over the video (V2): dropped, their menu; sprites
 *               made PNGs
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

#define APP "Reel Edit"
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
} re_app;
extern re_app A;

/* ---- viewer.c ---- */
void app_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void app_report(const char *text);
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
int tl_on_overlays(int screen_y); /* a drop at this height goes on V2 */
const char *tl_help(int screen_y); /* interactive help: the ruler, V2 or V1 */
/* The playhead dragged along the ruler: null events while it lasts, each
   moving the playhead to the pointer */
int tl_scrubbing(void);
void tl_scrub_tick(void);
void tl_drag_end(int *block);
/* Scroll_Request: the wheel scrolls the Timeline sideways; Ctrl+wheel zooms
   about the pointer */
void tl_scroll(int *block);

/* ---- effects.c: fades and dissolves, from the menus ---- */
int fx_fade_toggle(int item, int end);   /* a clip's fade (end 0 in, 1 out), a second; or taken away. 1 changed */
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
/* A picture dropped at a frame: an overlay there (V2), or a clip on V1;
   a sprite file's sprite (picked from a menu if it has more) made a PNG
   beside it first */
void ov_load(const char *path, int filetype, int frame, int overlay);
void ov_menu_open(int x, int y, int ov);
void ov_menu_select(int *block);
int ov_fade_toggle(int ov, int end); /* a second's fade, or taken away. 1 changed */
void ov_delete(int ov);

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
int sb_message(int *block);    /* 1: it was for the save box */
void sb_forget(void);          /* the editor closed: no save to finish */

#endif
