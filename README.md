# Conduit

A USB-audio sampler / looper for the **TrimUI Brick**, **Brick Hammer** and **Brick Pro**.
Sample from a USB audio interface (or the internal mic), loop it, bend it
with pitch, filter, crush and delay, overdub layers, and save WAVs to the
MicroSD card. Written in C on SDL2 only, so it runs on **TrimUI OS**,
**MustardOS (muOS)** and **KNULLI** without Python or extra libraries.

**Latest release:** [v0.1.0](../../releases/tag/v0.1.0) · [all releases](../../releases)

> Status: 0.1.0 dev. Engine, UI, WAV I/O and the full record → loop → FX →
> save path are tested headless in CI. Not yet tested on Brick hardware.

## Features

- **Input:** any capture device SDL sees. USB devices are picked
  automatically and hot-plugging is followed. 48 kHz stereo float internally;
  SDL converts from whatever the interface runs at (mono, 44.1 kHz, ...).
- **Sampler:** up to 120 s. Instant or threshold-triggered recording, with
  input gain and level meters.
- **Looper:** trim start and end, pitch ±24 semitones plus fine tune
  (tape-style varispeed), reverse, overdub (in time, unlimited layers),
  adjustable loop decay (while overdubbing or always), 16 levels of undo,
  and a click-free loop seam.
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
| B | Play / stop loop. **Hold for 1 s:** stop and erase the loop (nothing new is recorded) |
| X | Overdub on / off: play over the loop while it keeps going; what you play is added to it |
| Y | Reverse. On the FILES tab: delete (press twice) |
| START | Save take (loop region, raw) |
| D-pad ↑↓ | Select parameter |
| D-pad ←→ | Change value (hold **L2** for fine, **R2** for coarse) |
| L1 / R1 | Switch tab: LOOP · FX · IO · FILES |
| SELECT + START | Bounce processed loop to WAV |
| SELECT + Y | Resample (print FX into the buffer) |
| SELECT + B | Undo: the last overdub, then the one before (up to 16), or an erase |
| SELECT + X | Erase the loop (press twice) |
| MENU | Quit |

### Overdub

While overdubbing you hear your input straight away, and it is added to the
loop where you heard it: Conduit allows for the time the sound takes to get
out to your ears and back in from the interface, so layers stay in time pass
after pass.

**LOOP → Loop decay** makes the loop a little quieter on every pass, like
tape wearing out: older layers slowly fade, and stacking overdubs can't
build up until it distorts (the same part played over and over levels off
instead of growing). It's 2% a pass by default (−0.18 dB); set it from Off
to 50% in 0.5% steps (hold L2 for 0.05%, R2 for 5%). At Off, layers just
add up; once they pile up near full scale they're rounded off smoothly, like
tape, rather than clipping harshly. **LOOP → Decay**
chooses when: *While overdubbing* (the loop only wears while you're adding
to it) or *Always* (it fades even while it just plays; the loop as it was
when it started fading is kept as an undo step).

If layers still land a little early or late with your interface, trim them
with **IO → Dub align** (+ moves them earlier, − later).

There's no limit on layers. SELECT + B undoes them one at a time, newest
first, up to 16 steps back (fewer for very long takes: undo keeps at most
256 MB of copies). An erase can be undone too, until you record a new take;
the loop comes back stopped, and B plays it.

The loop's seam is crossfaded (10 ms), and playback fades in and out when
it starts and stops, so neither clicks. The USB input is kept in step with
the Brick's output even though their clocks differ slightly; the IO page's
Diagnostics line counts any input dropouts (`xrun`).

If A and B feel swapped on your firmware, set `swap_ab=1` in `conduit.cfg`.
Without an SDL controller mapping, Conduit falls back to raw joystick
buttons. Set them with `joy_a=`, `joy_b=`, and so on. **IO → Diagnostics**
shows the last raw button number pressed.

## Install

Each release has one package per firmware, all with the same binary:

| TrimUI OS | MustardOS | KNULLI |
| --- | --- | --- |
| `Conduit-trimui.zip` | `Conduit-mustardos.muxapp` | `Conduit-knulli.zip` |

Download it from the latest [release](../../releases), from the artifacts of the latest
run on the Actions tab, or build it (see below).

**TrimUI OS:** unzip `Conduit-trimui.zip` at the root of the SD card, so the app lands in
`SD:/Apps/Conduit`, then launch **Conduit** from Apps.

**MustardOS:** copy `Conduit-mustardos.muxapp` into the `ARCHIVE` folder at the root of the
first SD card, install it from **Applications → Archive Manager**, then launch **Conduit**
from Applications. By hand instead: unzip it and copy its `Conduit/` folder into
`MUOS/application/`.

**KNULLI:** unzip `Conduit-knulli.zip` into `/userdata/roms/ports/` (it holds `Conduit.sh`
and `conduit/`), refresh PORTS, then launch **Conduit**.

With the card mounted on a computer you can also run `make install SD=/path/to/card`
(add `OS=mustardos` or `OS=knulli`).

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
make              # cross-compile for the TrimUI -> build/trimui/conduit
make desktop      # a build for this computer -> build/desktop/conduit
                  #   (run it windowed: CONDUIT_WINDOWED=1 build/desktop/conduit)
make test         # engine + WAV unit tests (ASan/UBSan)
make test-e2e     # the desktop build, run headless on SDL's disk audio driver
make package      # build/Conduit-trimui.zip, Conduit-mustardos.muxapp, Conduit-knulli.zip
make version      # the version built (CONDUIT_VERSION in src/main.c, or VERSION=x.y.z)
```

The TrimUI build is cross-compiled with [zig](https://ziglang.org) (aarch64, glibc 2.28)
against the SDL2 headers in `third_party/` and links to the firmware's own SDL2 at run
time, like the other TrimUI apps. Without zig on the path, run
`pip install ziglang==0.14.1`. The desktop build needs the SDL2 dev files
(`libsdl2-dev`).

On every push, CI (`.github/workflows/build.yml`) builds the three packages, checks them,
and runs the unit and end-to-end tests. To release, push a `v*` tag (or create a release
on the website, or run the workflow by hand with a tag): the packages are built as the
tag's version and published with `.github/release-notes.md`. Keep the README's
**Latest release** line at the version in `src/main.c`; CI checks it.

Keyboard on desktop: arrows, X=A, Z=B, S=X, A=Y, Q/W=L1/R1, 1/2=L2/R2,
Backspace=SELECT, Enter=START, Esc=MENU.

## Layout

```
src/main.c          UI, input, device handling, save/load, self-test
src/dsp.c           engine: input FIFO, recorder, looper, overdub/undo, FX, bounce
src/wav.c           WAV writer (fsync) and reader (+ resampler)
src/font_data.h     embedded DejaVu Sans Mono glyphs (make font: tools/genfont.py)
Apps/Conduit/       the app folder: launch.sh, config.json, icon, conduit_diag.sh
mustardos/          MustardOS: mux_launch.sh, wake_screen.sh, the list glyph
knulli/Conduit.sh   KNULLI PORTS launcher
third_party/        SDL2 headers for the cross build
tools/              mksdlstub.py (link stub), genfont.py, mkicon.py
tests/              unit tests and the end-to-end script
```

Inspired by [GrainBrick](https://github.com/ETRNLIAM/GrainBrick). Conduit
shares no code with it.

## License

MIT for Conduit's code. The embedded glyphs come from DejaVu fonts, which
use the Bitstream Vera license.
