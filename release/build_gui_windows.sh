#!/usr/bin/env bash
#
# Build a portable scrcpy-gui for Windows x64, natively with MSYS2 (UCRT64).
#
# Prerequisites (in the "MSYS2 UCRT64" shell):
#
#     pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,meson,ninja,pkgconf,sdl3,ffmpeg,libusb} \
#                        unzip zip wget
#
# Usage: release/build_gui_windows.sh
#
# Output: dist/scrcpy-gui-win64-<version>/ and dist/scrcpy-gui-win64-<version>.zip
#
# The release build of upstream (release/build_windows.sh, cross-compiled
# from Linux with dependencies built from source) also works for scrcpy-gui;
# this script is the quick local equivalent.

set -e
cd "$(dirname "${BASH_SOURCE[0]}")/.."

if [[ "$MSYSTEM" != UCRT64 ]]
then
    echo "Run this script from the MSYS2 UCRT64 shell" >&2
    exit 1
fi

# Project version, plus the commit if git is available in this shell
VERSION=$(sed -n "s/^ *version: '\(.*\)',$/\1/p" meson.build)
if command -v git >/dev/null && git rev-parse --short HEAD >/dev/null 2>&1
then
    VERSION="$VERSION-$(git rev-parse --short HEAD)"
fi
BUILD_DIR=build-gui-release
DIST_NAME="scrcpy-gui-win64-$VERSION"
DIST_DIR="dist/$DIST_NAME"

# Prebuilt server of the upstream version this fork is based on
SERVER_URL=https://github.com/Genymobile/scrcpy/releases/download/v5.0.1/scrcpy-server-v5.0.1
SERVER_SHA256=764eb6f79811d5211fe9df341120882ba9994c7a61b897d7bf3fb662e53bc536
SERVER_FILE="dist/scrcpy-server-v5.0.1"

mkdir -p dist
if [[ ! -f "$SERVER_FILE" ]]
then
    echo "[scrcpy-gui] Downloading prebuilt server..."
    wget -q "$SERVER_URL" -O "$SERVER_FILE.tmp"
    echo "$SERVER_SHA256  $SERVER_FILE.tmp" | sha256sum --check
    mv "$SERVER_FILE.tmp" "$SERVER_FILE"
fi

echo "[scrcpy-gui] Downloading adb (platform-tools)..."
app/deps/adb_windows.sh
ADB_DIR="$PWD/app/deps/work/install/adb-windows"

echo "[scrcpy-gui] Building..."
rm -rf "$BUILD_DIR"
meson setup "$BUILD_DIR" \
    --buildtype=release \
    --strip \
    -Db_lto=true \
    -Dcompile_server=false \
    -Dprebuilt_server="$SERVER_FILE" \
    -Dportable=true
ninja -C "$BUILD_DIR"
meson test -C "$BUILD_DIR" --print-errorlogs

echo "[scrcpy-gui] Packaging..."
rm -rf "$DIST_DIR" "dist/$DIST_NAME.zip"
mkdir -p "$DIST_DIR"

EXE="$BUILD_DIR/app/scrcpy-gui.exe"
cp "$EXE" "$DIST_DIR/"
cp "$SERVER_FILE" "$DIST_DIR/scrcpy-server"
cp app/data/scrcpy.png app/data/disconnected.png "$DIST_DIR/"
cp app/data/scrcpy-noconsole.vbs app/data/open_a_terminal_here.bat \
    "$DIST_DIR/"
cp LICENSE "$DIST_DIR/LICENSE.txt"
cp app/data/fonts/LICENSE-Inter.txt app/data/fonts/LICENSE-Lucide.txt \
    "$DIST_DIR/"
cp app/src/gui/third_party/imgui/LICENSE.txt "$DIST_DIR/LICENSE-imgui.txt"
cp app/src/gui/third_party/cjson/LICENSE "$DIST_DIR/LICENSE-cJSON.txt"
cp -r "$ADB_DIR"/. "$DIST_DIR/"

# DLLs from the MSYS2 environment (ldd lists the dependencies recursively)
ldd "$EXE" | awk '{print $3}' | grep -i "^$MINGW_PREFIX/bin/" \
    | sort -u | while read -r dll
do
    cp "$dll" "$DIST_DIR/"
done

# Check that every DLL dependency is either shipped or a system DLL
missing=0
while read -r dll
do
    if [[ ! -f "$DIST_DIR/$dll" && ! -f "/c/Windows/System32/$dll" ]] \
            && [[ "$dll" != api-ms-win-* ]]
    then
        echo "Missing DLL dependency: $dll" >&2
        missing=1
    fi
done < <(for f in "$DIST_DIR"/*.exe "$DIST_DIR"/*.dll
         do
             objdump -p "$f" | sed -n 's/^\s*DLL Name: //p'
         done | sort -u)
if [[ "$missing" != 0 ]]
then
    exit 1
fi

(cd dist && zip -qr "$DIST_NAME.zip" "$DIST_NAME")

echo "[scrcpy-gui] Done:"
echo "    $DIST_DIR/"
echo "    dist/$DIST_NAME.zip"
