#!/usr/bin/env sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
APP="$ROOT/dist/TTrans.app"
mkdir -p "$APP/Contents/MacOS"
c++ -std=c++14 -O2 -Wall -Wextra -pthread -I "$ROOT/include" \
  "$ROOT/src/main.cpp" "$ROOT/src/protocol.cpp" "$ROOT/src/socket.cpp" "$ROOT/src/transfer.cpp" "$ROOT/src/web_gui.cpp" \
  -o "$APP/Contents/MacOS/ttrans"
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
