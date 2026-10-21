#!/bin/sh
# Build tools/rivergpu-test.c for the phone (iOS 16 arm64, ad-hoc signed;
# SWEET AMFI treats ad-hoc binaries on the root filesystem as platform).
#   tools/build-rivergpu-test.sh OUT
set -eu
cd "$(dirname "$0")/.."
out=${1:?usage: tools/build-rivergpu-test.sh OUT}
xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=16.0 -O2 -Wall \
  -framework IOKit -framework CoreFoundation -o "$out" tools/rivergpu-test.c
codesign -f -s - "$out"
