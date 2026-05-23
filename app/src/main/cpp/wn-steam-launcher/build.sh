#!/usr/bin/env bash
# Cross-compile steam.exe (the Steam Launcher in-Wine host) for Wine
# (x86_64 / 64-bit PE) and stage it into the APK's assets so
# XServerDisplayActivity can drop it into the wine prefix at game-launch
# time.
#
# Built 64-bit on purpose: it hosts Valve's real steamclient64.dll — the same
# binary GameHub's agent uses — so IClientAppManager::LaunchApp drives the
# game through steamclient's own app-launch path (DRM context, app env).
#
# Named "steam.exe" on disk because steamclient's CGameLauncher path
# requires its host process to look like real Steam (GameHub uses the
# same name); our older wn-steam-launcher.exe name caused LaunchApp to
# queue the launch but never spawn the game, forcing the CreateProcess
# fallback. Source code still lives under wn-steam-launcher/ for clarity.
#
# Usage:   ./build.sh
# Output:  ../../assets/wnsteam/bionic/steam.exe
#
# Requires:  x86_64-w64-mingw32-g++  (mingw-w64 cross compiler, x86_64 target)

set -euo pipefail

cd "$(dirname "$0")"

CXX=x86_64-w64-mingw32-g++
OUT_DIR_REL="../../assets/wnsteam/bionic"
OUT_FILE="$OUT_DIR_REL/steam.exe"

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
