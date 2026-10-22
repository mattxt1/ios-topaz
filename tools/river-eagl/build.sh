#!/bin/sh
# Build the River EAGL adapter as an ad-hoc signed iOS 16 arm64 dylib,
# installed on the phone as /usr/local/lib/libriveregl.dylib.
#   tools/river-eagl/build.sh OUT
set -eu
cd "$(dirname "$0")"
out=${1:?usage: tools/river-eagl/build.sh OUT}
xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=16.0 -O2 -Wall \
  -dynamiclib -install_name /usr/local/lib/libriveregl.dylib \
  -framework IOKit -framework CoreFoundation -framework OpenGLES -lobjc \
  -o "$out" river-eagl.c river-eagl-stubs.S
codesign -f -s - "$out"
