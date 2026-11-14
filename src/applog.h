/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Conduit's log, for troubleshooting on the TrimUI Brick, Brick Hammer and
 * Brick Pro: log.txt in the app folder (launch.sh keeps the last two runs as
 * log.1.txt and log.2.txt). Grown from HI5's and RadioScan's performance log
 * (perflog.c: the SYS, STAT, PROC, HINT, JUMP and END lines are theirs), with
 * what a looper on USB audio needs on top. Each line is the seconds since the
 * start, a tag and what happened:
 *
 *   LOG    the log's own: its level, a full log
 *   SYS    at start: MustardOS, the board, this build, the processor, the
 *          memory, where the binary is loaded (for reading a CRASH)
 *   MSG    what the app and its parts say (MIDI, the USB link, the updater):
 *          everything written to stderr, timestamped
 *   AUDIO  a sound device opened, and the output's callbacks stopping and
 *          coming back
 *   STAT   every 10 s (every second at DETAIL): the audio callback's load,
 *          dropouts, the longest wait between callbacks, the screen's frame
 *          time and rate, the processor's clock, temperature, cores, load
 *   PROC   with each STAT: the app's and its busiest threads' processor time,
 *          memory, page faults, how often the audio thread waited
 *   INPUT  with each STAT: presses, how long a press took to show on the
 *          screen (average and worst), the longest hold, time since the last
 *          press, MIDI messages in
 *   HW     every minute and when it changes: the battery (charge, charging,
 *          volts), every thermal zone, free space where recordings go
 *   USB, SOUND, PAD  a USB device, a sound card or an input device came or went
 *   STALL  the screen didn't update for 2 s (and what the app was doing), and
 *          when it came back
 *   SLEEP  the device was asleep (suspended) for a while
 *   HINT   what to change when something is wrong
 *   JUMP   the app's own output jumped far more than the sound around it
 *   KEY    every press and release (DETAIL only)
 *   CRASH  the app crashed: the signal, the address, the code's position and
 *          a backtrace (resolve them against the unstripped build,
 *          build/trimui/conduit.debug: addr2line -e conduit.debug 0x<offset>)
 *   SIGNAL the app was asked to close (SIGTERM, SIGINT, SIGHUP)
 *   END    at quit: why, and the session's worst figures
 * The app adds its own (main.c: ENGINE, TRACK, SET, PAGE, ERROR, NOTE).
 *
 * CONDUIT_LOG sets the level: 0 off (stderr as it is, crashes still logged),
 * 1 BASIC (the default), 2 DETAIL. The audio thread only calls
 * perf_audio_begin / perf_audio_end / perf_audio_output, which update a few
 * counters; a monitor thread watches the hardware and the main thread. */
#pragma once
#include <stdint.h>

enum { PERF_OFF, PERF_BASIC, PERF_DETAIL };

/* prefix: the settings prefix ("CONDUIT"), for <prefix>_LOG and the hints;
 * build: the build's name. Captures stderr, installs the crash and signal
 * handlers and starts the monitor. */
void perf_init(const char *prefix, const char *build);
int perf_level(void);
/* a sound device opened (or reopened): output or capture, rate, frames a
 * buffer, channels, its name */
void perf_audio_open(int capture, int rate, int frames, int channels, const char *device);
void perf_audio_closed(int capture);

/* audio thread: first and last thing in the output callback */
uint64_t perf_audio_begin(int frames);
void perf_audio_end(uint64_t t0);
/* audio thread: the float stereo just made, for the JUMP check */
void perf_audio_output(const float *s, int frames);

/* main thread */
void perf_frame(uint64_t us, uint32_t now_ms);    /* a frame took this long and is on the screen now (SDL ticks) */
void perf_tick(void);                             /* often: STAT, INPUT and HINT lines when due */
void perf_before_stat(void (*fn)(void));          /* called before each STAT line (the app's own lines) */
void perf_where(const char *what);                /* what the main thread is doing (a string that stays), for STALL */
/* a button: its number (< 32), name, down or up, when it happened (SDL ticks) */
void perf_input(int id, const char *name, int down, uint32_t ev_ms);
void perf_midi_in(int n);                         /* MIDI messages handled */
void perf_watch_dir(const char *dir);             /* the free space here goes in HW */
void perf_log(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int perf_quit_requested(void);                    /* a SIGTERM / SIGINT / SIGHUP came */
void perf_close(const char *why);                 /* the END line; stderr back as it was */
uint64_t perf_now_us(void);                       /* the monotonic clock the log uses */
uint32_t perf_dropouts(void);                     /* this session's, so far (for the tests) */
uint32_t perf_jumps(void);
