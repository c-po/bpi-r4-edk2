#!/bin/bash
# Unpack the VyOS arm64 ISO so mksdimg.sh/mkdtb.sh can use its contents.
# Uses osirrox (xorriso) -- no root, no loop mount.
set -euo pipefail
ROOT=${ROOT:-/vyos/bpi-r4}
. "$ROOT/common.sh"
ISO=${1:-$(find_vyos_iso)}
OUT=${ISODIR:-/tmp/vyos-iso}
safe_rmdir "$OUT"; mkdir -p "$OUT"
osirrox -indev "$ISO" -extract / "$OUT" >/dev/null 2>&1
echo "Unpacked $(basename "$ISO") -> $OUT"
echo "Kernel: $(detect_kver "$OUT")"
ls -la "$OUT/live/"
