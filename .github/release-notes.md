Conduit, a four-track USB-audio looper (MIDI clock sync and MIDI mapping over USB) for the TrimUI Brick, Brick Hammer and Brick Pro running
MustardOS.

**Install on MustardOS**

Copy `Conduit-mustardos.muxapp` into the `ARCHIVE` folder of the first SD card, install it from
**Applications → Archive Manager**, then open **Applications → Conduit**. Press MENU to quit.

**MustardOS only:** Conduit is now built only for MustardOS; the TrimUI OS and KNULLI packages are no longer
released.

Recordings are saved in `recordings/` inside the app folder. The log is written to `log.txt` in the same
folder. To check whether your firmware supports USB audio input, run `conduit_diag.sh` from the app folder.
