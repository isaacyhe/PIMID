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
git clone --depth 1 --branch v9.2.0 https://gitlab.com/qemu-project/qemu.git .
mkdir build && cd build
../configure --target-list=x86_64-linux-user --enable-plugins --enable-tcg \
             --disable-docs --disable-werror
ninja -j$(nproc) qemu-x86_64
```

> **glibc >= 2.41:** glibc defines `struct sched_attr`, clashing with QEMU
> 9.2's copy in `linux-user/syscall.c` (~line 361). Wrap QEMU's definition in
> `#ifndef SCHED_ATTR_SIZE_VER0 ... #endif` and rebuild.

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
`pimid-cache` and `pimid-migrate-config` (1.12.11). A co-simulation needs no
`--scope` flag (retired in 1.12.9): the nodes in the file decide. The image is
built from the dev repository root as the context (`-f docker/Dockerfile .`),
with the tree built and `PIMID_SRC_ROOT` naming the tree the binaries came from.

## Reproducibility (ASLR)

Cycle counts vary slightly (~1%) run-to-run due to address-space layout
randomization; instruction and access counts are exact. For bit-stable timing
disable ASLR with `setarch`:

```bash
setarch -R ./build/pimid --method exec --config <cfg>
# Docker: the default seccomp profile blocks personality(); add
docker run --security-opt seccomp=unconfined ... setarch -R pimid ...
```
