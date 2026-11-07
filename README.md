# Conduit

A four-track USB-audio looper / sampler for the **TrimUI Brick**, **Brick Hammer**
and **Brick Pro**. Record loops from a USB audio interface (or the internal mic)
into four tracks, keep them in time with each other or with **MIDI clock over
USB**, pan and mix them, bend them with pitch, a scrubbed loop window, filter,
crush and delay, overdub layers, map every control to MIDI, and save the mix
and each track as WAVs on the MicroSD card. Written in C on SDL2 only, so it
runs on **TrimUI OS**, **MustardOS (muOS)** and **KNULLI** without Python or
extra libraries.

**Latest release:** [v0.1.0](../../releases/tag/v0.1.0) · [all releases](../../releases)

> Status: 0.1.0 dev. Engine, UI, MIDI, WAV I/O and the record → loop → sync →
> save path are tested headless in CI. Not yet tested on Brick hardware.

## Features

- **Input:** any capture device SDL sees. USB devices are picked
  automatically and hot-plugging is followed. 48 kHz stereo float internally;
  SDL converts from whatever the interface runs at (mono, 44.1 kHz, ...).
- **Four tracks:** each up to 120 s, with its own length, volume, pan, mute,
  FX send, pitch, window, overdub, decay and 16 levels of undo.
- **Sync:** the first loop sets the bar, or follow **MIDI clock** from a USB
  MIDI device (Start, Stop, Continue, tempo changes). Synced takes are whole
  bars (or ½, ¼ bar), start on the next bar line (switchable) and stop on one.
  Any track can be **Free** instead: any length, its own time.
- **MIDI:** map every control (record, play, overdub, erase, undo, track
  select, every setting, the window and pitch bend) to a note, a CC or the
  pitch wheel: point at it and move the knob. Class-compliant USB MIDI devices
  work with nothing to install.
- **Loop window and analog sticks:** play a window of a track and scrub it
  along with the left stick; bend the pitch and flip the direction with the
  right one (or with MIDI knobs).
- **Looper:** pitch ±24 semitones plus fine tune (tape-style varispeed),
  reverse, overdub (in time, unlimited layers), adjustable loop decay (while
  overdubbing or always), and click-free seams, jumps, starts and stops.
- **Master FX:** drive → sample-rate reducer → bit crusher → resonant SVF
  (LP/HP/BP) → delay (free ms or synced to 1/16 … 1 bar, ping-pong, damped
  feedback). Each track's **FX send** decides how much of it goes through:
  all (the default), none (dry) or a blend.
- **Save to SD:** the mix (with FX) and every track as its own file, from
  bar 1 and as long as the tracks take to come round together, so they line
  up in a DAW. 16-bit PCM or 32-bit float WAV, fsync'd so a power-off right
  after saving won't corrupt them.
- **Resample:** prints the mix with its FX into a track so you can keep
  layering.
- **Files:** a browser to load any WAV (PCM or float, any sample rate) into a
  track, or delete recordings.

## Controls

Everything acts on the **selected track** (highlighted; L2 / R2 to change it).

| Button | Action |
|---|---|
| A | Record / stop (synced: starts on the next bar, stops on a bar). FILES: load into the track. MIDI: learn |
| B | Play / stop the track. **Hold for 1 s:** stop and erase it (nothing new is recorded) |
| X | Overdub on / off: play over the loop while it keeps going; what you play is added to it |
| Y | Reverse. FILES: delete (press twice). MIDI: unmap |
| L2 / R2 (tap) | Previous / next track |
| START | Save the mix + every track |
| D-pad ↑↓ | Select setting |
| D-pad ←→ | Change value (hold **L2** for fine, **R2** for coarse) |
| L1 / R1 | Switch tab: TRACK · FX · SYNC · IO · MIDI · FILES |
| SELECT + A | Save the selected track's window (raw) |
| SELECT + Y | Resample: print the mix (with FX) into the selected track; the tracks in it are muted |
| SELECT + B | Undo on the selected track: the last overdub, then the one before (up to 16), or an erase |
| SELECT + X | Erase the selected track (press twice) |
| SELECT + L2 | Mute / unmute the selected track |
| SELECT + R2 | Play / stop all tracks |
| SELECT + ↑ | FX send on / off for the selected track (back on at the amount it had) |
| MENU | Quit |

On a device or USB gamepad with analog sticks:

| Stick | Action |
|---|---|
| Left ←→ | Scrub the loop window along the track (further = faster) |
| Left ↑ / ↓ (flick) | Double / halve the window |
| Right ↑↓ | Bend the pitch up to ±12 semitones; springs back when let go |
| Right ← / → (flick) | Reverse / forward |

### Tracks and sync

**TRACK → Length** is *Synced* (the default) or *Free*. With **SYNC → Sync
to: Internal**, the first synced loop you record is one bar, and every later
synced take is a whole number of bars (or ½ or ¼ of one): press A and it
starts on the next bar line (**SYNC → Rec waits for bar**; Off starts at
once), press A again and it carries on to the end of the bar it's in. Pressed
just after a bar line (up to 1/16 bar late), it ends on the bar just gone.
Erase every synced track and the next one sets a new bar.

With **Sync to: MIDI clock**, the bar is **Beats per bar** beats of the
clock coming in over USB. Takes armed before the clock starts begin on bar 1
when Start arrives; Stop pauses the synced tracks, and Start plays them from
bar 1 again. Tempo changes are followed: the loops speed up and slow down
with the clock, like tape. If the loops sit a little early or late against
your other gear, trim **SYNC → Clock offset**.

A synced track pitched away from 0 plays free (it's faster or slower); set
it back to 0 and it rejoins the beat. A *Free* track is any length and runs
in its own time.

**TRACK → FX send** sets how much of the track goes through the master FX
(FX tab): *100%* (the default) is all of it, *Off* keeps it dry, past the
FX, and anything between is a blend of the two. SELECT + ↑ switches it off
and back on at the amount it had; on the MIDI tab, *FX send on/off* (or
*Track N FX send on/off*) does the same from a pad, and *Track N FX send*
sets the amount from a knob. The saved mix follows the sends; the saved
tracks are dry.

**TRACK → Window start / end** pick the part of the track that loops. Moving
the window while it plays jumps with a short crossfade, so scrubbing doesn't
click; on a synced track the window keeps its place in the bar.

### MIDI

Plug in a class-compliant USB MIDI controller (through a hub along with the
audio interface if needed; **SYNC → MIDI in** shows what's connected). On the
**MIDI** tab, select what you want to control, press **A** and move a knob,
press a pad or a key, or move the pitch wheel. That control now does it
(one control does one thing; **Y** unmaps). Notes and CCs at 64 or more
trigger actions; CCs and the pitch wheel set settings across their range;
a pad steps through a switch's values. Targets come in two kinds: the
*selected track's* (Record, Overdub, Track: Pan, ...) and fixed ones
(*Track 3 record*, *Track 2 volume*, ...). The map is saved in `conduit.cfg`.

### Overdub

While overdubbing, what you play is added to the loop where you heard it:
Conduit allows for the time the sound takes to get out to your ears and back
in from the interface, so layers stay in time pass after pass. You hear the
new layer through Conduit once the loop comes back round. To hear your input
as you play, use your interface's own (direct) monitoring, or turn up
**IO → Input monitor** if it has none; with both you'd hear yourself twice.

**TRACK → Loop decay** makes the loop a little quieter on every pass, like
tape wearing out: older layers slowly fade, and stacking overdubs can't
build up until it distorts (the same part played over and over levels off
instead of growing). It's 2% a pass by default (−0.18 dB); set it from Off
to 50% in 0.5% steps (hold L2 for 0.05%, R2 for 5%). At Off, layers just
add up; once they pile up near full scale they're rounded off smoothly, like
tape, rather than clipping harshly. **TRACK → Decay**
chooses when: *While overdubbing* (the loop only wears while you're adding
to it) or *Always* (it fades even while it just plays; the loop as it was
when it started fading is kept as an undo step).

If layers still land a little early or late with your interface, trim them
with **IO → Dub align** (+ moves them earlier, − later).

There's no limit on layers. SELECT + B undoes them one at a time, newest
first, up to 16 steps back per track (fewer for very long takes: undo keeps
at most 64 MB of copies per track). An erase can be undone too, until you
record a new take on that track; the loop comes back stopped, and B plays it.

The loop's seam is crossfaded (10 ms), and playback fades in and out when
it starts and stops, so neither clicks. The USB input is kept in step with
the Brick's output even though their clocks differ slightly, and Conduit
measures the rate the interface really delivers, so a 48 kHz interface that
the driver reports as 44.1 kHz (or the other way round) still records at its
true pitch. The IO page's Diagnostics line shows that measured input rate
(`in 48.0 kHz` when all is well), counts any input dropouts (`xrun`) and
shows the last MIDI message received.

### Saving

**START** saves `conduit_<date>_<time>_mix.wav` (the mix through the FX and
Master volume) and `..._t1.wav` … `_t4.wav` (each track with its volume and
pan, no FX, muted ones too). They all start at bar 1 and run as long as the
synced tracks take to come round together (1 and 3 bars: 3 bars), up to
120 s, so you can drop them into a DAW and they line up. Load one back into a
track from FILES and it fits the bar exactly. **SELECT + A** saves just the
selected track's window, raw.

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
- USB MIDI comes with the same driver (it appears as `/dev/snd/midiC*D*`);
  `conduit_diag.sh` lists the MIDI devices too.

## Build

```sh
make              # cross-compile for the TrimUI -> build/trimui/conduit
make desktop      # a build for this computer -> build/desktop/conduit
                  #   (run it windowed: CONDUIT_WINDOWED=1 build/desktop/conduit)
make test         # engine + WAV unit tests (ASan/UBSan)
make test-e2e     # the desktop build, run headless on SDL's disk audio driver,
                  #   with a FIFO standing in for a USB MIDI device
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
src/main.c          UI, input (buttons, sticks, MIDI map), devices, save/load, self-test
src/dsp.c           engine: input FIFO, four tracks, sync transport (MIDI clock), overdub/undo, FX, mixdown
src/midi.c          USB MIDI in: raw ALSA devices, parser, reader thread
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
