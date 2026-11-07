/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "bloom.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* delay times at the unit's rate (23.4375 kHz) */
static const int CHAIN_D[BLOOM_STAGES] = { 13, 83, 116, 239, 339, 481, 555, 823, 999, 1100, 1347, 1563, 1841, 2001, 2083 };
static const int LEFT_D[BLOOM_SIDE] = { 249, 48, 21 };
static const int RIGHT_D[BLOOM_SIDE] = { 147, 43, 55 };

/* the value written d samples ago (1 <= d <= n) */
static inline float tap(const BloomLine *l, int d)
{
    int i = l->w - d;
    if (i < 0) i += l->n;
    return l->b[i];
}

static inline void put(BloomLine *l, float v)
{
    l->b[l->w] = v;
    if (++l->w == l->n) l->w = 0;
}

/* one diffusing all-pass (gain 1/2): what it stores is the negated sum of
 * its input and half its delayed self; it passes on the delayed value less
 * half the sum */
static inline float allpass(BloomLine *l, float in)
{
    float d = tap(l, l->n);
    float s = in + 0.5f * d;
    put(l, -s);
    return d - 0.5f * s;
}

int bloom_init(Bloom *b, double scale)
{
    memset(b, 0, sizeof *b);
    int len[1 + BLOOM_STAGES + 2 * BLOOM_SIDE], k = 0, total = 0;
    len[k++] = (int)lround(BLOOM_PRE * scale);
    for (int i = 0; i < BLOOM_STAGES; i++) len[k++] = (int)lround(CHAIN_D[i] * scale);
    for (int i = 0; i < BLOOM_SIDE; i++) len[k++] = (int)lround(LEFT_D[i] * scale);
    for (int i = 0; i < BLOOM_SIDE; i++) len[k++] = (int)lround(RIGHT_D[i] * scale);
    for (int i = 0; i < k; i++) total += len[i];
    b->mem = calloc((size_t)total, sizeof(float));
    if (!b->mem) return -1;
    float *m = b->mem; k = 0;
    BloomLine *all[1 + BLOOM_STAGES + 2 * BLOOM_SIDE];
    all[0] = &b->pre;
    for (int i = 0; i < BLOOM_STAGES; i++) all[1 + i] = &b->chain[i];
    for (int i = 0; i < BLOOM_SIDE; i++) all[1 + BLOOM_STAGES + i] = &b->left[i];
    for (int i = 0; i < BLOOM_SIDE; i++) all[1 + BLOOM_STAGES + BLOOM_SIDE + i] = &b->right[i];
    for (int i = 0; i < 1 + BLOOM_STAGES + 2 * BLOOM_SIDE; i++) { all[i]->b = m; all[i]->n = len[i]; m += len[i]; }
    return 0;
}

void bloom_free(Bloom *b) { free(b->mem); b->mem = NULL; }

void bloom_reset(Bloom *b)
{
    int total = b->pre.n;
    for (int i = 0; i < BLOOM_STAGES; i++) total += b->chain[i].n;
    for (int i = 0; i < BLOOM_SIDE; i++) total += b->left[i].n + b->right[i].n;
    memset(b->mem, 0, sizeof(float) * (size_t)total);
    b->in_lp = b->fb_lp = b->loop_prev = b->out_l = b->out_r = 0;
    memset(b->dec_hist, 0, sizeof b->dec_hist);
    memset(b->up_l, 0, sizeof b->up_l); memset(b->up_r, 0, sizeof b->up_r);
    b->phase = 0;
}

void bloom_tick(Bloom *b, int type, float decay, float x, float *l, float *r)
{
    /* both sides come out a sample late, as on the unit */
    *l = b->out_l; *r = b->out_r;

    /* input: a sample late (Bloom 1) or after the pre-delay (Bloom 2), through
     * a gentle low-pass that is read a sample later still */
    float xin = type ? tap(&b->pre, b->pre.n) : tap(&b->pre, 1);
    put(&b->pre, x);
    float in_lp_prev = b->in_lp;
    b->in_lp = 0.5f * b->in_lp + 0.5f * xin;

    /* the loop: last sample's output, a quarter of it, low-passed (the
     * damping; x2 at DC), then x1.5 back in at the head of the chain */
    b->fb_lp = 0.25f * decay * b->loop_prev + 0.5f * b->fb_lp;
    float v = 1.5f * b->fb_lp + in_lp_prev;
    for (int i = 0; i < BLOOM_STAGES; i++) v = allpass(&b->chain[i], v);
    b->loop_prev = v;

    /* the stereo spread: three short all-passes per side from the loop */
    float sl = -v, sr = -v;
    for (int i = 0; i < BLOOM_SIDE; i++) sl = allpass(&b->left[i], sl);
    for (int i = 0; i < BLOOM_SIDE; i++) sr = allpass(&b->right[i], sr);
    b->out_l = sl; b->out_r = sr;
}

void bloom_process(Bloom *b, int type, float decay, const float *in, float *out, int n)
{
    for (int i = 0; i < n; i++) {
        /* down to 24 kHz: (1 3 3 1)/8 over the mono send, every other sample */
        float x = 0.5f * (in[2 * i] + in[2 * i + 1]);
        float y0 = (b->dec_hist[2] + 3.f * b->dec_hist[1] + 3.f * b->dec_hist[0] + x) * 0.125f;
        b->dec_hist[2] = b->dec_hist[1]; b->dec_hist[1] = b->dec_hist[0]; b->dec_hist[0] = x;
        if (b->phase == 0) {
            float l, r;
            bloom_tick(b, type, decay, y0, &l, &r);
            b->up_l[1] = b->up_l[0]; b->up_l[0] = l;
            b->up_r[1] = b->up_r[0]; b->up_r[0] = r;
        }
        /* back up to 48 kHz: halfway between the last two, then the last */
        float ol, orr;
        if (b->phase == 0) { ol = 0.5f * (b->up_l[0] + b->up_l[1]); orr = 0.5f * (b->up_r[0] + b->up_r[1]); }
        else { ol = b->up_l[0]; orr = b->up_r[0]; }
        b->phase ^= 1;
        out[2 * i] = ol; out[2 * i + 1] = orr;
    }
}
