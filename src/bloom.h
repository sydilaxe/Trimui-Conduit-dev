/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Bloom: a long, swelling stereo reverb after the Alesis MidiVerb II "Bloom"
 * programs (45: Bloom 1, 8 s; 49: Bloom 2, 7 s, the same with a pre-delay).
 *
 * The signal flow -- a damped feedback loop through a chain of fifteen
 * diffusing all-pass filters, which is what makes the sound swell instead
 * of starting dense, then three short all-passes per side for the stereo
 * spread -- and its delay times come from the analysis of the MidiVerb II
 * in thement's midiverb_emulator (github.com/thement/midiverb_emulator).
 * This is an independent floating-point implementation; it shares no code
 * with that project.
 *
 * Mono in, stereo out, like the unit. It runs at half Conduit's rate
 * (24 kHz, near the MidiVerb II's 23.4 kHz, with the delays scaled to keep
 * its timing), which also keeps its darker top end. */
#pragma once

#define BLOOM_STAGES 15
#define BLOOM_SIDE 3
#define BLOOM_PRE 2000          /* Bloom 2's pre-delay, at the unit's rate (85 ms) */

typedef struct { float *b; int n, w; } BloomLine;

typedef struct {
    float *mem;
    BloomLine pre, chain[BLOOM_STAGES], left[BLOOM_SIDE], right[BLOOM_SIDE];
    float in_lp, fb_lp, loop_prev, out_l, out_r;   /* one-sample states */
    /* 48 kHz <-> 24 kHz */
    float dec_hist[3], up_l[2], up_r[2];
    int phase;
} Bloom;

/* scale: the core's rate / 23437.5 Hz (1.024 for 24 kHz; 1.0 runs it at the unit's own rate) */
int  bloom_init(Bloom *b, double scale);
void bloom_free(Bloom *b);
void bloom_reset(Bloom *b);

/* one sample at the core's rate. type 0: Bloom 1, 1: Bloom 2. decay scales
 * the loop's feedback (1.0: as the unit) */
void bloom_tick(Bloom *b, int type, float decay, float x, float *l, float *r);

/* Conduit's side: n frames of the 48 kHz stereo send in, the 48 kHz stereo
 * reverb out (out may be in) */
void bloom_process(Bloom *b, int type, float decay, const float *in, float *out, int n);
