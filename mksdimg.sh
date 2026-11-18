#!/bin/bash
# Build a bootable BPI-R4 (MT7988A) SD card image:
#   ATF BL2 + FIP(BL31 + BL33) + FAT32 ESP holding GRUB arm64.
#
# BL33 is EDK2 (see mkedk2.sh) unless FIRMWARE=uboot selects the older
# U-Boot w/ EFI_LOADER build. Both boot the same ESP.
#
# Layout follows frank-w / OpenWrt's proven BPI-R4 numbers. The MT7988 BootROM
# locates BL2 via the GPT partition carrying attribute bit 2 ("legacy BIOS
# bootable"); ATF BL2 then locates the FIP by GPT partition *name* "fip"
# (plat/mediatek/apsoc_common/bl2/bl2_boot_mmc.c:167-181). Raw dd offsets alone
# are NOT enough -- the GPT is mandatory.
#
# Fully rootless: the FAT filesystem is built with mtools, never loop-mounted.
set -euo pipefail

ROOT=/vyos/bpi-r4
FIRMWARE=${FIRMWARE:-edk2}
case "$FIRMWARE" in
    edk2)  ATF=$ROOT/arm-trusted-firmware-mtk/build-edk2/mt7988/release
           DEFAULT_IMG=$ROOT/bpi-r4-edk2.img ;;
    uboot) ATF=$ROOT/arm-trusted-firmware-mtk/build/mt7988/release
           DEFAULT_IMG=$ROOT/bpi-r4-uboot-efi.img ;;
    *)     echo "ERROR: FIRMWARE must be edk2 or uboot, not '$FIRMWARE'" >&2; exit 1 ;;
esac
GRUBDIR=${GRUBDIR:-$ROOT/grub}
. "$ROOT/common.sh"
VYOS_ISO=${VYOS_ISO:-$(find_vyos_iso)}
ISODIR=${ISODIR:-/tmp/vyos-iso}   # unpacked ISO tree (see unpack-iso.sh)
DTBDIR=${DTBDIR:-$ROOT/dtb}       # merged board DTB (see mkdtb.sh)
IMG=${IMG:-$DEFAULT_IMG}
ESP_IMG=$(mktemp /tmp/esp.XXXXXX.img)
WORKCFG=$(mktemp /tmp/grubcfg.XXXXXX)
trap 'rm -f "$ESP_IMG" "$WORKCFG"' EXIT

KVER=${KVER:-$(detect_kver "$ISODIR")}

BL2=$ATF/bl2.img
FIP=$ATF/fip.bin

# --- partition geometry (512-byte sectors) ---------------------------------
BL2_START=34;      BL2_END=8191        # ~4 MB   attr bit 2 -> BROM loads BL2
ENV_START=8192;    ENV_END=9215        #  512 KB matches CONFIG_ENV_OFFSET=0x400000
FACT_START=9216;   FACT_END=13311      #    2 MB
FIP_START=13312;   FIP_END=17407       #    2 MB  found by name "fip"
ESP_START=17408;   ESP_END=5260287     # 2.5 GB FAT32 ESP (GRUB + unpacked VyOS live)
IMG_SIZE=${IMG_SIZE:-4G}               # rest left unallocated as a VyOS install target

BL2_MAX=$(( (BL2_END - BL2_START + 1) * 512 ))
FIP_MAX=$(( (FIP_END - FIP_START + 1) * 512 ))
ESP_BYTES=$(( (ESP_END - ESP_START + 1) * 512 ))

# --- sanity checks ----------------------------------------------------------
for f in "$BL2" "$FIP" "$GRUBDIR/bootaa64.efi" "$GRUBDIR/grub.cfg"; do
    [ -f "$f" ] || { echo "ERROR: missing input: $f" >&2; exit 1; }
done

check_fits() {  # file, max_bytes, label
    local sz; sz=$(stat -c%s "$1")
    if [ "$sz" -gt "$2" ]; then
        echo "ERROR: $3 is $sz bytes, exceeds its $2-byte partition" >&2; exit 1
    fi
    printf '  %-6s %8d bytes  (partition holds %d)\n' "$3" "$sz" "$2"
}
echo "Input sizes:"
check_fits "$BL2" "$BL2_MAX" bl2
check_fits "$FIP" "$FIP_MAX" fip

# --- GPT --------------------------------------------------------------------
echo "Creating $IMG ($IMG_SIZE)..."
rm -f "$IMG"
truncate -s "$IMG_SIZE" "$IMG"

# sfdisk (Debian "fdisk" package, installed by default), not sgdisk ("gdisk",
# optional). Bit 2 = LegacyBIOSBootable, the BootROM's BL2 marker; GUID:63 is
# the read-only bit the firmware partitions have always carried.
part() { echo "start=$1, size=$(($2 - $1 + 1)), type=$3, name=\"$4\"${5:+, attrs=\"$5\"}"; }
LINUX=0FC63DAF-8483-4772-8E79-3D69D8477DE4
EFISYS=C12A7328-F81F-11D2-BA4B-00A0C93EC93B
sfdisk -q "$IMG" <<EOF
label: gpt
first-lba: 34
$(part $BL2_START  $BL2_END  $LINUX  bl2        LegacyBIOSBootable)
$(part $ENV_START  $ENV_END  $LINUX  u-boot-env GUID:63)
$(part $FACT_START $FACT_END $LINUX  factory    GUID:63)
$(part $FIP_START  $FIP_END  $LINUX  fip        GUID:63)
$(part $ESP_START  $ESP_END  $EFISYS ESP)
EOF

# --- firmware ---------------------------------------------------------------
echo "Writing BL2 at LBA $BL2_START, FIP at LBA $FIP_START..."
dd if="$BL2" of="$IMG" bs=512 seek=$BL2_START conv=notrunc,fsync status=none
dd if="$FIP" of="$IMG" bs=512 seek=$FIP_START conv=notrunc,fsync status=none

# --- ESP (built standalone with mtools, then blitted in) --------------------
# does everything we intend to stage actually fit in the ESP?
need=$(stat -c%s "$GRUBDIR/bootaa64.efi")
[ -d "$ISODIR/live" ] && need=$((need + $(du -bc "$ISODIR"/live/{vmlinuz-$KVER,initrd.img-$KVER,filesystem.squashfs} 2>/dev/null | tail -1 | cut -f1)))
[ -f "$VYOS_ISO" ] && need=$((need + $(stat -c%s "$VYOS_ISO")))
if [ "$need" -gt $((ESP_BYTES * 95 / 100)) ]; then
    echo "ERROR: need $((need/1024/1024)) MB on the ESP but it is only $((ESP_BYTES/1024/1024)) MB" >&2
    exit 1
fi
printf '  ESP payload %d MB of %d MB available\n' $((need/1024/1024)) $((ESP_BYTES/1024/1024))

echo "Building FAT32 ESP ($((ESP_BYTES/1024/1024)) MB)..."
truncate -s "$ESP_BYTES" "$ESP_IMG"
mkfs.vfat -F32 -n BPI-ESP "$ESP_IMG" >/dev/null
mmd   -i "$ESP_IMG" ::/EFI ::/EFI/BOOT
mcopy -i "$ESP_IMG" "$GRUBDIR/bootaa64.efi" ::/EFI/BOOT/BOOTAA64.EFI
# EDK2's UEFI Shell, for the GRUB "UEFI Shell" entry
SHELL_EFI=$ROOT/edk2/Build/BpiR4/DEBUG_GCC/AARCH64/Shell.efi
if [ -f "$SHELL_EFI" ]; then
    mmd   -i "$ESP_IMG" ::/EFI/tools
    mcopy -i "$ESP_IMG" "$SHELL_EFI" ::/EFI/tools/Shell.efi
fi
# rewrite grub.cfg's kver to match the kernel we are actually staging, so the
# two can never silently drift apart into an unbootable image
sed "s|^set kver=.*|set kver=${KVER}|" "$GRUBDIR/grub.cfg" > "$WORKCFG"
mcopy -i "$ESP_IMG" "$WORKCFG"              ::/EFI/BOOT/grub.cfg
# --- VyOS: unpacked live tree (primary boot path) ---
if [ -d "$ISODIR/live" ]; then
    echo "Staging unpacked VyOS live tree from $ISODIR/live ..."
    mmd -i "$ESP_IMG" ::/live
    for f in vmlinuz-$KVER initrd.img-$KVER filesystem.squashfs; do
        [ -f "$ISODIR/live/$f" ] || { echo "ERROR: missing $ISODIR/live/$f" >&2; exit 1; }
        printf '  %s (%s)\n' "$f" "$(du -h "$ISODIR/live/$f" | cut -f1)"
        mcopy -i "$ESP_IMG" "$ISODIR/live/$f" "::/live/$f"
    done
else
    echo "WARNING: $ISODIR/live not found -- run ./unpack-iso.sh first" >&2
fi

# --- board device trees: EDK2 carries its own copies, these are for the
# GRUB installer/override entries (mmc0 only exists once an overlay is merged)
mmd -i "$ESP_IMG" ::/dtb
for m in sd emmc; do
    DTB=mt7988a-bananapi-bpi-r4-$m.dtb
    [ -f "$DTBDIR/$DTB" ] || { echo "ERROR: $DTBDIR/$DTB missing -- run ./mkdtb.sh first" >&2; exit 1; }
    echo "Staging device tree $DTB ..."
    mcopy -i "$ESP_IMG" "$DTBDIR/$DTB" "::/dtb/$DTB"
done

# --- VyOS: the ISO itself (loopback / findiso fallback path) ---
if [ -f "$VYOS_ISO" ]; then
    echo "Adding $(basename "$VYOS_ISO") to the ESP as /vyos.iso ..."
    mcopy -i "$ESP_IMG" "$VYOS_ISO" ::/vyos.iso
fi
dd if="$ESP_IMG" of="$IMG" bs=512 seek=$ESP_START conv=notrunc,fsync status=none

echo
echo "Done: $IMG"
sfdisk -l -o Device,Start,End,Size,Name,Attrs "$IMG"
echo
echo "Flash with:  sudo dd if=$IMG of=/dev/sdX bs=4M conv=fsync && sudo sfdisk --relocate gpt-bak-std /dev/sdX"
