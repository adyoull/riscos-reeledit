/* export.h - a project made into a video file (export.c): MP4, H.264 and
 * AAC, at the project's size or smaller, by MLT's avformat consumer.
 *
 * Reel Edit runs it in a program of its own (its !RunImage with -export,
 * in a TaskWindow: pre-emptively multitasked, so editing carries on), from
 * the project saved to a file; the progress comes back as lines of text
 * (re_export_main).
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef RE_EXPORT_H
#define RE_EXPORT_H

enum { RE_EXPORT_SMALL, RE_EXPORT_NORMAL, RE_EXPORT_HIGH };

typedef struct
{
    int height;          /* lines (0: the project's); the width keeps its shape (even) */
    int quality;         /* RE_EXPORT_: the bit rate (and libx264's rate factor) */
    int hardware;        /* 1: the VideoCore's H.264 encoder (h264_encvchiq), if FFmpeg has it */
    const char *vcodec;  /* (NULL: the VideoCore's when hardware, else libx264, else mpeg4) */
    int range, in, out;  /* range: only the project's frames in to out (render previews: render.h) */
} re_export_settings;

/* The project file exported to out. progress (if given) is told each
   picture made of how many; stop (if given) is asked each picture, and
   non-zero stops it (the file is deleted). 0 done, 1 stopped, -1 failed
   (err says why). */
int re_export(const char *project, const char *out, const re_export_settings *s,
              void (*progress)(void *ctx, int done, int total), int (*stop)(void *ctx), void *ctx, char *err,
              int errsize);

/* The video bit rate (bits a second) for a size, frame rate and quality */
int re_export_bitrate(int width, int height, double fps, int quality);

/* !RunImage -export project out height quality hardware [in out]: re_export, the
   progress written as "progress DONE TOTAL" lines (at most four a second),
   then "done" or "failed: why" (stopped: killed by its TaskWindow; the
   caller deletes what was written). The exit status: 0, or 2. */
int re_export_main(int argc, char **argv);

#endif
