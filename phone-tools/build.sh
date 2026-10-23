#!/bin/sh
# Build River's on-phone tools for iOS 16 arm64, signed ad hoc (with NAME-ents.plist
# when there is one), into ../../userland/tools/out (not in git).
#   rput     PATH MODE < data: install a file over SSH (iOS has no cat/chmod)
#   oslog    [-t SECS] [-l] [PROC...]: stream the unified log      (from NWylynko/ios-flame)
#   exq      extension records per point (EXQ_POINTS=1)            (from NWylynko/ios-flame)
#   psx      processes without /proc; psx -k NAME kills            (from NWylynko/ios-flame)
#   pstack   PID: thread backtraces                                (from NWylynko/ios-flame)
#   libcgprobe.dylib: logs failed CGBitmapContextCreate calls with unslid return
#            addresses (load with riverenv=PROC:DYLD_INSERT_LIBRARIES=/usr/local/lib/libcgprobe.dylib)
# Install: tools/river-ssh 'rput /usr/local/bin/NAME 755' < ../userland/tools/out/NAME
set -eu
cd "$(dirname "$0")"
out=../../userland/tools/out
mkdir -p "$out"
cc() { xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=16.0 -O2 -Wall "$@"; }
sign() {
	if [ -f "$1-ents.plist" ]; then codesign -f -s - --entitlements "$1-ents.plist" "$out/$1"
	else codesign -f -s - "$out/$1"; fi
}
for c in rput rget psx pstack; do cc -o "$out/$c" "$c.c"; sign "$c"; done
for m in oslog exq; do cc -fobjc-arc -framework Foundation -o "$out/$m" "$m.m"; sign "$m"; done
cc -Wno-deprecated-declarations -dynamiclib -install_name /usr/local/lib/libcgprobe.dylib \
	-framework CoreGraphics -framework CoreFoundation -o "$out/libcgprobe.dylib" cgprobe.c
codesign -f -s - "$out/libcgprobe.dylib"
cc -fobjc-arc -framework Foundation -larchive -o "$out/appinstall" appinstall.m
sign appinstall
cc -fobjc-arc -framework Foundation -o "$out/applaunch" applaunch.m
sign applaunch
cc -o "$out/btaudio-toggle" btaudio-toggle.c
sign btaudio-toggle
cc -framework IOKit -o "$out/netprobe" netprobe.c
sign netprobe
ls -l "$out"
