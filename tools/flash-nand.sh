#!/bin/sh
# Write the EDK2 SPI-NAND firmware into the BPI-R4 NAND, OpenWrt layout:
#   mtd "bl2"            BL2, four copies 512 KiB apart (BootROM fallbacks)
#   UBI volume "fip"     FIP (BL31 + EDK2), read by BL2 through UBI
# Every other UBI volume is left alone.
#
# Run as root on a VyOS live system booted with a device tree whose "bl2"
# partition is not read-only (the stick's "NAND writable" GRUB entry), with
# mtd-utils installed.
#
# usage: flash-nand.sh <nand-bl2-4x.bin> <nand-fip.bin>
set -eu

BL2=$1
FIP=$2

die() { echo "ERROR: $*" >&2; exit 1; }

mtd_by_name() {
    awk -v n="\"$1\"" '$4 == n { sub(":", "", $1); print $1 }' /proc/mtd
}

ubi_vol_by_name() {
    for v in /sys/class/ubi/ubi*_*; do
        [ "$(cat "$v/name")" = "$1" ] && { basename "$v"; return; }
    done
}

for t in flash_erase nandwrite ubiupdatevol; do
    command -v $t >/dev/null || die "$t missing, install mtd-utils"
done

BL2_MTD=$(mtd_by_name bl2)
[ -n "$BL2_MTD" ] || die "no mtd partition named bl2"
FIP_VOL=$(ubi_vol_by_name fip)
[ -n "$FIP_VOL" ] || die "no UBI volume named fip (is ubi0 attached?)"

# MTD_WRITEABLE (0x400) must be set, i.e. no "read-only" in the DT
flags=$(cat /sys/class/mtd/$BL2_MTD/flags)
[ $((flags & 0x400)) -ne 0 ] || die "/dev/$BL2_MTD is read-only, boot the NAND writable entry"

size=$(cat /sys/class/mtd/$BL2_MTD/size)
[ "$(stat -c%s "$BL2")" -eq "$size" ] || die "$BL2 is not exactly $size bytes"
for off in 0 524288 1048576 1572864; do
    [ "$(tail -c +$((off + 1)) "$BL2" | head -c 8)" = "SPINAND!" ] \
        || die "$BL2: no SPINAND! header at $off"
done

vol_size=$(cat /sys/class/ubi/$FIP_VOL/data_bytes)
[ "$(stat -c%s "$FIP")" -le "$vol_size" ] || die "$FIP is larger than the fip volume"

echo "Writing $BL2 -> /dev/$BL2_MTD"
flash_erase /dev/$BL2_MTD 0 0
nandwrite -p /dev/$BL2_MTD "$BL2"
dd if=/dev/$BL2_MTD bs=128k status=none | cmp - "$BL2" || die "BL2 read-back mismatch"

echo "Writing $FIP -> /dev/$FIP_VOL"
ubiupdatevol /dev/$FIP_VOL "$FIP"
head -c "$(stat -c%s "$FIP")" /dev/$FIP_VOL | cmp - "$FIP" \
    || die "FIP read-back mismatch"

sync
echo "Done: BL2 and FIP written and verified."
