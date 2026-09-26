#!/usr/bin/env bash
# Parameter sweep for map/TrackContinuity on the host simulation.
# Prints, per (sigma, jump) pair, the NEW-algorithm "correct on the stacked
# stretch" percentages in the order the test prints its cases.
set -e
cd "$(dirname "$0")/../.."
CLANGXX="${CLANGXX:-/c/Program Files/LLVM/bin/clang++}"
MINGW="${MINGW:-$HOME/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64}"
for sig in ${SIGMAS:-7 10}; do
  for jump in ${JUMPS:-7 12 20}; do
    OUT="${TMPDIR:-/tmp}/tc_${sig}_${jump}.exe"
    "$CLANGXX" --target=x86_64-w64-mingw32 --sysroot="$MINGW" -std=c++17 -O2 \
      -DTC_SIGMA_DIST=${sig}.0f -DTC_JUMP_COST=${jump}.0f \
      -I src/map test/host/test_trackcontinuity.cpp src/map/TrackContinuity.cpp -o "$OUT"
    echo "sigma=$sig jump=$jump: $("$OUT" | grep ' NEW:' | sed 's/.*stretch *//; s/%.*//' | tr '\n' ' ')"
  done
done
