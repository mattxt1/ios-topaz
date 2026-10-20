#!/bin/bash
# Read river's persistent console from the unused userdata tail in recovery.
# This script opens the phone's partition only for reading.
set -euo pipefail
umask 077
[[ $# == 1 ]] || { echo "usage: $0 PRIVATE-LOG.txt" >&2; exit 2; }
out=$1
device=/dev/block/mmcblk0p64
if [[ -n ${RIVER_SERIAL:-} ]]; then
	serial=$RIVER_SERIAL
else
	serial=$(adb devices | awk '$2 == "recovery" {print $1}')
	[[ -n $serial && $serial != *$'\n'* ]] || {
		echo 'connect exactly one ADB recovery device or set RIVER_SERIAL' >&2; exit 1;
	}
fi
phone() { adb -s "$serial" "$@"; }
[[ $(adb devices | awk -v s="$serial" '$1 == s {print $2}') == recovery ]] || {
	echo 'phone must be in ADB recovery' >&2; exit 1;
}
[[ $(phone shell 'id -u' | tr -d '\r') == 0 ]] || {
	echo 'recovery ADB is not root' >&2; exit 1;
}
[[ $(phone shell 'getprop ro.product.device' | tr -d '\r') == river ]] || {
	echo 'connected device is not river' >&2; exit 1;
}
[[ $(phone shell 'readlink -f /dev/block/by-name/userdata' | tr -d '\r') == "$device" ]] || {
	echo 'unexpected userdata partition' >&2; exit 1;
}
bytes=$(phone shell "blockdev --getsize64 $device" | tr -d '\r')
[[ $bytes == 53729017344 ]] || { echo 'unexpected userdata length' >&2; exit 1; }
skip=$((bytes / 4096 - 1025))
mkdir -p "$(dirname "$out")"
phone exec-out "dd if=$device bs=4096 skip=$skip count=1025 2>/dev/null" > "$out.bin"
chmod 600 "$out.bin"
[[ $(wc -c < "$out.bin" | tr -d ' ') == 4198400 ]] || {
	echo 'short log read' >&2; exit 1;
}
python3 "$(dirname "$0")/sweet-log.py" "$out.bin" "$out"
chmod 600 "$out"
echo "Saved local console log: $out" >&2
