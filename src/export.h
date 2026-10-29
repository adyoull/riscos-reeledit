/* export.h - a project made into a video file (export.c): MP4, H.264 and
 * AAC, at the project's size or smaller, by MLT's avformat consumer.
 *
 * ReelEdit runs it in a program of its own (its !RunImage with -export,
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
    int gop;             /* a keyframe every this many frames (0: two seconds') */
    int fast;            /* 1: scaled as previews are (patches/mlt 0014's scaler, not swscale): proxies */
} re_export_settings;

/* What the last export or proxy used and how fast it went ("decoder
   h264_vchiq, encoder h264_encvchiq, 250 frames in 9.5 s"), for the log */
const char *re_export_report(void);

/* The project file exported to out. progress (if given) is told each
   picture made of how many; stop (if given) is asked each picture, and
   non-zero stops it (the file is deleted). 0 done, 1 stopped, -1 failed
   (err says why). */
int re_export(const char *project, const char *out, const re_export_settings *s,
              void (*progress)(void *ctx, int done, int total), int (*stop)(void *ctx), void *ctx, char *err,
              int errsize);

/* A clip's file made into its proxy (proxy.h): RE_PROXY_HEIGHT lines, a
   keyframe every 5 frames, its own frame rate and sound; as re_export */
int re_export_proxy(const char *file, const char *out, int hardware, void (*progress)(void *ctx, int done, int total),
                    int (*stop)(void *ctx), void *ctx, char *err, int errsize);

/* The video bit rate (bits a second) for a size, frame rate and quality */
int re_export_bitrate(int width, int height, double fps, int quality);

/* !RunImage -export project out height quality hardware [in out]: re_export, the
   progress written as "progress DONE TOTAL" lines (at most four a second),
   then "done" or "failed: why" (stopped: killed by its TaskWindow; the
   caller deletes what was written). The exit status: 0, or 2.
   !RunImage -proxy file out hardware: re_export_proxy, the same way. */
int re_export_main(int argc, char **argv);

/* The arguments after -export or -proxy in a file instead (one a line: a
   name may hold spaces, which a command line would split): !RunImage
   -export @file. re_export_args reads them (the file then deleted): how
   many (*args malloc'd, each too; re_export_args_free), or -1. */
int re_export_args(const char *file, char ***args);
void re_export_args_free(char **args, int n);

#endif
