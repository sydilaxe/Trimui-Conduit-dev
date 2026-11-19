# brickio: USB audio and MIDI for the TrimUI Brick and Brick Pro

A small package for the sydilaxe TrimUI apps on MustardOS: USB audio interfaces and USB MIDI devices,
low latency, resilient, cheap on the processor. C on SDL2 and the kernel's ALSA files; nothing else to
ship. GPL-3.0-or-later.

**This folder is a copy of [brickio](https://github.com/sydilaxe/Brickio-dev)'s `src/`** (which version:
`BRICKIO_VERSION`). Change brickio, not these files, then bring it in again from this repository:
`sh <brickio>/tools/brickio-sync.sh <tag>`. TrimFX compiles four of its modules (the Makefile's
`BRICKIO`); the others (`link`, `mclock`, `blink`, `perflog`, ...) come with the copy and aren't built
yet. TrimFX keeps its own `src/perflog.c/h` for now: its dropout count differs from brickio's, and one
will be chosen from a Brick's logs. The two tests below are brickio's, with their includes pointed here.

| File | What |
|---|---|
| `usbaudio.c/h` | the devices: picks the USB interface first, opens it direct, native channels, real-time threads, the buffer, the watchdog, the log |
| `duplex.c/h` | full duplex on one USB interface: capture and playback through ALSA (libasound, loaded at run time), linked, one real-time thread, no cushion |
| `aring.c/h` | the input's ring between the capture and playback callbacks: a cushion, clock drift followed by resampling, channel pairs |
| `midi.c/h` | USB MIDI in (a reader thread, the app woken at once) and out |
| `tests/test_aring.c` | the ring against jitter, bursts, drift (also two devices), stalls, multichannel and mono |
| `tests/test_duplex.c` | duplex.c against ALSA's null device: opens, runs, stops, starts again (skipped without libasound) |

## What it does

- **USB first.** For input and output, the first USB interface: by its name, or by the kernel's list of
  USB-Audio cards (`/proc/asound/cards`), since many show only a product name. It follows hot-plugging.
  A device chosen in SETUP is kept across hot-plugs and restarts (`audio.txt`).
- **Direct.** On ALSA, a USB interface is opened through `plughw:CARD=<id>` (format conversion only).
  That skips the shared `dmix` / `dsnoop` path, whose usual 1024-frame periods set a latency floor of
  about 90 ms. The period then follows the buffer. The module checks in the kernel's `hw_params` that
  the card's stream really opened, and falls back to the shared path if it didn't. `<APP>_DIRECT=0`
  turns it off.
- **Full resolution.** 32-bit float to and from SDL, which converts from what the interface runs (16, 24,
  32 bits). A 24-bit / 48 kHz interface keeps its 24 bits.
- **Channels.** The native channel count comes from the kernel (`stream0`), so a multichannel interface
  (a MixPre) is opened whole and any pair can be chosen, for input and for output, without reopening.
  A mono interface feeds both sides.
- **Input and output can be different devices** (an OP-Z in, a bluebox out). The ring follows the two
  clocks' drift by reading a hair faster or slower (cubic interpolation, up to ±0.4 %, too slow to hear
  as pitch). It never needs to skip unless they differ by more than that.
- **Low latency, then safe.** The buffer starts at 256 frames (AUTO). It is matched to the interface's
  own period if that is longer. It grows a step (to 2048) after 3 output dropouts in 20 s. The input
  cushion starts small and grows by 256 frames each time the input runs dry. After every calm 30 s it
  comes back down 16 frames. So the latency settles as low as each interface allows.
- **DUPLEX: one interface both ways.** When input and output are the same USB card, `duplex.c` opens its
  capture and playback straight through ALSA, links them so they start together on the interface's one
  clock, and runs one thread: read a period, process, write it. No cushion and no resampling: the round
  trip is the input's period plus the 2 periods the output keeps queued (`<APP>_PERIODS=3` for 3), about
  4 ms at 64 frames plus the interface. An xrun restarts both streams together (counted as a dropout).
  An interface that can't run 48 kHz, or any failure, falls back to the two-stream path below.
  `<APP>_DUPLEX=0` turns it off.
- **Real-time, each on its own core.** The audio threads take SCHED_FIFO priority on their first
  callback. `ua_pin()` (called first thing, before any thread starts) keeps the output thread (the
  effects) on the last core, the input thread on the one before, and every other thread, SDL's
  included, on the rest. The input runs above the output: it only copies, and must never miss a
  period. `<APP>_PIN=0` turns the pinning off.
- **Latency test.** `ua_latency_test()` measures the true round trip with a cable from the output to the
  input: the effects go quiet, a click goes out, and the frames until it comes back are counted (five
  times, the middle one kept). `ua_latency_result()` gives it.
- **Watchdog.** A device that stops calling back for 1.5 s is opened again.
- **Speaker guard** (optional, for FX sends). With no USB output, the internal speaker stays silent
  unless it is chosen.
- **The log.** It records what the hardware really runs (format, rate, channels, period), whether
  DIRECT and real-time took, every reopen and buffer change, and HINTs (a rate other than 48 kHz, for
  instance).
- **MIDI.** A reader thread parses each message as it arrives: notes, CCs, program change, pitch bend,
  aftertouch, and the clock / start / stop / continue real-time messages. It queues them with their
  time and calls a wake function, so the app can act at once rather than at its next screen frame.
  `midi_send` writes to every device that takes it. `midi_clock_out(bpm)` sends the MIDI clock from a
  thread of its own, on an absolute timetable (no drift), with start and stop. It works alongside USB
  audio on the same port or hub.

## Latency

With one interface both ways (DUPLEX), the round trip is 3 periods plus the interface's own converters:

| Buffer | Round trip, DUPLEX |
|---|---|
| 64 (1.3 ms) | **4 ms** + the interface |
| 128 (2.7 ms) | **8 ms** + the interface |
| 256 (5.3 ms) | **16 ms** + the interface |

With two interfaces (or DUPLEX off), the round trip is roughly the capture period, plus the input
cushion, plus two output periods, plus the interface's own converters (1-2 ms each way).

| Buffer | Cushion, one interface (settled) | Round trip, about |
|---|---|---|
| 64 (1.3 ms) | ~5.5 ms | **10 ms** |
| 128 (2.7 ms) | ~7.6 ms | **16 ms** |
| 256 (5.3 ms), AUTO's start | ~13 ms | **25-30 ms** (measured on a bluebox: ~25 ms) |
| 512 (10.7 ms) | ~22 ms | **50 ms** |
| 1024 (the shared path, or a 1024-frame interface) | ~40 ms | **85-90 ms** |

How low an interface goes depends on the interface and the load. The 1010music bluebox ran cleanly at
256 on a Brick Pro, and fell behind at 64.

A BUFFER chosen in SETUP is kept for the interface it was chosen on, by its ALSA card id (in
`audio.txt`), so each interface comes back with its own. `<APP>_BUFFER` overrides every interface.

The ring's cost is about 0.03 % of a desktop core (60 s of audio in 21 ms). On the Brick's Cortex-A53
that is about 0.3 %.

## Using it in an app

1. Copy `lib/brickio/` into the app. Add its `.c` files to the build, `-Ilib/brickio` to the flags,
   and `-lpthread -ldl` to the link.
2. First thing in `main`, before any thread starts: `ua_pin("MYAPP");`. Then, after
   `SDL_Init(SDL_INIT_AUDIO | ...)`:

   ```c
   #include "usbaudio.h"
   #include "midi.h"

   static void process(const float *in, float *out, int frames, int have_input, void *ud)
   {
       /* the audio thread: stereo interleaved, 48 kHz; in is silence while there is no input */
   }

   UaConfig ac = {.app = "MYAPP", .speaker_guard = 0, .process = process,
                  .log = my_log /* printf-like, or NULL for stderr */, .settings = "audio.txt"};
   ua_start(&ac);
   midi_start(NULL, my_wake /* e.g. push an SDL user event, or NULL */);
   ```

3. In the main loop, pass every SDL event to `ua_event(&e)`. Each frame, call `ua_tick()`; it returns
   `UA_NOTE_*` bits to show (`ua_note_text` gives the line). Read MIDI with `midi_poll`.
4. In SETUP, call `ua_cycle_device`, `ua_cycle_pair` and `ua_cycle_buffer`, and show `ua_status`
   (device names, channels, buffer, latency, the hardware's format, drift, counters).
5. At exit: `ua_stop(); midi_stop();`.
6. If the app updates itself, keep `audio.txt` among the user's files.

Settings (env, `<APP>_` prefix): `_BUFFER` (64-2048, fixed), `_AUDIO` / `_INPUT` (part of a device's
name, or `internal`), `_DIRECT=0`, `_DUPLEX=0`, `_PERIODS=3`, `_PIN=0`.

## Tested

`make test` runs `tests/test_aring.c`: a continuous sine through the ring with capture and playback
out of step. The cases cover 1-5 ms of jitter, capture in 1024-frame bursts against 512 (and the other
way), clocks ±300 ppm apart and ±2500 ppm (two devices), a 300 ms stall, 8 channels (pair 3-4) and
mono. Each must come out with no gaps or clicks, no underruns or skips once settled, the drift followed,
and a small cushion.
