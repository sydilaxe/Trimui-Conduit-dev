/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The musical clock: see mclock.h. The logic is the sydilaxe apps' midi.c's, line for line where it
 * could be; what was song.bpm, live.playing and engine_* is the ClockHost. */
#include "mclock.h"

#include <math.h>
#include <stdatomic.h>
#include <stddef.h>

#include "aclock.h"
#include "linkpos.h"

static const ClockHost *H;
static atomic_int mode = MCLOCK_AUTO, transport = 1;
static _Atomic float latency_s = 0.02f;

void mclock_set_mode(int m) { atomic_store(&mode, m < MCLOCK_AUTO || m > MCLOCK_OFF ? MCLOCK_AUTO : m); }
void mclock_set_transport(int on) { atomic_store(&transport, on != 0); }
void mclock_set_latency(float s) { atomic_store(&latency_s, s < 0.f ? 0.f : s); }

static int can_follow(void) { int m = atomic_load(&mode); return m == MCLOCK_AUTO || m == MCLOCK_FOLLOW; }
static int can_send(void) { int m = atomic_load(&mode); return m == MCLOCK_AUTO || m == MCLOCK_SEND; }
/* START, STOP, CONTINUE and song position too, unless set to the clock only */
static int can_transport(void) { return can_follow() && atomic_load(&transport); }

/* the app's position read while the audio may be moving it: the middle of three reads is whole */
static double position(void)
{
    double a = H->position(), b = H->position(), c = H->position();
    if (a > b) {
        double t = a;
        a = b, b = t;
    }
    return c < a ? a : c > b ? b : c;
}

/* where the sound is at the speaker: the audio clock, else the position less the output's latency */
static double heard_now(uint64_t t)
{
    double h;
    return aclock_position(t, &h) ? h : position() - atomic_load(&latency_s) * H->bpm() / 15.0;
}

/* ------------------------------------------------------------ clock in */

static uint64_t tick_us, ext_seen_us;
static double tick_avg_us;
static int64_t ticks;           /* the 16th / 6 the last tick marked: the first tick after START is 0 */
static atomic_int following;    /* an outside clock leads */
static int ext_running;
static int settle;              /* clocks after START before the phase is held: until the first step has played */
static uint64_t precise_us;     /* the last position from a leading Brick: its tempo and phase win over the ticks' */
static double pll_i;

int mclock_following(void) { return atomic_load(&following); }

/* the last ticks' times, for the tempo: a straight line through four beats of them, which averages
 * out the jitter of USB and the thread waking */
#define TK 96
static uint64_t tk_t[TK];
static int tk_n, tk_w;

/* the microseconds a tick, from a straight line through the last n ticks' times */
static double fit(int cnt)
{
    uint64_t t0 = tk_t[(tk_w - cnt + TK) % TK];
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, n = cnt;
    for (int i = 0; i < cnt; i++) {
        double y = (double)(tk_t[(tk_w - cnt + i + TK) % TK] - t0);
        sx += i, sy += y, sxx += (double)i * i, sxy += i * y;
    }
    double d = n * sxx - sx * sx;
    return d > 0.0 ? (n * sxy - sx * sy) / d : 0.0;
}

static void clock_tick(uint64_t t)
{
    if (tick_us && (t <= tick_us || t - tick_us >= 250000)) tk_n = 0; /* a gap: the fit starts afresh */
    tk_t[tk_w] = t;
    tk_w = (tk_w + 1) % TK;
    if (tk_n < TK) tk_n++;
    /* a tempo change: the last beat's ticks (a short fit) disagree with the four beats' by more than
     * 1 %, far beyond what jitter does (a few hundredths of one): the four beats start again from that
     * beat, so the new tempo is taken in one beat rather than four (and the phase has less to catch up) */
    if (tk_n >= 48) {
        double s = fit(24);
        if (s > 0.0 && tick_avg_us > 0.0 && fabs(s - tick_avg_us) > 0.01 * tick_avg_us) tk_n = 24;
    }
    if (tk_n >= 2) {
        double slope = fit(tk_n);
        if (slope > 0.0) tick_avg_us = slope;
    }
    tick_us = t;
    ext_seen_us = t;
    if (!can_follow()) return;
    if (tick_avg_us <= 0.0) { /* the tempo isn't known yet (a clock that starts with START): count it all the same */
        if (ext_running && H->playing()) ticks++, settle -= settle > 0;
        return;
    }
    if (precise_us && t - precise_us < 1000000) { /* a leading Brick's positions hold the tempo and phase */
        atomic_store(&following, 1);
        ticks++;
        return;
    }
    atomic_store(&following, 1);
    /* the tempo: set when it has really changed (the phase lock below takes up what is left), more
     * finely while the fit is still filling (after START or a tempo change) */
    float bpm = (float)(60e6 / (tick_avg_us * 24.0));
    bpm = bpm < 40.f ? 40.f : bpm > 250.f ? 250.f : bpm;
    if (fabsf(bpm - H->bpm()) > (tk_n < TK ? 0.05f : 0.15f)) H->set_bpm(roundf(bpm * 100.f) / 100.f); /* finer while the fit fills */
    if (!ext_running || !H->playing()) return;
    ticks++;
    if (settle > 0) {
        settle--;
        return;
    }
    /* the phase: what is heard (the audio clock) should be where the clock is; trimmed gently,
     * jumped only when far off */
    double err = ticks / 6.0 - heard_now(t);
    if (fabs(err) > 2.0) {
        H->set_position(position() + err);
        H->set_rate(1.0);
        pll_i = 0.0;
    } else {
        /* a little integral takes up the tempo's last difference, so the tempo needn't be set again for it */
        pll_i = fmax(-0.004, fmin(0.004, pll_i + err * 0.002));
        H->set_rate(1.0 + fmax(-0.02, fmin(0.02, err * 0.05 + pll_i)));
    }
}

void mclock_realtime(uint8_t b, uint64_t t)
{
    switch (b) {
    case 0xf8: clock_tick(t); break;
    case 0xfa: /* START: from the top. The first tick after it is the downbeat (MIDI 1.0), whether the
                * clock was running before (most drum machines) or starts with it (most sequencers) */
        if (!can_transport()) break;
        ticks = -1;
        ext_running = 1;
        settle = 6;
        H->set_rate(1.0);
        H->set_playing(0); /* the first step plays now */
        H->set_playing(1);
        break;
    case 0xfb: /* CONTINUE: from the song position */
        if (!can_transport()) break;
        ext_running = 1;
        H->set_playing(1);
        if (ticks > 0) H->set_position(ticks / 6.0 + atomic_load(&latency_s) * H->bpm() / 15.0);
        else settle = 6;
        break;
    case 0xfc: /* STOP */
        if (!can_transport()) break;
        ext_running = 0;
        H->set_rate(1.0);
        H->set_playing(0);
        break;
    }
}

void mclock_link_position(uint64_t t, double lead, double bpm, int lead_playing)
{
    if (!can_follow()) return;
    ext_seen_us = precise_us = t;
    atomic_store(&following, 1);
    if (bpm >= 40.0 && bpm <= 250.0 && fabs(bpm - H->bpm()) > 1e-4) H->set_bpm((float)bpm);
    if (!H->playing()) {
        if (lead_playing && can_transport()) { /* joined mid-song: in at the leader's place */
            H->set_playing(1);
            H->set_position(lead + (LINK_TRANSIT_S + atomic_load(&latency_s)) * bpm / 15.0);
            ext_running = 1;
            pll_i = 0.0;
        }
        return;
    }
    if (!lead_playing) { /* the leader stopped (a leader passing on another's clock sends no STOP) */
        if (can_transport()) {
            ext_running = 0;
            H->set_rate(1.0);
            H->set_playing(0);
        }
        return;
    }
    double err = lead + LINK_TRANSIT_S * bpm / 15.0 - heard_now(t);
    if (!can_transport()) err -= 16.0 * floor((err + 8.0) / 16.0); /* the bar only: the nearest way round */
    if (fabs(err) > 0.5) {
        H->set_position(position() + err);
        H->set_rate(1.0);
        pll_i = 0.0;
        return;
    }
    pll_i = fmax(-0.003, fmin(0.003, pll_i + err * 0.02));
    H->set_rate(1.0 + fmax(-0.02, fmin(0.02, err * 0.4 + pll_i)));
}

void mclock_watch(uint64_t t)
{
    if (atomic_load(&following) && (t - ext_seen_us > 1000000 || !can_follow())) { /* the outside clock stopped, or not to be followed */
        atomic_store(&following, 0);
        ext_running = 0;
        H->set_rate(1.0);
    }
}

/* ------------------------------------------------------------ clock out */

static atomic_int out_running;
static double out_est;
static int64_t out_sent;
static uint64_t out_t, link_sent_us;

int mclock_sending(void) { return atomic_load(&out_running); }

void mclock_reset(void)
{
    tick_us = ext_seen_us = 0;
    tick_avg_us = 0.0;
    ticks = 0;
    atomic_store(&following, 0);
    ext_running = settle = 0;
    precise_us = 0, pll_i = 0.0;
    tk_n = tk_w = 0;
    atomic_store(&out_running, 0);
    link_sent_us = 0;
}

void mclock_init(const ClockHost *host)
{
    H = host;
    mclock_reset();
}

int mclock_step(uint64_t t, uint8_t *out, int max)
{
    int n = 0;
    int play = H->playing() && can_send() && !atomic_load(&following);
    if (!play) {
        if (atomic_load(&out_running) && n < max) out[n++] = 0xfc;
        atomic_store(&out_running, 0);
        return n;
    }
    double bpm = H->bpm();
    /* where the sound is, through whichever output plays it (a USB audio interface's own buffering
     * included): the audio clock */
    double heard = heard_now(t);
    if (!atomic_load(&out_running)) { /* START now: the next tick is the first */
        atomic_store(&out_running, 1);
        out_est = heard;
        out_sent = (int64_t)floor(heard * 6.0);
        out_t = t;
        if (n < max) out[n++] = 0xfa;
        return n;
    }
    /* the app's position moves in blocks: a smooth estimate between them, held to it */
    out_est += (double)(t - out_t) * 1e-6 * bpm / 15.0;
    out_t = t;
    double d = heard - out_est;
    out_est += fabs(d) > 1.0 ? d : d * 0.02;
    int64_t want = (int64_t)floor(out_est * 6.0);
    if (want - out_sent > 4) out_sent = want - 1; /* a jump (a new position): no burst of ticks */
    while (out_sent < want && n < max) {
        out[n++] = 0xf8;
        out_sent++;
    }
    if (want < out_sent) out_sent = want; /* moved back: wait for it */
    return n;
}

int mclock_link_step(uint64_t t, uint8_t *out, int max)
{
    if (!atomic_load(&out_running) && !atomic_load(&following)) {
        link_sent_us = 0;
        return 0;
    }
    if (link_sent_us && t - link_sent_us < 20000) return 0;
    link_sent_us = t;
    return linkpos_encode(out, max, heard_now(t), H->bpm(), H->playing());
}
