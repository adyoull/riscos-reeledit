/*
 * neon_check.c -- the MLT core module's NEON loops (patches/mlt 0011)
 * against the C loops they stand in for, on random rows
 * Copyright (C) 2026 Andrew Youll
 *
 * Built by build/build-riscos.sh as neon_check,ff8; run on the machine
 * (needs NEON). Prints the largest difference of each loop from the C
 * (8.8 against 16.16 fixed point: 1 or 2 at most) and exits 1 if one is
 * larger or the NEON wasn't used, 0 otherwise.
 */

#include "core_neon.h"

#include <framework/mlt_factory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 1000 /* (not a multiple of 16: the tails too) */

static uint8_t s[N], d[N], a[2 * N], a2[2 * N], c[N], u[N], v[N], cu[N], cv[N], su[N], sv[N];
static int worst, bad;

static void fill(uint8_t *p, int n, int kind)
{
    for (int i = 0; i < n; i++) {
        int r = rand() & 255;
        /* (alphas: mostly 0 and 255, as a logo's) */
        p[i] = kind == 1 ? (r < 100 ? 0 : r > 160 ? 255 : r) : r;
    }
}

static void compare(const char *what, const uint8_t *x, const uint8_t *y, int n, int allowed)
{
    int most = 0;
    for (int i = 0; i < n; i++)
        if (abs(x[i] - y[i]) > most)
            most = abs(x[i] - y[i]);
    if (most > worst)
        worst = most;
    if (most > allowed) {
        bad = 1;
        printf("%s: off by %d\n", what, most);
    }
}

/* (linked without MLT: core_neon.c asks only this, once) */
char *mlt_environment(const char *name)
{
    return strcmp(name, "MLT_NEON") ? NULL : (char *) "1";
}

int main(void)
{
    if (!mlt_core_neon()) {
        printf("neon_check: no NEON in this build\n");
        return 1;
    }
    srand(1);
    int weights[] = {0, 1, 300, 16384, 32768, 50000, 65535, 65536};
    for (int t = 0; t < 400; t++) {
        int32_t w = t < 8 ? weights[t] : rand() % 65537;
        int n = t % 2 ? N : 1 + rand() % N;
        fill(s, N, 0);
        fill(d, N, 0);
        fill(a, 2 * N, 1);
        fill(a2, 2 * N, 1);

        /* mix: transition_luma.c dissolve_yuv420p, composite without alpha */
        memcpy(c, d, n);
        for (int i = 0; i < n; i++)
            c[i] = (s[i] * w + c[i] * ((1 << 16) - w)) >> 16;
        uint8_t x[N];
        memcpy(x, d, n);
        if (!mlt_core_neon_mix(x, s, n, w))
            bad = 1;
        compare("mix", x, c, n, 2);

        /* blend: composite_yuv420p's luma with alpha */
        memcpy(c, d, n);
        for (int i = 0; i < n; i++) {
            if (!a[i])
                continue;
            int mix = (w * (a[i] + 1)) >> 8;
            c[i] = mix >= (1 << 16) ? s[i] : (s[i] * mix + c[i] * ((1 << 16) - mix)) >> 16;
        }
        memcpy(x, d, n);
        if (!mlt_core_neon_blend(x, s, a, n, w))
            bad = 1;
        compare("blend", x, c, n, 2);
        for (int i = 0; i < n; i++)
            if (!a[i] && x[i] != d[i]) {
                bad = 1;
                printf("blend: a see-through pixel changed\n");
                break;
            }

        /* blend_chroma: composite_yuv420p's chroma with alpha */
        fill(u, N, 0);
        fill(v, N, 0);
        fill(su, N, 0);
        fill(sv, N, 0);
        int cn = n / 2;
        memcpy(cu, u, cn);
        memcpy(cv, v, cn);
        for (int i = 0; i < cn; i++) {
            int al = (a[2 * i] + a[2 * i + 1] + a2[2 * i] + a2[2 * i + 1] + 2) >> 2;
            if (!al)
                continue;
            int mix = (w * (al + 1)) >> 8;
            if (mix >= (1 << 16)) {
                cu[i] = su[i];
                cv[i] = sv[i];
            } else {
                cu[i] = (su[i] * mix + cu[i] * ((1 << 16) - mix)) >> 16;
                cv[i] = (sv[i] * mix + cv[i] * ((1 << 16) - mix)) >> 16;
            }
        }
        if (!mlt_core_neon_blend_chroma(u, v, su, sv, a, a2, cn, w))
            bad = 1;
        compare("blend_chroma u", u, cu, cn, 2);
        compare("blend_chroma v", v, cv, cn, 2);

        /* scale: filter_brightness.c's yuv420p, a fade's level below 1 */
        int32_t m = w >= 65536 - 128 ? 30000 : w;
        int32_t add = 128 * ((1 << 16) - m);
        memcpy(c, d, n);
        for (int i = 0; i < n; i++)
            c[i] = CLAMP((c[i] * m) >> 16, 16, 235);
        memcpy(x, d, n);
        if (!mlt_core_neon_scale(x, n, m, 0, 16, 235))
            bad = 1;
        compare("scale luma", x, c, n, 2);
        memcpy(c, d, n);
        for (int i = 0; i < n; i++)
            c[i] = CLAMP((c[i] * m + add) >> 16, 16, 240);
        memcpy(x, d, n);
        if (!mlt_core_neon_scale(x, n, m, add, 16, 240))
            bad = 1;
        compare("scale chroma", x, c, n, 2);
    }
    printf("neon_check: %s (largest difference from the C: %d)\n", bad ? "FAILED" : "passed", worst);
    return bad;
}
