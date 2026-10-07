# External Dependencies

`external/` contains all external simulators and modeling tools integrated
into PIMID.

## Included Components

### 1. QEMU (Execution Engine)
- **Repository**: https://gitlab.com/qemu-project/qemu
- **Purpose**: User-mode emulation that drives execution-based simulation; PIMID's
  zsim-derived timing model attaches as a QEMU TCG plugin (`libzsim_qemu.so`)
- **Location**: `qemu/`
- **Build**: see [build.md](build.md) (a system `qemu-x86_64` with plugin
  support also works)

### 2. ZSim (Timing Model)
- **Repository**: https://github.com/s5z/zsim (heavily modified)
- **Purpose**: Core + cache-hierarchy timing for workload execution
  (in-order / out-of-order / ALU / null core models, see [cores.md](cores.md))
- **Location**: `zsim/`
- **Build**: built automatically with PIMID via CMake as QEMU TCG plugins
  (`libzsim_qemu.so`, `libpimid_trace.so`); the legacy native Intel-PIN build
  (`scons`) is still present but optional

### 3. Ramulator2 (DRAM Simulator)
- **Repository**: https://github.com/CMU-SAFARI/ramulator2
- **Purpose**: Cycle-accurate DRAM timing simulation
- **Technologies**: DDR3, DDR4, DDR5, LPDDR5, GDDR6, HBM2, HBM3
- **Location**: `ramulator/`
- **Build**: Integrated via CMake (auto-built with PIMID)

### 4. CACTI 7.0 (SRAM/Cache Characterization)
- **Repository**: https://github.com/HewlettPackard/cacti
- **Purpose**: Cache and memory access time, area, and power modeling
- **Technologies**: SRAM, cache hierarchies, subarray characterization
- **Location**: `cacti/`
- **Build**: Integrated via CMake (auto-built with PIMID)

### 5. NVSim (Non-Volatile Memory Simulator)
- **Repository**: https://github.com/SEAL-UCSB/NVSim
- **Purpose**: Performance, energy, and area estimation for NVM
- **Technologies**: STT-MRAM, PCM, ReRAM
- **Location**: `nvsim/`
- **Note**: **Namespaced** (`nvsim::`) to avoid symbol collision with CACTI
- **Build**: Integrated via CMake (auto-built with PIMID)

### 6. McPAT (Power Modeling)
- **Repository**: https://github.com/HewlettPackard/mcpat
- **Purpose**: Integrated power, area, and timing modeling
- **Components**: Cores, caches, NoCs, memory controllers
- **Location**: `mcpat/`
- **Note**: Uses **bundled CACTI 6.5-P** (API compatibility with McPAT internals)
- **Build**: Integrated via CMake (auto-built with PIMID)

### 7. Garnet (Network-on-Chip Simulator)
- **Origin**: Extracted from gem5
- **Purpose**: Detailed NoC simulation with virtual channels, routing, flow control
- **Topologies**: see [network.md](network.md)
- **Location**: `garnet/`
- **Note**: **Standalone** - no gem5 dependency, includes gem5 compatibility headers
- **Build**: Integrated via CMake (auto-built with PIMID)

### 8. Intel PIN (Legacy, Optional)
- **Purpose**: Dynamic binary instrumentation for the legacy native ZSim build
- **Location**: `pin/` (symlink to a local installation; not shipped)
- **Required for**: the legacy `scons` ZSim build only
- **Not required for**: normal PIMID use - execution runs through QEMU plugins

## PIMID Integration Points

### Memory Models (`src/memory/`, `include/memory/`)
- **DRAM**: `ramulator_wrapper.cpp` -> `ramulator/`
- **SRAM**: `cacti_wrapper.cpp` -> `cacti/`
- **NVM**: `nvsim_wrapper.cpp` -> `nvsim/`

### Network Models (`src/network/`, `include/network/`)
- **Garnet NoC**: `garnet_wrapper.cpp` -> `garnet/`

### Power Models (`src/power/`, `include/power/`)
- **McPAT**: `mcpat_wrapper.cpp` -> `mcpat/`

## Building

Everything is built automatically with PIMID:

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

See [build.md](build.md) for prerequisites, the custom QEMU build, and
HPC/no-root notes.

## CACTI Version Notes

PIMID uses **two versions of CACTI**:

1. **CACTI 7.0** (`external/cacti/`): Used by PIMID's `cacti_wrapper` for standalone SRAM/subarray characterization. Modern API with improved accuracy.

2. **CACTI 6.5-P** (`external/mcpat/cacti/`): Bundled with McPAT. Required for McPAT's internal power calculations due to API dependencies (`TechnologyParameter::DeviceType`, etc.).

This separation avoids API conflicts while leveraging the strengths of both versions.

## Licensing

| Component | License |
|-----------|---------|
| QEMU | GPL-2.0 |
| ZSim | GPL-2.0 |
| Ramulator2 | MIT |
| CACTI | HP Labs |
| NVSim | Custom (see nvsim/README) |
| McPAT | HP Labs |
| Garnet | BSD (gem5) |

PIMID itself is GPL-2.0.
