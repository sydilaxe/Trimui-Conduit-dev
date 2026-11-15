/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Conduit's log: see applog.h. The STAT / PROC / HINT / JUMP / END part is
 * HI5's and RadioScan's perflog.c, with the float output and Conduit's own
 * hints; the rest (stderr captured and timestamped, the monitor thread, the
 * crash and signal handlers, INPUT) is Conduit's. */
#define _GNU_SOURCE
#include "applog.h"

#include <dirent.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define AT(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define SET(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
#define TAKE(x) __atomic_exchange_n(&(x), 0, __ATOMIC_RELAXED)
#define ADD(x, v) __atomic_fetch_add(&(x), (v), __ATOMIC_RELAXED)

static int level = PERF_BASIC;
static char prefix[16] = "APP";
static const char *build_name;
static uint64_t t_start, t_last_stat;

/* where the lines go: the stderr the app was started with (log.txt) */
static int out_fd = 2;
static uint64_t out_bytes;
#define LOG_QUIET_AT (24u << 20)  /* past this, the routine lines stop */
#define LOG_FULL_AT (32u << 20)   /* past this, everything but CRASH and END */
static int quiet_said, full_said;

uint64_t perf_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

int perf_level(void) { return level; }

static int routine(const char *tag)
{
    static const char *const r[] = { "STAT", "PROC", "INPUT", "HW", "ENGINE", "KEY", "SET", "MSG" };
    for (size_t i = 0; i < sizeof r / sizeof r[0]; i++)
        if (!strcmp(tag, r[i]))
            return 1;
    return 0;
}

static void write_all(int fd, const char *b, size_t n)
{
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return;
        b += w, n -= (size_t)w;
    }
}

/* one line, in one write (lines from different threads don't mix) */
static void vline(const char *tag, const char *fmt, va_list ap)
{
    uint64_t used = AT(out_bytes);
    int crit = !strcmp(tag, "CRASH") || !strcmp(tag, "END") || !strcmp(tag, "LOG");
    if (used > LOG_FULL_AT && !crit) {
        if (!__atomic_exchange_n(&full_said, 1, __ATOMIC_RELAXED))
            perf_log("LOG", "log.txt is %u MB: nothing more until the app closes (CRASH and END still)",
                     (unsigned)(LOG_FULL_AT >> 20));
        return;
    }
    if (used > LOG_QUIET_AT && routine(tag)) {
        if (!__atomic_exchange_n(&quiet_said, 1, __ATOMIC_RELAXED))
            perf_log("LOG", "log.txt is %u MB: only problems from here on (no more STAT, PROC, INPUT, HW, ENGINE, SET, MSG)",
                     (unsigned)(LOG_QUIET_AT >> 20));
        return;
    }
    char b[1200];
    int n = snprintf(b, sizeof b, "%9.3f %-6s ", (perf_now_us() - t_start) / 1e6, tag);
    int m = vsnprintf(b + n, sizeof b - (size_t)n - 1, fmt, ap);
    if (m < 0)
        m = 0;
    if ((size_t)(n + m) > sizeof b - 2)
        m = (int)sizeof b - 2 - n;
    n += m;
    b[n++] = '\n';
    write_all(out_fd, b, (size_t)n);
    ADD(out_bytes, (uint64_t)n);
}

void perf_log(const char *tag, const char *fmt, ...)
{
    if (level == PERF_OFF && strcmp(tag, "CRASH") && strcmp(tag, "SIGNAL"))
        return;
    va_list ap;
    va_start(ap, fmt);
    vline(tag, fmt, ap);
    va_end(ap);
}

/* a number from a system file, or -1 */
static long read_num(const char *path)
{
    FILE *f = fopen(path, "r");
    long v = -1;
    if (f) {
        if (fscanf(f, "%ld", &v) != 1)
            v = -1;
        fclose(f);
    }
    return v;
}

/* the first line of a system file, or "" */
static void read_line(const char *path, char *out, int n)
{
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    if (!fgets(out, n, f))
        out[0] = 0;
    out[strcspn(out, "\n")] = 0;
    fclose(f);
}

/* the hottest of the thermal zones, in degrees (some kernels give millidegrees) */
static long temperature(void)
{
    long hot = -1;
    for (int z = 0; z < 8; z++) {
        char p[64];
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/temp", z);
        long t = read_num(p);
        if (t > 1000)
            t /= 1000;
        if (t > hot)
            hot = t;
    }
    return hot;
}

#define CPU "/sys/devices/system/cpu/"

static long max_khz = -1;
static uintptr_t exe_base, exe_end;

static void system_line(void)
{
    char board[64], gov[32], fw[64], kern[128];
    read_line("/opt/muos/device/config/board/name", board, sizeof board);
    read_line("/opt/muos/config/system/version", fw, sizeof fw);
    read_line("/proc/version", kern, sizeof kern);
    read_line(CPU "cpu0/cpufreq/scaling_governor", gov, sizeof gov);
    max_khz = read_num(CPU "cpu0/cpufreq/scaling_max_freq");
    long min_khz = read_num(CPU "cpu0/cpufreq/scaling_min_freq");
    long pages = sysconf(_SC_PHYS_PAGES), avail = sysconf(_SC_AVPHYS_PAGES), psz = sysconf(_SC_PAGESIZE);
    char clock[48] = "clock ?", temp[16] = "? C";
    if (max_khz > 0)
        snprintf(clock, sizeof clock, "%ld-%ld MHz", min_khz / 1000, max_khz / 1000);
    if (temperature() >= 0)
        snprintf(temp, sizeof temp, "%ld C", temperature());
    perf_log("SYS", "MustardOS%s%s%s%s, build %s, %ld cores, governor %s, %s, memory %ld MB (%ld free), %s",
             fw[0] ? " " : "", fw, board[0] ? ", board " : "", board, build_name ? build_name : "?",
             sysconf(_SC_NPROCESSORS_ONLN), gov[0] ? gov : "?", clock,
             pages > 0 ? pages * (psz / 1024) / 1024 : -1, avail > 0 ? avail * (psz / 1024) / 1024 : -1, temp);
    perf_log("SYS", "%s", kern[0] ? kern : "kernel ?");
    perf_log("SYS", "pid %d, binary loaded at 0x%lx (a CRASH's offsets are from there)", (int)getpid(),
             (unsigned long)exe_base);
}

/* where the binary is mapped: the first mapping of /proc/self/exe */
static void find_base(void)
{
    char exe[512], line[700];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return;
    exe[n] = 0;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *p = strchr(line, '/'), *dash = strchr(line, '-');
        if (p && dash && !strcmp(p, exe)) {
            if (!exe_base)
                exe_base = (uintptr_t)strtoull(line, NULL, 16);
            exe_end = (uintptr_t)strtoull(dash + 1, NULL, 16);
        }
    }
    fclose(f);
}

/* ------------------------------------------------------------------ crashes */

static const char *volatile where_now = "starting";
static volatile sig_atomic_t quit_sig;

/* async-signal-safe pieces: no stdio, no malloc */
static void s_str(char *b, size_t *n, size_t cap, const char *s)
{
    while (*s && *n + 1 < cap)
        b[(*n)++] = *s++;
}
static void s_hex(char *b, size_t *n, size_t cap, uintptr_t v)
{
    char t[2 + 2 * sizeof v];
    int k = 0;
    do
        t[k++] = "0123456789abcdef"[v & 15];
    while ((v >>= 4) && k < (int)sizeof t);
    s_str(b, n, cap, "0x");
    while (k && *n + 1 < cap)
        b[(*n)++] = t[--k];
}
static void s_dec(char *b, size_t *n, size_t cap, unsigned long v)
{
    char t[24];
    int k = 0;
    do
        t[k++] = (char)('0' + v % 10);
    while ((v /= 10) && k < (int)sizeof t);
    while (k && *n + 1 < cap)
        b[(*n)++] = t[--k];
}
static void s_time(char *b, size_t *n, size_t cap)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000 - t_start;
    char t[16];
    size_t k = 0;
    s_dec(t, &k, sizeof t, (unsigned long)(us / 1000000));
    for (size_t pad = k; pad < 5; pad++)
        s_str(b, n, cap, " ");
    t[k] = 0;
    s_str(b, n, cap, t);
    s_str(b, n, cap, ".");
    unsigned long ms = (unsigned long)(us / 1000 % 1000);
    if (ms < 100)
        s_str(b, n, cap, "0");
    if (ms < 10)
        s_str(b, n, cap, "0");
    s_dec(b, n, cap, ms);
    s_str(b, n, cap, " ");
}

static const char *sig_name(int s)
{
    switch (s) {
    case SIGSEGV: return "SIGSEGV (a bad memory access)";
    case SIGBUS: return "SIGBUS (a bad memory access)";
    case SIGILL: return "SIGILL (an illegal instruction)";
    case SIGFPE: return "SIGFPE (an arithmetic fault)";
    case SIGABRT: return "SIGABRT (aborted)";
    case SIGTERM: return "SIGTERM";
    case SIGINT: return "SIGINT";
    case SIGHUP: return "SIGHUP";
    }
    return "signal";
}

static void on_crash(int sig, siginfo_t *si, void *ucv)
{
    char b[600];
    size_t n = 0;
    uintptr_t pc = 0;
#if defined(__aarch64__)
    pc = (uintptr_t)((ucontext_t *)ucv)->uc_mcontext.pc;
#elif defined(__x86_64__)
    pc = (uintptr_t)((ucontext_t *)ucv)->uc_mcontext.gregs[REG_RIP];
#endif
    char tname[20] = "?";
    prctl(PR_GET_NAME, tname, 0, 0, 0);
    s_time(b, &n, sizeof b);
    s_str(b, &n, sizeof b, "CRASH  ");
    s_str(b, &n, sizeof b, sig_name(sig));
    s_str(b, &n, sizeof b, " at address ");
    s_hex(b, &n, sizeof b, (uintptr_t)si->si_addr);
    s_str(b, &n, sizeof b, ", pc ");
    s_hex(b, &n, sizeof b, pc);
    if (exe_base && pc >= exe_base && pc < exe_end) {
        s_str(b, &n, sizeof b, " (binary + ");
        s_hex(b, &n, sizeof b, pc - exe_base);
        s_str(b, &n, sizeof b, ")");
    }
    s_str(b, &n, sizeof b, ", in thread '");
    s_str(b, &n, sizeof b, tname);
    s_str(b, &n, sizeof b, "' (tid ");
    s_dec(b, &n, sizeof b, (unsigned long)syscall(SYS_gettid));
    s_str(b, &n, sizeof b, "); the main thread was ");
    s_str(b, &n, sizeof b, (const char *)where_now);
    b[n++] = '\n';
    write_all(out_fd, b, n);
    n = 0;
    s_time(b, &n, sizeof b);
    s_str(b, &n, sizeof b, "CRASH  backtrace (offsets in the binary: addr2line -f -e conduit.debug <offset>):\n");
    write_all(out_fd, b, n);
    void *fr[48];
    int k = backtrace(fr, 48);
    backtrace_symbols_fd(fr, k, out_fd);
    for (int i = 0; i < k; i++) {
        uintptr_t a = (uintptr_t)fr[i];
        if (!exe_base || a < exe_base || a >= exe_end)
            continue;
        n = 0;
        s_str(b, &n, sizeof b, "         #");
        s_dec(b, &n, sizeof b, (unsigned long)i);
        s_str(b, &n, sizeof b, " binary + ");
        s_hex(b, &n, sizeof b, a - exe_base);
        b[n++] = '\n';
        write_all(out_fd, b, n);
    }
    fsync(out_fd);
    /* the handler was reset: crash for real, so launch.sh sees the signal */
    raise(sig);
}

static void on_quit(int sig)
{
    char b[160];
    size_t n = 0;
    quit_sig = sig;
    s_time(b, &n, sizeof b);
    s_str(b, &n, sizeof b, "SIGNAL ");
    s_str(b, &n, sizeof b, sig_name(sig));
    s_str(b, &n, sizeof b, ": closing (a second one ends it at once)\n");
    write_all(out_fd, b, n);
}

int perf_quit_requested(void) { return quit_sig != 0; }

static void handlers(void)
{
    static char alt[64 * 1024];
    stack_t ss = { .ss_sp = alt, .ss_size = sizeof alt };
    sigaltstack(&ss, NULL);
    /* load the unwinder now: backtrace's first call may allocate */
    void *fr[4];
    backtrace(fr, 4);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    int crash[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (size_t i = 0; i < sizeof crash / sizeof crash[0]; i++)
        sigaction(crash[i], &sa, NULL);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_quit;
    sa.sa_flags = SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    int quit[] = { SIGTERM, SIGINT, SIGHUP };
    for (size_t i = 0; i < sizeof quit / sizeof quit[0]; i++)
        sigaction(quit[i], &sa, NULL);
    signal(SIGPIPE, SIG_IGN); /* a dropped connection is an error, not the end of the app */
}

/* ------------------------------------------------------------------ stderr */

static int pipe_rd = -1;
static pthread_t rd_thread;
static int rd_running;

static void msg_line(char *s)
{
    if (!strncmp(s, "[conduit] ", 10))
        s += 10;
    if (*s)
        perf_log("MSG", "%s", s);
}

static void *reader(void *u)
{
    prctl(PR_SET_NAME, "conduit-log", 0, 0, 0);
    char acc[1024], buf[512];
    size_t an = 0;
    for (;;) {
        ssize_t r = read(pipe_rd, buf, sizeof buf);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == '\n' || an == sizeof acc - 1) {
                acc[an] = 0;
                msg_line(acc);
                an = 0;
                if (buf[i] == '\n')
                    continue;
            }
            acc[an++] = buf[i];
        }
    }
    if (an) {
        acc[an] = 0;
        msg_line(acc);
    }
    return NULL;
}

static void capture_stderr(void)
{
    int p[2];
    int fd = dup(2);
    if (fd < 0 || pipe(p))
        return;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    out_fd = fd;
    fflush(stderr);
    dup2(p[1], 2);
    close(p[1]);
    pipe_rd = p[0];
    fcntl(pipe_rd, F_SETFD, FD_CLOEXEC);
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (pthread_create(&rd_thread, NULL, reader, NULL) == 0)
        rd_running = 1;
    else { /* no thread: back as it was */
        dup2(out_fd, 2);
        close(pipe_rd);
        pipe_rd = -1;
    }
}

/* ------------------------------------------------------------------ audio */

/* written by the audio thread, taken (and reset) by the main thread */
static uint32_t a_calls, a_late, a_us_max, a_gap_max;
static uint64_t a_us_sum, a_frames;
/* the audio thread's own: the sound made against real time, for dropouts */
static int a_rate = 48000, a_bufsz = 512;
static int a_reset = 1;
static uint64_t a_clock0, a_made_us, a_prev;
static uint64_t a_last_cb;    /* the monitor's: when the output last called */
static int a_out_open;
static uint64_t a_open_at;

void perf_audio_open(int capture, int rate, int frames, int channels, const char *device)
{
    const char *dev = device && *device ? device : "the default device";
    if (!capture) {
        a_rate = rate > 0 ? rate : 48000;
        a_bufsz = frames;
        SET(a_reset, 1);
        SET(a_last_cb, 0);
        SET(a_open_at, perf_now_us());
        SET(a_out_open, 1);
    }
    perf_log("AUDIO", "%s %s: %d Hz, %d ch, %d frames a buffer (%.1f ms)", capture ? "input" : "output", dev, rate,
             channels, frames, rate > 0 ? frames * 1000.0 / rate : 0);
}

void perf_audio_closed(int capture)
{
    if (!capture)
        SET(a_out_open, 0);
    perf_log("AUDIO", "%s closed", capture ? "input" : "output");
}

/* the audio thread's id, for its switches in PROC; callbacks that came late
 * (over one and a half buffers after the last), dropout or not */
static volatile int a_tid;
static uint32_t a_slow;

uint64_t perf_audio_begin(int frames)
{
    uint64_t t = perf_now_us();
    SET(a_last_cb, t);
    if (!AT(a_tid))
        SET(a_tid, (int)syscall(SYS_gettid));
    uint64_t buf_us = (uint64_t)frames * 1000000ULL / (uint64_t)a_rate;
    if (AT(a_reset)) {
        SET(a_reset, 0);
        a_clock0 = t, a_made_us = 0, a_prev = 0;
    }
    else {
        /* the sound made so far against the time gone by: more than three
         * buffers behind, the device ran dry (a dropout); start counting again */
        if (t - a_clock0 > a_made_us + 3 * buf_us) {
            ADD(a_late, 1);
            a_clock0 = t - a_made_us;
        }
    }
    if (a_prev) {
        uint32_t gap = (uint32_t)(t - a_prev);
        if (gap * 2 > buf_us * 3)
            ADD(a_slow, 1);
        if (gap > AT(a_gap_max))
            SET(a_gap_max, gap);
    }
    a_prev = t;
    a_made_us += buf_us;
    ADD(a_calls, 1);
    ADD(a_frames, (uint64_t)frames);
    return t;
}

void perf_audio_end(uint64_t t0)
{
    uint32_t spent = (uint32_t)(perf_now_us() - t0);
    ADD(a_us_sum, (uint64_t)spent);
    if (spent > AT(a_us_max))
        SET(a_us_max, spent);
}

/* JUMP: a click is an isolated sharp turn in the output: its second
 * difference far above the sharpest of the last ~50 ms, and the sound just
 * after it smooth again (a step or a lone bad sample, as a gap or a cut
 * leaves). Checked across buffers too, where a gap would land. */
#define J_AFTER 72 /* samples looked at after a candidate: 1.5 ms at 48 kHz */
static float j_prev[2][2], j_pk = 1.f; /* the decaying peak (starts high: no jump at the very start) */
static float j_cand, j_cand_typ, j_after_sum;
static int j_after_left = -1, j_skip;
static uint32_t j_count, j_logged_at_s = UINT32_MAX;
static volatile float j_size, j_typ;
static volatile uint32_t j_when_ms, j_new;

static void jump_found(float size, float typ)
{
    ADD(j_count, 1);
    uint32_t ms = (uint32_t)((perf_now_us() - t_start) / 1000);
    if (ms / 1000 != j_logged_at_s) { /* at most one a second for the log */
        j_logged_at_s = ms / 1000;
        j_size = size, j_typ = typ, j_when_ms = ms;
        __atomic_store_n(&j_new, 1, __ATOMIC_RELEASE);
    }
}

void perf_audio_output(const float *s, int frames)
{
    float pk = j_pk;
    for (int i = 0; i < frames; i++) {
        float d = 0.f; /* the sharper channel's turn */
        for (int c = 0; c < 2; c++) {
            float x = s[i * 2 + c];
            x = x > 1.f ? 1.f : x < -1.f ? -1.f : x;
            float dc = fabsf(x - 2.f * j_prev[c][1] + j_prev[c][0]);
            j_prev[c][0] = j_prev[c][1], j_prev[c][1] = x;
            if (dc > d)
                d = dc;
        }
        if (j_after_left >= 0) { /* after a candidate: is the sound smooth again? */
            if (j_skip > 0)
                j_skip--; /* a step turns twice, the second the other way: not counted */
            else
                j_after_sum += d;
            if (--j_after_left < 0 && j_after_sum / (J_AFTER - 2) < j_cand * 0.06f)
                jump_found(j_cand, j_cand_typ);
        }
        else if (d > 0.2f && d > 4.f * pk) {
            j_cand = d, j_cand_typ = pk, j_after_sum = 0.f, j_after_left = J_AFTER, j_skip = 2;
        }
        pk = d > pk ? d : pk * 0.99971f; /* half its height in ~50 ms at 48 kHz */
    }
    j_pk = pk;
}

/* ------------------------------------------------------------------ the screen and the buttons */

static uint32_t s_frames;
static uint64_t s_us_sum, s_us_max, s_first;
static void (*before_stat)(void);
static uint64_t main_beat;    /* the main thread's last frame or tick, for STALL */
static uint64_t stall_since;  /* the monitor saw a stall (0: none) */

/* presses: since the last INPUT line */
static uint32_t i_presses, i_midi, i_lat_n;
static uint64_t i_lat_sum, i_lat_max;
static uint32_t i_pending_ms; /* a press waiting to be shown (SDL ticks + 1, 0: none) */
static uint64_t i_last_press; /* perf_now_us of the last press */
static uint64_t i_held_at[32];
static const char *i_name[32];
static uint64_t i_stuck_said[32];

void perf_before_stat(void (*fn)(void)) { before_stat = fn; }
void perf_where(const char *what) { where_now = what; }
void perf_midi_in(int n) { i_midi += (uint32_t)n; }

void perf_frame(uint64_t us, uint32_t now_ms)
{
    uint64_t now = perf_now_us();
    SET(main_beat, now);
    if (!s_first)
        s_first = now;
    s_frames++;
    s_us_sum += us;
    if (us > s_us_max)
        s_us_max = us;
    if (i_pending_ms) {
        uint32_t lat = now_ms - (i_pending_ms - 1);
        if (lat < 60000) {
            i_lat_n++;
            i_lat_sum += lat;
            if (lat > i_lat_max)
                i_lat_max = lat;
        }
        i_pending_ms = 0;
    }
}

void perf_input(int id, const char *name, int down, uint32_t ev_ms)
{
    uint64_t now = perf_now_us();
    if (id >= 0 && id < 32) {
        i_name[id] = name;
        if (down) {
            i_held_at[id] = now;
            i_stuck_said[id] = 0;
        }
        else if (i_held_at[id]) {
            if (level >= PERF_DETAIL)
                perf_log("KEY", "%s up after %.2f s", name, (now - i_held_at[id]) / 1e6);
            i_held_at[id] = 0;
        }
    }
    if (!down)
        return;
    i_presses++;
    i_last_press = now;
    if (!i_pending_ms)
        i_pending_ms = ev_ms + 1;
    if (level >= PERF_DETAIL)
        perf_log("KEY", "%s down", name);
}

static uint64_t held_longest(uint64_t now, const char **who)
{
    uint64_t m = 0;
    for (int i = 0; i < 32; i++)
        if (i_held_at[i] && now - i_held_at[i] > m) {
            m = now - i_held_at[i];
            *who = i_name[i];
        }
    return m;
}

/* ------------------------------------------------------------------ PROC */

static long ticks_hz;
#define NTHREAD 48
static struct {
    int tid;
    unsigned long ticks;
} th_prev[NTHREAD];
static int th_nprev;
static unsigned long p_prev_ticks, p_prev_minflt, p_prev_majflt, a_prev_invol, a_prev_vol;

static int read_stat(const char *path, char *name, int n, unsigned long *ticks, unsigned long *minflt,
                     unsigned long *majflt)
{
    char b[512];
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    size_t r = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[r] = 0;
    char *l = strchr(b, '('), *rp = strrchr(b, ')');
    if (!l || !rp)
        return 0;
    snprintf(name, (size_t)n, "%.*s", (int)(rp - l - 1), l + 1);
    unsigned long v[13];
    if (sscanf(rp + 2, "%*c %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu", &v[0], &v[1], &v[2], &v[3], &v[4],
               &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11]) < 12)
        return 0;
    if (minflt)
        *minflt = v[6];
    if (majflt)
        *majflt = v[8];
    *ticks = v[10] + v[11];
    return 1;
}

static long status_kb(const char *path, const char *key)
{
    char line[128];
    long v = -1;
    size_t k = strlen(key);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, k) && line[k] == ':') {
            v = atol(line + k + 1);
            break;
        }
    fclose(f);
    return v;
}

static void proc_line(double secs)
{
    if (secs <= 0)
        return;
    if (!ticks_hz)
        ticks_hz = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
    double per = 100.0 / (ticks_hz * secs);
    char name[32];
    unsigned long ticks, minflt = 0, majflt = 0;
    double app = -1;
    if (read_stat("/proc/self/stat", name, sizeof name, &ticks, &minflt, &majflt)) {
        if (p_prev_ticks)
            app = (ticks - p_prev_ticks) * per;
        p_prev_ticks = ticks;
    }
    struct {
        double pc;
        char name[24];
    } top[4] = {{-1, ""}, {-1, ""}, {-1, ""}, {-1, ""}};
    int nnew = 0;
    typeof(th_prev[0]) now_th[NTHREAD];
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    while (d && (e = readdir(d)) && nnew < NTHREAD) {
        int tid = atoi(e->d_name);
        if (tid <= 0)
            continue;
        char path[64];
        snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
        if (!read_stat(path, name, sizeof name, &ticks, NULL, NULL))
            continue;
        now_th[nnew].tid = tid, now_th[nnew].ticks = ticks, nnew++;
        for (int i = 0; i < th_nprev; i++)
            if (th_prev[i].tid == tid) {
                double pc = (ticks - th_prev[i].ticks) * per;
                for (int k = 0; k < 4; k++)
                    if (pc > top[k].pc) {
                        memmove(&top[k + 1], &top[k], sizeof top[0] * (size_t)(3 - k));
                        top[k].pc = pc;
                        snprintf(top[k].name, sizeof top[k].name, "%s%s", tid == AT(a_tid) ? "audio:" : "", name);
                        break;
                    }
            }
    }
    if (d)
        closedir(d);
    memcpy(th_prev, now_th, sizeof now_th[0] * (size_t)nnew);
    th_nprev = nnew;
    if (app < 0)
        return;
    char threads[160] = "";
    for (int k = 0; k < 4 && top[k].pc >= 0; k++)
        snprintf(threads + strlen(threads), sizeof threads - strlen(threads), "%s%s %.0f%%", k ? ", " : "",
                 top[k].name, top[k].pc);
    char sw[64] = "";
    if (AT(a_tid)) {
        char path[64];
        snprintf(path, sizeof path, "/proc/self/task/%d/status", AT(a_tid));
        long vol = status_kb(path, "voluntary_ctxt_switches"), inv = status_kb(path, "nonvoluntary_ctxt_switches");
        if (vol >= 0 && inv >= 0) {
            if (a_prev_vol)
                snprintf(sw, sizeof sw, " | audio thread waited %ld, preempted %ld", vol - (long)a_prev_vol,
                         inv - (long)a_prev_invol);
            a_prev_vol = (unsigned long)vol, a_prev_invol = (unsigned long)inv;
        }
    }
    long rss = status_kb("/proc/self/status", "VmRSS"), hwm = status_kb("/proc/self/status", "VmHWM");
    long avail = status_kb("/proc/meminfo", "MemAvailable");
    perf_log("PROC", "cpu %.0f%% (%s) | memory %.1f MB, peak %.1f | faults %lu major, %lu minor | free %ld MB%s", app,
             threads, rss / 1024.0, hwm / 1024.0, p_prev_majflt ? majflt - p_prev_majflt : 0,
             p_prev_minflt ? minflt - p_prev_minflt : 0, avail / 1024, sw);
    p_prev_minflt = minflt, p_prev_majflt = majflt;
}

/* ------------------------------------------------------------------ STAT */

/* the session's worst, for the END line */
static uint32_t w_late, w_gap, w_mhz_min, w_mhz_max, w_jumps, w_presses;
static double w_peak, w_load;
static long w_temp = -1;
static uint64_t w_frame, w_lat;

enum { H_LOAD, H_LATE, H_CLOCK, H_HOT, H_SCREEN, H_SLOWKEY, H_COUNT };
static uint64_t hint_at[H_COUNT];

static int hint_due(int h, uint64_t now)
{
    if (hint_at[h] && now - hint_at[h] < 60000000ULL)
        return 0;
    hint_at[h] = now;
    return 1;
}

static void input_line(uint64_t now, double secs)
{
    const char *who = "";
    uint64_t held = held_longest(now, &who);
    char lat[64] = "no presses shown";
    if (i_lat_n)
        snprintf(lat, sizeof lat, "press to screen avg %.0f ms max %llu ms", (double)i_lat_sum / i_lat_n,
                 (unsigned long long)i_lat_max);
    char idle[48] = "no press yet";
    if (i_last_press)
        snprintf(idle, sizeof idle, "last press %.0f s ago", (now - i_last_press) / 1e6);
    char hold[48] = "";
    if (held)
        snprintf(hold, sizeof hold, " | %s held %.1f s", who, held / 1e6);
    perf_log("INPUT", "presses %u (%.1f/s) | %s | %s%s | MIDI in %u", i_presses, secs > 0 ? i_presses / secs : 0, lat,
             idle, hold, i_midi);
    if (i_lat_max > w_lat)
        w_lat = i_lat_max;
    if (i_lat_n && (double)i_lat_sum / i_lat_n > 150 && hint_due(H_SLOWKEY, now))
        perf_log("HINT", "presses take %.0f ms to show on the screen: the main thread is busy (see PROC and STALL)",
                 (double)i_lat_sum / i_lat_n);
    w_presses += i_presses;
    i_presses = i_midi = i_lat_n = 0;
    i_lat_sum = i_lat_max = 0;
}

static void status(uint64_t now, double secs)
{
    uint32_t calls = TAKE(a_calls), late = TAKE(a_late), us_max = TAKE(a_us_max), gap = TAKE(a_gap_max);
    uint32_t slow = TAKE(a_slow), jumps = TAKE(j_count);
    uint64_t us_sum = TAKE(a_us_sum), frames = TAKE(a_frames);
    double made_us = frames * 1e6 / a_rate;
    double load = made_us > 0 ? 100.0 * us_sum / made_us : 0;
    double buf_us = calls ? made_us / calls : a_bufsz * 1e6 / a_rate;
    double peak = buf_us > 0 ? 100.0 * us_max / buf_us : 0;
    long khz = read_num(CPU "cpu0/cpufreq/scaling_cur_freq"), temp = temperature();
    char cores[16], loadavg[32];
    read_line(CPU "online", cores, sizeof cores);
    read_line("/proc/loadavg", loadavg, sizeof loadavg);
    loadavg[strcspn(loadavg, " ")] = 0;
    char cpu[48] = "cpu ?";
    if (khz > 0 || temp >= 0)
        snprintf(cpu, sizeof cpu, "cpu %ld MHz %ld C", khz > 0 ? khz / 1000 : 0, temp);
    double fsecs = s_first > now - (uint64_t)(secs * 1e6) ? (now - s_first) / 1e6 : secs;
    double fps = fsecs > 0.5 ? s_frames / fsecs : 0, frame_avg = s_frames ? s_us_sum / 1000.0 / s_frames : 0;
    if (before_stat)
        before_stat();

    perf_log("STAT",
             "audio load avg %.0f%% peak %.0f%% | dropouts %u, late calls %u, jumps %u | gap max %.1f ms (buffer %.1f) | "
             "%.0f calls/s | screen %.0f fps, avg %.1f ms max %.1f ms | %s cores %s load %s",
             load, peak, late, slow, jumps, gap / 1000.0, buf_us / 1000.0, secs > 0 ? calls / secs : 0, fps, frame_avg,
             s_us_max / 1000.0, cpu, cores[0] ? cores : "?", loadavg[0] ? loadavg : "?");
    proc_line(secs);
    input_line(now, secs);

    w_late += late;
    w_jumps += jumps;
    if (gap > w_gap)
        w_gap = gap;
    if (peak > w_peak)
        w_peak = peak;
    if (load > w_load)
        w_load = load;
    if (temp > w_temp)
        w_temp = temp;
    if (s_us_max > w_frame)
        w_frame = s_us_max;
    if (khz > 0) {
        if ((uint32_t)(khz / 1000) < w_mhz_min)
            w_mhz_min = (uint32_t)(khz / 1000);
        if ((uint32_t)(khz / 1000) > w_mhz_max)
            w_mhz_max = (uint32_t)(khz / 1000);
    }

    int slow_clock = khz > 0 && max_khz > 0 && khz < max_khz * 9 / 10;
    if (load > 60 && hint_due(H_LOAD, now))
        perf_log("HINT", "audio load %.0f%%: set %s_BUFFER=%d in launch.sh, or use fewer effects (Bloom adjustable, "
                         "master scatter)", load, prefix, a_bufsz * 2);
    if (late && hint_due(H_LATE, now))
        perf_log("HINT", "%u dropouts (crackles): set %s_BUFFER=%d in launch.sh%s%s", late, prefix, a_bufsz * 2,
                 load < 40 && !slow_clock ? "; the load is low, so something else held the sound up (see sysinfo.txt)"
                                          : "",
                 s_us_max > 100000 ? "; the screen stalled too" : "");
    if (slow_clock && hint_due(H_CLOCK, now))
        perf_log("HINT", "the processor runs at %ld of %ld MHz: the governor isn't on performance (launch.sh sets it)%s",
                 khz / 1000, max_khz / 1000, temp >= 75 ? ", or it is too hot" : "");
    if (temp >= 80 && hint_due(H_HOT, now))
        perf_log("HINT", "the processor is at %ld C: it slows down when hot; a case off or a cooler room helps", temp);
    if (s_frames && fps > 0 && fps < 20 && hint_due(H_SCREEN, now))
        perf_log("HINT", "the screen draws %.0f frames a second (avg %.1f ms a frame): the sound comes first, but "
                         "this may be the processor (see the cpu figures)",
                 fps, frame_avg);
    s_frames = 0, s_us_sum = s_us_max = 0;
}

void perf_tick(void)
{
    uint64_t now = perf_now_us();
    SET(main_beat, now);
    if (level == PERF_OFF)
        return;
    uint64_t st = AT(stall_since);
    if (st && __atomic_compare_exchange_n(&stall_since, &st, 0, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        perf_log("STALL", "over: the main thread was held up %.1f s", (now - st) / 1e6);
    if (__atomic_exchange_n(&j_new, 0, __ATOMIC_ACQUIRE))
        perf_log("JUMP", "at %.2f s the output jumped %.3f (typically %.4f): a click made inside the app",
                 j_when_ms / 1000.0, j_size, j_typ);
    /* a button held a minute: stuck, or something resting on it */
    for (int i = 0; i < 32; i++)
        if (i_held_at[i] && now - i_held_at[i] > 60000000ULL && !i_stuck_said[i]) {
            i_stuck_said[i] = now;
            perf_log("HINT", "%s has been held for a minute: stuck, or something resting on it?",
                     i_name[i] ? i_name[i] : "a button");
        }
    uint64_t every = level == PERF_DETAIL ? 1000000ULL : 10000000ULL;
    if (now - t_last_stat >= every) {
        status(now, (now - t_last_stat) / 1e6);
        t_last_stat = now;
    }
}

uint32_t perf_dropouts(void) { return w_late + AT(a_late); }
uint32_t perf_jumps(void) { return w_jumps + AT(j_count); }

/* ------------------------------------------------------------------ the monitor thread */

static pthread_t mon_thread;
static int mon_running, mon_stop;
static char watch_dir[512];
static pthread_mutex_t watch_mx = PTHREAD_MUTEX_INITIALIZER;

void perf_watch_dir(const char *dir)
{
    pthread_mutex_lock(&watch_mx);
    snprintf(watch_dir, sizeof watch_dir, "%s", dir ? dir : "");
    pthread_mutex_unlock(&watch_mx);
}

/* a list of names, kept to tell what came and went */
#define NLIST 40
typedef struct {
    char s[NLIST][120];
    int n, seen;
} List;

static int in_list(const List *l, const char *s)
{
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->s[i], s))
            return 1;
    return 0;
}

static void list_diff(List *old, List *now, const char *tag, const char *what)
{
    if (!old->seen) {
        char all[1000] = "";
        for (int i = 0; i < now->n; i++)
            snprintf(all + strlen(all), sizeof all - strlen(all), "%s%s", i ? "; " : "", now->s[i]);
        perf_log(tag, "%s at start: %s", what, now->n ? all : "none");
    }
    else {
        for (int i = 0; i < now->n; i++)
            if (!in_list(old, now->s[i]))
                perf_log(tag, "+ %s", now->s[i]);
        for (int i = 0; i < old->n; i++)
            if (!in_list(now, old->s[i]))
                perf_log(tag, "- %s", old->s[i]);
    }
    *old = *now;
    old->seen = 1;
}

static void add(List *l, const char *s)
{
    if (l->n < NLIST && !in_list(l, s))
        snprintf(l->s[l->n++], sizeof l->s[0], "%s", s);
}

static void scan_usb(List *l)
{
    l->n = 0;
    DIR *d = opendir("/sys/bus/usb/devices");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' || strchr(e->d_name, ':'))
            continue;
        char p[300], vid[16], pid[16], prod[64], man[64];
        snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/idVendor", e->d_name);
        read_line(p, vid, sizeof vid);
        if (!vid[0])
            continue;
        snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/idProduct", e->d_name);
        read_line(p, pid, sizeof pid);
        snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/product", e->d_name);
        read_line(p, prod, sizeof prod);
        snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/manufacturer", e->d_name);
        read_line(p, man, sizeof man);
        if (!strcmp(vid, "1d6b") && (!strcmp(pid, "0001") || !strcmp(pid, "0002") || !strcmp(pid, "0003")))
            continue; /* the root hubs */
        char s[160];
        snprintf(s, sizeof s, "%s:%s %s%s%s (port %s)", vid, pid, man, man[0] ? " " : "", prod[0] ? prod : "?",
                 e->d_name);
        add(l, s);
    }
    if (d)
        closedir(d);
}

static void scan_sound(List *l)
{
    l->n = 0;
    FILE *f = fopen("/proc/asound/cards", "r");
    char line[200];
    while (f && fgets(line, sizeof line, f)) {
        int idx;
        char *c = strstr(line, "]: ");
        if (sscanf(line, " %d [", &idx) == 1 && c) {
            c += 3;
            c[strcspn(c, "\n")] = 0;
            char s[160];
            snprintf(s, sizeof s, "card %d: %s", idx, c);
            add(l, s);
        }
    }
    if (f)
        fclose(f);
}

static void scan_pads(List *l)
{
    l->n = 0;
    FILE *f = fopen("/proc/bus/input/devices", "r");
    char line[200];
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "N: Name=\"", 9)) {
            char *s = line + 9;
            s[strcspn(s, "\"\n")] = 0;
            add(l, s);
        }
    if (f)
        fclose(f);
}

/* the battery and the charger, as one string; *charging, *pct for hints */
static void power(char *out, int n, int *charging, int *pct)
{
    out[0] = 0;
    *charging = -1, *pct = -1;
    DIR *d = opendir("/sys/class/power_supply");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char p[300], type[24];
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
        read_line(p, type, sizeof type);
        size_t k = strlen(out);
        if (!strcmp(type, "Battery")) {
            char st[24];
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
            long cap = read_num(p);
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", e->d_name);
            read_line(p, st, sizeof st);
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/voltage_now", e->d_name);
            long uv = read_num(p);
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/current_now", e->d_name);
            long ua = read_num(p);
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/temp", e->d_name);
            long t = read_num(p);
            snprintf(out + k, (size_t)n - k, "%sbattery %ld%% %s", k ? ", " : "", cap, st[0] ? st : "?");
            k = strlen(out);
            if (uv > 0)
                snprintf(out + k, (size_t)n - k, " %.2f V", uv / 1e6), k = strlen(out);
            if (ua != -1 && ua != 0)
                snprintf(out + k, (size_t)n - k, " %ld mA", ua / 1000), k = strlen(out);
            if (t > 0)
                snprintf(out + k, (size_t)n - k, " %.0f C", t / 10.0);
            *pct = (int)cap;
            *charging = !strcmp(st, "Charging") || !strcmp(st, "Full");
        }
        else if (type[0]) {
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/online", e->d_name);
            long on = read_num(p);
            if (on >= 0)
                snprintf(out + k, (size_t)n - k, "%s%s %s", k ? ", " : "", e->d_name, on ? "plugged in" : "unplugged");
        }
    }
    if (d)
        closedir(d);
    if (!out[0])
        snprintf(out, (size_t)n, "no battery information");
}

static void hw_line(void)
{
    char pw[200], th[300] = "";
    int chg, pct;
    power(pw, sizeof pw, &chg, &pct);
    for (int z = 0; z < 10; z++) {
        char p[64], type[32];
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/temp", z);
        long t = read_num(p);
        if (t < 0)
            continue;
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/type", z);
        read_line(p, type, sizeof type);
        snprintf(th + strlen(th), sizeof th - strlen(th), "%s%s %ld C", th[0] ? ", " : "", type[0] ? type : "zone",
                 t > 1000 ? t / 1000 : t);
    }
    char disk[96] = "";
    pthread_mutex_lock(&watch_mx);
    struct statvfs sv;
    if (watch_dir[0] && statvfs(watch_dir, &sv) == 0)
        snprintf(disk, sizeof disk, " | recordings: %.0f MB free of %.0f",
                 (double)sv.f_bavail * sv.f_frsize / 1048576.0, (double)sv.f_blocks * sv.f_frsize / 1048576.0);
    pthread_mutex_unlock(&watch_mx);
    char up[32] = "";
    read_line("/proc/uptime", up, sizeof up);
    up[strcspn(up, " ")] = 0;
    perf_log("HW", "%s | %s%s | up %s s", pw, th[0] ? th : "no thermal zones", disk, up[0] ? up : "?");
}

/* the time spent asleep since boot (µs): the boot clock runs on through a
 * suspend, the monotonic one doesn't. Read monotonic first, and never below
 * 0: awake, the two are the same but for the moment between the reads. */
static uint64_t boot_minus_mono(void)
{
    struct timespec b, m;
    clock_gettime(CLOCK_MONOTONIC, &m);
    clock_gettime(CLOCK_BOOTTIME, &b);
    int64_t d = ((int64_t)b.tv_sec - (int64_t)m.tv_sec) * 1000000LL + ((int64_t)b.tv_nsec - (int64_t)m.tv_nsec) / 1000;
    return d > 0 ? (uint64_t)d : 0;
}

enum { MH_BATT, MH_DISK, MH_COUNT };
static uint64_t mhint_at[MH_COUNT];

static void *monitor(void *u)
{
    prctl(PR_SET_NAME, "conduit-mon", 0, 0, 0);
    static List usb, snd, pads, tmp;
    uint64_t slept0 = boot_minus_mono();
    int round = 0, last_chg = -2, audio_stopped = 0;
    uint64_t audio_stop_at = 0;
    while (!AT(mon_stop)) {
        usleep(250000);
        if (++round % 4)
            continue;
        uint64_t now = perf_now_us();
        /* asleep: the monotonic clock stood still while the boot clock ran */
        uint64_t slept = boot_minus_mono();
        if (slept > slept0 && slept - slept0 > 2000000ULL) {
            perf_log("SLEEP", "the device was asleep for %.0f s", (slept - slept0) / 1e6);
            SET(main_beat, now);
        }
        slept0 = slept;
        /* the main thread held up */
        uint64_t beat = AT(main_beat);
        if (beat && now - beat > 2000000ULL && !AT(stall_since)) {
            SET(stall_since, beat);
            perf_log("STALL", "the screen hasn't updated for %.1f s; the main thread is %s", (now - beat) / 1e6,
                     (const char *)where_now);
        }
        /* the output stopped calling */
        uint64_t cb = AT(a_last_cb);
        if (AT(a_out_open)) {
            if (!cb && !audio_stopped && now - AT(a_open_at) > 3000000ULL) {
                audio_stopped = 1, audio_stop_at = AT(a_open_at);
                perf_log("AUDIO", "the output opened 3 s ago and hasn't asked for sound yet");
            }
            else if (cb && now - cb > 1000000ULL && !audio_stopped) {
                audio_stopped = 1, audio_stop_at = cb;
                perf_log("AUDIO", "the output stopped asking for sound (%.1f s ago): silence until it comes back",
                         (now - cb) / 1e6);
            }
            else if (cb && now - cb < 500000ULL && audio_stopped) {
                audio_stopped = 0;
                perf_log("AUDIO", "the output is back, after %.1f s", (cb - audio_stop_at) / 1e6);
            }
        }
        else
            audio_stopped = 0;
        if (round % 20 == 4) { /* every 5 s: what is plugged in, the charger */
            scan_usb(&tmp), list_diff(&usb, &tmp, "USB", "USB devices");
            scan_sound(&tmp), list_diff(&snd, &tmp, "SOUND", "sound cards");
            scan_pads(&tmp), list_diff(&pads, &tmp, "PAD", "input devices");
            char pw[200];
            int chg, pct;
            power(pw, sizeof pw, &chg, &pct);
            if (last_chg != -2 && chg != last_chg)
                hw_line();
            last_chg = chg;
            if (pct >= 0 && pct <= 10 && chg == 0 && (!mhint_at[MH_BATT] || now - mhint_at[MH_BATT] > 300000000ULL)) {
                mhint_at[MH_BATT] = now;
                perf_log("HINT", "the battery is at %d%%: save your work (SELECT + START); the sound may stop when "
                                 "it runs out", pct);
            }
        }
        if (round % 240 == 4) { /* every minute */
            hw_line();
            pthread_mutex_lock(&watch_mx);
            struct statvfs sv;
            int low = watch_dir[0] && statvfs(watch_dir, &sv) == 0 &&
                      (double)sv.f_bavail * sv.f_frsize < 256.0 * 1048576.0;
            pthread_mutex_unlock(&watch_mx);
            if (low && (!mhint_at[MH_DISK] || now - mhint_at[MH_DISK] > 600000000ULL)) {
                mhint_at[MH_DISK] = now;
                perf_log("HINT", "under 256 MB free where recordings go: saving may fail (delete some on FILES)");
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ start, end */

void perf_init(const char *pfx, const char *build)
{
    snprintf(prefix, sizeof prefix, "%s", pfx);
    build_name = build;
    char name[32];
    snprintf(name, sizeof name, "%s_LOG", prefix);
    const char *v = getenv(name);
    if (v && *v)
        level = atoi(v) <= 0 ? PERF_OFF : atoi(v) >= 2 ? PERF_DETAIL : PERF_BASIC;
    t_start = t_last_stat = perf_now_us();
    w_mhz_min = UINT32_MAX;
    find_base();
    if (level != PERF_OFF)
        capture_stderr();
    handlers();
    if (level == PERF_OFF)
        return;
    time_t wall = time(NULL);
    char when[64];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&wall));
    perf_log("LOG", "log %s, started %s (%s_LOG=0 off, 1 basic, 2 detail: a status line every second and every "
                    "press)", level == PERF_DETAIL ? "DETAIL" : "BASIC", when, prefix);
    system_line();
    if (pthread_create(&mon_thread, NULL, monitor, NULL) == 0)
        mon_running = 1;
}

void perf_close(const char *why)
{
    if (mon_running) {
        SET(mon_stop, 1);
        pthread_join(mon_thread, NULL);
        mon_running = 0;
    }
    /* the last of stderr first */
    if (rd_running) {
        fflush(stderr);
        dup2(out_fd, 2);
        /* the reader ends once the pipe is empty and closed; a child still holding stderr would keep it
         * open, so wait a second at most */
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 1;
        if (pthread_timedjoin_np(rd_thread, NULL, &until) == 0)
            close(pipe_rd);
        else
            pthread_detach(rd_thread);
        rd_running = 0;
    }
    if (level == PERF_OFF)
        return;
    uint64_t now = perf_now_us();
    status(now, (now - t_last_stat) / 1e6);
    hw_line();
    char cpu[64] = "";
    if (w_mhz_max)
        snprintf(cpu, sizeof cpu, ", cpu %u-%u MHz", w_mhz_min, w_mhz_max);
    if (w_temp >= 0)
        snprintf(cpu + strlen(cpu), sizeof cpu - strlen(cpu), ", max %ld C", w_temp);
    perf_log("END", "%s; session %.0f s: dropouts %u, jumps %u, audio load worst avg %.0f%% peak %.0f%%, gap max %.1f "
                    "ms, screen max %.1f ms, presses %u, press to screen max %llu ms%s",
             why, (now - t_start) / 1e6, w_late, w_jumps, w_load, w_peak, w_gap / 1000.0, w_frame / 1000.0, w_presses,
             (unsigned long long)w_lat, cpu);
    fsync(out_fd);
}
