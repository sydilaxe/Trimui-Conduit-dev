/* SPDX-License-Identifier: GPL-3.0-or-later */
/* duplex.c against ALSA's null device (no timing there: the streams run as fast as they can): it
 * opens, configures (float, 48 kHz, the period), runs the period callback with the channels as
 * asked, stops, and starts again. Skipped where there is no libasound. */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../duplex.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static volatile int periods, bad_frames, threads;
static void on_thread(void) { threads++; }
static void period(const float *cap, float *play, int frames, void *ud)
{
    (void)ud;
    (void)cap; /* (NULL when playback only) */
    if (frames != 64) bad_frames++;
    for (int i = 0; i < frames * 2; i++) play[i] = 0.f;
    periods++;
}
static void lg(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("%s ", tag), vprintf(fmt, ap), printf("\n");
    va_end(ap);
}

int main(void)
{
    void *l = dlopen("libasound.so.2", RTLD_NOW);
    if (!l) { printf("no libasound here: skipped\n"); return 0; }
    dlclose(l);
    for (int round = 0; round < 2; round++) {
        char err[200] = "";
        periods = 0;
        /* the second round as an app written before 0.1 sets it up (no rate: 48 kHz) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers" /* on purpose: rate left out */
        DxConfig c = {"pcm:null", 2, 2, 64, 2, on_thread, period, NULL, lg};
#pragma GCC diagnostic pop
        if (!round) c.rate = 48000;
        int r = dx_start(&c, err, sizeof err);
        CHECK(r == 0, "round %d: starts on the null device (%s)", round, err);
        if (r) break;
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
        CHECK(dx_alive(), "round %d: the thread runs", round);
        CHECK(periods > 0, "round %d: periods came through (%d)", round, periods);
        CHECK(dx_frames() == 64 && !bad_frames, "round %d: 64 frames a period (%d)", round, dx_frames());
        dx_stop();
        CHECK(!dx_alive(), "round %d: stopped", round);
    }
    CHECK(threads == 2, "the thread hook ran once a start (%d)", threads);
    /* playback only (a synth): no input opened, the output's own pace; and at 96 kHz */
    {
        char err[200] = "";
        periods = 0, bad_frames = 0;
        DxConfig c = {"pcm:null", 0, 2, 64, 2, NULL, period, NULL, lg, 96000};
        int r = dx_start(&c, err, sizeof err);
        CHECK(r == 0, "playback only, 96 kHz: starts (%s)", err);
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
        CHECK(r || periods > 0, "playback only: periods came through (%d)", periods);
        dx_stop();
    }
    char err[200] = "";
    DxConfig bad = {"no-such-card-xyz", 2, 2, 64, 2, NULL, period, NULL, lg, 48000};
    CHECK(dx_start(&bad, err, sizeof err) < 0 && *err, "a card that isn't there fails cleanly (%s)", err);
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
