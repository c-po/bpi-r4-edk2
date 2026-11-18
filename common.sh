# Shared helpers for the BPI-R4 image scripts. Source, don't execute.
ROOT=${ROOT:-/vyos/bpi-r4}

# Newest vyos-*arm64.iso in $ROOT, unless VYOS_ISO overrides it. The ISO name
# changes with every build, so nothing hardcodes it.
find_vyos_iso() {
    if [ -n "${VYOS_ISO:-}" ]; then
        [ -f "$VYOS_ISO" ] || { echo "ERROR: VYOS_ISO=$VYOS_ISO not found" >&2; return 1; }
        echo "$VYOS_ISO"; return 0
    fi
    local iso
    iso=$(ls -1t "$ROOT"/vyos-*arm64.iso 2>/dev/null | head -1)
    [ -n "$iso" ] || { echo "ERROR: no vyos-*arm64.iso in $ROOT" >&2; return 1; }
    echo "$iso"
}

# Kernel version from the unpacked ISO, e.g. 6.18.50-vyos. Never hardcode it:
# a mismatch between grub.cfg and the staged files is a silent unbootable image.
detect_kver() {
    local isodir=$1 k
    k=$(ls -1 "$isodir"/live/vmlinuz-* 2>/dev/null | head -1)
    [ -n "$k" ] || { echo "ERROR: no vmlinuz-* in $isodir/live" >&2; return 1; }
    basename "$k" | sed 's/^vmlinuz-//'
}

# osirrox restores the ISO's read-only permissions, so a plain rm -rf of a
# previous extraction fails. Make it writable first.
safe_rmdir() { [ -d "$1" ] && { chmod -R u+w "$1" 2>/dev/null || true; rm -rf "$1"; }; return 0; }
