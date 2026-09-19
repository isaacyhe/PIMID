# Memory Technologies

PIMID models 11 memory technologies through three production backends:

| Technology | Backend | Notes |
|---|---|---|
| DDR3, DDR4, DDR5 | Ramulator2 | JEDEC-verified presets |
| LPDDR5, GDDR6 | Ramulator2 | |
| HBM2, HBM3 | Ramulator2 | HBM3 = 16 channels, HBM2 = 8 |
| SRAM | CACTI 7.0 | arrays + cache timing/area |
| STT-MRAM, PCM, ReRAM | NVSim | first run characterizes (~5-7 min), then cached |

Per-technology JEDEC organization (channels, banks, timings) drives both the
memory timing and the device-internal network shape — see
[dram_specs.md](dram_specs.md) and [network.md](network.md).

## PE placement levels

PEs attach inside the memory hierarchy via `pim.placement.level`. The levels,
finest to coarsest, are `SUBARRAY`, `BANK`, `BANK_GROUP`, `RANK`, `CHANNEL`,
`LOGIC_DIE`, plus `HOST_MC` (PEs share the host memory controller). `CHIP`
exists as a virtual layer but is degenerate -- not a distinct placement tier.

Which levels are valid is **per technology**: a tier must be real silicon in
that device's JEDEC organization, so the coarse anchor differs by device class
-- DDRx is rank-centric, LPDDR5/GDDR6 are channel-centric, and only HBM has an
in-package logic die.

| Technology | Valid placement ladder (fine -> coarse) |
|---|---|
| DDR3 | `SUBARRAY -> BANK -> RANK` (no bank groups) |
| DDR4, DDR5 | `SUBARRAY -> BANK -> BANK_GROUP -> RANK` |
| LPDDR5, GDDR6 | `SUBARRAY -> BANK -> BANK_GROUP -> CHANNEL` |
| HBM2, HBM3 | `SUBARRAY -> BANK -> BANK_GROUP -> CHANNEL -> LOGIC_DIE` |
| SRAM | `SUBBANK -> BANK` (CHIP is the configured device network) |
| STT_MRAM, PCM, RERAM | `MAT -> BANK` (CHIP is the configured device network) |

**Exactly one tier below the bank, per family (1.11.73).** The word for that
tier is the family's own, and each family has only one:

| Family | Tier below the bank | What it is | Count and width come from |
|---|---|---|---|
| DRAM | `SUBARRAY` | the local-sense-amp row stripe (512 rows; 1024 on HBM) | the preset's `bank_rows` / the subarray height; width = the global sense-amp datapath of the architecture object |
| SRAM | `SUBBANK` | CACTI's line of mats activated together to produce one data word; its width IS the bank's (`num_do_b_subbank = out_w`) | CACTI geometry: mats per access, `out_w` |
| NVM | `MAT` | NVSim's mat, the unit it H-trees into a bank | NVSim: `numRowMat x numColumnMat`, `mat.numDataBit` (cached per part since 1.11.73) |

`SUBARRAY` on SRAM/NVM is accepted as the legacy spelling (same level, the run
names the canonical word once); `SUBBANK` on a non-SRAM part or `MAT` on a
non-NVM part names a tier the part does not have and is refused. Note that
CACTI's and NVSim's *own* "subarray" is a smaller thing than the DRAM
subarray -- it is the array slice below a mat -- and is deliberately NOT a
PIMID tier: there is one level below the bank, not two. An NVSim cache entry
written before 1.11.73 carries no mat count or width; the run says the tier
is not sourceable, keeps the tree shape it has as an unsourced count, and
refuses to PLACE PEs at `MAT` until the cache is regenerated (or
`memory.subarrays_per_bank` states a count explicitly, which is then labelled
as user-set exactly as on DRAM).

Finer placement = more PEs, each local to a smaller slice, and a deeper, more
parallel device network. Coarser placement funnels more PEs through shared
datapaths, so they contend more. The detailed model charges this contention for
real -- including cross-rank contention under message-passing (see
[network.md](network.md)).

Access pricing is purely by data LOCATION: a PE's access to its own placement
unit traverses zero network hops; anything else pays the real tiered tree
distance to where the data lives (or to its region's abstract endpoint, for
regions hosting no PE). The PE local memory is a scratchpad, not a cache --
nothing is cached, migrated, or auto-prepped by the simulator; data placement
is the workload's job (see the device-org prep section in
[benchmarks.md](benchmarks.md)).

When multiple PEs are placed at a level and no explicit `pim.pe_mem_map` is
given, PEs are distributed **distant/strided** across that level's units -- each
PE local to its own evenly-spaced slice (e.g. 4 PEs over 512 banks land on banks
0, 128, 256, 384), never clustered at the front. This keeps per-PE working sets
disjoint and the load balanced; an explicit `pe_mem_map` overrides it.

## Characterization cache warehouse

Expensive deterministic backend characterizations (NVSim's multi-minute design
search) are memoized in a central warehouse with `rw/ro/wo/off` modes and an
inspectable manifest: a technology is characterized once and reused by every
later run and sweep process. Details: [cache_warehouse.md](cache_warehouse.md).
