#!/bin/bash
# Build the BPI-R4 UEFI firmware (Tianocore EDK2) and wrap it in an ATF FIP:
#   ATF BL2 (DRAM init, DDR4_4BG_MODE) + FIP(BL31 + EDK2 as BL33)
#
# EDK2 replaces only U-Boot. The ATF build goes to its own BUILD_BASE so the
# U-Boot FIP in build/mt7988/release stays untouched.
#
# One binary serves 4 GB and 8 GB boards: EDK2 probes the DRAM size at boot.
#
# Outputs:
#   $OUT/BPI_R4_EFI.fd                                    the EDK2 BL33 payload
#   arm-trusted-firmware-mtk/build-edk2/mt7988/release/{bl2.img,fip.bin}       SD
#   arm-trusted-firmware-mtk/build-edk2-emmc/mt7988/release/{bl2.img,fip.bin}  eMMC
#   arm-trusted-firmware-mtk/build-edk2-nand/mt7988/release/{bl2.img,fip.bin}  SPI-NAND
set -euo pipefail

ROOT=${ROOT:-/vyos/bpi-r4}
EDK2DIR=${EDK2DIR:-$ROOT/edk2}
ATFDIR=${ATFDIR:-$ROOT/arm-trusted-firmware-mtk}
DTBDIR=${DTBDIR:-$ROOT/dtb}
TARGET=${TARGET:-DEBUG}          # DEBUG keeps error messages and ASSERTs on the UART
VERBOSE=${VERBOSE:-0}            # 1: full DXE/BDS/SD bring-up log
OUT=${OUT:-$EDK2DIR/out}
export CROSS_COMPILE=${CROSS_COMPILE:-aarch64-linux-gnu-}

PLATDIR=$EDK2DIR/bpi-r4-platform/Platform/BananaPi/BpiR4
FIP_MAX=$((2 * 1024 * 1024))     # size of the "fip" partition, see mksdimg.sh

command -v "${CROSS_COMPILE}gcc" >/dev/null \
    || { echo "ERROR: ${CROSS_COMPILE}gcc not found (apt install gcc-aarch64-linux-gnu)" >&2; exit 1; }
MKIMAGE=${MKIMAGE:-$(command -v mkimage)} \
    || { echo "ERROR: mkimage not found (apt install u-boot-tools)" >&2; exit 1; }

# --- EDK2 -------------------------------------------------------------------
# Both DTBs are embedded; EDK2 installs the one matching the boot medium,
# with the EEPROM MAC addresses added. GRUB's "devicetree" would replace it.
# mkdtb.sh produces them from the ISO's kernel.
for m in sd emmc; do
    dtb=$DTBDIR/mt7988a-bananapi-bpi-r4-$m.dtb
    [ -f "$dtb" ] || { echo "ERROR: $dtb missing -- run ./mkdtb.sh first" >&2; exit 1; }
    cp "$dtb" "$PLATDIR/BpiR4-$m.dtb"
done

# Local EDK2 core patches (MediaTek xHCI), applied once to the checkout
for p in "$ROOT"/patches/edk2-*.patch; do
    [ -f "$p" ] || continue
    if git -C "$EDK2DIR/edk2" apply --reverse --check "$p" 2>/dev/null; then
        continue
    fi
    git -C "$EDK2DIR/edk2" apply "$p" || { echo "ERROR: $p does not apply" >&2; exit 1; }
    echo "Applied $(basename "$p")"
done

export WORKSPACE=$EDK2DIR
export PACKAGES_PATH=$EDK2DIR/edk2:$EDK2DIR/edk2-platforms:$EDK2DIR/bpi-r4-platform
export GCC_AARCH64_PREFIX=$CROSS_COMPILE
[ -x "$EDK2DIR/edk2/BaseTools/Source/C/bin/GenFw" ] || make -C "$EDK2DIR/edk2/BaseTools" -j"$(nproc)"

(
    cd "$EDK2DIR"
    set +u                       # edksetup.sh is not nounset-clean
    . edk2/edksetup.sh >/dev/null
    build -a AARCH64 -t GCC -b "$TARGET" -n "$(nproc)" \
          -p Platform/BananaPi/BpiR4/BpiR4.dsc \
          $([ "$VERBOSE" = 1 ] && echo "-D VERBOSE=TRUE")
)

FD=$EDK2DIR/Build/BpiR4/${TARGET}_GCC/FV/BPI_R4_EFI.fd
[ -f "$FD" ] || { echo "ERROR: build produced no $FD" >&2; exit 1; }
mkdir -p "$OUT"
cp "$FD" "$OUT/BPI_R4_EFI.fd"

# --- ATF --------------------------------------------------------------------
# One BL2 per boot medium (the BootROM header and boot device driver differ),
# each with its own BUILD_BASE. Every FIP carries the same EDK2 FD.
# SPI-NAND matches the OpenWrt layout: BL2 in mtd "bl2", FIP in the UBI volume
# "fip" (UBI from 0x200000). spim-nand leaves UBI off unless asked.
# DDR4_4BG_MODE is not in mt7988's MAKE_DEP, so incremental builds ignore it:
# always start from a clean BUILD_BASE.
build_atf() {  # boot_device, build_base, extra make args...
    local dev=$1 base=$2
    shift 2
    rm -rf "${ATFDIR:?}/$base"
    make -C "$ATFDIR" PLAT=mt7988 BOOT_DEVICE="$dev" \
         DRAM_USE_COMB=1 DDR4_4BG_MODE=1 \
         USE_MKIMAGE=1 MKIMAGE="$MKIMAGE" \
         BL33="$OUT/BPI_R4_EFI.fd" BUILD_BASE="$base" "$@" \
         all fip -j"$(nproc)"
    set -- "$dev" "$base"

    local rel=$ATFDIR/$2/mt7988/release sz
    # not grep -q: it exits early, strings dies of SIGPIPE, and pipefail trips
    strings "$rel/bl2.bin" | grep -x "EMI: DDR4 4BG mode" >/dev/null \
        || { echo "ERROR: $1 bl2 was built without DDR4_4BG_MODE" >&2; exit 1; }

    sz=$(stat -c%s "$rel/fip.bin")
    [ "$sz" -le "$FIP_MAX" ] \
        || { echo "ERROR: fip.bin is $sz bytes, exceeds its $FIP_MAX-byte partition" >&2; exit 1; }
}
build_atf sdmmc     build-edk2
build_atf emmc      build-edk2-emmc
build_atf spim-nand build-edk2-nand UBI=1

echo
echo "EDK2 ($TARGET): $OUT/BPI_R4_EFI.fd ($(stat -c%s "$OUT/BPI_R4_EFI.fd") bytes)"
for b in build-edk2 build-edk2-emmc build-edk2-nand; do
    rel=$ATFDIR/$b/mt7988/release
    echo "$b:"
    echo "  BL2: $rel/bl2.img ($(stat -c%s "$rel/bl2.img") bytes)"
    echo "  FIP: $rel/fip.bin ($(stat -c%s "$rel/fip.bin") bytes of $FIP_MAX)"
done
