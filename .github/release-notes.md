Conduit, a four-track USB-audio looper (MIDI clock sync and MIDI mapping over USB) for the TrimUI Brick, Brick Hammer and Brick Pro running
MustardOS.

**Install on MustardOS**

Copy `Conduit-mustardos.muxapp` into the `ARCHIVE` folder of the first SD card, install it from
**Applications → Archive Manager**, then open **Applications → Conduit**. Press MENU to quit.

**New: updates itself.** A few seconds after it starts, Conduit asks GitHub for the latest release and, when
it's newer, offers it: **A** downloads it, puts it in place and restarts; **B** asks again next time. Your
settings, recordings and logs are kept, and a failed update changes nothing. `CONDUIT_UPDATE=0` in
`launch.sh` turns the check off. Copies from before this release don't have the updater: install this one
by hand once.

**MustardOS only:** Conduit is now built only for MustardOS; the TrimUI OS and KNULLI packages are no longer
released.

Recordings are saved in `recordings/` inside the app folder. The log is written to `log.txt` in the same
folder. To check whether your firmware supports USB audio input, run `conduit_diag.sh` from the app folder.
