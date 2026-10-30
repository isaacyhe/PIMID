# Building and Running

## Prerequisites (Ubuntu/Debian)

```bash
sudo apt install build-essential cmake libboost-all-dev libyaml-cpp-dev \
    libhdf5-dev libconfig-dev libconfig++-dev \
    ninja-build libglib2.0-dev libpixman-1-dev python3-venv
```

## Custom QEMU (required for exec/trace-gen)

PIMID needs QEMU with TCG plugin support; the stock `qemu-user` package does
not include it. The build expects it at `external/qemu/build/qemu-x86_64`:

```bash
cd external/qemu
git clone --depth 1 --branch v11.1.2 https://gitlab.com/qemu-project/qemu.git .
git apply ../qemu-patches/0001-i386-one-rep-iteration-per-block-with-plugins.patch
git apply ../qemu-patches/0002-x86_64-linux-user-reserved-va-without-vsyscall-page.patch
git apply ../qemu-patches/0003-linux-user-fixed-width-guest-process-and-thread-ids.patch
git apply ../qemu-patches/0004-linux-user-fixed-step-guest-time-stamp-counter.patch
git apply ../qemu-patches/0005-linux-user-seeded-getrandom.patch
git apply ../qemu-patches/0006-linux-user-fixed-guest-stat-block-size.patch
git apply ../qemu-patches/0007-linux-user-guest-stdio-never-a-terminal.patch
git apply ../qemu-patches/0008-linux-user-fixed-guest-identity.patch
mkdir build && cd build
../configure --target-list=x86_64-linux-user --enable-plugins --enable-tcg \
             --disable-docs --disable-werror
ninja -j$(nproc) qemu-x86_64
```

> **QEMU 11.1.2 is required (since PIMID 1.12.16).** The ZSim and trace plugins are built
> against its plugin API (version 7; QEMU 9.2 refuses them), and from 10.2.0 QEMU flushes its
> translations *before* entering parallel mode at a guest's first thread. 9.2 queued that flush
> as exclusive work, which deadlocked under ZSim when a program had more threads than simulated
> cores. Its plugin header includes `<glib.h>`, so building PIMID's plugins needs the glib
> development headers (found through `pkg-config glib-2.0`), which building QEMU needs anyway.
>
> **All eight patches in `external/qemu-patches/` are required.** 0001: QEMU 10 and later run a `REP`
> string instruction (`memset`, `memcpy`) for up to 65535 iterations inside one block execution;
> ZSim ends a phase only at a block boundary, so a long one piled its timing events into one phase
> and overflowed ZSim's global heap. With a plugin loaded the patch runs one iteration per block
> execution, as QEMU 9.2 did. Both plugins stop with a message naming the patch if it is missing.
> 0002 (since PIMID (1.12.62)): PIMID launches the guest with `-B 0 -R 0x8000000000`, a guest
> address space reserved by QEMU, so that the guest's layout does not depend on the host process
> (see [Reproducibility](architecture.md#reproducibility)). Stock QEMU refuses `-R` for an x86_64
> guest -- the run stops with `qemu-x86_64: Cannot allocate vsyscall page` -- because the legacy
> vsyscall page lies outside any reservation; the patch runs the guest without that page under
> `-R`, as on a host booted with `vsyscall=none` (glibc reaches the time calls through the vDSO).
> 0003 (since PIMID (1.12.124), second version since (1.12.157)): PIMID launches the guest with
> `-tid-base 10000000`: every process and thread ID the guest sees is 10000000 + 4194304 + its order of
> first sight (eight digits, independent of the host's process numbering), and every ID it passes back
> is translated back (see [Reproducibility](architecture.md#reproducibility)). A QEMU without the patch
> stops at once with `qemu: unknown option 'tid-base'`. 0004 (since (1.12.157)): PIMID launches the guest
> with `-tsc-step 1001`: the guest's time stamp counter advances by 1001 on every read instead of
> following the host's; a QEMU without it stops at once with `qemu: unknown option 'tsc-step'`. 0005
> (since (1.12.157)): with `-seed` the guest's `getrandom()` is answered from QEMU's seeded generator, as
> `AT_RANDOM` already was (without it the host answered; a QEMU without 0005 runs, but the guest's heap
> contents then differ between runs). 0006: PIMID launches the guest with `-st-blksize 4096`: the guest's `stat` calls report a 4096-byte block size instead of the host file system's, so the guest's output buffers no longer follow the file system the run's log is written to; a QEMU without it stops at once with `qemu: unknown option 'st-blksize'`. 0007: PIMID launches the guest with `-stdio-notty`: the guest's standard input, output and error are never a terminal (terminal ioctls on them fail with `ENOTTY`, and `fstat` reports a pipe owned by the process's own user and group, with device, inode and times 0), so a run started at a terminal does the same work, and leaves the same data in the guest's memory, as one whose output goes to a file or a pipe; a QEMU without it stops at once with `qemu: unknown option 'stdio-notty'`. 0008: PIMID launches the guest with `-guest-ids 1000:1000 -rlimit-stack 8388608 -0 /pimid-guest/<program> -execfn /pimid-guest/<program>`: the guest sees one fixed identity (user and group 1000, one group; the files of the user running PIMID owned by 1000:1000 with permissions 0755 or 0644; `uname` node `pimid`, release 6.8.0), an 8 MiB stack limit and its program as `/pimid-guest/<file name>`, whoever runs PIMID, wherever it is installed and whatever `ulimit -s` says; a QEMU without it stops at once with `qemu: unknown option 'guest-ids'`. PIMID's `qemu-x86_64`, built from QEMU 11.1.2 with the eight patches as above, has md5 `a7b17fbc4dff095e18f1d8c6d6f74c01`.

## Guest libraries (since (1.12.135))

The guest program runs under QEMU user mode, which by default gives it the HOST's own C library, C++
library and OpenMP runtime. Their code runs inside the simulated region, so a host with another
distribution gives other results for the same program (measured: glibc 2.43 instead of 2.39 moved
serial cells by up to 0.25% and small OpenMP cells by tens of percent). PIMID therefore runs every
guest on one pinned copy of these libraries, the Ubuntu 24.04 packages
`libc6 2.39-0ubuntu8.9`, `libstdc++6`, `libgcc-s1` and `libgomp1 14.2.0-4ubuntu2~24.04.1`:

```bash
scripts/fetch_guest_sysroot.sh        # once; CMake runs it at configure time (PIMID_FETCH_GUEST_SYSROOT, default ON)
```

The script downloads the four packages from Launchpad, checks their pinned sha256 sums, and unpacks
them into `guest-sysroot/noble/` (about 13 MB; not tracked by git). At every launch PIMID checks the
libraries a guest loads (`ld.so`, `libc`, `libm`, `libmvec`, `libgcc_s`, `libgomp`, `libstdc++`)
against sizes and hashes compiled into it and passes the directory to QEMU with `-L`: QEMU then
opens a file the guest asks for from this directory when it is there and from the host otherwise,
so the guest sees its usual paths (`/lib/x86_64-linux-gnu/libc.so.6`) and libraries the sysroot
does not hold (a workload's own) still come from the host. The `[guest] launch:` line names the
guest libraries in use.

- No network on the build machine: download the four `.deb` files elsewhere and run the script with
  `PIMID_GUEST_DEBS=<dir>`.
- A missing sysroot: the run continues on the host's libraries, with a warning.
- A modified sysroot: the run stops (`[guest] FATAL: ... is not PIMID's pinned copy`).
- `PIMID_GUEST_SYSROOT=<dir>` names another copy (it must pass the same check);
  `PIMID_GUEST_SYSROOT=host` runs on the host's libraries.

## Guest MPI shim

`libpimid_mpi.so`, PIMID's message-passing library, runs inside the guest (the workloads load it in place of
MPICH), so its machine code is part of the program being simulated: compiled by another compiler it gives other
message-passing results (measured: GCC 13.4 instead of 13.3 moved the thread-MPI and co-simulation MPI cells compared).
The tree therefore ships one prebuilt copy, `guest-shim/libpimid_mpi.so`, built with Ubuntu 24.04's g++
`13.3.0-6ubuntu2~24.04.1` and the build's own flags, and the build installs it next to `pimid` instead of
compiling it. Before the first launch PIMID checks the shim the guest will load against
`guest-shim/libpimid_mpi.so.sum` (its size and FNV-1a 64 hash; the file also records its md5, the PIMID version it
carries and the compiler); the `[mpi] guest shim` line reports it.

```bash
scripts/build_guest_shim.sh           # Ubuntu 24.04, g++ 13.3.0: rebuilds guest-shim/libpimid_mpi.so and its .sum
scripts/build_guest_shim.sh --check   # rebuilds in a scratch directory and compares with the tracked copy
```

- The shim carries the PIMID version (its component stamp), so it is rebuilt whenever the version or
  `src/parallel_abstraction/pimid_mpi.cpp` changes; the script refuses any other compiler.
- A different shim: the run stops (`[mpi] FATAL: the guest's MPI shim ... is not PIMID's prebuilt copy`);
  `PIMID_GUEST_SHIM=local` runs it with a warning.
- `-DPIMID_BUILD_GUEST_SHIM=ON` compiles the shim from source instead (development; run with
  `PIMID_GUEST_SHIM=local`).

## Build PIMID

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
make -C ../benchmarks all     # build the benchmark suite
```

All bundled models (Ramulator2, CACTI, NVSim, McPAT, Garnet) build
automatically. HDF5 is located via CMake's find_package, so distros shipping
only `libhdf5_serial` work out of the box.

## Run

```bash
./build/pimid --method exec --config examples/tech_DDR4.yaml --no-power
```

Config workload paths resolve relative to the current directory first, then
relative to the PIMID root (the executable's tree or `$PIMID_ROOT`), so the
shipped examples work from any directory.

## Building without root (HPC / cloud)

- User-local tools: `python3 -m pip install --user meson ninja tomli`;
  configure with `-DCMAKE_PREFIX_PATH=$HOME/.local`; run with
  `LD_LIBRARY_PATH=$HOME/.local/lib64:$LD_LIBRARY_PATH`.
- **yaml-cpp is vendored** (via Ramulator's FetchContent). Do NOT install a
  system/local yaml-cpp -- it creates a duplicate target. If
  `find_package(yaml-cpp)` misses, add `-DCMAKE_CXX_FLAGS="-DHAVE_YAML_CPP"`.
- Missing `libconfig` headers: build it from source into `~/.local`.
- Use a fresh build directory whenever a dependency is added (CMake caches
  negative find results).

## Docker

```bash
docker pull ghcr.io/isaacyhe/pimid:latest                    # also tagged by version, e.g. :1.12.10
docker run --rm -it ghcr.io/isaacyhe/pimid:latest            # shell at /opt/pimid
docker run --rm ghcr.io/isaacyhe/pimid:latest \
    pimid --method exec --config examples/tech_DDR4.yaml --no-power
docker run --rm ghcr.io/isaacyhe/pimid:latest pimid_smoke    # full feature sweep
docker run --rm ghcr.io/isaacyhe/pimid:latest pimid-cache list              # the characterization cache the image carries
docker run --rm -v "$PWD:/work" ghcr.io/isaacyhe/pimid:latest \
    pimid-migrate-config --check /work/my_config.yaml                      # is a pre-1.12.8 file in the current shape?
```

The image ships the prebuilt simulator, QEMU, the plugins, the examples (every
hardware block under `system:`, 1.12.8), the benchmark workloads, the
characterization cache warehouse with the tree's NVM entries, and the two tools
`pimid-cache` and `pimid-migrate-config` (1.12.11). The sweep runs the
addressed co-simulation example (`cosim/host_memory_device_addressed.yaml`, the
address map of 1.12.12) and checks that a multi-node configuration (a second
host; a device reaching beyond its own window) is refused (1.12.13). A
co-simulation needs no
`--scope` flag (retired in 1.12.9): the nodes in the file decide. The image is
built from the dev repository root as the context (`-f docker/Dockerfile .`),
with the tree built and `PIMID_SRC_ROOT` naming the tree the binaries came from.

## Reproducibility (ASLR)

Since (1.12.62) the guest's addresses do not depend on the host's address-space
layout: the launch reserves the guest address space (see
[Reproducibility](architecture.md#reproducibility)). Measured on 116 cells,
three jobs with ASLR on against three with it off: every cycle count, access
count and power figure of the 107 serial cells was the same. ASLR still
reaches one report line on the host side: the McPAT clock-feasibility line
("N array(s) sized by CACTI for this clock") listed 4 instead of 7 arrays for
the OOO host core in one of the three ASLR runs, with the same power. For
bit-stable logs keep ASLR off with `setarch`:

```bash
setarch -R ./build/pimid --method exec --config <cfg>
# Docker: the default seccomp profile blocks personality(); add
docker run --security-opt seccomp=unconfined ... setarch -R pimid ...
```
