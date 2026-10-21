/* sound.h - the Viewer's sound, through SharedSoundBuffer and StreamManager
 * (as reelcore, riscos-ffmpeg's player core, plays it): given from the
 * program's own time slice, played by StreamManager from interrupts, so
 * nothing waits for a thread.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef RE_SOUND_H
#define RE_SOUND_H
#include <stdint.h>

typedef struct re_sound re_sound;

/* 16-bit stereo at RATE. NULL when SharedSoundBuffer or StreamManager
   isn't there (WHY says so). */
re_sound *re_sound_open(int rate, char *why, int why_size);
void re_sound_close(re_sound *s);

/* Sound to play (interleaved stereo sample frames). Playing starts once two
   blocks are queued, unless paused. */
void re_sound_add(re_sound *s, const int16_t *samples, int frames);
/* Stops (keeping what's queued) or carries on. */
void re_sound_pause(re_sound *s, int paused);
/* Throws away what's queued (a seek, a stop): StreamManager can't empty its
   buffer, so the stream is opened again. */
void re_sound_clear(re_sound *s);
/* Seconds queued and not yet played. */
double re_sound_queued(re_sound *s);
/* Blocks refused by StreamManager (full), for the log. */
unsigned re_sound_refused(re_sound *s);

#endif
