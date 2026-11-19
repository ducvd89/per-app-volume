#!/bin/bash
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "Run with sudo: sudo $0"; exit 1; }
rm -rf "/Library/Audio/Plug-Ins/HAL/PerAppVolume.driver"
killall coreaudiod || true   # launchctl kickstart is blocked by SIP
echo "Driver removed."
