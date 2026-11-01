#!/usr/bin/env bash
#
# Build scrcpy-gui for Windows x64 as a single executable, natively with
# MSYS2 (UCRT64).
#
# SDL, FFmpeg (minimal), dav1d and libusb are built from source and linked
# statically (app/deps scripts), and adb, scrcpy-server and the icons are
# embedded in the executable (extracted on first launch to
# %LOCALAPPDATA%\scrcpy-gui\runtime\<hash>\).
#
# Prerequisites (in the "MSYS2 UCRT64" shell):
#
#     pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,meson,ninja,pkgconf,cmake,nasm,autotools} \
#                        make diffutils unzip wget
#
# Usage: release/build_gui_windows_onefile.sh
#
# Output: dist/scrcpy-gui-<version>.exe

set -e
cd "$(dirname "${BASH_SOURCE[0]}")/.."

if [[ "$MSYSTEM" != UCRT64 ]]
then
    echo "Run this script from the MSYS2 UCRT64 shell" >&2
    exit 1
fi

VERSION=$(sed -n "s/^ *version: '\(.*\)',$/\1/p" meson.build)
if command -v git >/dev/null && git rev-parse --short HEAD >/dev/null 2>&1
then
    VERSION="$VERSION-$(git rev-parse --short HEAD)"
fi
BUILD_DIR=build-gui-onefile
BUNDLE_DIR="$BUILD_DIR-bundle"
OUTPUT="dist/scrcpy-gui-$VERSION.exe"

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

echo "[scrcpy-gui] Building static dependencies..."
app/deps/adb_windows.sh
app/deps/sdl.sh win64 native static
app/deps/dav1d.sh win64 native static
app/deps/ffmpeg.sh win64 native static
app/deps/libusb.sh win64 native static

DEPS_INSTALL_DIR="$PWD/app/deps/work/install/win64-native-static"
ADB_DIR="$PWD/app/deps/work/install/adb-windows"

echo "[scrcpy-gui] Preparing the files to embed..."
rm -rf "$BUNDLE_DIR"
mkdir -p "$BUNDLE_DIR"
cp "$SERVER_FILE" "$BUNDLE_DIR/scrcpy-server"
cp "$ADB_DIR"/adb.exe "$ADB_DIR"/AdbWinApi.dll "$ADB_DIR"/AdbWinUsbApi.dll \
    "$BUNDLE_DIR/"
cp app/data/scrcpy.png app/data/disconnected.png "$BUNDLE_DIR/"

echo "[scrcpy-gui] Building..."
# Never fall back to the MSYS2 (shared) libraries
unset PKG_CONFIG_PATH
export PKG_CONFIG_LIBDIR="$DEPS_INSTALL_DIR/lib/pkgconfig"

rm -rf "$BUILD_DIR"
meson setup "$BUILD_DIR" \
    --buildtype=release \
    -Db_lto=true \
    -Dc_args="-I$DEPS_INSTALL_DIR/include" \
    -Dcpp_args="-I$DEPS_INSTALL_DIR/include" \
    -Dc_link_args="-L$DEPS_INSTALL_DIR/lib -static" \
    -Dcpp_link_args="-L$DEPS_INSTALL_DIR/lib -static" \
    -Dstatic=true \
    -Dcompile_server=false \
    -Dbundle_dir="$BUNDLE_DIR"
ninja -C "$BUILD_DIR"

EXE="$BUILD_DIR/app/scrcpy-gui.exe"

echo "[scrcpy-gui] Checking dependencies..."
# Only system DLLs are allowed
imports=$(objdump -p "$EXE") # fail (set -e) rather than check nothing
bad=0
while read -r dll
do
    if [[ ! -f "/c/Windows/System32/$dll" && "$dll" != api-ms-win-* ]]
    then
        echo "Unexpected DLL dependency: $dll" >&2
        bad=1
    fi
done < <(sed -n 's/^\s*DLL Name: //p' <<< "$imports" | sort -u)
if [[ "$bad" != 0 ]]
then
    exit 1
fi

# meson --strip only applies to "meson install"
strip -o "$OUTPUT" "$EXE"
echo "[scrcpy-gui] Done: $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"
