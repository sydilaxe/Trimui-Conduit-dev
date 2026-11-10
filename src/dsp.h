/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Conduit audio engine: capture -> four looper tracks -> pan/mix -> master FX -> output. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "verb.h"

#define CD_SR 48000
#define CD_TRACKS 4
#define CD_RING 32768          /* input FIFO frames (power of two) */
#define CD_DLY_MAX (CD_SR * 2) /* 2 s delay line */
#define CD_XFADE (CD_SR / 100) /* loop-seam crossfade: 10 ms */
#define CD_FADE (CD_SR / 200)  /* play / stop / jump fade: 5 ms */
#define CD_PPQ 24              /* MIDI clock ticks per beat */

typedef enum {
    /* per track (Track.p) */
    P_LOOP_START, P_LOOP_END, P_PITCH, P_FINE, P_REVERSE, P_DECAY, P_DECAY_MODE,
    P_VOLUME, P_PAN, P_MUTE, P_FX_SEND, P_VERB_SEND, P_SYNC,
    /* scatter: the loop in slices on the beat, played in a pattern */
    P_SC_MODE, P_SC_DIV, P_SC_AMOUNT, P_SC_GATE, P_SC_BARS, P_SC_VAR,
    P_SC_TRIG, P_SC_HITS, P_SC_STEPS, P_SC_ROT, P_SC_EVOLVE,
    P_TRACK_N,
    /* global (Engine.p) */
    P_MASTER = P_TRACK_N, P_MONITOR,
    /* master FX */
    P_FILT_MODE, P_CUTOFF, P_RESO, P_DRIVE, P_BITS, P_DOWNS,
    P_DLY_SYNC, P_DLY_TIME, P_DLY_FB, P_DLY_MIX, P_PINGPONG,
    P_DLY_TONE, P_DLY_LOWCUT, P_DLY_WOW, P_DLY_WOW_RATE, P_DLY_DRIVE,
    /* the reverb (its own send) */
    P_VERB_TYPE, P_VERB_TIME, P_VERB_LEVEL, P_VERB_PRE, P_VERB_SIZE, P_VERB_DAMP, P_VERB_DIFF,
    P_VERB_WIDTH, P_VERB_LOWCUT, P_VERB_HICUT,
    /* IO */
    P_IN_GAIN, P_TRIGGER, P_THRESH, P_AUTOPLAY, P_SAVE_FMT, P_DUB_ALIGN,
    /* the live input through the effects */
    P_IN_FX_SEND, P_IN_VERB_SEND, P_IN_PRINT,
    /* master scatter: the whole mix (after the FX, before the live input),
     * its settings as a track's P_SC_* (same order) */
    P_MSC_MODE, P_MSC_DIV, P_MSC_AMOUNT, P_MSC_GATE, P_MSC_BARS, P_MSC_VAR,
    P_MSC_TRIG, P_MSC_HITS, P_MSC_STEPS, P_MSC_ROT, P_MSC_EVOLVE,
    P_MSC_FB, P_MSC_FREEZE,    /* the master's own: its output fed back, and held */
    /* sync */
    P_CLOCK, P_REC_WAIT, P_BEATS, P_CLOCK_OFS, P_CLOCK_START, P_CLOCK_OUT,
    P_COUNT
} ParamId;

typedef enum { ST_IDLE, ST_ARMED, ST_REC } RecState;
enum { SYNC_ON = 0, SYNC_FREE = 1 };       /* P_SYNC */
/* P_CLOCK: the first loop (and Conduit leads), the clock coming in, or
 * either: the clock (or a leading Brick on the USB link) while one comes,
 * else the first loop */
enum { CLOCK_INTERNAL = 0, CLOCK_MIDI = 1, CLOCK_AUTO = 2 };
enum { START_FOLLOW = 0, START_IGNORE = 1 }; /* P_CLOCK_START: the clock's Start / Stop / Continue */
/* P_IN_PRINT: takes are the input as it comes (its effects only heard), or
 * the input through its effects (printed into the loop) */
enum { IN_PRINT_DRY = 0, IN_PRINT_WET = 1 };
/* P_SC_MODE: what the pattern does to the steps it picks (Chop: only they
 * sound) */
enum { SC_OFF, SC_SHUFFLE, SC_STUTTER, SC_REVERSE, SC_CHOP, SC_SCATTER, SC_GRAIN, SC_N };
/* P_SC_TRIG: which steps it picks: by chance (Amount), or a Euclidean
 * rhythm (Hits spread evenly over Steps, 0: a bar of them, turned by Rotate) */
enum { SC_TRIG_RANDOM, SC_TRIG_EUCLID };
/* P_SC_DIV: the slice (a step) as a note value, from a bar to a 64th, with
 * triplets and dotted ones; P_SC_BARS: the random choices repeat after 1,
 * 2, 4 or 8 bars */
enum { SC_SZ_BAR, SC_SZ_2, SC_SZ_4, SC_SZ_4T, SC_SZ_8D, SC_SZ_8, SC_SZ_8T, SC_SZ_16D, SC_SZ_16, SC_SZ_16T,
       SC_SZ_32, SC_SZ_64, SC_SIZES };

/* a track's scatter player: where it is in the pattern and what it last read */
typedef struct {
    int active;                /* it played the last frame */
    int step;                  /* slice the playhead is in */
    long pass;                 /* times round the loop */
    double last;               /* where it read the last frame */
    double xpos; int xleft;    /* a jump: where it was, fading out */
    float env;                 /* the gate */
    /* the last step's decision (it holds for the whole step) and what it was for */
    long cg; int cn, cvalid;
    float ctp[P_SC_EVOLVE - P_SC_MODE + 1];
    int c_hit, c_slice, c_rev, c_rep, c_mute; double c_where;
} Scatter;

/* Undo: up to CD_UNDO_LEVELS steps per track, newest last. An overdub keeps a
 * copy of the take as it was before; an erase keeps only the take's length
 * and loop points (the samples stay in the buffer until something is recorded). */
#define CD_UNDO_LEVELS 16
#define CD_UNDO_BYTES ((size_t)64 << 20)   /* copies kept per track, at most */
enum { UNDO_DUB = 1, UNDO_ERASE = 2 };
typedef struct {
    int kind, len;
    float loop_start, loop_end;
    float *data;               /* UNDO_DUB: the take's frames 0..len (stereo) */
} UndoLevel;

typedef struct {
    float ic1[2], ic2[2];      /* SVF state */
    float hold[2]; int hold_n; /* sample-rate reducer */
    float *dly;                /* stereo interleaved, CD_DLY_MAX frames */
    int dly_w;
    float dly_time_s;          /* smoothed, in frames */
    float damp[2];
    float hp[2];               /* the repeats' low-cut */
    double wow_ph;             /* the delay's tape wobble */
    float cutoff_s;            /* smoothed cutoff 0..1 */
} FxState;

typedef struct {
    float p[P_TRACK_N];        /* written by UI thread, read by audio thread */
    volatile float bend;       /* live pitch bend in semitones (stick / MIDI), not saved */
    volatile float xy;         /* the X/Y mixer's gain 0..1 (1: off), on top of the volume, not saved */

    float *buf;                /* stereo interleaved sample memory */
    int cap;                   /* capacity in frames */
    volatile int len;          /* frames recorded */
    volatile int rec_pos;
    volatile int rec_state;    /* RecState */
    volatile int playing;
    volatile int overdub;
    volatile int cmd;          /* pending UI commands */
    UndoLevel undo[CD_UNDO_LEVELS];   /* UI thread only */
    int undo_n;
    size_t undo_bytes;
    volatile int undo_stale;   /* set by the audio thread when a new take overwrites the buffer */
    double pos;                /* playhead (frames) */
    volatile int pos_i;        /* copy for UI */
    volatile unsigned version; /* bumps when buffer content changes */

    /* sync: a synced take's length is a whole number of bars (or 1/2, 1/4 bar)
     * and its playhead follows the transport */
    volatile int synced;       /* this take was recorded in sync */
    double phase0;             /* transport phase (bars) where the take starts */
    volatile double cyc_rec;   /* frames per bar when it was recorded */
    volatile double cycles;    /* its length in bars */
    int arm_sync;              /* ARMED: starts on the next bar line (else on the threshold) */
    int rec_sync, rec_first;   /* REC: quantized take / the first take (sets the bar) */
    double stop_cyc;           /* REC: stop once this many bars are in */
    int stop_len;              /* REC: playable frames once known */

    int last_od;               /* last overdub write index */
    double od_comp;            /* overdub write offset, fixed when writing starts */
    float play_g;              /* play / stop fade */
    float gl, gr;              /* smoothed output gains (volume, pan, mute) */
    float gs, gv;              /* smoothed FX and reverb sends */
    float dc[2];               /* the soft clipper's DC blocker */
    double xpos; int xleft;    /* jump crossfade: the old playhead, fading out */
    int snap;                  /* next locked frame jumps without a crossfade */
    int clear_pending;         /* erase once the fade-out ends */
    volatile int decay_armed;  /* Decay: Always may degrade the loop (the UI saved an undo step first) */
    int writing;               /* the loop was written last frame (overdub or decay) */
    Scatter sc;                /* audio thread */
    volatile int sc_pos_i;     /* where scatter reads (-1: off), for the UI */
    volatile int sc_step_i;    /* ... the step it's in, and times round the loop */
    volatile long sc_pass_i;
} Track;

/* the transport: bars since the start. Internal: the first synced loop is
 * one bar. MIDI: Beats per bar beats of the incoming MIDI clock. */
typedef struct {
    volatile double cyc;       /* frames per bar (0: none yet) */
    volatile double phase;     /* bars */
    double rate;               /* bars per frame (0 while stopped) */
    volatile int running;
    int bar_edge;              /* the last frame crossed a bar line */
    /* MIDI clock follower (audio thread) */
    long ticks;                /* since Start (-1: Start seen, waiting for the first tick) */
    double tick_t, hist[CD_PPQ * 4];
    int hist_n, hist_w;
    int ext_run, seen_transport, locked;
    double err_avg;
    volatile double bpm;       /* measured MIDI clock tempo (0: none), for the UI */
    /* a leading Brick's position over the USB link (link.h): where its sound
     * was (16ths) at lk_t, its exact tempo, whether it plays; lk_t 0: none */
    double lk_t, lk_pos, lk_bpm;
    int lk_play;
    volatile int following;    /* the transport follows a clock or a Brick now (not the first loop) */
    volatile int linked;       /* ...a leading Brick's positions */
    /* where the sound is, for the clock out (written by the audio thread each
     * block, read by the MIDI thread through snap_seq): the phase (bars) heard
     * at snap_t (seconds), bars per second, beats per bar, running */
    volatile uint32_t snap_seq;
    double snap_t, snap_phase, snap_rate;
    int snap_beats, snap_run, snap_follow;
} Transport;

/* master scatter: the mix as it was, the last 21 s, and its player. The
 * steps it doesn't change play the mix as it is; the ones it changes play
 * from the bar before (or, a repeat, from the start of their own step). */
#define MSC_RING (1 << 20)
typedef struct {
    float *ring;               /* stereo, MSC_RING frames, indexed by frame count: what it cuts up */
    float *yring;              /* ... and what came out (fed back a bar later) */
    Scatter s;
    volatile int pos_ago;      /* frames back it reads now (0: the mix as it is, -1: off), for the UI */
    volatile long g;           /* the step now (since the transport started) */
} MScat;

#define CD_EVQ 1024
typedef struct { uint8_t msg; double t, pos, bpm; } ClockEv; /* pos, bpm: a link position (msg 0xF0 / 0xF1) */

typedef struct {
    float p[P_COUNT];          /* global params (the per-track ones are in Track.p) */
    Track trk[CD_TRACKS];
    Transport tr;
    volatile int busy;         /* UI holds the buffers (save/bounce): no writes */

    /* input FIFO (capture thread -> output thread) */
    float ring[CD_RING * 2];
    volatile uint32_t ring_w, ring_r;
    double ring_frac;          /* fractional read position (drift follower) */
    float fill_avg;            /* smoothed FIFO fill at callback start */
    int priming;               /* 1: waiting for the FIFO to reach its target fill */
    int margin;                /* FIFO safety margin (frames): grows after an underrun */
    int in_block, lat_frames;  /* capture block size; device latency estimate (frames), set by the app */

    /* input clock: how many capture frames arrive per output frame, measured.
     * A device that runs at another rate than the driver says (48 kHz sent
     * as 44.1 kHz is 1.5 semitones) is read at its real rate, so takes keep
     * their pitch. */
    double in_ratio;           /* 1.0: input and output agree */
    uint64_t out_total;        /* output frames rendered */
    uint64_t rm_o0, rm_ol;     /* measuring: anchor and window start (output frames) */
    uint32_t rm_w0, rm_wl;     /* ... and the input frames written then */
    int rm_state, rm_bad;      /* 0: warming up after (re)start, 1: measuring */
    volatile int in_restart;   /* set by the app when it (re)opens the capture device */

    /* MIDI clock events (MIDI thread -> output thread) */
    ClockEv evq[CD_EVQ];
    volatile uint32_t ev_w, ev_r;
    double (*now)(void);       /* seconds, monotonic (tests replace it) */

    float mon_g;               /* input monitor gain */
    volatile uint32_t underruns, overruns;
    volatile float in_peak, out_peak;
    FxState fx;
    Verb verb;
    /* the live input's sends: smoothed FX and reverb send, and its own FX
     * and reverb (the same settings) for when they're printed into takes */
    float in_gs, in_gv;
    int in_print;              /* the print mode the input chain last ran in */
    FxState fx_in;
    Verb verb_in;
    MScat msc;
} Engine;

int  eng_init(Engine *e, int max_seconds);   /* per track */
void eng_free(Engine *e);
void eng_default_params(float *p);
void eng_default_track(float *tp);
void eng_reset_fx_params(float *p);
void eng_default_verb(float *p);

/* capture callback side */
void eng_push_input(Engine *e, const float *in, int frames);
/* output callback side */
void eng_render(Engine *e, float *out, int frames);
/* MIDI thread: a real-time message (0xF8 clock, 0xFA start, 0xFB continue,
 * 0xFC stop) that arrived at time t (seconds, on e->now's clock) */
void eng_midi_clock(Engine *e, uint8_t msg, double t);
double eng_clock(void);        /* the monotonic clock (seconds) MIDI times are on */
/* MIDI thread: a leading Brick's position over the USB link (link.h): where
 * its sound was at time t (16ths since its start), its exact tempo, whether
 * it plays. One producer at a time with eng_midi_clock. */
void eng_link_pos(Engine *e, double pos16, double bpm, int playing, double t);
/* MIDI thread: where the sound is at time t (16ths since the transport
 * started), the tempo, and whether it runs and follows something else;
 * 0 until the audio has run */
int eng_heard(Engine *e, double t, double *pos16, double *bpm, int *running, int *following);

/* UI-side actions on track t. Those marked [lock] must be called with the
 * output device locked (or before audio starts). */
void eng_rec_button(Engine *e, int t);    /* idle->rec/armed, rec->stop (on the bar when synced) */
void eng_play_toggle(Engine *e, int t);
void eng_overdub_toggle(Engine *e, int t);/* turning it on saves an undo step */
void eng_erase(Engine *e, int t);         /* stop (with a fade) and empty the track; undoable */
void eng_clear(Engine *e, int t);         /* [lock] */
int  eng_undo(Engine *e, int t);          /* [lock] 0: nothing to undo, UNDO_DUB or UNDO_ERASE */
int  eng_undo_count(Engine *e, int t);    /* undo steps available */
void eng_ui_tick(Engine *e);              /* call often from the UI: saves an undo step before Decay: Always starts */
void eng_set_buffer(Engine *e, int t, const float *st, int frames); /* [lock] */

/* the playable loop of track t is [ls, le) (the 10 ms seam crossfade is
 * already taken off le) */
void eng_loop_range(const Engine *e, int t, int *ls, int *le);
double eng_rate(const float *tp, float bend);
/* scatter: the steps in a loop window L frames long (bar: the take's frames
 * per bar, 0 if it has none: the window is a bar), the steps in a bar, the
 * Euclidean cycle, and what the pattern does at step index g (passes *
 * steps + step): picked or not, which slice (or for Grain where, 0..1),
 * backwards, how many repeats, muted */
int  sc_steps(const float *tp, int L, double bar, int beats);
int  sc_bar_steps(const float *tp, int beats);      /* rounded */
double sc_bar_stepsf(const float *tp, int beats);   /* exact: a dotted 8th is 5 1/3 a bar of 4 */
int  sc_cycle(const float *tp, int beats);
void sc_decide(const float *tp, int track, long g, int st, int nsteps, int beats,
               int *hit, int *slice, double *where, int *rev, int *rep, int *mute);
/* the master scatter's settings from the global params, as a track's (tp[P_SC_*]) */
void msc_params(const float *p, float *tp);
/* [lock] the master scatter's output as heard: the last whole patterns (up
 * to max_frames, at most 8 bars) up to the last bar line, into st; frames,
 * 0 if there's no bar or not that much yet */
int  eng_msc_capture(Engine *e, float *st, int max_frames);
#define MSC_TRACK CD_TRACKS    /* its "track" number in sc_decide */
int  eng_track_synced(const Engine *e, int t);   /* plays in sync now */

/* Offline mixdown from bar 1: the mix (through the master FX) and each
 * track (volume and pan, no FX), so the files line up in a DAW. The length
 * is what all synced tracks repeat in (or the longest free one), at most
 * max_frames. The UI sets e->busy around it. */
typedef struct {
    int frames, done;
    float p[P_COUNT], tp[CD_TRACKS][P_TRACK_N];
    double pos[CD_TRACKS], speed[CD_TRACKS], cyc;
    int on[CD_TRACKS], ls[CD_TRACKS], le[CD_TRACKS], xf[CD_TRACKS];
    float gl[CD_TRACKS], gr[CD_TRACKS], mute[CD_TRACKS], send[CD_TRACKS], vsend[CD_TRACKS], dc[CD_TRACKS][2];
    Scatter sc[CD_TRACKS];
    MScat msc;                 /* ring only while master scatter is on */
    uint64_t msc_f;            /* frames through it, the thrown-away first pass included */
    FxState fx;
    Verb verb;
} Mixdown;
int  eng_mix_begin(Engine *e, Mixdown *m, int max_frames);  /* frames, 0 if there's nothing */
void eng_mix_render(Engine *e, Mixdown *m, float *mix, float *stems[CD_TRACKS], int n);
void eng_mix_end(Mixdown *m);
int  eng_track_has(const Engine *e, int t);

/* FX building blocks (exposed for tests) */
int  fx_init(FxState *f);
void fx_free(FxState *f);
void fx_reset(FxState *f, const float *p);
/* process n stereo frames in place; bar = frames per bar (tempo-synced
 * delay), 0 if unknown */
void fx_block(FxState *f, const float *p, double bar, float *st, int n);
