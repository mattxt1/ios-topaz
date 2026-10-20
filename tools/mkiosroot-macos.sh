#!/bin/bash
# Build a bare case-sensitive HFS+ iOS root on macOS from the three APFS
# images named by the D22AP Erase identity in BuildManifest.plist.
#
#   sudo tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg OUT.img [SIZE]
#
# The output is a raw, partition-map-free HFSX volume for rd=emmc0s64.
# Root is required to retain the source numeric owners and groups.
set -euo pipefail

if [[ $# -lt 4 || $# -gt 5 ]]; then
	echo 'usage: sudo tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg OUT.img [SIZE]' >&2
	exit 2
fi
if [[ $(id -u) -ne 0 ]]; then
	echo 'run as root to preserve iOS file ownership (sudo tools/mkiosroot.sh ...)' >&2
	exit 1
fi

os=$(realpath "$1")
sysos=$(realpath "$2")
appos=$(realpath "$3")
out=$4
size=${5:-16g}
[[ -f $os && -f $sysos && -f $appos ]] || { echo 'missing source DMG' >&2; exit 1; }
[[ ! -e $out ]] || { echo "refusing to overwrite $out" >&2; exit 1; }

work=$(mktemp -d)
work=$(cd "$work" && pwd -P)  # /var on macOS resolves to /private/var in mount output
image="$work/root.cdr"
mounts=()
cleanup() {
	local m
	for ((m=${#mounts[@]}-1; m>=0; m--)); do
		hdiutil detach "${mounts[m]}" -quiet || true
	done
	# Keep the work directory if a mount is still live.
	if ! mount | grep -Fq " on $work/"; then
		rm -rf "$work"
	else
		echo "mounted image remains under $work; detach it before removing files" >&2
	fi
}
trap cleanup EXIT

mount_source() {
	local src=$1 name=$2 dest="$work/$2"
	mkdir "$dest"
	hdiutil attach -quiet -readonly -nobrowse -owners on -noverify -mountpoint "$dest" "$src"
	mounts+=("$dest")
	# apfs-fuse exposes an additional root/ directory; native APFS does not.
	if [[ -d $dest/root/System ]]; then dest="$dest/root"; fi
	mounted_source=$dest
}
mount_source "$os" os; osroot=$mounted_source
mount_source "$sysos" sysos; sysroot=$mounted_source
mount_source "$appos" appos; approot=$mounted_source
[[ -d $osroot/System && -d $sysroot/System ]] || {
	echo 'OS or SystemOS image did not mount at its filesystem root' >&2
	exit 1
}

# UDTO has no UDIF footer: the .cdr bytes are directly writable to eMMC.
hdiutil create -quiet -size "$size" -fs 'Case-sensitive Journaled HFS+' \
	-volname iOSRoot -layout NONE -type UDTO "$work/root"
mkdir "$work/target"
hdiutil attach -quiet -nobrowse -owners on -mountpoint "$work/target" "$image"
mounts+=("$work/target")
target="$work/target"

copy_tree() {
	local source=$1 destination=$2
	mkdir -p "$destination"
	rsync -aH --numeric-ids --extended-attributes "$source/" "$destination/"
}
echo 'copying OS'
copy_tree "$osroot" "$target"
echo 'copying SystemOS cryptex'
copy_tree "$sysroot" "$target/private/preboot/Cryptexes/OS"
echo 'copying AppOS cryptex'
copy_tree "$approot" "$target/private/preboot/Cryptexes/App"

# At ignition level 1 dyld expects the shared cache at this root path.
dsc=private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld
mkdir -p "$target/System/Library/Caches/com.apple.dyld"
for f in "$target/$dsc"/*; do
	[[ -f $f ]] || continue
	ln "$f" "$target/System/Library/Caches/com.apple.dyld/${f##*/}"
done

# Link cryptex files absent from the OS root, retaining symlinks as symlinks.
for c in OS App; do
	base="$target/private/preboot/Cryptexes/$c"
	while IFS= read -r -d '' f; do
		rel=${f#"$base/"}
		[[ $rel == System/Library/Caches/com.apple.dyld/* ]] && continue
		dst="$target/$rel"
		if [[ ! -e $dst && ! -L $dst ]]; then
			mkdir -p "${dst%/*}"
			if [[ -L $f ]]; then cp -P "$f" "$dst"; else ln "$f" "$dst"; fi
		fi
	done < <(find "$base" ! -type d -print0)
done

db="$target/private/var/db"
install -d -o 272 -g 272 -m 0751 "$db/diagnostics"
install -d -o 272 -g 272 -m 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for d in diagnostics uuidtext diagnostics/timesync; do
	install -o 272 -g 272 -m 0644 /dev/null "$db/$d/.migrated"
done

install -d -m 0700 -o 0 -g 0 "$target/private/var/keybags"
cat > "$target/private/var/keybags/systembag.kb" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>
EOF
chown 0:0 "$target/private/var/keybags/systembag.kb"
chmod 0600 "$target/private/var/keybags/systembag.kb"

df -h "$target"
hdiutil detach "$target" -quiet
unset "mounts[$((${#mounts[@]}-1))]"
[[ $(dd if="$image" bs=1 skip=1024 count=2 2>/dev/null) == HX ]] || {
	echo 'output is not a bare HFSX volume' >&2; exit 1;
}
mv "$image" "$out"
echo "created $out"
