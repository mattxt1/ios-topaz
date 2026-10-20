#!/bin/bash
# Stream a bare HFSX root to river userdata from a root ADB recovery shell.
# Only /dev/block/mmcblk0p64 is ever opened for writing.
set -euo pipefail

[[ $# == 1 ]] || { echo "usage: $0 ios16-root.img" >&2; exit 2; }
image=$1
[[ -f $image ]] || { echo "no image: $image" >&2; exit 1; }
bytes=$(stat -f %z "$image")
[[ $(dd if="$image" bs=1 skip=1024 count=2 2>/dev/null) == HX ]] || {
	echo 'image is not a bare case-sensitive HFS+ volume' >&2; exit 1;
}
[[ $((bytes % 4194304)) == 0 ]] || {
	echo 'image length must be a multiple of 4 MiB for readback verification' >&2; exit 1;
}

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
	echo "unexpected partition or image size: partition=$partition_bytes image=$bytes" >&2; exit 1;
}
if phone shell mount | grep -Eq 'mmcblk0p64| /data '; then
	echo 'userdata is mounted; refusing to write' >&2; exit 1;
fi

local_hash=$(shasum -a 256 "$image" | awk '{print $1}')
echo "Writing $bytes bytes ($local_hash) to $device in recovery"
phone shell -T "dd of=$device bs=4M conv=fsync" < "$image"
blocks=$((bytes / 4194304))
remote_hash=$(phone shell -T "dd if=$device bs=4M count=$blocks 2>/dev/null | sha256sum" | awk '{print $1}')
[[ $remote_hash == "$local_hash" ]] || {
	echo "readback hash mismatch: $remote_hash != $local_hash" >&2; exit 1;
}
echo "Readback verified: $remote_hash"
