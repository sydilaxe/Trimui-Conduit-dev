Conduit, a USB-audio sampler / looper for the TrimUI Brick, Brick Hammer and Brick Pro (TrimUI OS, MustardOS
and KNULLI).

**Install on MustardOS**

Copy `Conduit-mustardos.muxapp` into the `ARCHIVE` folder of the first SD card, install it from
**Applications → Archive Manager**, then open **Applications → Conduit**. Press MENU to quit.

**Install on TrimUI OS**

Unzip `Conduit-trimui.zip` at the root of the SD card (it contains `Apps/Conduit/`), then open
**Apps → Conduit**. Press MENU to quit.

**Install on KNULLI**

Unzip `Conduit-knulli.zip` into `/userdata/roms/ports/` (it contains `Conduit.sh` and `conduit/`), refresh
PORTS, then launch **Conduit**.

Recordings are saved in `recordings/` inside the app folder. The log is written to `log.txt` in the same
folder. To check whether your firmware supports USB audio input, run `conduit_diag.sh` from the app folder.
