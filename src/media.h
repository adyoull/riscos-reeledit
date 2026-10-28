/* media.h - what the Timeline shows of its clips' files: thumbnails (a
 * frame's picture, small) and waveforms (the sound's peaks, a frame at a
 * time), made a little at a time, between the desktop's other work.
 *
 * Asking for one that isn't made yet gives NULL and puts it on the list to
 * make; re_media_step makes them, most recently asked for first (what's in
 * view), a few packets at a time. Thumbnails are kept, the least recently
 * asked for going when there are too many; peaks (a byte a frame) are kept
 * for every file. A file that can't be read isn't tried again.
 *
 * Read with FFmpeg's software decoders (the VideoCore's are kept for
 * playing), not through MLT: a thumbnail is the nearest frame a seek and a
 * few frames' decoding reach (frames no other frame needs are skipped).
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef RE_MEDIA_H
#define RE_MEDIA_H

typedef struct re_media re_media;

/* Frames are fps a second (the project's); thumbnails w by h pixels, the
   picture fitted inside with black bars. NULL: no memory. */
re_media *re_media_new(double fps, int w, int h);
void re_media_free(re_media *m);
void re_media_size(re_media *m, int *w, int *h);

/* A frame of a file: w * h pixels, 4 bytes each (R, G, B, 255), top row
   first; NULL not made yet (to be made) or can't be. *serial (if not NULL):
   a number no other thumbnail has had (a cache's key). */
const unsigned char *re_media_thumb(re_media *m, const char *file, int frame, unsigned *serial);
/* The made thumbnail of that file nearest the frame (a stand-in while the
   right one's made), or NULL. Doesn't ask for anything. */
const unsigned char *re_media_thumb_near(re_media *m, const char *file, int frame, unsigned *serial);

/* A file's sound's peaks: a byte a frame (0 silent, 255 full scale), *n of
   them; while they're being made, those made so far (*done 0). NULL: none
   yet, or no sound. */
const unsigned char *re_media_peaks(re_media *m, const char *file, int *n, int *done);

/* Some of the work: 1 something to show (a thumbnail made; peaks done, or
   a stretch more of them), 0 more to do, -1 nothing to do */
int re_media_step(re_media *m);
int re_media_pending(re_media *m); /* things asked for, not made */

#endif
