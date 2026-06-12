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
cp "$BUILD/ttrans-gui.exe" "$PACKAGE_DIR/ttrans-gui.exe"
mkdir -p "$PACKAGE_DIR/assets"
cp "$ROOT"/assets/fa-*.ttf "$PACKAGE_DIR/assets/"
cp "$ROOT"/assets/FONT-AWESOME-LICENSE.txt "$PACKAGE_DIR/assets/"
cp "$MINGW_PREFIX/bin/SDL2.dll" "$PACKAGE_DIR/SDL2.dll"
for dll in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
  if [ -f "$MINGW_PREFIX/bin/$dll" ]; then
    cp "$MINGW_PREFIX/bin/$dll" "$PACKAGE_DIR/$dll"
  else
    echo "Required runtime DLL not found: $MINGW_PREFIX/bin/$dll" >&2
    exit 1
  fi
done
cp "$ROOT"/packaging/windows/* "$PACKAGE_DIR/"
"$PACKAGE_DIR/ttrans.exe" --help >/dev/null
rm -f "$ZIP"
(cd "$PACKAGE_DIR" && cmake -E tar cf "$ZIP" --format=zip .)
echo "Packaged $ZIP"
