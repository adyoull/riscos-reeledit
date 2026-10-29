/* sound.c - SharedSoundBuffer/StreamManager (sound.h), from reelcore.c.
 *
 * RISC OS's 16-bit stereo has the right sample in each word's low halfword
 * and the left in its high one (as SoundDMA and a linear handler have it),
 * the other way round from the engine's interleaved left, right; so each
 * pair is swapped as it's queued.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "sound.h"
#include "ro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SharedSoundBuffer_OpenStream         0x55FC0
#define SharedSoundBuffer_CloseStream        0x55FC1
#define SharedSoundBuffer_SampleRate         0x55FC5
#define SharedSoundBuffer_Pause              0x55FC9
#define SharedSoundBuffer_ReturnStreamHandle 0x55FCE
#define StreamManager_AddBlock               0x57282
#define StreamManager_SetBuffer              0x57287
#define StreamManager_BufferStats            0x57288

#define BLOCK       2048 /* sample frames per StreamManager block */
#define BLOCK_BYTES (BLOCK * 4)

struct re_sound
{
    int rate;
    int handle, stream;   /* SharedSoundBuffer's and StreamManager's */
    int started, paused;
    uint8_t *pend;        /* not yet given to StreamManager */
    unsigned pend_len, pend_size;
    unsigned added_bytes, added_blocks;
    unsigned refused;
};

static _kernel_oserror *call(int n, int r0, intptr_t r1, int r2, _kernel_swi_regs *out)
{
    _kernel_swi_regs r;
    memset(&r, 0, sizeof(r));
    r.r[0] = r0; r.r[1] = r1; r.r[2] = r2;
    return _kernel_swi(n, &r, out ? out : &r);
}

static int have(const char *swi)
{
    _kernel_swi_regs r;
    memset(&r, 0, sizeof(r));
    r.r[1] = (intptr_t) swi;
    return _kernel_swi(OS_SWINumberFromString, &r, &r) == NULL;
}

/* Opens the stream, paused: NULL or an error message */
static const char *start(re_sound *s)
{
    static char msg[96];
    _kernel_swi_regs r;
    _kernel_oserror *e;
    if ((e = call(SharedSoundBuffer_OpenStream, 2, (intptr_t) "ReelEdit", BLOCK_BYTES, &r))) {
        snprintf(msg, sizeof(msg), "SharedSoundBuffer_OpenStream: %.60s", e->errmess);
        return msg;
    }
    s->handle = r.r[0];
    if ((e = call(SharedSoundBuffer_ReturnStreamHandle, s->handle, 0, 0, &r))) {
        call(SharedSoundBuffer_CloseStream, s->handle, 0, 0, NULL);
        s->handle = 0;
        snprintf(msg, sizeof(msg), "SharedSoundBuffer_ReturnStreamHandle: %.50s", e->errmess);
        return msg;
    }
    s->stream = r.r[0];
    /* room for far more than is kept queued, so AddBlock doesn't refuse */
    call(StreamManager_SetBuffer, s->stream, s->rate * 4 * 2 + BLOCK_BYTES * 4, 0, NULL);
    call(SharedSoundBuffer_SampleRate, s->handle, s->rate * 1024, 0, NULL);
    call(SharedSoundBuffer_Pause, s->handle, 0, 0, NULL); /* R1 bit 0 clear: paused */
    s->started = 0;
    s->pend_len = 0;
    s->added_bytes = s->added_blocks = 0;
    return NULL;
}

static void stop(re_sound *s)
{
    if (s->handle)
        call(SharedSoundBuffer_CloseStream, s->handle, 0, 0, NULL);
    s->handle = s->stream = 0;
}

re_sound *re_sound_open(int rate, char *why, int why_size)
{
    const char *err;
    if (!have("SharedSoundBuffer_OpenStream") || !have("StreamManager_AddBlock")) {
        snprintf(why, why_size, "SharedSoundBuffer/StreamManager not loaded");
        return NULL;
    }
    re_sound *s = calloc(1, sizeof(*s));
    if (!s) {
        snprintf(why, why_size, "no memory");
        return NULL;
    }
    s->rate = rate;
    if ((err = start(s))) {
        snprintf(why, why_size, "%s", err);
        free(s);
        return NULL;
    }
    return s;
}

void re_sound_close(re_sound *s)
{
    if (!s)
        return;
    stop(s);
    free(s->pend);
    free(s);
}

/* Gives StreamManager whole blocks; starts playing once two are there. */
static void push(re_sound *s)
{
    unsigned off = 0;
    if (!s->handle)
        return;
    while (s->pend_len - off >= BLOCK_BYTES) {
        if (call(StreamManager_AddBlock, s->stream, (intptr_t) (s->pend + off), BLOCK_BYTES, NULL)) {
            s->refused++; /* full: again next time */
            break;
        }
        off += BLOCK_BYTES;
        s->added_bytes += BLOCK_BYTES;
        s->added_blocks++;
    }
    if (off) {
        memmove(s->pend, s->pend + off, s->pend_len - off);
        s->pend_len -= off;
    }
    if (!s->started && !s->paused && s->added_blocks >= 2) {
        call(SharedSoundBuffer_Pause, s->handle, 1, 0, NULL); /* play */
        s->started = 1;
    }
}

void re_sound_add(re_sound *s, const int16_t *samples, int frames)
{
    unsigned bytes = (unsigned) frames * 4;
    if (!s || frames <= 0 || !s->handle) /* (no stream: reopening it failed) */
        return;
    if (s->pend_len + bytes > (unsigned) s->rate * 4) /* StreamManager refusing for a second: start again */
        s->pend_len = 0;
    if (s->pend_len + bytes > s->pend_size) {
        unsigned size = s->pend_len + bytes + BLOCK_BYTES * 2;
        uint8_t *p = realloc(s->pend, size);
        if (!p)
            return;
        s->pend = p;
        s->pend_size = size;
    }
    uint8_t *to = s->pend + s->pend_len; /* (little-endian words: right, then left) */
    for (int f = 0; f < frames; f++, to += 4) {
        uint16_t l = (uint16_t) samples[2 * f], r = (uint16_t) samples[2 * f + 1];
        to[0] = (uint8_t) r; to[1] = (uint8_t) (r >> 8);
        to[2] = (uint8_t) l; to[3] = (uint8_t) (l >> 8);
    }
    s->pend_len += bytes;
    push(s);
}

void re_sound_pause(re_sound *s, int paused)
{
    if (!s)
        return;
    s->paused = paused;
    if (s->started)
        call(SharedSoundBuffer_Pause, s->handle, paused ? 0 : 1, 0, NULL);
    else if (!paused)
        push(s);
}

void re_sound_clear(re_sound *s)
{
    if (!s)
        return;
    stop(s);
    start(s); /* (if it fails, the handle is 0 and nothing more is played) */
}

double re_sound_queued(re_sound *s)
{
    _kernel_swi_regs r;
    unsigned added, played, q;
    if (!s || !s->stream || call(StreamManager_BufferStats, s->stream, 0, 0, &r))
        return 0;
    added = (unsigned) r.r[0];
    played = (unsigned) r.r[1];
    if (added == s->added_blocks && added != s->added_bytes) { /* counted in blocks */
        added *= BLOCK_BYTES;
        played *= BLOCK_BYTES;
    }
    q = (added > played ? added - played : 0) + s->pend_len;
    return q / (s->rate * 4.0);
}

unsigned re_sound_refused(re_sound *s)
{
    return s ? s->refused : 0;
}
