#!/bin/sh
# Rebuilds what changed and runs the game's entry point, printing where it stops.
# usage: ./boot.sh [regen]     "regen" retranslates the program first
cd "$(dirname "$0")" || exit 1
EXE="${SE_EXE:-$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamapps/content/app_314650/depot_314651/system/SpaceEngine.exe}"
if [ "$1" = regen ]; then
    ../tools/.venv/bin/python translate.py program gen/prog | tail -2 || exit 1
fi
make -j10 build/libprogram.a 2>&1 | grep -E 'error|warning' | head -5
make build/se 2>&1 | grep -E 'error|warning|Undefined|ld:' | head -10
# A run that neither exits nor fails is stopped after SE_BOOT_SECONDS (default 40).
SE_TRACE=1 ./build/se "$EXE" > build/boot.log 2>&1 &
pid=$!
( sleep "${SE_BOOT_SECONDS:-40}"; kill "$pid" 2>/dev/null && echo "stopped: still running after ${SE_BOOT_SECONDS:-40} s" ) &
watchdog=$!
wait "$pid"; status=$?
kill "$watchdog" 2>/dev/null
echo "exit=$status after $(grep -c '^  ' build/boot.log) Windows calls"
grep -v '^  ' build/boot.log | tail -6 | cut -c1-240
