/*
 * Conduit — USB audio sampler / looper for TrimUI Brick & Brick Pro.
 * SDL2 only (audio, video, input); runs on TrimUI OS, MustardOS and KNULLI.
 */
#include <SDL2/SDL.h>
#include "dsp.h"
#include "wav.h"
#include "font_data.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SW 1024
#define SH 768
/* the version: the default for plain builds; releases are built with VERSION=<tag> (see the Makefile) */
#ifndef CONDUIT_VERSION
#define CONDUIT_VERSION "0.1.0"
#endif
#define MAX_SECONDS 120
#define MAX_DEVS 32
#define MAX_FILES 512
#define ROWS_VISIBLE 8
#define WAVE_X 16
#define WAVE_Y 60
#define WAVE_W 992
#define WAVE_H 280

/* ------------------------------------------------------------ params */

enum { K_PCT, K_LOOPPOS, K_ST, K_CENTS, K_ENUM, K_DB, K_MS, K_INT, K_GAIN, K_DECAY };
typedef struct {
    const char *name; float min, max, step, coarse; int kind; const char *const *labels;
} ParamDesc;

static const char *const L_ONOFF[] = { "Off", "On" };
static const char *const L_FILT[] = { "Off", "Low-pass", "High-pass", "Band-pass" };
static const char *const L_SYNC[] = { "Free (ms)", "1/16 loop", "1/8 loop", "1/4 loop", "1/3 loop", "1/2 loop", "1 loop" };
static const char *const L_TRIG[] = { "Instant", "Threshold" };
static const char *const L_FMT[] = { "16-bit PCM", "32-bit float" };
static const char *const L_DECAY[] = { "While overdubbing", "Always (loop fades)" };

static const ParamDesc PD[P_COUNT] = {
    [P_LOOP_START] = { "Loop start", 0, 1, 0.002f, 0.02f, K_LOOPPOS, 0 },
    [P_LOOP_END]   = { "Loop end",   0, 1, 0.002f, 0.02f, K_LOOPPOS, 0 },
    [P_PITCH]      = { "Pitch",    -24, 24, 1, 12, K_ST, 0 },
    [P_FINE]       = { "Fine tune", -100, 100, 5, 25, K_CENTS, 0 },
    [P_REVERSE]    = { "Reverse", 0, 1, 1, 1, K_ENUM, L_ONOFF },
    [P_DECAY]      = { "Loop decay", 0, 0.5f, 0.005f, 0.05f, K_DECAY, 0 },
    [P_DECAY_MODE] = { "Decay", 0, 1, 1, 1, K_ENUM, L_DECAY },
    [P_VOLUME]     = { "Loop volume", 0, 1.5f, 0.05f, 0.25f, K_PCT, 0 },
    [P_MONITOR]    = { "Input monitor", 0, 1, 0.05f, 0.25f, K_PCT, 0 },
    [P_FILT_MODE]  = { "Filter", 0, 3, 1, 1, K_ENUM, L_FILT },
    [P_CUTOFF]     = { "Cutoff", 0, 1, 0.01f, 0.1f, K_PCT, 0 },
    [P_RESO]       = { "Resonance", 0, 0.97f, 0.03f, 0.15f, K_PCT, 0 },
    [P_DRIVE]      = { "Drive", 0, 1, 0.05f, 0.2f, K_PCT, 0 },
    [P_BITS]       = { "Bit depth", 1, 16, 1, 4, K_INT, 0 },
    [P_DOWNS]      = { "Downsample", 1, 32, 1, 4, K_INT, 0 },
    [P_DLY_SYNC]   = { "Delay sync", 0, 6, 1, 1, K_ENUM, L_SYNC },
    [P_DLY_TIME]   = { "Delay time", 10, 1900, 10, 100, K_MS, 0 },
    [P_DLY_FB]     = { "Delay feedback", 0, 0.95f, 0.05f, 0.2f, K_PCT, 0 },
    [P_DLY_MIX]    = { "Delay mix", 0, 1, 0.05f, 0.25f, K_PCT, 0 },
    [P_PINGPONG]   = { "Ping-pong", 0, 1, 1, 1, K_ENUM, L_ONOFF },
    [P_IN_GAIN]    = { "Input gain", -12, 24, 1, 6, K_GAIN, 0 },
    [P_TRIGGER]    = { "Rec trigger", 0, 1, 1, 1, K_ENUM, L_TRIG },
    [P_THRESH]     = { "Threshold", -60, -6, 1, 6, K_DB, 0 },
    [P_AUTOPLAY]   = { "Loop after rec", 0, 1, 1, 1, K_ENUM, L_ONOFF },
    [P_SAVE_FMT]   = { "Save format", 0, 1, 1, 1, K_ENUM, L_FMT },
    [P_DUB_ALIGN]  = { "Dub align", -60, 120, 1, 10, K_MS, 0 },
};

enum { ROW_IN_DEV = -1, ROW_OUT_DEV = -2, ROW_FOLDER = -3, ROW_DIAG = -4, ROW_END = -100 };
enum { PG_LOOP, PG_FX, PG_IO, PG_FILES, PG_N };
static const char *const PAGE_NAME[PG_N] = { "LOOP", "FX", "IO", "FILES" };
static const int PAGE_ROWS[PG_N][14] = {
    { P_LOOP_START, P_LOOP_END, P_PITCH, P_FINE, P_REVERSE, P_DECAY, P_DECAY_MODE, P_VOLUME, P_MONITOR, ROW_END },
    { P_FILT_MODE, P_CUTOFF, P_RESO, P_DRIVE, P_BITS, P_DOWNS, P_DLY_SYNC, P_DLY_TIME, P_DLY_FB, P_DLY_MIX, P_PINGPONG, ROW_END },
    { ROW_IN_DEV, ROW_OUT_DEV, P_IN_GAIN, P_MONITOR, P_TRIGGER, P_THRESH, P_AUTOPLAY, P_SAVE_FMT, P_DUB_ALIGN, ROW_FOLDER, ROW_DIAG, ROW_END },
    { ROW_END },
};
static int page_count(int pg) { int n = 0; while (PAGE_ROWS[pg][n] != ROW_END) n++; return n; }

/* ------------------------------------------------------------- input */

enum { A_UP, A_DOWN, A_LEFT, A_RIGHT, A_A, A_B, A_X, A_Y, A_L1, A_R1, A_L2, A_R2,
       A_SEL, A_START, A_MENU, A_N };
static const char *const ACT_KEY[A_N] = { "up", "down", "left", "right", "a", "b", "x", "y",
                                          "l1", "r1", "l2", "r2", "select", "start", "menu" };

/* ---------------------------------------------------------------- app */

typedef struct {
    Engine eng;
    SDL_Window *win; SDL_Renderer *ren; SDL_Texture *font;
    SDL_AudioDeviceID out_dev, in_dev;
    char in_name[256], out_name[256];      /* "" = system default */
    char in_open[256];                      /* what is actually open */
    int in_failed, out_block;
    int b_erased;                           /* B held: the loop was erased */

    int page, sel[PG_N], scroll[PG_N];
    char files[MAX_FILES][256]; long fsize[MAX_FILES]; int nfiles;
    int del_confirm; Uint32 confirm_until; int clear_confirm;

    char toast[200]; Uint32 toast_until; int toast_bad;
    char rec_dir[512], cfg_path[512];

    float peak[WAVE_W]; unsigned pk_version; int pk_len; Uint32 pk_time;

    int held[A_N]; Uint32 next_rep[A_N], held_since[A_N];
    int joy_map[A_N];   /* raw joystick button index, -1 = none */
    int have_gc, swap_ab, last_raw_btn, axis_held[2];
    int running;
} App;

static App G;

static void toast(int bad, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void toast(int bad, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vsnprintf(G.toast, sizeof G.toast, fmt, ap);
    va_end(ap);
    G.toast_until = SDL_GetTicks() + 2600; G.toast_bad = bad;
    fprintf(stderr, "[conduit] %s\n", G.toast);
}

/* --------------------------------------------------------- utilities */

static void mkdirs(const char *path)
{
    char t[512]; snprintf(t, sizeof t, "%s", path);
    for (char *p = t + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(t, 0775); *p = '/'; }
    mkdir(t, 0775);
}

static int ci_contains(const char *s, const char *needle)
{
    size_t n = strlen(needle);
    for (; s && *s; s++) if (!strncasecmp(s, needle, n)) return 1;
    return 0;
}

static void stamp_name(char *out, size_t sz, const char *kind)
{
    char ts[32]; time_t t = time(NULL); struct tm tmv; localtime_r(&t, &tmv);
    strftime(ts, sizeof ts, "%Y%m%d_%H%M%S", &tmv);
    snprintf(out, sz, "%s/conduit_%s_%s.wav", G.rec_dir, kind, ts);
    struct stat st;
    for (int i = 2; stat(out, &st) == 0 && i < 100; i++)
        snprintf(out, sz, "%s/conduit_%s_%s_%d.wav", G.rec_dir, kind, ts, i);
}

typedef struct { char name[256]; long size; time_t mtime; } FileEnt;
static int cmp_newest(const void *a, const void *b)
{
    const FileEnt *x = a, *y = b;
    if (x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;
    return strcmp(y->name, x->name);
}

/* newest first */
static void scan_files(void)
{
    static FileEnt ents[MAX_FILES];
    int n = 0;
    G.nfiles = 0;
    DIR *d = opendir(G.rec_dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && n < MAX_FILES) {
        size_t l = strlen(de->d_name);
        if (l < 5 || l > 250 || de->d_name[0] == '.' || strcasecmp(de->d_name + l - 4, ".wav")) continue;
        char p[800]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", G.rec_dir, de->d_name);
        if (stat(p, &st)) continue;
        snprintf(ents[n].name, 256, "%s", de->d_name);
        ents[n].size = (long)st.st_size; ents[n].mtime = st.st_mtime;
        n++;
    }
    closedir(d);
    qsort(ents, n, sizeof ents[0], cmp_newest);
    for (int i = 0; i < n; i++) { memcpy(G.files[i], ents[i].name, 256); G.fsize[i] = ents[i].size; }
    G.nfiles = n;
    if (G.sel[PG_FILES] >= G.nfiles) G.sel[PG_FILES] = G.nfiles ? G.nfiles - 1 : 0;
}

/* ------------------------------------------------------------ config */

static const int PERSIST[] = { P_DECAY, P_DECAY_MODE, P_VOLUME, P_MONITOR, P_IN_GAIN, P_TRIGGER,
                               P_THRESH, P_AUTOPLAY, P_SAVE_FMT, P_DUB_ALIGN };
static const char *const PERSIST_KEY[] = { "loop_decay", "decay_mode", "volume", "monitor", "in_gain",
                                           "trigger", "threshold", "autoplay", "save_format", "dub_align_ms" };
#define NPERSIST (int)(sizeof PERSIST / sizeof PERSIST[0])

static void load_cfg(void)
{
    /* defaults for the TrimUI "Player1" evdev joystick when SDL has no
     * GameController mapping; override with joy_<action>= in conduit.cfg */
    static const int def[A_N] = { -1, -1, -1, -1, 1, 0, 3, 2, 4, 5, -1, -1, 6, 7, 8 };
    memcpy(G.joy_map, def, sizeof def);
    FILE *f = fopen(G.cfg_path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0; char *k = line, *v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        if (!strcmp(k, "in_device")) snprintf(G.in_name, sizeof G.in_name, "%s", v);
        else if (!strcmp(k, "out_device")) snprintf(G.out_name, sizeof G.out_name, "%s", v);
        else if (!strcmp(k, "swap_ab")) G.swap_ab = atoi(v);
        else if (!strncmp(k, "joy_", 4)) {
            for (int a = 0; a < A_N; a++) if (!strcmp(k + 4, ACT_KEY[a])) G.joy_map[a] = atoi(v);
        } else
            for (int i = 0; i < NPERSIST; i++)
                if (!strcmp(k, PERSIST_KEY[i])) {
                    float x = (float)atof(v); const ParamDesc *d = &PD[PERSIST[i]];
                    G.eng.p[PERSIST[i]] = x < d->min ? d->min : x > d->max ? d->max : x;
                }
    }
    fclose(f);
}

static void save_cfg(void)
{
    FILE *f = fopen(G.cfg_path, "w");
    if (!f) return;
    fprintf(f, "# Conduit settings (edit while the app is closed)\n");
    fprintf(f, "in_device=%s\nout_device=%s\nswap_ab=%d\n", G.in_name, G.out_name, G.swap_ab);
    for (int i = 0; i < NPERSIST; i++) fprintf(f, "%s=%g\n", PERSIST_KEY[i], G.eng.p[PERSIST[i]]);
    fprintf(f, "# raw joystick buttons (used only without an SDL controller mapping)\n");
    for (int a = 0; a < A_N; a++) fprintf(f, "joy_%s=%d\n", ACT_KEY[a], G.joy_map[a]);
    fclose(f);
}

/* ------------------------------------------------------------- audio */

/* how far behind the playhead an overdub lands (besides the input FIFO,
 * which the engine adds): the output's buffer (SDL's block and the driver's,
 * about two blocks) and the capture block */
static void set_latency(void)
{
    if (G.out_dev) SDL_LockAudioDevice(G.out_dev);
    G.eng.lat_frames = 2 * (G.out_block ? G.out_block : 512) + G.eng.in_block;
    if (G.out_dev) SDL_UnlockAudioDevice(G.out_dev);
}

static void out_cb(void *u, Uint8 *s, int len) { eng_render((Engine *)u, (float *)s, len / 8); }
static void in_cb(void *u, Uint8 *s, int len) { eng_push_input((Engine *)u, (const float *)s, len / 8); }

static SDL_AudioSpec want_spec(int capture)
{
    SDL_AudioSpec w; memset(&w, 0, sizeof w);
    w.freq = CD_SR; w.format = AUDIO_F32SYS; w.channels = 2; w.samples = 512;
    w.callback = capture ? in_cb : out_cb; w.userdata = &G.eng;
    return w;
}

static int open_output(void)
{
    if (G.out_dev) SDL_CloseAudioDevice(G.out_dev);
    SDL_AudioSpec w = want_spec(0), got;
    G.out_dev = SDL_OpenAudioDevice(G.out_name[0] ? G.out_name : NULL, 0, &w, &got, 0);
    if (!G.out_dev && G.out_name[0]) {
        toast(1, "Output '%s' unavailable, using default", G.out_name);
        G.out_dev = SDL_OpenAudioDevice(NULL, 0, &w, &got, 0);
    }
    if (!G.out_dev) { fprintf(stderr, "no audio output: %s\n", SDL_GetError()); return -1; }
    G.out_block = got.samples;
    set_latency();
    SDL_PauseAudioDevice(G.out_dev, 0);
    return 0;
}

/* pick the device to open: explicit name, else first USB capture device */
static const char *choose_input(void)
{
    if (G.in_name[0]) return G.in_name;
    int n = SDL_GetNumAudioDevices(1);
    for (int i = 0; i < n; i++) {
        const char *nm = SDL_GetAudioDeviceName(i, 1);
        if (ci_contains(nm, "usb")) return nm;
    }
    return NULL;
}

static void open_input(int announce)
{
    if (G.in_dev) { SDL_CloseAudioDevice(G.in_dev); G.in_dev = 0; }
    const char *want = choose_input();
    SDL_AudioSpec w = want_spec(1), got;
    G.in_dev = SDL_OpenAudioDevice(want, 1, &w, &got, 0);
    if (!G.in_dev && want) {
        if (announce) toast(1, "Couldn't open '%s', trying default", want);
        want = NULL;
        G.in_dev = SDL_OpenAudioDevice(NULL, 1, &w, &got, 0);
    }
    G.in_failed = !G.in_dev;
    snprintf(G.in_open, sizeof G.in_open, "%s", G.in_dev ? (want ? want : "System default") : "None");
    if (G.in_dev) {
        SDL_LockAudioDevice(G.out_dev);
        G.eng.in_block = got.samples;
        SDL_UnlockAudioDevice(G.out_dev);
        set_latency();
        SDL_PauseAudioDevice(G.in_dev, 0);
        if (announce) toast(0, "Input: %s", G.in_open);
    } else if (announce) toast(1, "No capture device: %s", SDL_GetError());
}

/* cycle through [default, dev0..devN-1] */
static void cycle_device(int capture, int dir)
{
    char *cur = capture ? G.in_name : G.out_name;
    int n = SDL_GetNumAudioDevices(capture), idx = 0;
    if (n > MAX_DEVS) n = MAX_DEVS;
    for (int i = 0; i < n; i++)
        if (cur[0] && !strcmp(cur, SDL_GetAudioDeviceName(i, capture))) idx = i + 1;
    idx = (idx + dir + n + 1) % (n + 1);
    if (idx == 0) cur[0] = 0;
    else snprintf(cur, 256, "%s", SDL_GetAudioDeviceName(idx - 1, capture));
    if (capture) open_input(1);
    else { open_output(); toast(0, "Output: %s", cur[0] ? cur : "System default"); }
}

/* ----------------------------------------------------------- actions */

static void begin_busy(void) { G.eng.overdub = 0; G.eng.busy = 1; SDL_Delay(25); }
static void end_busy(void) { G.eng.busy = 0; }
static void draw(void);

static void save_take(void)
{
    if (G.eng.rec_state == ST_REC) { toast(1, "Stop recording first (A)"); return; }
    if (G.eng.len < 2) { toast(1, "Nothing recorded yet"); return; }
    char path[600]; stamp_name(path, sizeof path, "take");
    begin_busy();
    int ls, le; eng_loop_range(&G.eng, &ls, &le);
    int bits = G.eng.p[P_SAVE_FMT] > 0.5f ? 32 : 16;
    int r = wav_write(path, G.eng.buf + 2 * (size_t)ls, le - ls, CD_SR, bits);
    end_busy();
    if (r) toast(1, "Save failed: %s", strerror(errno));
    else { toast(0, "Saved %s", strrchr(path, '/') + 1); scan_files(); }
}

static float *render_bounce(int *n)
{
    snprintf(G.toast, sizeof G.toast, "Rendering...");
    G.toast_until = SDL_GetTicks() + 5000; G.toast_bad = 0;
    if (G.ren) draw();
    begin_busy();
    float *b = eng_bounce(&G.eng, n);
    end_busy();
    return b;
}

static void save_bounce(void)
{
    if (G.eng.len < 2 || G.eng.rec_state == ST_REC) { toast(1, "Nothing to bounce"); return; }
    int n; float *b = render_bounce(&n);
    if (!b) { toast(1, "Bounce failed (out of memory?)"); return; }
    char path[600]; stamp_name(path, sizeof path, "bounce");
    int r = wav_write(path, b, n, CD_SR, G.eng.p[P_SAVE_FMT] > 0.5f ? 32 : 16);
    free(b);
    if (r) toast(1, "Save failed: %s", strerror(errno));
    else { toast(0, "Bounced %s", strrchr(path, '/') + 1); scan_files(); }
}

static void resample(void)
{
    if (G.eng.len < 2 || G.eng.rec_state == ST_REC) { toast(1, "Nothing to resample"); return; }
    int n; float *b = render_bounce(&n);
    if (!b) { toast(1, "Resample failed (out of memory?)"); return; }
    SDL_LockAudioDevice(G.out_dev);
    eng_set_buffer(&G.eng, b, n);
    eng_reset_fx_params(G.eng.p);
    G.eng.p[P_PITCH] = 0; G.eng.p[P_FINE] = 0; G.eng.p[P_REVERSE] = 0;
    fx_reset(&G.eng.fx, G.eng.p);
    G.eng.playing = 1;
    SDL_UnlockAudioDevice(G.out_dev);
    free(b);
    toast(0, "Resampled into buffer (FX + pitch reset)");
}

static void load_file(int i)
{
    if (i < 0 || i >= G.nfiles) return;
    char path[800]; snprintf(path, sizeof path, "%s/%s", G.rec_dir, G.files[i]);
    int n; float *b = wav_read(path, CD_SR, G.eng.cap, &n);
    if (!b) { toast(1, "Can't read %s", G.files[i]); return; }
    SDL_LockAudioDevice(G.out_dev);
    eng_set_buffer(&G.eng, b, n);
    SDL_UnlockAudioDevice(G.out_dev);
    free(b);
    eng_play_toggle(&G.eng);
    toast(0, "Loaded %s (%.1fs)", G.files[i], n / (double)CD_SR);
}

static void adjust(int id, int dir)
{
    const ParamDesc *d = &PD[id];
    float step = G.held[A_R2] ? d->coarse : d->step;
    if (G.held[A_L2] && d->kind != K_ENUM && d->kind != K_INT) step *= 0.1f;
    float *p = G.eng.p, v = p[id] + dir * step;
    if (d->kind == K_ENUM) {
        int n = (int)d->max + 1;
        v = (float)(((int)p[id] + dir + n) % n);
    }
    v = v < d->min ? d->min : v > d->max ? d->max : v;
    if (id == P_LOOP_START && v > p[P_LOOP_END] - 0.001f) v = p[P_LOOP_END] - 0.001f;
    if (id == P_LOOP_END && v < p[P_LOOP_START] + 0.001f) v = p[P_LOOP_START] + 0.001f;
    p[id] = v < d->min ? d->min : v;
}

static void move_sel(int dir)
{
    int n = G.page == PG_FILES ? G.nfiles : page_count(G.page);
    if (n <= 0) return;
    int s = G.sel[G.page] + dir;
    s = s < 0 ? n - 1 : s >= n ? 0 : s;
    G.sel[G.page] = s;
    if (s < G.scroll[G.page]) G.scroll[G.page] = s;
    if (s >= G.scroll[G.page] + ROWS_VISIBLE) G.scroll[G.page] = s - ROWS_VISIBLE + 1;
    G.del_confirm = 0;
}

static void press(int a)
{
    Uint32 now = SDL_GetTicks();
    if (now > G.confirm_until) { G.del_confirm = 0; G.clear_confirm = 0; }

    if (G.held[A_SEL] && a != A_SEL) {
        switch (a) {
        case A_START: save_bounce(); break;
        case A_Y: resample(); break;
        case A_B:
            SDL_LockAudioDevice(G.out_dev);
            { int k = eng_undo(&G.eng); SDL_UnlockAudioDevice(G.out_dev);
              int left = eng_undo_count(&G.eng);
              if (!k) toast(1, "Nothing to undo");
              else toast(0, "%s (%d more undo%s)", k == UNDO_ERASE ? "Loop back (stopped): B plays it" : "Undid overdub",
                         left, left == 1 ? "" : "s"); }
            break;
        case A_X:
            if (!G.clear_confirm) { G.clear_confirm = 1; G.confirm_until = now + 2500; toast(1, "Press SELECT+X again to clear the buffer"); }
            else { eng_erase(&G.eng); G.clear_confirm = 0; toast(0, "Loop erased"); }
            break;
        case A_A: save_take(); break;
        case A_MENU: G.running = 0; break;
        case A_L1: case A_R1:
            if (G.held[A_L1] && G.held[A_R1]) G.running = 0;
            break;
        }
        return;
    }

    int row = G.page == PG_FILES ? 0 : PAGE_ROWS[G.page][G.sel[G.page]];
    switch (a) {
    case A_MENU: G.running = 0; break;
    case A_L1: G.page = (G.page + PG_N - 1) % PG_N; if (G.page == PG_FILES) scan_files(); break;
    case A_R1: G.page = (G.page + 1) % PG_N; if (G.page == PG_FILES) scan_files(); break;
    case A_UP: move_sel(-1); break;
    case A_DOWN: move_sel(1); break;
    case A_LEFT: case A_RIGHT: {
        int dir = a == A_RIGHT ? 1 : -1;
        if (G.page == PG_FILES) { move_sel(dir * ROWS_VISIBLE); break; }
        if (row >= 0) adjust(row, dir);
        else if (row == ROW_IN_DEV) cycle_device(1, dir);
        else if (row == ROW_OUT_DEV) cycle_device(0, dir);
        break;
    }
    case A_A:
        if (G.page == PG_FILES) load_file(G.sel[PG_FILES]);
        else eng_rec_button(&G.eng);
        break;
    case A_B: G.b_erased = 0; eng_play_toggle(&G.eng); break;
    case A_X: eng_overdub_toggle(&G.eng); break;
    case A_Y:
        if (G.page == PG_FILES) {
            if (!G.nfiles) break;
            if (!G.del_confirm) { G.del_confirm = 1; G.confirm_until = now + 2500; toast(1, "Press Y again to delete %s", G.files[G.sel[PG_FILES]]); }
            else {
                char p[800]; snprintf(p, sizeof p, "%s/%s", G.rec_dir, G.files[G.sel[PG_FILES]]);
                if (remove(p) == 0) { sync(); toast(0, "Deleted"); } else toast(1, "Delete failed");
                G.del_confirm = 0; scan_files();
            }
        } else G.eng.p[P_REVERSE] = G.eng.p[P_REVERSE] > 0.5f ? 0.f : 1.f;
        break;
    case A_START: save_take(); break;
    }
}

static void set_held(int a, int down)
{
    if (a < 0 || a >= A_N) return;
    if (down && !G.held[a]) {
        G.held[a] = 1;
        G.held_since[a] = SDL_GetTicks();
        G.next_rep[a] = G.held_since[a] + 320;
        press(a);
    } else if (!down) G.held[a] = 0;
}

static void repeat_tick(void)
{
    Uint32 now = SDL_GetTicks();
    /* hold B for a second: stop and erase the loop (no new recording) */
    if (G.held[A_B] && !G.held[A_SEL] && !G.b_erased && G.page != PG_FILES && now - G.held_since[A_B] >= 1000) {
        G.b_erased = 1;
        if (G.eng.len > 0 || G.eng.rec_state != ST_IDLE) { eng_erase(&G.eng); toast(0, "Loop erased"); }
    }
    for (int a = A_UP; a <= A_RIGHT; a++)
        if (G.held[a] && now >= G.next_rep[a]) {
            press(a);
            G.next_rep[a] = now + (now - G.held_since[a] > 1500 ? 25 : 65);
        }
}

static int gc_action(int b)
{
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return G.swap_ab ? A_B : A_A;
    case SDL_CONTROLLER_BUTTON_B: return G.swap_ab ? A_A : A_B;
    case SDL_CONTROLLER_BUTTON_X: return G.swap_ab ? A_Y : A_X;
    case SDL_CONTROLLER_BUTTON_Y: return G.swap_ab ? A_X : A_Y;
    case SDL_CONTROLLER_BUTTON_BACK: return A_SEL;
    case SDL_CONTROLLER_BUTTON_GUIDE: return A_MENU;
    case SDL_CONTROLLER_BUTTON_START: return A_START;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return A_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return A_R1;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return A_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return A_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return A_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return A_RIGHT;
    }
    return -1;
}

static int key_action(int k)
{
    switch (k) {
    case SDLK_UP: return A_UP; case SDLK_DOWN: return A_DOWN;
    case SDLK_LEFT: return A_LEFT; case SDLK_RIGHT: return A_RIGHT;
    case 'x': return A_A; case 'z': return A_B; case 's': return A_X; case 'a': return A_Y;
    case 'q': return A_L1; case 'w': return A_R1; case '1': return A_L2; case '2': return A_R2;
    case SDLK_RSHIFT: case SDLK_BACKSPACE: return A_SEL;
    case SDLK_RETURN: return A_START; case SDLK_ESCAPE: return A_MENU;
    }
    return -1;
}

static void open_pads(void)
{
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) { if (SDL_GameControllerOpen(i)) G.have_gc = 1; }
        else SDL_JoystickOpen(i);
        fprintf(stderr, "[conduit] pad %d: %s (%s)\n", i, SDL_JoystickNameForIndex(i),
                SDL_IsGameController(i) ? "controller" : "raw joystick");
    }
}

static void handle_event(SDL_Event *e)
{
    switch (e->type) {
    case SDL_QUIT: G.running = 0; break;
    case SDL_KEYDOWN: if (!e->key.repeat) set_held(key_action(e->key.keysym.sym), 1); break;
    case SDL_KEYUP: set_held(key_action(e->key.keysym.sym), 0); break;
    case SDL_CONTROLLERBUTTONDOWN: set_held(gc_action(e->cbutton.button), 1); break;
    case SDL_CONTROLLERBUTTONUP: set_held(gc_action(e->cbutton.button), 0); break;
    case SDL_CONTROLLERAXISMOTION:
        if (e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) set_held(A_L2, e->caxis.value > 16000);
        if (e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) set_held(A_R2, e->caxis.value > 16000);
        break;
    case SDL_CONTROLLERDEVICEADDED: case SDL_JOYDEVICEADDED: break;
    case SDL_JOYBUTTONDOWN: case SDL_JOYBUTTONUP:
        G.last_raw_btn = e->jbutton.button;
        if (G.have_gc) break;
        for (int a = 0; a < A_N; a++)
            if (G.joy_map[a] == e->jbutton.button) set_held(a, e->type == SDL_JOYBUTTONDOWN);
        break;
    case SDL_JOYHATMOTION:
        if (G.have_gc) break;
        set_held(A_UP, e->jhat.value & SDL_HAT_UP); set_held(A_DOWN, e->jhat.value & SDL_HAT_DOWN);
        set_held(A_LEFT, e->jhat.value & SDL_HAT_LEFT); set_held(A_RIGHT, e->jhat.value & SDL_HAT_RIGHT);
        break;
    case SDL_JOYAXISMOTION:
        if (G.have_gc) break;
        if (e->jaxis.axis == 0) { set_held(A_LEFT, e->jaxis.value < -16000); set_held(A_RIGHT, e->jaxis.value > 16000); }
        if (e->jaxis.axis == 1) { set_held(A_UP, e->jaxis.value < -16000); set_held(A_DOWN, e->jaxis.value > 16000); }
        if (e->jaxis.axis == 2) set_held(A_L2, e->jaxis.value > 0);
        if (e->jaxis.axis == 5) set_held(A_R2, e->jaxis.value > 0);
        break;
    case SDL_AUDIODEVICEADDED:
        if (e->adevice.iscapture && !G.in_name[0]) {
            const char *nm = SDL_GetAudioDeviceName((int)e->adevice.which, 1);
            if ((ci_contains(nm, "usb") && !ci_contains(G.in_open, "usb")) || !G.in_dev) open_input(1);
        }
        break;
    case SDL_AUDIODEVICEREMOVED:
        if (G.in_dev && e->adevice.which == G.in_dev) {
            SDL_CloseAudioDevice(G.in_dev); G.in_dev = 0;
            toast(1, "Input disconnected");
            open_input(0);
        }
        if (G.out_dev && e->adevice.which == G.out_dev) { G.out_name[0] = 0; open_output(); }
        break;
    }
}

/* ----------------------------------------------------------- drawing */

#define C_BG 0x0d1015
#define C_PANEL 0x161b23
#define C_PANEL2 0x1f2631
#define C_TEXT 0xe8e6e1
#define C_DIM 0x8a919c
#define C_ACCENT 0xe3b341
#define C_WAVE 0x63c7ff
#define C_REC 0xff5252
#define C_PLAY 0x3ddc84
#define C_DUB 0xff9f1c

static void col(Uint32 c, Uint8 a) { SDL_SetRenderDrawColor(G.ren, c >> 16, (c >> 8) & 255, c & 255, a); }
static void fill(int x, int y, int w, int h, Uint32 c, Uint8 a) { SDL_Rect r = { x, y, w, h }; col(c, a); SDL_RenderFillRect(G.ren, &r); }

static int text(int x, int y, Uint32 c, float scale, const char *s)
{
    SDL_SetTextureColorMod(G.font, c >> 16, (c >> 8) & 255, c & 255);
    int cw = (int)(FONT_CW * scale), ch = (int)(FONT_CH * scale), x0 = x;
    for (; *s; s++) {
        int g = (unsigned char)*s - FONT_FIRST;
        if (g < 0 || g >= FONT_COUNT) g = '?' - FONT_FIRST;
        if (g) {
            SDL_Rect src = { g * FONT_CW, 0, FONT_CW, FONT_CH }, dst = { x, y, cw, ch };
            SDL_RenderCopy(G.ren, G.font, &src, &dst);
        }
        x += cw;
    }
    return x - x0;
}

static void textf(int x, int y, Uint32 c, float sc, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void textf(int x, int y, Uint32 c, float sc, const char *fmt, ...)
{
    char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    text(x, y, c, sc, b);
}

static void text_right(int xr, int y, Uint32 c, float sc, const char *s)
{
    text(xr - (int)(strlen(s) * FONT_CW * sc), y, c, sc, s);
}

static void fit(char *out, size_t sz, const char *s, int maxch)
{
    int n = (int)strlen(s);
    if (n <= maxch) snprintf(out, sz, "%s", s);
    else snprintf(out, sz, "%.*s...", maxch - 3, s);
}

static void make_font(void)
{
    Uint32 *px = malloc((size_t)FONT_ATLAS_W * FONT_CH * 4);
    for (int i = 0; i < FONT_ATLAS_W * FONT_CH; i++) px[i] = (Uint32)font_atlas[i] << 24 | 0xFFFFFF;
    G.font = SDL_CreateTexture(G.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, FONT_ATLAS_W, FONT_CH);
    SDL_UpdateTexture(G.font, NULL, px, FONT_ATLAS_W * 4);
    SDL_SetTextureBlendMode(G.font, SDL_BLENDMODE_BLEND);
    free(px);
}

static void update_peaks(void)
{
    Engine *e = &G.eng;
    Uint32 now = SDL_GetTicks();
    if (e->version == G.pk_version && e->len == G.pk_len) return;
    if (now - G.pk_time < 70 && e->len == G.pk_len) return;
    G.pk_version = e->version; G.pk_len = e->len; G.pk_time = now;
    int len = e->len;
    for (int c = 0; c < WAVE_W; c++) {
        long a = (long)c * len / WAVE_W, b = (long)(c + 1) * len / WAVE_W;
        if (b <= a) b = a + 1;
        long step = (b - a) / 48; if (step < 1) step = 1;
        float m = 0;
        for (long i = a; i < b && i < len; i += step) {
            float l = fabsf(e->buf[2 * i]), r = fabsf(e->buf[2 * i + 1]);
            if (l > m) m = l;
            if (r > m) m = r;
        }
        G.peak[c] = m;
    }
}

static void fmt_time(char *o, size_t sz, double s) { snprintf(o, sz, "%d:%04.1f", (int)(s / 60), fmod(s, 60)); }

static void meter(int x, int y, int w, const char *label, float pk)
{
    float db = pk > 1e-6f ? 20.f * log10f(pk) : -90.f;
    float t = (db + 60.f) / 60.f; t = t < 0 ? 0 : t > 1 ? 1 : t;
    text(x, y - 3, C_DIM, 0.75f, label);
    int bx = x + 40, bw = w - 40 - 70;
    fill(bx, y, bw, 12, C_PANEL2, 255);
    Uint32 c = db > -1.f ? C_REC : db > -9.f ? C_ACCENT : C_PLAY;
    fill(bx, y, (int)(bw * t), 12, c, 255);
    char b[16]; if (db < -60) snprintf(b, sizeof b, "-inf"); else snprintf(b, sizeof b, "%.0f dB", db);
    text_right(x + w, y - 3, C_DIM, 0.75f, b);
}

static void value_str(int id, char *o, size_t sz)
{
    const ParamDesc *d = &PD[id]; float v = G.eng.p[id];
    switch (d->kind) {
    case K_ENUM: snprintf(o, sz, "%s", d->labels[(int)v]); break;
    case K_PCT: snprintf(o, sz, "%.0f%%", v * 100); break;
    case K_DECAY:
        if (v < 0.0001f) snprintf(o, sz, "Off");
        else snprintf(o, sz, "%.*f%% per pass  (%.2f dB)", v < 0.01f ? 2 : 1, v * 100, 20 * log10f(1 - v));
        break;
    case K_LOOPPOS: { char t[32]; fmt_time(t, sizeof t, v * G.eng.len / CD_SR); snprintf(o, sz, "%s  (%.1f%%)", t, v * 100); } break;
    case K_ST: snprintf(o, sz, "%+.0f st  x%.3f", v, eng_rate(G.eng.p)); break;
    case K_CENTS: snprintf(o, sz, "%+.0f ct", v); break;
    case K_DB: snprintf(o, sz, "%.0f dB", v); break;
    case K_GAIN: snprintf(o, sz, "%+.0f dB", v); break;
    case K_MS:
        if (id == P_DUB_ALIGN) snprintf(o, sz, "%+.0f ms%s", v, v == 0 ? "  (auto)" : v > 0 ? "  (earlier)" : "  (later)");
        else if (G.eng.p[P_DLY_SYNC] > 0.5f) snprintf(o, sz, "(synced)");
        else snprintf(o, sz, "%.0f ms", v);
        break;
    case K_INT:
        if (id == P_BITS && v >= 16) snprintf(o, sz, "16 (off)");
        else if (id == P_DOWNS && v <= 1) snprintf(o, sz, "1 (off)");
        else snprintf(o, sz, "%.0f", v);
        break;
    }
}

static void draw(void)
{
    Engine *e = &G.eng;
    Uint32 now = SDL_GetTicks();
    col(C_BG, 255); SDL_RenderClear(G.ren);
    SDL_SetRenderDrawBlendMode(G.ren, SDL_BLENDMODE_BLEND);

    /* header */
    text(16, 12, C_ACCENT, 1.5f, "CONDUIT");
    const char *st = "IDLE"; Uint32 sc = C_DIM;
    if (e->rec_state == ST_REC) { st = "REC"; sc = C_REC; }
    else if (e->rec_state == ST_ARMED) { st = "ARMED"; sc = (now / 300) & 1 ? C_REC : C_DIM; }
    else if (e->overdub) { st = "OVERDUB"; sc = C_DUB; }
    else if (e->playing) { st = "PLAY"; sc = C_PLAY; }
    fill(150, 12, 112, 32, sc, 50);
    text(150 + (112 - (int)strlen(st) * FONT_CW) / 2, 16, sc, 1.f, st);
    char nm[64]; fit(nm, sizeof nm, G.in_open, 38);
    textf(280, 16, G.in_failed ? C_REC : C_DIM, 1.f, "IN %s", nm);
    {
        int ls, le; eng_loop_range(e, &ls, &le);
        char t[32], b[64];
        if (e->rec_state == ST_REC) { fmt_time(t, sizeof t, e->rec_pos / (double)CD_SR); snprintf(b, sizeof b, "%s / %d:00", t, MAX_SECONDS / 60); }
        else if (e->len) { fmt_time(t, sizeof t, (le - ls) / (double)CD_SR / eng_rate(e->p)); snprintf(b, sizeof b, "loop %s", t); }
        else snprintf(b, sizeof b, "empty");
        text_right(1008, 16, C_TEXT, 1.f, b);
    }

    /* waveform */
    fill(WAVE_X, WAVE_Y, WAVE_W, WAVE_H, C_PANEL, 255);
    int mid = WAVE_Y + WAVE_H / 2;
    col(C_PANEL2, 255); SDL_RenderDrawLine(G.ren, WAVE_X, mid, WAVE_X + WAVE_W - 1, mid);
    if (e->len > 1) {
        update_peaks();
        int ls, le; eng_loop_range(e, &ls, &le);
        int xs = WAVE_X + (int)((long)ls * WAVE_W / e->len), xe = WAVE_X + (int)((long)le * WAVE_W / e->len);
        Uint32 wc = e->rec_state == ST_REC ? C_REC : e->overdub ? C_DUB : C_WAVE;
        for (int c = 0; c < WAVE_W; c++) {
            int x = WAVE_X + c, h = (int)(sqrtf(G.peak[c]) * (WAVE_H / 2 - 6));
            int inside = e->rec_state == ST_REC || (x >= xs && x < xe);
            col(inside ? wc : C_DIM, inside ? 255 : 110);
            SDL_RenderDrawLine(G.ren, x, mid - h, x, mid + h);
        }
        if (e->rec_state != ST_REC) {
            fill(xs - 1, WAVE_Y, 3, WAVE_H, C_ACCENT, 255);
            fill(xe - 2, WAVE_Y, 3, WAVE_H, C_ACCENT, 255);
            fill(xs, WAVE_Y, xe - xs, 4, C_ACCENT, 90);
            if (e->playing) {
                int px = WAVE_X + (int)((long)e->pos_i * WAVE_W / e->len);
                fill(px - 1, WAVE_Y, 2, WAVE_H, C_TEXT, 230);
            }
        }
    } else {
        const char *m = e->rec_state == ST_ARMED ? "Waiting for signal above threshold..."
                      : "Press A to sample from the input";
        text(WAVE_X + (WAVE_W - (int)strlen(m) * FONT_CW) / 2, mid - 12, C_DIM, 1.f, m);
    }

    /* meters */
    meter(16, 356, 488, "IN", e->in_peak);
    meter(520, 356, 488, "OUT", e->out_peak);

    /* tabs */
    for (int i = 0; i < PG_N; i++) {
        int x = 16 + i * 250, on = i == G.page;
        fill(x, 384, 242, 38, on ? C_PANEL2 : C_PANEL, 255);
        if (on) fill(x, 418, 242, 4, C_ACCENT, 255);
        text(x + (242 - (int)strlen(PAGE_NAME[i]) * FONT_CW) / 2, 391, on ? C_TEXT : C_DIM, 1.f, PAGE_NAME[i]);
    }

    /* rows */
    int y0 = 432, rh = 36;
    int n = G.page == PG_FILES ? G.nfiles : page_count(G.page);
    int sc0 = G.scroll[G.page];
    if (G.page == PG_FILES && n == 0) {
        textf(32, y0 + 10, C_DIM, 1.f, "No recordings yet in %s", G.rec_dir);
    }
    for (int k = 0; k < ROWS_VISIBLE && sc0 + k < n; k++) {
        int i = sc0 + k, y = y0 + k * rh, on = i == G.sel[G.page];
        if (on) { fill(16, y, 992, rh - 4, C_PANEL2, 255); fill(16, y, 5, rh - 4, C_ACCENT, 255); }
        char v[300], nm2[120];
        if (G.page == PG_FILES) {
            fit(nm2, sizeof nm2, G.files[i], 60);
            text(32, y + 4, on ? C_TEXT : C_DIM, 1.f, nm2);
            snprintf(v, sizeof v, "%.1f MB", G.fsize[i] / 1048576.0);
            text_right(992, y + 4, C_DIM, 1.f, v);
            continue;
        }
        int id = PAGE_ROWS[G.page][i];
        const char *label = id >= 0 ? PD[id].name : id == ROW_IN_DEV ? "Input device"
                          : id == ROW_OUT_DEV ? "Output device" : id == ROW_FOLDER ? "Save folder" : "Diagnostics";
        text(32, y + 4, on ? C_TEXT : C_DIM, 1.f, label);
        if (id >= 0) {
            value_str(id, v, sizeof v);
            text(300, y + 4, on ? C_ACCENT : C_TEXT, 1.f, v);
            const ParamDesc *d = &PD[id];
            if (d->kind != K_ENUM) {
                float t = (G.eng.p[id] - d->min) / (d->max - d->min);
                fill(640, y + 12, 352, 8, C_PANEL, 255);
                fill(640, y + 12, (int)(352 * t), 8, on ? C_ACCENT : C_DIM, 255);
            }
        } else {
            if (id == ROW_IN_DEV) fit(v, sizeof v, G.in_name[0] ? G.in_name : "Auto (USB first, else default)", 56);
            else if (id == ROW_OUT_DEV) fit(v, sizeof v, G.out_name[0] ? G.out_name : "System default", 56);
            else if (id == ROW_FOLDER) fit(v, sizeof v, G.rec_dir, 56);
            else snprintf(v, sizeof v, "%s | %d in devs | xrun %u/%u | btn %d",
                          SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
                          SDL_GetNumAudioDevices(1), (unsigned)e->overruns, (unsigned)e->underruns, G.last_raw_btn);
            text(300, y + 4, id <= ROW_FOLDER ? C_DIM : (on ? C_ACCENT : C_TEXT), id == ROW_DIAG ? 0.85f : 1.f, v);
        }
    }
    if (n > ROWS_VISIBLE) {
        int th = ROWS_VISIBLE * rh * ROWS_VISIBLE / n;
        fill(1010, y0 + sc0 * (ROWS_VISIBLE * rh - th) / (n - ROWS_VISIBLE), 4, th, C_DIM, 200);
    }

    /* footer */
    fill(0, 728, SW, 40, C_PANEL, 255);
    const char *hint = G.page == PG_FILES
        ? "A load   Y delete   B play   LEFT/RIGHT page jump   L1/R1 tab   MENU quit"
        : G.held[A_SEL] ? "SELECT +  START bounce  Y resample  B undo (x16)  X erase  A save  MENU quit"
                        : "A rec  B play (hold: erase)  X overdub  Y rev  START save  hold SELECT";
    text(16, 736, C_DIM, 0.95f, hint);

    /* toast */
    if (now < G.toast_until) {
        int w = (int)strlen(G.toast) * FONT_CW + 40; if (w > 980) w = 980;
        int x = (SW - w) / 2;
        fill(x, 286, w, 44, C_BG, 235);
        fill(x, 286, 4, 44, G.toast_bad ? C_REC : C_ACCENT, 255);
        char t[90]; fit(t, sizeof t, G.toast, 78);
        text(x + 20, 296, C_TEXT, 1.f, t);
    }
    SDL_RenderPresent(G.ren);
}

/* -------------------------------------------------------- self test */

static int screenshot(const char *path)
{
    Uint32 *px = malloc(SW * SH * 4);
    if (SDL_RenderReadPixels(G.ren, NULL, SDL_PIXELFORMAT_ARGB8888, px, SW * 4)) { free(px); return -1; }
    FILE *f = fopen(path, "wb");
    if (!f) { free(px); return -1; }
    fprintf(f, "P6\n%d %d\n255\n", SW, SH);
    for (int i = 0; i < SW * SH; i++) { unsigned char c[3] = { px[i] >> 16, px[i] >> 8, px[i] }; fwrite(c, 1, 3, f); }
    fclose(f); free(px);
    return 0;
}

/* Scripted run used by `make test-e2e` with SDL's disk audio driver:
 * record -> loop -> FX -> overdub -> undo -> save -> bounce -> resample -> reload */
static int selftest(const char *shot_dir)
{
#define WAITMS(ms) do { Uint32 _t = SDL_GetTicks() + (ms); while (SDL_GetTicks() < _t) { SDL_Event ev; while (SDL_PollEvent(&ev)) handle_event(&ev); eng_ui_tick(&G.eng); if (G.ren) draw(); SDL_Delay(10); } } while (0)
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "SELFTEST FAIL: %s\n", msg); return 1; } else fprintf(stderr, "ok   %s\n", msg); } while (0)
    char p[700];
    Engine *e = &G.eng;
    CHECK(G.in_dev != 0, "capture device opened");
    eng_rec_button(e); WAITMS(150);
    CHECK(e->rec_state == ST_REC, "recording started");
    WAITMS(1200);
    if (shot_dir) { snprintf(p, sizeof p, "%s/shot_rec.ppm", shot_dir); screenshot(p); }
    eng_rec_button(e); WAITMS(150);
    CHECK(e->rec_state == ST_IDLE && e->len > CD_SR / 2, "take captured (>0.5s)");
    CHECK(e->playing, "auto-loop after record");
    float mx = 0; for (int i = 0; i < e->len * 2; i++) if (fabsf(e->buf[i]) > mx) mx = fabsf(e->buf[i]);
    fprintf(stderr, "     take: %.2fs peak %.3f\n", e->len / (double)CD_SR, mx);
    CHECK(mx > 0.05f, "take contains input signal");

    e->p[P_LOOP_START] = 0.1f; e->p[P_LOOP_END] = 0.6f;
    e->p[P_PITCH] = 7; e->p[P_FILT_MODE] = 1; e->p[P_CUTOFF] = 0.6f; e->p[P_RESO] = 0.5f;
    e->p[P_DLY_SYNC] = 3; e->p[P_DLY_MIX] = 0.5f; e->p[P_BITS] = 8; e->p[P_DRIVE] = 0.3f;
    G.page = PG_FX;
    WAITMS(500);
    CHECK(e->out_peak > 0.01f, "processed loop is audible");
    if (shot_dir) { snprintf(p, sizeof p, "%s/shot_fx.ppm", shot_dir); screenshot(p); }

    float before = e->buf[2 * (e->len / 3)];
    eng_overdub_toggle(e); WAITMS(700); eng_overdub_toggle(e); WAITMS(100);
    CHECK(eng_undo_count(e) == 1, "overdub recorded");
    if (shot_dir) { snprintf(p, sizeof p, "%s/shot_dub.ppm", shot_dir); screenshot(p); }
    SDL_LockAudioDevice(G.out_dev); int u = eng_undo(e); SDL_UnlockAudioDevice(G.out_dev);
    CHECK(u == UNDO_DUB && e->buf[2 * (e->len / 3)] == before, "undo restored buffer");

    int n0 = G.nfiles;
    save_take(); scan_files();
    CHECK(G.nfiles == n0 + 1, "take saved to recordings folder");
    WAITMS(1100); /* new timestamp */
    save_bounce(); scan_files();
    CHECK(G.nfiles == n0 + 2, "bounce saved");
    int oldlen = e->len; resample();
    CHECK(e->len > 0 && e->len != oldlen && e->p[P_PITCH] == 0, "resampled into buffer");
    G.page = PG_FILES; scan_files();
    CHECK(G.nfiles >= 2 && strstr(G.files[0], "bounce"), "file list is newest first");
    load_file(0); WAITMS(300);
    CHECK(e->len > 0 && e->playing, "reloaded newest file and looping");
    if (shot_dir) { snprintf(p, sizeof p, "%s/shot_files.ppm", shot_dir); screenshot(p); }
    int kept = e->len;
    eng_erase(e); WAITMS(200);
    CHECK(e->len == 0 && !e->playing && e->rec_state == ST_IDLE, "erase stops and empties the loop without recording");
    SDL_LockAudioDevice(G.out_dev); u = eng_undo(e); SDL_UnlockAudioDevice(G.out_dev);
    CHECK(u == UNDO_ERASE && e->len == kept, "undo brings an erased loop back");
    fprintf(stderr, "SELFTEST PASS (driver %s, xruns in %u)\n", SDL_GetCurrentAudioDriver(), (unsigned)e->overruns);
    return 0;
}

/* -------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    int test = 0; const char *shots = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) test = 1;
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) shots = argv[++i];
        else if (!strcmp(argv[i], "--version")) { puts("Conduit " CONDUIT_VERSION); return 0; }
    }
    const char *rd = getenv("CONDUIT_REC_DIR"), *cf = getenv("CONDUIT_CFG");
    snprintf(G.rec_dir, sizeof G.rec_dir, "%s", rd && *rd ? rd : "recordings");
    snprintf(G.cfg_path, sizeof G.cfg_path, "%s", cf && *cf ? cf : "conduit.cfg");
    mkdirs(G.rec_dir);

    if (eng_init(&G.eng, MAX_SECONDS)) { fprintf(stderr, "out of memory\n"); return 1; }
    load_cfg();

    SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    G.win = SDL_CreateWindow("Conduit", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, SW, SH,
                             SDL_WINDOW_SHOWN | (getenv("CONDUIT_WINDOWED") ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP));
    if (!G.win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    G.ren = SDL_CreateRenderer(G.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!G.ren) G.ren = SDL_CreateRenderer(G.win, -1, SDL_RENDERER_SOFTWARE);
    if (!G.ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
    SDL_RenderSetLogicalSize(G.ren, SW, SH);
    SDL_ShowCursor(0);
    /* MustardOS: the screen can stay dark once the window is up; mux_launch.sh points this at wake_screen.sh */
    { const char *wake = getenv("CONDUIT_SCREEN_WAKE");
      if (wake && *wake) { char cmd[600]; snprintf(cmd, sizeof cmd, "'%s' >/dev/null 2>&1 &", wake); if (system(cmd)) {} } }
    make_font();
    open_pads();

    if (open_output()) { toast(1, "No audio output available"); }
    open_input(0);
    fprintf(stderr, "[conduit] audio driver %s, input '%s', recordings -> %s\n",
            SDL_GetCurrentAudioDriver(), G.in_open, G.rec_dir);
    for (int i = 0; i < SDL_GetNumAudioDevices(1); i++)
        fprintf(stderr, "[conduit]   capture %d: %s\n", i, SDL_GetAudioDeviceName(i, 1));
    scan_files();
    if (G.in_failed) toast(1, "No input device - plug in USB audio or check IO page");
    else toast(0, "Input: %s", G.in_open);

    G.running = 1;
    int rc = 0;
    if (test) rc = selftest(shots);
    else
        while (G.running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) handle_event(&ev);
            repeat_tick();
            eng_ui_tick(&G.eng);
            draw();
            SDL_Delay(4);
        }

    save_cfg();
    if (G.in_dev) SDL_CloseAudioDevice(G.in_dev);
    if (G.out_dev) SDL_CloseAudioDevice(G.out_dev);
    SDL_DestroyTexture(G.font); SDL_DestroyRenderer(G.ren); SDL_DestroyWindow(G.win);
    SDL_Quit();
    eng_free(&G.eng);
    return rc;
}
