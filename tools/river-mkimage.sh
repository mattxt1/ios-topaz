#!/bin/sh
# Build a RAM-only River boot image from a CI kernel directory.
#   tools/river-mkimage.sh KDIR NAME [-l] [-a 'extra boot-args'] [-r riverenv] [SRC:DEST:MODE ...]
# KDIR   ../k-r76 (holds kernel.development.sweet from CI; ADT and GPU firmware are made here)
# NAME   output shim-river/out/river-NAME-boot.img
# -l     1.65 GB low window instead of the full 4 GiB window (full needs kernel 0205+)
# -a     extra boot-args (e.g. 'riverfilessys=1 sweetnull=dropbear')
# -r     riverenv value (default backboardd:DYLD_INSERT_LIBRARIES=/usr/local/lib/sweetgl.dylib)
# SRC:DEST:MODE files for the boot-time file drop (river-files.py); DEST outside /usr/local
#        needs -a riverfilessys=1. Keep images under ~38 MB (ABL refused 39 MB):
#        sweetgl.dylib (20 MB) is already on the phone, so leave it out.
# Base boot-args come from ../k-r49/payload.bin (they carry the private RTC offset).
set -eu
cd "$(dirname "$0")/.."
k=$1 name=$2
shift 2
full=--full-ram extra= env=backboardd:DYLD_INSERT_LIBRARIES=/usr/local/lib/sweetgl.dylib
while getopts la:r: o; do
	case $o in
	l) full= ;;
	a) extra=$OPTARG ;;
	r) env=$OPTARG ;;
	*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))
[ -f "$k/kernel.development.sweet" ] || { echo "no $k/kernel.development.sweet" >&2; exit 1; }
python3 tools/mkadt_river.py $full "$k/river-$name.adt" >/dev/null
[ -f "$k/river-gpufw.bin" ] || python3 tools/river-gpufw.py ../device-private/gpu-firmware "$k/river-gpufw.bin" >/dev/null
base="$(strings -n 20 ../k-r49/payload.bin | grep -m1 'debug=0xc')"
blobs="--blob RiverGPUFW=$k/river-gpufw.bin"
if [ $# -gt 0 ]; then
	python3 tools/river-files.py "$k/river-files-$name.bin" "$@" | cut -c1-120
	blobs="$blobs --blob RiverFiles=$k/river-files-$name.bin"
fi
python3 tools/mkpayload.py "$k/kernel.development.sweet" "$k/river-$name.adt" "$k/payload-$name.bin" \
	--cmdline "$base riverenv=$env riverfbinv=0 riverblit=1${extra:+ $extra}" $blobs >/dev/null
python3 shim-river/build.py --boot ../device-private/boot_b.img --dtb ../device-private/boot_b-kernel.dtb \
	--payload "$k/payload-$name.bin" -o "shim-river/out/river-$name-boot.img" | tail -1
