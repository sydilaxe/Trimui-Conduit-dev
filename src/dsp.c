/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsp.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CMD_REC  1
#define CMD_PLAY 2
#define CMD_ODUB 4
#define CMD_CLEAR 8
#define BLK 128
#define MIN_TAKE (CD_SR / 50)  /* 20 ms */
#define JUMP_TOL 4             /* frames: a locked playhead further off than this jumps (crossfaded) */

static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline int clampi(int x, int a, int b) { return x < a ? a : x > b ? b : x; }
static inline double clampd(double x, double a, double b) { return x < a ? a : x > b ? b : x; }
static inline float db2lin(float db) { return powf(10.f, db / 20.f); }

double eng_clock(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void eng_default_track(float *tp)
{
    memset(tp, 0, sizeof(float) * P_TRACK_N);
    tp[P_LOOP_START] = 0.f; tp[P_LOOP_END] = 1.f;
    tp[P_DECAY] = 0.02f; tp[P_DECAY_MODE] = 0.f;
    tp[P_VOLUME] = 0.9f; tp[P_PAN] = 0.f; tp[P_MUTE] = 0.f; tp[P_FX_SEND] = 1.f; tp[P_VERB_SEND] = 0.f; tp[P_SYNC] = SYNC_ON;
    tp[P_SC_MODE] = SC_OFF; tp[P_SC_DIV] = 2.f; tp[P_SC_AMOUNT] = 0.5f; tp[P_SC_GATE] = 1.f; tp[P_SC_BARS] = 0.f; tp[P_SC_VAR] = 0.f;
}

void eng_default_params(float *p)
{
    memset(p, 0, sizeof(float) * P_COUNT);
    eng_default_track(p);   /* unused slots, kept sane */
    p[P_MASTER] = 1.f; p[P_MONITOR] = 0.f;
    eng_reset_fx_params(p);
    eng_default_verb(p);
    p[P_IN_GAIN] = 0.f;     p[P_TRIGGER] = 0.f; p[P_THRESH] = -36.f;
    p[P_AUTOPLAY] = 1.f;    p[P_SAVE_FMT] = 0.f; p[P_DUB_ALIGN] = 0.f;
    p[P_IN_FX_SEND] = 0.f;  p[P_IN_VERB_SEND] = 0.f; p[P_IN_PRINT] = IN_PRINT_DRY;
    p[P_CLOCK] = CLOCK_INTERNAL; p[P_REC_WAIT] = 1.f; p[P_BEATS] = 4.f; p[P_CLOCK_OFS] = 0.f;
    p[P_CLOCK_START] = START_FOLLOW; p[P_CLOCK_OUT] = 1.f;
}

void eng_reset_fx_params(float *p)
{
    p[P_FILT_MODE] = 0.f; p[P_CUTOFF] = 1.f; p[P_RESO] = 0.2f;
    p[P_DRIVE] = 0.f;     p[P_BITS] = 16.f;  p[P_DOWNS] = 1.f;
    p[P_DLY_SYNC] = 0.f;  p[P_DLY_TIME] = 350.f; p[P_DLY_FB] = 0.4f;
    p[P_DLY_MIX] = 0.f;   p[P_PINGPONG] = 0.f;
    p[P_DLY_TONE] = 0.5f; p[P_DLY_LOWCUT] = 20.f; p[P_DLY_WOW] = 0.f; p[P_DLY_WOW_RATE] = 0.8f; p[P_DLY_DRIVE] = 0.f;
}

void eng_default_verb(float *p)
{
    p[P_VERB_TYPE] = VERB_EXACT_1; p[P_VERB_TIME] = 8.f; p[P_VERB_LEVEL] = 0.8f; p[P_VERB_PRE] = 0.f;
    p[P_VERB_SIZE] = 1.f; p[P_VERB_DAMP] = 0.5f; p[P_VERB_DIFF] = 0.5f; p[P_VERB_WIDTH] = 1.f;
    p[P_VERB_LOWCUT] = 20.f; p[P_VERB_HICUT] = 16000.f;
}

/* ---------------------------------------------------------------- FX */

int fx_init(FxState *f)
{
    memset(f, 0, sizeof(*f));
    f->dly = calloc((size_t)CD_DLY_MAX * 2, sizeof(float));
    return f->dly ? 0 : -1;
}

void fx_free(FxState *f) { free(f->dly); f->dly = NULL; }

static const float sync_div[] = { 0.f, 1.f / 16, 1.f / 8, 1.f / 4, 1.f / 3, 1.f / 2, 1.f };

static float dly_target(const float *p, double bar)
{
    int s = (int)p[P_DLY_SYNC];
    float t;
    if (s > 0 && s < 7 && bar > 0) t = (float)(bar * sync_div[s]);
    else t = p[P_DLY_TIME] * 0.001f * CD_SR;
    return clampf(t, 16.f, (float)(CD_DLY_MAX - 4));
}

void fx_reset(FxState *f, const float *p)
{
    float *d = f->dly;
    memset(f, 0, sizeof(*f));
    f->dly = d;
    if (d) memset(d, 0, sizeof(float) * CD_DLY_MAX * 2);
    f->dly_time_s = dly_target(p, 0);
    f->cutoff_s = p[P_CUTOFF];
}

void fx_block(FxState *f, const float *p, double bar, float *st, int n)
{
    int mode = (int)p[P_FILT_MODE];
    float drive = p[P_DRIVE];
    int bits = (int)p[P_BITS];
    int downs = (int)p[P_DOWNS];
    float fb = clampf(p[P_DLY_FB], 0.f, 0.95f);
    float mix = p[P_DLY_MIX];
    int ping = p[P_PINGPONG] > 0.5f;
    float tgt = dly_target(p, bar);
    /* the repeats: how dark (tone: 1-16 kHz), how thin (low-cut), how they
     * wobble (wow) and how gritty they get (drive) */
    float dc = 1.f - expf(-2.f * (float)M_PI * 1000.f * powf(16.f, clampf(p[P_DLY_TONE], 0.f, 1.f)) / CD_SR);
    float hc = 1.f - expf(-2.f * (float)M_PI * clampf(p[P_DLY_LOWCUT], 20.f, 2000.f) / CD_SR);
    int lowcut = p[P_DLY_LOWCUT] > 20.5f;
    float wow = clampf(p[P_DLY_WOW], 0.f, 1.f) * 0.004f * CD_SR;   /* up to +-4 ms */
    double wstep = 2 * M_PI * clampf(p[P_DLY_WOW_RATE], 0.05f, 10.f) / CD_SR;
    float dg = 1.f + 4.f * clampf(p[P_DLY_DRIVE], 0.f, 1.f), dsat = p[P_DLY_DRIVE] > 0.001f;

    /* smooth cutoff once per block */
    f->cutoff_s += (p[P_CUTOFF] - f->cutoff_s) * 0.25f;
    float fc = 20.f * powf(1000.f, clampf(f->cutoff_s, 0.f, 1.f));
    fc = clampf(fc, 20.f, 0.45f * CD_SR);
    float g = tanf((float)M_PI * fc / CD_SR);
    float k = 2.f - 2.f * clampf(p[P_RESO], 0.f, 0.97f);
    float a1 = 1.f / (1.f + g * (g + k)), a2 = g * a1, a3 = g * a2;

    float drg = 1.f + drive * 15.f, dnorm = 1.f / tanhf(drg);
    float q = (float)(1 << (bits > 1 ? bits - 1 : 1));

    for (int i = 0; i < n; i++) {
        float x[2] = { st[2 * i], st[2 * i + 1] };
        if (drive > 0.001f) { x[0] = tanhf(x[0] * drg) * dnorm; x[1] = tanhf(x[1] * drg) * dnorm; }
        if (downs > 1) {
            if (f->hold_n <= 0) { f->hold[0] = x[0]; f->hold[1] = x[1]; f->hold_n = downs; }
            f->hold_n--;
            x[0] = f->hold[0]; x[1] = f->hold[1];
        }
        for (int c = 0; c < 2; c++) {
            float v = x[c];
            if (bits < 16) v = floorf(v * q + 0.5f) / q;
            if (mode > 0) {
                float v3 = v - f->ic2[c];
                float v1 = a1 * f->ic1[c] + a2 * v3;
                float v2 = f->ic2[c] + a2 * f->ic1[c] + a3 * v3;
                f->ic1[c] = 2.f * v1 - f->ic1[c];
                f->ic2[c] = 2.f * v2 - f->ic2[c];
                v = mode == 1 ? v2 : mode == 2 ? (v - k * v1 - v2) : v1;
            }
            x[c] = v;
        }

        /* delay with smoothed (tape-style) time and damped feedback */
        f->dly_time_s += (tgt - f->dly_time_s) * 0.0005f;
        float wo = 0.f;
        if (wow > 0.f) { f->wow_ph += wstep; if (f->wow_ph > 2 * M_PI) f->wow_ph -= 2 * M_PI; wo = wow * (float)sin(f->wow_ph); }
        float rp = (float)f->dly_w - clampf(f->dly_time_s + wo, 4.f, (float)(CD_DLY_MAX - 4));
        while (rp < 0) rp += CD_DLY_MAX;
        int r0 = (int)rp;
        if (r0 >= CD_DLY_MAX) { r0 = 0; rp = 0; }  /* float rounding at the wrap */
        float fr = rp - r0; int r1 = r0 + 1 == CD_DLY_MAX ? 0 : r0 + 1;
        float dl = f->dly[2 * r0] * (1 - fr) + f->dly[2 * r1] * fr;
        float dr = f->dly[2 * r0 + 1] * (1 - fr) + f->dly[2 * r1 + 1] * fr;
        f->damp[0] += dc * (dl - f->damp[0]);
        f->damp[1] += dc * (dr - f->damp[1]);
        float rl = f->damp[0], rr = f->damp[1];
        if (lowcut) { f->hp[0] += hc * (rl - f->hp[0]); f->hp[1] += hc * (rr - f->hp[1]); rl -= f->hp[0]; rr -= f->hp[1]; }
        if (dsat) { rl = tanhf(rl * dg) / dg; rr = tanhf(rr * dg) / dg; }
        float wl, wr;
        if (ping) { wl = 0.5f * (x[0] + x[1]) + fb * rr; wr = fb * rl; }
        else { wl = x[0] + fb * rl; wr = x[1] + fb * rr; }
        f->dly[2 * f->dly_w] = clampf(wl, -4.f, 4.f);
        f->dly[2 * f->dly_w + 1] = clampf(wr, -4.f, 4.f);
        if (++f->dly_w == CD_DLY_MAX) f->dly_w = 0;

        st[2 * i] = x[0] + mix * dl;
        st[2 * i + 1] = x[1] + mix * dr;
    }
}

/* ------------------------------------------------------------ engine */

int eng_init(Engine *e, int max_seconds)
{
    memset(e, 0, sizeof(*e));
    for (int i = 0; i < CD_TRACKS; i++) {
        Track *t = &e->trk[i];
        t->cap = max_seconds * CD_SR;
        t->buf = calloc((size_t)t->cap * 2, sizeof(float));
        if (!t->buf) { eng_free(e); return -1; }
        eng_default_track(t->p);
        t->gs = t->p[P_FX_SEND];
        t->xy = 1.f;
        t->last_od = -1;
    }
    if (fx_init(&e->fx) || verb_init(&e->verb) || fx_init(&e->fx_in) || verb_init(&e->verb_in)) { eng_free(e); return -1; }
    eng_default_params(e->p);
    fx_reset(&e->fx, e->p);
    fx_reset(&e->fx_in, e->p);
    e->in_block = 512;
    e->lat_frames = 3 * 512;
    e->priming = 1;
    e->margin = 64;
    e->in_ratio = 1.0;
    e->tr.ticks = -1;
    return 0;
}

static void undo_reset(Track *t);

void eng_free(Engine *e)
{
    for (int i = 0; i < CD_TRACKS; i++) {
        Track *t = &e->trk[i];
        free(t->buf); t->buf = NULL;
        undo_reset(t);
    }
    fx_free(&e->fx);
    verb_free(&e->verb);
    fx_free(&e->fx_in);
    verb_free(&e->verb_in);
}

double eng_rate(const float *tp, float bend)
{
    return pow(2.0, (tp[P_PITCH] + tp[P_FINE] / 100.0 + bend) / 12.0);
}

/* The loop region [ls, le) as set by the user, and the crossfade length xf.
 * What plays is [ls, le - xf): its first xf frames are blended with the last
 * xf frames of the region, so the seam where it wraps doesn't click. */
static void loop_region(int len, const float *tp, int *ls, int *le, int *xf)
{
    float a = clampf(tp[P_LOOP_START], 0.f, 1.f), b = clampf(tp[P_LOOP_END], 0.f, 1.f);
    if (b < a) { float t = a; a = b; b = t; }
    int s = (int)(a * len), en = (int)(b * len);
    if (en > len) en = len;
    if (en - s < MIN_TAKE / 4) { en = s + MIN_TAKE / 4; if (en > len) { en = len; s = en - MIN_TAKE / 4; if (s < 0) s = 0; } }
    int f = CD_XFADE;
    if (f > (en - s) / 4) f = (en - s) / 4;
    *ls = s; *le = en; *xf = f;
}

void eng_loop_range(const Engine *e, int t, int *ls, int *le)
{
    int xf; loop_region(e->trk[t].len, e->trk[t].p, ls, le, &xf); *le -= xf;
}

static int lock_mode(const Track *t) { return t->synced && t->p[P_SYNC] < 0.5f; }
int eng_track_synced(const Engine *e, int t) { return lock_mode(&e->trk[t]) && e->trk[t].len > 0; }
int eng_track_has(const Engine *e, int t) { return e->trk[t].len > 1 && e->trk[t].rec_state != ST_REC; }

/* another track already keeps the bar */
static int other_synced(const Engine *e, const Track *self)
{
    for (int i = 0; i < CD_TRACKS; i++) {
        const Track *t = &e->trk[i];
        if (t == self) continue;
        if (t->len > 0 && t->rec_state != ST_REC && lock_mode(t)) return 1;
        if (t->rec_state == ST_REC && (t->rec_sync || t->rec_first)) return 1;
    }
    return 0;
}

/* take lengths in bars: 1/4, 1/2, 1, 2, 3, ... */
static double snap_up(double x) { return x <= 0.25 ? 0.25 : x <= 0.5 ? 0.5 : ceil(x - 1e-9); }
static double snap_down(double x) { return x >= 1 ? floor(x + 1e-9) : x >= 0.5 ? 0.5 : x >= 0.25 ? 0.25 : 0; }
static double snap_near(double x) { return x < 0.375 ? 0.25 : x < 0.75 ? 0.5 : floor(x + 0.5); }

void eng_push_input(Engine *e, const float *in, int frames)
{
    uint32_t w = __atomic_load_n(&e->ring_w, __ATOMIC_RELAXED);
    uint32_t r = __atomic_load_n(&e->ring_r, __ATOMIC_ACQUIRE);
    for (int i = 0; i < frames; i++) {
        if (w - r >= CD_RING) { e->overruns++; break; }
        uint32_t k = (w & (CD_RING - 1)) * 2;
        e->ring[k] = in[2 * i]; e->ring[k + 1] = in[2 * i + 1];
        w++;
    }
    __atomic_store_n(&e->ring_w, w, __ATOMIC_RELEASE);
}

/* the transport follows something else now: always with Sync to MIDI, with
 * Auto while a clock or a leading Brick comes */
static int follows_now(const Engine *e)
{
    int m = (int)(e->p[P_CLOCK] + 0.5f);
    return m == CLOCK_MIDI || (m == CLOCK_AUTO && e->tr.following);
}

static void ev_push(Engine *e, uint8_t msg, double t, double pos, double bpm)
{
    uint32_t w = __atomic_load_n(&e->ev_w, __ATOMIC_RELAXED);
    uint32_t r = __atomic_load_n(&e->ev_r, __ATOMIC_ACQUIRE);
    if (w - r >= CD_EVQ) return;
    ClockEv *ev = &e->evq[w & (CD_EVQ - 1)];
    ev->msg = msg, ev->t = t, ev->pos = pos, ev->bpm = bpm;
    __atomic_store_n(&e->ev_w, w + 1, __ATOMIC_RELEASE);
}

void eng_link_pos(Engine *e, double pos16, double bpm, int playing, double t)
{
    ev_push(e, playing ? 0xF0 : 0xF1, t, pos16, bpm);
}

int eng_heard(Engine *e, double t, double *pos16, double *bpm, int *running, int *following)
{
    Transport *tr = &e->tr;
    for (int tries = 0; tries < 8; tries++) {
        uint32_t s0 = __atomic_load_n(&tr->snap_seq, __ATOMIC_ACQUIRE);
        if (s0 & 1) continue;
        double st = tr->snap_t, ph = tr->snap_phase, rate = tr->snap_rate;
        int beats = tr->snap_beats, run = tr->snap_run, fol = tr->snap_follow;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&tr->snap_seq, __ATOMIC_RELAXED) != s0) continue;
        if (!s0 || beats < 1) return 0;
        *pos16 = (ph + (run ? (t - st) * rate : 0.0)) * beats * 4.0;
        *bpm = rate * beats * 60.0;
        *running = run;
        *following = fol;
        return 1;
    }
    return 0;
}

void eng_midi_clock(Engine *e, uint8_t msg, double t) { ev_push(e, msg, t, 0, 0); }

static void do_clear(Track *t)
{
    t->playing = 0; t->overdub = 0; t->rec_state = ST_IDLE;
    t->len = 0; t->rec_pos = 0; t->pos = 0;
    t->play_g = 0; t->clear_pending = 0; t->decay_armed = 0; t->xleft = 0;
    t->version++;
}

static void start_rec(Engine *e, Track *t, double phase0, int sync, int first)
{
    t->playing = 0; t->overdub = 0; t->clear_pending = 0; t->undo_stale = 1;
    t->rec_pos = 0; t->len = 0; t->rec_state = ST_REC; t->play_g = 0; t->decay_armed = 0;
    t->rec_sync = sync; t->rec_first = first; t->stop_cyc = 0; t->stop_len = 0;
    t->phase0 = phase0; t->synced = 0; t->xleft = 0;
    if (first) { e->tr.cyc = 0; e->tr.running = 0; e->tr.rate = 0; }
    t->version++;
}

static void finish_rec(Engine *e, Track *t)
{
    int len = t->rec_pos;
    t->rec_state = ST_IDLE;
    if (t->rec_sync) {
        if (t->stop_len <= 0) {
            /* ended before the bar it was going to (the buffer filled, the
             * clock stopped): keep what fits a whole length */
            double ec = e->tr.phase - t->phase0, c = snap_down(ec);
            if (c <= 0 || ec <= 0) len = 0;
            else { t->stop_cyc = c; t->stop_len = (int)(len * c / ec); }
        }
        if (len) {
            if (t->stop_len + CD_XFADE < len) len = t->stop_len + CD_XFADE;
            int xf = CD_XFADE < len / 4 ? CD_XFADE : len / 4;
            t->cycles = t->stop_cyc;
            t->cyc_rec = (double)(len - xf) / t->stop_cyc;
            t->synced = len >= MIN_TAKE;
            if (!t->synced) len = 0;
        }
    } else {
        len = len >= MIN_TAKE ? len : 0;
        t->synced = 0;
        if (t->rec_first && len) {
            /* the first synced take is one bar long and starts the transport */
            int xf = CD_XFADE < len / 4 ? CD_XFADE : len / 4;
            double play = len - xf;
            e->tr.cyc = play; e->tr.phase = 0; e->tr.rate = 1.0 / play; e->tr.running = 1;
            t->phase0 = 0; t->cyc_rec = play; t->cycles = 1; t->synced = 1;
        }
    }
    t->rec_sync = t->rec_first = 0;
    t->len = len;
    t->p[P_LOOP_START] = 0.f; t->p[P_LOOP_END] = 1.f;
    int ls, le, xf; loop_region(len, t->p, &ls, &le, &xf);
    t->pos = t->p[P_REVERSE] > 0.5f && len ? le - xf - 1 : 0;
    t->snap = 1;
    t->playing = len > 0 && e->p[P_AUTOPLAY] > 0.5f;
    t->version++;
}

/* stop a synced take on the bar: the shortest whole length that holds what
 * was played (pressed up to a 1/16 bar late still ends on the bar just gone) */
static void quantized_stop(Engine *e, Track *t)
{
    double ec = e->tr.phase - t->phase0;
    double c = snap_up(ec - 1.0 / 16);
    t->stop_cyc = c;
    if (ec >= c && ec > 0) {
        t->stop_len = (int)lround(t->rec_pos * c / ec);
        if (t->stop_len < 1) t->stop_len = 1;
        if (t->rec_pos >= t->stop_len + CD_XFADE) finish_rec(e, t);
    }
}

static void arm(Track *t, int sync) { t->rec_state = ST_ARMED; t->arm_sync = sync; t->playing = 0; t->overdub = 0; }

static void handle_cmds(Engine *e, Track *t)
{
    int c = __atomic_exchange_n(&t->cmd, 0, __ATOMIC_ACQ_REL);
    if (!c) return;
    if (c & CMD_CLEAR) {
        /* fade out first when something is playing; render() clears at the end of the fade */
        if (t->rec_state == ST_REC || t->play_g <= 0.f) do_clear(t);
        else { t->playing = 0; t->overdub = 0; t->rec_state = ST_IDLE; t->clear_pending = 1; }
        return;
    }
    if (c & CMD_REC) {
        if (t->rec_state == ST_REC) {
            if (!t->rec_sync) finish_rec(e, t);
            else if (t->stop_cyc <= 0) quantized_stop(e, t);
        } else if (t->rec_state == ST_ARMED) t->rec_state = ST_IDLE;
        else if (!e->busy) {
            int want_sync = t->p[P_SYNC] < 0.5f, midi = follows_now(e);
            int first = want_sync && !midi && !other_synced(e, t);
            if (want_sync && !first) {
                if (!e->tr.running || e->p[P_REC_WAIT] > 0.5f) arm(t, 1);
                else start_rec(e, t, e->tr.phase, 1, 0);
            } else if (e->p[P_TRIGGER] > 0.5f) arm(t, 0);
            else start_rec(e, t, e->tr.phase, 0, first);
        }
    }
    if (t->clear_pending) return;
    if ((c & CMD_PLAY) && t->rec_state != ST_REC && t->len > 0) {
        t->playing = !t->playing;
        if (!t->playing) t->overdub = 0;
        else if (t->play_g <= 0.f) {
            int ls, le, xf; loop_region(t->len, t->p, &ls, &le, &xf);
            t->pos = t->p[P_REVERSE] > 0.5f ? le - xf - 1 : ls; t->snap = 1;
        }
    }
    if ((c & CMD_ODUB) && t->rec_state != ST_REC && t->len > 0 && !e->busy) {
        if (t->overdub) t->overdub = 0;
        else {
            t->overdub = 1; t->last_od = -1;
            if (!t->playing) {
                if (t->play_g <= 0.f) {
                    int ls, le, xf; loop_region(t->len, t->p, &ls, &le, &xf);
                    t->pos = t->p[P_REVERSE] > 0.5f ? le - xf - 1 : ls; t->snap = 1;
                }
                t->playing = 1;
            }
        }
    }
}

/* Layers that pile up past -2 dB are rounded off like tape instead of hard
 * clipped: unchanged up to 0.8, then easing towards 1.0 (same slope at the
 * knee, so no edge). Below the knee it changes nothing. */
static inline float sat(float x)
{
    float a = fabsf(x);
    if (a <= 0.8f) return x;
    float y = 0.8f + 0.2f * tanhf((a - 0.8f) * 5.f);
    return x < 0 ? -y : y;
}

/* A track turned up until it would distort (volume past 100%, a hot take)
 * is rounded off warmly instead of clipped: untouched below the knee, then
 * easing towards full scale with the same slope at the knee, so nothing
 * changes until it would clip. The knee is a little lower on the positive
 * side, which adds the even harmonics that make tape and valves sound warm;
 * the offset that leaves is taken out of the distortion alone (dc), so a
 * clean signal is never filtered. */
static inline float warm_clip(float x, float *dc)
{
    float a = fabsf(x), k = x > 0 ? 0.75f : 0.9f;
    if (a <= k && fabsf(*dc) < 1e-7f) return x;
    float y = x;
    if (a > k) { y = k + (1 - k) * tanhf((a - k) / (1 - k)); if (x < 0) y = -y; }
    float d = y - x;
    *dc += (d - *dc) * 0.0013f;          /* ~10 Hz */
    if (fabsf(*dc) < 1e-7f && a <= k) *dc = 0;
    return x + d - *dc;
}

static inline void od_write(float *buf, int idx, float il, float ir, float keep)
{
    buf[2 * idx] = sat(buf[2 * idx] * keep + il);
    buf[2 * idx + 1] = sat(buf[2 * idx + 1] * keep + ir);
}

/* overdub one loop frame; the crossfaded head also writes its tail partner,
 * so what plays there is keep * old + input exactly */
static inline void od_write_loop(float *buf, int idx, int ls, int le, int xf, float il, float ir, float keep)
{
    od_write(buf, idx, il, ir, keep);
    if (idx - ls < xf) od_write(buf, le - xf + (idx - ls), il, ir, keep);
}

/* the loop's sample at q in [ls, le - xf), with the seam crossfade */
static inline void loop_val(const float *buf, int ls, int le, int xf, int q, float *l, float *r)
{
    int d = q - ls;
    if (d < xf) {
        float t = (d + 0.5f) / xf;
        int j = le - xf + d;
        *l = buf[2 * q] * t + buf[2 * j] * (1 - t);
        *r = buf[2 * q + 1] * t + buf[2 * j + 1] * (1 - t);
    } else { *l = buf[2 * q]; *r = buf[2 * q + 1]; }
}

static inline double wrap(double p, int ls, int L)
{
    if (p >= ls && p < ls + L) return p;
    p = fmod(p - ls, (double)L); if (p < 0) p += L;
    if (p >= L) p = 0;
    return ls + p;
}

/* the loop at position p (inside [ls, le - xf)), linearly interpolated */
static inline void loop_at(const float *buf, int ls, int le, int xf, double p, float *l, float *r)
{
    int end = le - xf;
    int i0 = (int)p; float fr = (float)(p - i0);
    if (i0 >= end) i0 = end - 1;
    int i1 = i0 + 1 >= end ? ls : i0 + 1;
    float l0, r0, l1, r1;
    loop_val(buf, ls, le, xf, i0, &l0, &r0);
    loop_val(buf, ls, le, xf, i1, &l1, &r1);
    *l = l0 * (1 - fr) + l1 * fr;
    *r = r0 * (1 - fr) + r1 * fr;
}

/* per block, per track */
typedef struct {
    int ls, le, xf, end, L;
    double speed;              /* buffer frames per output frame (signed) */
    int rev, lock, gate;
    float keep; int decay_always;
    float gl, gr;              /* output gain targets */
    float send, vsend;         /* FX and reverb send targets */
    int scat;                  /* scatter on */
    double scbar;              /* the take's frames per bar for scatter (0: the window is a bar) */
} TC;

static void prep(const Engine *e, const Track *t, TC *c)
{
    const float *tp = t->p;
    memset(c, 0, sizeof *c);
    if (t->len > 1) {
        loop_region(t->len, tp, &c->ls, &c->le, &c->xf);
        c->end = c->le - c->xf; c->L = c->end - c->ls;
    }
    double user = eng_rate(tp, t->bend);
    c->rev = tp[P_REVERSE] > 0.5f;
    int sy = lock_mode(t);
    c->gate = !sy || e->tr.running;
    c->lock = sy && e->tr.running && fabs(user - 1.0) < 1e-9;
    double sp = sy ? (e->tr.running ? user * t->cyc_rec * e->tr.rate : 0) : user;
    c->speed = c->rev ? -sp : sp;
    /* loop decay: each pass of the write head scales the loop by keep, so
     * layers fade away over time and stacked overdubs can't keep growing */
    c->keep = 1.f - clampf(tp[P_DECAY], 0.f, 1.f);
    c->decay_always = tp[P_DECAY_MODE] > 0.5f && c->keep < 1.f && t->decay_armed;
    /* pan: a stereo balance with equal power, unity in the middle */
    float vol = tp[P_MUTE] > 0.5f ? 0.f : tp[P_VOLUME] * clampf(t->xy, 0.f, 1.f);
    double a = (clampf(tp[P_PAN], -1.f, 1.f) + 1.0) * M_PI / 4;
    c->gl = vol * (float)fmin(1.0, M_SQRT2 * cos(a));
    c->gr = vol * (float)fmin(1.0, M_SQRT2 * sin(a));
    c->send = clampf(tp[P_FX_SEND], 0.f, 1.f);
    c->vsend = clampf(tp[P_VERB_SEND], 0.f, 1.f);
    c->scat = tp[P_SC_MODE] >= 0.5f;
    c->scbar = t->synced && t->cyc_rec > 0 ? t->cyc_rec : 0;
}

/* where a locked track plays: the take keeps its place in the bar */
static double lock_target(const Engine *e, const Track *t, const TC *c)
{
    double el = (e->tr.phase - t->phase0) * t->cyc_rec;
    double d = fmod(el - c->ls, (double)c->L);
    if (d < 0) d += c->L;
    if (d >= c->L) d = 0;
    return c->rev ? c->end - 1 - d : c->ls + d;
}

/* a jump: the old playhead keeps going where it was (next at from) and
 * fades out under the new one */
static void start_jump(Track *t, double from)
{
    if (t->play_g <= 0.f) return;
    t->xpos = from;
    t->xleft = CD_FADE;
}

/* the take at any position (the fading playhead may be outside the window) */
static inline void raw_at(const float *buf, int len, double p, float *l, float *r)
{
    if (p < 0) p = 0;
    if (p > len - 1.001) p = len - 1.001;
    int i0 = (int)p; float fr = (float)(p - i0);
    *l = buf[2 * i0] * (1 - fr) + buf[2 * i0 + 2] * fr;
    *r = buf[2 * i0 + 1] * (1 - fr) + buf[2 * i0 + 3] * fr;
}

/* ------------------------------------------------------------ scatter */

int sc_steps(const float *tp, int L, double bar, int beats)
{
    int spb = (beats < 1 ? 1 : beats) << clampi((int)tp[P_SC_DIV], 0, 3);   /* steps per bar */
    double step = (bar > 0 ? bar : (double)L) / spb;
    long n = step > 0 ? lround(L / step) : 1;
    return n < 1 ? 1 : n > 4096 ? 4096 : (int)n;
}

static uint32_t sc_hash(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint32_t x = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du ^ (d + 1) * 0x27D4EB2Fu;
    x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12; x *= 0x297A2D39u; x ^= x >> 15;
    return x;
}

/* the pattern: the same step of the pattern does the same thing every time
 * round, and Variation picks another pattern */
void sc_decide(const float *tp, int track, long g, int st, int nsteps, int beats,
               int *slice, double *where, int *rev, int *rep, int *mute)
{
    int mode = clampi((int)tp[P_SC_MODE], 0, SC_N - 1);
    float a = clampf(tp[P_SC_AMOUNT], 0.f, 1.f);
    int spb = (beats < 1 ? 1 : beats) << clampi((int)tp[P_SC_DIV], 0, 3);
    long patlen = (long)spb << clampi((int)tp[P_SC_BARS], 0, 3);
    long idx = g % patlen; if (idx < 0) idx += patlen;
    uint32_t var = (uint32_t)clampi((int)tp[P_SC_VAR], 0, 999);
#define R(salt) (sc_hash(var, (uint32_t)track, (uint32_t)idx, (salt)) * (1.0 / 4294967296.0))
    *slice = st; *where = -1; *rev = 0; *rep = 1; *mute = 0;
    switch (mode) {
    case SC_SHUFFLE:
        if (R(1) < a) *slice = (int)(R(2) * nsteps);
        break;
    case SC_STUTTER:
        if (R(1) < a) {
            static const int reps[] = { 2, 2, 3, 4, 4, 6, 8 };
            *rep = reps[(int)(R(2) * 7)];
            if (R(3) < 0.3) *slice = (st + nsteps - 1) % nsteps;   /* hold the last one */
        }
        break;
    case SC_REVERSE:
        if (R(1) < a) *rev = 1;
        break;
    case SC_EUCLID: {
        /* Amount of the bar's steps sound, spread as evenly as they go */
        int k = (int)lroundf(a * spb); k = k < 1 ? 1 : k > spb ? spb : k;
        int rot = (int)(sc_hash(var, (uint32_t)track, 0, 9) % (uint32_t)spb);
        *mute = ((idx % spb + rot) * k) % spb >= k;
        break;
    }
    case SC_SCATTER:
        if (R(1) < a) switch ((int)(R(4) * 6)) {
            case 0: *slice = (int)(R(2) * nsteps); break;
            case 1: *rep = 2; break;
            case 2: *rep = 4; break;
            case 3: *rev = 1; break;
            case 4: *slice = (int)(R(2) * nsteps); *rev = 1; break;
            default: *mute = 1; break;
        }
        break;
    case SC_GRAIN: {
        static const int reps[] = { 1, 1, 2, 4 };
        if (R(1) < a) *where = R(2);
        *rep = reps[(int)(R(3) * 4)];
        *rev = R(5) < 0.25 * a;
        break;
    }
    }
#undef R
    if (*slice < 0) *slice = 0;
    if (*slice >= nsteps) *slice = nsteps - 1;
}

/* track k's loop through its scatter pattern at the playhead pos: the slice
 * the pattern puts there, crossfaded where it jumps, through the gate */
static void sc_read(Scatter *s, const float *tp, int k, const float *buf, int len, int ls, int le, int xf,
                    double pos, double speed, double bar, int beats, float *l, float *r)
{
    int L = le - xf - ls;
    int n = sc_steps(tp, L, bar, beats);
    double SL = (double)L / n, rel = pos - ls;
    if (rel < 0) rel = 0;
    if (rel >= L) rel = L - 1e-6;
    int st = (int)(rel / SL); if (st >= n) st = n - 1;
    double f = rel - st * SL;
    if (!s->active) { s->pass = 0; s->step = st; s->last = pos - speed; s->env = 1.f; s->xleft = 0; }
    else if (st != s->step) {
        if (speed >= 0 ? st < s->step : st > s->step) s->pass++;
        s->step = st;
    }
    int slice, rev, rep, mute; double where;
    sc_decide(tp, k, s->pass * n + st, st, n, beats, &slice, &where, &rev, &rep, &mute);
    double piece = SL / rep, f2 = fmod(f, piece);
    double off = rev ? piece - f2 : f2;
    double src = where >= 0 ? ls + fmod(where * L + off, (double)L) : ls + slice * SL + off;
    if (src >= ls + L) src -= L;
    if (src < ls) src = ls;
    /* the gate: the first Gate of each piece sounds, with 2 ms fades */
    double prog = (speed >= 0 ? f2 : piece - f2) / piece;
    float gate = clampf(tp[P_SC_GATE], 0.05f, 1.f);
    float target = !mute && (gate >= 0.999f || prog < gate) ? 1.f : 0.f;
    const float ramp = 1.f / (CD_SR * 0.002f);
    if (s->env < target) { s->env += ramp; if (s->env > target) s->env = target; }
    else if (s->env > target) { s->env -= ramp; if (s->env < target) s->env = target; }
    /* a jump in where it reads: the old place fades out under the new */
    double expect = s->last + speed;
    if (fabs(src - expect) > JUMP_TOL) { s->xpos = expect; s->xleft = CD_FADE; }
    loop_at(buf, ls, le, xf, src, l, r);
    if (s->xleft > 0) {
        float ol, orr, wo = (float)s->xleft / CD_FADE;
        raw_at(buf, len, s->xpos, &ol, &orr);
        *l = *l * (1 - wo) + ol * wo; *r = *r * (1 - wo) + orr * wo;
        s->xpos += speed; s->xleft--;
    }
    *l *= s->env; *r *= s->env;
    s->last = src; s->active = 1;
}

/* Count the capture frames that arrive against the output frames played.
 * The count since the anchor is exact but for the capture block it was cut
 * in, so a change is taken only when it is bigger than that error: a device
 * that agrees with the output (or drifts by a hair) stays at 1.0, one that is off by a whole rate
 * (44.1 vs 48 kHz) is corrected about a second after it starts. A stall that
 * throws the count off (two 1 s windows in a row that disagree with the
 * total) starts the count again from the last window. */
static void measure_input_clock(Engine *e, uint32_t w)
{
    uint64_t now = e->out_total;
    if (e->rm_state == 0) {
        if (now - e->rm_o0 >= CD_SR / 2) {
            e->rm_state = 1; e->rm_bad = 0;
            e->rm_w0 = e->rm_wl = w; e->rm_o0 = e->rm_ol = now;
        }
        return;
    }
    if (now - e->rm_ol < CD_SR) return;
    double win = (double)(uint32_t)(w - e->rm_wl) / (double)(now - e->rm_ol);
    double el = (double)(now - e->rm_o0);
    double cum = (double)(uint32_t)(w - e->rm_w0) / el;
    if (fabs(win - cum) > 0.03 * cum) {
        if (++e->rm_bad >= 2) { e->rm_w0 = e->rm_wl; e->rm_o0 = e->rm_ol; e->rm_bad = 0; }
    } else {
        e->rm_bad = 0;
        /* small drift (< 0.25%) is the FIFO follower's job */
        double tol = 2.0 * (e->in_block + 1024) / el;
        if (tol < 0.0025) tol = 0.0025;
        if (cum > 0.25 && cum < 4.0 && fabs(cum - e->in_ratio) > tol * cum) e->in_ratio = cum;
        /* ~10 minutes: start a fresh count before the frame counter wraps */
        if (el > 600.0 * CD_SR) { e->rm_w0 = e->rm_wl; e->rm_o0 = e->rm_ol; }
    }
    e->rm_wl = w; e->rm_ol = now;
}

/* lock the transport onto target (bars) as heard: a jump when far off,
 * else a hair faster or slower (at most 0.5%) */
static void transport_lock(Transport *tr, double target)
{
    if (!tr->locked || fabs(target - tr->phase) * tr->cyc > 0.03 * CD_SR) {
        tr->phase = target; tr->err_avg = 0; tr->locked = 1;
        double f = target - floor(target);
        if (f * tr->cyc < 0.25 * CD_SR) tr->bar_edge = 1;   /* (re)started on or just after the bar */
    } else {
        double err = (target - tr->phase) * tr->cyc;
        tr->err_avg += (err - tr->err_avg) * 0.1;
    }
    tr->rate = (1.0 + clampd(tr->err_avg * 2e-5, -0.005, 0.005)) / tr->cyc;
    tr->running = 1;
}

#define LINK_TRANSIT 0.0003 /* a bulk transfer through a hub and the follower's thread waking (s) */

/* follow the MIDI clock, a leading Brick or the first loop: the transport's phase and rate */
static void transport_follow(Engine *e, const float *p, double now)
{
    Transport *tr = &e->tr;
    const int N = CD_PPQ * 4;
    int ignore = p[P_CLOCK_START] > 0.5f;   /* the clock's Start / Stop / Continue pass by */
    uint32_t w = __atomic_load_n(&e->ev_w, __ATOMIC_ACQUIRE), r = e->ev_r;
    while (r != w) {
        ClockEv ev = e->evq[r & (CD_EVQ - 1)]; r++;
        switch (ev.msg) {
        case 0xFA: if (ignore) break; tr->ticks = -1; tr->tick_t = ev.t; tr->ext_run = 1; tr->seen_transport = 1; tr->locked = 0; break;
        case 0xFB: if (ignore) break; tr->ext_run = 1; tr->seen_transport = 1; tr->locked = 0; break;
        case 0xFC: if (ignore) break; tr->ext_run = 0; tr->seen_transport = 1; break;
        case 0xF0: case 0xF1: /* a leading Brick's position */
            tr->lk_t = ev.t; tr->lk_pos = ev.pos; tr->lk_bpm = ev.bpm; tr->lk_play = ev.msg == 0xF0;
            break;
        case 0xF8: {
            double last = tr->hist[(tr->hist_w + N - 1) % N];
            if (tr->hist_n && ev.t - last > 1.0) tr->hist_n = 0;   /* a gap: measure afresh */
            tr->hist[tr->hist_w] = ev.t; tr->hist_w = (tr->hist_w + 1) % N;
            if (tr->hist_n < N) tr->hist_n++;
            if (!tr->seen_transport) tr->ext_run = 1;   /* clock without Start: it's already playing */
            if (tr->ext_run) { tr->ticks++; tr->tick_t = ev.t; }
            break;
        }
        }
    }
    __atomic_store_n(&e->ev_r, r, __ATOMIC_RELEASE);

    double iv = 0, fit_newest = tr->tick_t;
    if (tr->hist_n >= 3) {
        /* a straight line through the ticks (up to four beats): its slope is the
         * tick interval and its end where the newest tick really was, so USB's
         * jitter on each tick averages out rather than nudging the transport */
        double newest = tr->hist[(tr->hist_w + N - 1) % N];
        double base = tr->hist[(tr->hist_w + N - tr->hist_n) % N];
        double n = tr->hist_n, sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int i = 0; i < tr->hist_n; i++) {
            double y = tr->hist[(tr->hist_w + N - tr->hist_n + i) % N] - base;
            sx += i, sy += y, sxx += (double)i * i, sxy += i * y;
        }
        iv = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        fit_newest = base + (sy - iv * sx) / n + iv * (n - 1);
        if (now - newest > 0.5 || iv <= 1e-4) iv = 0;   /* the clock stopped coming */
    }
    tr->bpm = iv > 0 ? 60.0 / (iv * CD_PPQ) : 0;
    int linked = tr->lk_t > 0 && now - tr->lk_t < 0.5 && tr->lk_bpm >= 20;
    if (linked)
        tr->bpm = tr->lk_bpm;
    int mode = (int)(p[P_CLOCK] + 0.5f);
    tr->following = mode == CLOCK_MIDI || (mode == CLOCK_AUTO && (linked || iv > 0));
    tr->linked = tr->following && linked;

    if (!tr->following) {
        tr->running = tr->cyc > 0;
        tr->rate = tr->cyc > 0 ? 1.0 / tr->cyc : 0;
        tr->locked = 0;
        return;
    }
    int beats = (int)p[P_BEATS]; if (beats < 1) beats = 1;
    /* when this block is heard */
    double T = now + e->lat_frames / (double)CD_SR + p[P_CLOCK_OFS] * 0.001;
    if (linked) {
        /* a leading Brick says where its sound is, at its exact tempo: the
         * transport goes where that sound will be when this block is heard */
        if (!tr->lk_play && !ignore) { tr->running = 0; tr->rate = 0; tr->locked = 0; return; }
        tr->cyc = 60.0 / tr->lk_bpm * beats * CD_SR;
        double lead = tr->lk_pos + (T - tr->lk_t + LINK_TRANSIT) * tr->lk_bpm / 15.0;
        double target = lead / (4.0 * beats);
        if (ignore && tr->locked) { /* started by hand: the bar lines up, the count stays ours */
            double d = target - tr->phase;
            target = tr->phase + (d - floor(d + 0.5));
        }
        transport_lock(tr, target);
        return;
    }
    double tpb = (double)CD_PPQ * beats;
    if (iv <= 0 || !tr->ext_run || tr->ticks < 0) { tr->running = 0; tr->rate = 0; tr->locked = 0; return; }
    tr->cyc = iv * CD_SR * tpb;
    /* where the clock is when this block is heard */
    /* the newest tick as the line has it (the raw one until the line spans a few) */
    double tick_t = tr->hist_n >= 8 && tr->ext_run && fabs(fit_newest - tr->tick_t) < iv ? fit_newest : tr->tick_t;
    double dt = clampd((T - tick_t) / iv, -2.0, 2.0);
    transport_lock(tr, (tr->ticks + dt) / tpb);
}

static void transport_update(Engine *e, const float *p)
{
    Transport *tr = &e->tr;
    double now = e->now ? e->now() : eng_clock();
    transport_follow(e, p, now);
    /* for the clock out: where this block will be heard */
    int beats = (int)p[P_BEATS]; if (beats < 1) beats = 1;
    __atomic_store_n(&tr->snap_seq, tr->snap_seq + 1, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    tr->snap_t = now + e->lat_frames / (double)CD_SR;
    tr->snap_phase = tr->phase;
    tr->snap_rate = tr->rate * CD_SR;
    tr->snap_beats = beats;
    tr->snap_run = tr->running && tr->cyc > 0;
    tr->snap_follow = tr->following;
    __atomic_store_n(&tr->snap_seq, tr->snap_seq + 1, __ATOMIC_RELEASE);
}

void eng_render(Engine *e, float *out, int frames)
{
    float p[P_COUNT];
    for (int k = 0; k < CD_TRACKS; k++) handle_cmds(e, &e->trk[k]);
    memcpy(p, e->p, sizeof p);
    transport_update(e, p);
    int beats = (int)p[P_BEATS]; if (beats < 1) beats = 1;
    float gain = db2lin(p[P_IN_GAIN]);
    float thr = db2lin(p[P_THRESH]);
    float inmax = 0, outmax = 0;
    const float fade = 1.f / CD_FADE;
    Transport *tr = &e->tr;

    /* Input FIFO. The capture and output devices run on their own clocks, so
     * the FIFO is kept at a target fill by reading it a hair faster or slower
     * (at most 0.3%, a few cents, never heard), instead of dropping or
     * inserting audio, which clicks. It starts (and restarts after an
     * underrun, with a bigger safety margin) once it holds the target. */
    uint32_t w = __atomic_load_n(&e->ring_w, __ATOMIC_ACQUIRE);
    uint32_t r = e->ring_r;
    if (e->in_restart) { e->in_restart = 0; e->rm_state = 0; e->rm_o0 = e->out_total; e->priming = 1; }
    measure_input_clock(e, w);
    int target = e->in_block + frames + e->margin;
    uint32_t fill = w - r;
    if (e->priming) {
        /* start at the target exactly: what's older is dropped (it was never played) */
        if (fill >= (uint32_t)target) { r = w - (uint32_t)target; fill = (uint32_t)target; e->priming = 0; e->ring_frac = 0; e->fill_avg = (float)target; }
    } else if (fill > (uint32_t)(4 * target + 4096)) {
        /* a long stall (device reopened, the app was paused): catch up once */
        r = w - (uint32_t)target; e->ring_frac = 0; e->fill_avg = (float)target; e->overruns++;
    }
    e->fill_avg += ((float)fill - e->fill_avg) * 0.02f;
    double ratio = e->in_ratio * (1.0 + clampf((e->fill_avg - target) * 0.00002f, -0.003f, 0.003f));

    float mix[BLK * 2], snd[BLK * 2], vb[BLK * 2], inb[BLK * 2];
    float recb[BLK * 2], ifx[BLK * 2], ivb[BLK * 2], inlev[BLK];
    TC tc[CD_TRACKS];
    /* the live input's sends: in Dry mode into the loops' FX and reverb
     * (heard only), when printed through the input's own (recorded too) */
    int print = p[P_IN_PRINT] > 0.5f;
    float in_fs_t = clampf(p[P_IN_FX_SEND], 0.f, 1.f), in_vs_t = clampf(p[P_IN_VERB_SEND], 0.f, 1.f);
    if (print != e->in_print) {
        /* nothing left over from the last time the input's own chain ran */
        if (print) { fx_reset(&e->fx_in, p); verb_reset(&e->verb_in); }
        e->in_print = print;
    }
    int done = 0;
    while (done < frames) {
        int n = frames - done > BLK ? BLK : frames - done;
        for (int k = 0; k < CD_TRACKS; k++) prep(e, &e->trk[k], &tc[k]);
        /* the input for this block, then its effects: what's recorded (recb)
         * and the live input's effects, heard (ifx, ivb) */
        float ifs[BLK];
        for (int i = 0; i < n; i++) {
            float il = 0, ir = 0;
            if (!e->priming) {
                if (w - r < 2) {
                    /* ran dry: the devices' timing is rougher than the margin allows */
                    e->priming = 1; e->underruns++;
                    if (e->margin < 4096) e->margin += 256;
                }
                else {
                    uint32_t k0 = (r & (CD_RING - 1)) * 2, k1 = ((r + 1) & (CD_RING - 1)) * 2;
                    float fr = (float)e->ring_frac;
                    il = (e->ring[k0] * (1 - fr) + e->ring[k1] * fr) * gain;
                    ir = (e->ring[k0 + 1] * (1 - fr) + e->ring[k1 + 1] * fr) * gain;
                    e->ring_frac += ratio;
                    while (e->ring_frac >= 1.0) { e->ring_frac -= 1.0; r++; }
                }
            }
            float m = fabsf(il) > fabsf(ir) ? fabsf(il) : fabsf(ir);
            if (m > inmax) inmax = m;
            inlev[i] = m;
            inb[2 * i] = il; inb[2 * i + 1] = ir;
            e->in_gs += (in_fs_t - e->in_gs) * 0.004f;
            e->in_gv += (in_vs_t - e->in_gv) * 0.004f;
            ifs[i] = e->in_gs;
            ifx[2 * i] = il * e->in_gs; ifx[2 * i + 1] = ir * e->in_gs;
            ivb[2 * i] = il * e->in_gv; ivb[2 * i + 1] = ir * e->in_gv;
        }
        if (print) {
            /* always run while printing, so the delay's and reverb's tails ring
             * out when a send is turned down */
            fx_block(&e->fx_in, p, tr->cyc, ifx, n);
            verb_process(&e->verb_in, p, ivb, n);
            /* the FX send splits the input as it does a track's */
            for (int i = 0; i < 2 * n; i++) recb[i] = inb[i] * (1 - ifs[i / 2]) + ifx[i] + ivb[i];
        } else memcpy(recb, inb, sizeof(float) * 2 * (size_t)n);
        for (int i = 0; i < n; i++) {
            float il = recb[2 * i], ir = recb[2 * i + 1], m = inlev[i];

            float ml = 0, mr = 0, sl = 0, sr = 0, vl = 0, vr = 0;
            if (!print) { sl = ifx[2 * i]; sr = ifx[2 * i + 1]; vl = ivb[2 * i]; vr = ivb[2 * i + 1]; }
            for (int k = 0; k < CD_TRACKS; k++) {
                Track *t = &e->trk[k];
                TC *c = &tc[k];

                if (t->rec_state == ST_ARMED) {
                    if (t->arm_sync) {
                        if (tr->running && tr->bar_edge) {
                            start_rec(e, t, floor(tr->phase + 1e-9), 1, 0);
                            /* the clock started a little before we could follow it:
                             * keep the take on the bar, with silence for what was missed */
                            int late = (int)((tr->phase - t->phase0) * tr->cyc);
                            if (late > 0 && late < t->cap / 2) {
                                memset(t->buf, 0, sizeof(float) * 2 * (size_t)late);
                                t->rec_pos = t->len = late;
                            }
                            prep(e, t, c);
                        }
                    } else if (m >= thr) {
                        int first = t->p[P_SYNC] < 0.5f && !follows_now(e) && !other_synced(e, t);
                        start_rec(e, t, tr->phase, 0, first); prep(e, t, c);
                    }
                }
                if (t->rec_state == ST_REC) {
                    if (t->rec_sync && !tr->running) { finish_rec(e, t); prep(e, t, c); }
                    else {
                        if (t->rec_sync && t->stop_cyc > 0 && !t->stop_len && tr->phase - t->phase0 >= t->stop_cyc)
                            t->stop_len = t->rec_pos > 0 ? t->rec_pos : 1;
                        if (!e->busy) {
                            t->buf[2 * t->rec_pos] = il; t->buf[2 * t->rec_pos + 1] = ir;
                            t->rec_pos++; t->len = t->rec_pos;
                        }
                        if ((t->stop_len && t->rec_pos >= t->stop_len + CD_XFADE) || t->rec_pos >= t->cap) {
                            finish_rec(e, t); prep(e, t, c);
                        }
                    }
                }

                /* play / stop fade */
                if (t->playing && t->rec_state != ST_REC && c->gate) { if ((t->play_g += fade) > 1.f) t->play_g = 1.f; }
                else if ((t->play_g -= fade) < 0.f) t->play_g = 0.f;
                if (t->clear_pending && t->play_g <= 0.f) { do_clear(t); prep(e, t, c); }
                t->gl += (c->gl - t->gl) * 0.004f;
                t->gr += (c->gr - t->gr) * 0.004f;
                t->gs += (c->send - t->gs) * 0.004f;
                t->gv += (c->vsend - t->gv) * 0.004f;

                if (t->play_g <= 0.f || c->L <= 0 || t->rec_state == ST_REC) { t->writing = 0; t->sc.active = 0; continue; }
                int ls = c->ls, le = c->le, xf = c->xf, end = c->end, L = c->L;

                /* where it plays this frame */
                if (c->lock) {
                    double tg = lock_target(e, t, c);
                    double d = tg - wrap(t->pos + c->speed, ls, L);
                    if (d > L / 2.0) d -= L; else if (d < -L / 2.0) d += L;
                    if (!t->snap && fabs(d) > JUMP_TOL) start_jump(t, t->pos + c->speed);
                    t->pos = tg;
                } else if (t->pos < ls || t->pos >= end) {
                    if (!t->snap) start_jump(t, t->pos);
                    t->pos = wrap(t->pos, ls, L);
                }
                t->snap = 0;

                int writing = (t->overdub || c->decay_always) && !e->busy;
                if (writing && !t->writing) {
                    /* overdub lands where the player heard the loop: behind
                     * the playhead by the output + input latency (the
                     * devices' and the FIFO's), plus the user's trim (Dub
                     * align). Fixed for the whole pass: following the FIFO's
                     * fill as it wobbles would move the write head against
                     * the playhead and bend the pitch of what is written. */
                    double cm = (double)e->lat_frames + (double)target / e->in_ratio
                                + p[P_DUB_ALIGN] * 0.001 * CD_SR;
                    t->od_comp = cm < 0 ? 0 : cm;
                    t->last_od = -1;
                }
                t->writing = writing;
                if (writing) {
                    float wl = t->overdub ? il : 0.f, wr = t->overdub ? ir : 0.f;
                    double pp = wrap(t->pos - t->od_comp * c->speed, ls, L);
                    int idx = (int)pp; if (idx >= end) idx = end - 1;
                    int rev = c->speed < 0;
                    if (t->last_od < ls || t->last_od >= end) {
                        od_write_loop(t->buf, idx, ls, le, xf, wl, wr, c->keep);
                    } else {
                        /* write every index the write head crossed since the
                         * last frame (fills gaps when rate > 1, skips
                         * repeats when rate < 1) */
                        int cur = t->last_od, guard = 0;
                        while (cur != idx && guard++ < 64) {
                            cur += rev ? -1 : 1;
                            if (cur >= end) cur = ls;
                            if (cur < ls) cur = end - 1;
                            od_write_loop(t->buf, cur, ls, le, xf, wl, wr, c->keep);
                        }
                    }
                    t->last_od = idx;
                }

                float dl, dr;
                if (c->scat) {
                    /* scatter reads the slices the pattern puts under the playhead */
                    sc_read(&t->sc, t->p, k, t->buf, t->len, ls, le, xf, t->pos, c->speed, c->scbar, beats, &dl, &dr);
                    t->xleft = 0;
                } else {
                    /* back from scatter: fade from where it was reading */
                    if (t->sc.active) { t->sc.active = 0; start_jump(t, t->sc.last + c->speed); }
                    loop_at(t->buf, ls, le, xf, t->pos, &dl, &dr);
                    if (t->xleft > 0) {
                        /* a jump: the old playhead fades out under the new one */
                        float ol, orr, wo = (float)t->xleft / CD_FADE;
                        raw_at(t->buf, t->len, t->xpos, &ol, &orr);
                        dl = dl * (1 - wo) + ol * wo; dr = dr * (1 - wo) + orr * wo;
                        t->xpos += c->speed; t->xleft--;
                    }
                }
                if (!c->lock) t->pos = wrap(t->pos + c->speed, ls, L);
                /* the FX send splits the track: that much goes through the
                 * master FX, the rest past it (dry) */
                float ol = warm_clip(dl * t->play_g * t->gl, &t->dc[0]);
                float orr = warm_clip(dr * t->play_g * t->gr, &t->dc[1]);
                sl += ol * t->gs; sr += orr * t->gs;
                vl += ol * t->gv; vr += orr * t->gv;   /* the reverb: a send of its own */
                ml += ol * (1 - t->gs); mr += orr * (1 - t->gs);
            }
            mix[2 * i] = ml; mix[2 * i + 1] = mr;
            snd[2 * i] = sl; snd[2 * i + 1] = sr;
            vb[2 * i] = vl; vb[2 * i + 1] = vr;

            if (tr->running) {
                double ph0 = tr->phase;
                tr->phase += tr->rate;
                tr->bar_edge = floor(tr->phase) > floor(ph0);
            } else tr->bar_edge = 0;
        }
        fx_block(&e->fx, p, tr->cyc, snd, n);
        verb_process(&e->verb, p, vb, n);
        for (int i = 0; i < 2 * n; i++) mix[i] += snd[i] + vb[i];
        if (print) for (int i = 0; i < 2 * n; i++) mix[i] += ifx[i] + ivb[i];
        /* the input is heard through Conduit only as far as Input monitor
         * says, overdubbing or not: an interface that monitors its own input
         * would otherwise be heard twice, the second copy late. Its
         * effects are heard whatever the monitor (what's sent to the FX
         * is taken off the dry, as on a track) */
        float mon_t = p[P_MONITOR];
        float master = p[P_MASTER];
        for (int i = 0; i < n; i++) {
            e->mon_g += (mon_t - e->mon_g) * fade;
            for (int c = 0; c < 2; c++) {
                float o = mix[2 * i + c] * master + inb[2 * i + c] * (1 - ifs[i]) * e->mon_g;
                o = sat(o);   /* loops + monitored input round off instead of clipping */
                float a = fabsf(o); if (a > outmax) outmax = a;
                out[2 * (done + i) + c] = o;
            }
        }
        done += n;
    }
    __atomic_store_n(&e->ring_r, r, __ATOMIC_RELEASE);
    e->out_total += (uint64_t)frames;
    for (int k = 0; k < CD_TRACKS; k++) {
        Track *t = &e->trk[k];
        t->pos_i = (int)t->pos;
        t->sc_pos_i = t->sc.active ? (int)t->sc.last : -1;
        if (t->writing || t->rec_state == ST_REC) t->version++;
    }
    float dec = powf(0.9995f, (float)frames);
    e->in_peak = inmax > e->in_peak * dec ? inmax : e->in_peak * dec;
    e->out_peak = outmax > e->out_peak * dec ? outmax : e->out_peak * dec;
}

/* ---------------------------------------------------------- UI side */

void eng_rec_button(Engine *e, int t) { __atomic_fetch_or(&e->trk[t].cmd, CMD_REC, __ATOMIC_ACQ_REL); }
void eng_play_toggle(Engine *e, int t) { __atomic_fetch_or(&e->trk[t].cmd, CMD_PLAY, __ATOMIC_ACQ_REL); }

/* ---- undo stack (UI thread) */

static void undo_drop_oldest(Track *t)
{
    if (!t->undo_n) return;
    if (t->undo[0].data) t->undo_bytes -= (size_t)t->undo[0].len * 2 * sizeof(float);
    free(t->undo[0].data);
    memmove(&t->undo[0], &t->undo[1], sizeof(UndoLevel) * (size_t)(t->undo_n - 1));
    t->undo_n--;
}

static void undo_reset(Track *t)
{
    while (t->undo_n) undo_drop_oldest(t);
    t->undo_stale = 0;
}

/* a new take since: the old steps no longer apply */
static void undo_check(Track *t) { if (t->undo_stale) undo_reset(t); }

static void undo_push(Track *t, int kind)
{
    undo_check(t);
    int len = t->len;
    if (len <= 0) return;
    size_t bytes = kind == UNDO_DUB ? (size_t)len * 2 * sizeof(float) : 0;
    if (bytes > CD_UNDO_BYTES) return;
    while (t->undo_n && (t->undo_n == CD_UNDO_LEVELS || t->undo_bytes + bytes > CD_UNDO_BYTES))
        undo_drop_oldest(t);
    UndoLevel *u = &t->undo[t->undo_n];
    memset(u, 0, sizeof *u);
    u->kind = kind; u->len = len;
    u->loop_start = t->p[P_LOOP_START]; u->loop_end = t->p[P_LOOP_END];
    if (bytes) {
        /* the audio thread only reads the buffer until the overdub starts */
        if (!(u->data = malloc(bytes))) return;
        memcpy(u->data, t->buf, bytes);
        t->undo_bytes += bytes;
    }
    t->undo_n++;
}

void eng_overdub_toggle(Engine *e, int ti)
{
    Track *t = &e->trk[ti];
    if (!t->overdub && t->len > 0 && t->rec_state == ST_IDLE && !e->busy && !t->clear_pending)
        undo_push(t, UNDO_DUB);
    __atomic_fetch_or(&t->cmd, CMD_ODUB, __ATOMIC_ACQ_REL);
}

void eng_erase(Engine *e, int ti)
{
    Track *t = &e->trk[ti];
    if (t->len > 0 && t->rec_state == ST_IDLE && !t->clear_pending) undo_push(t, UNDO_ERASE);
    __atomic_fetch_or(&t->cmd, CMD_CLEAR, __ATOMIC_ACQ_REL);
}

void eng_clear(Engine *e, int ti) { do_clear(&e->trk[ti]); undo_reset(&e->trk[ti]); }

int eng_undo_count(Engine *e, int ti) { undo_check(&e->trk[ti]); return e->trk[ti].undo_n; }

void eng_ui_tick(Engine *e)
{
    for (int k = 0; k < CD_TRACKS; k++) {
        Track *t = &e->trk[k];
        int want = t->p[P_DECAY_MODE] > 0.5f && t->p[P_DECAY] > 0.f && t->playing && t->len > 0
                   && t->rec_state == ST_IDLE && !t->clear_pending;
        if (want && !t->decay_armed) {
            /* the loop as it was before it starts to fade: undo gets it back */
            undo_push(t, UNDO_DUB);
            t->decay_armed = 1;
        } else if (!want && !t->overdub) t->decay_armed = 0;
    }
}

int eng_undo(Engine *e, int ti)
{
    Track *t = &e->trk[ti];
    undo_check(t);
    if (!t->undo_n || t->rec_state == ST_REC) return 0;
    UndoLevel *u = &t->undo[t->undo_n - 1];
    int kind = u->kind;
    t->overdub = 0;
    if (kind == UNDO_DUB) {
        if (t->len != u->len) { undo_reset(t); return 0; }
        memcpy(t->buf, u->data, (size_t)u->len * 2 * sizeof(float));
        t->undo_bytes -= (size_t)u->len * 2 * sizeof(float);
        free(u->data);
    } else {
        /* the samples are still in the buffer: give the take back, stopped */
        t->clear_pending = 0; t->playing = 0; t->rec_state = ST_IDLE;
        t->len = t->rec_pos = u->len;
        t->p[P_LOOP_START] = u->loop_start; t->p[P_LOOP_END] = u->loop_end;
        int ls, le; eng_loop_range(e, ti, &ls, &le);
        t->pos = ls; t->snap = 1;
    }
    t->undo_n--;
    t->version++;
    return kind;
}

void eng_set_buffer(Engine *e, int ti, const float *st, int frames)
{
    Track *t = &e->trk[ti];
    if (frames > t->cap) frames = t->cap;
    t->playing = 0; t->overdub = 0; t->rec_state = ST_IDLE;
    undo_reset(t);
    memcpy(t->buf, st, (size_t)frames * 2 * sizeof(float));
    t->len = frames; t->rec_pos = frames; t->pos = 0; t->play_g = 0; t->clear_pending = 0; t->decay_armed = 0;
    t->xleft = 0; t->snap = 1;
    t->p[P_LOOP_START] = 0.f; t->p[P_LOOP_END] = 1.f;
    t->synced = 0;
    if (t->p[P_SYNC] < 0.5f && frames > MIN_TAKE) {
        int ls, le; eng_loop_range(e, ti, &ls, &le);
        double play = le - ls, cyc = e->tr.cyc;
        double whole = cyc > 0 ? snap_near(frames / cyc) : 0;
        if (cyc > 0 && fabs(frames / cyc - whole) < 0.002 * whole && frames + CD_XFADE <= t->cap) {
            /* whole bars already (a mix or track Conduit saved, from bar 1):
             * it loops as it is, so its seam partner is its own start */
            memcpy(t->buf + 2 * (size_t)frames, t->buf, sizeof(float) * 2 * CD_XFADE);
            t->len = t->rec_pos = frames + CD_XFADE;
            t->cycles = whole; t->cyc_rec = frames / whole; t->phase0 = 0; t->synced = 1;
        } else if (!follows_now(e) && !other_synced(e, t)) {
            /* the only synced track: it sets the bar */
            e->tr.cyc = play; e->tr.phase = 0; e->tr.rate = 1.0 / play; e->tr.running = 1;
            t->phase0 = 0; t->cyc_rec = play; t->cycles = 1; t->synced = 1;
        } else if (cyc > 0) {
            /* fit it to the nearest whole length (a slight varispeed) */
            double c = snap_near(play / cyc);
            t->cycles = c; t->cyc_rec = play / c; t->phase0 = 0; t->synced = 1;
        }
    }
    t->version++;
}

/* ------------------------------------------------------------ mixdown */

static long gcdl(long a, long b) { while (b) { long t = a % b; a = b; b = t; } return a; }

int eng_mix_begin(Engine *e, Mixdown *m, int max_frames)
{
    memset(m, 0, sizeof *m);
    memcpy(m->p, e->p, sizeof m->p);
    double cyc = e->tr.cyc;
    m->cyc = cyc;
    long q = 0;                /* quarter bars all synced tracks repeat in */
    double longest = 0;
    for (int k = 0; k < CD_TRACKS; k++) {
        Track *t = &e->trk[k];
        memcpy(m->tp[k], t->p, sizeof m->tp[k]);
        if (!eng_track_has(e, k)) continue;
        const float *tp = m->tp[k];
        loop_region(t->len, tp, &m->ls[k], &m->le[k], &m->xf[k]);
        int end = m->le[k] - m->xf[k], L = end - m->ls[k];
        if (L < 1) continue;
        m->on[k] = 1;
        double user = eng_rate(tp, 0);
        int rev = tp[P_REVERSE] > 0.5f;
        int sy = lock_mode(t) && cyc > 0;
        double sp = sy ? user * t->cyc_rec / cyc : user;
        m->speed[k] = rev ? -sp : sp;
        if (sy) {
            /* from bar 1, where it would play there */
            double el = (0 - t->phase0) * t->cyc_rec;
            double d = fmod(el - m->ls[k], (double)L); if (d < 0) d += L;
            m->pos[k] = rev ? end - 1 - d : m->ls[k] + d;
            double bars = L / (t->cyc_rec * user);
            long qq = lround(bars * 4);
            if (qq >= 1 && fabs(qq / 4.0 - bars) < 0.01) q = q ? q / gcdl(q, qq) * qq : qq;
            if (tp[P_SC_MODE] >= 0.5f) {   /* the whole scatter pattern */
                long pq = 4L << clampi((int)tp[P_SC_BARS], 0, 3);
                q = q ? q / gcdl(q, pq) * pq : pq;
            }
            else if (bars * cyc > longest) longest = bars * cyc;
            if (q > 4 * 256) q = 4 * 256;
        } else {
            m->pos[k] = rev ? end - 1 : m->ls[k];
            if (L / user > longest) longest = L / user;
        }
        float vol = tp[P_VOLUME] * clampf(t->xy, 0.f, 1.f);   /* as heard, X/Y mix included */
        double a = (clampf(tp[P_PAN], -1.f, 1.f) + 1.0) * M_PI / 4;
        m->gl[k] = vol * (float)fmin(1.0, M_SQRT2 * cos(a));
        m->gr[k] = vol * (float)fmin(1.0, M_SQRT2 * sin(a));
        m->mute[k] = tp[P_MUTE] > 0.5f;
        m->send[k] = clampf(tp[P_FX_SEND], 0.f, 1.f);
        m->vsend[k] = clampf(tp[P_VERB_SEND], 0.f, 1.f);
    }
    double frames = q ? q / 4.0 * cyc : longest;
    if (frames > max_frames) frames = max_frames;
    if (frames < 1 || fx_init(&m->fx)) { m->frames = 0; return 0; }
    if (verb_init(&m->verb)) { fx_free(&m->fx); m->frames = 0; return 0; }
    fx_reset(&m->fx, m->p);
    m->frames = (int)(frames + 0.5);
    /* one pass first, thrown away, so the delay's tail wraps round seamlessly */
    double save[CD_TRACKS]; memcpy(save, m->pos, sizeof save);
    float blk[BLK * 2];
    for (int done = 0; done < m->frames; done += BLK) {
        int n = m->frames - done < BLK ? m->frames - done : BLK;
        eng_mix_render(e, m, blk, NULL, n);
    }
    memcpy(m->pos, save, sizeof save);
    m->done = 0;
    return m->frames;
}

void eng_mix_render(Engine *e, Mixdown *m, float *mix, float *stems[CD_TRACKS], int n)
{
    float snd[BLK * 2], vb[BLK * 2];
    for (int done = 0; done < n; done += BLK) {
        int b = n - done < BLK ? n - done : BLK;
        float *mx = mix + 2 * done;
        for (int i = 0; i < 2 * b; i++) { mx[i] = 0; snd[i] = 0; vb[i] = 0; }
        for (int k = 0; k < CD_TRACKS; k++) {
            float *sk = stems && stems[k] ? stems[k] + 2 * done : NULL;
            if (sk) memset(sk, 0, sizeof(float) * 2 * b);
            if (!m->on[k]) continue;
            const Track *t = &e->trk[k];
            int ls = m->ls[k], le = m->le[k], xf = m->xf[k], L = le - xf - ls;
            double pp = m->pos[k];
            float sd = m->send[k], vs = m->vsend[k];
            int scat = m->tp[k][P_SC_MODE] >= 0.5f;
            double scbar = t->synced && t->cyc_rec > 0 ? t->cyc_rec : 0;
            int beats = (int)m->p[P_BEATS];
            for (int i = 0; i < b; i++) {
                float l, r;
                pp = wrap(pp, ls, L);
                if (scat) sc_read(&m->sc[k], m->tp[k], k, t->buf, t->len, ls, le, xf, pp, m->speed[k], scbar, beats, &l, &r);
                else loop_at(t->buf, ls, le, xf, pp, &l, &r);
                pp += m->speed[k];
                l = warm_clip(l * m->gl[k], &m->dc[k][0]); r = warm_clip(r * m->gr[k], &m->dc[k][1]);
                if (sk) { sk[2 * i] = l; sk[2 * i + 1] = r; }
                if (!m->mute[k]) {
                    mx[2 * i] += l * (1 - sd); mx[2 * i + 1] += r * (1 - sd);
                    snd[2 * i] += l * sd; snd[2 * i + 1] += r * sd;
                    vb[2 * i] += l * vs; vb[2 * i + 1] += r * vs;
                }
            }
            m->pos[k] = wrap(pp, ls, L);
        }
        fx_block(&m->fx, m->p, m->cyc, snd, b);
        verb_process(&m->verb, m->p, vb, b);
        for (int i = 0; i < 2 * b; i++) mx[i] = clampf((mx[i] + snd[i] + vb[i]) * m->p[P_MASTER], -1.f, 1.f);
    }
    m->done += n;
}

void eng_mix_end(Mixdown *m) { fx_free(&m->fx); verb_free(&m->verb); }
