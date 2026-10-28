# Memory architecture specification structs (C++ API)

`src/memory/` ships a set of header-only architecture specifications used by
PE placement, bandwidth tracking, and the memory models. They describe the
*internal* structure of each memory technology: hierarchy organization, port
bitwidths at every level, timing, and data-movement energy.

Not to be confused with [dram_specs.md](dram_specs.md): that page documents
the **Ramulator2 presets** that drive cycle-level DRAM timing. The structs
here capture the architectural parameters (especially internal port widths)
that Ramulator2 does not model.

## Why internal port bitwidths matter

Inside DRAM chips, banks and bank groups have **narrow internal ports**
(8-16 bits for DDR), not the wide external interfaces (64+ bits) seen at the
DIMM level:

```
DDR4/DDR5:  8-16 bit bank ports   (narrow) -> rank-level PIM optimal
HBM2/HBM3:  64-128 bit bank ports (wide)   -> bank-level PIM viable
```

This is the single biggest architectural factor for where PIM processing
elements should be placed. DDR's planar wire routing limits practical
internal ports to 8-16 bits; HBM's through-silicon vias (TSVs) enable wide
internal paths, which is why fine-grained (bank/subarray) PIM is viable on
HBM but not on commodity DDR.

## Files

| File | Purpose |
|------|---------|
| `dram_architecture_v2.h` | DRAM specs: `createDDR4_2400_Verified()`, `createDDR5_4800_Verified()`, `createHBM2_Verified()`, `createHBM3_Verified()` |
| `sram_architecture.h` | SRAM (cache) specs with inner-bank timing |
| `sttmram_architecture.h` | STT-MRAM specs with read/write asymmetry |
| `pcm_architecture.h` | PCM (phase-change memory) specs |
| `reram_architecture.h` | ReRAM/memristor specs with analog-compute support |

1.11.59 (C016-class): `dram_architecture.h` / `.cpp` -- the legacy v1
`DRAMArchitecture` object listed here until now -- were DELETED. They were
compiled but had no caller anywhere in the tree, and they carried HBM
access-time literals that had drifted 16-37% from their own declared sums
since release 1.1.1 raised HBM tRCD/tCAS to the JEDEC spec minima. Use the v2
object below; it derives those sums from tRCD/tCAS/tRP at construction.

The structs cover four explicit DRAM creators; the other simulator
technologies (DDR3, LPDDR5, GDDR6 -- see [memory.md](memory.md)) take their
cycle timing from Ramulator2 presets and their organization from the
architecture extractor.

## Quick start

```cpp
#include "memory/dram_architecture_v2.h"
using namespace pimid::memory;

auto ddr4 = createDDR4_2400_Verified();
ddr4->printVerificationReport();

std::cout << "Bank serialization: "
          << ddr4->datapath.bank_serialization_bits.value_bits << " bits\n";
std::cout << "Bank BW:            " << ddr4->getBankEffectiveBW() << " GB/s\n";

auto hbm2 = createHBM2_Verified();
std::cout << "HBM2 bank serialization: "
          << hbm2->datapath.bank_serialization_bits.value_bits << " bits\n";
```

## Structure

**Datapath (`DRAMDatapathStages`)** -- width of each stage between the cell
array and the package pins, every one carrying its own verification status
and citation (`VerifiedValue`): `row_buffer_bits`, `gsa_datapath_bits`,
`prefetch_datapath_bits`, `bank_serialization_bits` (the bank-level PIM
bottleneck, and the one stage nobody publishes), `chip_io_bits`,
`rank_databus_bits`, `channel_databus_bits`. Bank and bank-group bandwidth
are computed, not stored: `getBankEffectiveBW()`,
`getBankGroupEffectiveBW()`.

**Organization (`DRAMArchitectureV2::organization`)** -- physical hierarchy
and capacity at each level (subarrays per bank, banks per bank group, bank
groups per chip, chips per rank, per-level capacities).

**Timing (`DRAMArchitectureV2::timing`)** -- standard DRAM timing (tRCD,
tCAS, ...) plus hierarchical access latencies. `subarray_access_ns` and
`bank_access_ns` are computed from tRCD/tCAS/tRP by
`deriveHierarchicalAccessTimes()` rather than written as literals, which is
what the deleted v1 object got wrong. The header also breaks down the
inner-bank datapath hidden inside tCAS (column decoder, mux, output driver,
H-tree segments, global I/O) -- e.g. ~6.65 ns total for DDR4 vs ~3.05 ns for
HBM2, because TSVs shorten internal paths.

**Energy (`DRAMArchitectureV2::energy`)** -- data-movement energy per byte at
subarray / bank / chip / rank, with an `energy_source` citation string.

## Specification sources

Timing and organization values come from JEDEC standards (JESD79-4/-5,
JESD235A, JESD238) and vendor datasheets. External I/O widths are exact;
internal port bitwidths are estimates based on
industry knowledge and bandwidth measurements (treat them as ~representative,
not vendor-exact). Background reading: SALP (ISCA'12), Tiered-Latency DRAM
(HPCA'13), FIGARO (MICRO'20) for DRAM internals; ISAAC and PRIME (ISCA'16)
for ReRAM analog PIM.
