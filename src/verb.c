/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "verb.h"
#include "dsp.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int verb_init(Verb *v)
{
    memset(v, 0, sizeof *v);
    v->pre_cap = (int)(VERB_PRE_MAX * CD_SR) + 1;
    v->pre = calloc((size_t)v->pre_cap * 2, sizeof(float));
    if (!v->pre || bloom_init(&v->bloom, 1.024)) { verb_free(v); return -1; }
    mv2_init(&v->mv);
    v->type = -1;
    return 0;
}

void verb_free(Verb *v)
{
    free(v->pre); v->pre = NULL;
    bloom_free(&v->bloom);
}

void verb_reset(Verb *v)
{
    bloom_reset(&v->bloom);
    mv2_reset(&v->mv);
    memset(v->pre, 0, sizeof(float) * 2 * (size_t)v->pre_cap);
    memset(v->lo.z, 0, sizeof v->lo.z); memset(v->hi.z, 0, sizeof v->hi.z);
}

/* the adjustable Bloom: in floating point (without the unit's integer
 * rounding) its tail lasts about 14.6 s at the unit's loop gain; a bigger
 * room passes round the loop less often, so it keeps more each time */
float verb_decay(float seconds, float size)
{
    float T = seconds < 1.f ? 1.f : seconds > 30.f ? 30.f : seconds;
    return expf(-4.2f * size / T) / 0.75f;
}

/* RBJ biquads: high-pass (low-cut) and low-pass (high-cut), Q 0.707 */
static void eq_set(VerbEq *q, float f, int high)
{
    if (q->f == f) return;
    q->f = f;
    double w = 2 * M_PI * f / CD_SR, cw = cos(w), al = sin(w) / (2 * 0.70710678), a0 = 1 + al;
    if (high) { q->b0 = (float)((1 + cw) / 2 / a0); q->b1 = (float)(-(1 + cw) / a0); }
    else { q->b0 = (float)((1 - cw) / 2 / a0); q->b1 = (float)((1 - cw) / a0); }
    q->b2 = q->b0;
    q->a1 = (float)(-2 * cw / a0); q->a2 = (float)((1 - al) / a0);
}

static inline float eq_run(VerbEq *q, int c, float x)
{
    float y = q->b0 * x + q->z[c][0];
    q->z[c][0] = q->b1 * x - q->a1 * y + q->z[c][1];
    q->z[c][1] = q->b2 * x - q->a2 * y;
    return y;
}

void verb_process(Verb *v, const float *p, float *buf, int n)
{
    int type = (int)p[P_VERB_TYPE];
    if (type < 0 || type > 3) type = 0;
    if (type != v->type) {
        /* the one coming in starts empty, not with a tail from long ago */
        if (type >= VERB_ADJ_1) bloom_reset(&v->bloom); else mv2_reset(&v->mv);
        v->type = type;
    }
    float lc = p[P_VERB_LOWCUT], hc = p[P_VERB_HICUT];
    int use_lo = lc > 20.5f, use_hi = hc < 15999.f;
    if (use_lo) eq_set(&v->lo, lc, 1);
    if (use_hi) eq_set(&v->hi, hc, 0);
    int pre = (int)(p[P_VERB_PRE] * 0.001f * CD_SR);
    if (pre < 0) pre = 0;
    if (pre > v->pre_cap - 1) pre = v->pre_cap - 1;

    /* input tone, then the pre-delay */
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < 2; c++) {
            float x = buf[2 * i + c];
            if (use_lo) x = eq_run(&v->lo, c, x);
            if (use_hi) x = eq_run(&v->hi, c, x);
            if (pre) {
                int r = v->pre_w - pre; if (r < 0) r += v->pre_cap;
                v->pre[2 * v->pre_w + c] = x;
                x = v->pre[2 * r + c];
            }
            buf[2 * i + c] = x;
        }
        if (pre && ++v->pre_w == v->pre_cap) v->pre_w = 0;
    }

    if (type <= VERB_EXACT_2) mv2_process(&v->mv, type == VERB_EXACT_2 ? 49 : 45, buf, buf, n);
    else {
        float size = p[P_VERB_SIZE];
        float dm = p[P_VERB_DAMP];   /* 0..1 -> loop low-pass pole 0..0.9, 0.5 at the middle (as the unit) */
        float pole = dm <= 0.5f ? dm : 0.5f + (dm - 0.5f) * 0.8f;
        bloom_set(&v->bloom, size, 0.25f + 0.5f * p[P_VERB_DIFF], pole);
        bloom_process(&v->bloom, type == VERB_ADJ_2, verb_decay(p[P_VERB_TIME], v->bloom.size), buf, buf, n);
    }

    /* width (mid / side) and level */
    float wd = p[P_VERB_WIDTH], lev = p[P_VERB_LEVEL];
    for (int i = 0; i < n; i++) {
        float m = 0.5f * (buf[2 * i] + buf[2 * i + 1]), s = 0.5f * (buf[2 * i] - buf[2 * i + 1]) * wd;
        buf[2 * i] = (m + s) * lev;
        buf[2 * i + 1] = (m - s) * lev;
    }
}
