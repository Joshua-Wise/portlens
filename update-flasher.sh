#!/bin/bash
# Copy freshly-built firmware binaries into the GitHub Pages flasher.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD="$SCRIPT_DIR/build"
DEST="$SCRIPT_DIR/docs/firmware"

if [ ! -f "$BUILD/portlens.bin" ]; then
  echo "error: build/portlens.bin not found — run 'idf.py build' first" >&2
  exit 1
fi

cp "$BUILD/bootloader/bootloader.bin"           "$DEST/bootloader.bin"
cp "$BUILD/partition_table/partition-table.bin" "$DEST/partition-table.bin"
cp "$BUILD/portlens.bin"                        "$DEST/portlens.bin"

echo "Updated docs/firmware/ with:"
ls -lh "$DEST"
