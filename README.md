# Conduit

A four-track USB-audio looper / sampler for the **TrimUI Brick**, **Brick Hammer**
and **Brick Pro**. Record loops from a USB audio interface (or the internal mic)
into four tracks, keep them in time with each other or with **MIDI clock over
USB**, pan and mix them, bend them with pitch, a scrubbed loop window, filter,
crush and delay, overdub layers, map every control to MIDI, and save the mix
and each track as WAVs on the MicroSD card. Written in C on SDL2 only, and
built for **MustardOS (muOS)**, without Python or extra libraries.

**Latest release:** [v0.1.0](../../releases/tag/v0.1.0) · [all releases](../../releases)

> Status: 0.1.0 dev. Engine, UI, MIDI, WAV I/O and the record → loop → sync →
> save path are tested headless in CI. Not yet tested on Brick hardware.

## Features

- **Input:** any capture device SDL sees. USB devices are picked
  automatically and hot-plugging is followed. 48 kHz stereo float internally;
  SDL converts from whatever the interface runs at (mono, 44.1 kHz, ...).
- **Scatter and SEQ:** cut a track's loop into slices on the beat (from a
  bar to a 64th, with triplets and dotted values) and play them in a pattern: a Euclidean rhythm (or
  chance) picks steps to shuffle, stutter, reverse, chop or turn into
  grains, per track or over the whole mix. The SEQ tab shows the steps like a
  step sequencer. It
  follows the bar, so it stays with MIDI clock and linked Bricks.
- **Live input through the effects:** send what you're playing to the Bloom
  reverb and the FX as you play, to hear them live, and choose whether takes
  record it dry or with the effects printed in.
- **Four tracks:** each up to 120 s, with its own length, volume, pan, mute,
  FX send, reverb send, pitch, window, overdub, decay and 16 levels of undo.
- **Sync:** the first loop sets the bar, or follow **MIDI clock** from a USB
  MIDI device (Start, Stop, Continue, tempo changes), or another Brick over a
  **USB link** (a hub, no MIDI interface needed). Leading, Conduit sends MIDI
  clock to your other gear and to linked Bricks. Synced takes are whole
  bars (or ½, ¼ bar), start on the next bar line (switchable) and stop on one.
  Any track can be **Free** instead: any length, its own time.
- **MIDI:** map every control (record, play, overdub, erase, undo, track
  select, every setting, the window and pitch bend) to a note, a CC or the
  pitch wheel: point at it and move the knob. Class-compliant USB MIDI devices
  work with nothing to install.
- **Loop window and analog sticks:** play a window of a track and scrub it
  along with the left stick; bend the pitch and flip the direction with the
  right one (or with MIDI knobs).
- **X/Y mix:** SELECT + → turns the sticks into a vector mixer for the four
  tracks, one in each corner of a pad: glide between them with the left
  stick, throw the mix with the right one.
- **Looper:** pitch ±24 semitones plus fine tune (tape-style varispeed),
  reverse, overdub (in time, unlimited layers), adjustable loop decay (while
  overdubbing or always), and click-free seams, jumps, starts and stops.
- **Master FX:** drive → sample-rate reducer → bit crusher → resonant SVF
  (LP/HP/BP) → delay (free ms or synced to 1/16 … 1 bar, ping-pong, tone,
  low-cut, wow and saturation on the repeats). Each track's **FX send** decides how much of it goes through:
  all (the default), none (dry) or a blend.
- **Bloom reverb:** a separate send to the Alesis MidiVerb II's *Bloom*
  programs (45 and 49), emulated exactly, or adjustable (time, size,
  damping, diffusion), with pre-delay, width and tone on both.
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
| D-pad ↑↓ | Select setting: down a column, then on to the top of the next |
| D-pad ←→ | Change value (hold **L2** for fine, **R2** for coarse) |
| L1 / R1 | Switch tab: TRACK · SEQ · EFFECTS · SYNC · IO · MIDI · FILES |
| SELECT + A | Save the selected track's window (raw) |
| SELECT + Y | Resample: print the mix (with FX) into the selected track; the tracks in it are muted |
| SELECT + B | Undo on the selected track: the last overdub, then the one before (up to 16), or an erase |
| SELECT + X | Erase the selected track (press twice) |
| SELECT + L2 | Mute / unmute the selected track |
| SELECT + R2 | Play / stop all tracks |
| SELECT + ↑ | FX send on / off for the selected track (back on at the amount it had) |
| SELECT + ↓ | Reverb send on / off for the selected track (back on at the amount it had, 50% the first time) |
| SELECT + → | X/Y mix on / off: the sticks mix the four tracks (below) |
| SELECT + ← | Centre the X/Y mix |
| SELECT + L1 | Scatter on / off for the selected track (back to the mode it had; *Scatter* the first time) |
| SELECT + R1 | Scatter: the next variation (another pattern) |
| MENU | Quit |

On a device or USB gamepad with analog sticks:

| Stick | Action |
|---|---|
| Left ←→ | Scrub the loop window along the track (further = faster) |
| Left ↑ / ↓ (flick) | Double / halve the window |
| Right ↑↓ | Bend the pitch up to ±12 semitones; springs back when let go |
| Right ← / → (flick) | Reverse / forward |

With **X/Y mix** on (SELECT + →, `X/Y MIX` and a small pad in the top right),
the sticks mix the four tracks instead, one in each corner of a square:
**1** top left, **2** top right, **3** bottom left, **4** bottom right.

| Stick | Action |
|---|---|
| Left | Move the mix point (further = faster); it stays where you leave it |
| Right | Throw the mix from there (the full pad); springs back when let go |

In the middle every track plays at its own volume; towards a corner the
nearest track stays at its volume and the others fade (on an edge, the two
tracks on it stay up). A thin bar in each track's number box and *xy* in its
info show the gain. The mix sits on top of each track's volume, mute and pan,
and what you save or resample is the mix as you hear it (each track's file
too). SELECT + ← puts the point back in the middle; SELECT + → again gives
the sticks back to the window and bend, and every track its full volume. With
MIDI, map *X/Y mix: left - right* and *top - bottom* to two knobs (moving
either turns the X/Y mix on) and *X/Y mix on/off* to a pad.

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
your other gear, trim **SYNC → Clock offset**. The tempo comes from a
straight line through the last four beats of ticks, so USB's timing jitter
averages out: the loops hold within about a quarter of a millisecond of the
clock.

**Sync to: Auto** follows a MIDI clock (or a leading Brick on the USB link)
while one comes in, and otherwise the first loop sets the bar as with
*Internal*. **SYNC → MIDI Start/Stop: Ignore** follows the clock's tempo but
not its Start, Stop and Continue: you start and stop the loops yourself
(from a leading Brick they still line up with its bar).

While Conduit leads (*Internal*, or *Auto* with nothing coming in) and **SYNC →
Send MIDI clock** is On (the default), it sends MIDI clock to every MIDI port,
24 ticks a beat at the first loop's tempo (a bar of *Beats per bar* beats),
timed to the sound as it comes out of your audio interface: Start when the
loops start on the bar (song position and Continue when later), Stop when
they stop.

### Linking Bricks over USB

Several Bricks can play in time by cable, with no MIDI interface between them:
the **leader** has a USB hub in its **top** port, and each **follower** plugs its
**bottom** (charging) port into the same hub with a data cable. Use a powered
hub: each follower charges from it. Nothing to set up: Conduit offers its bottom
port as a "TrimUI clock link" when it starts, and a leader finds the Bricks on
its hub within a second and sends them its clock, Start and Stop, and where its
sound is 50 times a second, at its exact tempo. A follower set to *Auto* or *MIDI
clock* puts its loops there, within a few hundredths of a millisecond, at any
tempo. It's the same link as Columbia Street's, so either app can lead the
other, and a leader can still use USB audio and MIDI gear on the same hub.
**SYNC → Tempo** says what the transport follows (*a leading Brick (USB link)*)
or that it leads. `CONDUIT_LINK=0` in `launch.sh` leaves the bottom port alone;
`CONDUIT_MIDI_CLOCK=0` never sends clock.

A synced track pitched away from 0 plays free (it's faster or slower); set
it back to 0 and it rejoins the beat. A *Free* track is any length and runs
in its own time.

**TRACK → Volume** goes to 150%. Turned up, a track stays clean until it
would distort; then it's rounded off with a warm soft clip (a little
asymmetric, like tape) instead of clipping hard. Quiet parts just get
louder.

**TRACK → FX send** sets how much of the track goes through the master FX
(EFFECTS tab): *100%* (the default) is all of it, *Off* keeps it dry, past the
FX, and anything between is a blend of the two. SELECT + ↑ switches it off
and back on at the amount it had; on the MIDI tab, *FX send on/off* (or
*Track N FX send on/off*) does the same from a pad, and *Track N FX send*
sets the amount from a knob. The saved mix follows the sends; the saved
tracks are dry.

**TRACK → Window start / end** pick the part of the track that loops. Moving
the window while it plays jumps with a short crossfade, so scrubbing doesn't
click; on a synced track the window keeps its place in the bar.

### Effects

Every tab fits on one screen: the four track lanes on top, the tabs, and the
tab's settings below in titled columns, as sliders, switches and side-by-side
choices (two short settings often share a row). ↑↓ walk down a column and on
into the next, ←→ change the selected one. A setting that does nothing as
things are set (Amount when the steps are Euclidean, the delay's Time when
it's synced) is greyed out. Names and values are short in the cells (*Fdbk*,
*1/8T*, *L60*); the footer shows the selected setting in full. The IN / OUT
meters are the thin bars at the top right, and the lanes take whatever room
the settings leave, so they are biggest on TRACK. MIDI and FILES are lists
that scroll. (`CONDUIT_GRID=0` brings back the old one-row-per-setting
lists.)

The **EFFECTS** tab holds everything after the tracks, in five columns:

- **Drive · crush · filter** and **Delay** are the master FX chain that each
  track's **FX send** feeds. The delay has *Mix*, *Sync* (free ms, or 1/16 …
  1 bar), *Time*, *Feedback* and *Ping-pong*, and how its repeats sound:
  *Tone* (from dark at 1 kHz to bright at 16 kHz), *Low-cut* (thinner
  repeats, up to 2 kHz), *Wow* and *Wow rate* (tape wobble, up to ±4 ms) and
  *Saturation* (repeats that get grittier as they go round).
- **Reverb** is fed by each track's **Reverb send**, a send of its own (see
  below).

### Scatter and the SEQ tab

Scatter cuts a track's loop window into slices and plays them in a pattern,
like a DJ's scatter effect, or granular playback with long grains. Its
settings are on the **SEQ** tab, for the selected track (L2/R2 picks
another). On that tab each track's lane shows the steps of the bar it's
playing, as on a step sequencer:

- Each step shows the audio it plays: the track's own under the steps the
  pattern leaves (lined up with the loop), or the slice it puts there,
  repeated, reversed or cut short by the gate as you'll hear it.
- The steps the pattern picks are lit (yellow), the others plain, and a
  silent step is black. The white bars mark the step playing now, the white
  line where the playhead is in it, and the ticks on top mark the beats.
- A lit step's tag (top left) shows what it does: a number is the slice it plays instead,
  `x4` repeats it 4 times, `<` plays it backwards, `~` is a grain.
- The top line shows the mode, how steps are picked, the slice size and the
  variation.

The slices are steps of the bar. **Slice size** is a note value: 1 bar, 1/2,
1/4 (a beat), 1/8, 1/16, 1/32 or 1/64, the triplets (1/4, 1/8 and 1/16
triplet) and the dotted values (1/8 and 1/16 dotted). A 1-bar loop in 4/4 has
16 slices at 1/16 and 12 at 1/8 triplets. Dotted slices don't fit a bar
evenly (5⅓ dotted 8ths a bar), so they run across the bar lines and come
back round with it every 3 bars. They follow the track's
playhead, which follows the bar, so with **Sync to: MIDI clock** or a
leading Brick the slices land on its beat at any tempo. A *Free* track's
window counts as one bar.

**Steps picked** chooses which steps the pattern changes:

- **Euclidean** (the default): **Hits** steps out of every **Steps**, spread
  as evenly as they go, starting on the first and turned later by
  **Rotate**. *Steps* at 0 is a bar's worth (16 at 1/16 in 4/4); set it to
  another number for a polyrhythm against the bar (5 of 13 runs on across
  bar lines). 4 of 16 is four on the floor, 3 of 8 the tresillo, 5 of 16 a
  bossa nova.
- **Random:** each step is picked with the chance **Amount**.

**Scatter** is what happens to the picked steps:

- **Shuffle:** they play another slice of the loop.
- **Stutter:** they repeat the start of their slice 2, 3, 4, 6 or 8 times
  (now and then the slice before).
- **Reverse:** they play backwards.
- **Chop:** only they sound; the other steps are silent.
- **Scatter (all of them):** any of the above, or silence.
- **Grain:** they play from anywhere in the loop (not only slice starts),
  some repeated 2 or 4 times, some backwards.

Then:

- **Gate:** how much of each slice (or repeat) sounds; lower is choppier.
- **Pattern length:** the random choices (which slice, how many repeats,
  and the steps themselves with *Random*) repeat after 1, 2, 4 or 8 bars.
- **Evolve:** off, the pattern repeats exactly. Turned up, that share of
  its steps get new random choices each time the pattern comes round, so
  it keeps changing slowly (at 100% it is new every time). The Euclidean
  hits stay put; what's done on them changes.
- **Variation:** other random choices with the same settings (SELECT + R1
  steps to the next). The same variation is always the same pattern.

Jumps are crossfaded and the gate fades in 2 ms, so nothing clicks. Overdubs
still go into the loop in order, and Scatter only changes what's heard. On the
other tabs the lane shows the slices as ticks along its bottom and where
Scatter reads as a yellow line. The saved mix and Resample include the
pattern: the mix lasts whole patterns, and a resampled track has Scatter
switched off because the pattern is already in it. Scatter's mode and
Variation start Off and 0 each time; the rest is remembered per track. MIDI
can map every setting, as well as *Scatter on/off* and *Scatter: next
variation*.

### Master scatter

Scatter can also work on the **whole mix**, like a DJ mixer's: all four
tracks with their FX and reverb, but not the live input. Its settings are the
**MIX PATTERN** and **MIX SHAPE** columns on the SEQ tab. They are the same as a
track's, and it can run on top of the tracks' own scatter. While the SEQ tab
is open, a **MIX** strip under the four lanes
shows its steps in the bar. With one of the master settings selected, SELECT + L1
switches it on and off and SELECT + R1 steps to the next variation. MIDI has
*Master scatter on/off* and *Master scatter: next variation*.

The mix isn't a loop, so master scatter keeps the last 21 seconds of it and
plays from there:

- Steps it doesn't pick play the mix as it is.
- A repeat (stutter) replays the start of its own step.
- The other changes (another slice, backwards, a grain) play from the bar
  before.

It needs a bar to follow: a synced loop, MIDI clock or a leading Brick. Until
the transport runs it passes the mix through ("WAITS").

**Mosaics.** Two settings let the passes build on each other:

- **Master feedback:** what it cuts up is the mix blended with its own output
  from a bar before. Each pass slices the slices of the last one, so the
  pattern builds up into a mosaic of the loops. Higher values keep more of
  the mosaic and take in less of the fresh mix. With nothing picked it's a
  one-bar echo that fades.
- **Master freeze:** takes in no more of the mix and keeps working on what
  it has. With nothing picked, the last bar repeats exactly. With steps
  picked, and with Evolve, each pass cuts the frozen mosaic up again. Gated
  and silent steps wear it away.

Evolve on the master and the tracks keeps the choices moving pass to pass.
MIDI has *Master scatter: freeze on/off*.

The saved mix includes master scatter. **Resample** (SELECT + Y) with
Feedback, Freeze or Evolve on takes the mosaic **as you heard it**: the last
whole pattern (up to 8 bars) up to the bar line. It goes into the selected
track as a synced loop; the other tracks are muted and the FX and master
scatter are switched off, so you can keep going: scatter the mosaic again,
feed it back, resample again. Without those three settings, Resample renders
the mix afresh as before, and switches master scatter off afterwards.

### The live input through the effects

The input can go through the effects as you play, not only the loops. On
the **IO** tab:

- **Live reverb send:** how much of the input goes to the Bloom reverb.
- **Live FX send:** how much goes through the drive, crush, filter and delay.
  As with a track's FX send, that much is taken off the dry input (at 100%
  you hear it only through the FX).
- **Record live input:**
  - *Dry (FX heard only)*, the default: you hear the input's reverb and FX,
    but takes and overdubs record it dry. Turn up the track's own sends to
    hear the effects on the loop.
  - *With the live FX*: the input goes through its own copy of the FX and
    the reverb (the same settings), and that is recorded: the reverb's swell
    and tail are part of the loop. Leave the track's reverb send at 0% then,
    or the loop gets the reverb twice.

The input's effects are heard whatever **Input monitor** is set to: that sets
only the dry input, so with an interface that monitors itself you can leave
it at 0% and hear the dry signal from the interface and the reverb from
Conduit. The saved mix and Resample hold only the loops, not the live input.

### Reverb

**TRACK → Reverb send** sends the track to the Bloom reverb, on its own:
it's a second send, separate from the FX send, so a track can go through
the FX, the reverb, both or neither. It's 0% (off) by default; SELECT + ↓
switches it on and off.

**EFFECTS → Reverb** picks one of four:

- **Bloom 1, exact** (the default): the Alesis MidiVerb II's program 45,
  "Bloom 1 8 Sec", emulated exactly: the unit's own program, in its 16-bit
  integer arithmetic, its 16 K words of delay memory and its 23.4 kHz rate,
  13-bit mono in and stereo out. Instead of starting dense, the sound swells
  over about half a second through a long chain of diffusers, then rings
  out.
- **Bloom 2, exact:** program 49, "Bloom 2 7 Sec": the same with an 85 ms
  pre-delay.
- **Bloom 1 / Bloom 2, adjustable:** the same design in floating point at
  24 kHz, whose insides can be changed (below).

Then:

- **Level:** how loud the reverb comes back into the mix.
- **Time:** 1 to 30 s (adjustable types; the exact ones decay as the unit
  does).
- **Pre-delay:** up to 500 ms before the reverb starts (on top of Bloom 2's
  own 85 ms).
- **Size:** 50 to 150% of the unit's room; a bigger one swells more slowly
  (adjustable types).
- **Damping:** how quickly the tail darkens (adjustable types; 50% is the
  unit's).
- **Diffusion:** lower is grainier and more echoey, higher smoother
  (adjustable types; 50% is the unit's).
- **Width:** 0 (mono) to 150% (wider than the unit).
- **Low-cut / High-cut:** the tone of what goes into the reverb.

The saved mix includes it; the saved tracks are dry. The exact types match
the emulation sample for sample; what neither models is the unit's analog
side, so Conduit stands in a 10 kHz low-pass on the way in and out, and
converts between its 48 kHz and the unit's 23,437.5 Hz.

The MidiVerb II programs come from thement's
[midiverb_emulator](https://github.com/thement/midiverb_emulator) (GPL-3),
which decompiled them from the unit's DSP microcode; `src/midiverb2.c` is a
port of programs 45 and 49 from it.

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
To hear the reverb or the FX on what you play, see
[The live input through the effects](#the-live-input-through-the-effects).

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
buttons, numbered as MustardOS's Brick or Brick Pro controller. Set them with
`joy_a=`, `joy_b=`, and so on. **IO → Diagnostics**
shows the last raw button number pressed.

## Install

Conduit is built only for MustardOS. Each release has one package, `Conduit-mustardos.muxapp`, for
the Brick, Brick Hammer and Brick Pro. Download it from the latest [release](../../releases), from the artifacts of the latest
run on the Actions tab, or build it (see below).

Copy `Conduit-mustardos.muxapp` into the `ARCHIVE` folder at the root of the
first SD card, install it from **Applications → Archive Manager**, then launch **Conduit**
from Applications. By hand instead: unzip it and copy its `Conduit/` folder into
`MUOS/application/`.

With the card mounted on a computer you can also run `make install SD=/path/to/card`
(it copies the app into `MUOS/application/Conduit`).

Recordings go to `recordings/` inside the app folder (on the MicroSD card).
To change this, set `CONDUIT_REC_DIR` in the launcher.

### Updates

Conduit updates itself, as Columbia Street, HI5, RadioScan and Jaffedrone do. A few seconds after it
starts (once Wi-Fi is up) it asks GitHub for this repo's latest release, and tries again every minute,
five times in all, if it can't reach it. When the release is newer, a prompt covers the screen: **A**
downloads `Conduit-mustardos.muxapp`, puts the new files in place and restarts Conduit; **B** (or MENU)
closes it, and it is offered again next time. The sound keeps going under the prompt. Your settings
(`conduit.cfg`), your recordings and the logs are never touched; files that haven't changed aren't
written; and every new file is written and checked beside the old one before any is replaced, so a
failed or broken download changes nothing. `log.txt` says what was found (`update: the latest release
is v…`). `CONDUIT_UPDATE=0` in `launch.sh` turns the check off (an update replaces `launch.sh`, so set
it again after). A copy from before the updater has none: install the first release with it by hand
once.

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
make package      # build/Conduit-mustardos.muxapp
make update-test  # the self-update end to end, against tools/fake_github.py (a local GitHub)
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

The updater (`src/update.c`, `src/net.c`, the same as Columbia Street's and Jaffedrone's)
reaches GitHub over HTTPS with [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) 3.6.7, which
`make` downloads (pinned by its SHA-256) into `third_party/mbedtls/` (not committed), builds with
`tools/mbedtls.mk` and links in statically. Mbed TLS is under Apache-2.0 (or GPL-2.0-or-later); the
package carries its licence as `LICENSE-mbedtls.txt`. `Apps/Conduit/cacert.pem` is curl's extract of
Mozilla's CA certificates (MPL-2.0), which the updater checks GitHub's certificate against.
`build/conduit_update APP_DIR [--force]` (from `src/main_update.c`) checks for a newer release and
installs it into `APP_DIR`, as the app does on **A**; point `CONDUIT_UPDATE_API` at
`tools/fake_github.py` (`http://127.0.0.1:8766/latest.json`) to try one without the internet.
Releases must stay full releases marked Latest (the updater reads GitHub's latest release, which
leaves pre-releases and drafts out), tagged `v<number>.<number>...`, higher each time.

Keyboard on desktop: arrows, X=A, Z=B, S=X, A=Y, Q/W=L1/R1, 1/2=L2/R2,
Backspace=SELECT, Enter=START, Esc=MENU.

## Layout

```
src/main.c          UI, input (buttons, sticks, MIDI map), devices, save/load, self-test
src/update.c, net.c self-update: GitHub's latest release, the download and unzip; HTTPS (Mbed TLS)
src/dsp.c           engine: input FIFO, four tracks, sync transport (MIDI clock, the USB link), overdub/undo, FX, mixdown
src/midi.c          USB MIDI in and clock out: raw ALSA devices, parser, reader thread; the USB link's leader side
src/link.c          the clock link between Bricks: the bottom port as a USB gadget (configfs, FunctionFS), finding followers
src/verb.c          the reverb send: tone, pre-delay, the Bloom, width, level
src/midiverb2.c     MidiVerb II programs 45 and 49, exact (from midiverb_emulator)
src/bloom.c         the adjustable Bloom (the same design in floating point)
src/wav.c           WAV writer (fsync) and reader (+ resampler)
src/font_data.h     embedded DejaVu Sans Mono glyphs (make font: tools/genfont.py)
Apps/Conduit/       the app folder: launch.sh, conduit_diag.sh
mustardos/          MustardOS: mux_launch.sh, wake_screen.sh, the list glyph
third_party/        SDL2 headers for the cross build
tools/              mksdlstub.py (link stub), genfont.py
tests/              unit tests and the end-to-end script
```

Inspired by [GrainBrick](https://github.com/ETRNLIAM/GrainBrick). Conduit
shares no code with it. The Bloom reverb's MidiVerb II programs come from
thement's [midiverb_emulator](https://github.com/thement/midiverb_emulator).

## License

Conduit is free software under the GNU General Public License, version 3
or (at your option) any later version: see [LICENSE](LICENSE). You may
copy, change and share it, as long as what you share stays under the same
license with its source. The embedded glyphs come from DejaVu fonts, which
use the Bitstream Vera license.
