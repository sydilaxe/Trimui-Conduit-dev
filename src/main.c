/*
 * Conduit — four-track USB audio looper / sampler for TrimUI Brick & Brick Pro.
 * SDL2 only (audio, video, input); runs on TrimUI OS, MustardOS and KNULLI.
 */
#include <SDL2/SDL.h>
#include "dsp.h"
#include "wav.h"
#include "midi.h"
#include "font_data.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
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
#define LANE_X 100
#define LANE_Y 56
#define LANE_W 908
#define LANE_H 66
#define LANE_STEP 72

/* ------------------------------------------------------------ params */

enum { K_PCT, K_LOOPPOS, K_ST, K_CENTS, K_ENUM, K_DB, K_MS, K_INT, K_GAIN, K_DECAY, K_PAN, K_SEND, K_SEC };
typedef struct {
    const char *name; float min, max, step, coarse; int kind; const char *const *labels;
} ParamDesc;

static const char *const L_ONOFF[] = { "Off", "On" };
static const char *const L_FILT[] = { "Off", "Low-pass", "High-pass", "Band-pass" };
static const char *const L_DSYNC[] = { "Free (ms)", "1/16 bar", "1/8 bar", "1/4 bar", "1/3 bar", "1/2 bar", "1 bar" };
static const char *const L_TRIG[] = { "Instant", "Threshold" };
static const char *const L_FMT[] = { "16-bit PCM", "32-bit float" };
static const char *const L_DECAY[] = { "While overdubbing", "Always (loop fades)" };
static const char *const L_LEN[] = { "Synced (whole bars)", "Free" };
static const char *const L_VERB[] = { "Bloom 1  (MidiVerb II #45)", "Bloom 2  (#49: 85 ms pre-delay)" };
static const char *const L_CLOCK[] = { "Internal (first loop = 1 bar)", "MIDI clock (USB)" };

static const ParamDesc PD[P_COUNT] = {
    [P_LOOP_START] = { "Window start", 0, 1, 0.002f, 0.02f, K_LOOPPOS, 0 },
    [P_LOOP_END]   = { "Window end",   0, 1, 0.002f, 0.02f, K_LOOPPOS, 0 },
    [P_PITCH]      = { "Pitch",    -24, 24, 1, 12, K_ST, 0 },
    [P_FINE]       = { "Fine tune", -100, 100, 5, 25, K_CENTS, 0 },
    [P_REVERSE]    = { "Reverse", 0, 1, 1, 1, K_ENUM, L_ONOFF },
    [P_DECAY]      = { "Loop decay", 0, 0.5f, 0.005f, 0.05f, K_DECAY, 0 },
    [P_DECAY_MODE] = { "Decay", 0, 1, 1, 1, K_ENUM, L_DECAY },
    [P_VOLUME]     = { "Volume", 0, 1.5f, 0.05f, 0.25f, K_PCT, 0 },
    [P_PAN]        = { "Pan", -1, 1, 0.05f, 0.25f, K_PAN, 0 },
    [P_MUTE]       = { "Mute", 0, 1, 1, 1, K_ENUM, L_ONOFF },
    [P_FX_SEND]    = { "FX send", 0, 1, 0.05f, 0.25f, K_SEND, 0 },
    [P_VERB_SEND]  = { "Reverb send", 0, 1, 0.05f, 0.25f, K_PCT, 0 },
    [P_SYNC]       = { "Length", 0, 1, 1, 1, K_ENUM, L_LEN },
    [P_MASTER]     = { "Master volume", 0, 1.5f, 0.05f, 0.25f, K_PCT, 0 },
    [P_MONITOR]    = { "Input monitor", 0, 1, 0.05f, 0.25f, K_PCT, 0 },
    [P_FILT_MODE]  = { "Filter", 0, 3, 1, 1, K_ENUM, L_FILT },
    [P_CUTOFF]     = { "Cutoff", 0, 1, 0.01f, 0.1f, K_PCT, 0 },
    [P_RESO]       = { "Resonance", 0, 0.97f, 0.03f, 0.15f, K_PCT, 0 },
    [P_DRIVE]      = { "Drive", 0, 1, 0.05f, 0.2f, K_PCT, 0 },
    [P_BITS]       = { "Bit depth", 1, 16, 1, 4, K_INT, 0 },
    [P_DOWNS]      = { "Downsample", 1, 32, 1, 4, K_INT, 0 },
    [P_DLY_SYNC]   = { "Delay sync", 0, 6, 1, 1, K_ENUM, L_DSYNC },
    [P_DLY_TIME]   = { "Delay time", 10, 1900, 10, 100, K_MS, 0 },
    [P_DLY_FB]     = { "Delay feedback", 0, 0.95f, 0.05f, 0.2f, K_PCT, 0 },
    [P_DLY_MIX]    = { "Delay mix", 0, 1, 0.05f, 0.25f, K_PCT, 0 },
    [P_PINGPONG]   = { "Ping-pong", 0, 1, 1, 1, K_ENUM, L_ONOFF },
    [P_VERB_TYPE]  = { "Reverb", 0, 1, 1, 1, K_ENUM, L_VERB },
    [P_VERB_TIME]  = { "Reverb time", 1, 30, 0.5f, 2, K_SEC, 0 },
    [P_VERB_LEVEL] = { "Reverb level", 0, 1.5f, 0.05f, 0.25f, K_PCT, 0 },
    [P_IN_GAIN]    = { "Input gain", -12, 24, 1, 6, K_GAIN, 0 },
    [P_TRIGGER]    = { "Rec trigger", 0, 1, 1, 1, K_ENUM, L_TRIG },
    [P_THRESH]     = { "Threshold", -60, -6, 1, 6, K_DB, 0 },
    [P_AUTOPLAY]   = { "Loop after rec", 0, 1, 1, 1, K_ENUM, L_ONOFF },
    [P_SAVE_FMT]   = { "Save format", 0, 1, 1, 1, K_ENUM, L_FMT },
    [P_DUB_ALIGN]  = { "Dub align", -60, 120, 1, 10, K_MS, 0 },
    [P_CLOCK]      = { "Sync to", 0, 1, 1, 1, K_ENUM, L_CLOCK },
    [P_REC_WAIT]   = { "Rec waits for bar", 0, 1, 1, 1, K_ENUM, L_ONOFF },
    [P_BEATS]      = { "Beats per bar", 1, 16, 1, 4, K_INT, 0 },
    [P_CLOCK_OFS]  = { "Clock offset", -100, 100, 1, 10, K_MS, 0 },
};
/* config / MIDI-map keys (loop_decay ... keep the names older versions saved) */
static const char *const PKEY[P_COUNT] = {
    [P_LOOP_START] = "win_start", [P_LOOP_END] = "win_end", [P_PITCH] = "pitch", [P_FINE] = "fine",
    [P_REVERSE] = "reverse", [P_DECAY] = "loop_decay", [P_DECAY_MODE] = "decay_mode", [P_VOLUME] = "volume",
    [P_PAN] = "pan", [P_MUTE] = "mute", [P_FX_SEND] = "fx_send", [P_VERB_SEND] = "reverb_send", [P_SYNC] = "length", [P_MASTER] = "master", [P_MONITOR] = "monitor",
    [P_FILT_MODE] = "filter", [P_CUTOFF] = "cutoff", [P_RESO] = "resonance", [P_DRIVE] = "drive",
    [P_BITS] = "bits", [P_DOWNS] = "downsample", [P_DLY_SYNC] = "delay_sync", [P_DLY_TIME] = "delay_time",
    [P_DLY_FB] = "delay_feedback", [P_DLY_MIX] = "delay_mix", [P_PINGPONG] = "pingpong", [P_VERB_TYPE] = "reverb", [P_VERB_TIME] = "reverb_time", [P_VERB_LEVEL] = "reverb_level",
    [P_IN_GAIN] = "in_gain", [P_TRIGGER] = "trigger", [P_THRESH] = "threshold", [P_AUTOPLAY] = "autoplay",
    [P_SAVE_FMT] = "save_format", [P_DUB_ALIGN] = "dub_align_ms", [P_CLOCK] = "sync_to",
    [P_REC_WAIT] = "rec_wait", [P_BEATS] = "beats_per_bar", [P_CLOCK_OFS] = "clock_offset_ms",
};

enum { ROW_IN_DEV = -1, ROW_OUT_DEV = -2, ROW_FOLDER = -3, ROW_DIAG = -4, ROW_TEMPO = -5, ROW_MIDI_DEV = -6,
       ROW_END = -100 };
enum { PG_TRACK, PG_FX, PG_SYNC, PG_IO, PG_MIDI, PG_FILES, PG_N };
static const char *const PAGE_NAME[PG_N] = { "TRACK", "FX", "SYNC", "IO", "MIDI", "FILES" };
static const int PAGE_ROWS[PG_N][16] = {
    { P_LOOP_START, P_LOOP_END, P_PITCH, P_FINE, P_REVERSE, P_VOLUME, P_PAN, P_FX_SEND, P_VERB_SEND, P_MUTE, P_SYNC, P_DECAY, P_DECAY_MODE, ROW_END },
    { P_FILT_MODE, P_CUTOFF, P_RESO, P_DRIVE, P_BITS, P_DOWNS, P_DLY_SYNC, P_DLY_TIME, P_DLY_FB, P_DLY_MIX, P_PINGPONG, P_VERB_TYPE, P_VERB_TIME, P_VERB_LEVEL, ROW_END },
    { P_CLOCK, P_REC_WAIT, P_BEATS, P_CLOCK_OFS, ROW_TEMPO, ROW_MIDI_DEV, ROW_END },
    { ROW_IN_DEV, ROW_OUT_DEV, P_IN_GAIN, P_MONITOR, P_MASTER, P_TRIGGER, P_THRESH, P_AUTOPLAY, P_SAVE_FMT, P_DUB_ALIGN, ROW_FOLDER, ROW_DIAG, ROW_END },
    { ROW_END },
    { ROW_END },
};
static int page_count(int pg) { int n = 0; while (PAGE_ROWS[pg][n] != ROW_END) n++; return n; }

/* ------------------------------------------------------------- input */

enum { A_UP, A_DOWN, A_LEFT, A_RIGHT, A_A, A_B, A_X, A_Y, A_L1, A_R1, A_L2, A_R2,
       A_SEL, A_START, A_MENU, A_N };
static const char *const ACT_KEY[A_N] = { "up", "down", "left", "right", "a", "b", "x", "y",
                                          "l1", "r1", "l2", "r2", "select", "start", "menu" };

/* things a button or a MIDI note / CC can do */
enum { ACT_REC, ACT_PLAY, ACT_DUB, ACT_ERASE, ACT_UNDO, ACT_REVERSE, ACT_MUTE, ACT_NEXT, ACT_PREV,
       ACT_SEL, ACT_SAVE_ALL, ACT_SAVE_TRACK, ACT_RESAMPLE, ACT_PLAY_ALL, ACT_FX, ACT_VERB };
/* the live controls (analog sticks, or mapped knobs) */
enum { LIVE_WPOS, LIVE_WSIZE, LIVE_BEND, LIVE_DIR };

/* MIDI map: every target can take one note, CC or the pitch wheel */
enum { MT_ACT, MT_PARAM, MT_LIVE };
typedef struct { char key[32], name[48]; int kind, id, trk; } Target;   /* trk -1: the selected track */
typedef struct { uint8_t type, ch, num, hi; } Bind;                      /* type 0: none */
#define MAX_TARGETS 128

/* ---------------------------------------------------------------- app */

typedef struct {
    Engine eng;
    SDL_Window *win; SDL_Renderer *ren; SDL_Texture *font;
    SDL_AudioDeviceID out_dev, in_dev;
    char in_name[256], out_name[256];      /* "" = system default */
    char in_open[256];                      /* what is actually open */
    int in_failed, out_block;
    int b_erased;                           /* B held: the loop was erased */
    int sel;                                /* selected track */
    float send_mem[CD_TRACKS], verb_mem[CD_TRACKS];              /* the FX send to go back to when it's switched on again */
    int trig_used[A_N];                     /* L2/R2 used as a fine / coarse modifier while held */

    int page, sel_row[PG_N], scroll[PG_N];
    char files[MAX_FILES][256]; long fsize[MAX_FILES]; int nfiles;
    int del_confirm; Uint32 confirm_until; int clear_confirm;

    char toast[200]; Uint32 toast_until; int toast_bad;
    char rec_dir[512], cfg_path[512];

    float peak[CD_TRACKS][LANE_W]; unsigned pk_version[CD_TRACKS]; int pk_len[CD_TRACKS]; Uint32 pk_time[CD_TRACKS];

    int held[A_N]; Uint32 next_rep[A_N], held_since[A_N];
    int joy_map[A_N];   /* raw joystick button index, -1 = none */
    int have_gc, swap_ab, last_raw_btn, axis_held[2];
    float ax[4];        /* sticks: left x/y, right x/y, -1..1 */
    int flick[4];       /* a stick pushed past halfway (edge-triggered moves) */
    int bend_on, bend_trk;  /* the right stick is bending this track */
    Uint32 last_tick;

    Target tg[MAX_TARGETS]; Bind bind[MAX_TARGETS]; int ntg;
    int learn;          /* target waiting for a MIDI control, -1 none */
    char midi_last[64];
    int running;
} App;

static App G;

static void toast(int bad, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void toast(int bad, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vsnprintf(G.toast, sizeof G.toast, fmt, ap);
    va_end(ap);
    G.toast_until = SDL_GetTicks() + 2600; G.toast_bad = bad;
    fprintf(stderr, "[conduit] %s\n", G.toast);
}

static Track *trk(int k) { return &G.eng.trk[k]; }
static float *pp(int id) { return id < P_TRACK_N ? &G.eng.trk[G.sel].p[id] : &G.eng.p[id]; }

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

/* recordings/conduit_<time>[_n] -- one stamp for the files saved together */
static void stamp_base(char *out, size_t sz, const char *probe_suffix)
{
    char ts[32]; time_t t = time(NULL); struct tm tmv; localtime_r(&t, &tmv);
    strftime(ts, sizeof ts, "%Y%m%d_%H%M%S", &tmv);
    snprintf(out, sz, "%s/conduit_%s", G.rec_dir, ts);
    char probe[700]; struct stat st;
    snprintf(probe, sizeof probe, "%s_%s.wav", out, probe_suffix);
    for (int i = 2; stat(probe, &st) == 0 && i < 100; i++) {
        snprintf(out, sz, "%s/conduit_%s_%d", G.rec_dir, ts, i);
        snprintf(probe, sizeof probe, "%s_%s.wav", out, probe_suffix);
    }
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
    if (G.sel_row[PG_FILES] >= G.nfiles) G.sel_row[PG_FILES] = G.nfiles ? G.nfiles - 1 : 0;
}

/* --------------------------------------------------------- MIDI map */

static void add_target(int kind, int id, int t, const char *key, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void add_target(int kind, int id, int t, const char *key, const char *fmt, ...)
{
    if (G.ntg >= MAX_TARGETS) return;
    Target *g = &G.tg[G.ntg++];
    g->kind = kind; g->id = id; g->trk = t;
    snprintf(g->key, sizeof g->key, "%s", key);
    va_list ap; va_start(ap, fmt); vsnprintf(g->name, sizeof g->name, fmt, ap); va_end(ap);
}

static void build_targets(void)
{
    static const struct { int act; const char *key, *name; } acts[] = {
        { ACT_REC, "rec", "Record / stop" }, { ACT_PLAY, "play", "Play / stop" },
        { ACT_DUB, "overdub", "Overdub" }, { ACT_ERASE, "erase", "Erase" }, { ACT_UNDO, "undo", "Undo" },
        { ACT_REVERSE, "reverse_toggle", "Reverse on/off" }, { ACT_MUTE, "mute_toggle", "Mute on/off" },
        { ACT_FX, "fx_toggle", "FX send on/off" }, { ACT_VERB, "reverb_toggle", "Reverb send on/off" },
        { ACT_NEXT, "next_track", "Next track" }, { ACT_PREV, "prev_track", "Previous track" },
        { ACT_PLAY_ALL, "play_all", "Play / stop all tracks" },
        { ACT_SAVE_ALL, "save_all", "Save mix + tracks" }, { ACT_SAVE_TRACK, "save_track", "Save track" },
        { ACT_RESAMPLE, "resample", "Resample mix into track" },
    };
    char k[32];
    G.ntg = 0;
    for (size_t i = 0; i < sizeof acts / sizeof acts[0]; i++) add_target(MT_ACT, acts[i].act, -1, acts[i].key, "%s", acts[i].name);
    for (int t = 0; t < CD_TRACKS; t++) { snprintf(k, sizeof k, "select_%d", t + 1); add_target(MT_ACT, ACT_SEL, t, k, "Select track %d", t + 1); }
    add_target(MT_LIVE, LIVE_WPOS, -1, "window_pos", "Window position (scrub)");
    add_target(MT_LIVE, LIVE_WSIZE, -1, "window_size", "Window size");
    add_target(MT_LIVE, LIVE_BEND, -1, "bend", "Pitch bend (+-12 st)");
    add_target(MT_LIVE, LIVE_DIR, -1, "direction", "Direction (rev / fwd)");
    for (int t = 0; t < CD_TRACKS; t++) {
        snprintf(k, sizeof k, "t%d_rec", t + 1); add_target(MT_ACT, ACT_REC, t, k, "Track %d record / stop", t + 1);
        snprintf(k, sizeof k, "t%d_play", t + 1); add_target(MT_ACT, ACT_PLAY, t, k, "Track %d play / stop", t + 1);
        snprintf(k, sizeof k, "t%d_overdub", t + 1); add_target(MT_ACT, ACT_DUB, t, k, "Track %d overdub", t + 1);
        snprintf(k, sizeof k, "t%d_erase", t + 1); add_target(MT_ACT, ACT_ERASE, t, k, "Track %d erase", t + 1);
        snprintf(k, sizeof k, "t%d_undo", t + 1); add_target(MT_ACT, ACT_UNDO, t, k, "Track %d undo", t + 1);
        snprintf(k, sizeof k, "t%d_mute_toggle", t + 1); add_target(MT_ACT, ACT_MUTE, t, k, "Track %d mute on/off", t + 1);
        snprintf(k, sizeof k, "t%d_volume", t + 1); add_target(MT_PARAM, P_VOLUME, t, k, "Track %d volume", t + 1);
        snprintf(k, sizeof k, "t%d_pan", t + 1); add_target(MT_PARAM, P_PAN, t, k, "Track %d pan", t + 1);
        snprintf(k, sizeof k, "t%d_fx_toggle", t + 1); add_target(MT_ACT, ACT_FX, t, k, "Track %d FX send on/off", t + 1);
        snprintf(k, sizeof k, "t%d_fx_send", t + 1); add_target(MT_PARAM, P_FX_SEND, t, k, "Track %d FX send", t + 1);
        snprintf(k, sizeof k, "t%d_reverb_toggle", t + 1); add_target(MT_ACT, ACT_VERB, t, k, "Track %d reverb send on/off", t + 1);
        snprintf(k, sizeof k, "t%d_reverb_send", t + 1); add_target(MT_PARAM, P_VERB_SEND, t, k, "Track %d reverb send", t + 1);
    }
    for (int id = 0; id < P_TRACK_N; id++) add_target(MT_PARAM, id, -1, PKEY[id], "Track: %s", PD[id].name);
    for (int id = P_TRACK_N; id < P_COUNT; id++) add_target(MT_PARAM, id, -1, PKEY[id], "%s", PD[id].name);
    for (int i = 0; i < MAX_TARGETS; i++) memset(&G.bind[i], 0, sizeof G.bind[i]);
    G.learn = -1;
}

static void bind_str(const Bind *b, char *o, size_t sz)
{
    static const char *nn[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (b->type == MM_NOTE) snprintf(o, sz, "Note %s%d (%d)  ch %d", nn[b->num % 12], b->num / 12 - 1, b->num, b->ch + 1);
    else if (b->type == MM_CC) snprintf(o, sz, "CC %d  ch %d", b->num, b->ch + 1);
    else if (b->type == MM_PB) snprintf(o, sz, "Pitch wheel  ch %d", b->ch + 1);
    else snprintf(o, sz, "-");
}

/* ------------------------------------------------------------ config */

static const int PERSIST[] = { P_MASTER, P_MONITOR, P_IN_GAIN, P_TRIGGER, P_THRESH, P_AUTOPLAY, P_SAVE_FMT,
                               P_DUB_ALIGN, P_CLOCK, P_REC_WAIT, P_BEATS, P_CLOCK_OFS,
                               P_VERB_TYPE, P_VERB_TIME, P_VERB_LEVEL };
static const int PERSIST_T[] = { P_DECAY, P_DECAY_MODE, P_VOLUME, P_PAN, P_FX_SEND, P_VERB_SEND, P_SYNC };
#define NPERSIST (int)(sizeof PERSIST / sizeof PERSIST[0])
#define NPERSIST_T (int)(sizeof PERSIST_T / sizeof PERSIST_T[0])

static float clamp_param(int id, float x) { const ParamDesc *d = &PD[id]; return x < d->min ? d->min : x > d->max ? d->max : x; }

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
        } else if (!strncmp(k, "midi_", 5)) {
            char ty[8] = { 0 }; int ch = 0, num = 0;
            if (sscanf(v, "%7[a-z],%d,%d", ty, &ch, &num) != 3 || ch < 1 || ch > 16 || num < 0 || num > 127) continue;
            int type = !strcmp(ty, "note") ? MM_NOTE : !strcmp(ty, "cc") ? MM_CC : !strcmp(ty, "bend") ? MM_PB : 0;
            for (int i = 0; i < G.ntg && type; i++)
                if (!strcmp(k + 5, G.tg[i].key)) { G.bind[i].type = (uint8_t)type; G.bind[i].ch = (uint8_t)(ch - 1); G.bind[i].num = (uint8_t)num; }
        } else if (k[0] == 't' && k[1] >= '1' && k[1] <= '0' + CD_TRACKS && k[2] == '_') {
            for (int i = 0; i < NPERSIST_T; i++)
                if (!strcmp(k + 3, PKEY[PERSIST_T[i]])) trk(k[1] - '1')->p[PERSIST_T[i]] = clamp_param(PERSIST_T[i], (float)atof(v));
        } else {
            for (int i = 0; i < NPERSIST; i++)
                if (!strcmp(k, PKEY[PERSIST[i]])) G.eng.p[PERSIST[i]] = clamp_param(PERSIST[i], (float)atof(v));
            /* settings from the one-track versions apply to every track */
            for (int i = 0; i < NPERSIST_T; i++)
                if (!strcmp(k, PKEY[PERSIST_T[i]]))
                    for (int t = 0; t < CD_TRACKS; t++) trk(t)->p[PERSIST_T[i]] = clamp_param(PERSIST_T[i], (float)atof(v));
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
    for (int i = 0; i < NPERSIST; i++) fprintf(f, "%s=%g\n", PKEY[PERSIST[i]], G.eng.p[PERSIST[i]]);
    for (int t = 0; t < CD_TRACKS; t++)
        for (int i = 0; i < NPERSIST_T; i++) fprintf(f, "t%d_%s=%g\n", t + 1, PKEY[PERSIST_T[i]], trk(t)->p[PERSIST_T[i]]);
    fprintf(f, "# MIDI map: midi_<target>=note|cc|bend,<channel 1-16>,<number>\n");
    for (int i = 0; i < G.ntg; i++) {
        Bind *b = &G.bind[i];
        if (b->type) fprintf(f, "midi_%s=%s,%d,%d\n", G.tg[i].key,
                             b->type == MM_NOTE ? "note" : b->type == MM_CC ? "cc" : "bend", b->ch + 1, b->num);
    }
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
    fprintf(stderr, "output: %d Hz, %d ch, block %d\n", got.freq, got.channels, got.samples);
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
        G.eng.in_restart = 1;
        SDL_UnlockAudioDevice(G.out_dev);
        fprintf(stderr, "capture '%s': %d Hz, %d ch, block %d\n", G.in_open, got.freq, got.channels, got.samples);
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

static void begin_busy(void) { for (int k = 0; k < CD_TRACKS; k++) trk(k)->overdub = 0; G.eng.busy = 1; SDL_Delay(25); }
static void end_busy(void) { G.eng.busy = 0; }
static void draw(void);

static int any_recording(void)
{
    for (int k = 0; k < CD_TRACKS; k++) if (trk(k)->rec_state == ST_REC) return 1;
    return 0;
}

static void busy_note(const char *msg)
{
    snprintf(G.toast, sizeof G.toast, "%s", msg);
    G.toast_until = SDL_GetTicks() + 5000; G.toast_bad = 0;
    if (G.ren) draw();
}

static void describe_len(char *o, size_t sz, int frames, double cyc)
{
    double bars = cyc > 0 ? frames / cyc : 0;
    if (cyc > 0 && fabs(bars * 4 - floor(bars * 4 + 0.5)) < 0.02) {
        if (bars < 0.9) snprintf(o, sz, "%s bar", bars < 0.4 ? "1/4" : "1/2");
        else snprintf(o, sz, "%.0f bar%s", bars, bars > 1.5 ? "s" : "");
    } else snprintf(o, sz, "%.1f s", frames / (double)CD_SR);
}

/* the mix (through the FX) and every track, from bar 1, to WAV files that
 * line up in a DAW */
static void save_all(void)
{
    if (any_recording()) { toast(1, "Stop recording first"); return; }
    int have = 0;
    for (int k = 0; k < CD_TRACKS; k++) have += eng_track_has(&G.eng, k);
    if (!have) { toast(1, "Nothing recorded yet"); return; }
    busy_note("Saving...");
    char base[600], path[700];
    stamp_base(base, sizeof base, "mix");
    int bits = G.eng.p[P_SAVE_FMT] > 0.5f ? 32 : 16;
    begin_busy();
    Mixdown *m = calloc(1, sizeof *m);
    int n = m ? eng_mix_begin(&G.eng, m, MAX_SECONDS * CD_SR) : 0;
    WavOut *mw = NULL, *tw[CD_TRACKS] = { 0 };
    float *stems[CD_TRACKS] = { 0 }, *mix = NULL;
    int ok = n > 0, files = 0;
    if (ok) {
        snprintf(path, sizeof path, "%s_mix.wav", base);
        mw = wav_open(path, CD_SR, bits);
        mix = malloc(sizeof(float) * 2 * 4096);
        ok = mw && mix;
        for (int k = 0; ok && k < CD_TRACKS; k++)
            if (m->on[k]) {
                snprintf(path, sizeof path, "%s_t%d.wav", base, k + 1);
                tw[k] = wav_open(path, CD_SR, bits);
                stems[k] = malloc(sizeof(float) * 2 * 4096);
                ok = tw[k] && stems[k];
            }
    }
    for (int done = 0; ok && done < n; done += 4096) {
        int b = n - done < 4096 ? n - done : 4096;
        eng_mix_render(&G.eng, m, mix, stems, b);
        ok = wav_append(mw, mix, b) == 0;
        for (int k = 0; ok && k < CD_TRACKS; k++) if (tw[k]) ok = wav_append(tw[k], stems[k], b) == 0;
    }
    if (mw) { if (wav_close(mw, ok)) ok = 0; else files++; }
    for (int k = 0; k < CD_TRACKS; k++) {
        if (tw[k]) { if (wav_close(tw[k], ok)) ok = 0; else files++; }
        free(stems[k]);
    }
    free(mix);
    if (m) { if (n) eng_mix_end(m); free(m); }
    end_busy();
    sync();
    if (!ok) { toast(1, "Save failed: %s", n ? strerror(errno) : "out of memory?"); return; }
    char len[32]; describe_len(len, sizeof len, n, G.eng.tr.cyc);
    toast(0, "Saved the mix + %d track%s (%s): %s_*", files - 1, files == 2 ? "" : "s", len, strrchr(base, '/') + 1);
    scan_files();
}

/* the selected track's loop window as it is (no FX) */
static void save_track(int k)
{
    Track *t = trk(k);
    if (t->rec_state == ST_REC) { toast(1, "Stop recording first (A)"); return; }
    if (t->len < 2) { toast(1, "Track %d is empty", k + 1); return; }
    char base[600], path[700], sfx[16]; snprintf(sfx, sizeof sfx, "t%d", k + 1);
    stamp_base(base, sizeof base, sfx);
    snprintf(path, sizeof path, "%s_%s.wav", base, sfx);
    begin_busy();
    int ls, le; eng_loop_range(&G.eng, k, &ls, &le);
    int bits = G.eng.p[P_SAVE_FMT] > 0.5f ? 32 : 16;
    int r = wav_write(path, t->buf + 2 * (size_t)ls, le - ls, CD_SR, bits);
    end_busy();
    if (r) toast(1, "Save failed: %s", strerror(errno));
    else { toast(0, "Saved track %d: %s", k + 1, strrchr(path, '/') + 1); scan_files(); }
}

/* print the mix (with its FX) into track k; the tracks that went into it are muted */
static void resample(int k)
{
    if (any_recording()) { toast(1, "Stop recording first"); return; }
    busy_note("Rendering...");
    begin_busy();
    Mixdown *m = calloc(1, sizeof *m);
    int n = m ? eng_mix_begin(&G.eng, m, trk(k)->cap) : 0;
    float *b = n ? malloc(sizeof(float) * 2 * (size_t)n) : NULL;
    if (b) for (int done = 0; done < n; done += 4096) {
        int c = n - done < 4096 ? n - done : 4096;
        eng_mix_render(&G.eng, m, b + 2 * (size_t)done, NULL, c);
    }
    int used[CD_TRACKS] = { 0 };
    if (m) { for (int j = 0; j < CD_TRACKS; j++) used[j] = m->on[j] && !(m->mute[j]); if (n) eng_mix_end(m); free(m); }
    end_busy();
    if (!n) { toast(1, "Nothing to resample"); return; }
    if (!b) { toast(1, "Resample failed (out of memory?)"); return; }
    SDL_LockAudioDevice(G.out_dev);
    for (int j = 0; j < CD_TRACKS; j++) if (used[j] && j != k) trk(j)->p[P_MUTE] = 1;
    Track *t = trk(k);
    t->p[P_MUTE] = 0; t->p[P_PITCH] = 0; t->p[P_FINE] = 0; t->p[P_REVERSE] = 0; t->p[P_VOLUME] = 1; t->p[P_PAN] = 0;
    eng_set_buffer(&G.eng, k, b, n);
    eng_reset_fx_params(G.eng.p);
    fx_reset(&G.eng.fx, G.eng.p);
    t->playing = 1;
    SDL_UnlockAudioDevice(G.out_dev);
    free(b);
    toast(0, "Mix resampled into track %d (FX reset, the others muted)", k + 1);
}

static void load_file(int i)
{
    if (i < 0 || i >= G.nfiles) return;
    char path[800]; snprintf(path, sizeof path, "%s/%s", G.rec_dir, G.files[i]);
    Track *t = trk(G.sel);
    if (t->rec_state == ST_REC) { toast(1, "Track %d is recording", G.sel + 1); return; }
    int n; float *b = wav_read(path, CD_SR, t->cap, &n);
    if (!b) { toast(1, "Can't read %s", G.files[i]); return; }
    SDL_LockAudioDevice(G.out_dev);
    eng_set_buffer(&G.eng, G.sel, b, n);
    SDL_UnlockAudioDevice(G.out_dev);
    free(b);
    eng_play_toggle(&G.eng, G.sel);
    toast(0, "Track %d: %s (%.1fs)", G.sel + 1, G.files[i], n / (double)CD_SR);
}

static void play_all(void)
{
    int any = 0;
    for (int k = 0; k < CD_TRACKS; k++) any |= trk(k)->playing;
    for (int k = 0; k < CD_TRACKS; k++)
        if (trk(k)->len > 0 && trk(k)->rec_state != ST_REC && trk(k)->playing == any) eng_play_toggle(&G.eng, k);
    toast(0, any ? "All tracks stopped" : "All tracks playing");
}

static void do_undo(int k)
{
    SDL_LockAudioDevice(G.out_dev);
    int u = eng_undo(&G.eng, k);
    SDL_UnlockAudioDevice(G.out_dev);
    int left = eng_undo_count(&G.eng, k);
    if (!u) toast(1, "Track %d: nothing to undo", k + 1);
    else toast(0, "Track %d: %s (%d more undo%s)", k + 1, u == UNDO_ERASE ? "loop back (stopped), B plays it" : "undid overdub",
               left, left == 1 ? "" : "s");
}

static void do_action(int act, int k)
{
    Track *t = trk(k);
    switch (act) {
    case ACT_REC: eng_rec_button(&G.eng, k); break;
    case ACT_PLAY: eng_play_toggle(&G.eng, k); break;
    case ACT_DUB: eng_overdub_toggle(&G.eng, k); break;
    case ACT_ERASE:
        if (t->len > 0 || t->rec_state != ST_IDLE) { eng_erase(&G.eng, k); toast(0, "Track %d erased", k + 1); }
        break;
    case ACT_UNDO: do_undo(k); break;
    case ACT_REVERSE: t->p[P_REVERSE] = t->p[P_REVERSE] > 0.5f ? 0.f : 1.f; break;
    case ACT_MUTE: t->p[P_MUTE] = t->p[P_MUTE] > 0.5f ? 0.f : 1.f; toast(0, "Track %d %s", k + 1, t->p[P_MUTE] > 0.5f ? "muted" : "unmuted"); break;
    case ACT_NEXT: G.sel = (G.sel + 1) % CD_TRACKS; break;
    case ACT_PREV: G.sel = (G.sel + CD_TRACKS - 1) % CD_TRACKS; break;
    case ACT_SEL: G.sel = k; break;
    case ACT_SAVE_ALL: save_all(); break;
    case ACT_SAVE_TRACK: save_track(k); break;
    case ACT_RESAMPLE: resample(k); break;
    case ACT_PLAY_ALL: play_all(); break;
    case ACT_FX:
        if (t->p[P_FX_SEND] > 0.f) { G.send_mem[k] = t->p[P_FX_SEND]; t->p[P_FX_SEND] = 0.f; }
        else t->p[P_FX_SEND] = G.send_mem[k] > 0.f ? G.send_mem[k] : 1.f;
        toast(0, "Track %d FX send %s", k + 1, t->p[P_FX_SEND] > 0.f ? "on" : "off (dry)");
        break;
    case ACT_VERB:
        if (t->p[P_VERB_SEND] > 0.f) { G.verb_mem[k] = t->p[P_VERB_SEND]; t->p[P_VERB_SEND] = 0.f; }
        else t->p[P_VERB_SEND] = G.verb_mem[k] > 0.f ? G.verb_mem[k] : 0.5f;
        toast(0, "Track %d reverb send %s", k + 1, t->p[P_VERB_SEND] > 0.f ? "on" : "off");
        break;
    }
}

/* ------------------------------------------------ the loop window */

static void win_set(Track *t, float s, float z)
{
    float zmin = t->len > 0 ? fmaxf(1.f / 256, (float)(CD_SR / 50) / t->len) : 1.f / 256;
    if (z < zmin) z = zmin;
    if (z > 1) z = 1;
    if (s < 0) s = 0;
    if (s > 1 - z) s = 1 - z;
    t->p[P_LOOP_START] = s; t->p[P_LOOP_END] = s + z;
}

static void live_set(int what, int k, float v)   /* v 0..1 (bend: 0.5 = none) */
{
    Track *t = trk(k);
    float s = t->p[P_LOOP_START], z = t->p[P_LOOP_END] - s;
    switch (what) {
    case LIVE_WPOS: win_set(t, v * (1 - z), z); break;
    case LIVE_WSIZE: {
        /* 1/64 .. the whole track; synced tracks in halves, so the window stays in time */
        float e = (1 - v) * 6;
        z = eng_track_synced(&G.eng, k) ? powf(2.f, -floorf(e + 0.5f)) : powf(2.f, -e);
        win_set(t, s, z);
        break;
    }
    case LIVE_BEND: {
        float d = v - 0.5f;
        t->bend = fabsf(d) < 0.01f ? 0.f : d * 24.f;
        break;
    }
    case LIVE_DIR: t->p[P_REVERSE] = v < 0.5f ? 1.f : 0.f; break;
    }
}

/* ------------------------------------------------------------- MIDI */

static void midi_apply(int i, const MidiMsg *m)
{
    Target *g = &G.tg[i];
    Bind *b = &G.bind[i];
    int k = g->trk >= 0 ? g->trk : G.sel;
    float v = m->type == MM_PB ? m->val / 16383.f : m->val / 127.f;
    int on = m->type == MM_NOTE ? 1 : m->type == MM_NOTE_OFF ? 0 : v >= 0.5f;
    int rise = on && !b->hi;
    b->hi = (uint8_t)on;
    switch (g->kind) {
    case MT_ACT: if (rise) do_action(g->id, k); break;
    case MT_PARAM: {
        const ParamDesc *d = &PD[g->id];
        float *dst = g->id < P_TRACK_N ? &trk(k)->p[g->id] : &G.eng.p[g->id];
        if (m->type == MM_NOTE || m->type == MM_NOTE_OFF) {
            /* a pad steps through a switch's values */
            if (rise && d->kind == K_ENUM) *dst = (float)(((int)*dst + 1) % ((int)d->max + 1));
            else if (rise && g->id == P_FX_SEND) do_action(ACT_FX, k);   /* a pad switches the send */
            else if (rise && g->id == P_VERB_SEND) do_action(ACT_VERB, k);
        } else {
            float x = d->min + v * (d->max - d->min);
            if (d->kind == K_ENUM || d->kind == K_INT) x = floorf(x + 0.5f);
            *dst = clamp_param(g->id, x);
            if (g->id == P_LOOP_START || g->id == P_LOOP_END) {
                Track *t = trk(k);
                if (t->p[P_LOOP_END] < t->p[P_LOOP_START] + 0.001f) {
                    if (g->id == P_LOOP_START) t->p[P_LOOP_START] = t->p[P_LOOP_END] - 0.001f;
                    else t->p[P_LOOP_END] = t->p[P_LOOP_START] + 0.001f;
                }
            }
        }
        break;
    }
    case MT_LIVE:
        if (m->type == MM_NOTE || m->type == MM_NOTE_OFF) { if (rise && g->id == LIVE_DIR) do_action(ACT_REVERSE, k); }
        else live_set(g->id, k, v);
        break;
    }
}

static void midi_tick(void)
{
    MidiMsg m;
    while (midi_poll(&m)) {
        char b[48]; Bind tmp = { m.type == MM_NOTE_OFF ? MM_NOTE : m.type, m.ch, m.num, 0 };
        bind_str(&tmp, b, sizeof b);
        snprintf(G.midi_last, sizeof G.midi_last, "%s%s", b, m.type == MM_CC || m.type == MM_PB ? "" : m.type == MM_NOTE ? " on" : " off");
        if (G.learn >= 0) {
            if (m.type != MM_NOTE && m.type != MM_CC && m.type != MM_PB) continue;
            /* one control, one target */
            for (int i = 0; i < G.ntg; i++) {
                Bind *o = &G.bind[i];
                if (o->type == m.type && o->ch == m.ch && (m.type == MM_PB || o->num == m.num)) memset(o, 0, sizeof *o);
            }
            Bind *nb = &G.bind[G.learn];
            nb->type = m.type; nb->ch = m.ch; nb->num = m.type == MM_PB ? 0 : m.num; nb->hi = 0;
            bind_str(nb, b, sizeof b);
            toast(0, "%s  <-  %s", G.tg[G.learn].name, b);
            G.learn = -1;
            continue;
        }
        int type = m.type == MM_NOTE_OFF ? MM_NOTE : m.type;
        for (int i = 0; i < G.ntg; i++) {
            Bind *o = &G.bind[i];
            if (o->type == type && o->ch == m.ch && (type == MM_PB || o->num == m.num)) midi_apply(i, &m);
        }
    }
}

/* ----------------------------------------------------- the sticks */

/* left stick: x scrubs the window along the track, y doubles / halves it.
 * right stick: y bends the pitch (springs back), x flicks the direction. */
static void sticks_tick(void)
{
    Uint32 now = SDL_GetTicks();
    float dt = G.last_tick ? (now - G.last_tick) / 1000.f : 0.f;
    if (dt > 0.1f) dt = 0.1f;
    G.last_tick = now;
    if (!G.have_gc) return;
    Track *t = trk(G.sel);
    /* scrub: full tilt moves the window 60% of the track a second */
    float x = G.ax[0];
    if (fabsf(x) > 0.15f && t->len > 0) {
        float s = t->p[P_LOOP_START], z = t->p[P_LOOP_END] - s;
        float sp = (fabsf(x) - 0.15f) / 0.85f;
        win_set(t, s + (x > 0 ? 1 : -1) * sp * sp * 0.6f * dt, z);
    }
    /* flicks: past 60% once, then back under 30% before the next */
    for (int ax = 1; ax <= 2; ax++) {
        float v = G.ax[ax];
        int f = v > 0.6f ? 1 : v < -0.6f ? -1 : 0;
        if (f && !G.flick[ax] && t->len > 0) {
            if (ax == 1) {
                float s = t->p[P_LOOP_START], z = t->p[P_LOOP_END] - s;
                win_set(t, s, f < 0 ? z * 2 : z / 2);
                toast(0, "Track %d window: %.1f%% of the track", G.sel + 1, (t->p[P_LOOP_END] - t->p[P_LOOP_START]) * 100);
            } else t->p[P_REVERSE] = f < 0 ? 1.f : 0.f;
        }
        if (f) G.flick[ax] = 1;
        else if (fabsf(v) < 0.3f) G.flick[ax] = 0;
    }
    /* bend: +-12 semitones, back to none when the stick is let go */
    float y = -G.ax[3];
    float bend = fabsf(y) < 0.12f ? 0.f : (y - (y > 0 ? 0.12f : -0.12f)) / 0.88f * 12.f;
    if (G.bend_on && (bend == 0.f || G.bend_trk != G.sel)) { trk(G.bend_trk)->bend = 0.f; G.bend_on = 0; }
    if (bend != 0.f) { t->bend = bend; G.bend_trk = G.sel; G.bend_on = 1; }
}

/* -------------------------------------------------------- the UI */

static void adjust(int id, int dir)
{
    const ParamDesc *d = &PD[id];
    float step = G.held[A_R2] ? d->coarse : d->step;
    if (G.held[A_R2]) G.trig_used[A_R2] = 1;
    if (G.held[A_L2]) G.trig_used[A_L2] = 1;
    if (G.held[A_L2] && d->kind != K_ENUM && d->kind != K_INT) step *= 0.1f;
    float *p = pp(id), v = *p + dir * step;
    if (d->kind == K_ENUM) {
        int n = (int)d->max + 1;
        v = (float)(((int)*p + dir + n) % n);
    }
    v = clamp_param(id, v);
    if (id == P_LOOP_START && v > *pp(P_LOOP_END) - 0.001f) v = *pp(P_LOOP_END) - 0.001f;
    if (id == P_LOOP_END && v < *pp(P_LOOP_START) + 0.001f) v = *pp(P_LOOP_START) + 0.001f;
    if (fabsf(v) < 1e-6f) v = 0;
    *p = v < d->min ? d->min : v;
}

static int row_count(int pg) { return pg == PG_FILES ? G.nfiles : pg == PG_MIDI ? G.ntg : page_count(pg); }

static void move_sel(int dir)
{
    int n = row_count(G.page);
    if (n <= 0) return;
    int s = G.sel_row[G.page] + dir;
    s = s < 0 ? n - 1 : s >= n ? 0 : s;
    G.sel_row[G.page] = s;
    if (s < G.scroll[G.page]) G.scroll[G.page] = s;
    if (s >= G.scroll[G.page] + ROWS_VISIBLE) G.scroll[G.page] = s - ROWS_VISIBLE + 1;
    G.del_confirm = 0;
}

static void press(int a)
{
    Uint32 now = SDL_GetTicks();
    if (now > G.confirm_until) { G.del_confirm = 0; G.clear_confirm = 0; }
    if (a == A_L2 || a == A_R2) G.trig_used[a] = 0;

    if (G.held[A_SEL] && a != A_SEL) {
        switch (a) {
        case A_START: save_all(); break;
        case A_Y: resample(G.sel); break;
        case A_B: do_undo(G.sel); break;
        case A_X:
            if (!G.clear_confirm) { G.clear_confirm = 1; G.confirm_until = now + 2500; toast(1, "Press SELECT+X again to erase track %d", G.sel + 1); }
            else { do_action(ACT_ERASE, G.sel); G.clear_confirm = 0; }
            break;
        case A_A: save_track(G.sel); break;
        case A_L2: G.trig_used[A_L2] = 1; do_action(ACT_MUTE, G.sel); break;
        case A_R2: G.trig_used[A_R2] = 1; play_all(); break;
        case A_UP: do_action(ACT_FX, G.sel); break;
        case A_DOWN: do_action(ACT_VERB, G.sel); break;
        case A_MENU: G.running = 0; break;
        case A_L1: case A_R1:
            if (G.held[A_L1] && G.held[A_R1]) G.running = 0;
            break;
        }
        return;
    }

    int row = G.page >= PG_MIDI ? 0 : PAGE_ROWS[G.page][G.sel_row[G.page]];
    switch (a) {
    case A_MENU: G.running = 0; break;
    case A_L1: G.page = (G.page + PG_N - 1) % PG_N; G.learn = -1; if (G.page == PG_FILES) scan_files(); break;
    case A_R1: G.page = (G.page + 1) % PG_N; G.learn = -1; if (G.page == PG_FILES) scan_files(); break;
    case A_UP: move_sel(-1); break;
    case A_DOWN: move_sel(1); break;
    case A_LEFT: case A_RIGHT: {
        int dir = a == A_RIGHT ? 1 : -1;
        if (G.page == PG_FILES || G.page == PG_MIDI) { move_sel(dir * ROWS_VISIBLE); break; }
        if (row >= 0) adjust(row, dir);
        else if (row == ROW_IN_DEV) cycle_device(1, dir);
        else if (row == ROW_OUT_DEV) cycle_device(0, dir);
        break;
    }
    case A_A:
        if (G.page == PG_FILES) load_file(G.sel_row[PG_FILES]);
        else if (G.page == PG_MIDI) {
            G.learn = G.sel_row[PG_MIDI];
            toast(0, "Move a knob or hit a pad for: %s", G.tg[G.learn].name);
        }
        else do_action(ACT_REC, G.sel);
        break;
    case A_B:
        if (G.page == PG_MIDI && G.learn >= 0) { G.learn = -1; toast(0, "Learning cancelled"); break; }
        G.b_erased = 0; do_action(ACT_PLAY, G.sel);
        break;
    case A_X: do_action(ACT_DUB, G.sel); break;
    case A_Y:
        if (G.page == PG_FILES) {
            if (!G.nfiles) break;
            if (!G.del_confirm) { G.del_confirm = 1; G.confirm_until = now + 2500; toast(1, "Press Y again to delete %s", G.files[G.sel_row[PG_FILES]]); }
            else {
                char p[800]; snprintf(p, sizeof p, "%s/%s", G.rec_dir, G.files[G.sel_row[PG_FILES]]);
                if (remove(p) == 0) { sync(); toast(0, "Deleted"); } else toast(1, "Delete failed");
                G.del_confirm = 0; scan_files();
            }
        } else if (G.page == PG_MIDI) {
            int i = G.sel_row[PG_MIDI];
            memset(&G.bind[i], 0, sizeof G.bind[i]); G.learn = -1;
            toast(0, "Unmapped %s", G.tg[i].name);
        } else do_action(ACT_REVERSE, G.sel);
        break;
    case A_START: save_all(); break;
    }
}

/* L2 / R2 tapped on their own pick the track (held, they're fine / coarse for the d-pad) */
static void release(int a)
{
    if ((a == A_L2 || a == A_R2) && !G.trig_used[a] && !G.held[A_SEL] && SDL_GetTicks() - G.held_since[a] < 600)
        do_action(a == A_L2 ? ACT_PREV : ACT_NEXT, G.sel);
}

static void set_held(int a, int down)
{
    if (a < 0 || a >= A_N) return;
    if (down && !G.held[a]) {
        G.held[a] = 1;
        G.held_since[a] = SDL_GetTicks();
        G.next_rep[a] = G.held_since[a] + 320;
        press(a);
    } else if (!down && G.held[a]) { G.held[a] = 0; release(a); }
}

static void repeat_tick(void)
{
    Uint32 now = SDL_GetTicks();
    /* hold B for a second: stop and erase the selected track (no new recording) */
    if (G.held[A_B] && !G.held[A_SEL] && !G.b_erased && G.page != PG_FILES && G.page != PG_MIDI && now - G.held_since[A_B] >= 1000) {
        G.b_erased = 1;
        do_action(ACT_ERASE, G.sel);
    }
    for (int a = A_UP; a <= A_RIGHT; a++)
        if (G.held[a] && !G.held[A_SEL] && now >= G.next_rep[a]) {
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
    case SDL_CONTROLLERAXISMOTION: {
        float v = e->caxis.value / 32767.f;
        switch (e->caxis.axis) {
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: set_held(A_L2, e->caxis.value > 16000); break;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: set_held(A_R2, e->caxis.value > 16000); break;
        case SDL_CONTROLLER_AXIS_LEFTX: G.ax[0] = v; break;
        case SDL_CONTROLLER_AXIS_LEFTY: G.ax[1] = v; break;
        case SDL_CONTROLLER_AXIS_RIGHTX: G.ax[2] = v; break;
        case SDL_CONTROLLER_AXIS_RIGHTY: G.ax[3] = v; break;
        }
        break;
    }
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

static void update_peaks(int k)
{
    Track *t = trk(k);
    Uint32 now = SDL_GetTicks();
    if (t->version == G.pk_version[k] && t->len == G.pk_len[k]) return;
    if (now - G.pk_time[k] < 70 && t->len == G.pk_len[k]) return;
    G.pk_version[k] = t->version; G.pk_len[k] = t->len; G.pk_time[k] = now;
    int len = t->len;
    for (int c = 0; c < LANE_W; c++) {
        long a = (long)c * len / LANE_W, b = (long)(c + 1) * len / LANE_W;
        if (b <= a) b = a + 1;
        long step = (b - a) / 48; if (step < 1) step = 1;
        float m = 0;
        for (long i = a; i < b && i < len; i += step) {
            float l = fabsf(t->buf[2 * i]), r = fabsf(t->buf[2 * i + 1]);
            if (l > m) m = l;
            if (r > m) m = r;
        }
        G.peak[k][c] = m;
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
    const ParamDesc *d = &PD[id]; float v = *pp(id);
    switch (d->kind) {
    case K_ENUM: snprintf(o, sz, "%s", d->labels[(int)v]); break;
    case K_PCT:
        if (id == P_VOLUME && v > 1.001f) snprintf(o, sz, "%.0f%%  (warm soft clip if it would distort)", v * 100);
        else snprintf(o, sz, "%.0f%%", v * 100);
        break;
    case K_SEND:
        if (v < 0.005f) snprintf(o, sz, "Off  (dry, past the FX)");
        else if (v > 0.995f) snprintf(o, sz, "100%%  (all through the FX)");
        else snprintf(o, sz, "%.0f%%  (the rest dry)", v * 100);
        break;
    case K_SEC: snprintf(o, sz, "%.1f s", v); break;
    case K_PAN:
        if (fabsf(v) < 0.005f) snprintf(o, sz, "Center");
        else snprintf(o, sz, "%s %.0f", v < 0 ? "Left" : "Right", fabsf(v) * 100);
        break;
    case K_DECAY:
        if (v < 0.0001f) snprintf(o, sz, "Off  (layers add up)");
        else snprintf(o, sz, "%.*f%% per pass  (%.2f dB)", v < 0.01f ? 2 : 1, v * 100, 20 * log10f(1 - v));
        break;
    case K_LOOPPOS: { char t[32]; fmt_time(t, sizeof t, v * trk(G.sel)->len / CD_SR); snprintf(o, sz, "%s  (%.1f%%)", t, v * 100); } break;
    case K_ST: snprintf(o, sz, "%+.0f st  x%.3f", v, eng_rate(trk(G.sel)->p, 0)); break;
    case K_CENTS: snprintf(o, sz, "%+.0f ct", v); break;
    case K_DB: snprintf(o, sz, "%.0f dB", v); break;
    case K_GAIN: snprintf(o, sz, "%+.0f dB", v); break;
    case K_MS:
        if (id == P_DUB_ALIGN) snprintf(o, sz, "%+.0f ms%s", v, v == 0 ? "  (auto)" : v > 0 ? "  (earlier)" : "  (later)");
        else if (id == P_CLOCK_OFS) snprintf(o, sz, "%+.0f ms%s", v, v == 0 ? "" : v > 0 ? "  (loops earlier)" : "  (loops later)");
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

static const char *track_state(int k, Uint32 *c)
{
    Track *t = trk(k);
    Uint32 now = SDL_GetTicks();
    if (t->rec_state == ST_REC) { *c = C_REC; return t->stop_cyc > 0 ? "REC>|" : "REC"; }
    if (t->rec_state == ST_ARMED) { *c = (now / 300) & 1 ? C_REC : C_DIM; return "ARMED"; }
    if (t->overdub) { *c = C_DUB; return "DUB"; }
    if (t->playing && eng_track_synced(&G.eng, k) && !G.eng.tr.running) { *c = C_ACCENT; return "WAIT"; }
    if (t->playing) { *c = C_PLAY; return t->p[P_MUTE] > 0.5f ? "MUTE" : "PLAY"; }
    if (t->len > 0) { *c = C_DIM; return "STOP"; }
    *c = C_DIM; return "EMPTY";
}

static void draw_lane(int k)
{
    Track *t = trk(k);
    int y = LANE_Y + k * LANE_STEP, on = k == G.sel;
    Uint32 sc; const char *st = track_state(k, &sc);
    /* number box */
    fill(16, y, 76, LANE_H, on ? C_PANEL2 : C_PANEL, 255);
    if (on) fill(16, y, 5, LANE_H, C_ACCENT, 255);
    textf(32, y + 2, on ? C_ACCENT : C_TEXT, 1.4f, "%d", k + 1);
    text(30, y + 42, sc, 0.7f, st);
    if (t->p[P_MUTE] > 0.5f) text(64, y + 6, C_REC, 0.8f, "M");
    /* waveform */
    fill(LANE_X, y, LANE_W, LANE_H, on ? C_PANEL2 : C_PANEL, 255);
    int mid = y + LANE_H / 2;
    if (t->len > 1) {
        update_peaks(k);
        int ls, le; eng_loop_range(&G.eng, k, &ls, &le);
        int xs = LANE_X + (int)((long)ls * LANE_W / t->len), xe = LANE_X + (int)((long)le * LANE_W / t->len);
        Uint32 wc = t->rec_state == ST_REC ? C_REC : t->overdub ? C_DUB : t->p[P_MUTE] > 0.5f ? C_DIM : C_WAVE;
        for (int c = 0; c < LANE_W; c++) {
            int x = LANE_X + c, h = (int)(sqrtf(G.peak[k][c]) * (LANE_H / 2 - 3));
            int inside = t->rec_state == ST_REC || (x >= xs && x < xe);
            col(inside ? wc : C_DIM, inside ? 255 : 90);
            SDL_RenderDrawLine(G.ren, x, mid - h, x, mid + h);
        }
        if (t->rec_state != ST_REC) {
            /* bar lines of a synced take */
            if (eng_track_synced(&G.eng, k) && t->cycles >= 1 && t->cycles <= 32)
                for (int b = 1; b < (int)t->cycles; b++) fill(LANE_X + (int)(LANE_W * b / t->cycles), y, 1, LANE_H, C_DIM, 120);
            if (xs > LANE_X || xe < LANE_X + LANE_W - 2) {
                fill(xs - 1, y, 2, LANE_H, C_ACCENT, 255);
                fill(xe - 1, y, 2, LANE_H, C_ACCENT, 255);
            }
            if (t->playing) {
                int px = LANE_X + (int)((long)t->pos_i * LANE_W / t->len);
                fill(px - 1, y, 2, LANE_H, C_TEXT, 230);
            }
        }
        char info[96], len[32];
        if (t->rec_state == ST_REC) {
            char tm[32]; fmt_time(tm, sizeof tm, t->rec_pos / (double)CD_SR);
            snprintf(info, sizeof info, "%s%s", tm, t->stop_cyc > 0 ? "  ends on the bar" : "");
        } else {
            describe_len(len, sizeof len, le - ls, eng_track_synced(&G.eng, k) ? t->cyc_rec : 0);
            float pan = t->p[P_PAN];
            char pn[16];
            if (fabsf(pan) < 0.005f) snprintf(pn, sizeof pn, "C");
            else snprintf(pn, sizeof pn, "%c%.0f", pan < 0 ? 'L' : 'R', fabsf(pan) * 100);
            char fx[40] = "";
            if (t->p[P_FX_SEND] < 0.005f) snprintf(fx, sizeof fx, "dry  ");
            else if (t->p[P_FX_SEND] < 0.995f) snprintf(fx, sizeof fx, "fx %.0f%%  ", t->p[P_FX_SEND] * 100);
            if (t->p[P_VERB_SEND] > 0.005f) { size_t fl = strlen(fx); snprintf(fx + fl, sizeof fx - fl, "rev %.0f%%  ", t->p[P_VERB_SEND] * 100); }
            snprintf(info, sizeof info, "%s%s  %s%s%svol %.0f%%  %s", len, eng_track_synced(&G.eng, k) ? "" : " free",
                     t->p[P_REVERSE] > 0.5f ? "rev " : "", t->bend != 0.f ? "bend " : "", fx, t->p[P_VOLUME] * 100, pn);
        }
        int iw = (int)(strlen(info) * FONT_CW * 0.75f);
        fill(LANE_X + LANE_W - 10 - iw, y + 1, iw + 10, (int)(FONT_CH * 0.75f) + 2, C_BG, 190);
        text_right(LANE_X + LANE_W - 5, y + 2, C_TEXT, 0.75f, info);
    } else {
        const char *m = "";
        if (t->rec_state == ST_ARMED) {
            if (!t->arm_sync) m = "Waiting for signal above threshold...";
            else if (G.eng.p[P_CLOCK] > 0.5f && !G.eng.tr.running) m = "Waiting for MIDI clock (Start)...";
            else m = "Starts on the next bar...";
        } else if (on) m = "A records here";
        text(LANE_X + 16, mid - 10, C_DIM, 0.9f, m);
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
    Uint32 sc; const char *st = track_state(G.sel, &sc);
    fill(150, 12, 150, 32, sc, 50);
    textf(150 + (150 - ((int)strlen(st) + 3) * FONT_CW) / 2, 16, sc, 1.f, "%d: %s", G.sel + 1, st);
    {
        /* transport: the beat, and where the bar comes from */
        int beats = (int)e->p[P_BEATS]; if (beats < 1) beats = 1;
        int shown = beats > 8 ? 8 : beats;
        double ph = e->tr.phase;
        int lit = e->tr.running ? (int)((ph - floor(ph)) * beats) : -1;
        for (int b = 0; b < shown; b++)
            fill(316 + b * 18, 22, 14, 14, b == lit ? (b == 0 ? C_ACCENT : C_PLAY) : C_PANEL2, 255);
        char tb[64];
        if (e->p[P_CLOCK] > 0.5f) {
            if (e->tr.bpm > 0) snprintf(tb, sizeof tb, "MIDI %.1f BPM%s", e->tr.bpm, e->tr.running ? "" : " (stopped)");
            else snprintf(tb, sizeof tb, "MIDI: no clock");
        } else if (e->tr.cyc > 0) snprintf(tb, sizeof tb, "bar %.2f s (first loop)", e->tr.cyc / CD_SR);
        else snprintf(tb, sizeof tb, "first loop sets the bar");
        text(316 + shown * 18 + 8, 16, e->tr.running ? C_TEXT : C_DIM, 0.9f, tb);
    }
    char nm[40]; fit(nm, sizeof nm, G.in_open, 22);
    text_right(1008, 18, G.in_failed ? C_REC : C_DIM, 0.8f, nm);

    for (int k = 0; k < CD_TRACKS; k++) draw_lane(k);

    /* meters */
    meter(16, 356, 488, "IN", e->in_peak);
    meter(520, 356, 488, "OUT", e->out_peak);

    /* tabs */
    for (int i = 0; i < PG_N; i++) {
        int x = 16 + i * 166, on = i == G.page;
        fill(x, 384, 160, 38, on ? C_PANEL2 : C_PANEL, 255);
        if (on) fill(x, 418, 160, 4, C_ACCENT, 255);
        char nmt[24];
        if (i == PG_TRACK) snprintf(nmt, sizeof nmt, "TRACK %d", G.sel + 1); else snprintf(nmt, sizeof nmt, "%s", PAGE_NAME[i]);
        text(x + (160 - (int)strlen(nmt) * FONT_CW) / 2, 391, on ? C_TEXT : C_DIM, 1.f, nmt);
    }

    /* rows */
    int y0 = 432, rh = 36;
    int n = row_count(G.page);
    int sc0 = G.scroll[G.page];
    if (G.page == PG_FILES && n == 0) textf(32, y0 + 10, C_DIM, 1.f, "No recordings yet in %s", G.rec_dir);
    for (int k = 0; k < ROWS_VISIBLE && sc0 + k < n; k++) {
        int i = sc0 + k, y = y0 + k * rh, on = i == G.sel_row[G.page];
        if (on) { fill(16, y, 992, rh - 4, C_PANEL2, 255); fill(16, y, 5, rh - 4, C_ACCENT, 255); }
        char v[300], nm2[120];
        if (G.page == PG_FILES) {
            fit(nm2, sizeof nm2, G.files[i], 60);
            text(32, y + 4, on ? C_TEXT : C_DIM, 1.f, nm2);
            snprintf(v, sizeof v, "%.1f MB", G.fsize[i] / 1048576.0);
            text_right(992, y + 4, C_DIM, 1.f, v);
            continue;
        }
        if (G.page == PG_MIDI) {
            text(32, y + 4, on ? C_TEXT : C_DIM, 1.f, G.tg[i].name);
            if (G.learn == i) snprintf(v, sizeof v, "%s", (now / 400) & 1 ? "move a control..." : "");
            else bind_str(&G.bind[i], v, sizeof v);
            text(600, y + 4, G.learn == i ? C_ACCENT : G.bind[i].type ? (on ? C_ACCENT : C_TEXT) : C_DIM, 1.f, v);
            continue;
        }
        int id = PAGE_ROWS[G.page][i];
        const char *label = id >= 0 ? PD[id].name : id == ROW_IN_DEV ? "Input device"
                          : id == ROW_OUT_DEV ? "Output device" : id == ROW_FOLDER ? "Save folder"
                          : id == ROW_TEMPO ? "Tempo" : id == ROW_MIDI_DEV ? "MIDI in" : "Diagnostics";
        text(32, y + 4, on ? C_TEXT : C_DIM, 1.f, label);
        if (id >= 0) {
            value_str(id, v, sizeof v);
            text(300, y + 4, on ? C_ACCENT : C_TEXT, 1.f, v);
            const ParamDesc *d = &PD[id];
            if (d->kind != K_ENUM && d->kind != K_DECAY) {
                float t = (*pp(id) - d->min) / (d->max - d->min);
                fill(700, y + 12, 292, 8, C_PANEL, 255);
                fill(700, y + 12, (int)(292 * t), 8, on ? C_ACCENT : C_DIM, 255);
            }
        } else {
            char md[200];
            if (id == ROW_IN_DEV) fit(v, sizeof v, G.in_name[0] ? G.in_name : "Auto (USB first, else default)", 56);
            else if (id == ROW_OUT_DEV) fit(v, sizeof v, G.out_name[0] ? G.out_name : "System default", 56);
            else if (id == ROW_FOLDER) fit(v, sizeof v, G.rec_dir, 56);
            else if (id == ROW_TEMPO) {
                if (e->tr.bpm > 0) snprintf(v, sizeof v, "MIDI clock %.1f BPM, %s%s", e->tr.bpm, e->tr.ext_run ? "playing" : "stopped",
                                            e->p[P_CLOCK] > 0.5f ? "" : "  (not followed)");
                else if (e->tr.cyc > 0) snprintf(v, sizeof v, "bar %.3f s (first loop)", e->tr.cyc / CD_SR);
                else snprintf(v, sizeof v, "no bar yet: the first synced loop sets it");
            } else if (id == ROW_MIDI_DEV) {
                midi_devices(md, sizeof md);
                fit(v, sizeof v, md[0] ? md : "none (plug in a USB MIDI device)", 56);
            }
            else snprintf(v, sizeof v, "%s | in %.1f kHz | %d in devs | xrun %u/%u | btn %d | %s",
                          SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
                          e->in_ratio * CD_SR / 1000.0, SDL_GetNumAudioDevices(1), (unsigned)e->overruns, (unsigned)e->underruns, G.last_raw_btn,
                          G.midi_last[0] ? G.midi_last : "no MIDI yet");
            text(300, y + 4, id == ROW_DIAG ? (on ? C_ACCENT : C_TEXT) : C_DIM, id == ROW_DIAG ? 0.8f : 1.f, v);
        }
    }
    if (n > ROWS_VISIBLE) {
        int th = ROWS_VISIBLE * rh * ROWS_VISIBLE / n;
        fill(1010, y0 + sc0 * (ROWS_VISIBLE * rh - th) / (n - ROWS_VISIBLE), 4, th, C_DIM, 200);
    }

    /* footer */
    fill(0, 728, SW, 40, C_PANEL, 255);
    const char *hint = G.page == PG_FILES ? "A load into track   Y delete   B play   L2/R2 track   L1/R1 tab"
        : G.page == PG_MIDI ? "A learn (move a knob / hit a pad)   Y unmap   B cancel   L1/R1 tab"
        : G.held[A_SEL] ? "SEL+ START all  A track  B undo  X erase  Y resamp  L2 mute  R2 all  UP fx  DN rev"
                        : "A rec  B play (hold: erase)  X dub  Y rev  L2/R2 track  START save  hold SELECT";
    text(16, 736, C_DIM, 0.9f, hint);

    /* toast */
    if (now < G.toast_until) {
        int w = (int)strlen(G.toast) * FONT_CW + 40; if (w > 980) w = 980;
        int x = (SW - w) / 2;
        fill(x, 290, w, 44, C_BG, 235);
        fill(x, 290, 4, 44, G.toast_bad ? C_REC : C_ACCENT, 255);
        char t[90]; fit(t, sizeof t, G.toast, 78);
        text(x + 20, 300, C_TEXT, 1.f, t);
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

static void tick_ui(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) handle_event(&ev);
    repeat_tick();
    midi_tick();
    sticks_tick();
    eng_ui_tick(&G.eng);
}

/* Scripted run used by `make test-e2e` with SDL's disk audio driver:
 * two tracks in sync -> FX -> overdub -> undo -> save mix + tracks ->
 * resample -> reload -> erase -> MIDI clock and mapping (when fed) */
static int selftest(const char *shot_dir)
{
#define WAITMS(ms) do { Uint32 _t = SDL_GetTicks() + (ms); while (SDL_GetTicks() < _t) { tick_ui(); if (G.ren) draw(); SDL_Delay(10); } } while (0)
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "SELFTEST FAIL: %s\n", msg); return 1; } else fprintf(stderr, "ok   %s\n", msg); } while (0)
#define SHOT(name) do { if (shot_dir) { snprintf(p, sizeof p, "%s/%s.ppm", shot_dir, name); screenshot(p); } } while (0)
    char p[700];
    Engine *e = &G.eng;
    Track *t1 = trk(0), *t2 = trk(1);
    CHECK(G.in_dev != 0, "capture device opened");

    /* track 1: the first loop sets the bar */
    G.sel = 0;
    do_action(ACT_REC, 0); WAITMS(150);
    CHECK(t1->rec_state == ST_REC, "track 1 recording");
    WAITMS(1000);
    SHOT("shot_rec");
    do_action(ACT_REC, 0); WAITMS(150);
    CHECK(t1->rec_state == ST_IDLE && t1->len > CD_SR / 2 && t1->playing, "track 1 take captured and looping");
    CHECK(e->tr.cyc > 0 && fabs(e->tr.cyc - (t1->len - CD_XFADE)) < 2 && e->tr.running, "first loop set the bar");
    float mx = 0; for (int i = 0; i < t1->len * 2; i++) if (fabsf(t1->buf[i]) > mx) mx = fabsf(t1->buf[i]);
    fprintf(stderr, "     bar %.3fs, peak %.3f\n", e->tr.cyc / CD_SR, mx);
    CHECK(mx > 0.05f, "take contains input signal");

    /* track 2: waits for the bar, stops on the bar (about 1.5 bars played -> 2 bars) */
    G.held[A_R2] = 1; G.held_since[A_R2] = SDL_GetTicks(); G.trig_used[A_R2] = 0; set_held(A_R2, 0);
    CHECK(G.sel == 1, "R2 tap selects track 2");
    do_action(ACT_REC, 1); WAITMS(60);
    CHECK(t2->rec_state == ST_ARMED || t2->rec_state == ST_REC, "track 2 armed for the next bar");
    { Uint32 t0 = SDL_GetTicks(); while (t2->rec_state != ST_REC && SDL_GetTicks() - t0 < 3000) WAITMS(5); }
    CHECK(t2->rec_state == ST_REC && fabs(t2->phase0 - floor(t2->phase0)) < 1e-9, "track 2 started on a bar line");
    WAITMS((Uint32)(1.5 * e->tr.cyc * 1000 / CD_SR));
    do_action(ACT_REC, 1);
    { Uint32 t0 = SDL_GetTicks(); while (t2->rec_state == ST_REC && SDL_GetTicks() - t0 < 4000) WAITMS(10); }
    int ls, le; eng_loop_range(e, 1, &ls, &le);
    fprintf(stderr, "     track 2: %d frames = %.3f bars\n", le - ls, (le - ls) / e->tr.cyc);
    CHECK(t2->rec_state == ST_IDLE && eng_track_synced(e, 1) && t2->cycles == 2 && fabs((le - ls) - 2 * e->tr.cyc) < 2,
          "track 2 is exactly 2 bars and in sync");
    WAITMS(200);
    CHECK(t2->playing, "track 2 loops");

    t2->p[P_PAN] = -0.6f;
    e->p[P_FILT_MODE] = 1; e->p[P_CUTOFF] = 0.6f; e->p[P_RESO] = 0.5f;
    e->p[P_DLY_SYNC] = 3; e->p[P_DLY_MIX] = 0.5f; e->p[P_BITS] = 8; e->p[P_DRIVE] = 0.3f;
    G.page = PG_FX;
    WAITMS(500);
    CHECK(e->out_peak > 0.01f, "processed mix is audible");
    SHOT("shot_fx");

    float before = t1->buf[2 * (t1->len / 3)];
    G.sel = 0;
    do_action(ACT_DUB, 0); WAITMS(700); do_action(ACT_DUB, 0); WAITMS(100);
    CHECK(eng_undo_count(e, 0) == 1, "overdub on track 1 recorded");
    SHOT("shot_dub");
    do_undo(0);
    CHECK(t1->buf[2 * (t1->len / 3)] == before, "undo restored track 1");

    /* save: the mix and both tracks, 2 bars each */
    int n0 = G.nfiles;
    save_all(); scan_files();
    CHECK(G.nfiles == n0 + 3, "saved the mix + 2 tracks");
    int found = 0;
    for (int i = 0; i < G.nfiles; i++)
        if (strstr(G.files[i], "_mix.wav")) {
            snprintf(p, sizeof p, "%s/%s", G.rec_dir, G.files[i]);
            int n; float *b = wav_read(p, CD_SR, 1 << 30, &n);
            fprintf(stderr, "     mix: %d frames = %.3f bars\n", n, n / e->tr.cyc);
            found = b && abs(n - (int)lround(2 * e->tr.cyc)) <= 1;
            free(b);
            break;
        }
    CHECK(found, "the mix is 2 bars long (what both tracks repeat in)");

    /* resample the mix into track 3 */
    G.sel = 2;
    resample(2);
    WAITMS(200);
    CHECK(trk(2)->len > 0 && trk(2)->playing && t1->p[P_MUTE] > 0.5f && t2->p[P_MUTE] > 0.5f && eng_track_synced(e, 2),
          "mix resampled into track 3, tracks 1-2 muted");
    t1->p[P_MUTE] = 0; t2->p[P_MUTE] = 0;

    /* load the newest file into track 4 */
    G.sel = 3; G.page = PG_FILES; scan_files();
    load_file(0); WAITMS(300);
    eng_loop_range(e, 3, &ls, &le);
    CHECK(trk(3)->len > 0 && trk(3)->playing && eng_track_synced(e, 3) && fabs((le - ls) - 2 * e->tr.cyc) < 2,
          "a saved 2-bar track loads into track 4 at exactly 2 bars, in sync");
    SHOT("shot_files");

    int kept = trk(3)->len;
    do_action(ACT_ERASE, 3); WAITMS(200);
    CHECK(trk(3)->len == 0 && !trk(3)->playing && trk(3)->rec_state == ST_IDLE, "erase empties track 4 without recording");
    do_undo(3);
    CHECK(trk(3)->len == kept, "undo brings it back");

    /* MIDI: the e2e script feeds 120 BPM clock and CC 7 */
    if (getenv("CONDUIT_MIDI_DEV")) {
        { Uint32 t0 = SDL_GetTicks(); while (e->tr.bpm <= 0 && SDL_GetTicks() - t0 < 6000) WAITMS(20); }
        fprintf(stderr, "     MIDI clock: %.2f BPM\n", e->tr.bpm);
        CHECK(fabs(e->tr.bpm - 120) < 1.0, "MIDI clock tempo measured (120 BPM)");
        e->p[P_CLOCK] = CLOCK_MIDI;
        WAITMS(500);
        CHECK(e->tr.running && fabs(e->tr.cyc - 2.0 * CD_SR) < 0.01 * CD_SR, "transport follows the clock (1 bar = 2.000 s)");
        G.page = PG_MIDI;
        int ti = -1;
        for (int i = 0; i < G.ntg; i++) if (!strcmp(G.tg[i].key, "t1_volume")) ti = i;
        G.sel_row[PG_MIDI] = ti; press(A_A);
        { Uint32 t0 = SDL_GetTicks(); while (G.learn >= 0 && SDL_GetTicks() - t0 < 4000) WAITMS(20); }
        CHECK(ti >= 0 && G.bind[ti].type == MM_CC && G.bind[ti].num == 7, "learned CC 7 for track 1 volume");
        float v0 = t1->p[P_VOLUME];
        WAITMS(800);
        fprintf(stderr, "     track 1 volume %.2f -> %.2f\n", v0, t1->p[P_VOLUME]);
        CHECK(t1->p[P_VOLUME] != v0, "CC 7 moves track 1 volume");
        SHOT("shot_midi");
        e->p[P_CLOCK] = CLOCK_INTERNAL;
    }
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
    build_targets();
    for (int k = 0; k < CD_TRACKS; k++) { G.send_mem[k] = 1.f; G.verb_mem[k] = 0.5f; }
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
    midi_start(&G.eng);
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
            tick_ui();
            draw();
            SDL_Delay(4);
        }

    save_cfg();
    midi_stop();
    if (G.in_dev) SDL_CloseAudioDevice(G.in_dev);
    if (G.out_dev) SDL_CloseAudioDevice(G.out_dev);
    SDL_DestroyTexture(G.font); SDL_DestroyRenderer(G.ren); SDL_DestroyWindow(G.win);
    SDL_Quit();
    eng_free(&G.eng);
    return rc;
}
