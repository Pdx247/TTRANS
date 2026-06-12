#!/usr/bin/env sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
APP="$ROOT/dist/TTrans.app"
mkdir -p "$APP/Contents/MacOS"
mkdir -p "$APP/Contents/Frameworks"
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build" --config Release
cp "$ROOT/build/ttrans" "$APP/Contents/MacOS/ttrans"
SDL_DYLIB="$(otool -L "$APP/Contents/MacOS/ttrans" | awk '/libSDL2.*dylib/ {print $1; exit}')"
if [ -n "$SDL_DYLIB" ] && [ -f "$SDL_DYLIB" ]; then
  SDL_NAME="$(basename "$SDL_DYLIB")"
  cp "$SDL_DYLIB" "$APP/Contents/Frameworks/$SDL_NAME"
  install_name_tool -change "$SDL_DYLIB" "@executable_path/../Frameworks/$SDL_NAME" "$APP/Contents/MacOS/ttrans"
  install_name_tool -id "@executable_path/../Frameworks/$SDL_NAME" "$APP/Contents/Frameworks/$SDL_NAME" || true
fi
cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>ttrans</string>
<key>CFBundleIdentifier</key><string>dev.pdx247.ttrans</string>
<key>CFBundleName</key><string>TTrans</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>0.1.0</string>
</dict></plist>
PLIST
hdiutil create -volname TTrans -srcfolder "$APP" -ov -format UDZO "$ROOT/dist/TTrans-macos.dmg"
echo "Packaged $ROOT/dist/TTrans-macos.dmg"
