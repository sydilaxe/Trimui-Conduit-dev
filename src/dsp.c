#include "dsp.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CMD_REC  1
#define CMD_PLAY 2
#define CMD_ODUB 4
#define BLK 128
#define MIN_TAKE (CD_SR / 50)  /* 20 ms */

static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline float db2lin(float db) { return powf(10.f, db / 20.f); }

void eng_default_params(float *p)
{
    memset(p, 0, sizeof(float) * P_COUNT);
    p[P_LOOP_START] = 0.f;  p[P_LOOP_END] = 1.f;
    p[P_PITCH] = 0.f;       p[P_FINE] = 0.f;  p[P_REVERSE] = 0.f;
    p[P_ODUB_KEEP] = 0.9f;  p[P_VOLUME] = 0.9f; p[P_MONITOR] = 0.f;
    eng_reset_fx_params(p);
    p[P_IN_GAIN] = 0.f;     p[P_TRIGGER] = 0.f; p[P_THRESH] = -36.f;
    p[P_AUTOPLAY] = 1.f;    p[P_SAVE_FMT] = 0.f;
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
    e->undo = calloc((size_t)e->cap * 2, sizeof(float));
    e->stamp = calloc((size_t)e->cap, 1);
    if (!e->buf || !e->undo || !e->stamp || fx_init(&e->fx)) { eng_free(e); return -1; }
    eng_default_params(e->p);
    fx_reset(&e->fx, e->p);
    e->od_session = 1;
    return 0;
}

void eng_free(Engine *e)
{
    free(e->buf); free(e->undo); free(e->stamp);
    e->buf = e->undo = NULL; e->stamp = NULL;
    fx_free(&e->fx);
}

double eng_rate(const float *p)
{
    return pow(2.0, (p[P_PITCH] + p[P_FINE] / 100.0) / 12.0);
}

static void loop_range(int len, const float *p, int *ls, int *le)
{
    float a = clampf(p[P_LOOP_START], 0.f, 1.f), b = clampf(p[P_LOOP_END], 0.f, 1.f);
    if (b < a) { float t = a; a = b; b = t; }
    int s = (int)(a * len), en = (int)(b * len);
    if (en > len) en = len;
    if (en - s < MIN_TAKE / 4) { en = s + MIN_TAKE / 4; if (en > len) { en = len; s = en - MIN_TAKE / 4; if (s < 0) s = 0; } }
    *ls = s; *le = en;
}

void eng_loop_range(const Engine *e, int *ls, int *le) { loop_range(e->len, e->p, ls, le); }

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

static void start_rec(Engine *e)
{
    e->playing = 0; e->overdub = 0; e->have_undo = 0;
    e->rec_pos = 0; e->len = 0; e->rec_state = ST_REC;
    e->version++;
}

static void finish_rec(Engine *e)
{
    e->rec_state = ST_IDLE;
    e->len = e->rec_pos >= MIN_TAKE ? e->rec_pos : 0;
    e->p[P_LOOP_START] = 0.f; e->p[P_LOOP_END] = 1.f;
    e->pos = e->p[P_REVERSE] > 0.5f && e->len ? e->len - 1 : 0;
    e->playing = e->len > 0 && e->p[P_AUTOPLAY] > 0.5f;
    e->version++;
}

static void handle_cmds(Engine *e)
{
    int c = __atomic_exchange_n(&e->cmd, 0, __ATOMIC_ACQ_REL);
    if (!c) return;
    if (c & CMD_REC) {
        if (e->rec_state == ST_REC) finish_rec(e);
        else if (e->rec_state == ST_ARMED) e->rec_state = ST_IDLE;
        else if (!e->busy) {
            if (e->p[P_TRIGGER] > 0.5f) { e->rec_state = ST_ARMED; e->playing = 0; e->overdub = 0; }
            else start_rec(e);
        }
    }
    if ((c & CMD_PLAY) && e->rec_state != ST_REC && e->len > 0) {
        e->playing = !e->playing;
        if (!e->playing) e->overdub = 0;
        else { int ls, le; eng_loop_range(e, &ls, &le); e->pos = e->p[P_REVERSE] > 0.5f ? le - 1 : ls; }
    }
    if ((c & CMD_ODUB) && e->rec_state != ST_REC && e->len > 0 && !e->busy) {
        if (e->overdub) e->overdub = 0;
        else {
            if (++e->od_session == 0) { memset(e->stamp, 0, (size_t)e->cap); e->od_session = 1; }
            e->overdub = 1; e->have_undo = 1; e->last_od = -1;
            if (!e->playing) {
                int ls, le; eng_loop_range(e, &ls, &le);
                e->pos = e->p[P_REVERSE] > 0.5f ? le - 1 : ls; e->playing = 1;
            }
        }
    }
}

static inline void od_write(Engine *e, int idx, float il, float ir, float keep)
{
    if (e->stamp[idx] != e->od_session) {
        e->undo[2 * idx] = e->buf[2 * idx]; e->undo[2 * idx + 1] = e->buf[2 * idx + 1];
        e->stamp[idx] = e->od_session;
    }
    e->buf[2 * idx] = clampf(e->buf[2 * idx] * keep + il, -1.f, 1.f);
    e->buf[2 * idx + 1] = clampf(e->buf[2 * idx + 1] * keep + ir, -1.f, 1.f);
}

/* read loop with linear interpolation; advance playhead */
static inline void loop_read(const float *buf, double *pos, int ls, int le, double step,
                             float *l, float *r)
{
    double p = *pos;
    int L = le - ls;
    if (p < ls || p >= le) p = step < 0 ? le - 1 : ls;
    int i0 = (int)p; float fr = (float)(p - i0);
    int i1 = i0 + 1 >= le ? ls : i0 + 1;
    *l = buf[2 * i0] * (1 - fr) + buf[2 * i1] * fr;
    *r = buf[2 * i0 + 1] * (1 - fr) + buf[2 * i1 + 1] * fr;
    p += step;
    if (p >= le) p -= L;
    if (p < ls) p += L;
    *pos = p;
}

void eng_render(Engine *e, float *out, int frames)
{
    float p[P_COUNT];
    handle_cmds(e);
    memcpy(p, e->p, sizeof p);
    float gain = db2lin(p[P_IN_GAIN]);
    float thr = db2lin(p[P_THRESH]);
    float keep = p[P_ODUB_KEEP], vol = p[P_VOLUME], mon = p[P_MONITOR];
    double rate = eng_rate(p);
    int rev = p[P_REVERSE] > 0.5f;
    float inmax = 0, outmax = 0;

    /* keep monitoring latency bounded when capture clock runs ahead */
    uint32_t w = __atomic_load_n(&e->ring_w, __ATOMIC_ACQUIRE);
    uint32_t r = e->ring_r;
    if (w - r > (uint32_t)(frames + 4096)) r = w - (uint32_t)frames;

    float dry[BLK * 2], inb[BLK * 2];
    int done = 0;
    while (done < frames) {
        int n = frames - done > BLK ? BLK : frames - done;
        int len = e->len, ls = 0, le = 0;
        if (len > 1) loop_range(len, p, &ls, &le);
        double period = le > ls ? (le - ls) / rate : 0;
        for (int i = 0; i < n; i++) {
            float il = 0, ir = 0;
            if (w != r) {
                uint32_t k = (r & (CD_RING - 1)) * 2;
                il = e->ring[k] * gain; ir = e->ring[k + 1] * gain; r++;
            } else e->underruns++;
            float m = fabsf(il) > fabsf(ir) ? fabsf(il) : fabsf(ir);
            if (m > inmax) inmax = m;
            inb[2 * i] = il; inb[2 * i + 1] = ir;

            if (e->rec_state == ST_ARMED && m >= thr) start_rec(e);
            if (e->rec_state == ST_REC && !e->busy) {
                e->buf[2 * e->rec_pos] = il; e->buf[2 * e->rec_pos + 1] = ir;
                e->rec_pos++; e->len = e->rec_pos;
                if (e->rec_pos >= e->cap) finish_rec(e);
            }

            float dl = 0, dr = 0;
            if (e->playing && len > 1 && le > ls) {
                if (e->overdub && !e->busy) {
                    double pp = e->pos;
                    if (pp < ls || pp >= le) pp = rev ? le - 1 : ls;
                    int idx = (int)pp;
                    if (e->last_od < ls || e->last_od >= le) {
                        od_write(e, idx, il, ir, keep);
                    } else {
                        /* write every index the playhead crossed since the
                         * last frame (fills gaps when rate > 1, skips
                         * repeats when rate < 1) */
                        int cur = e->last_od, guard = 0;
                        while (cur != idx && guard++ < 64) {
                            cur += rev ? -1 : 1;
                            if (cur >= le) cur = ls;
                            if (cur < ls) cur = le - 1;
                            od_write(e, cur, il, ir, keep);
                        }
                    }
                    e->last_od = idx;
                }
                double pos = e->pos;
                loop_read(e->buf, &pos, ls, le, rev ? -rate : rate, &dl, &dr);
                e->pos = pos;
            }
            dry[2 * i] = dl; dry[2 * i + 1] = dr;
        }
        fx_block(&e->fx, p, period, dry, n);
        for (int i = 0; i < 2 * n; i++) {
            float o = dry[i] * vol + inb[i] * mon;
            o = clampf(o, -1.f, 1.f);
            float a = fabsf(o); if (a > outmax) outmax = a;
            out[2 * done + i] = o;
        }
        done += n;
    }
    __atomic_store_n(&e->ring_r, r, __ATOMIC_RELEASE);
    e->pos_i = (int)e->pos;
    if (e->overdub || e->rec_state == ST_REC) e->version++;
    float dec = powf(0.9995f, (float)frames);
    e->in_peak = inmax > e->in_peak * dec ? inmax : e->in_peak * dec;
    e->out_peak = outmax > e->out_peak * dec ? outmax : e->out_peak * dec;
}

/* ---------------------------------------------------------- UI side */

void eng_rec_button(Engine *e) { __atomic_fetch_or(&e->cmd, CMD_REC, __ATOMIC_ACQ_REL); }
void eng_play_toggle(Engine *e) { __atomic_fetch_or(&e->cmd, CMD_PLAY, __ATOMIC_ACQ_REL); }
void eng_overdub_toggle(Engine *e) { __atomic_fetch_or(&e->cmd, CMD_ODUB, __ATOMIC_ACQ_REL); }

void eng_clear(Engine *e)
{
    e->playing = 0; e->overdub = 0; e->rec_state = ST_IDLE;
    e->len = 0; e->rec_pos = 0; e->have_undo = 0; e->pos = 0;
    e->version++;
}

int eng_undo(Engine *e)
{
    if (!e->have_undo) return 0;
    e->overdub = 0;
    for (int i = 0; i < e->len; i++)
        if (e->stamp[i] == e->od_session) {
            e->buf[2 * i] = e->undo[2 * i]; e->buf[2 * i + 1] = e->undo[2 * i + 1];
        }
    if (++e->od_session == 0) { memset(e->stamp, 0, (size_t)e->cap); e->od_session = 1; }
    e->have_undo = 0;
    e->version++;
    return 1;
}

void eng_set_buffer(Engine *e, const float *st, int frames)
{
    if (frames > e->cap) frames = e->cap;
    e->playing = 0; e->overdub = 0; e->rec_state = ST_IDLE; e->have_undo = 0;
    memcpy(e->buf, st, (size_t)frames * 2 * sizeof(float));
    e->len = frames; e->rec_pos = frames; e->pos = 0;
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
    int ls, le; loop_range(len, p, &ls, &le);
    double rate = eng_rate(p);
    int rev = p[P_REVERSE] > 0.5f;
    int n = (int)((le - ls) / rate + 0.5);
    if (n < 1) return NULL;
    float *res = malloc((size_t)n * 2 * sizeof(float));
    FxState fx;
    if (!res || fx_init(&fx)) { free(res); return NULL; }
    fx_reset(&fx, p);
    double period = (le - ls) / rate;
    double pos = rev ? le - 1 : ls;
    float blk[BLK * 2];
    /* pass 0 primes the delay line so the kept pass loops seamlessly */
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) pos = rev ? le - 1 : ls;
        for (int done = 0; done < n;) {
            int m = n - done > BLK ? BLK : n - done;
            for (int i = 0; i < m; i++)
                loop_read(e->buf, &pos, ls, le, rev ? -rate : rate, &blk[2 * i], &blk[2 * i + 1]);
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
