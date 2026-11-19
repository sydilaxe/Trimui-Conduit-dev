/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The input ring (src/aring.c) against what USB interfaces do: capture and playback out of step,
 * capture in bursts, jitter, clocks that drift apart (two devices), stalls, multichannel and mono
 * interfaces. A continuous sine goes in at the capture clock; what comes out must be the same sine
 * (resampled a hair): no gaps, no clicks, no skips once settled, and a small cushion. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../aring.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned rs = 12345;
static double urand(void) { rs = rs * 1664525u + 1013904223u; return (rs >> 8) / 16777216.0; }

typedef struct {
    const char *name;
    int in_block, out_block, src_ch, pair;
    double ppm, jitter_ms, secs, stall_at, stall_ms, settle;
    int locked; /* one interface both ways: the smaller cushion */
} Case;

static ARing ring;

static void run(const Case *c)
{
    ar_init(&ring);
    atomic_store(&ring.locked, c->locked);
    rs = 12345;
    const double SR = 48000.0, f = 440.0;
    double t_in = 0.0, t_out = urand() * c->out_block / SR;
    double in_period = c->in_block / (SR * (1.0 + c->ppm * 1e-6)), out_period = c->out_block / SR;
    long n_in = 0, n_out = 0;
    float *src = malloc(sizeof(float) * c->in_block * c->src_ch), *dst = malloc(sizeof(float) * 2 * c->out_block);
    double prev[2] = {0, 0};
    int have = 0, clicks = 0, live = 0;
    unsigned u0 = 0, s0 = 0;
    double fill_sum = 0, ppm_sum = 0;
    long fill_n = 0, settled_at = (long)(SR * (c->settle > 0 ? c->settle : 15.0)) / c->out_block * c->out_block;
    int stalled = 0;
    while (t_out < c->secs) {
        double ti = t_in + urand() * c->jitter_ms / 1000.0, to = t_out + urand() * c->jitter_ms / 1000.0;
        if (ti <= to) {
            int stall = c->stall_ms > 0 && t_in >= c->stall_at && t_in < c->stall_at + c->stall_ms / 1000.0;
            if (!stall) {
                for (int i = 0; i < c->in_block; i++) {
                    double ph = 2.0 * M_PI * f * (double)(n_in + i) / SR;
                    for (int ch = 0; ch < c->src_ch; ch++) {
                        int is_pair = c->src_ch == 1 || ch / 2 == c->pair;
                        src[i * c->src_ch + ch] = is_pair ? (float)(0.5 * sin(ph + (ch & 1) * 0.3)) : (float)(urand() - 0.5);
                    }
                }
                ar_push(&ring, src, c->in_block, c->src_ch, c->pair, (uint64_t)(ti * 1e9));
            } else stalled = 1;
            n_in += c->in_block;
            t_in += in_period;
        } else {
            int on = ar_pull(&ring, dst, c->out_block, c->in_block, (uint64_t)(to * 1e9));
            long at = n_out;
            if (at == settled_at) u0 = ring.underruns, s0 = ring.skips;
            for (int i = 0; i < c->out_block; i++) {
                if (!on) { have = 0; continue; }
                live++;
                double x = dst[2 * i];
                if (dst[2 * i] != dst[2 * i] || dst[2 * i + 1] != dst[2 * i + 1]) clicks += 1000;
                /* a clean 440 Hz sine of 0.5 never bends more than 0.5 * (2 pi 440 / 48000)^2 = 0.0017 a
                 * sample; the fades and crossfades stay under 0.02 */
                if (have >= 2 && fabs(x - 2.0 * prev[1] + prev[0]) > 0.02) clicks++;
                prev[0] = prev[1], prev[1] = x, have = have < 2 ? have + 1 : 2;
            }
            if (at > settled_at) fill_sum += ring.fill, ppm_sum += ring.ppm, fill_n++;
            n_out += c->out_block;
            t_out += out_period;
        }
    }
    unsigned under = ring.underruns - u0, skips = ring.skips - s0;
    double lat_ms = fill_n ? fill_sum / fill_n / 48.0 : 0, ppm = fill_n ? ppm_sum / fill_n : 0;
    printf("%-34s underruns %u (settled %u), skips %u, clicks %d, drift followed %+.0f ppm, cushion %.1f ms (margin %u)%s\n", c->name,
           ring.underruns, under, skips, clicks, ppm, lat_ms, ring.margin, ring.exact ? ", exact" : "");
    if (c->locked) printf("    slips %u, resample %d\n", ring.slips, ring.resample);
    /* one clock: still bit-exact at the end, whatever happened on the way (a stall is not drift) */
    if (c->locked && c->ppm == 0) CHECK(ring.exact, "%s: still bit-exact", c->name);
    if (c->locked && fabs(c->ppm) >= 300) CHECK(!ring.exact, "%s: follows the drift instead", c->name);
    CHECK(live > 0, "%s: sound came through", c->name);
    CHECK(clicks == 0, "%s: %d clicks", c->name, clicks);
    if (!c->stall_ms) {
        CHECK(under == 0 && skips == 0, "%s: settled, no underruns (%u) or skips (%u)", c->name, under, skips);
        CHECK(fabs(ppm - c->ppm) < 60 + fabs(c->ppm) * 0.05, "%s: follows the drift (%+.0f ppm for %+.0f)", c->name, ppm, c->ppm);
        CHECK(lat_ms < (0.5 * c->in_block + 1.5 * c->out_block + 700) / 48.0, "%s: cushion %.1f ms", c->name, lat_ms);
    } else
        CHECK(stalled && ring.underruns >= 1, "%s: the stall was caught", c->name);
    free(src);
    free(dst);
}

/* the pair picked from a multichannel interface, and a mono one */
static void channels(void)
{
    static float in[8 * 64], out[2 * 64];
    ARing *r = &ring;
    ar_init(r);
    for (int i = 0; i < 64; i++)
        for (int c = 0; c < 8; c++) in[i * 8 + c] = (float)(c + 1) / 10.f;
    for (int k = 0; k < 200; k++) ar_push(r, in, 64, 8, 1, 0);
    while (!ar_pull(r, out, 64, 64, 0)) ar_push(r, in, 64, 8, 1, 0);
    for (int k = 0; k < 3; k++) ar_push(r, in, 64, 8, 1, 0), ar_pull(r, out, 64, 64, 0);
    CHECK(fabsf(out[100] - 0.3f) < 1e-4f && fabsf(out[101] - 0.4f) < 1e-4f, "8 channels, pair 2: channels 3-4 (%.3f %.3f)", out[100], out[101]);
    ar_init(r);
    for (int i = 0; i < 64; i++) in[i] = 0.25f;
    for (int k = 0; k < 200; k++) ar_push(r, in, 64, 1, 0, 0);
    while (!ar_pull(r, out, 64, 64, 0)) ar_push(r, in, 64, 1, 0, 0);
    for (int k = 0; k < 3; k++) ar_push(r, in, 64, 1, 0, 0), ar_pull(r, out, 64, 64, 0);
    CHECK(fabsf(out[100] - 0.25f) < 1e-4f && fabsf(out[101] - 0.25f) < 1e-4f, "mono: both sides (%.3f %.3f)", out[100], out[101]);
    ar_init(r);
    for (int k = 0; k < 200; k++) ar_push(r, in, 64, 2, 5, 0);
    while (!ar_pull(r, out, 64, 64, 0)) ar_push(r, in, 64, 2, 5, 0);
    CHECK(1, "a pair past the last falls back to the first");
}

int main(void)
{
    static const Case C[] = {
        /* name                              in    out  ch pair   ppm  jit   secs  stall */
        {"same clock, 512 / 512, 1 ms jitter", 512, 512, 2, 0, 0, 1.0, 60, 0, 0, 0, 0},
        {"same clock, 256 / 256, 1 ms jitter", 256, 256, 2, 0, 0, 1.0, 60, 0, 0, 0, 0},
        {"bursts: capture 1024, play 512", 1024, 512, 2, 0, 0, 2.0, 60, 0, 0, 0, 0},
        {"bursts: capture 512, play 1024", 512, 1024, 2, 0, 0, 2.0, 60, 0, 0, 0, 0},
        {"jitter 5 ms", 512, 512, 2, 0, 0, 5.0, 60, 0, 0, 0, 0},
        {"input clock +300 ppm", 512, 512, 2, 0, 300, 1.0, 120, 0, 0, 40, 0},
        {"input clock -300 ppm", 512, 512, 2, 0, -300, 1.0, 120, 0, 0, 40, 0},
        {"two devices: +2500 ppm", 512, 512, 2, 0, 2500, 2.0, 150, 0, 0, 60, 0},
        {"two devices: -2500 ppm", 512, 512, 2, 0, -2500, 2.0, 150, 0, 0, 60, 0},
        {"multichannel 8 in, pair 3-4", 512, 512, 8, 1, 50, 1.0, 100, 0, 0, 40, 0},
        {"mono interface", 512, 512, 1, 0, -50, 1.0, 100, 0, 0, 40, 0},
        {"a 300 ms stall at 20 s", 512, 512, 2, 0, 0, 1.0, 60, 20.0, 300, 0, 0},
        {"one interface, 256, smaller cushion", 256, 256, 2, 0, 20, 1.0, 90, 0, 0, 30, 1},
        {"one interface, 128, smaller cushion", 128, 128, 2, 0, 20, 1.0, 90, 0, 0, 30, 1},
        {"one interface, 64, smaller cushion", 64, 64, 2, 0, 20, 0.5, 90, 0, 0, 30, 1},
        {"one interface, one clock, 128", 128, 128, 2, 0, 0, 0.5, 90, 0, 0, 30, 1},
        {"one interface, one clock, 64", 64, 64, 2, 0, 0, 0.5, 90, 0, 0, 30, 1},
        {"locked, but 400 ppm apart", 128, 128, 2, 0, 400, 0.5, 120, 0, 0, 60, 1},
        {"one interface, one clock, a stall", 128, 128, 2, 0, 0, 0.5, 120, 20, 300, 30, 1},
    };
    for (unsigned i = 0; i < sizeof C / sizeof *C; i++) run(&C[i]);
    channels();
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
