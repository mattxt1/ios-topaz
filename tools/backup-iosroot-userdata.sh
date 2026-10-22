#!/bin/bash
# Read the bare HFSX iOS root back from river userdata, from a root ADB
# recovery shell, into a gzip file on the Mac. Read-only: the partition is
# only opened for reading. The SHA-256 of the raw bytes is computed on the
# Mac while streaming and on the phone afterwards, and must match.
#   tools/backup-iosroot-userdata.sh OUT.img.gz [BYTES]
# BYTES defaults to 16 GiB (the root image written by flash-iosroot-userdata.sh).
# Restore: gunzip, then tools/flash-iosroot-userdata.sh on the .img (a write).
set -euo pipefail

[[ $# == 1 || $# == 2 ]] || { echo "usage: $0 OUT.img.gz [BYTES]" >&2; exit 2; }
out=$1
bytes=${2:-17179869184}
[[ ! -e $out ]] || { echo "refusing to overwrite $out" >&2; exit 1; }
[[ $((bytes % 4194304)) == 0 ]] || { echo 'BYTES must be a multiple of 4 MiB' >&2; exit 1; }
blocks=$((bytes / 4194304))

device=/dev/block/mmcblk0p64
if [[ -n ${RIVER_SERIAL:-} ]]; then
	serial=$RIVER_SERIAL
else
	recovery_serials=$(adb devices | awk '$2 == "recovery" {print $1}')
	if [[ -z $recovery_serials || $recovery_serials == *$'\n'* ]]; then
		echo 'connect exactly one ADB recovery device or set RIVER_SERIAL' >&2; exit 1
	fi
	serial=$recovery_serials
fi
phone() { adb -s "$serial" "$@"; }
state=$(adb devices | awk -v serial="$serial" '$1 == serial {print $2}')
[[ $state == recovery ]] || { echo "phone must be in ADB recovery (got $state)" >&2; exit 1; }
[[ $(phone shell 'id -u' | tr -d '\r') == 0 ]] || { echo 'recovery ADB is not root' >&2; exit 1; }
[[ $(phone shell 'getprop ro.product.device' | tr -d '\r') == river ]] || {
	echo 'connected device is not river' >&2; exit 1;
}
[[ $(phone shell 'readlink /dev/block/by-name/userdata' | tr -d '\r') == "$device" ]] || {
	echo 'userdata does not point at the expected partition' >&2; exit 1;
}
partition_bytes=$(phone shell "blockdev --getsize64 $device" | tr -d '\r')
[[ $partition_bytes == 53729017344 && $bytes -le $partition_bytes ]] || {
	echo "unexpected partition size: $partition_bytes (want 53729017344, reading $bytes)" >&2; exit 1;
}
if phone shell mount | grep -Eq 'mmcblk0p64| /data '; then
	echo 'userdata is mounted; refusing (it could change while being read)' >&2; exit 1;
fi
[[ $(phone exec-out "dd if=$device bs=1 skip=1024 count=2 2>/dev/null") == HX ]] || {
	echo 'userdata does not start with a bare case-sensitive HFS+ volume' >&2; exit 1;
}

mkdir -p "$(dirname "$out")"
echo "Reading $bytes bytes of $device to $out"
local_hash=$(phone exec-out "dd if=$device bs=4M count=$blocks 2>/dev/null" |
	tee >(gzip -1 > "$out") | shasum -a 256 | awk '{print $1}')
sleep 2	# let gzip finish writing
remote_hash=$(phone shell -T "dd if=$device bs=4M count=$blocks 2>/dev/null | sha256sum" | awk '{print $1}')
[[ $remote_hash == "$local_hash" ]] || {
	echo "hash mismatch: phone $remote_hash, Mac $local_hash" >&2; exit 1;
}
echo "$local_hash  ${out%.gz}" > "$out.sha256"
gzip -t "$out"
echo "Backup verified: $local_hash ($(stat -f %z "$out") bytes compressed)"
