#!/usr/bin/env bash
# Host test for the built-in OSM traffic-light merge (src/map/SignMerge.h).
set -e
cd "$(dirname "$0")/../.."
CLANGXX="${CLANGXX:-/c/Program Files/LLVM/bin/clang++}"
MINGW="${MINGW:-$HOME/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64}"
OUT="${TMPDIR:-/tmp}/signmerge.exe"
"$CLANGXX" --target=x86_64-w64-mingw32 --sysroot="$MINGW" -std=c++17 -O1 -Wall \
  -I src/map test/host/test_signmerge.cpp -o "$OUT"
"$OUT"
