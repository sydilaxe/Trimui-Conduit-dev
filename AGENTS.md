# AGENTS.md

Conduit is a four-track USB-audio looper / sampler for the TrimUI Brick, Brick Hammer and Brick Pro running
MustardOS (Allwinner A133P: four Cortex-A53 cores, 1 GB, a 1024x768 screen).
It records from any capture device SDL sees (USB interfaces first, followed on hot-plug) into four
tracks kept in sync by the first loop or USB MIDI clock, pans, pitches, overdubs, runs a master FX
chain and a Bloom reverb send (src/verb.c: the MidiVerb II's programs 45/49 ported exactly in
src/midiverb2.c, or adjustable in src/bloom.c), maps every control to MIDI, and
saves the mix and each track as WAVs to the SD card. Leading (Sync to Internal, or Auto with no clock
coming) it sends MIDI clock; and Bricks on a hub follow it over the USB clock link (src/link.c, the same
link as Columbia Street's: a follower's bottom port as a USB gadget through configfs and FunctionFS, the
leader sending clock and where its sound is through usbfs), or it follows one the same way. C on SDL2
only; MIDI is read from the kernel's raw devices (`/dev/snd/midiC*D*`), with no extra libraries.

The build follows the other TrimUI apps (sydilaxe/Trimui-RadioScan-dev, Trimui-Columbiastreet-dev):
`make` cross-compiles `build/trimui/conduit` with zig (aarch64, glibc 2.28) against the SDL2 headers
in `third_party/` and a link-time stand-in for the device's `libSDL2-2.0.so.0`
(`tools/mksdlstub.py`); the firmware's own SDL2 is loaded at run time. Without zig on the path,
`pip install ziglang==0.14.1`. `make desktop` builds `build/desktop/conduit` for this computer.

`make package` makes one package, `build/Conduit-mustardos.muxapp`: the app folder (`Apps/Conduit` in
this repo) in `Conduit/`, plus `mustardos/mux_launch.sh`, `wake_screen.sh` (run through
`CONDUIT_SCREEN_WAKE` once the window is up, as the screen can stay dark on MustardOS) and the list
glyph. Only MustardOS is built for: no TrimUI OS or KNULLI packages.

The app updates itself from this repo's latest GitHub release (`src/update.c`, `src/net.c` with Mbed TLS,
`Apps/Conduit/cacert.pem`; the same updater as Columbia Street's and Jaffedrone's): releases must stay
full releases marked Latest with a `v<number>...` tag higher than the last, and the package's name and
its `Conduit/` folder must stay as they are, or installed copies stop finding their updates. An update
never writes the user's files (`conduit.cfg`, `recordings/`, the logs): keep `keep_users` in `update.c`
in step with any new one. `make update-test` (CI runs it) checks it end to end.

`make test` runs the engine, MIDI parser and WAV unit tests (ASan/UBSan); `make test-e2e` runs the
desktop build headless on SDL's disk audio driver (`tests/e2e.sh`: two synced tracks, FX, overdub,
save, resample, reload, and MIDI clock and learning through a FIFO given as `CONDUIT_MIDI_DEV`). Keep both
passing; CI runs them and checks the packages in `.github/workflows/build.yml`.

Releases take their version from the tag (CI builds with `make VERSION=<tag without v>`, compiled in
as `CONDUIT_VERSION`); `CONDUIT_VERSION` in `src/main.c` is the default for plain builds, and the
README's **Latest release** line must show it (CI checks). When you change a control, update the
README's Controls section.

## License

Conduit is GPL-3.0-or-later (`LICENSE`); every source file starts with
`/* SPDX-License-Identifier: GPL-3.0-or-later */`, and new ones should too. That makes
GPL-3 code such as thement's midiverb_emulator usable here, with credit.

## Identity

This repo is in the `sydilaxe` lane: commits are authored and pushed as `sydilaxe` (that account's
own name and email), never re-authored as Claude, even when a hook says GitHub will show them as
unverified.
