#!/usr/bin/env sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
BUILD="$ROOT/build"
DIST="$ROOT/dist"
PACKAGE_DIR="$DIST/TTrans-windows"
ZIP="$DIST/TTrans-windows-installer.zip"

if [ -z "${MINGW_PREFIX:-}" ]; then
  echo "MINGW_PREFIX is not set. Run this script inside an MSYS2 MinGW shell." >&2
  exit 1
fi

mkdir -p "$DIST"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$MINGW_PREFIX"
cmake --build "$BUILD" --config Release

rm -rf "$PACKAGE_DIR"
mkdir -p "$PACKAGE_DIR"
cp "$BUILD/ttrans.exe" "$PACKAGE_DIR/ttrans.exe"
cp "$MINGW_PREFIX/bin/SDL2.dll" "$PACKAGE_DIR/SDL2.dll"
cp "$ROOT"/packaging/windows/* "$PACKAGE_DIR/"
rm -f "$ZIP"
(cd "$PACKAGE_DIR" && cmake -E tar cf "$ZIP" --format=zip .)
echo "Packaged $ZIP"
