#!/bin/sh
# HELP: Conduit - sample from USB audio input, loop, mangle and save WAVs to the SD card.
# ICON: conduit

# MustardOS application launcher.
# Install: copy the Conduit/ folder to /mnt/mmc/MUOS/application/
[ -f /opt/muos/script/var/func.sh ] && . /opt/muos/script/var/func.sh
command -v SET_VAR >/dev/null 2>&1 && SET_VAR "system" "foreground_process" "conduit"
echo app >/tmp/act_go 2>/dev/null

APPDIR="$(cd "$(dirname "$0")" && pwd)"
cd "$APPDIR" || exit 1

export HOME="$APPDIR"
export CONDUIT_REC_DIR="${CONDUIT_REC_DIR:-$APPDIR/recordings}"
export CONDUIT_CFG="$APPDIR/conduit.cfg"
# use muOS' controller mappings when present
[ -f /usr/lib/gamecontrollerdb.txt ] && export SDL_GAMECONTROLLERCONFIG_FILE=/usr/lib/gamecontrollerdb.txt

./conduit >"$APPDIR/conduit.log" 2>&1
sync
