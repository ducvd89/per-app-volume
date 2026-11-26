#!/bin/bash
# Build the HAL driver and the menu bar app (needs only Command Line Tools).
set -euo pipefail
cd "$(dirname "$0")"
BUILD=build
ARCHS="-arch arm64 -arch x86_64"
MINOS=13.0
# Keep the path of the source folder out of the binaries.
CMAP="-ffile-prefix-map=$PWD=."
SMAP="-file-prefix-map $PWD=."
rm -rf "$BUILD"; mkdir -p "$BUILD/obj"

echo "==> Driver"
DRV="$BUILD/PerAppVolume.driver"
mkdir -p "$DRV/Contents/MacOS" "$DRV/Contents/Resources"
clang $ARCHS -mmacosx-version-min=$MINOS -O2 -Wall "$CMAP" -bundle \
  -framework CoreAudio -framework CoreFoundation -framework IOSurface -I App/Engine \
  -o "$DRV/Contents/MacOS/PerAppVolume" Driver/PerAppVolumeDriver.c
cp Driver/Info.plist "$DRV/Contents/Info.plist"
codesign --force --sign - "$DRV"

echo "==> App"
APP="$BUILD/Per-App Volume.app"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
for A in arm64 x86_64; do
  clang -arch $A -mmacosx-version-min=$MINOS -O2 -Wall "$CMAP" -c App/Engine/PAVEngine.c -o "$BUILD/obj/PAVEngine-$A.o"
  swiftc -O -swift-version 5 -target $A-apple-macos$MINOS $SMAP \
    -import-objc-header App/Engine/PAVEngine.h \
    App/Sources/*.swift "$BUILD/obj/PAVEngine-$A.o" \
    -framework CoreAudio -framework IOSurface -framework AppKit -framework SwiftUI -framework Carbon -framework ServiceManagement \
    -o "$BUILD/obj/PerAppVolume-$A"
done
lipo -create "$BUILD/obj/PerAppVolume-arm64" "$BUILD/obj/PerAppVolume-x86_64" -output "$APP/Contents/MacOS/PerAppVolume"
cp App/Info.plist "$APP/Contents/Info.plist"

# Updates. The app looks for new releases in the GitHub repo it was built from and installs only what was
# signed with the builder's key (scripts/update-key.swift). Both are written into the built app here, so
# the source names neither. Without a GitHub remote or without a key, the app has no updater.
REPO=$(git remote get-url origin 2>/dev/null | sed -nE 's#^(https://github\.com/|git@github\.com:)([^/]+/[^/]+)$#\2#p' | sed 's/\.git$//' || true)
UPDATE_KEY=$(swift scripts/update-key.swift public 2>/dev/null || true)
if [ -n "$REPO" ] && [ -n "$UPDATE_KEY" ]; then
  /usr/libexec/PlistBuddy -c "Add :PAVUpdateRepo string $REPO" -c "Add :PAVUpdateKey string $UPDATE_KEY" "$APP/Contents/Info.plist"
  echo "==> Updates: on (signed with the key in the Keychain)"
else
  UPDATE_KEY=""
  echo "==> Updates: off (needs a GitHub remote and an update key)"
fi

echo "==> Icon"
swiftc -O scripts/make-icon.swift -o "$BUILD/obj/mkicon"
"$BUILD/obj/mkicon" "$BUILD/obj/AppIcon.iconset"
iconutil -c icns "$BUILD/obj/AppIcon.iconset" -o "$APP/Contents/Resources/AppIcon.icns"

echo "==> Embed driver"
cp -R "$DRV" "$APP/Contents/Resources/PerAppVolume.driver"

codesign --force --deep --sign - "$APP"

echo "==> Package"
mkdir -p dist
rm -rf "dist/Per-App Volume.app" "dist/Per-App-Volume.zip" "dist/Per-App-Volume.dmg"
cp -R "$APP" "dist/Per-App Volume.app"
(cd dist && ditto -c -k --keepParent "Per-App Volume.app" "Per-App-Volume.zip")
# The signature the app checks before it installs this zip as an update.
rm -f dist/Per-App-Volume.zip.sig
if [ -n "$UPDATE_KEY" ]; then swift scripts/update-key.swift sign dist/Per-App-Volume.zip > dist/Per-App-Volume.zip.sig; fi

# Drag-to-install disk image: the app plus a link to /Applications.
STAGE="$BUILD/dmg"
rm -rf "$STAGE"; mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/Per-App Volume.app"
ln -s /Applications "$STAGE/Applications"
hdiutil create -quiet -volname "Per-App Volume" -srcfolder "$STAGE" -fs HFS+ -format UDZO -ov "dist/Per-App-Volume.dmg"

echo "==> Done: dist/Per-App Volume.app, dist/Per-App-Volume.zip, dist/Per-App-Volume.dmg"
