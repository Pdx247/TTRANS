#!/usr/bin/env sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
sh "$ROOT/scripts/build_linux.sh"
tar -C "$ROOT/dist" -czf "$ROOT/dist/TTrans-linux-elf.tar.gz" ttrans
echo "Packaged $ROOT/dist/TTrans-linux-elf.tar.gz"
