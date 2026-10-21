/* app.h - what the parts of !ReelEdit's front end share: the project, the
 * engine playing it, and the calls each window makes on the others.
 *
 *   viewer.c    the task: the poll loop, the Viewer window (picture,
 *               transport, toolbar), the menus, keys and messages
 *   timeline.c  the Timeline window: ruler, playhead, V1's clips
 *   savebox.c   the Save as box (a draggable file icon, as every RISC OS
 *               program's)
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
} re_app;
extern re_app A;

/* ---- viewer.c ---- */
void app_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void app_report(const char *text);
/* The project was edited: the engine sees it, the windows show it. */
void app_edited(void);
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
void tl_select(int item);      /* -1: none */
/* The playhead dragged along the ruler: null events while it lasts, each
   moving the playhead to the pointer */
int tl_scrubbing(void);
void tl_scrub_tick(void);
void tl_drag_end(int *block);
/* Scroll_Request: the wheel scrolls the Timeline sideways; Ctrl+wheel zooms
   about the pointer */
void tl_scroll(int *block);

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
