#include "dsp.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CMD_REC  1
#define CMD_PLAY 2
#define CMD_ODUB 4
#define CMD_CLEAR 8
#define BLK 128
#define MIN_TAKE (CD_SR / 50)  /* 20 ms */

static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline float db2lin(float db) { return powf(10.f, db / 20.f); }

void eng_default_params(float *p)
{
    memset(p, 0, sizeof(float) * P_COUNT);
    p[P_LOOP_START] = 0.f;  p[P_LOOP_END] = 1.f;
    p[P_PITCH] = 0.f;       p[P_FINE] = 0.f;  p[P_REVERSE] = 0.f;
    p[P_DECAY] = 0.02f;    p[P_DECAY_MODE] = 0.f;
      p[P_VOLUME] = 0.9f; p[P_MONITOR] = 0.f;
    eng_reset_fx_params(p);
    p[P_IN_GAIN] = 0.f;     p[P_TRIGGER] = 0.f; p[P_THRESH] = -36.f;
    p[P_AUTOPLAY] = 1.f;    p[P_SAVE_FMT] = 0.f; p[P_DUB_ALIGN] = 0.f;
}

void eng_reset_fx_params(float *p)
{
    p[P_FILT_MODE] = 0.f; p[P_CUTOFF] = 1.f; p[P_RESO] = 0.2f;
    p[P_DRIVE] = 0.f;     p[P_BITS] = 16.f;  p[P_DOWNS] = 1.f;
    p[P_DLY_SYNC] = 0.f;  p[P_DLY_TIME] = 350.f; p[P_DLY_FB] = 0.4f;
    p[P_DLY_MIX] = 0.f;   p[P_PINGPONG] = 0.f;
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

static float dly_target(const float *p, double loop_period)
{
    int s = (int)p[P_DLY_SYNC];
    float t;
    if (s > 0 && s < 7 && loop_period > 0) t = (float)(loop_period * sync_div[s]);
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

void fx_block(FxState *f, const float *p, double loop_period, float *st, int n)
{
    int mode = (int)p[P_FILT_MODE];
    float drive = p[P_DRIVE];
    int bits = (int)p[P_BITS];
    int downs = (int)p[P_DOWNS];
    float fb = clampf(p[P_DLY_FB], 0.f, 0.95f);
    float mix = p[P_DLY_MIX];
    int ping = p[P_PINGPONG] > 0.5f;
    float tgt = dly_target(p, loop_period);

    /* smooth cutoff once per block */
    f->cutoff_s += (p[P_CUTOFF] - f->cutoff_s) * 0.25f;
    float fc = 20.f * powf(1000.f, clampf(f->cutoff_s, 0.f, 1.f));
    fc = clampf(fc, 20.f, 0.45f * CD_SR);
    float g = tanf((float)M_PI * fc / CD_SR);
    float k = 2.f - 2.f * clampf(p[P_RESO], 0.f, 0.97f);
    float a1 = 1.f / (1.f + g * (g + k)), a2 = g * a1, a3 = g * a2;

    float dg = 1.f + drive * 15.f, dnorm = 1.f / tanhf(dg);
    float q = (float)(1 << (bits > 1 ? bits - 1 : 1));

    for (int i = 0; i < n; i++) {
        float x[2] = { st[2 * i], st[2 * i + 1] };
        if (drive > 0.001f) { x[0] = tanhf(x[0] * dg) * dnorm; x[1] = tanhf(x[1] * dg) * dnorm; }
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
        float rp = (float)f->dly_w - f->dly_time_s;
        while (rp < 0) rp += CD_DLY_MAX;
        int r0 = (int)rp;
        if (r0 >= CD_DLY_MAX) { r0 = 0; rp = 0; }  /* float rounding at the wrap */ float fr = rp - r0; int r1 = r0 + 1 == CD_DLY_MAX ? 0 : r0 + 1;
        float dl = f->dly[2 * r0] * (1 - fr) + f->dly[2 * r1] * fr;
        float dr = f->dly[2 * r0 + 1] * (1 - fr) + f->dly[2 * r1 + 1] * fr;
        f->damp[0] += 0.4f * (dl - f->damp[0]);
        f->damp[1] += 0.4f * (dr - f->damp[1]);
        float wl, wr;
        if (ping) { wl = 0.5f * (x[0] + x[1]) + fb * f->damp[1]; wr = fb * f->damp[0]; }
        else { wl = x[0] + fb * f->damp[0]; wr = x[1] + fb * f->damp[1]; }
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
    e->cap = max_seconds * CD_SR;
    e->buf = calloc((size_t)e->cap * 2, sizeof(float));
    if (!e->buf || fx_init(&e->fx)) { eng_free(e); return -1; }
    eng_default_params(e->p);
    fx_reset(&e->fx, e->p);
    e->in_block = 512;
    e->lat_frames = 3 * 512;
    e->priming = 1;
    e->margin = 64;
    e->in_ratio = 1.0;
    return 0;
}

void eng_free(Engine *e)
{
    free(e->buf); e->buf = NULL;
    for (int i = 0; i < e->undo_n; i++) free(e->undo[i].data);
    e->undo_n = 0; e->undo_bytes = 0;
    fx_free(&e->fx);
}

double eng_rate(const float *p)
{
    return pow(2.0, (p[P_PITCH] + p[P_FINE] / 100.0) / 12.0);
}

/* The loop region [ls, le) as set by the user, and the crossfade length xf.
 * What plays is [ls, le - xf): its first xf frames are blended with the last
 * xf frames of the region, so the seam where it wraps doesn't click. */
static void loop_region(int len, const float *p, int *ls, int *le, int *xf)
{
    float a = clampf(p[P_LOOP_START], 0.f, 1.f), b = clampf(p[P_LOOP_END], 0.f, 1.f);
    if (b < a) { float t = a; a = b; b = t; }
    int s = (int)(a * len), en = (int)(b * len);
    if (en > len) en = len;
    if (en - s < MIN_TAKE / 4) { en = s + MIN_TAKE / 4; if (en > len) { en = len; s = en - MIN_TAKE / 4; if (s < 0) s = 0; } }
    int f = CD_XFADE;
    if (f > (en - s) / 4) f = (en - s) / 4;
    *ls = s; *le = en; *xf = f;
}

void eng_loop_range(const Engine *e, int *ls, int *le)
{
    int xf; loop_region(e->len, e->p, ls, le, &xf); *le -= xf;
}

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

static void do_clear(Engine *e)
{
    e->playing = 0; e->overdub = 0; e->rec_state = ST_IDLE;
    e->len = 0; e->rec_pos = 0; e->pos = 0;
    e->play_g = 0; e->clear_pending = 0; e->decay_armed = 0;
    e->version++;
}

static void start_rec(Engine *e)
{
    e->playing = 0; e->overdub = 0; e->clear_pending = 0; e->undo_stale = 1;
    e->rec_pos = 0; e->len = 0; e->rec_state = ST_REC; e->play_g = 0; e->decay_armed = 0;
    e->version++;
}

static void finish_rec(Engine *e)
{
    e->rec_state = ST_IDLE;
    e->len = e->rec_pos >= MIN_TAKE ? e->rec_pos : 0;
    e->p[P_LOOP_START] = 0.f; e->p[P_LOOP_END] = 1.f;
    int ls, le; eng_loop_range(e, &ls, &le);
    e->pos = e->p[P_REVERSE] > 0.5f && e->len ? le - 1 : 0;
    e->playing = e->len > 0 && e->p[P_AUTOPLAY] > 0.5f;
    e->version++;
}

static void handle_cmds(Engine *e)
{
    int c = __atomic_exchange_n(&e->cmd, 0, __ATOMIC_ACQ_REL);
    if (!c) return;
    if (c & CMD_CLEAR) {
        /* fade out first when something is playing; render() clears at the end of the fade */
        if (e->rec_state == ST_REC || e->play_g <= 0.f) do_clear(e);
        else { e->playing = 0; e->overdub = 0; e->rec_state = ST_IDLE; e->clear_pending = 1; }
        return;
    }
    if (c & CMD_REC) {
        if (e->rec_state == ST_REC) finish_rec(e);
        else if (e->rec_state == ST_ARMED) e->rec_state = ST_IDLE;
        else if (!e->busy) {
            if (e->p[P_TRIGGER] > 0.5f) { e->rec_state = ST_ARMED; e->playing = 0; e->overdub = 0; }
            else start_rec(e);
        }
    }
    if (e->clear_pending) return;
    if ((c & CMD_PLAY) && e->rec_state != ST_REC && e->len > 0) {
        e->playing = !e->playing;
        if (!e->playing) e->overdub = 0;
        else if (e->play_g <= 0.f) { int ls, le; eng_loop_range(e, &ls, &le); e->pos = e->p[P_REVERSE] > 0.5f ? le - 1 : ls; }
    }
    if ((c & CMD_ODUB) && e->rec_state != ST_REC && e->len > 0 && !e->busy) {
        if (e->overdub) e->overdub = 0;
        else {
            e->overdub = 1; e->last_od = -1;
            if (!e->playing) {
                if (e->play_g <= 0.f) {
                    int ls, le; eng_loop_range(e, &ls, &le);
                    e->pos = e->p[P_REVERSE] > 0.5f ? le - 1 : ls;
                }
                e->playing = 1;
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

static inline void od_write(Engine *e, int idx, float il, float ir, float keep)
{
    e->buf[2 * idx] = sat(e->buf[2 * idx] * keep + il);
    e->buf[2 * idx + 1] = sat(e->buf[2 * idx + 1] * keep + ir);
}

/* overdub one loop frame; the crossfaded head also writes its tail partner,
 * so what plays there is keep * old + input exactly */
static inline void od_write_loop(Engine *e, int idx, int ls, int le, int xf, float il, float ir, float keep)
{
    od_write(e, idx, il, ir, keep);
    if (idx - ls < xf) od_write(e, le - xf + (idx - ls), il, ir, keep);
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

/* read the loop with linear interpolation; advance the playhead */
static inline void loop_read(const float *buf, double *pos, int ls, int le, int xf, double step,
                             float *l, float *r)
{
    int end = le - xf, L = end - ls;
    double p = *pos;
    if (p < ls || p >= end) p = step < 0 ? end - 1 : ls;
    int i0 = (int)p; float fr = (float)(p - i0);
    int i1 = i0 + 1 >= end ? ls : i0 + 1;
    float l0, r0, l1, r1;
    loop_val(buf, ls, le, xf, i0, &l0, &r0);
    loop_val(buf, ls, le, xf, i1, &l1, &r1);
    *l = l0 * (1 - fr) + l1 * fr;
    *r = r0 * (1 - fr) + r1 * fr;
    p += step;
    if (p >= end) p -= L;
    if (p < ls) p += L;
    *pos = p;
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

void eng_render(Engine *e, float *out, int frames)
{
    float p[P_COUNT];
    handle_cmds(e);
    memcpy(p, e->p, sizeof p);
    float gain = db2lin(p[P_IN_GAIN]);
    float thr = db2lin(p[P_THRESH]);
    /* loop decay: each pass of the write head scales the loop by keep, so
     * layers fade away over time and stacked overdubs can't keep growing */
    float keep = 1.f - clampf(p[P_DECAY], 0.f, 1.f), vol = p[P_VOLUME];
    int decay_always = p[P_DECAY_MODE] > 0.5f && keep < 1.f && e->decay_armed;
    double rate = eng_rate(p);
    int rev = p[P_REVERSE] > 0.5f;
    float inmax = 0, outmax = 0;
    const float fade = 1.f / CD_FADE;

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

    float dry[BLK * 2], inb[BLK * 2];
    int done = 0;
    while (done < frames) {
        int n = frames - done > BLK ? BLK : frames - done;
        int len = e->len, ls = 0, le = 0, xf = 0;
        if (len > 1) loop_region(len, p, &ls, &le, &xf);
        int end = le - xf, L = end - ls;
        double period = L > 0 ? L / rate : 0;
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
            inb[2 * i] = il; inb[2 * i + 1] = ir;

            if (e->rec_state == ST_ARMED && m >= thr) start_rec(e);
            if (e->rec_state == ST_REC && !e->busy) {
                e->buf[2 * e->rec_pos] = il; e->buf[2 * e->rec_pos + 1] = ir;
                e->rec_pos++; e->len = e->rec_pos;
                if (e->rec_pos >= e->cap) finish_rec(e);
            }

            /* play / stop fade */
            if (e->playing && e->rec_state != ST_REC) { if ((e->play_g += fade) > 1.f) e->play_g = 1.f; }
            else if ((e->play_g -= fade) < 0.f) e->play_g = 0.f;
            if (e->clear_pending && e->play_g <= 0.f) { do_clear(e); len = 0; L = 0; }

            float dl = 0, dr = 0;
            if (e->play_g > 0.f && len > 1 && L > 0 && e->rec_state != ST_REC) {
                int writing = (e->overdub || decay_always) && !e->busy;
                if (writing && !e->writing) {
                    /* overdub lands where the player heard the loop: behind
                     * the playhead by the output + input latency (the
                     * devices' and the FIFO's), plus the user's trim (Dub
                     * align). Fixed for the whole pass: following the FIFO's
                     * fill as it wobbles would move the write head against
                     * the playhead and bend the pitch of what is written. */
                    double c = (double)e->lat_frames + (double)target / e->in_ratio
                               + p[P_DUB_ALIGN] * 0.001 * CD_SR;
                    e->od_comp = c < 0 ? 0 : c;
                    e->last_od = -1;
                }
                e->writing = writing;
                if (writing) {
                    float wl = e->overdub ? il : 0.f, wr = e->overdub ? ir : 0.f;
                    double pp = e->pos;
                    if (pp < ls || pp >= end) pp = rev ? end - 1 : ls;
                    pp += (rev ? e->od_comp : -e->od_comp) * rate;
                    pp = fmod(pp - ls, (double)L); if (pp < 0) pp += L;
                    int idx = ls + (int)pp; if (idx >= end) idx = end - 1;
                    if (e->last_od < ls || e->last_od >= end) {
                        od_write_loop(e, idx, ls, le, xf, wl, wr, keep);
                    } else {
                        /* write every index the write head crossed since the
                         * last frame (fills gaps when rate > 1, skips
                         * repeats when rate < 1) */
                        int cur = e->last_od, guard = 0;
                        while (cur != idx && guard++ < 64) {
                            cur += rev ? -1 : 1;
                            if (cur >= end) cur = ls;
                            if (cur < ls) cur = end - 1;
                            od_write_loop(e, cur, ls, le, xf, wl, wr, keep);
                        }
                    }
                    e->last_od = idx;
                }
                double pos = e->pos;
                loop_read(e->buf, &pos, ls, le, xf, rev ? -rate : rate, &dl, &dr);
                e->pos = pos;
                dl *= e->play_g; dr *= e->play_g;
            }
            dry[2 * i] = dl; dry[2 * i + 1] = dr;
        }
        fx_block(&e->fx, p, period, dry, n);
        /* while overdubbing you hear yourself straight away (the take is
         * written behind the playhead, so the loop plays it next pass) */
        float mon_t = e->overdub && p[P_MONITOR] < 1.f ? 1.f : p[P_MONITOR];
        for (int i = 0; i < n; i++) {
            e->mon_g += (mon_t - e->mon_g) * fade;
            for (int c = 0; c < 2; c++) {
                float o = dry[2 * i + c] * vol + inb[2 * i + c] * e->mon_g;
                o = sat(o);   /* loop + monitored input round off instead of clipping */
                float a = fabsf(o); if (a > outmax) outmax = a;
                out[2 * (done + i) + c] = o;
            }
        }
        done += n;
    }
    __atomic_store_n(&e->ring_r, r, __ATOMIC_RELEASE);
    e->out_total += (uint64_t)frames;
    e->pos_i = (int)e->pos;
    if (e->writing || e->rec_state == ST_REC) e->version++;
    float dec = powf(0.9995f, (float)frames);
    e->in_peak = inmax > e->in_peak * dec ? inmax : e->in_peak * dec;
    e->out_peak = outmax > e->out_peak * dec ? outmax : e->out_peak * dec;
}

/* ---------------------------------------------------------- UI side */

void eng_rec_button(Engine *e) { __atomic_fetch_or(&e->cmd, CMD_REC, __ATOMIC_ACQ_REL); }
void eng_play_toggle(Engine *e) { __atomic_fetch_or(&e->cmd, CMD_PLAY, __ATOMIC_ACQ_REL); }
/* ---- undo stack (UI thread) */

static void undo_drop_oldest(Engine *e)
{
    if (!e->undo_n) return;
    if (e->undo[0].data) e->undo_bytes -= (size_t)e->undo[0].len * 2 * sizeof(float);
    free(e->undo[0].data);
    memmove(&e->undo[0], &e->undo[1], sizeof(UndoLevel) * (size_t)(e->undo_n - 1));
    e->undo_n--;
}

static void undo_reset(Engine *e)
{
    while (e->undo_n) undo_drop_oldest(e);
    e->undo_stale = 0;
}

/* a new take since: the old steps no longer apply */
static void undo_check(Engine *e) { if (e->undo_stale) undo_reset(e); }

static void undo_push(Engine *e, int kind)
{
    undo_check(e);
    int len = e->len;
    if (len <= 0) return;
    size_t bytes = kind == UNDO_DUB ? (size_t)len * 2 * sizeof(float) : 0;
    if (bytes > CD_UNDO_BYTES) return;
    while (e->undo_n && (e->undo_n == CD_UNDO_LEVELS || e->undo_bytes + bytes > CD_UNDO_BYTES))
        undo_drop_oldest(e);
    UndoLevel *u = &e->undo[e->undo_n];
    memset(u, 0, sizeof *u);
    u->kind = kind; u->len = len;
    u->loop_start = e->p[P_LOOP_START]; u->loop_end = e->p[P_LOOP_END];
    if (bytes) {
        /* the audio thread only reads the buffer until the overdub starts */
        if (!(u->data = malloc(bytes))) return;
        memcpy(u->data, e->buf, bytes);
        e->undo_bytes += bytes;
    }
    e->undo_n++;
}

void eng_overdub_toggle(Engine *e)
{
    if (!e->overdub && e->len > 0 && e->rec_state == ST_IDLE && !e->busy && !e->clear_pending)
        undo_push(e, UNDO_DUB);
    __atomic_fetch_or(&e->cmd, CMD_ODUB, __ATOMIC_ACQ_REL);
}

void eng_erase(Engine *e)
{
    if (e->len > 0 && e->rec_state == ST_IDLE && !e->clear_pending) undo_push(e, UNDO_ERASE);
    __atomic_fetch_or(&e->cmd, CMD_CLEAR, __ATOMIC_ACQ_REL);
}

void eng_clear(Engine *e) { do_clear(e); undo_reset(e); }

int eng_undo_count(Engine *e) { undo_check(e); return e->undo_n; }

void eng_ui_tick(Engine *e)
{
    int want = e->p[P_DECAY_MODE] > 0.5f && e->p[P_DECAY] > 0.f && e->playing && e->len > 0
               && e->rec_state == ST_IDLE && !e->clear_pending;
    if (want && !e->decay_armed) {
        /* the loop as it was before it starts to fade: SELECT + B gets it back */
        undo_push(e, UNDO_DUB);
        e->decay_armed = 1;
    } else if (!want && !e->overdub) e->decay_armed = 0;
}

int eng_undo(Engine *e)
{
    undo_check(e);
    if (!e->undo_n || e->rec_state == ST_REC) return 0;
    UndoLevel *u = &e->undo[e->undo_n - 1];
    int kind = u->kind;
    e->overdub = 0;
    if (kind == UNDO_DUB) {
        if (e->len != u->len) { undo_reset(e); return 0; }
        memcpy(e->buf, u->data, (size_t)u->len * 2 * sizeof(float));
        e->undo_bytes -= (size_t)u->len * 2 * sizeof(float);
        free(u->data);
    } else {
        /* the samples are still in the buffer: give the take back, stopped */
        e->clear_pending = 0; e->playing = 0; e->rec_state = ST_IDLE;
        e->len = e->rec_pos = u->len;
        e->p[P_LOOP_START] = u->loop_start; e->p[P_LOOP_END] = u->loop_end;
        int ls, le; eng_loop_range(e, &ls, &le);
        e->pos = ls;
    }
    e->undo_n--;
    e->version++;
    return kind;
}

void eng_set_buffer(Engine *e, const float *st, int frames)
{
    if (frames > e->cap) frames = e->cap;
    e->playing = 0; e->overdub = 0; e->rec_state = ST_IDLE;
    undo_reset(e);
    memcpy(e->buf, st, (size_t)frames * 2 * sizeof(float));
    e->len = frames; e->rec_pos = frames; e->pos = 0; e->play_g = 0; e->clear_pending = 0; e->decay_armed = 0;
    e->p[P_LOOP_START] = 0.f; e->p[P_LOOP_END] = 1.f;
    e->version++;
}

float *eng_bounce(Engine *e, int *out_frames)
{
    *out_frames = 0;
    int len = e->len;
    if (len < 2) return NULL;
    float p[P_COUNT];
    memcpy(p, e->p, sizeof p);
    int ls, le, xf; loop_region(len, p, &ls, &le, &xf);
    double rate = eng_rate(p);
    int rev = p[P_REVERSE] > 0.5f;
    int end = le - xf;
    int n = (int)((end - ls) / rate + 0.5);
    if (n < 1) return NULL;
    float *res = malloc((size_t)n * 2 * sizeof(float));
    FxState fx;
    if (!res || fx_init(&fx)) { free(res); return NULL; }
    fx_reset(&fx, p);
    double period = (end - ls) / rate;
    double pos = rev ? end - 1 : ls;
    float blk[BLK * 2];
    /* pass 0 primes the delay line so the kept pass loops seamlessly */
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) pos = rev ? end - 1 : ls;
        for (int done = 0; done < n;) {
            int m = n - done > BLK ? BLK : n - done;
            for (int i = 0; i < m; i++)
                loop_read(e->buf, &pos, ls, le, xf, rev ? -rate : rate, &blk[2 * i], &blk[2 * i + 1]);
            fx_block(&fx, p, period, blk, m);
            if (pass == 1)
                for (int i = 0; i < 2 * m; i++)
                    res[2 * done + i] = clampf(blk[i] * p[P_VOLUME], -1.f, 1.f);
            done += m;
        }
    }
    fx_free(&fx);
    *out_frames = n;
    return res;
}
