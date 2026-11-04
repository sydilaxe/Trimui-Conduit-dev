#!/bin/sh
# Headless end-to-end run of the desktop build (make test-e2e): SDL "disk" audio driver feeds a synthetic signal
# into the capture device, the dummy video driver renders the UI offscreen.
set -e
cd "$(dirname "$0")/.."
BIN="${1:-build/desktop/conduit}"
W=build/e2e; rm -rf "$W"; mkdir -p "$W/rec"
python3 - "$W/in.raw" <<'PY'
import sys, numpy as np
sr = 48000; t = np.arange(sr * 30) / sr
sig = 0.3*np.sin(2*np.pi*220*t) + 0.2*np.sin(2*np.pi*331*t) * (np.sin(2*np.pi*0.5*t) > 0)
st = np.stack([sig, np.roll(sig, 240)], 1).astype('<f4')
st.tofile(sys.argv[1])
PY
SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILEIN="$W/in.raw" SDL_DISKAUDIOFILE="$W/out.raw" \
SDL_VIDEODRIVER=dummy CONDUIT_WINDOWED=1 \
CONDUIT_REC_DIR="$W/rec" CONDUIT_CFG="$W/conduit.cfg" \
  "$BIN" --selftest --shots "$W"
ls -l "$W/rec"
