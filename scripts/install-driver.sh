#!/bin/bash
# Install the virtual audio driver and restart Core Audio. Run with sudo.
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "Chạy với sudo: sudo $0"; exit 1; }
SRC="build/PerAppVolume.driver"
[ -d "$SRC" ] || { echo "Chưa build. Chạy ./build.sh trước."; exit 1; }
DST="/Library/Audio/Plug-Ins/HAL/PerAppVolume.driver"
rm -rf "$DST"
cp -R "$SRC" "$DST"
chown -R root:wheel "$DST"
chmod -R 755 "$DST"
echo "Đã cài driver vào $DST. Khởi động lại Core Audio..."
launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || killall coreaudiod || true
echo "Xong."
