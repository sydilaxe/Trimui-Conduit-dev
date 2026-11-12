#!/bin/sh
# The self-update end to end, without the internet (make update-test; CI runs it). As in
# Columbia Street, RadioScan and Jaffedrone.
#
# tools/fake_github.py serves a v9.9.9 release the way GitHub does: the API's JSON, a
# ~1000-character signed redirect, then the MustardOS package (.muxapp, a zip).
# build/conduit_update installs it over an older app folder, as the app does when you press
# A. First, the same release with a damaged zip must fail and leave the folder as it was.
set -eu

T=build/update-test
PORT=${PORT:-8767}
rm -rf $T
mkdir -p "$T/rel/Conduit/glyph" "$T/rel/Conduit/recordings" $T/bad $T/app/recordings

# the release: launch.sh, mux_launch.sh and friends, a binary, the README, the glyph in its
# folder, and settings and a recording that must never reach the user's folder
R="$T/rel/Conduit"
cp Apps/Conduit/launch.sh Apps/Conduit/cacert.pem mustardos/mux_launch.sh "$R/"
cp mustardos/glyph/conduit.svg "$R/glyph/"
cp build/conduit_update "$R/conduit"
head -c 300000 /dev/urandom > "$R/README.md"
echo theirs > "$R/conduit.cfg"
echo theirs > "$R/recordings/take.wav"
Z=Conduit-mustardos.muxapp
(cd $T/rel && zip -qr $Z "Conduit")

# the device's folder: an older version, the user's settings, recording and log, and a
# README that's already the same as the release's (dated 2001, so we can tell it's left alone)
A=$T/app
cp Apps/Conduit/cacert.pem $A/
echo old > $A/mux_launch.sh
echo old > $A/conduit
echo old > $A/launch.sh
echo mine > $A/conduit.cfg
echo mine > $A/recordings/take.wav
echo mine > $A/log.txt
cp "$R/README.md" $A/
touch -t 200101010000 $A/README.md

# the same release with its zipped binary damaged (the CRC check must catch it)
cp $T/rel/$Z $T/bad/
off=$(python3 -c '
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1]); i = z.getinfo("Conduit/conduit")
print(i.header_offset + 30 + len(i.filename) + len(i.extra) + i.compress_size // 2)' $T/bad/$Z)
printf 'XXXXXXXX' | dd of=$T/bad/$Z bs=1 seek="$off" conv=notrunc 2>/dev/null

python3 tools/fake_github.py $T/rel --port $PORT --tag v9.9.9 --asset $Z &
good=$!
python3 tools/fake_github.py $T/bad --port $((PORT + 1)) --tag v9.9.9 --asset $Z &
bad=$!
trap 'kill $good $bad 2>/dev/null || true' EXIT
for i in $(seq 1 50); do
    curl -s -o /dev/null http://127.0.0.1:$PORT/latest.json && curl -s -o /dev/null http://127.0.0.1:$((PORT + 1))/latest.json && break
    sleep 0.1
done

fail() { echo "update test: $*" >&2; exit 1; }

cp -r $A $T/before
if CONDUIT_UPDATE_API=http://127.0.0.1:$((PORT + 1))/latest.json build/conduit_update $A; then
    fail "a damaged zip was installed"
fi
diff -r $T/before $A >/dev/null || fail "a failed update changed the app folder"
echo "update test: a damaged zip is refused and changes nothing"

CONDUIT_UPDATE_API=http://127.0.0.1:$PORT/latest.json build/conduit_update $A
cmp -s $A/conduit "$R/conduit" || fail "the binary wasn't replaced"
test -x $A/conduit || fail "the binary isn't executable"
cmp -s $A/launch.sh "$R/launch.sh" || fail "launch.sh wasn't replaced"
cmp -s $A/mux_launch.sh "$R/mux_launch.sh" || fail "mux_launch.sh wasn't replaced"
test -x $A/mux_launch.sh || fail "mux_launch.sh isn't executable"
cmp -s $A/glyph/conduit.svg "$R/glyph/conduit.svg" || fail "the glyph's folder wasn't made"
for f in conduit.cfg recordings/take.wav log.txt; do
    test "$(cat $A/$f)" = mine || fail "the user's $f was overwritten"
done
test "$(date -r $A/README.md +%Y)" = 2001 || fail "an unchanged file was rewritten"
test -z "$(find $A -name '*.new')" || fail ".new files were left behind"
echo "update test: installed v9.9.9 (the .muxapp) over an older version, kept the settings, recordings and log, skipped the unchanged README"
