/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The reverb send: input tone (low-cut, high-cut) -> pre-delay -> a Bloom
 * (the exact MidiVerb II emulation, or the adjustable one) -> width -> level. */
#pragma once
#include "bloom.h"
#include "midiverb2.h"

#define VERB_PRE_MAX 0.5f       /* seconds */

typedef struct { float b0, b1, b2, a1, a2; float z[2][2]; float f; } VerbEq;   /* f: the frequency it was set for */

typedef struct {
    Bloom bloom;
    MV2 mv;
    int type;                  /* the type running (-1: none yet) */
    float *pre; int pre_cap, pre_w;
    VerbEq lo, hi;
} Verb;

enum { VERB_EXACT_1, VERB_EXACT_2, VERB_ADJ_1, VERB_ADJ_2 };

int  verb_init(Verb *v);
void verb_free(Verb *v);
void verb_reset(Verb *v);
/* n frames of the stereo send in, the reverb's return out (in place), with
 * the engine's global params p (P_VERB_*) */
void verb_process(Verb *v, const float *p, float *buf, int n);
/* Reverb time (s) and size -> the adjustable Bloom's loop gain */
float verb_decay(float seconds, float size);
