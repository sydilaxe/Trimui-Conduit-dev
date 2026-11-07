/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests for the Conduit engine and WAV I/O (built with ASan/UBSan). */
#include "../src/dsp.h"
#include "../src/wav.h"
#include "../src/midi.h"
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
            if (k == 300) { o0 = e->overruns; u0 = e->underruns; eng_rec_button(e, 0); }
            if (k == 300 + 94) eng_rec_button(e, 0);   /* ~1 s take */
            eng_render(e, out, 512);
        }
        char msg[160];
        snprintf(msg, sizeof msg, "input %s: measured %.4f (want %.4f)",
                 dir == 0 ? "48k sent as 44.1k" : "44.1k sent as 48k", e->in_ratio, speed);
        T(fabs(e->in_ratio - speed) < 0.01 * speed, msg);
        /* count zero crossings of the take: 440 Hz at CD_SR */
        int zc = 0;
        for (int i = 1; i < e->trk[0].len; i++) if ((e->trk[0].buf[2 * i - 2] < 0) != (e->trk[0].buf[2 * i] < 0)) zc++;
        double hz = zc / 2.0 * CD_SR / e->trk[0].len;
        snprintf(msg, sizeof msg, "  the take keeps its pitch: %.1f Hz (want 440), no overflows or underruns", hz);
        T(fabs(hz - 440) < 3 && e->overruns == o0 && e->underruns == u0, msg);
        eng_free(e); free(e);
    }
}

/* Overdubbing doesn't put the input on the output unless Input monitor
 * asks for it (an interface's own monitoring would be doubled); the new
 * layer is heard when the loop comes round. */
static void test_overdub_monitor(void)
{
    Engine *e = calloc(1, sizeof *e);
    eng_init(e, 2);
    int n = CD_SR;
    float *z = calloc((size_t)n * 2, sizeof(float));
    eng_set_buffer(e, 0, z, n); free(z);
    float pk; run_silence(e, 4096, &pk);     /* FIFO primed */
    eng_play_toggle(e, 0); run_silence(e, 4096, &pk);
    for (int mon = 0; mon < 2; mon++) {
        e->p[P_MONITOR] = (float)mon;
        eng_overdub_toggle(e, 0);
        /* first 0.4 s of the overdub: the layer hasn't come round yet */
        float in[512 * 2], out[512 * 2], top = 0;
        static double ph;
        for (int k = 0; k < 40; k++) {
            for (int i = 0; i < 512; i++) { float v = 0.5f * (float)sin(ph); ph += 2 * M_PI * 220 / CD_SR; in[2 * i] = in[2 * i + 1] = v; }
            eng_push_input(e, in, 512);
            eng_render(e, out, 512);
            if (k >= 4) for (int i = 0; i < 1024; i++) if (fabsf(out[i]) > top) top = fabsf(out[i]);
        }
        eng_overdub_toggle(e, 0);
        char msg[120];
        if (mon == 0) {
            snprintf(msg, sizeof msg, "overdub with Input monitor 0%%: input not on the output (peak %.3f)", top);
            T(top < 0.01f, msg);
            /* the layer plays when the loop comes round */
            run_silence(e, CD_SR, &pk);
            snprintf(msg, sizeof msg, "  ... and plays back on the next pass (peak %.3f)", pk);
            T(pk > 0.3f, msg);
        } else {
            snprintf(msg, sizeof msg, "overdub with Input monitor 100%%: input heard straight away (peak %.3f)", top);
            T(top > 0.3f, msg);
        }
    }
    eng_free(e); free(e);
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
    eng_set_buffer(e, 0, st, n);
    T(fabsf(st[2 * (n - 1)] - st[0]) > 0.3f, "test take ends far from where it starts");
    float in[512 * 2] = { 0 }, out[512 * 2], prev[2] = { 0, 0 }, worst = 0;
    eng_play_toggle(e, 0);
    for (int k = 0; k < 400; k++) {   /* ~8 wraps, plus the start */
        eng_push_input(e, in, 512); eng_render(e, out, 512);
        float m = max_step(out, 512, prev); if (m > worst) worst = m;
    }
    /* the sine moves at most 0.0076 per sample; the crossfade adds a little */
    char msg[100]; snprintf(msg, sizeof msg, "loop wraps and starts without a click (max step %.4f)", worst);
    T(worst < 0.02f, msg);
    eng_play_toggle(e, 0);
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
    e->trk[0].p[P_DECAY] = 0;   /* this test checks the rest of the loop stays as it was */
    int n = CD_SR;
    float *st = calloc((size_t)n * 2, sizeof(float));
    int at = 30000;   /* one click in the loop, peaking at `at` */
    for (int i = -40; i <= 40; i++) st[2 * (at + i)] = st[2 * (at + i) + 1] = 0.4f * (1 - abs(i) / 41.f);
    eng_set_buffer(e, 0, st, n);
    eng_play_toggle(e, 0);
    int refr = 0;
    clap_along(e, 40, &refr);
    float *before = malloc(sizeof(float) * 2 * n);
    memcpy(before, e->trk[0].buf, sizeof(float) * 2 * n);
    eng_overdub_toggle(e, 0);
    clap_along(e, 100, &refr);   /* about one pass */
    eng_overdub_toggle(e, 0);
    clap_along(e, 1, &refr);
    int best = -1; float bv = 0;
    for (int i = 0; i < n; i++) { float d = e->trk[0].buf[2 * i] - before[2 * i]; if (d > bv) { bv = d; best = i; } }
    char msg[120];
    snprintf(msg, sizeof msg, "overdub lands on the beat it was played to (%+d frames)", best - at);
    T(best >= 0 && abs(best - at) <= 256, msg);
    int other = 0;
    for (int i = 0; i < n; i++) if (abs(i - at) > 600 && fabsf(e->trk[0].buf[2 * i] - before[2 * i]) > 1e-4f) other++;
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
    eng_set_buffer(e, 0, st, CD_SR);
    eng_play_toggle(e, 0);
    float in[512 * 2] = { 0 }, out[512 * 2], prev[2] = { 0, 0 }, worst = 0;
    for (int k = 0; k < 10; k++) { eng_push_input(e, in, 512); eng_render(e, out, 512); max_step(out, 512, prev); }
    eng_erase(e, 0);
    for (int k = 0; k < 10; k++) {
        eng_push_input(e, in, 512); eng_render(e, out, 512);
        float m = max_step(out, 512, prev); if (m > worst) worst = m;
    }
    T(e->trk[0].len == 0 && !e->trk[0].playing && e->trk[0].rec_state == ST_IDLE && worst < 0.01f,
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
    eng_set_buffer(e, 0, st, n);
    int ls, le; eng_loop_range(e, 0, &ls, &le);
    eng_play_toggle(e, 0);

    e->trk[0].p[P_DECAY] = 0.1f; e->trk[0].p[P_DECAY_MODE] = 0;
    blocks(e, 0, 100);
    T(fabs(mean_abs(e->trk[0].buf, n) - 0.5) < 1e-6, "Decay while overdubbing: just playing leaves the loop alone");
    eng_overdub_toggle(e, 0);
    blocks(e, 0, 47);                  /* 47 * 512 = 24064 frames: 5 passes and a bit */
    eng_overdub_toggle(e, 0);
    blocks(e, 0, 2);
    double m = mean_abs(e->trk[0].buf + 2 * ls, le - ls);
    char msg[120];
    snprintf(msg, sizeof msg, "10%% decay over 5 overdub passes: level %.3f (want 0.5 x 0.9^5 = 0.295)", m);
    T(m < 0.5 * pow(0.9, 4.5) && m > 0.5 * pow(0.9, 6), msg);
    while (eng_undo(e, 0)) {}
    T(fabs(mean_abs(e->trk[0].buf, n) - 0.5) < 1e-6, "undo takes back the decay");

    /* Always: fades while just playing, after saving an undo step */
    eng_set_buffer(e, 0, st, n); eng_play_toggle(e, 0);
    e->trk[0].p[P_DECAY] = 0.05f; e->trk[0].p[P_DECAY_MODE] = 1;
    blocks(e, 0, 94);                  /* ~10 passes */
    m = mean_abs(e->trk[0].buf + 2 * ls, le - ls);
    snprintf(msg, sizeof msg, "Decay always: 5%% a pass while playing, level %.3f after ~10 passes (want ~0.30)", m);
    T(m < 0.5 * pow(0.95, 9) && m > 0.5 * pow(0.95, 11), msg);
    T(eng_undo_count(e, 0) == 1 && eng_undo(e, 0) == UNDO_DUB && fabs(mean_abs(e->trk[0].buf, n) - 0.5) < 1e-6,
      "Decay always saved the loop first: undo brings it back");

    /* the same part layered over and over levels off at input / decay */
    for (int i = 0; i < 2 * n; i++) st[i] = 0;
    eng_set_buffer(e, 0, st, n); eng_play_toggle(e, 0);
    e->trk[0].p[P_DECAY] = 0.05f; e->trk[0].p[P_DECAY_MODE] = 0;
    blocks(e, 0, 20);
    eng_overdub_toggle(e, 0);
    blocks(e, 0.02f, 1000);            /* ~100 passes */
    m = mean_abs(e->trk[0].buf + 2 * ls, le - ls);
    snprintf(msg, sizeof msg, "100 layers with 5%% decay level off at %.3f (want 0.02 / 0.05 = 0.4, not 1.0+)", m);
    T(m > 0.37 && m < 0.41, msg);
    eng_overdub_toggle(e, 0);
    blocks(e, 0, 20);

    /* decay Off: layers add up, and past -2 dB round off smoothly (no hard clip) */
    eng_set_buffer(e, 0, st, n); eng_play_toggle(e, 0);
    e->trk[0].p[P_DECAY] = 0;
    blocks(e, 0, 20);
    eng_overdub_toggle(e, 0);
    blocks(e, 0.02f, 94);              /* ~10 passes: 0.2, still linear */
    m = mean_abs(e->trk[0].buf + 2 * ls, le - ls);
    snprintf(msg, sizeof msg, "decay Off: 10 layers add up to %.3f (want 0.2)", m);
    T(fabs(m - 0.2) < 0.025, msg);
    blocks(e, 0.02f, 1000);            /* 100 more: would be 2.2 */
    float top = 0;
    for (int i = 2 * ls; i < 2 * le; i++) if (fabsf(e->trk[0].buf[i]) > top) top = fabsf(e->trk[0].buf[i]);
    snprintf(msg, sizeof msg, "decay Off: 110 layers (2.2 if summed) settle below full scale, no hard clip (peak %.3f)", top);
    T(top < 0.95f && top > 0.85f, msg);
    eng_overdub_toggle(e, 0);
    free(st);
    eng_free(e); free(e);
}


/* one seamless pass of everything through the master FX (the mixdown) */
static float *bounce(Engine *e, int *n)
{
    Mixdown m;
    *n = eng_mix_begin(e, &m, 1 << 26);
    if (!*n) return NULL;
    float *b = malloc(sizeof(float) * 2 * (size_t)*n);
    eng_mix_render(e, &m, b, NULL, *n);
    eng_mix_end(&m);
    return b;
}

static double fake_t;
static double fake_now(void) { return fake_t; }

/* render blocks of silence; the MIDI clock (when bpm > 0) ticks at its
 * times with up to +-1 ms of jitter, as USB MIDI does */
static double tick_next, tick_iv;
static void clocked(Engine *e, int blocks, float *out_last)
{
    float in[512 * 2] = { 0 }, out[512 * 2];
    for (int k = 0; k < blocks; k++) {
        while (tick_iv > 0 && tick_next <= fake_t) {
            eng_midi_clock(e, 0xF8, tick_next + (rand() % 2001 - 1000) * 1e-6);
            tick_next += tick_iv;
        }
        eng_push_input(e, in, 512);
        eng_render(e, out, 512);
        fake_t += 512.0 / CD_SR;
    }
    if (out_last) memcpy(out_last, out, sizeof out);
}

static void fill_sine(float *st, int n, double hz, float amp)
{
    for (int i = 0; i < n; i++) st[2 * i] = st[2 * i + 1] = amp * (float)sin(2 * M_PI * hz * i / CD_SR);
}

/* Internal sync: the first loop is the bar; the next take waits for a bar
 * line, stops on one (rounded up, or back when pressed a little late), and
 * stays locked to the first. Rec wait off starts at once; a Free track is
 * any length. */
static void test_sync(void)
{
    Engine *e = calloc(1, sizeof *e);
    eng_init(e, 4);
    int bar = 24000;
    float *st = malloc(sizeof(float) * 2 * (bar + CD_XFADE));
    fill_sine(st, bar + CD_XFADE, 220, 0.3f);
    eng_set_buffer(e, 0, st, bar + CD_XFADE);
    T(fabs(e->tr.cyc - bar) < 1e-9 && e->trk[0].synced, "the first loop sets the bar (0.5 s)");
    eng_play_toggle(e, 0);
    float pk;
    run_silence(e, 5000, &pk);

    eng_rec_button(e, 1);
    run_silence(e, 512, &pk);
    T(e->trk[1].rec_state == ST_ARMED, "the next take waits for the bar");
    while (e->trk[1].rec_state == ST_ARMED) run_silence(e, 512, &pk);
    double ph0 = e->trk[1].phase0;
    int lag = (int)lround((e->tr.phase - ph0) * bar) - e->trk[1].rec_pos;
    char msg[160];
    snprintf(msg, sizeof msg, "it starts on the bar line (bar %.0f, %d frames off)", ph0, lag);
    T(ph0 == floor(ph0) && abs(lag) <= 1, msg);
    while (e->tr.phase - ph0 < 0.6) run_silence(e, 512, &pk);
    eng_rec_button(e, 1);
    while (e->trk[1].rec_state == ST_REC) run_silence(e, 512, &pk);
    int ls, le; eng_loop_range(e, 1, &ls, &le);
    snprintf(msg, sizeof msg, "stopped at 0.6 bar: rounds up to 1 bar (%d frames)", le - ls);
    T(eng_track_synced(e, 1) && e->trk[1].cycles == 1 && abs(le - ls - bar) <= 1, msg);

    /* pressed 0.03 bar late: ends on the bar just gone */
    eng_rec_button(e, 2);
    while (e->trk[2].rec_state != ST_REC) run_silence(e, 512, &pk);
    ph0 = e->trk[2].phase0;
    while (e->tr.phase - ph0 < 2.03) run_silence(e, 64, &pk);
    eng_rec_button(e, 2);
    run_silence(e, 512, &pk);
    eng_loop_range(e, 2, &ls, &le);
    snprintf(msg, sizeof msg, "stopped 0.03 bar late: 2 bars, at once (%d frames)", le - ls);
    T(e->trk[2].rec_state == ST_IDLE && e->trk[2].cycles == 2 && abs(le - ls - 2 * bar) <= 2, msg);

    /* many bars on, the takes still play together */
    run_silence(e, 20 * bar, &pk);
    double d1 = fmod(e->trk[1].pos - e->trk[0].pos + bar, bar);
    double d2 = fmod(e->trk[2].pos - e->trk[0].pos + 2.0 * bar, (double)bar);
    if (d1 > bar / 2) d1 -= bar;
    if (d2 > bar / 2) d2 -= bar;
    snprintf(msg, sizeof msg, "20 bars on, tracks 2 and 3 are in step with track 1 (%.2f, %.2f frames)", d1, d2);
    T(fabs(d1) < 1 && fabs(d2) < 1, msg);

    /* pitched, a synced track plays free; back at 0 it rejoins the beat without a click */
    fill_sine(st, bar + CD_XFADE, 220, 0.3f);
    e->trk[1].p[P_PITCH] = 7;
    run_silence(e, bar, &pk);
    e->trk[1].p[P_PITCH] = 0;
    float in[512 * 2] = { 0 }, out[512 * 2], prev[2] = { 0, 0 }, worst = 0;
    eng_push_input(e, in, 512); eng_render(e, out, 512); prev[0] = out[1022]; prev[1] = out[1023];
    for (int k = 0; k < 20; k++) { eng_push_input(e, in, 512); eng_render(e, out, 512); float m = max_step(out, 512, prev); if (m > worst) worst = m; }
    d1 = fmod(e->trk[1].pos - e->trk[0].pos + bar, bar); if (d1 > bar / 2) d1 -= bar;
    snprintf(msg, sizeof msg, "pitch back to 0: in step again (%.2f frames), crossfaded (max step %.3f)", d1, worst);
    T(fabs(d1) < 1 && worst < 0.12f, msg);

    /* the window moved while playing: crossfaded, not a click */
    e->trk[1].p[P_MUTE] = 1; e->trk[2].p[P_MUTE] = 1;
    run_silence(e, 4096, &pk);
    eng_push_input(e, in, 512); eng_render(e, out, 512); prev[0] = out[1022]; prev[1] = out[1023];
    e->trk[0].p[P_LOOP_START] = 0.37f; e->trk[0].p[P_LOOP_END] = 0.62f;
    worst = 0;
    for (int k = 0; k < 20; k++) { eng_push_input(e, in, 512); eng_render(e, out, 512); float m = max_step(out, 512, prev); if (m > worst) worst = m; }
    snprintf(msg, sizeof msg, "moving the window while it plays doesn't click (max step %.3f)", worst);
    T(worst < 0.03f, msg);
    eng_loop_range(e, 0, &ls, &le);
    T(e->trk[0].pos_i >= ls && e->trk[0].pos_i < le, "the playhead is inside the new window");
    e->trk[0].p[P_LOOP_START] = 0; e->trk[0].p[P_LOOP_END] = 1;
    e->trk[1].p[P_MUTE] = 0; e->trk[2].p[P_MUTE] = 0;

    /* Rec wait off: starts straight away */
    e->p[P_REC_WAIT] = 0;
    eng_rec_button(e, 3);
    run_silence(e, 512, &pk);
    T(e->trk[3].rec_state == ST_REC && e->trk[3].phase0 != floor(e->trk[3].phase0), "Rec wait off: starts at once, mid-bar");
    eng_rec_button(e, 3);
    while (e->trk[3].rec_state == ST_REC) run_silence(e, 512, &pk);
    T(eng_track_synced(e, 3) && e->trk[3].cycles >= 0.25, "...and still ends on a whole length");
    e->p[P_REC_WAIT] = 1;

    /* Free: any length, not snapped */
    eng_erase(e, 3); run_silence(e, 2048, &pk);
    e->trk[3].p[P_SYNC] = SYNC_FREE;
    eng_rec_button(e, 3);
    run_silence(e, 512, &pk);
    T(e->trk[3].rec_state == ST_REC, "a Free track records at once");
    run_silence(e, 7 * 1024, &pk);
    eng_rec_button(e, 3);
    run_silence(e, 512, &pk);
    T(!eng_track_synced(e, 3) && e->trk[3].len > 0 && e->trk[3].len % bar != 0, "a Free take keeps its own length");

    /* pan: hard left leaves the right side silent */
    for (int k = 1; k < CD_TRACKS; k++) e->trk[k].p[P_MUTE] = 1;
    e->trk[0].p[P_PAN] = -1;
    run_silence(e, 4096, &pk);
    float lmax = 0, rmax = 0;
    for (int k = 0; k < 20; k++) {
        eng_push_input(e, in, 512); eng_render(e, out, 512);
        for (int i = 0; i < 512; i++) { lmax = fmaxf(lmax, fabsf(out[2 * i])); rmax = fmaxf(rmax, fabsf(out[2 * i + 1])); }
    }
    snprintf(msg, sizeof msg, "pan hard left: left %.3f, right %.4f", lmax, rmax);
    T(lmax > 0.25f && rmax < 1e-3f, msg);
    e->trk[0].p[P_PAN] = 0;
    run_silence(e, 4096, &pk);
    lmax = rmax = 0;
    for (int k = 0; k < 20; k++) {
        eng_push_input(e, in, 512); eng_render(e, out, 512);
        for (int i = 0; i < 512; i++) { lmax = fmaxf(lmax, fabsf(out[2 * i])); rmax = fmaxf(rmax, fabsf(out[2 * i + 1])); }
    }
    snprintf(msg, sizeof msg, "pan center: both sides at the track's volume (%.3f / %.3f)", lmax, rmax);
    T(fabsf(lmax - rmax) < 1e-3f && fabsf(lmax - 0.3f * 0.9f) < 0.01f, msg);
    free(st);
    eng_free(e); free(e);
}

/* Mixdown: as long as what the synced tracks repeat in (1 and 3 bars -> 3),
 * every track as its own file (a muted one too), and the mix is their sum */
static void test_mixdown(void)
{
    Engine *e = calloc(1, sizeof *e);
    eng_init(e, 4);
    int bar = 12000;
    float *st = malloc(sizeof(float) * 2 * (3 * bar + CD_XFADE));
    fill_sine(st, bar + CD_XFADE, 300, 0.2f);
    eng_set_buffer(e, 0, st, bar + CD_XFADE);
    fill_sine(st, 3 * bar + CD_XFADE, 450, 0.2f);
    eng_set_buffer(e, 1, st, 3 * bar + CD_XFADE);
    e->trk[2].p[P_SYNC] = SYNC_FREE;
    fill_sine(st, 7000, 600, 0.2f);
    eng_set_buffer(e, 2, st, 7000);
    T(eng_track_synced(e, 1) && e->trk[1].cycles == 3 && !eng_track_synced(e, 2), "a 3-bar file loads as 3 bars; Free stays free");
    /* a file that is whole bars already (a saved mix) loads exactly, no varispeed */
    fill_sine(st, 2 * bar, 500, 0.2f);
    eng_set_buffer(e, 3, st, 2 * bar);
    int ls, le; eng_loop_range(e, 3, &ls, &le);
    T(e->trk[3].cycles == 2 && le - ls == 2 * bar && e->trk[3].cyc_rec == bar, "a whole-bars file loads at exactly its length");
    eng_clear(e, 3);
    e->trk[1].p[P_MUTE] = 1;
    e->trk[0].p[P_PAN] = 1;
    Mixdown m;
    int n = eng_mix_begin(e, &m, 1 << 26);
    char msg[120]; snprintf(msg, sizeof msg, "mix of 1 + 3 bars is 3 bars (%d frames)", n);
    T(n == 3 * bar, msg);
    float *mix = malloc(sizeof(float) * 2 * n), *stems[CD_TRACKS] = { 0 };
    for (int k = 0; k < 3; k++) stems[k] = malloc(sizeof(float) * 2 * n);
    eng_mix_render(e, &m, mix, stems, n);
    eng_mix_end(&m);
    double err = 0, s1 = 0, l0 = 0;
    for (int i = 0; i < n; i++)
        for (int c = 0; c < 2; c++) err = fmax(err, fabs(mix[2 * i + c] - (stems[0][2 * i + c] + stems[2][2 * i + c])));
    for (int i = 0; i < 2 * n; i++) s1 = fmax(s1, fabs(stems[1][i]));
    for (int i = 0; i < n; i++) l0 = fmax(l0, fabs(stems[0][2 * i]));
    T(err < 1e-6, "the mix is the sum of the unmuted tracks (FX off)");
    T(s1 > 0.1, "a muted track still gets its own file");
    T(l0 < 1e-6, "tracks are saved with their pan (hard right: nothing on the left)");
    for (int k = 0; k < 3; k++) free(stems[k]);
    free(mix); free(st);
    eng_free(e); free(e);
}

/* MIDI clock: the transport finds the tempo, starts on Start (a take armed
 * before it starts on bar 1), follows tempo changes, stops on Stop */
static void test_midi_clock(void)
{
    Engine *e = calloc(1, sizeof *e);
    eng_init(e, 8);
    e->now = fake_now; fake_t = 100.0; e->lat_frames = 0;
    e->p[P_CLOCK] = CLOCK_MIDI;
    srand(7);
    float out[1024];
    eng_rec_button(e, 0);
    clocked(e, 40, NULL);
    T(e->trk[0].rec_state == ST_ARMED && !e->tr.running, "no clock: a synced take waits");
    double start = fake_t;
    char msg[160];
    eng_midi_clock(e, 0xFA, start);
    tick_iv = 60.0 / (120 * CD_PPQ); tick_next = start + 0.001;
    clocked(e, 10, NULL);   /* ~100 ms: a few ticks to find the tempo */
    T(e->trk[0].rec_state == ST_REC && e->trk[0].phase0 == 0 && e->tr.phase < 0.06, "Start: the armed take starts on bar 1");
    clocked(e, 200, NULL);
    snprintf(msg, sizeof msg, "tempo %.2f BPM (120), bar %.0f frames (96000)", e->tr.bpm, e->tr.cyc);
    T(e->tr.running && fabs(e->tr.bpm - 120) < 0.3 && fabs(e->tr.cyc - 96000) < 300, msg);
    /* how far the transport is from the clock (ticks since Start / 96) */
    double worst = 0;
    for (int k = 0; k < 400; k++) {
        clocked(e, 1, NULL);
        double truth = (fake_t - 512.0 / CD_SR - (start + 0.001)) / (tick_iv * 96);
        double err = fabs(e->tr.phase - 512.0 / 96000 - truth) * 2000;   /* ms */
        if (err > worst) worst = err;
    }
    snprintf(msg, sizeof msg, "the transport stays within %.2f ms of the clock (jitter +-1 ms)", worst);
    T(worst < 4, msg);
    eng_rec_button(e, 0);
    while (e->trk[0].rec_state == ST_REC) clocked(e, 1, NULL);
    int ls, le; eng_loop_range(e, 0, &ls, &le);
    snprintf(msg, sizeof msg, "the take is %.0f bars long, %.3f%% off that at the clock's tempo", e->trk[0].cycles,
             fabs(le - ls - 96000 * e->trk[0].cycles) / (960 * e->trk[0].cycles));
    T(e->trk[0].cycles >= 2 && fabs(le - ls - 96000 * e->trk[0].cycles) < 0.003 * 96000 * e->trk[0].cycles, msg);

    /* tempo up to 125: the loop speeds up with it */
    tick_iv = 60.0 / (125 * CD_PPQ);
    clocked(e, 600, NULL);
    double sp = e->trk[0].cyc_rec * e->tr.rate;
    snprintf(msg, sizeof msg, "the clock speeds up to 125: tempo %.2f, loop speed x%.4f (want x1.0417)", e->tr.bpm, sp);
    T(fabs(e->tr.bpm - 125) < 0.3 && fabs(sp - 125.0 / 120) < 0.004, msg);

    /* Stop: it fades out and holds; Start again: from bar 1 */
    eng_midi_clock(e, 0xFC, fake_t);
    clocked(e, 20, out);
    float pk = 0; for (int i = 0; i < 1024; i++) pk = fmaxf(pk, fabsf(out[i]));
    T(!e->tr.running && pk < 1e-6f, "Stop: the transport stops, the loop goes quiet");
    eng_midi_clock(e, 0xFA, fake_t); tick_next = fake_t + 0.001;
    clocked(e, 3, NULL);
    snprintf(msg, sizeof msg, "Start again: running from bar 1 (phase %.4f)", e->tr.phase);
    T(e->tr.running && e->tr.phase >= 0 && e->tr.phase < 0.02, msg);
    tick_iv = 0;
    eng_free(e); free(e);
}

static void test_midi_parse(void)
{
    const uint8_t bytes[] = { 0x90, 60, 100, 62, 0, 0xB0, 7, 0xF8, 64, 0xE0, 0x00, 0x40,
                              0xF0, 1, 2, 3, 0xF7, 0x80, 60, 0, 0xF2, 1, 2, 0xB1, 10, 127 };
    MidiParser p; memset(&p, 0, sizeof p);
    MidiMsg got[16]; int n = 0;
    for (size_t i = 0; i < sizeof bytes && n < 16; i++) if (midi_parse(&p, bytes[i], &got[n])) n++;
    T(n == 7, "MIDI: 7 messages");
    T(got[0].type == MM_NOTE && got[0].num == 60 && got[0].val == 100, "MIDI: note on");
    T(got[1].type == MM_NOTE_OFF && got[1].num == 62, "MIDI: running status, velocity 0 is note off");
    T(got[2].type == MM_RT && got[2].num == 0xF8, "MIDI: a clock tick inside a message");
    T(got[3].type == MM_CC && got[3].num == 7 && got[3].val == 64, "MIDI: ...doesn't break the CC around it");
    T(got[4].type == MM_PB && got[4].val == 8192, "MIDI: pitch bend centre = 8192");
    T(got[5].type == MM_NOTE_OFF && got[5].num == 60, "MIDI: SysEx skipped");
    T(got[6].type == MM_CC && got[6].ch == 1 && got[6].num == 10 && got[6].val == 127, "MIDI: song position skipped, channel 2 CC");
}

/* FX send: 0 keeps a track dry (past the FX), 1 puts it all through, and
 * between them it's a blend; the mixdown does the same */
static void test_fx_send(void)
{
    Engine *e = calloc(1, sizeof *e);
    eng_init(e, 2);
    int n = 24000 + CD_XFADE;
    float *st = malloc(sizeof(float) * 2 * n);
    fill_sine(st, n, 3000, 0.5f);
    eng_set_buffer(e, 0, st, n);
    e->p[P_FILT_MODE] = 1; e->p[P_CUTOFF] = 0.1f; e->p[P_RESO] = 0;   /* low-pass ~40 Hz: kills 3 kHz */
    eng_play_toggle(e, 0);
    float pk, lev[3];
    float sends[3] = { 1.f, 0.f, 0.5f };
    for (int k = 0; k < 3; k++) {
        e->trk[0].p[P_FX_SEND] = sends[k];
        run_silence(e, 8192, &pk);
        run_silence(e, 8192, &pk);
        lev[k] = pk;
    }
    char msg[160];
    snprintf(msg, sizeof msg, "FX send 100%%: through the low-pass (%.3f); off: dry (%.3f); 50%%: half (%.3f)", lev[0], lev[1], lev[2]);
    T(lev[0] < 0.02f && fabsf(lev[1] - 0.45f) < 0.01f && fabsf(lev[2] - 0.225f) < 0.02f, msg);
    int m; float *b;
    e->trk[0].p[P_FX_SEND] = 0;
    b = bounce(e, &m);
    float top = 0; for (int i = 0; b && i < 2 * m; i++) top = fmaxf(top, fabsf(b[i]));
    free(b);
    e->trk[0].p[P_FX_SEND] = 1;
    b = bounce(e, &m);
    float top1 = 0; for (int i = 0; b && i < 2 * m; i++) top1 = fmaxf(top1, fabsf(b[i]));
    free(b);
    snprintf(msg, sizeof msg, "the saved mix follows the send too (dry %.3f, through the FX %.3f)", top, top1);
    T(fabsf(top - 0.45f) < 0.01f && top1 < 0.02f, msg);
    free(st);
    eng_free(e); free(e);
}

/* Volume past 100%: clean gain until it would distort, then a warm soft
 * clip (rounded, a little asymmetric, no DC left), never a hard clip */
static void test_warm_clip(void)
{
    Engine *e = calloc(1, sizeof *e);
    eng_init(e, 2);
    int n = 24000 + CD_XFADE;
    float *st = malloc(sizeof(float) * 2 * n);
    e->p[P_MASTER] = 1; e->trk[0].p[P_FX_SEND] = 0;
    float in[512 * 2] = { 0 }, out[512 * 2];
    char msg[200];
    for (int pass = 0; pass < 2; pass++) {
        float amp = pass == 0 ? 0.4f : 0.9f;   /* x1.5: 0.6 (fine), 1.35 (would clip) */
        fill_sine(st, n, 200, amp);
        eng_set_buffer(e, 0, st, n);
        e->trk[0].p[P_VOLUME] = 1.5f;
        eng_play_toggle(e, 0);
        float pk;
        run_silence(e, 24000, &pk);
        double sum = 0; float hi = 0, lo = 0, even = 0; int flat = 0, cnt = 0; float prev = 0;
        static float hist[94 * 512];
        for (int k = 0; k < 94; k++) {
            eng_push_input(e, in, 512); eng_render(e, out, 512);
            for (int i = 0; i < 512; i++) {
                float v = out[2 * i];
                hist[cnt] = v;
                /* odd harmonics only (symmetric): v(t) = -v(t + half a period) */
                if (cnt >= 120) even = fmaxf(even, fabsf(v + hist[cnt - 120]));
                sum += v; cnt++;
                hi = fmaxf(hi, v); lo = fminf(lo, v);
                if (fabsf(v) > 0.5f && v == prev) flat++;
                prev = v;
            }
        }
        if (pass == 0) {
            snprintf(msg, sizeof msg, "150%% of a 0.4 sine: clean gain, peak %.4f (want 0.6000)", hi);
            T(fabsf(hi - 0.6f) < 0.002f && fabsf(lo + 0.6f) < 0.002f, msg);
        } else {
            snprintf(msg, sizeof msg, "150%% of a 0.9 sine (1.35): rounded off at +%.3f / %.3f, warm (even harmonics %.3f), mean %.4f, no flat tops",
                     hi, lo, even, sum / cnt);
            T(hi < 1.0f && hi > 0.85f && -lo < 1.05f && even > 0.01f && fabs(sum / cnt) < 0.005 && flat == 0, msg);
        }
        eng_play_toggle(e, 0);
        run_silence(e, 4096, &pk);
    }
    free(st);
    eng_free(e); free(e);
}

/* Reverb: its own send (0 by default), the Bloom swells then rings out for
 * about the Reverb time, wide (left and right differ), Bloom 2 starts 85 ms
 * later, it stays stable at the longest time, and the saved mix has it */
static double verb_run(Engine *e, int type, float time, double *onset_ms, double *db_at4, double *corr, int *swell)
{
    int n = 4800 + CD_XFADE;
    float *st = calloc((size_t)n * 2, sizeof(float));
    srand(5);
    for (int i = 0; i < 2400; i++) st[2 * i] = st[2 * i + 1] = 0.3f * ((rand() % 20001) / 10000.f - 1);
    eng_set_buffer(e, 0, st, n);
    free(st);
    e->p[P_VERB_TYPE] = (float)type; e->p[P_VERB_TIME] = time; e->p[P_VERB_LEVEL] = 1;
    e->trk[0].p[P_FX_SEND] = 0; e->trk[0].p[P_VERB_SEND] = 1;
    bloom_reset(&e->verb);
    float in[512 * 2] = { 0 }, out[512 * 2];
    float pk;
    run_silence(e, 8192, &pk);                       /* the FIFO primes, nothing plays */
    eng_play_toggle(e, 0);
    /* one pass of the burst, then stop: what's left is the reverb */
    double e100[80] = { 0 }, lr = 0, ll = 0, rr = 0;
    int first = -1, frames = 0;
    for (int k = 0; k < 750; k++) {                  /* 8 s */
        if (k == 9) eng_play_toggle(e, 0);           /* ~0.1 s in: the burst has played */
        eng_push_input(e, in, 512); eng_render(e, out, 512);
        for (int i = 0; i < 512; i++, frames++) {
            float l = out[2 * i], r = out[2 * i + 1];
            if (frames > 4800 + 240 && first < 0 && fabsf(l) + fabsf(r) > 1e-3f) first = frames;
            if (frames / 4800 < 80) e100[frames / 4800] += l * l + r * r;
            if (frames > 9600 && frames < 4 * 48000) { lr += l * r; ll += l * l; rr += r * r; }
        }
    }
    double top = 0; int at = 0;
    for (int s = 2; s < 80; s++) if (e100[s] > top) { top = e100[s]; at = s; }
    *onset_ms = first / 48.0;
    *db_at4 = 10 * log10(e100[40] / top + 1e-30);
    *corr = lr / sqrt(ll * rr + 1e-30);
    *swell = at;
    return top;
}

static void test_reverb(void)
{
    Engine *e = calloc(1, sizeof *e);
    eng_init(e, 2);
    char msg[200];
    double on1, on2, d8, d2, c, c2, dummy;
    int sw, sw2;
    /* send 0: nothing reaches it */
    {
        int n = 4800 + CD_XFADE;
        float *st = calloc((size_t)n * 2, sizeof(float));
        for (int i = 0; i < 2400; i++) st[2 * i] = st[2 * i + 1] = 0.3f;
        eng_set_buffer(e, 0, st, n); free(st);
        e->p[P_VERB_LEVEL] = 1;
        float pk; run_silence(e, 8192, &pk);
        eng_play_toggle(e, 0); run_silence(e, 4800, &pk);
        eng_play_toggle(e, 0); run_silence(e, 4800, &pk);
        run_silence(e, 48000, &pk);
        snprintf(msg, sizeof msg, "reverb send 0 (the default): no reverb (%.6f after the loop stops)", pk);
        T(pk < 1e-6f, msg);
    }
    verb_run(e, 0, 8, &on1, &d8, &c, &sw);
    snprintf(msg, sizeof msg, "Bloom 1: swells to its peak at %.1f s, %.0f dB 4 s on (8 s time), wide (L/R correlation %.2f)", sw * 0.1, d8, c);
    T(sw >= 3 && sw <= 12 && d8 > -40 && d8 < -15 && fabs(c) < 0.3, msg);
    verb_run(e, 0, 2, &dummy, &d2, &c2, &sw2);
    snprintf(msg, sizeof msg, "Reverb time 2 s: %.0f dB 4 s on (8 s: %.0f dB)", d2, d8);
    T(d2 < d8 - 30, msg);
    verb_run(e, 1, 7, &on2, &dummy, &c2, &sw2);
    snprintf(msg, sizeof msg, "Bloom 2 starts %.0f ms after Bloom 1 (its 85 ms pre-delay)", on2 - on1);
    T(on2 - on1 > 75 && on2 - on1 < 95, msg);

    /* the longest time, fed for a minute: stays bounded */
    {
        Bloom b; bloom_init(&b, 1.024);
        float x[512 * 2], y[512 * 2], top = 0;
        srand(9);
        for (int k = 0; k < 6000; k++) {
            for (int i = 0; i < 1024; i++) x[i] = k < 3000 ? 0.5f * ((rand() % 20001) / 10000.f - 1) : 0;
            bloom_process(&b, 0, expf(-4.2f / 30) / 0.75f, x, y, 512);
            for (int i = 0; i < 1024; i++) { if (!isfinite(y[i])) top = 1e9f; top = fmaxf(top, fabsf(y[i])); }
        }
        snprintf(msg, sizeof msg, "30 s reverb fed loud noise for 32 s: stays bounded (peak %.2f)", top);
        T(top < 4.f, msg);
        bloom_free(&b);
    }

    /* the saved mix has the reverb; the saved track doesn't */
    {
        int n = 24000 + CD_XFADE;
        float *st = calloc((size_t)n * 2, sizeof(float));
        for (int i = 0; i < 1200; i++) st[2 * i] = st[2 * i + 1] = 0.3f * sinf(i * 0.3f);
        eng_set_buffer(e, 0, st, n); free(st);
        e->p[P_VERB_TYPE] = 0; e->p[P_VERB_TIME] = 8;
        e->trk[0].p[P_VERB_SEND] = 1;
        Mixdown m;
        int len = eng_mix_begin(e, &m, 1 << 26);
        float *mix = malloc(sizeof(float) * 2 * len), *stem = malloc(sizeof(float) * 2 * len), *stems[CD_TRACKS] = { stem };
        eng_mix_render(e, &m, mix, stems, len);
        eng_mix_end(&m);
        double tm = 0, ts = 0;
        for (int i = 2 * 4800; i < 2 * len; i++) { tm += fabs(mix[i]); ts += fabs(stem[i]); }
        snprintf(msg, sizeof msg, "the saved mix carries the reverb tail (%.1f), the saved track is dry (%.4f)", tm, ts);
        T(tm > 10 && ts < 1e-6, msg);
        free(mix); free(stem);
    }
    eng_free(e); free(e);
}

int main(void)
{
    test_rate_mismatch();
    test_overdub_monitor();
    Engine *e = calloc(1, sizeof *e);
    T(eng_init(e, 10) == 0, "engine init");
    float pk;

    /* record 1 s of 440 Hz */
    eng_rec_button(e, 0);
    feed_sine(e, 440, 0.5f, CD_SR);
    eng_rec_button(e, 0);
    run_silence(e, 512, &pk);
    T(e->trk[0].len >= CD_SR - 1024 && e->trk[0].len <= CD_SR + 1024, "record length ~1s");
    T(e->trk[0].playing, "auto-play after record");
    T(fabs(rms(e->trk[0].buf, e->trk[0].len) - 0.3536) < 0.02, "recorded level matches input (rms 0.354)");

    run_silence(e, CD_SR / 2, &pk);
    T(pk > 0.4f && pk <= 1.f, "loop plays back while input is silent");

    /* pitch up 12 st => loop plays twice as fast */
    e->trk[0].p[P_PITCH] = 12;
    T(fabs(eng_rate(e->trk[0].p, 0) - 2.0) < 1e-6, "pitch +12 st = rate 2.0");
    int a = e->trk[0].pos_i; run_silence(e, 4096, &pk); int b = e->trk[0].pos_i;
    int ls, le; eng_loop_range(e, 0, &ls, &le);
    int L = le - ls;
    int moved = ((b - a) % L + L) % L;
    T(abs(moved - 8192) < 64, "playhead advances at 2x");
    e->trk[0].p[P_PITCH] = 0;

    /* reverse */
    e->trk[0].p[P_REVERSE] = 1;
    run_silence(e, 512, &pk);   /* a synced track mirrors in place when reversed */
    a = e->trk[0].pos_i; run_silence(e, 4096, &pk); b = e->trk[0].pos_i;
    moved = ((a - b) % L + L) % L;
    T(abs(moved - 4096) < 64, "reverse moves playhead backwards");
    e->trk[0].p[P_REVERSE] = 0;

    /* loop range clamps */
    e->trk[0].p[P_LOOP_START] = 0.25f; e->trk[0].p[P_LOOP_END] = 0.5f;
    run_silence(e, CD_SR, &pk);
    eng_loop_range(e, 0, &ls, &le);
    T(e->trk[0].pos_i >= ls && e->trk[0].pos_i < le, "playhead stays inside loop region");

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
    e->trk[0].p[P_LOOP_START] = 0; e->trk[0].p[P_LOOP_END] = 1;

    /* overdub + undo */
    float *copy = malloc(sizeof(float) * 2 * e->trk[0].len);
    memcpy(copy, e->trk[0].buf, sizeof(float) * 2 * e->trk[0].len);
    eng_overdub_toggle(e, 0);
    feed_sine(e, 660, 0.3f, CD_SR / 2);
    eng_overdub_toggle(e, 0);
    run_silence(e, 512, &pk);
    T(memcmp(copy, e->trk[0].buf, sizeof(float) * 2 * e->trk[0].len) != 0, "overdub changed buffer");
    T(eng_undo(e, 0) == 1, "undo available");
    T(memcmp(copy, e->trk[0].buf, sizeof(float) * 2 * e->trk[0].len) == 0, "undo restores exactly");
    T(eng_undo(e, 0) == 0, "second undo is a no-op");

    /* three layers, undone one at a time back to the original */
    {
        float *lay[3];
        for (int k = 0; k < 3; k++) {
            eng_overdub_toggle(e, 0);
            feed_sine(e, 330 + 110 * k, 0.1f, CD_SR / 3);
            eng_overdub_toggle(e, 0);
            run_silence(e, 512, &pk);
            lay[k] = malloc(sizeof(float) * 2 * e->trk[0].len);
            memcpy(lay[k], e->trk[0].buf, sizeof(float) * 2 * e->trk[0].len);
        }
        T(eng_undo_count(e, 0) == 3, "three overdubs, three undo steps");
        T(eng_undo(e, 0) == UNDO_DUB && !memcmp(lay[1], e->trk[0].buf, sizeof(float) * 2 * e->trk[0].len), "undo 1 of 3: back to two layers");
        T(eng_undo(e, 0) == UNDO_DUB && !memcmp(lay[0], e->trk[0].buf, sizeof(float) * 2 * e->trk[0].len), "undo 2 of 3: back to one layer");
        T(eng_undo(e, 0) == UNDO_DUB && !memcmp(copy, e->trk[0].buf, sizeof(float) * 2 * e->trk[0].len), "undo 3 of 3: back to the take");
        for (int k = 0; k < 3; k++) free(lay[k]);
    }
    /* more overdubs than levels: the oldest go, the newest 16 stay */
    for (int k = 0; k < CD_UNDO_LEVELS + 4; k++) { eng_overdub_toggle(e, 0); run_silence(e, 1024, &pk); eng_overdub_toggle(e, 0); run_silence(e, 512, &pk); }
    T(eng_undo_count(e, 0) == CD_UNDO_LEVELS, "undo keeps the newest 16 steps");
    while (eng_undo(e, 0)) {}
    /* erase is undoable until a new take is recorded */
    {
        int len0 = e->trk[0].len;
        float *pre = malloc(sizeof(float) * 2 * len0);
        memcpy(pre, e->trk[0].buf, sizeof(float) * 2 * len0);
        eng_erase(e, 0); run_silence(e, 2048, &pk);
        T(e->trk[0].len == 0, "erase empties the loop");
        T(eng_undo(e, 0) == UNDO_ERASE && e->trk[0].len == len0 && !memcmp(pre, e->trk[0].buf, sizeof(float) * 2 * len0),
          "undo brings the erased loop back");
        free(pre);
        eng_erase(e, 0); run_silence(e, 2048, &pk);
        eng_rec_button(e, 0); feed_sine(e, 200, 0.2f, 8192); eng_rec_button(e, 0); run_silence(e, 512, &pk);
        T(eng_undo_count(e, 0) == 0, "a new take clears the undo steps");
        eng_set_buffer(e, 0, copy, len0);
        eng_play_toggle(e, 0); run_silence(e, 512, &pk);
    }

    /* overdub at 2x speed has no holes */
    e->trk[0].p[P_PITCH] = 12; e->trk[0].p[P_DECAY] = 1;
    eng_overdub_toggle(e, 0);
    feed_sine(e, 300, 0.2f, CD_SR / 2 + 2048);
    eng_overdub_toggle(e, 0);
    run_silence(e, 512, &pk);
    int zeros = 0; for (int i = 0; i < e->trk[0].len; i++) if (e->trk[0].buf[2 * i] == 0.f && copy[2 * i] != 0.f) zeros++;
    T(zeros < 50, "2x overdub writes every frame (no gaps)");
    T(eng_undo(e, 0) == UNDO_DUB, "undo the 2x overdub");
    e->trk[0].p[P_PITCH] = 0; e->trk[0].p[P_DECAY] = 0;
    free(copy);

    /* bounce: seamless and length-correct */
    eng_loop_range(e, 0, &ls, &le); L = le - ls;
    e->trk[0].p[P_PITCH] = -12;
    int n; float *bo = bounce(e, &n);
    T(bo && abs(n - 2 * L) < 4, "bounce at -12 st doubles length");
    free(bo);
    e->trk[0].p[P_PITCH] = 0; e->p[P_DLY_MIX] = 0.6f; e->p[P_DLY_SYNC] = 3;
    bo = bounce(e, &n);
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
    eng_clear(e, 0);
    run_silence(e, 8192, &pk);  /* let older input in the FIFO play out */
    e->p[P_TRIGGER] = 1; e->p[P_THRESH] = -20;
    eng_rec_button(e, 0);
    feed_sine(e, 440, 0.01f, 4096);  /* -40 dB: stays armed */
    T(e->trk[0].rec_state == ST_ARMED, "quiet input keeps recorder armed");
    feed_sine(e, 440, 0.5f, 4096);
    T(e->trk[0].rec_state == ST_REC, "loud input starts recording");
    eng_rec_button(e, 0); run_silence(e, 512, &pk);
    T(e->trk[0].len > 2048, "threshold take captured");

    /* buffer full stops recording */
    e->p[P_TRIGGER] = 0; eng_clear(e, 0);
    eng_rec_button(e, 0);
    feed_sine(e, 100, 0.2f, e->trk[0].cap + 8192);
    T(e->trk[0].rec_state == ST_IDLE && e->trk[0].len == e->trk[0].cap, "recording stops at buffer capacity");

    eng_free(e); free(e);

    test_drift();
    test_seam_and_fades();
    test_overdub_timing();
    test_erase();
    test_decay();
    test_sync();
    test_mixdown();
    test_midi_clock();
    test_midi_parse();
    test_fx_send();
    test_warm_clip();
    test_reverb();

    printf(fails ? "\n%d FAILED\n" : "\nall tests passed\n", fails);
    return fails != 0;
}
