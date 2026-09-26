#!/usr/bin/env bash
# Host test for map/TrackContinuity (elevated vs surface road matching).
set -e
cd "$(dirname "$0")/../.."
CLANGXX="${CLANGXX:-/c/Program Files/LLVM/bin/clang++}"
MINGW="${MINGW:-$HOME/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64}"
OUT="${TMPDIR:-/tmp}/tctest.exe"
"$CLANGXX" --target=x86_64-w64-mingw32 --sysroot="$MINGW" -std=c++17 -O2 -Wall \
  -I src/map test/host/test_trackcontinuity.cpp src/map/TrackContinuity.cpp -o "$OUT"
"$OUT"
