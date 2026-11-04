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

int main(void)
{
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
    int moved = (b - a + e->len) % e->len;
    T(abs(moved - 8192) < 64, "playhead advances at 2x");
    e->p[P_PITCH] = 0;

    /* reverse */
    e->p[P_REVERSE] = 1;
    a = e->pos_i; run_silence(e, 4096, &pk); b = e->pos_i;
    moved = (a - b + e->len) % e->len;
    T(abs(moved - 4096) < 64, "reverse moves playhead backwards");
    e->p[P_REVERSE] = 0;

    /* loop range clamps */
    e->p[P_LOOP_START] = 0.25f; e->p[P_LOOP_END] = 0.5f;
    run_silence(e, CD_SR, &pk);
    int ls, le; eng_loop_range(e, &ls, &le);
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

    /* overdub at 2x speed has no holes */
    e->p[P_PITCH] = 12; e->p[P_ODUB_KEEP] = 0;
    eng_overdub_toggle(e);
    feed_sine(e, 300, 0.2f, CD_SR / 2 + 2048);
    eng_overdub_toggle(e);
    run_silence(e, 512, &pk);
    int zeros = 0; for (int i = 0; i < e->len; i++) if (e->buf[2 * i] == 0.f && copy[2 * i] != 0.f) zeros++;
    T(zeros < 50, "2x overdub writes every frame (no gaps)");
    eng_undo(e);
    e->p[P_PITCH] = 0; e->p[P_ODUB_KEEP] = 0.9f;
    free(copy);

    /* bounce: seamless and length-correct */
    e->p[P_PITCH] = -12;
    int n; float *bo = eng_bounce(e, &n);
    T(bo && abs(n - 2 * e->len) < 4, "bounce at -12 st doubles length");
    free(bo);
    e->p[P_PITCH] = 0; e->p[P_DLY_MIX] = 0.6f; e->p[P_DLY_SYNC] = 3;
    bo = eng_bounce(e, &n);
    T(bo && abs(n - e->len) < 4, "bounce with delay keeps loop length");
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
    printf(fails ? "\n%d FAILED\n" : "\nall tests passed\n", fails);
    return fails != 0;
}
