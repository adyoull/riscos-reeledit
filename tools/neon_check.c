/*
 * neon_check.c -- the MLT core module's NEON loops (patches/mlt 0011), and
 * the avformat module's preview scaler (0014: fast_scale.c), and
 * ReelEdit's own (src/neon.c: pan and zoom, green screen), against the C
 * loops they stand in for, on random rows
 * Copyright (C) 2026 Andrew Youll
 *
 * Built by build/build-riscos.sh as neon_check,ff8; run on the machine
 * (needs NEON). Prints the largest difference of each loop from the C
 * (8.8 against 16.16 fixed point: 1 or 2 at most) and exits 1 if one is
 * larger or the NEON wasn't used, 0 otherwise.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */

#include "core_neon.h"
#include "fast_scale.h"
#include "scale.h"
#include "neon.h"

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

/* (linked without MLT: core_neon.c asks only this, once; fast_scale.c,
   built for this, each time: so it can be turned off) */
static int neon_wanted = 1;
char *mlt_environment(const char *name)
{
    return strcmp(name, "MLT_NEON") ? NULL : neon_wanted ? (char *) "1" : (char *) "0";
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
        if (t % 3 == 0) /* (runs of see-through and opaque, as a logo's or a keyed picture's: skipped or copied) */
            for (int k = 0; k < 2 * N; k++)
                a[k] = a2[k] = (k / 37) % 3 == 0 ? 0 : (k / 37) % 3 == 1 ? 255 : a[k];

        /* mix: transition_luma.c dissolve_yuv420p, composite without alpha */
        memcpy(c, d, n);
        for (int i = 0; i < n; i++)
            c[i] = (s[i] * w + c[i] * ((1 << 16) - w)) >> 16;
        uint8_t x[N];
        memcpy(x, d, n);
        if (!mlt_core_neon_mix(x, s, n, w))
            bad = 1;
        compare("mix", x, c, n, 2);

        /* mix_w: transition_luma.c's yuv420p wipe, a weight for each pixel (to the bit) */
        {
            static uint16_t wts[N];
            for (int i = 0; i < n; i++)
                wts[i] = (uint16_t) (rand() % 257);
            memcpy(c, d, n);
            for (int i = 0; i < n; i++)
                c[i] = (s[i] * wts[i] + c[i] * (256 - wts[i])) >> 8;
            memcpy(x, d, n);
            if (!mlt_core_neon_mix_w(x, s, wts, n))
                bad = 1;
            compare("mix_w", x, c, n, 0);
        }

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
    /* ReelEdit's: scale.c's rows, which must be the C's to the bit */
    re_neon_on(1);
    static int32_t xi[N];
    static uint16_t xw[N];
    for (int t = 0; t < 200; t++) {
        int n = t % 2 ? N - 1 : 1 + rand() % (N - 1), w = t < 3 ? (int[]){1, 128, 255}[t] : 1 + rand() % 255;
        fill(s, N, 0);
        fill(d, N, 0);
        uint8_t x[N];
        for (int i = 0; i < n; i++)
            c[i] = (uint8_t) ((s[i] * (256 - w) + d[i] * w + 128) >> 8);
        if (!re_neon_mix(s, d, x, n, w))
            bad = 1;
        compare("scale mix", x, c, n, 0);
        for (int i = 0; i < n; i++) {
            xi[i] = rand() % (N - 1);
            xw[i] = (uint16_t) (rand() % 257 > 255 ? 0 : rand() % 256);
            c[i] = (uint8_t) ((s[xi[i]] * (256 - xw[i]) + s[xi[i] + 1] * xw[i] + 128) >> 8);
        }
        if (!re_neon_row(s, xi, xw, x, n))
            bad = 1;
        compare("scale row", x, c, n, 0);
    }
    /* chroma.c's key: two rows of alpha zeroed under the chroma near the key */
    for (int t = 0; t < 200; t++) {
        int n = t % 2 ? N / 2 - 1 : 1 + rand() % (N / 2 - 1), ku = rand() % 256, kv = rand() % 256, var = rand() % 80;
        fill(u, N, 0);
        fill(v, N, 0);
        for (int i = 0; i < n; i += 3) { /* (some near the key) */
            u[i] = (uint8_t) (ku + rand() % 9 - 4 < 0 ? 0 : ku + rand() % 9 - 4 > 255 ? 255 : ku + rand() % 9 - 4);
            v[i] = (uint8_t) kv;
        }
        uint8_t x0[N], x1[N], c0[N], c1[N];
        fill(x0, N, 1);
        fill(x1, N, 1);
        memcpy(c0, x0, N);
        memcpy(c1, x1, N);
        for (int i = 0; i < n; i++)
            if (abs(u[i] - ku) <= var && abs(v[i] - kv) <= var)
                c0[2 * i] = c0[2 * i + 1] = c1[2 * i] = c1[2 * i + 1] = 0;
        if (!re_neon_key(u, v, x0, x1, n, ku, kv, var))
            bad = 1;
        compare("key row 0", x0, c0, N, 0);
        compare("key row 1", x1, c1, N, 0);
    }
    /* looks.c's charcoal: three rows, each pixel from its 3x3, as plus's charcoal (to the bit) */
    for (int t = 0; t < 100; t++) {
        static uint8_t r0[N], r1[N], r2[N], co[N], cx[N];
        fill(r0, N, 0);
        fill(r1, N, 0);
        fill(r2, N, 0);
        if (t % 3 == 0) /* (gentle slopes, so the sums aren't all over the top) */
            for (int i = 0; i < N; i++) {
                r0[i] = (uint8_t) (100 + (i * 3 + t) % 20);
                r1[i] = (uint8_t) (100 + (i * 5) % 17);
                r2[i] = (uint8_t) (100 + (i * 7 + t) % 23);
            }
        int xs = 1 + t % 2, n = N - 2 * xs, invert = t % 5 == 0;
        float scale = t % 4 ? 1.5f : 0.75f;
        for (int i = 0; i < n; i++) {
            const uint8_t *a0 = r0 + xs + i, *a1 = r1 + xs + i, *a2 = r2 + xs + i;
            int m00 = a0[-xs], m01 = a0[0], m02 = a0[xs], m10 = a1[-xs], m12 = a1[xs], m20 = a2[-xs], m21 = a2[0],
                m22 = a2[xs];
            int sum1 = (m20 - m00) + ((m21 - m01) << 1) + (m22 - m20);
            int sum2 = (m02 - m00) + ((m12 - m10) << 1) + (m22 - m20);
            int q = sum1 * sum1 + sum2 * sum2, root = 0;
            while ((root + 1) * (root + 1) <= q)
                root++;
            float sum = scale * root;
            co[i] = (uint8_t) (!invert ? (sum >= 16 && sum <= 235 ? 251 - sum : sum < 16 ? 235 : 16)
                                       : (sum >= 16 && sum <= 235 ? sum : sum < 16 ? 16 : 235));
        }
        /* (the table looks.c makes: each q's value, the rest past max) */
        static uint8_t lut[65536];
        double most = 235 / scale + 2;
        int lut_n = (int) (most * most);
        if (lut_n >= 65536) /* (looks.c works those out a pixel at a time: no table) */
            continue;
        for (int q = 0; q < lut_n; q++) {
            int root = 0;
            while ((root + 1) * (root + 1) <= q)
                root++;
            float sum = scale * root;
            lut[q] = (uint8_t) (!invert ? (sum >= 16 && sum <= 235 ? 251 - sum : sum < 16 ? 235 : 16)
                                        : (sum >= 16 && sum <= 235 ? sum : sum < 16 ? 16 : 235));
        }
        int done = re_neon_charcoal(r0 + xs, r1 + xs, r2 + xs, cx, n, xs, lut, lut_n, invert ? 235 : 16);
        if (done != (n & ~7))
            bad = 1;
        compare("charcoal", cx, co, done, 0);
    }
    /* fast_scale.c (0014): planes of all sizes, scaled up and down, the same with NEON as without */
    for (int t = 0; t < 300; t++) {
        int sw = 2 + rand() % 400, sh = 2 + rand() % 120, dw = 1 + rand() % 400, dh = 1 + rand() % 120;
        if (t < 4) { /* (halving: both ways, across only, down only, and a quarter) */
            static const int k[4][4] = {{320, 180, 160, 90}, {320, 180, 160, 180}, {320, 180, 320, 90}, {640, 360, 150, 84}};
            sw = k[t][0], sh = k[t][1], dw = k[t][2], dh = k[t][3];
        }
        uint8_t *src = malloc((size_t) sw * sh), *o1 = malloc((size_t) dw * dh), *o2 = malloc((size_t) dw * dh);
        fill(src, sw * sh, 0);
        neon_wanted = 1;
        int r1 = mlt_fast_scale_plane(src, sw, sh, o1, dw, dh);
        neon_wanted = 0;
        int r2 = mlt_fast_scale_plane(src, sw, sh, o2, dw, dh);
        neon_wanted = 1;
        if (r1 || r2)
            bad = 1;
        compare("fast_scale", o1, o2, dw * dh, 0);
        free(src);
        free(o1);
        free(o2);
    }
    printf("neon_check: %s (largest difference from the C: %d)\n", bad ? "FAILED" : "passed", worst);
    return bad;
}
