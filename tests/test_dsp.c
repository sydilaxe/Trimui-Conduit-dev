/* Unit tests for the Conduit engine and WAV I/O (built with ASan/UBSan). */
#include "../src/dsp.h"
#include "../src/wav.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define T(c, msg) do { if (c) printf("ok   %s\n", msg); else { printf("FAIL %s\n", msg); fails++; } } while (0)

static void feed_sine(Engine *e, float hz, float amp, int frames)
{
    float in[512 * 2], out[512 * 2];
    static double ph;
    for (int done = 0; done < frames; done += 512) {
        for (int i = 0; i < 512; i++) {
            float s = amp * (float)sin(ph); ph += 2 * M_PI * hz / CD_SR;
            in[2 * i] = s; in[2 * i + 1] = s;
        }
        eng_push_input(e, in, 512);
        eng_render(e, out, 512);
    }
}

static void run_silence(Engine *e, int frames, float *peak)
{
    float in[512 * 2] = { 0 }, out[512 * 2];
    *peak = 0;
    for (int done = 0; done < frames; done += 512) {
        eng_push_input(e, in, 512);
        eng_render(e, out, 512);
        for (int i = 0; i < 1024; i++) {
            if (!isfinite(out[i])) { *peak = NAN; return; }
            if (fabsf(out[i]) > *peak) *peak = fabsf(out[i]);
        }
    }
}

static double rms(const float *st, int n)
{
    double s = 0; for (int i = 0; i < 2 * n; i++) s += st[i] * st[i];
    return sqrt(s / (2.0 * n));
}

/* largest step between consecutive output samples, per channel */
static float max_step(const float *o, int frames, float *prev)
{
    float m = 0;
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < 2; c++) {
            float d = fabsf(o[2 * i + c] - prev[c]); if (d > m) m = d; prev[c] = o[2 * i + c];
        }
    return m;
}

/* The capture device's clock runs 0.2% fast, then 0.2% slow, against the
 * output's, in other-sized blocks that arrive up to 300 frames early or late: the monitored input must stay continuous (no
 * dropped or inserted audio) and the FIFO must never run dry or overflow. */
static void test_drift(void)
{
    Engine *e = calloc(1, sizeof *e);
    eng_init(e, 2);
    e->p[P_MONITOR] = 1; e->in_block = 480;
    float in[600 * 2], out[512 * 2], prev[2] = { 0, 0 };
    double ph = 0, owed = 0, pushed = 0;
    srand(1);
    for (int dir = 0; dir < 2; dir++) {
        double speed = dir == 0 ? 1.002 : 0.998;
        float worst = 0; uint32_t u0 = 0, o0 = 0;
        for (int k = 0; k < 6000; k++) {          /* ~64 s */
            owed += 512 * speed;
            double jitter = rand() % 601 - 300;
            while (pushed + 480 <= owed + jitter) {
                pushed += 480;
                for (int i = 0; i < 480; i++) {
                    float s = 0.5f * (float)sin(ph); ph += 2 * M_PI * 110 / CD_SR;
                    in[2 * i] = in[2 * i + 1] = s;
                }
                eng_push_input(e, in, 480);
            }
            eng_render(e, out, 512);
            /* the first seconds may grow the FIFO's margin once */
            if (k == 400) { u0 = e->underruns; o0 = e->overruns; prev[0] = out[1022]; prev[1] = out[1023]; }
            if (k > 400) { float m = max_step(out, 512, prev); if (m > worst) worst = m; }
        }
        /* a 0.5 110 Hz sine moves at most 0.0072 per sample */
        char msg[120];
        snprintf(msg, sizeof msg, "input %s 0.2%%: continuous (max step %.4f), no underruns or overflows",
                 dir == 0 ? "fast by" : "slow by", worst);
        T(worst < 0.01f && e->underruns == u0 && e->overruns == o0, msg);
    }
    eng_free(e); free(e);
}

/* A capture device that runs at 48 kHz while the driver calls it 44.1 kHz
 * (so SDL hands over 52.2k frames a second): the engine must measure that,
 * record the take at its real pitch and keep the FIFO from overflowing.
 * The reverse (44.1 kHz called 48) must work too. */
static void test_rate_mismatch(void)
{
    for (int dir = 0; dir < 2; dir++) {
        double speed = dir == 0 ? 48000.0 / 44100.0 : 44100.0 / 48000.0;
        Engine *e = calloc(1, sizeof *e);
        eng_init(e, 4);
        e->in_block = 512; e->in_restart = 1;
        float in[600 * 2], out[512 * 2];
        double ph = 0, owed = 0, pushed = 0;
        uint32_t o0 = 0, u0 = 0;
        for (int k = 0; k < 900; k++) {            /* ~9.6 s */
            owed += 512 * speed;
            while (pushed + 512 <= owed) {
                pushed += 512;
                for (int i = 0; i < 512; i++) {
                    /* a 440 Hz tone in real time, sampled at the device's real rate */
                    float s = 0.5f * (float)sin(ph); ph += 2 * M_PI * 440 / (CD_SR * speed);
                    in[2 * i] = in[2 * i + 1] = s;
                }
                eng_push_input(e, in, 512);
            }
            if (k == 300) { o0 = e->overruns; u0 = e->underruns; eng_rec_button(e); }
            if (k == 300 + 94) eng_rec_button(e);   /* ~1 s take */
            eng_render(e, out, 512);
        }
        char msg[160];
        snprintf(msg, sizeof msg, "input %s: measured %.4f (want %.4f)",
                 dir == 0 ? "48k sent as 44.1k" : "44.1k sent as 48k", e->in_ratio, speed);
        T(fabs(e->in_ratio - speed) < 0.01 * speed, msg);
        /* count zero crossings of the take: 440 Hz at CD_SR */
        int zc = 0;
        for (int i = 1; i < e->len; i++) if ((e->buf[2 * i - 2] < 0) != (e->buf[2 * i] < 0)) zc++;
        double hz = zc / 2.0 * CD_SR / e->len;
        snprintf(msg, sizeof msg, "  the take keeps its pitch: %.1f Hz (want 440), no overflows or underruns", hz);
        T(fabs(hz - 440) < 3 && e->overruns == o0 && e->underruns == u0, msg);
        eng_free(e); free(e);
    }
}

/* A take that doesn't end where it starts: the wrap must not click, and
 * neither may stopping or starting playback. */
static void test_seam_and_fades(void)
{
    Engine *e = calloc(1, sizeof *e);
    eng_init(e, 2);
    /* 0.513 s of a 0.6-amplitude 97 Hz sine: ends mid-cycle */
    int n = 24624;
    float *st = malloc(sizeof(float) * 2 * n);
    for (int i = 0; i < n; i++) st[2 * i] = st[2 * i + 1] = 0.6f * (float)sin(2 * M_PI * 97 * i / CD_SR);
    eng_set_buffer(e, st, n);
    T(fabsf(st[2 * (n - 1)] - st[0]) > 0.3f, "test take ends far from where it starts");
    float in[512 * 2] = { 0 }, out[512 * 2], prev[2] = { 0, 0 }, worst = 0;
    eng_play_toggle(e);
    for (int k = 0; k < 400; k++) {   /* ~8 wraps, plus the start */
        eng_push_input(e, in, 512); eng_render(e, out, 512);
        float m = max_step(out, 512, prev); if (m > worst) worst = m;
    }
    /* the sine moves at most 0.0076 per sample; the crossfade adds a little */
    char msg[100]; snprintf(msg, sizeof msg, "loop wraps and starts without a click (max step %.4f)", worst);
    T(worst < 0.02f, msg);
    eng_play_toggle(e);
    worst = 0;
    for (int k = 0; k < 20; k++) {
        eng_push_input(e, in, 512); eng_render(e, out, 512);
        float m = max_step(out, 512, prev); if (m > worst) worst = m;
    }
    snprintf(msg, sizeof msg, "stop fades out (max step %.4f) to silence", worst);
    T(worst < 0.02f && fabsf(out[1022]) < 1e-6f, msg);
    free(st);
    eng_free(e); free(e);
}

/* Overdub plays along with what the player heard: a simulated player claps
 * the moment they hear the loop's click (lat_frames = 0: no device latency
 * besides the input FIFO), so the clap must land on the click, not after it,
 * and keep (100%) must leave the rest of the loop as it was. */
static void clap_along(Engine *e, int blocks, int *armed_in)
{
    float out[512 * 2], in[512 * 2];
    int clap = -1, hand = 0, refractory = *armed_in;
    for (int k = 0; k < blocks; k++) {
        /* the player's hand: the clap they started in the last block */
        memset(in, 0, sizeof in);
        /* clap = frames until the clap starts; it peaks 40 frames in */
        for (int i = 0; i < 512 && clap >= 0; i++) {
            if (clap > 0) { clap--; continue; }
            int c = ++hand;
            if (c > 80) { clap = -1; hand = 0; break; }
            in[2 * i] = in[2 * i + 1] = 0.3f * (1 - abs(c - 40) / 41.f);
        }
        eng_push_input(e, in, 512);
        eng_render(e, out, 512);
        /* the player's ear: a click in the loop (once per pass) */
        for (int i = 0; i < 512; i++) {
            if (refractory > 0) { refractory--; continue; }
            if (out[2 * i] > 0.2f) {
                /* heard at frame i of this block, which plays out during the next one:
                 * the clap starts at the same point of the next input block, 40 frames
                 * before the click's peak, as the player claps to its rise */
                clap = i; hand = 0; refractory = CD_SR / 2; break;
            }
        }
    }
    *armed_in = refractory;
}

static void test_overdub_timing(void)
{
    Engine *e = calloc(1, sizeof *e);
    eng_init(e, 2);
    e->lat_frames = 0;
    e->p[P_DECAY] = 0;   /* this test checks the rest of the loop stays as it was */
    int n = CD_SR;
    float *st = calloc((size_t)n * 2, sizeof(float));
    int at = 30000;   /* one click in the loop, peaking at `at` */
    for (int i = -40; i <= 40; i++) st[2 * (at + i)] = st[2 * (at + i) + 1] = 0.4f * (1 - abs(i) / 41.f);
    eng_set_buffer(e, st, n);
    eng_play_toggle(e);
    int refr = 0;
    clap_along(e, 40, &refr);
    float *before = malloc(sizeof(float) * 2 * n);
    memcpy(before, e->buf, sizeof(float) * 2 * n);
    eng_overdub_toggle(e);
    clap_along(e, 100, &refr);   /* about one pass */
    eng_overdub_toggle(e);
    clap_along(e, 1, &refr);
    int best = -1; float bv = 0;
    for (int i = 0; i < n; i++) { float d = e->buf[2 * i] - before[2 * i]; if (d > bv) { bv = d; best = i; } }
    char msg[120];
    snprintf(msg, sizeof msg, "overdub lands on the beat it was played to (%+d frames)", best - at);
    T(best >= 0 && abs(best - at) <= 256, msg);
    int other = 0;
    for (int i = 0; i < n; i++) if (abs(i - at) > 600 && fabsf(e->buf[2 * i] - before[2 * i]) > 1e-4f) other++;
    T(other == 0, "overdub with keep 100% leaves the rest of the loop untouched");
    free(st); free(before);
    eng_free(e); free(e);
}

/* Erase: stops with a fade and leaves an empty buffer, no new recording */
static void test_erase(void)
{
    Engine *e = calloc(1, sizeof *e);
    eng_init(e, 2);
    float *st = malloc(sizeof(float) * 2 * CD_SR);
    for (int i = 0; i < 2 * CD_SR; i++) st[i] = 0.3f;
    eng_set_buffer(e, st, CD_SR);
    eng_play_toggle(e);
    float in[512 * 2] = { 0 }, out[512 * 2], prev[2] = { 0, 0 }, worst = 0;
    for (int k = 0; k < 10; k++) { eng_push_input(e, in, 512); eng_render(e, out, 512); max_step(out, 512, prev); }
    eng_erase(e);
    for (int k = 0; k < 10; k++) {
        eng_push_input(e, in, 512); eng_render(e, out, 512);
        float m = max_step(out, 512, prev); if (m > worst) worst = m;
    }
    T(e->len == 0 && !e->playing && e->rec_state == ST_IDLE && worst < 0.01f,
      "erase fades out, empties the buffer and doesn't start recording");
    free(st);
    eng_free(e); free(e);
}

static double mean_abs(const float *st, int n)
{
    double a = 0; for (int i = 0; i < 2 * n; i++) a += fabs(st[i]);
    return a / (2.0 * n);
}

static void blocks(Engine *e, float level, int count)
{
    float in[512 * 2], out[512 * 2];
    for (int i = 0; i < 1024; i++) in[i] = level;
    for (int k = 0; k < count; k++) { eng_push_input(e, in, 512); eng_ui_tick(e); eng_render(e, out, 512); }
}

/* Loop decay: each pass scales the loop; only while overdubbing unless
 * Decay is Always; Always saves an undo step first; and with a little decay,
 * layer upon layer of the same part levels off instead of growing. */
static void test_decay(void)
{
    Engine *e = calloc(1, sizeof *e);
    eng_init(e, 2);
    int n = 4800 + CD_XFADE / 2;     /* plays 4800 frames (0.1 s) a pass */
    float *st = malloc(sizeof(float) * 2 * n);
    for (int i = 0; i < 2 * n; i++) st[i] = 0.5f;
    eng_set_buffer(e, st, n);
    int ls, le; eng_loop_range(e, &ls, &le);
    eng_play_toggle(e);

    e->p[P_DECAY] = 0.1f; e->p[P_DECAY_MODE] = 0;
    blocks(e, 0, 100);
    T(fabs(mean_abs(e->buf, n) - 0.5) < 1e-6, "Decay while overdubbing: just playing leaves the loop alone");
    eng_overdub_toggle(e);
    blocks(e, 0, 47);                  /* 47 * 512 = 24064 frames: 5 passes and a bit */
    eng_overdub_toggle(e);
    blocks(e, 0, 2);
    double m = mean_abs(e->buf + 2 * ls, le - ls);
    char msg[120];
    snprintf(msg, sizeof msg, "10%% decay over 5 overdub passes: level %.3f (want 0.5 x 0.9^5 = 0.295)", m);
    T(m < 0.5 * pow(0.9, 4.5) && m > 0.5 * pow(0.9, 6), msg);
    while (eng_undo(e)) {}
    T(fabs(mean_abs(e->buf, n) - 0.5) < 1e-6, "undo takes back the decay");

    /* Always: fades while just playing, after saving an undo step */
    eng_set_buffer(e, st, n); eng_play_toggle(e);
    e->p[P_DECAY] = 0.05f; e->p[P_DECAY_MODE] = 1;
    blocks(e, 0, 94);                  /* ~10 passes */
    m = mean_abs(e->buf + 2 * ls, le - ls);
    snprintf(msg, sizeof msg, "Decay always: 5%% a pass while playing, level %.3f after ~10 passes (want ~0.30)", m);
    T(m < 0.5 * pow(0.95, 9) && m > 0.5 * pow(0.95, 11), msg);
    T(eng_undo_count(e) == 1 && eng_undo(e) == UNDO_DUB && fabs(mean_abs(e->buf, n) - 0.5) < 1e-6,
      "Decay always saved the loop first: undo brings it back");

    /* the same part layered over and over levels off at input / decay */
    for (int i = 0; i < 2 * n; i++) st[i] = 0;
    eng_set_buffer(e, st, n); eng_play_toggle(e);
    e->p[P_DECAY] = 0.05f; e->p[P_DECAY_MODE] = 0;
    blocks(e, 0, 20);
    eng_overdub_toggle(e);
    blocks(e, 0.02f, 1000);            /* ~100 passes */
    m = mean_abs(e->buf + 2 * ls, le - ls);
    snprintf(msg, sizeof msg, "100 layers with 5%% decay level off at %.3f (want 0.02 / 0.05 = 0.4, not 1.0+)", m);
    T(m > 0.37 && m < 0.41, msg);
    eng_overdub_toggle(e);
    blocks(e, 0, 20);

    /* decay Off: layers add up, and past -2 dB round off smoothly (no hard clip) */
    eng_set_buffer(e, st, n); eng_play_toggle(e);
    e->p[P_DECAY] = 0;
    blocks(e, 0, 20);
    eng_overdub_toggle(e);
    blocks(e, 0.02f, 94);              /* ~10 passes: 0.2, still linear */
    m = mean_abs(e->buf + 2 * ls, le - ls);
    snprintf(msg, sizeof msg, "decay Off: 10 layers add up to %.3f (want 0.2)", m);
    T(fabs(m - 0.2) < 0.025, msg);
    blocks(e, 0.02f, 1000);            /* 100 more: would be 2.2 */
    float top = 0;
    for (int i = 2 * ls; i < 2 * le; i++) if (fabsf(e->buf[i]) > top) top = fabsf(e->buf[i]);
    snprintf(msg, sizeof msg, "decay Off: 110 layers (2.2 if summed) settle below full scale, no hard clip (peak %.3f)", top);
    T(top < 0.95f && top > 0.85f, msg);
    eng_overdub_toggle(e);
    free(st);
    eng_free(e); free(e);
}

int main(void)
{
    test_rate_mismatch();
    Engine *e = calloc(1, sizeof *e);
    T(eng_init(e, 10) == 0, "engine init");
    float pk;

    /* record 1 s of 440 Hz */
    eng_rec_button(e);
    feed_sine(e, 440, 0.5f, CD_SR);
    eng_rec_button(e);
    run_silence(e, 512, &pk);
    T(e->len >= CD_SR - 1024 && e->len <= CD_SR + 1024, "record length ~1s");
    T(e->playing, "auto-play after record");
    T(fabs(rms(e->buf, e->len) - 0.3536) < 0.02, "recorded level matches input (rms 0.354)");

    run_silence(e, CD_SR / 2, &pk);
    T(pk > 0.4f && pk <= 1.f, "loop plays back while input is silent");

    /* pitch up 12 st => loop plays twice as fast */
    e->p[P_PITCH] = 12;
    T(fabs(eng_rate(e->p) - 2.0) < 1e-6, "pitch +12 st = rate 2.0");
    int a = e->pos_i; run_silence(e, 4096, &pk); int b = e->pos_i;
    int ls, le; eng_loop_range(e, &ls, &le);
    int L = le - ls;
    int moved = ((b - a) % L + L) % L;
    T(abs(moved - 8192) < 64, "playhead advances at 2x");
    e->p[P_PITCH] = 0;

    /* reverse */
    e->p[P_REVERSE] = 1;
    a = e->pos_i; run_silence(e, 4096, &pk); b = e->pos_i;
    moved = ((a - b) % L + L) % L;
    T(abs(moved - 4096) < 64, "reverse moves playhead backwards");
    e->p[P_REVERSE] = 0;

    /* loop range clamps */
    e->p[P_LOOP_START] = 0.25f; e->p[P_LOOP_END] = 0.5f;
    run_silence(e, CD_SR, &pk);
    eng_loop_range(e, &ls, &le);
    T(e->pos_i >= ls && e->pos_i < le, "playhead stays inside loop region");

    /* FX stability: extreme settings never produce NaN / runaway */
    e->p[P_FILT_MODE] = 1; e->p[P_CUTOFF] = 0.3f; e->p[P_RESO] = 0.97f;
    e->p[P_DRIVE] = 1; e->p[P_BITS] = 3; e->p[P_DOWNS] = 12;
    e->p[P_DLY_FB] = 0.95f; e->p[P_DLY_MIX] = 1; e->p[P_PINGPONG] = 1; e->p[P_DLY_SYNC] = 2;
    run_silence(e, CD_SR * 4, &pk);
    T(isfinite(pk) && pk <= 1.f, "extreme FX stay finite and clipped");
    for (int m = 0; m < 4; m++) {
        e->p[P_FILT_MODE] = (float)m; run_silence(e, 8192, &pk);
        T(isfinite(pk), m == 0 ? "filter off ok" : m == 1 ? "low-pass ok" : m == 2 ? "high-pass ok" : "band-pass ok");
    }
    /* low-pass actually attenuates */
    {
        FxState f; fx_init(&f); float p[P_COUNT]; eng_default_params(p);
        p[P_FILT_MODE] = 1; p[P_CUTOFF] = 0.2f; p[P_RESO] = 0;  /* ~80 Hz */
        fx_reset(&f, p);
        float blk[256 * 2]; double sum = 0; double ph = 0;
        for (int k = 0; k < 200; k++) {
            for (int i = 0; i < 256; i++) { float s = (float)sin(ph); ph += 2 * M_PI * 5000 / CD_SR; blk[2 * i] = blk[2 * i + 1] = s; }
            fx_block(&f, p, 0, blk, 256);
            if (k > 100) sum += rms(blk, 256);
        }
        T(sum / 99 < 0.01, "low-pass @~80Hz kills 5 kHz");
        fx_free(&f);
    }
    eng_reset_fx_params(e->p);
    e->p[P_LOOP_START] = 0; e->p[P_LOOP_END] = 1;

    /* overdub + undo */
    float *copy = malloc(sizeof(float) * 2 * e->len);
    memcpy(copy, e->buf, sizeof(float) * 2 * e->len);
    eng_overdub_toggle(e);
    feed_sine(e, 660, 0.3f, CD_SR / 2);
    eng_overdub_toggle(e);
    run_silence(e, 512, &pk);
    T(memcmp(copy, e->buf, sizeof(float) * 2 * e->len) != 0, "overdub changed buffer");
    T(eng_undo(e) == 1, "undo available");
    T(memcmp(copy, e->buf, sizeof(float) * 2 * e->len) == 0, "undo restores exactly");
    T(eng_undo(e) == 0, "second undo is a no-op");

    /* three layers, undone one at a time back to the original */
    {
        float *lay[3];
        for (int k = 0; k < 3; k++) {
            eng_overdub_toggle(e);
            feed_sine(e, 330 + 110 * k, 0.1f, CD_SR / 3);
            eng_overdub_toggle(e);
            run_silence(e, 512, &pk);
            lay[k] = malloc(sizeof(float) * 2 * e->len);
            memcpy(lay[k], e->buf, sizeof(float) * 2 * e->len);
        }
        T(eng_undo_count(e) == 3, "three overdubs, three undo steps");
        T(eng_undo(e) == UNDO_DUB && !memcmp(lay[1], e->buf, sizeof(float) * 2 * e->len), "undo 1 of 3: back to two layers");
        T(eng_undo(e) == UNDO_DUB && !memcmp(lay[0], e->buf, sizeof(float) * 2 * e->len), "undo 2 of 3: back to one layer");
        T(eng_undo(e) == UNDO_DUB && !memcmp(copy, e->buf, sizeof(float) * 2 * e->len), "undo 3 of 3: back to the take");
        for (int k = 0; k < 3; k++) free(lay[k]);
    }
    /* more overdubs than levels: the oldest go, the newest 16 stay */
    for (int k = 0; k < CD_UNDO_LEVELS + 4; k++) { eng_overdub_toggle(e); run_silence(e, 1024, &pk); eng_overdub_toggle(e); run_silence(e, 512, &pk); }
    T(eng_undo_count(e) == CD_UNDO_LEVELS, "undo keeps the newest 16 steps");
    while (eng_undo(e)) {}
    /* erase is undoable until a new take is recorded */
    {
        int len0 = e->len;
        float *pre = malloc(sizeof(float) * 2 * len0);
        memcpy(pre, e->buf, sizeof(float) * 2 * len0);
        eng_erase(e); run_silence(e, 2048, &pk);
        T(e->len == 0, "erase empties the loop");
        T(eng_undo(e) == UNDO_ERASE && e->len == len0 && !memcmp(pre, e->buf, sizeof(float) * 2 * len0),
          "undo brings the erased loop back");
        free(pre);
        eng_erase(e); run_silence(e, 2048, &pk);
        eng_rec_button(e); feed_sine(e, 200, 0.2f, 8192); eng_rec_button(e); run_silence(e, 512, &pk);
        T(eng_undo_count(e) == 0, "a new take clears the undo steps");
        eng_set_buffer(e, copy, len0);
        eng_play_toggle(e); run_silence(e, 512, &pk);
    }

    /* overdub at 2x speed has no holes */
    e->p[P_PITCH] = 12; e->p[P_DECAY] = 1;
    eng_overdub_toggle(e);
    feed_sine(e, 300, 0.2f, CD_SR / 2 + 2048);
    eng_overdub_toggle(e);
    run_silence(e, 512, &pk);
    int zeros = 0; for (int i = 0; i < e->len; i++) if (e->buf[2 * i] == 0.f && copy[2 * i] != 0.f) zeros++;
    T(zeros < 50, "2x overdub writes every frame (no gaps)");
    T(eng_undo(e) == UNDO_DUB, "undo the 2x overdub");
    e->p[P_PITCH] = 0; e->p[P_DECAY] = 0;
    free(copy);

    /* bounce: seamless and length-correct */
    eng_loop_range(e, &ls, &le); L = le - ls;
    e->p[P_PITCH] = -12;
    int n; float *bo = eng_bounce(e, &n);
    T(bo && abs(n - 2 * L) < 4, "bounce at -12 st doubles length");
    free(bo);
    e->p[P_PITCH] = 0; e->p[P_DLY_MIX] = 0.6f; e->p[P_DLY_SYNC] = 3;
    bo = eng_bounce(e, &n);
    T(bo && abs(n - L) < 4, "bounce with delay keeps loop length");
    T(bo && rms(bo, 4800) > 0.05, "bounce start already has delay tail (primed)");

    /* WAV roundtrip */
    T(wav_write("/tmp/conduit_t16.wav", bo, n, CD_SR, 16) == 0, "write 16-bit wav");
    T(wav_write("/tmp/conduit_t32.wav", bo, n, CD_SR, 32) == 0, "write float wav");
    int n16, n32;
    float *r16 = wav_read("/tmp/conduit_t16.wav", CD_SR, 1 << 30, &n16);
    float *r32 = wav_read("/tmp/conduit_t32.wav", CD_SR, 1 << 30, &n32);
    T(r16 && n16 == n, "read 16-bit wav length");
    T(r32 && n32 == n && memcmp(r32, bo, sizeof(float) * 2 * n) == 0, "float wav is bit exact");
    double err = 0; for (int i = 0; r16 && i < 2 * n; i++) err = fmax(err, fabs(r16[i] - bo[i]));
    T(err < 1.0 / 16000, "16-bit wav error < 1 LSB");
    int n44; float *r44 = wav_read("/tmp/conduit_t16.wav", 44100, 1 << 30, &n44);
    T(r44 && abs(n44 - (int)((long)n * 44100 / 48000)) < 3, "resample 48k->44.1k length");
    free(r16); free(r32); free(r44); free(bo);

    /* threshold trigger */
    eng_clear(e);
    run_silence(e, 8192, &pk);  /* let older input in the FIFO play out */
    e->p[P_TRIGGER] = 1; e->p[P_THRESH] = -20;
    eng_rec_button(e);
    feed_sine(e, 440, 0.01f, 4096);  /* -40 dB: stays armed */
    T(e->rec_state == ST_ARMED, "quiet input keeps recorder armed");
    feed_sine(e, 440, 0.5f, 4096);
    T(e->rec_state == ST_REC, "loud input starts recording");
    eng_rec_button(e); run_silence(e, 512, &pk);
    T(e->len > 2048, "threshold take captured");

    /* buffer full stops recording */
    e->p[P_TRIGGER] = 0; eng_clear(e);
    eng_rec_button(e);
    feed_sine(e, 100, 0.2f, e->cap + 8192);
    T(e->rec_state == ST_IDLE && e->len == e->cap, "recording stops at buffer capacity");

    eng_free(e); free(e);

    test_drift();
    test_seam_and_fades();
    test_overdub_timing();
    test_erase();
    test_decay();

    printf(fails ? "\n%d FAILED\n" : "\nall tests passed\n", fails);
    return fails != 0;
}
