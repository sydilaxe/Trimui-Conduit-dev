#!/bin/sh
# Headless end-to-end run of the desktop build (make test-e2e): SDL "disk" audio driver feeds a synthetic signal
# into the capture device, the dummy video driver renders the UI offscreen, and a FIFO stands in for a USB MIDI
# device (120 BPM clock, and CC 7 sweeping) through CONDUIT_MIDI_DEV.
set -e
cd "$(dirname "$0")/.."
BIN="${1:-build/desktop/conduit}"
W=build/e2e; rm -rf "$W"; mkdir -p "$W/rec"
python3 - "$W/in.raw" <<'PY'
import sys, numpy as np
sr = 48000; t = np.arange(sr * 40) / sr
sig = 0.3*np.sin(2*np.pi*220*t) + 0.2*np.sin(2*np.pi*331*t) * (np.sin(2*np.pi*0.5*t) > 0)
st = np.stack([sig, np.roll(sig, 240)], 1).astype('<f4')
st.tofile(sys.argv[1])
PY
mkfifo "$W/midi"
python3 - "$W/midi" <<'PY' &
import sys, time
try:
    f = open(sys.argv[1], 'wb', buffering=0)   # waits for Conduit to open it
    f.write(bytes([0xFA]))
    iv, nxt, cc, k = 60 / (120 * 24), time.monotonic(), 0, 0
    end = nxt + 90
    while time.monotonic() < end:
        nxt += iv
        time.sleep(max(0, nxt - time.monotonic()))
        out = [0xF8]
        k += 1
        if k % 3 == 0:
            cc = (cc + 5) % 128
            out += [0xB0, 7, cc]
        f.write(bytes(out))
except (BrokenPipeError, OSError):
    pass
PY
FEED=$!
SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILEIN="$W/in.raw" SDL_DISKAUDIOFILE="$W/out.raw" \
SDL_VIDEODRIVER=dummy CONDUIT_WINDOWED=1 CONDUIT_MIDI_DEV="$W/midi" \
CONDUIT_REC_DIR="$W/rec" CONDUIT_CFG="$W/conduit.cfg" \
  "$BIN" --selftest --shots "$W" || { kill $FEED 2>/dev/null; exit 1; }
kill $FEED 2>/dev/null || true
ls -l "$W/rec"
grep '^midi_' "$W/conduit.cfg"
