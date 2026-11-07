/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Alesis MidiVerb II's Bloom programs, emulated exactly: programs 45
 * ("Bloom 1 8 Sec") and 49 ("Bloom 2 7 Sec") as decompiled from the unit's
 * DSP microcode by thement's midiverb_emulator
 * (github.com/thement/midiverb_emulator, GPL-3; the decompiled programs are
 * that project's work). The same 16-bit integer arithmetic, the same 16 K
 * words of delay memory and the unit's 23437.5 Hz rate, so its output is
 * the emulator's, sample for sample: 13-bit mono in, 13-bit stereo out.
 *
 * Around it, what the emulator doesn't model either: the trip from
 * Conduit's 48 kHz to the unit's rate and back, through a 10 kHz low-pass
 * on each side standing in for the unit's analog filters. */
#pragma once
#include <stdint.h>

#define MV2_DRAM 0x4000

typedef struct {
    int16_t dram[MV2_DRAM];
    unsigned ptr;
    /* 48 kHz <-> 23437.5 Hz (one unit sample every 2.048 of ours: times in 1/125 samples) */
    int64_t t_now, t_next;
    float y_prev;
    int16_t o_prev[2], o_last[2];
    float bq_in[2][2], bq_out[2][2][2];   /* low-pass states: [section][z1,z2], [ch][section][..] */
    float b0[2], b1[2], b2[2], a1[2], a2[2];
} MV2;

void mv2_init(MV2 *m);
void mv2_reset(MV2 *m);
/* one sample at the unit's rate: prog 45 or 49, 13-bit input */
void mv2_tick(MV2 *m, int prog, int16_t in, int16_t *out_l, int16_t *out_r);
/* n frames of Conduit's 48 kHz stereo send in, the reverb out (out may be in) */
void mv2_process(MV2 *m, int prog, const float *in, float *out, int n);
