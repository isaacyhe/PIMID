# Memory Technologies

PIMID models 11 memory technologies through three production backends:

| Technology | Backend | Notes |
|---|---|---|
| DDR3, DDR4, DDR5 | Ramulator2 | JEDEC-verified presets |
| LPDDR5, GDDR6 | Ramulator2 | |
| HBM2, HBM3 | Ramulator2 | HBM3 = 16 channels, HBM2 = 8 |
| SRAM | CACTI 7.0 | arrays + cache timing/area |
| STT-MRAM, PCM, ReRAM | NVSim | first run characterizes (~5-7 min), then cached |

**SRAM and NVM timing (no replayed controller).** A PE's memory interface times an SRAM or NVM access with the
array model of one bank unit (`memory.bank_kb`, default 64 KB): a read takes the array's access time, and a write
takes the array's write time (NVSim's write latency; 1.12.90). Each bank serves one access at a time: it is busy
for the access time (a write: the write time), and a request to a busy bank waits until it is free (the bank model,
(1.12.111) release). The interface's own rate is its banks' aggregate (banks it covers x line / access time), so many
requests spread over its banks overlap while requests to one bank serialise. The array is characterised at
`power.device_corner` for both its power and its timing.

Per-technology JEDEC organization (channels, banks, timings) drives both the
memory timing and the device-internal network shape -- see
[dram_specs.md](dram_specs.md) and [network.md](network.md).

**Both memories are replayed controllers (1.12.18, ruling 17).** The device
memory's interfaces (since 1.11.98) and, in system scope, the host memory
controller record every DRAM access per epoch and replay it through a
Ramulator2 controller at the epoch boundary; each access is priced at the
previous epoch's measured service latency of its interface. The host keeps its
calibrated host-path adder on top (no DRAM timing in it); the M/D/1 queue it
used before prices only the first epoch, or everything with
`system.hosts[].memory.dram_replay: false`. When the device is the host's
memory the host joins the device's instance: one controller, one refresh
schedule, the controller's row and activate counters split by origin. A
decoupled host memory has its own instance. The controller counters (row hits /
misses / conflicts, activates, refresh commands, replayed and idle cycles,
bank-open time) feed the energy model; see [power.md](power.md).

**Under load the price follows the controller ((1.12.145), user ruling 2026-10-28).** A request's latency
counts from the moment it reached the controller's door, so a wait at a full queue is latency. The replay has its own
epoch (`simulation.dram_epoch_phases`, one phase), and an epoch still in flight is priced with its unfinished requests
counted at their age, so the latency the cores pay follows what the controller measures on loaded multi-core cells as
it already did on single-core ones.

**So is a memory of type Ramulator ((1.12.147), user ruling 2026-10-28).** With
`pim.placement.level: HOST_MC` the elements share the host memory controller,
which is built as one controller per channel of the memory (HBM3 16, DDR5 2).
Each controller is one interface of a single replay instance of that memory --
its own Ramulator2 YAML, technology, channel count and Ramulator's own address
mapper (RoBaRaCoCh), the line's own address -- and is charged in the bound
phase at the measured service latency of its interface (no path adder: the
controller charges the DRAM service, as before). An M/D/1 at tRCD + tCAS and
the channel's bandwidth prices the accesses before the first replayed epoch
(WeaveSimple's form for in-order and OOO elements, as on the replayed host
memory). The controller's row hits / misses / conflicts, activates, refresh
commands, replayed and idle cycles and bank-open time are the run's controller
counters (stats group `memDramReplay`). The weave-timed Ramulator controller
this replaces -- DAMOV-style: the bound phase charged tRCD + tCAS, Ramulator2
ran in the weave phase, so a dependent request never saw its latency -- is
removed, and no DRAM is timed in the weave phase any more. A system router
device of type Ramulator (PIMID's emitter never writes one; only a
hand-written zsim configuration can) is refused at load. An ALU element at
`HOST_MC` reaches no memory controller (it has no memory interface and
charges its flat access cost), so its run has no DRAM traffic to replay.

## PE placement levels

PEs attach inside the memory hierarchy via `pim.placement.level`. The levels,
finest to coarsest, are `SUBARRAY`, `BANK`, `BANK_GROUP`, `RANK`, `CHANNEL`,
`LOGIC_DIE`, plus `HOST_MC` (PEs share the host memory controller, a replayed controller since (1.12.147); see above). `CHIP`
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
non-NVM part names a tier the part does not have and is refused. The L0 count
key (`memory.subarrays_per_bank` / `subbanks_per_bank` / `mats_per_bank`) and
the `noc.levels` L0 key (`subarray` / `subbank` / `mat`) follow the same rule
(1.11.74). NVSim itself defines bank, mat and subarray and nothing else, so no
NVM has a subbank; CACTI defines the subbank for SRAM. Note that
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
regions hosting no PE). Nothing is cached, migrated, or auto-prepped by the
simulator; data placement is the workload's job (see the device-org prep
section in [benchmarks.md](benchmarks.md)). The compute unit's own data RAM
(`pim.pe.spm_bytes`, 1.12.18) is a scratchpad, not a cache: it holds only the
blocks the kernel moves into it ([benchmarks.md](benchmarks.md#the-pe-scratchpad)).

**Where data lives: first touch (1.12.18).** A 4 KB data page lives in the
memory of the first PE that accesses it, in every mode (serial, shared memory,
message passing), as Linux places a page on the node of the thread that first
touches it and as real PIM keeps a PE's data in its own bank (UPMEM's MRAM,
the HBM-PIM / AiM banks). A PE's pages fill its own units in the order it
touches them, `pages_per_unit` pages to a unit. A thread's stack follows its
own rule (it lives in its PE's memory, claimed by that PE; 1.12.17). So a
program decides placement the way it would on the hardware: data a thread
initialises lives with that thread's PE, and data one thread initialises for
all (a serial setup loop) lives with that one PE. Until 1.12.17 pages were
placed by address, in blocks of `pages_per_unit` pages across all units,
whoever used them; placement-dependent results of 1.12.17 and earlier are not
comparable. A page no PE has touched is the host's own and is spread over all units page by page, as a host memory controller interleaves its pages ((1.12.146), user ruling 2026-10-28). Capacity is not enforced: a PE that touches more pages than its
units hold wraps around within them, as the address map did. Each PE
interface reports `ftPages`, the pages it first touched inside the ROI (pages
claimed during setup are not counted there).

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
