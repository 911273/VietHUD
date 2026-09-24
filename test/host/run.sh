#!/usr/bin/env bash
# Host unit test for map/RoutePredictor — runs on the desktop, no ESP32/Arduino.
# Plain `g++` fails in some sandboxes (cc1plus can't write %TEMP%); clang with
# its integrated assembler targeting the MinGW sysroot is the reliable combo.
set -e
cd "$(dirname "$0")/../.."
CLANGXX="${CLANGXX:-/c/Program Files/LLVM/bin/clang++}"
MINGW="${MINGW:-$HOME/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64}"
OUT="${TMPDIR:-/tmp}/trtest.exe"
"$CLANGXX" --target=x86_64-w64-mingw32 --sysroot="$MINGW" -std=c++17 -O1 -Wall \
  -I src/map test/host/test_routepredictor.cpp src/map/RoutePredictor.cpp -o "$OUT"
"$OUT"
