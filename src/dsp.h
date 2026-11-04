/* Conduit audio engine: capture -> sampler/looper -> FX chain -> output. */
#pragma once
#include <stdint.h>

#define CD_SR 48000
#define CD_RING 32768          /* input FIFO frames (power of two) */
#define CD_DLY_MAX (CD_SR * 2) /* 2 s delay line */

typedef enum {
    /* LOOP page */
    P_LOOP_START, P_LOOP_END, P_PITCH, P_FINE, P_REVERSE, P_ODUB_KEEP,
    P_VOLUME, P_MONITOR,
    /* FX page */
    P_FILT_MODE, P_CUTOFF, P_RESO, P_DRIVE, P_BITS, P_DOWNS,
    P_DLY_SYNC, P_DLY_TIME, P_DLY_FB, P_DLY_MIX, P_PINGPONG,
    /* IO page */
    P_IN_GAIN, P_TRIGGER, P_THRESH, P_AUTOPLAY, P_SAVE_FMT,
    P_COUNT
} ParamId;

typedef enum { ST_IDLE, ST_ARMED, ST_REC } RecState;

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
    float p[P_COUNT];          /* written by UI thread, read by audio thread */

    float *buf;                /* stereo interleaved sample memory */
    uint8_t *stamp;            /* per-frame overdub undo stamps */
    float *undo;
    int cap;                   /* capacity in frames */
    volatile int len;          /* frames recorded */
    volatile int rec_pos;
    volatile int rec_state;    /* RecState */
    volatile int playing;
    volatile int overdub;
    volatile int busy;         /* UI holds buffer (save/bounce): no writes */
    uint8_t od_session;
    int have_undo;
    double pos;                /* playhead (frames) */
    volatile int pos_i;        /* copy for UI */
    volatile unsigned version; /* bumps when buffer content changes */

    /* input FIFO (capture thread -> output thread) */
    float ring[CD_RING * 2];
    volatile uint32_t ring_w, ring_r;

    volatile int cmd;          /* pending UI commands (CMD_* bits) */
    int last_od;               /* last overdub write index */
    volatile uint32_t underruns, overruns;
    volatile float in_peak, out_peak;
    FxState fx;
} Engine;

int  eng_init(Engine *e, int max_seconds);
void eng_free(Engine *e);
void eng_default_params(float *p);
void eng_reset_fx_params(float *p);

/* capture callback side */
void eng_push_input(Engine *e, const float *in, int frames);
/* output callback side */
void eng_render(Engine *e, float *out, int frames);

/* UI-side actions. Those marked [lock] must be called with the output
 * device locked (or before audio starts). */
void eng_rec_button(Engine *e);           /* idle->rec/armed, rec->stop */
void eng_play_toggle(Engine *e);
void eng_overdub_toggle(Engine *e);
void eng_clear(Engine *e);                /* [lock] */
int  eng_undo(Engine *e);                 /* [lock] returns 1 if restored */
void eng_set_buffer(Engine *e, const float *st, int frames); /* [lock] */

/* loop helpers */
void eng_loop_range(const Engine *e, int *ls, int *le);
double eng_rate(const float *p);

/* Offline render of one seamless pass of the loop through the FX chain.
 * Returns malloc'd stereo buffer, frame count in *out_frames. */
float *eng_bounce(Engine *e, int *out_frames);

/* FX building blocks (exposed for tests) */
int  fx_init(FxState *f);
void fx_free(FxState *f);
void fx_reset(FxState *f, const float *p);
/* process n stereo frames in place; loop_period = loop length in output
 * frames (used for tempo-synced delay), 0 if unknown */
void fx_block(FxState *f, const float *p, double loop_period, float *st, int n);
