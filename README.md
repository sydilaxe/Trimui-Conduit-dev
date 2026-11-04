# Conduit

A USB-audio sampler / looper for the **TrimUI Brick** and **Brick Pro**.
Sample from a USB audio interface (or the internal mic), loop it, bend it
with pitch, filter, crush and delay, overdub layers, and save WAVs to the
MicroSD card. Written in C on SDL2 only, so it runs on **MustardOS (muOS)**
and **KNULLI** without Python or extra libraries.

> Status: 0.1.0 dev. Engine, UI, WAV I/O and the full record → loop → FX →
> save path are tested headless in CI. Not yet tested on Brick hardware.

## Features

- **Input:** any capture device SDL sees. USB devices are picked
  automatically and hot-plugging is followed. 48 kHz stereo float internally;
  SDL converts from whatever the interface runs at (mono, 44.1 kHz, ...).
- **Sampler:** up to 120 s. Instant or threshold-triggered recording, with
  input gain and level meters.
- **Looper:** trim start and end, pitch ±24 semitones plus fine tune
  (tape-style varispeed), reverse, overdub with adjustable decay, and a
  one-level undo.
- **FX chain:** drive → sample-rate reducer → bit crusher → resonant SVF
  (LP/HP/BP) → delay (free ms or synced to 1/16 … 1 loop, ping-pong, damped
  feedback).
- **Resample:** prints the current FX and pitch into the buffer so you can
  keep layering.
- **Save to SD:** saves the raw take or a seamless processed bounce as 16-bit
  PCM or 32-bit float WAV. Writes are fsync'd, so a power-off right after
  saving won't corrupt the file.
- **Files:** a browser to reload (any PCM or float WAV, any sample rate) or
  delete recordings.

## Controls

| Button | Action |
|---|---|
| A | Record / stop (stop starts the loop). On the FILES tab: load |
| B | Play / stop loop |
| X | Overdub on / off |
| Y | Reverse. On the FILES tab: delete (press twice) |
| START | Save take (loop region, raw) |
| D-pad ↑↓ | Select parameter |
| D-pad ←→ | Change value (hold **L2** for fine, **R2** for coarse) |
| L1 / R1 | Switch tab: LOOP · FX · IO · FILES |
| SELECT + START | Bounce processed loop to WAV |
| SELECT + Y | Resample (print FX into the buffer) |
| SELECT + B | Undo last overdub |
| SELECT + X | Clear buffer (press twice) |
| MENU | Quit |

If A and B feel swapped on your firmware, set `swap_ab=1` in `conduit.cfg`.
Without an SDL controller mapping, Conduit falls back to raw joystick
buttons. Set them with `joy_a=`, `joy_b=`, and so on. **IO → Diagnostics**
shows the last raw button number pressed.

## Install

Download the zip for your OS from the Releases/Actions artifacts.

**MustardOS:** extract and copy `Conduit/` to `/mnt/mmc/MUOS/application/`,
then launch it from Applications.

**KNULLI:** extract and copy `Conduit.sh` and `conduit/` to
`/userdata/roms/ports/`, refresh PORTS, then launch Conduit.

Recordings go to `recordings/` inside the app folder (on the MicroSD card).
To change this, set `CONDUIT_REC_DIR` in the launcher.

## USB audio notes

USB input needs two things from the firmware: the USB-C port must be in
**host** mode, and the kernel must include **snd-usb-audio**. Not every
firmware build has both.

Plug in your interface and run `conduit_diag.sh` from the app folder (over
SSH or a terminal). It writes `conduit_diag.txt`, which shows whether the
driver exists and whether the device enumerated. It only reads; it changes
nothing.

- Class-compliant interfaces work best: most small USB mics, Zoom and Tascam
  recorders in interface mode, and Behringer UCA/UMC.
- A bus-powered interface may need a powered hub or a USB-C hub with power
  delivery.

## Build

```sh
make                    # native (desktop) build: CONDUIT_WINDOWED=1 ./conduit
make test               # engine + WAV unit tests (ASan/UBSan)
make test-e2e           # headless run on SDL's disk audio driver
make CROSS=aarch64-linux-gnu-   # cross build for the Brick
make package            # dist/ zips for MustardOS and KNULLI
```

On every push, CI (`.github/workflows/build.yml`) builds the aarch64 binary
in Debian bullseye (glibc 2.31) on a native arm64 runner, runs all tests and
uploads both zips. Pushing a `v*` tag makes a pre-release.

Keyboard on desktop: arrows, X=A, Z=B, S=X, A=Y, Q/W=L1/R1, 1/2=L2/R2,
Backspace=SELECT, Enter=START, Esc=MENU.

## Layout

```
src/main.c      UI, input, device handling, save/load, self-test
src/dsp.c       engine: input FIFO, recorder, looper, overdub/undo, FX, bounce
src/wav.c       WAV writer (fsync) and reader (+ resampler)
src/sdl_shim.h  SDL2 declarations for building without headers
src/font_data.h embedded DejaVu Sans Mono glyphs (tools/genfont.py)
pkg/            launchers for MustardOS / KNULLI, diag script
```

Inspired by [GrainBrick](https://github.com/ETRNLIAM/GrainBrick). Conduit
shares no code with it.

## License

MIT for Conduit's code. The embedded glyphs come from DejaVu fonts, which
use the Bitstream Vera license.
