#!/bin/bash
# Conduit launcher for KNULLI (PORTS).
# Install: copy Conduit.sh and the conduit/ folder to /userdata/roms/ports/
APPDIR="/userdata/roms/ports/conduit"
cd "$APPDIR" || exit 1

export CONDUIT_REC_DIR="${CONDUIT_REC_DIR:-$APPDIR/recordings}"
export CONDUIT_CFG="$APPDIR/conduit.cfg"

./conduit >"$APPDIR/conduit.log" 2>&1
sync
