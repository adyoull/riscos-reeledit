/* measure.h - a clip's length found by reading it through, a little at a
 * time (the desktop going on between). For files that don't say how long
 * they are: raw MPEG-2 video (.m2v), raw H.264 and the like, where MLT
 * takes a clip to be endless.
 *
 * Only the packets are read (no decoding): their times, as FFmpeg's parser
 * gives them (repeated fields counted), say how long the video is.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef RE_MEASURE_H
#define RE_MEASURE_H

typedef struct re_measure re_measure;

/* NULL: not a video FFmpeg can read */
re_measure *re_measure_open(const char *file);
/* Up to this many packets read: 0 more to read, 1 done, -1 failed */
int re_measure_step(re_measure *m, int packets);
double re_measure_progress(re_measure *m); /* 0 to 1, by bytes read */
double re_measure_seconds(re_measure *m);  /* the length (done: all of it) */
void re_measure_close(re_measure *m);

#endif
