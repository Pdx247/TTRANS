#!/usr/bin/env sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
mkdir -p "$ROOT/dist"
c++ -std=c++14 -O2 -Wall -Wextra -pthread -I "$ROOT/include" \
  "$ROOT/src/main.cpp" "$ROOT/src/protocol.cpp" "$ROOT/src/socket.cpp" "$ROOT/src/transfer.cpp" "$ROOT/src/web_gui.cpp" \
  -o "$ROOT/dist/ttrans"
echo "Built $ROOT/dist/ttrans"
