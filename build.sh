#!/bin/bash
# Build the HAL driver and the menu bar app (needs only Command Line Tools).
set -euo pipefail
cd "$(dirname "$0")"
BUILD=build
ARCHS="-arch arm64 -arch x86_64"
MINOS=13.0
rm -rf "$BUILD"; mkdir -p "$BUILD/obj"

echo "==> Driver"
DRV="$BUILD/PerAppVolume.driver"
mkdir -p "$DRV/Contents/MacOS" "$DRV/Contents/Resources"
clang $ARCHS -mmacosx-version-min=$MINOS -O2 -Wall -bundle \
  -framework CoreAudio -framework CoreFoundation -framework IOSurface -I App/Engine \
  -o "$DRV/Contents/MacOS/PerAppVolume" Driver/PerAppVolumeDriver.c
cp Driver/Info.plist "$DRV/Contents/Info.plist"
codesign --force --sign - "$DRV"

echo "==> App"
APP="$BUILD/Per-App Volume.app"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
for A in arm64 x86_64; do
  clang -arch $A -mmacosx-version-min=$MINOS -O2 -Wall -c App/Engine/PAVEngine.c -o "$BUILD/obj/PAVEngine-$A.o"
  swiftc -O -swift-version 5 -target $A-apple-macos$MINOS \
    -import-objc-header App/Engine/PAVEngine.h \
    App/Sources/*.swift "$BUILD/obj/PAVEngine-$A.o" \
    -framework CoreAudio -framework IOSurface -framework AppKit -framework SwiftUI -framework Carbon -framework ServiceManagement \
    -o "$BUILD/obj/PerAppVolume-$A"
done
lipo -create "$BUILD/obj/PerAppVolume-arm64" "$BUILD/obj/PerAppVolume-x86_64" -output "$APP/Contents/MacOS/PerAppVolume"
cp App/Info.plist "$APP/Contents/Info.plist"

echo "==> Icon"
swiftc -O scripts/make-icon.swift -o "$BUILD/obj/mkicon"
"$BUILD/obj/mkicon" "$BUILD/obj/AppIcon.iconset"
iconutil -c icns "$BUILD/obj/AppIcon.iconset" -o "$APP/Contents/Resources/AppIcon.icns"

echo "==> Embed driver"
cp -R "$DRV" "$APP/Contents/Resources/PerAppVolume.driver"

codesign --force --deep --sign - "$APP"

echo "==> Package"
mkdir -p dist
rm -rf "dist/Per-App Volume.app" "dist/Per-App-Volume.zip"
cp -R "$APP" "dist/Per-App Volume.app"
(cd dist && ditto -c -k --keepParent "Per-App Volume.app" "Per-App-Volume.zip")

echo "==> Done: dist/Per-App Volume.app  (zip: dist/Per-App-Volume.zip)"
