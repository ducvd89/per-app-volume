#!/bin/bash
# Install the virtual audio driver and restart Core Audio. Run with sudo.
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "Run with sudo: sudo $0"; exit 1; }
SRC="build/PerAppVolume.driver"
[ -d "$SRC" ] || { echo "Not built yet. Run ./build.sh first."; exit 1; }
DST="/Library/Audio/Plug-Ins/HAL/PerAppVolume.driver"
rm -rf "$DST"
cp -R "$SRC" "$DST"
chown -R root:wheel "$DST"
chmod -R 755 "$DST"
echo "Driver installed to $DST. Restarting Core Audio..."
killall coreaudiod || true   # launchctl kickstart is blocked by SIP
echo "Done."
