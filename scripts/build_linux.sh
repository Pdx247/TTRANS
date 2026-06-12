#!/usr/bin/env sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
mkdir -p "$ROOT/dist"
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build" --config Release
cp "$ROOT/build/ttrans" "$ROOT/dist/ttrans"
mkdir -p "$ROOT/dist/assets"
cp "$ROOT"/assets/fa-*.ttf "$ROOT/dist/assets/"
cp "$ROOT"/assets/FONT-AWESOME-LICENSE.txt "$ROOT/dist/assets/"
echo "Built $ROOT/dist/ttrans"
