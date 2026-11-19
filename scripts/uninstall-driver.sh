#!/bin/bash
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "Chạy với sudo: sudo $0"; exit 1; }
rm -rf "/Library/Audio/Plug-Ins/HAL/PerAppVolume.driver"
launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || killall coreaudiod || true
echo "Đã gỡ driver."
