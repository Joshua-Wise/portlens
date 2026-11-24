#!/bin/bash
# Run the PortLens flash station with the ESP-IDF Python env (no sudo needed).
# Usage: tools/flash-station.sh [--erase] [-b 460800] [--source build|docs] ...
set -e
. "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" >/dev/null 2>&1
exec python "$(dirname "$0")/flash_station.py" "$@"
