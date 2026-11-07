/* SPDX-License-Identifier: GPL-3.0-or-later */
/* MIDI over USB: the kernel's raw MIDI devices (/dev/snd/midiC*D*), read on
 * a thread of its own, no libraries. Clock goes straight to the engine;
 * notes, CCs and pitch bend are queued for the UI. While Conduit leads (Sync
 * to Internal, or Auto with no clock coming) and Send MIDI clock is on, the
 * same thread sends clock, Start (or song position and Continue) and Stop to
 * every port, timed to the sound as it is heard; and to Bricks following on
 * the USB link (link.h) it sends that and where its sound is, 50 times a
 * second. A Brick leading over the link is followed through the same path. */
#pragma once
#include <stdint.h>
#include "dsp.h"

enum { MM_NOTE = 1, MM_NOTE_OFF, MM_CC, MM_PB, MM_RT };
typedef struct {
    uint8_t type, ch, num;     /* ch 0..15; num: note or CC number (MM_RT: the byte) */
    uint16_t val;              /* velocity / CC value 0..127; pitch bend 0..16383 */
} MidiMsg;

typedef struct {
    uint8_t status, d0; int n, need, sysex;
    uint8_t sx[16]; int sxn, sx_done;   /* a SysEx's bytes (the link's position), sx_done at its F7 */
} MidiParser;

/* feed one byte: 1 when *m is a complete message (MM_RT for 0xF8..0xFF) */
int midi_parse(MidiParser *p, uint8_t b, MidiMsg *m);

/* start the reader (devices are found and re-found as they come and go).
 * CONDUIT_MIDI_DEV=path[:path...] reads those files instead (tests). */
int  midi_start(Engine *e);
void midi_stop(void);
int  midi_poll(MidiMsg *m);    /* UI thread: next note / CC / bend, 0 if none */
void midi_devices(char *out, int sz);   /* names of the open devices, "" if none */
/* Bricks following on the USB link, and this one's own link: 0 off, 1 waiting
 * for a leader, 2 a leader sending */
void midi_link_status(int *followers, int *follow_state);

/* the link's position message: F0 7D 'T' 'L', the 16ths heard x 65536 (five
 * 7-bit bytes, low first), BPM x 1000 (three), playing, F7 (14 bytes) */
int midi_link_pos(uint8_t *out, int max, double pos16, double bpm, int playing);
/* the clock out, stepped: the bytes to send for the sound heard at pos16
 * (16ths since the transport started) while run */
typedef struct { int running, pending; long sent, cont_at; } MidiClockOut;
int midi_clock_out(MidiClockOut *co, double pos16, int run, uint8_t *out, int max);
