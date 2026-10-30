#!/usr/bin/env bash
# build_guest_shim.sh -- builds PIMID's prebuilt guest MPI shim, guest-shim/libpimid_mpi.so.
#
# libpimid_mpi.so (src/parallel_abstraction/pimid_mpi.cpp) runs inside the
# guest, in the measured region, so its machine code is part of the program
# being simulated: compiled by another compiler it gives other message-passing
# results. PIMID therefore ships one copy, built here with Ubuntu 24.04's
# g++ 13.3.0 and the build's own flags, and checks it at every launch against
# guest-shim/libpimid_mpi.so.sum (see docs/build.md, "Guest MPI shim"). The
# shim carries the tree's version (its component stamp), so it is rebuilt
# whenever the version or pimid_mpi.cpp / pimid_mpi.h changes.
#
# Usage: scripts/build_guest_shim.sh           write guest-shim/libpimid_mpi.so and its .sum
#        scripts/build_guest_shim.sh --check   rebuild in a scratch directory and compare with the tracked copy
# Needs: Ubuntu 24.04's g++ 13.3.0 (CXX, default c++), python3, md5sum.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
REF="13.3.0-6ubuntu2~24.04.1"
CXX="${CXX:-c++}"
CHECK=0; [ "${1:-}" = "--check" ] && CHECK=1

ver_line="$("$CXX" --version | head -1)"
case "$ver_line" in
  *"$REF"*) ;;
  *) echo "build_guest_shim: needs Ubuntu 24.04's g++ $REF; $CXX is: $ver_line" >&2; exit 2;;
esac
VER="$(sed -nE 's/^project\(PIMID VERSION ([0-9.]+).*/\1/p' "$ROOT/CMakeLists.txt")"
[ -n "$VER" ] || { echo "build_guest_shim: no project VERSION in $ROOT/CMakeLists.txt" >&2; exit 2; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cd "$ROOT"
# the flags of the CMake target (CMAKE_CXX_FLAGS_RELEASE, -std=c++17, -fPIC and the target's own options)
"$CXX" -DPIMID_VERSION="\"$VER\"" -Dpimid_mpi_EXPORTS -Iinclude -Isrc \
    -Wall -Wextra -O3 -DNDEBUG -std=c++17 -fPIC -fvisibility=default -Wall -Wextra -O2 -Wno-unused-parameter \
    -c src/parallel_abstraction/pimid_mpi.cpp -o "$TMP/pimid_mpi.o"
"$CXX" -fPIC -Wall -Wextra -O3 -DNDEBUG -shared -Wl,-soname,libpimid_mpi.so \
    -o "$TMP/libpimid_mpi.so" "$TMP/pimid_mpi.o" -ldl

if [ $CHECK = 1 ]; then
  if cmp -s "$TMP/libpimid_mpi.so" "$ROOT/guest-shim/libpimid_mpi.so"; then
    echo "build_guest_shim: guest-shim/libpimid_mpi.so rebuilds identically (version $VER)"; exit 0
  fi
  echo "build_guest_shim: guest-shim/libpimid_mpi.so differs from a rebuild of version $VER; run scripts/build_guest_shim.sh" >&2
  exit 1
fi

mkdir -p "$ROOT/guest-shim"
cp "$TMP/libpimid_mpi.so" "$ROOT/guest-shim/libpimid_mpi.so"
chmod 0755 "$ROOT/guest-shim/libpimid_mpi.so"
size=$(stat -c %s "$ROOT/guest-shim/libpimid_mpi.so")
fnv=$(python3 -I -c 'import sys
h=0xcbf29ce484222325
for b in open(sys.argv[1],"rb").read():
    h=((h^b)*0x100000001b3)&0xffffffffffffffff
print("0x%016x"%h)' "$ROOT/guest-shim/libpimid_mpi.so")
md5=$(md5sum "$ROOT/guest-shim/libpimid_mpi.so" | cut -d' ' -f1)
echo "libpimid_mpi.so size=$size fnv1a64=$fnv md5=$md5 version=$VER compiler=g++-$REF" > "$ROOT/guest-shim/libpimid_mpi.so.sum"
cat "$ROOT/guest-shim/libpimid_mpi.so.sum"
