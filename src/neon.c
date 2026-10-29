/* neon.c - ReelEdit's own NEON loops (neon.h): scale.c's (pan and zoom)
 * and chroma.c's (green screen), each giving the same as its C to the bit.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#include "neon.h"

static int neon_on;

void re_neon_on(int on)
{
    neon_on = on;
}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#include <stdlib.h>

int re_neon_is_on(void)
{
    return neon_on;
}

int re_neon_mix(const uint8_t *a, const uint8_t *b, uint8_t *out, int n, int w)
{
    if (!neon_on)
        return 0;
    uint8x8_t wa = vdup_n_u8((uint8_t) (256 - w > 255 ? 255 : 256 - w)), wb = vdup_n_u8((uint8_t) w);
    int i = 0;
    if (w > 0 && w < 256) {
        for (; i + 8 <= n; i += 8) {
            uint16x8_t s = vmull_u8(vld1_u8(a + i), wa);
            s = vmlal_u8(s, vld1_u8(b + i), wb);
            vst1_u8(out + i, vrshrn_n_u16(s, 8)); /* ((s + 128) >> 8) */
        }
    }
    for (; i < n; i++)
        out[i] = (uint8_t) ((a[i] * (256 - w) + b[i] * w + 128) >> 8);
    return 1;
}

int re_neon_row(const uint8_t *row, const int32_t *xi, const uint16_t *xw, uint8_t *out, int n)
{
    if (!neon_on)
        return 0;
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        /* (the pairs gathered: NEON has no gather, so by hand into lanes) */
        uint8_t p0[8], p1[8];
        for (int k = 0; k < 8; k++) {
            p0[k] = row[xi[i + k]];
            p1[k] = row[xi[i + k] + 1];
        }
        uint16x8_t w1 = vld1q_u16(xw + i);
        uint16x8_t w0 = vsubq_u16(vdupq_n_u16(256), w1);
        uint16x8_t s = vmulq_u16(vmovl_u8(vld1_u8(p0)), w0);
        s = vmlaq_u16(s, vmovl_u8(vld1_u8(p1)), w1);
        vst1_u8(out + i, vrshrn_n_u16(s, 8));
    }
    for (; i < n; i++)
        out[i] = (uint8_t) ((row[xi[i]] * (256 - xw[i]) + row[xi[i] + 1] * xw[i] + 128) >> 8);
    return 1;
}


int re_neon_key(const uint8_t *u, const uint8_t *v, uint8_t *a0, uint8_t *a1, int n, int ku, int kv, int var)
{
    if (!neon_on || var > 255)
        return 0;
    uint8x16_t KU = vdupq_n_u8((uint8_t) ku), KV = vdupq_n_u8((uint8_t) kv), VAR = vdupq_n_u8((uint8_t) var);
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        uint8x16_t in = vandq_u8(vcleq_u8(vabdq_u8(vld1q_u8(u + i), KU), VAR),
                                 vcleq_u8(vabdq_u8(vld1q_u8(v + i), KV), VAR)); /* (0xFF: keyed) */
        uint8x16x2_t two = vzipq_u8(in, in);                                 /* (each for two pixels) */
        uint8x16_t keep0 = vmvnq_u8(two.val[0]), keep1 = vmvnq_u8(two.val[1]);
        vst1q_u8(a0 + 2 * i, vandq_u8(vld1q_u8(a0 + 2 * i), keep0));
        vst1q_u8(a0 + 2 * i + 16, vandq_u8(vld1q_u8(a0 + 2 * i + 16), keep1));
        vst1q_u8(a1 + 2 * i, vandq_u8(vld1q_u8(a1 + 2 * i), keep0));
        vst1q_u8(a1 + 2 * i + 16, vandq_u8(vld1q_u8(a1 + 2 * i + 16), keep1));
    }
    for (; i < n; i++)
        if (abs(u[i] - ku) <= var && abs(v[i] - kv) <= var)
            a0[2 * i] = a0[2 * i + 1] = a1[2 * i] = a1[2 * i + 1] = 0;
    return 1;
}

int re_neon_charcoal(const uint8_t *above, const uint8_t *row, const uint8_t *below, uint8_t *out, int n, int xs,
                     const uint8_t *lut, int lut_n, uint8_t sat)
{
    if (!neon_on)
        return 0;
    int i = 0;
    for (; i + 8 <= n; i += 8) {
#define L16(p) vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p)))
        int16x8_t m00 = L16(above + i - xs), m01 = L16(above + i), m02 = L16(above + i + xs);
        int16x8_t m10 = L16(row + i - xs), m12 = L16(row + i + xs);
        int16x8_t m20 = L16(below + i - xs), m21 = L16(below + i), m22 = L16(below + i + xs);
#undef L16
        /* (plus's sums, as they are) */
        int16x8_t s1 = vaddq_s16(vaddq_s16(vsubq_s16(m20, m00), vshlq_n_s16(vsubq_s16(m21, m01), 1)), vsubq_s16(m22, m20));
        int16x8_t s2 = vaddq_s16(vaddq_s16(vsubq_s16(m02, m00), vshlq_n_s16(vsubq_s16(m12, m10), 1)), vsubq_s16(m22, m20));
        int32_t q[8];
        vst1q_s32(q, vmlal_s16(vmull_s16(vget_low_s16(s1), vget_low_s16(s1)), vget_low_s16(s2), vget_low_s16(s2)));
        vst1q_s32(q + 4,
                  vmlal_s16(vmull_s16(vget_high_s16(s1), vget_high_s16(s1)), vget_high_s16(s2), vget_high_s16(s2)));
        for (int k = 0; k < 8; k++) /* (the table: no gather in NEON) */
            out[i + k] = q[k] < lut_n ? lut[q[k]] : sat;
    }
    return i;
}

int re_neon_invert(uint8_t *p, int n, int base, int add, int lo, int hi)
{
    if (!neon_on || base < 0 || base > 255 || add < 0 || add > 1 || lo < 0 || hi > 255 || lo > hi)
        return 0;
    uint8x16_t B = vdupq_n_u8((uint8_t) base), A = vdupq_n_u8((uint8_t) add), LO = vdupq_n_u8((uint8_t) lo),
               HI = vdupq_n_u8((uint8_t) hi);
    int i = 0;
    for (; i + 16 <= n; i += 16) { /* (base - p saturating at 0, then + add saturating at 255: below lo, above hi
                                       either way) */
        uint8x16_t v = vqaddq_u8(vqsubq_u8(B, vld1q_u8(p + i)), A);
        vst1q_u8(p + i, vminq_u8(vmaxq_u8(v, LO), HI));
    }
    for (; i < n; i++) {
        int v = base - p[i] + add;
        p[i] = (uint8_t) (v < lo ? lo : v > hi ? hi : v);
    }
    return 1;
}

#else

int re_neon_is_on(void)
{
    return 0;
}

int re_neon_mix(const uint8_t *a, const uint8_t *b, uint8_t *out, int n, int w)
{
    (void) a; (void) b; (void) out; (void) n; (void) w;
    return 0;
}

int re_neon_row(const uint8_t *row, const int32_t *xi, const uint16_t *xw, uint8_t *out, int n)
{
    (void) row; (void) xi; (void) xw; (void) out; (void) n;
    return 0;
}

int re_neon_key(const uint8_t *u, const uint8_t *v, uint8_t *a0, uint8_t *a1, int n, int ku, int kv, int var)
{
    (void) u; (void) v; (void) a0; (void) a1; (void) n; (void) ku; (void) kv; (void) var;
    return 0;
}

int re_neon_charcoal(const uint8_t *above, const uint8_t *row, const uint8_t *below, uint8_t *out, int n, int xs,
                     const uint8_t *lut, int lut_n, uint8_t sat)
{
    (void) above; (void) row; (void) below; (void) out; (void) n; (void) xs; (void) lut; (void) lut_n; (void) sat;
    return 0;
}

int re_neon_invert(uint8_t *p, int n, int base, int add, int lo, int hi)
{
    (void) p; (void) n; (void) base; (void) add; (void) lo; (void) hi;
    return 0;
}

#endif
