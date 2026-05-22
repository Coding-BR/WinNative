#!/usr/bin/env bash
# Cross-compile wn-steam-launcher.exe for Wine (x86-64 PE) and stage it
# into the APK's assets so XServerDisplayActivity can drop it into the
# wine prefix at game-launch time.
#
# Usage:   ./build.sh
# Output:  ../../assets/wnsteam/bionic/wn-steam-launcher.exe
#
# Requires:  x86_64-w64-mingw32-g++  (mingw-w64 cross compiler)

set -euo pipefail

cd "$(dirname "$0")"

CXX=x86_64-w64-mingw32-g++
OUT_DIR_REL="../../assets/wnsteam/bionic"
OUT_FILE="$OUT_DIR_REL/wn-steam-launcher.exe"

# Static link the runtime so we don't drag MinGW DLLs into the wine prefix.
# -Wl,--subsystem,console : keep it a console subsystem so stderr/stdout reach
# Wine's GuestProgramLauncher pipe → host logcat.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
    -static -static-libgcc -static-libstdc++ \
    -Wl,--subsystem,console \
    -o "$OUT_FILE" \
    src/main.cpp \
    -ladvapi32 -lkernel32 -luser32

x86_64-w64-mingw32-strip "$OUT_FILE"

echo "Built: $OUT_FILE  ($(stat -c '%s' "$OUT_FILE") bytes)"
file "$OUT_FILE"
