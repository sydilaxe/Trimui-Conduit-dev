/* Conduit audio engine: capture -> four looper tracks -> pan/mix -> master FX -> output. */
#pragma once
#include <stddef.h>
#include <stdint.h>

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
    P_VOLUME, P_PAN, P_MUTE, P_FX_SEND, P_SYNC,
    P_TRACK_N,
    /* global (Engine.p) */
    P_MASTER = P_TRACK_N, P_MONITOR,
    /* master FX */
    P_FILT_MODE, P_CUTOFF, P_RESO, P_DRIVE, P_BITS, P_DOWNS,
    P_DLY_SYNC, P_DLY_TIME, P_DLY_FB, P_DLY_MIX, P_PINGPONG,
    /* IO */
    P_IN_GAIN, P_TRIGGER, P_THRESH, P_AUTOPLAY, P_SAVE_FMT, P_DUB_ALIGN,
    /* sync */
    P_CLOCK, P_REC_WAIT, P_BEATS, P_CLOCK_OFS,
    P_COUNT
} ParamId;

typedef enum { ST_IDLE, ST_ARMED, ST_REC } RecState;
enum { SYNC_ON = 0, SYNC_FREE = 1 };       /* P_SYNC */
enum { CLOCK_INTERNAL = 0, CLOCK_MIDI = 1 }; /* P_CLOCK */

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
    float cutoff_s;            /* smoothed cutoff 0..1 */
} FxState;

typedef struct {
    float p[P_TRACK_N];        /* written by UI thread, read by audio thread */
    volatile float bend;       /* live pitch bend in semitones (stick / MIDI), not saved */

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
    float gs;                  /* smoothed FX send */
    double xpos; int xleft;    /* jump crossfade: the old playhead, fading out */
    int snap;                  /* next locked frame jumps without a crossfade */
    int clear_pending;         /* erase once the fade-out ends */
    volatile int decay_armed;  /* Decay: Always may degrade the loop (the UI saved an undo step first) */
    int writing;               /* the loop was written last frame (overdub or decay) */
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
} Transport;

#define CD_EVQ 1024
typedef struct { uint8_t msg; double t; } ClockEv;

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
} Engine;

int  eng_init(Engine *e, int max_seconds);   /* per track */
void eng_free(Engine *e);
void eng_default_params(float *p);
void eng_default_track(float *tp);
void eng_reset_fx_params(float *p);

/* capture callback side */
void eng_push_input(Engine *e, const float *in, int frames);
/* output callback side */
void eng_render(Engine *e, float *out, int frames);
/* MIDI thread: a real-time message (0xF8 clock, 0xFA start, 0xFB continue,
 * 0xFC stop) that arrived at time t (seconds, on e->now's clock) */
void eng_midi_clock(Engine *e, uint8_t msg, double t);
double eng_clock(void);        /* the monotonic clock (seconds) MIDI times are on */

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
    float gl[CD_TRACKS], gr[CD_TRACKS], mute[CD_TRACKS], send[CD_TRACKS];
    FxState fx;
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
