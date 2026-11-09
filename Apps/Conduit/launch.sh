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

./conduit > log.txt 2>&1
sync

[ -n "$gov_was" ] && echo "$gov_was" > $CPU/cpu0/cpufreq/scaling_governor 2>/dev/null
