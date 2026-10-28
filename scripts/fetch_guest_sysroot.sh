#!/usr/bin/env bash
# fetch_guest_sysroot.sh -- builds PIMID's pinned guest sysroot.
#
# The guest program runs under QEMU user mode, which by default hands it the
# HOST's own C library, C++ library and OpenMP runtime (/lib/x86_64-linux-gnu).
# Their code runs inside the simulated region, so a host with another
# distribution (another glibc, another libgomp) gave other results. PIMID
# therefore runs every guest on one fixed set of these libraries: the Ubuntu
# 24.04 (noble) packages below, unpacked into <pimid>/guest-sysroot/noble and
# given to QEMU with -L (see docs/build.md, "Guest libraries").
#
# Usage: scripts/fetch_guest_sysroot.sh [DEST]   (default: <pimid>/guest-sysroot/noble)
#        PIMID_GUEST_DEBS=<dir> uses .deb files already downloaded there.
# Needs: curl (or wget), sha256sum, and dpkg-deb or (ar + tar with zstd/xz).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="${1:-$HERE/../guest-sysroot/noble}"
URL="https://launchpad.net/ubuntu/+archive/primary/+files"

# name  sha256 (the Ubuntu archive's packages, unchanged)
PKGS=(
  "libc6_2.39-0ubuntu8.9_amd64.deb ff5557d99b51f761c4b7c92368b9cc45565eda17df9bf9eb4b134d09825008be"
  "libgcc-s1_14.2.0-4ubuntu2~24.04.1_amd64.deb aa7fadbe33b78bcf99885318040601c550c208929565b179891d9a3cc2aa68cd"
  "libgomp1_14.2.0-4ubuntu2~24.04.1_amd64.deb e8a95ec58125b4933597f30ff56c2ae10edf90f287262e366d4b6edea3019144"
  "libstdc++6_14.2.0-4ubuntu2~24.04.1_amd64.deb a51f8de7829211db961a31f02158058ad1a95f92ac6d0a5dff6350e2821c54c0"
)

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/root"

fetch() {   # $1 = file name
    if [ -n "${PIMID_GUEST_DEBS:-}" ] && [ -f "$PIMID_GUEST_DEBS/$1" ]; then
        cp "$PIMID_GUEST_DEBS/$1" "$TMP/$1"; return
    fi
    local enc="${1//~/%7E}"
    if command -v curl >/dev/null; then curl -fsSL -o "$TMP/$1" "$URL/$enc"
    else wget -q -O "$TMP/$1" "$URL/$enc"; fi
}

unpack() {  # $1 = .deb
    if command -v dpkg-deb >/dev/null; then dpkg-deb -x "$1" "$TMP/root"; return; fi
    local d="$TMP/ar.$$"; mkdir -p "$d"; (cd "$d" && ar x "$1")
    case "$(ls "$d"/data.tar.*)" in
        *.zst) tar --zstd -xf "$d"/data.tar.zst -C "$TMP/root" ;;
        *.xz)  tar -xJf "$d"/data.tar.xz -C "$TMP/root" ;;
        *)     tar -xf "$d"/data.tar.* -C "$TMP/root" ;;
    esac
    rm -rf "$d"
}

for p in "${PKGS[@]}"; do
    set -- $p
    fetch "$1"
    got="$(sha256sum "$TMP/$1" | cut -d' ' -f1)"
    if [ "$got" != "$2" ]; then
        echo "fetch_guest_sysroot: $1: sha256 $got, expected $2 -- refusing" >&2; exit 1
    fi
    unpack "$TMP/$1"
done
# merged /usr, as on the distribution: the guest asks for /lib/... and /lib64/...
ln -sfn usr/lib "$TMP/root/lib"
ln -sfn usr/lib64 "$TMP/root/lib64"
# the record PIMID checks before each launch (md5 of the libraries a guest loads)
( cd "$TMP/root" && md5sum usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 usr/lib/x86_64-linux-gnu/libc.so.6 \
    usr/lib/x86_64-linux-gnu/libm.so.6 usr/lib/x86_64-linux-gnu/libmvec.so.1 usr/lib/x86_64-linux-gnu/libgcc_s.so.1 \
    usr/lib/x86_64-linux-gnu/libgomp.so.1.0.0 usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33 ) > "$TMP/root/MANIFEST.md5"
mkdir -p "$(dirname "$DEST")"
rm -rf "$DEST.new"; mv "$TMP/root" "$DEST.new"; rm -rf "$DEST"; mv "$DEST.new" "$DEST"
echo "fetch_guest_sysroot: guest libraries ready in $DEST"
