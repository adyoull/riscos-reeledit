/* proxy.h - proxies (proxy.c): small copies of a project's big or
 * hard-to-decode videos, edited and played in their place.
 *
 * A proxy is a video file made of a clip's file: 360 lines (its shape
 * kept), a keyframe every 5 frames (seeking and stepping back decode
 * little), H.264 (the VideoCore decodes it), the same frames and sound.
 * A file gets one when it's taller than 540 lines or not H.264 (the
 * VideoCore decodes H.264 only, up to 1080p).
 *
 * The project is told each file's proxy (re_project_set_proxy); with
 * proxies on, re_project_apply_proxies makes the project again with each
 * clip's file replaced by its proxy (its frames, in and out unchanged),
 * and with them off, the files again. Undo and redo keep to it. What's
 * saved (re_project_save, _save_copy: exports, renders, autosaves) always
 * names the files, never the proxies; clips are named after their files
 * too.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef RE_PROXY_H
#define RE_PROXY_H
#include "project.h"

#define RE_PROXY_HEIGHT 360

/* 1: a video that wants a proxy; 0 not (small H.264, a picture, sound
   alone, raw video with no length known, not a video FFmpeg reads) */
int re_proxy_wanted(const char *file);

/* The project's proxies: a file's (NULL: forgotten); used or not */
void re_project_set_proxy(re_project *p, const char *file, const char *proxy);
void re_project_use_proxies(re_project *p, int on);
int re_project_proxies(re_project *p); /* how many it knows */
/* The project made again as proxies on or off ask: 1 it changed, 0 not,
   -1 failed (left as it was). Not an edit (nothing to undo). */
int re_project_apply_proxies(re_project *p);
/* A file's name in the project: its own (not its proxy's) */
const char *re_project_original(re_project *p, const char *file);

/* Every file in a directory deleted, then the directory (Unix name):
   leftover proxies and previews. 0, or -1 (not all could be). */
int re_wipe_dir(const char *dir);

#endif
