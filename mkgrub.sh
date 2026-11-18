#!/bin/bash
# Build a standalone arm64 GRUB EFI binary from Debian's grub-efi-arm64-bin.
# No arm64 chroot and no root needed -- we only unpack the .deb for its modules
# and drive the *host* grub-mkstandalone with -d pointing at them.
#
# The host grub-mkstandalone version must match the arm64 module version.
set -euo pipefail

DEB_VER=${DEB_VER:-2.06-13+deb12u2}
DEB=grub-efi-arm64-bin_${DEB_VER}_arm64.deb
URL=https://deb.debian.org/debian/pool/main/g/grub2/$DEB
OUT=${OUT:-/vyos/bpi-r4/grub}
WORK=${WORK:-/tmp/grub-arm64}

host_ver=$(grub-mkstandalone --version | grep -oE '[0-9]+\.[0-9]+' | head -1)
case "$DEB_VER" in
    "$host_ver"*) ;;
    *) echo "WARNING: host grub is $host_ver, arm64 modules are $DEB_VER" >&2 ;;
esac

mkdir -p "$WORK"; cd "$WORK"
[ -f "$DEB" ] || { echo "Fetching $DEB..."; curl -sSfLO "$URL"; }
dpkg-deb -x "$DEB" .
[ -d usr/lib/grub/arm64-efi ] || { echo "ERROR: arm64-efi modules not found" >&2; exit 1; }

cp "$OUT/grub-embedded.cfg" .

grub-mkstandalone -O arm64-efi \
  -d ./usr/lib/grub/arm64-efi \
  -o "$OUT/bootaa64.efi" \
  --modules="part_gpt part_msdos fat ext2 iso9660 loopback normal linux search \
             search_fs_file search_fs_uuid search_label serial terminal echo test \
             configfile all_video efi_gop efinet halt reboot minicmd ls cat help \
             gzio xzio sleep read regexp chain memrw" \
  "boot/grub/grub.cfg=./grub-embedded.cfg"

file "$OUT/bootaa64.efi" | grep -q "Aarch64" \
    || { echo "ERROR: output is not an Aarch64 EFI binary" >&2; exit 1; }
echo "Built $OUT/bootaa64.efi ($(stat -c%s "$OUT/bootaa64.efi") bytes)"
