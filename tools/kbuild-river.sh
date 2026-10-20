#!/bin/sh
# River kernel build on GitHub Actions, then a RAM-only river boot image.
#   REPO=owner/ios-topaz tools/kbuild-river.sh NAME [BOOTARGS]
# Dispatches xnu.yml with board=river on the pushed HEAD of the current
# branch, downloads the xnu-kernel artifact to ../k-NAME, packs it with
# tools/mkadt_river.py's ADT and builds shim-river/out/river-NAME-boot.img
# from the private boot_b.img/base DTB in ../device-private (or $RIVER_PRIVATE).
# Never calls fastboot: boot the image yourself with `fastboot boot`.
set -eu
cd "$(dirname "$0")/.."
name=${1:?usage: REPO=owner/ios-topaz kbuild-river.sh NAME [BOOTARGS]}
repo=${REPO:?set REPO to the GitHub fork that runs xnu.yml}
priv=${RIVER_PRIVATE:-../device-private}
# Storage-free, one Gold CPU, verbose early console; no rd= root device yet.
args=${2:-"debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress wdt=-1"}
branch=$(git rev-parse --abbrev-ref HEAD)
head=$(git rev-parse HEAD)
[ "$(git rev-parse "@{u}" 2>/dev/null)" = "$head" ] || { echo "push $branch to its upstream first" >&2; exit 1; }

t0=$(date -u +%Y-%m-%dT%H:%M:%SZ)
gh workflow run xnu.yml -R "$repo" --ref "$branch" -f board=river
run=""
for i in $(seq 30); do
  sleep 4
  run=$(gh run list -R "$repo" -w xnu.yml -L 5 --json databaseId,headSha,createdAt \
    -q ".[]|select(.headSha==\"$head\" and .createdAt>=\"$t0\")|.databaseId" | head -1)
  [ -n "$run" ] && break
done
[ -n "$run" ] || { echo "dispatched run not found" >&2; exit 1; }
echo "run $run (https://github.com/$repo/actions/runs/$run)"
gh run watch -R "$repo" "$run" --exit-status >/dev/null || {
  gh run view -R "$repo" "$run" --log-failed 2>/dev/null | grep -E "[0-9]+: error:" | head -20 >&2 || true
  exit 1
}
k=../k-$name
rm -rf "$k"; mkdir -p "$k"
gh run download -R "$repo" "$run" -n xnu-kernel -D "$k"
gh run download -R "$repo" "$run" -n xnu-buildtree -D "$k" || true
kernel=$k/kernel.development.sweet
# The board switch must have reached the compiler: river-only strings.
strings "$kernel" | grep -q "RIVER GIC" || { echo "$kernel is not a river build" >&2; exit 1; }
python3 tools/mkadt_river.py "$k/river.adt"
python3 tools/mkpayload.py "$kernel" "$k/river.adt" "$k/payload.bin" --cmdline "$args"
python3 shim-river/build.py --boot "$priv/boot_b.img" --dtb "$priv/boot_b-kernel.dtb" \
  --payload "$k/payload.bin" -o "shim-river/out/river-$name-boot.img"
