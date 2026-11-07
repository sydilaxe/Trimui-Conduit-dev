/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The MidiVerb II Bloom programs (45, 49): see midiverb2.h. The program
 * itself follows thement's midiverb_emulator decompilation
 * (decompiled-midiverb2.h, midiverb2_effect_45 and _49), operation for
 * operation, in its 16-bit integer arithmetic. */
#include "midiverb2.h"
#include <math.h>
#include <string.h>

/* the program's memory map: where each delay line is written, and how far
 * back each is read. The two programs differ only in where their lines
 * sit, and in program 49 reading its input 2000 samples late (85 ms). */
typedef struct {
    int in_rd;                 /* the input, read this far back */
    int lp_in, lp_fb, loop, out_l, out_r;   /* single-sample lines */
    int chain_w[15];           /* the fifteen diffusers */
    int right_w[3], left_w[3];
} Map;

static const int CHAIN_D[15] = { 13, 83, 116, 239, 339, 481, 555, 823, 999, 1100, 1347, 1563, 1841, 2001, 2083 };
static const int RIGHT_D[3] = { 147, 43, 55 };
static const int LEFT_D[3] = { 249, 48, 21 };

static const Map MAP45 = {
    1, 2193, 2184, 11, 2187, 2190,
    { 16381, 16366, 16281, 16163, 15922, 15581, 15098, 14541, 13716, 12715, 11613, 10264, 8699, 6856, 4853 },
    { 2666, 2768, 2723 }, { 2444, 2494, 2517 },
};
static const Map MAP49 = {
    2000, 194, 185, 11, 188, 191,
    { 14382, 14367, 14282, 14164, 13923, 13582, 13099, 12542, 11717, 10716, 9614, 8265, 6700, 4857, 2854 },
    { 667, 769, 724 }, { 445, 495, 518 },
};

/* what the DSP's 16-bit accumulator keeps: the low 16 bits */
static inline int16_t s16(int v) { return (int16_t)(uint16_t)(unsigned)v; }

#define RD(w, d) ((int)m->dram[(m->ptr + (unsigned)(w) - (unsigned)(d)) & (MV2_DRAM - 1)])
#define WR(w) (m->dram[(m->ptr + (unsigned)(w)) & (MV2_DRAM - 1)])

void mv2_tick(MV2 *m, int prog, int16_t in, int16_t *out_l, int16_t *out_r)
{
    const Map *k = prog == 49 ? &MAP49 : &MAP45;
    int16_t acc, t3, t5;
    WR(0) = in;
    acc = s16(RD(k->out_r, 1));
    t3 = s16(-acc);
    acc = s16(RD(k->out_l, 1));
    t5 = s16(-acc);
    /* the input, low-passed */
    acc = s16(RD(k->lp_in, 1) / 2 + RD(0, k->in_rd) / 2);
    WR(k->lp_in) = acc;
    /* the loop's feedback, low-passed (the damping) */
    acc = s16(-RD(k->loop, 1) / 4 + RD(k->lp_fb, 1) / 2);
    WR(k->lp_fb) = acc;
    /* into the chain of fifteen diffusers */
    acc = s16(acc * 3 / 2 + RD(k->lp_in, 1) + RD(k->chain_w[0], CHAIN_D[0]) / 2);
    WR(k->chain_w[0]) = s16(-acc);
    for (int i = 1; i < 15; i++) {
        acc = s16(-acc / 2 + RD(k->chain_w[i - 1], CHAIN_D[i - 1]) + RD(k->chain_w[i], CHAIN_D[i]) / 2);
        WR(k->chain_w[i]) = s16(-acc);
    }
    acc = s16(-acc / 2 + RD(k->chain_w[14], CHAIN_D[14]));
    WR(k->loop) = s16(-acc);
    /* right: three short diffusers from the loop */
    acc = s16(RD(k->loop, 0) + RD(k->right_w[0], RIGHT_D[0]) / 2);
    WR(k->right_w[0]) = s16(-acc);
    for (int i = 1; i < 3; i++) {
        acc = s16(-acc / 2 + RD(k->right_w[i - 1], RIGHT_D[i - 1]) + RD(k->right_w[i], RIGHT_D[i]) / 2);
        WR(k->right_w[i]) = s16(-acc);
    }
    acc = s16(-acc / 2 + RD(k->right_w[2], RIGHT_D[2]));
    WR(k->out_r) = s16(-acc);
    /* left: three more */
    acc = s16(RD(k->loop, 0) + RD(k->left_w[0], LEFT_D[0]) / 2);
    WR(k->left_w[0]) = s16(-acc);
    *out_l = t5;
    for (int i = 1; i < 3; i++) {
        acc = s16(-acc / 2 + RD(k->left_w[i - 1], LEFT_D[i - 1]) + RD(k->left_w[i], LEFT_D[i]) / 2);
        WR(k->left_w[i]) = s16(-acc);
    }
    acc = s16(-acc / 2 + RD(k->left_w[2], LEFT_D[2]));
    WR(k->out_l) = s16(-acc);
    *out_r = t3;
    m->ptr++;
}

#undef RD
#undef WR

/* a 4th-order Butterworth low-pass at 10 kHz (two biquads), at 48 kHz */
void mv2_init(MV2 *m)
{
    memset(m, 0, sizeof *m);
    const double q[2] = { 0.54119610, 1.30656296 };
    double w = 2 * M_PI * 10000.0 / 48000.0, cw = cos(w), sw = sin(w);
    for (int s = 0; s < 2; s++) {
        double al = sw / (2 * q[s]), a0 = 1 + al;
        m->b0[s] = (float)((1 - cw) / 2 / a0); m->b1[s] = (float)((1 - cw) / a0); m->b2[s] = m->b0[s];
        m->a1[s] = (float)(-2 * cw / a0); m->a2[s] = (float)((1 - al) / a0);
    }
    m->t_next = 256;
}

void mv2_reset(MV2 *m)
{
    float b0[2], b1[2], b2[2], a1[2], a2[2];
    memcpy(b0, m->b0, sizeof b0); memcpy(b1, m->b1, sizeof b1); memcpy(b2, m->b2, sizeof b2);
    memcpy(a1, m->a1, sizeof a1); memcpy(a2, m->a2, sizeof a2);
    memset(m, 0, sizeof *m);
    memcpy(m->b0, b0, sizeof b0); memcpy(m->b1, b1, sizeof b1); memcpy(m->b2, b2, sizeof b2);
    memcpy(m->a1, a1, sizeof a1); memcpy(m->a2, a2, sizeof a2);
    m->t_next = 256;
}

static inline float lp4(const MV2 *m, float z[2][2], float x)
{
    for (int s = 0; s < 2; s++) {   /* transposed direct form II */
        float y = m->b0[s] * x + z[s][0];
        z[s][0] = m->b1[s] * x - m->a1[s] * y + z[s][1];
        z[s][1] = m->b2[s] * x - m->a2[s] * y;
        x = y;
    }
    return x;
}

static inline int16_t to13(float x)
{
    /* the converter: 16 bits, clipped, then the top 13 (as the emulator feeds it) */
    float v = x * 32768.f;
    int i = v >= 32767.f ? 32767 : v <= -32768.f ? -32768 : (int)v;
    return (int16_t)(i >> 3);
}

void mv2_process(MV2 *m, int prog, const float *in, float *out, int n)
{
    for (int i = 0; i < n; i++) {
        float y = lp4(m, m->bq_in, 0.5f * (in[2 * i] + in[2 * i + 1]));
        m->t_now += 125;          /* our samples, in 1/125ths: the unit's come every 256 */
        while (m->t_next <= m->t_now) {
            float f = (float)(m->t_next - (m->t_now - 125)) / 125.f;
            int16_t l, r;
            mv2_tick(m, prog, to13(m->y_prev + (y - m->y_prev) * f), &l, &r);
            m->o_prev[0] = m->o_last[0]; m->o_prev[1] = m->o_last[1];
            m->o_last[0] = l; m->o_last[1] = r;
            m->t_next += 256;
        }
        m->y_prev = y;
        /* one unit sample behind: between the last two it gave */
        float f = (float)(m->t_now - (m->t_next - 256)) / 256.f;
        for (int c = 0; c < 2; c++) {
            float o = (m->o_prev[c] + (m->o_last[c] - m->o_prev[c]) * f) * (8.f / 32768.f);   /* 13-bit back to full scale */
            out[2 * i + c] = o;
        }
        out[2 * i] = lp4(m, m->bq_out[0], out[2 * i]);
        out[2 * i + 1] = lp4(m, m->bq_out[1], out[2 * i + 1]);
        if (m->t_now > ((int64_t)1 << 40)) { m->t_now -= (int64_t)1 << 39; m->t_next -= (int64_t)1 << 39; }
    }
}
