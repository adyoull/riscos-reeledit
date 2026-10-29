/* soundfx.c - sound clean-up (project.h): a clip's, a video overlay's or an
 * audio track's sound panned left or right, its rumble cut, its voice made
 * clearer, its background quietened, and a level to normalise it to.
 *
 * Each is a filter on the item's cut, marked with "shotcut:filter":
 *   reeleditPan      core panner, start 0 (left) .. 1 (right)
 *   reeleditRumble   FFmpeg's highpass at 100 Hz (wind, traffic, handling)
 *   reeleditVoice    FFmpeg's equalizer, +5 dB an octave and a half wide
 *                    about 3 kHz (where speech is clearest)
 *   reeleditGate     FFmpeg's agate: quiet stretches (below about -34 dB)
 *                    turned down by 20 dB, so hiss between words goes
 * Each works sample for sample (the filters that keep audio back to look
 * ahead, such as afftdn's noise reduction, don't work through MLT's
 * avfilter frame by frame, so they aren't used).
 *
 * A dissolve's ends get their clips' (rp_fit_sound_fx), as their volume.
 *
 * Normalising: the level that brings the loud parts of a stretch of a
 * file's peaks (media.c's, a byte a frame) to -3 dB: their 95th percentile,
 * so a single bang doesn't decide it.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "project_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PAN    "reeleditPan"
#define RUMBLE "reeleditRumble"
#define VOICE  "reeleditVoice"
#define GATE   "reeleditGate"

void re_sound_fx_none(re_sound_fx *fx)
{
    memset(fx, 0, sizeof(*fx));
}

int rp_cut_sound_fx(mlt_producer cut, re_sound_fx *fx)
{
    re_sound_fx_none(fx);
    if (!cut)
        return 0;
    mlt_filter f = rp_cut_filter(cut, PAN);
    if (f)
        fx->pan = (int) lround(mlt_properties_get_double(MLT_FILTER_PROPERTIES(f), "start") * 200 - 100);
    fx->rumble = rp_cut_filter(cut, RUMBLE) != NULL;
    fx->voice = rp_cut_filter(cut, VOICE) != NULL;
    fx->gate = rp_cut_filter(cut, GATE) != NULL;
    return fx->pan || fx->rumble || fx->voice || fx->gate;
}

/* A filter on or off (made with its settings: name=value pairs) */
static mlt_filter put(re_project *p, mlt_producer cut, const char *tag, int on, const char *service,
                      const char *const *settings)
{
    mlt_filter f = rp_cut_filter(cut, tag);
    if (!on) {
        if (f)
            mlt_service_detach(MLT_PRODUCER_SERVICE(cut), f);
        return NULL;
    }
    if (f)
        return f;
    f = mlt_factory_filter(re_project_profile(p), service, NULL);
    if (!f)
        return NULL;
    mlt_properties fp = MLT_FILTER_PROPERTIES(f);
    mlt_properties_set(fp, "shotcut:filter", tag);
    for (int i = 0; settings && settings[i]; i += 2)
        mlt_properties_set(fp, settings[i], settings[i + 1]);
    mlt_service_attach(MLT_PRODUCER_SERVICE(cut), f);
    mlt_filter_close(f);
    return f;
}

void rp_cut_set_sound_fx(re_project *p, mlt_producer cut, const re_sound_fx *fx)
{
    static const char *const rumble[] = {"av.f", "100", "av.p", "2", NULL};
    static const char *const voice[] = {"av.f", "3000", "av.t", "o", "av.w", "1.5", "av.g", "5", NULL};
    static const char *const gate[] = {"av.threshold", "0.02", "av.ratio", "4", "av.range", "0.1", "av.attack", "10",
                                       "av.release", "250", NULL};
    mlt_filter f = put(p, cut, PAN, fx->pan != 0, "panner", NULL);
    if (f) {
        mlt_properties_set_double(MLT_FILTER_PROPERTIES(f), "start", (fx->pan + 100) / 200.0);
        mlt_properties_set_int(MLT_FILTER_PROPERTIES(f), "channel", -1); /* (both: the balance) */
    }
    put(p, cut, RUMBLE, fx->rumble, "avfilter.highpass", rumble);
    put(p, cut, VOICE, fx->voice, "avfilter.equalizer", voice);
    put(p, cut, GATE, fx->gate, "avfilter.agate", gate);
}

static void clamp(re_sound_fx *fx)
{
    fx->pan = fx->pan < -100 ? -100 : fx->pan > 100 ? 100 : fx->pan;
    fx->rumble = !!fx->rumble;
    fx->voice = !!fx->voice;
    fx->gate = !!fx->gate;
}

void rp_fit_sound_fx(re_project *p)
{
    int n = p->v1 ? mlt_playlist_count(p->v1) : 0;
    for (int i = 0; i < n; i++) {
        mlt_tractor t = rp_mix_of(p, i);
        if (!t)
            continue;
        mlt_multitrack mt = mlt_tractor_multitrack(t);
        for (int k = 0; k < 2 && k < mlt_multitrack_count(mt); k++) {
            int clip = k ? i + 1 : i - 1;
            mlt_producer end = mlt_multitrack_track(mt, k);
            if (!end || !mlt_producer_is_cut(end)) /* (never the file's producer) */
                continue;
            re_sound_fx want, now;
            rp_cut_sound_fx(rp_is_clip(p, clip) ? mlt_playlist_get_clip(p->v1, clip) : NULL, &want);
            rp_cut_sound_fx(end, &now);
            if (memcmp(&want, &now, sizeof(now)))
                rp_cut_set_sound_fx(p, end, &want);
        }
    }
}

/* The cut of an item: kind 0 a clip (i), 1 an overlay (i), 2 a sound (track k, i) */
static mlt_producer cut_of(re_project *p, int kind, int k, int i)
{
    mlt_playlist pl;
    int e;
    switch (kind) {
    case 0:
        return rp_is_clip(p, i) ? mlt_playlist_get_clip(p->v1, i) : NULL;
    case 1:
        e = rp_overlay_entry(p, i, &pl);
        return e >= 0 ? mlt_playlist_get_clip(pl, e) : NULL;
    default:
        pl = rp_audio_track(p, k);
        e = pl ? rp_track_entry(pl, i) : -1;
        return e >= 0 ? mlt_playlist_get_clip(pl, e) : NULL;
    }
}

static int get(re_project *p, int kind, int k, int i, re_sound_fx *fx)
{
    mlt_producer cut = cut_of(p, kind, k, i);
    rp_cut_sound_fx(cut, fx);
    return cut && (kind == 2 || rp_cut_has_sound(cut)) ? 0 : -1;
}

static int set(re_project *p, int kind, int k, int i, const re_sound_fx *fx)
{
    re_sound_fx want = *fx, now;
    clamp(&want);
    if (get(p, kind, k, i, &now) || !memcmp(&now, &want, sizeof(now)))
        return 0;
    if (!p->in_edit)
        rp_checkpoint(p);
    rp_cut_set_sound_fx(p, cut_of(p, kind, k, i), &want);
    rp_changed(p);
    return 1;
}

int re_project_sound_fx(re_project *p, int item, re_sound_fx *fx) { return get(p, 0, 0, item, fx); }
int re_project_set_sound_fx(re_project *p, int item, const re_sound_fx *fx) { return set(p, 0, 0, item, fx); }
int re_project_overlay_sound_fx(re_project *p, int i, re_sound_fx *fx) { return get(p, 1, 0, i, fx); }
int re_project_overlay_set_sound_fx(re_project *p, int i, const re_sound_fx *fx) { return set(p, 1, 0, i, fx); }
int re_project_audio_sound_fx(re_project *p, int track, int i, re_sound_fx *fx) { return get(p, 2, track, i, fx); }
int re_project_audio_set_sound_fx(re_project *p, int track, int i, const re_sound_fx *fx)
{
    return set(p, 2, track, i, fx);
}

static int by_value(const void *a, const void *b)
{
    return *(const unsigned char *) a - *(const unsigned char *) b;
}

int re_normal_level(const unsigned char *peaks, int n, int f0, int f1, int *db)
{
    if (!peaks || n <= 0)
        return -1;
    f0 = f0 < 0 ? 0 : f0;
    f1 = f1 > n ? n : f1;
    int m = 0;
    unsigned char *v = malloc(f1 > f0 ? (size_t) (f1 - f0) : 1);
    if (!v)
        return -1;
    for (int f = f0; f < f1; f++)
        if (peaks[f] > 2) /* (silence left out: it would make the rest louder) */
            v[m++] = peaks[f];
    if (m < 5) {
        free(v);
        return -1;
    }
    qsort(v, (size_t) m, 1, by_value);
    int loud = v[(m - 1) * 95 / 100];
    free(v);
    double level = -3.0 - 20.0 * log10(loud / 255.0);
    int d = (int) lround(level);
    *db = d < -36 ? -36 : d > 12 ? 12 : d;
    return 0;
}
