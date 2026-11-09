#!/bin/sh
# HELP: Conduit: sample from USB audio, loop, mangle and save WAVs to the SD card. MENU quits.
# ICON: conduit
# GRID: Conduit

# MustardOS starts an app with this script, passing the app's folder. It sets up
# the app the way MustardOS's own apps do, then runs launch.sh.

APP_DIR="${1:-$(dirname "$0")}"

if [ -f /opt/muos/script/var/retro.sh ]; then
    . /opt/muos/script/var/retro.sh
    command -v SETUP_APP >/dev/null 2>&1 && SETUP_APP conduit ""
elif [ -f /opt/muos/script/var/func.sh ]; then
    # older muOS releases
    . /opt/muos/script/var/func.sh
    echo app >/tmp/act_go
    command -v SETUP_SDL_ENVIRONMENT >/dev/null 2>&1 && SETUP_SDL_ENVIRONMENT
    command -v SET_VAR >/dev/null 2>&1 && SET_VAR "system" "foreground_process" "conduit"
fi

cd "$APP_DIR" || exit 1
# the screen can stay dark once the app's window opens: the app runs this then
export CONDUIT_SCREEN_WAKE="$PWD/wake_screen.sh"
./launch.sh
