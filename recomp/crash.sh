#!/bin/sh
# Runs the game under lldb and prints the call stack of the thread that
# stopped. Translated functions are named f_<original address>, so the native
# stack is the game's stack.
# usage: ./crash.sh [seconds]
cd "$(dirname "$0")" || exit 1
EXE="${SE_EXE:-$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamapps/content/app_314650/depot_314651/system/SpaceEngine.exe}"
lldb -b -o run -k "bt 40" -k "register read sp" -k quit -- ./build/se "$EXE" > build/crash.log 2>&1 &
pid=$!
( sleep "${1:-60}"; kill "$pid" 2>/dev/null ) &
wait "$pid" 2>/dev/null
pkill -f 'build/se ' 2>/dev/null
grep -E 'stop reason' build/crash.log | head -2
awk '/^\* thread/{f=1} f && /frame #/' build/crash.log | sed -E 's/ at .*//; s/0x[0-9a-f]{16} //' | head -24
