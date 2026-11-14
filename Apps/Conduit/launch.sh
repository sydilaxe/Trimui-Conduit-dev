#!/bin/sh
# Conduit launcher for the TrimUI Brick, Brick Hammer and Brick Pro under MustardOS,
# run through mux_launch.sh.
# All four cores stay online, at full speed, while it runs.
cd "$(dirname "$0")"

CPU=/sys/devices/system/cpu
gov_was=$(cat $CPU/cpu0/cpufreq/scaling_governor 2>/dev/null)
for c in 0 1 2 3; do
    [ -f $CPU/cpu$c/online ] && echo 1 > $CPU/cpu$c/online 2>/dev/null
done
echo performance > $CPU/cpu0/cpufreq/scaling_governor 2>/dev/null

export LD_LIBRARY_PATH="/usr/lib:$LD_LIBRARY_PATH"
export HOME="$(pwd)"
export CONDUIT_REC_DIR="${CONDUIT_REC_DIR:-$(pwd)/recordings}"
export CONDUIT_CFG="$(pwd)/conduit.cfg"
# use the firmware's controller mappings when it has them
[ -f /usr/lib/gamecontrollerdb.txt ] && export SDL_GAMECONTROLLERCONFIG_FILE=/usr/lib/gamecontrollerdb.txt

# Optional: CONDUIT_UPDATE=0 ./conduit  never checks GitHub for a new version (an update replaces this
# file: set it again after)
# Troubleshooting (an update replaces this file: set them again after):
#   CONDUIT_LOG=2       log.txt gets a status line every second, and every button press (1: every
#                       10 s, the default; 0: off, but a crash is still logged)
#   CONDUIT_BUFFER=1024 frames a sound buffer (512 by default): bigger rides out dropouts, which
#                       log.txt's HINT lines point out
#   CONDUIT_SYSINFO=0   no sysinfo.txt (the system report, see sysinfo.sh)
# log.txt is this run's; log.1.txt and log.2.txt are the two before, so the log of a run that
# crashed is still there after starting again
[ -f log.1.txt ] && mv -f log.1.txt log.2.txt
[ -f log.txt ] && mv -f log.txt log.1.txt
[ "${CONDUIT_SYSINFO:-1}" != 0 ] && [ "${CONDUIT_LOG:-1}" != 0 ] && ./sysinfo.sh start conduit
# exit status 75: the app has just updated itself, so start the new version (in the same log)
while :; do
    ./conduit >> log.txt 2>&1
    st=$?
    how=""
    [ $st -gt 128 ] && how=" (killed by signal $((st - 128)))"
    echo "== conduit exited with status $st$how at $(date)" >> log.txt
    [ $st -eq 75 ] || break
done
./sysinfo.sh stop conduit
sync

[ -n "$gov_was" ] && echo "$gov_was" > $CPU/cpu0/cpufreq/scaling_governor 2>/dev/null
