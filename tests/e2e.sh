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
CONDUIT_REC_DIR="$W/rec" CONDUIT_CFG="$W/conduit.cfg" CONDUIT_LOG=2 \
  "$BIN" --selftest --shots "$W" 2>"$W/log.txt" || { cat "$W/log.txt"; kill $FEED 2>/dev/null; exit 1; }
kill $FEED 2>/dev/null || true
cat "$W/log.txt"
ls -l "$W/rec"
grep '^midi_' "$W/conduit.cfg"

# the log (applog.h): every kind of line the run should give, at DETAIL
fail() { echo "SELFTEST FAIL: $1"; exit 1; }
for tag in LOG SYS AUDIO STAT PROC INPUT ENGINE HW TRACK SET PAGE KEY MSG END; do
    grep -Eq "^ *[0-9]+\.[0-9]{3} $tag " "$W/log.txt" || fail "log.txt has no $tag line"
done
grep -q "MSG    SELFTEST PASS" "$W/log.txt" || fail "stderr isn't captured into the log"
grep -Eq "INPUT  presses [1-9].*press to screen avg [0-9]+ ms" "$W/log.txt" || fail "the presses aren't timed"
grep -Eq "TRACK  track 1 recorded .*peak -?[0-9.]+ dB" "$W/log.txt" || fail "a take isn't logged"
grep -q "END    the self-test passed" "$W/log.txt" || fail "no END line"
echo "ok   log.txt: every kind of line, stderr captured, takes and presses logged"

# a crash: the signal, where, a backtrace, and the exit a crash gives
rc=0
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy CONDUIT_REC_DIR="$W/rec" CONDUIT_CFG="$W/crash.cfg" \
  "$BIN" --crash-test 2>"$W/crash.txt" || rc=$?
cat "$W/crash.txt"
test "$rc" = 139 || fail "the crash test exited $rc, not 139 (SIGSEGV)"
grep -q "CRASH  SIGSEGV" "$W/crash.txt" || fail "no CRASH line"
grep -q "the main thread was the crash test" "$W/crash.txt" || fail "the CRASH line doesn't say what the app was doing"
grep -Eq "#[0-9]+ binary \+ 0x" "$W/crash.txt" || fail "no backtrace into the binary"
echo "ok   a crash is logged: signal, address, what the app was doing, backtrace"

# asked to close (MustardOS at shutdown): it closes as from the menu, saving its settings
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy CONDUIT_REC_DIR="$W/rec" CONDUIT_CFG="$W/term.cfg" \
  "$BIN" 2>"$W/term.txt" &
P=$!
sleep 3
kill -TERM $P
rc=0; wait $P || rc=$?
test "$rc" = 0 || { cat "$W/term.txt"; fail "after SIGTERM it exited $rc"; }
grep -q "SIGNAL SIGTERM" "$W/term.txt" || fail "no SIGNAL line"
grep -q "END    asked to close by a signal" "$W/term.txt" || fail "no END line after SIGTERM"
test -s "$W/term.cfg" || fail "the settings weren't saved after SIGTERM"
echo "ok   SIGTERM: logged, closed cleanly, settings saved"
