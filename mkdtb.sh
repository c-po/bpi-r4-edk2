#!/bin/bash
# Extract the BPI-R4 device tree from the VyOS ISO's squashfs and merge the SD
# and the eMMC overlay into it, one DTB each.
#
# Why this is needed: mmc0 is status="disabled" in the base
# mt7988a-bananapi-bpi-r4.dtb -- the SD card or eMMC only appears once
# the -sd or -emmc overlay is applied. U-Boot's EFI loader otherwise
# hands the kernel its own cut-down control FDT, which is not the mainline
# tree the VyOS kernel's drivers bind against.
set -euo pipefail
ROOT=${ROOT:-/vyos/bpi-r4}
. "$ROOT/common.sh"
ISODIR=${ISODIR:-/tmp/vyos-iso}
KVER=${KVER:-$(detect_kver "$ISODIR")}
OUT=${OUT:-$ROOT/dtb}
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

SQ="$ISODIR/live/filesystem.squashfs"
[ -f "$SQ" ] || { echo "ERROR: $SQ not found - run ./unpack-iso.sh first" >&2; exit 1; }

DTDIR="usr/lib/linux-image-$KVER/mediatek"
unsquashfs -d "$WORK/sq" -f -n "$SQ" \
  "$DTDIR/mt7988a-bananapi-bpi-r4.dtb" \
  "$DTDIR/mt7988a-bananapi-bpi-r4-sd.dtbo" \
  "$DTDIR/mt7988a-bananapi-bpi-r4-emmc.dtbo" >/dev/null

B="$WORK/sq/$DTDIR"
for f in mt7988a-bananapi-bpi-r4.dtb mt7988a-bananapi-bpi-r4-sd.dtbo mt7988a-bananapi-bpi-r4-emmc.dtbo; do
    [ -f "$B/$f" ] || { echo "ERROR: kernel does not ship $f" >&2; exit 1; }
done

# SD and eMMC share mmc0 on different pins: one merged DTB per boot medium
mkdir -p "$OUT"
for m in sd emmc; do
    dtb="$OUT/mt7988a-bananapi-bpi-r4-$m.dtb"
    fdtoverlay -i "$B/mt7988a-bananapi-bpi-r4.dtb" -o "$dtb" \
               "$B/mt7988a-bananapi-bpi-r4-$m.dtbo"

    # fail loudly if the overlay did not actually enable the MMC controller
    if ! dtc -I dtb -O dts "$dtb" 2>/dev/null \
         | grep -A26 "mmc@11230000" | grep -q 'status = "okay"'; then
        echo "ERROR: mmc0 is not enabled in $dtb" >&2; exit 1
    fi
    echo "Built $dtb ($(stat -c%s "$dtb") bytes, mmc0 enabled)"
done
