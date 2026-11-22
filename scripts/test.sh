#!/bin/bash
# Run the tests (needs only Command Line Tools). Nothing is installed and no audio device is touched:
# the engine and the driver are driven in-process against simulated counterparts.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=build/test
mkdir -p "$OUT"
CFLAGS="-O1 -g -Wall -Wno-unused-function -Wno-unused-parameter -mmacosx-version-min=13.0 -fsanitize=address,undefined"
FRAMEWORKS="-framework CoreAudio -framework CoreFoundation -framework IOSurface"

echo "==> Engine"
clang $CFLAGS $FRAMEWORKS tests/engine_test.c -o "$OUT/engine_test"
"$OUT/engine_test"

echo "==> Driver"
clang $CFLAGS -I App/Engine $FRAMEWORKS tests/driver_test.c -o "$OUT/driver_test"
"$OUT/driver_test"

echo "==> App"
clang -O2 -Wall -mmacosx-version-min=13.0 -c App/Engine/PAVEngine.c -o "$OUT/PAVEngine.o"
SOURCES=()
for f in App/Sources/*.swift; do [ "$(basename "$f")" = main.swift ] || SOURCES+=("$f"); done
swiftc -swift-version 5 -target "$(uname -m)-apple-macos13.0" -import-objc-header App/Engine/PAVEngine.h \
  "${SOURCES[@]}" tests/app/main.swift "$OUT/PAVEngine.o" \
  -framework CoreAudio -framework IOSurface -framework AppKit -framework SwiftUI -framework Carbon -framework ServiceManagement \
  -o "$OUT/app_test"
"$OUT/app_test"

echo "==> All tests passed"
