#!/bin/sh
# Minimal case-sensitive HFS+ root for rd=md0 tests, built with macOS tools.
#   tools/mkroot-macos.sh INIT OUT.img [SIZE]      (SIZE default 8m)
# INIT (tools/mkinit.py output) becomes /sbin/launchd. The volume is named
# SweetRoot, not journaled, with no partition map (-layout NONE), so OUT.img
# is the bare HFS+ volume that mkpayload.py --ramdisk expects.
set -eu
init=${1:?usage: mkroot-macos.sh INIT OUT.img [SIZE]}
out=${2:?usage: mkroot-macos.sh INIT OUT.img [SIZE]}
size=${3:-8m}
[ ! -e "$out" ] || { echo "refusing to overwrite $out" >&2; exit 1; }
tmp=$(mktemp -d)
hdiutil create -quiet -size "$size" -fs "Case-sensitive HFS+" -volname SweetRoot \
  -layout NONE -type UDIF "$tmp/root.dmg"
mnt=$(hdiutil attach -nobrowse -noverify "$tmp/root.dmg" | awk -F'\t' '/Volumes/{print $NF}')
mkdir -p "$mnt/sbin" "$mnt/dev" "$mnt/private/var" "$mnt/tmp"
cp "$init" "$mnt/sbin/launchd"
chmod 755 "$mnt/sbin/launchd"
hdiutil detach -quiet "$mnt"
hdiutil convert -quiet "$tmp/root.dmg" -format UDTO -o "$tmp/root"
mv "$tmp/root.cdr" "$out"
rm -rf "$tmp"
# HFSX signature "HX" at offset 1024
[ "$(dd if="$out" bs=1 skip=1024 count=2 2>/dev/null)" = HX ] || { echo "$out: not HFSX" >&2; exit 1; }
ls -l "$out"
