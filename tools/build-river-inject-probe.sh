#!/bin/sh
# Build tools/river-inject-probe.c as an ad-hoc signed iOS 16 arm64 dylib,
# installed on the phone as /usr/local/lib/libriverprobe.dylib.
#   tools/build-river-inject-probe.sh OUT
set -eu
cd "$(dirname "$0")/.."
out=${1:?usage: tools/build-river-inject-probe.sh OUT}
xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=16.0 -O2 -Wall \
  -dynamiclib -install_name /usr/local/lib/libriverprobe.dylib \
  -framework IOKit -framework CoreFoundation -o "$out" tools/river-inject-probe.c
codesign -f -s - "$out"
