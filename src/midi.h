/* MIDI input over USB: the kernel's raw MIDI devices (/dev/snd/midiC*D*),
 * read on a thread of its own, no libraries. Clock goes straight to the
 * engine; notes, CCs and pitch bend are queued for the UI. */
#pragma once
#include <stdint.h>
#include "dsp.h"

enum { MM_NOTE = 1, MM_NOTE_OFF, MM_CC, MM_PB, MM_RT };
typedef struct {
    uint8_t type, ch, num;     /* ch 0..15; num: note or CC number (MM_RT: the byte) */
    uint16_t val;              /* velocity / CC value 0..127; pitch bend 0..16383 */
} MidiMsg;

typedef struct { uint8_t status, d0; int n, need, sysex; } MidiParser;

/* feed one byte: 1 when *m is a complete message (MM_RT for 0xF8..0xFF) */
int midi_parse(MidiParser *p, uint8_t b, MidiMsg *m);

/* start the reader (devices are found and re-found as they come and go).
 * CONDUIT_MIDI_DEV=path[:path...] reads those files instead (tests). */
int  midi_start(Engine *e);
void midi_stop(void);
int  midi_poll(MidiMsg *m);    /* UI thread: next note / CC / bend, 0 if none */
void midi_devices(char *out, int sz);   /* names of the open devices, "" if none */
