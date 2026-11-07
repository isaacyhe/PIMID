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
1.12.111). The interface's own rate is its banks' aggregate (banks it covers x line / access time), so many
requests spread over its banks overlap while requests to one bank serialise. A request also waits for the other
elements' use of its bank, from the latest final epoch: the M/D/1 wait of that use, bounded by the bank's closed
population (at most the other elements that used the bank in that epoch, each with the PE's in-flight window,
can be ahead of it), so the wait is never more than (those elements) x (window) x (their mean service time).
The array is characterised at `power.device_corner` for both its power and its timing.
At the `BANK` placement the element's controller posts the writes to the banks of its slot ((1.12.266),
`memory.write_buffer_entries`, 32 entries by default): a write is acknowledged when the controller has taken it into
its write buffer, and it then waits for its bank. The banks serve reads first: a read goes ahead of the posted writes
still waiting for its bank, a write that has started holds the bank for the write time, and while more than 80% of
the entries are taken the oldest writes keep their place. A read of a buffered line is answered from the buffer. The
other elements' use of the bank is then a two-class queue that serves reads first (non-preemptive priority, Cobham
1954): a read waits for the other elements' reads and for the access in service, a write for both classes. `0` keeps
blocking writes (see [network.md](network.md)).

Per-technology JEDEC organization (channels, banks, timings) drives both the
memory timing and the device-internal network shape -- see
[dram_specs.md](dram_specs.md) and [network.md](network.md).

**Both memories are replayed controllers (1.12.18).** The device
memory's interfaces (since 1.11.98) and, in a co-simulation, the host memory
controller record every DRAM access per epoch and replay it through a
Ramulator2 controller at the epoch boundary; each access is priced at an
earlier epoch's measured service latency of its interface (the newest epoch at
or before k-2 the controller has been fed; 1.12.145). The host keeps its
calibrated host-path adder on top (no DRAM timing in it); the M/D/1 queue it
used before prices only the first epoch, or everything with
`system.hosts[].memory.dram_replay: false`. When the device is the host's
memory the host joins the device's instance: one controller, one refresh
schedule, the controller's row and activate counters split by origin. A
decoupled host memory has its own instance. The controller counters (row hits /
misses / conflicts, activates, refresh commands, replayed and idle cycles,
bank-open time) feed the energy model; see [power.md](power.md).

**Under load the price follows the controller (1.12.145).** A request's latency
counts from the moment it reached the controller's door, so a wait at a full queue is latency. The replay has its own
epoch (`simulation.dram_epoch_cycles`, 10 000 cycles, one phase at the default phase length; since 1.12.227 a shorter
`simulation.phase_length` no longer shortens it), and an epoch still in flight is priced with its unfinished requests
counted at their age, so the latency the cores pay follows what the controller measures on loaded multi-core cells as
it already did on single-core ones.

**The price is a window, and message-passing host records sit on the phase grid.**
An access is charged the request-weighted mean latency over the epoch it reads and the seven finalized epochs before
it (`simulation.dram_price_window`, 8 epochs; 1 = the epoch read alone). A single epoch read two back let a bursty
load lock into a cycle of period two lags -- burst epochs priced at the quiet epoch before them, quiet epochs at the
burst. Under thread-MPI the host cores' records are stamped on the ROI-relative phase grid, the axis the
shared-memory replay uses (the phases since the ROI baseline, plus the core's offset in its phase): a rank's own
clock stops while it waits, so on that clock a rank that had waited trailed the others and its records fell behind
the replay clock; on the grid every host record lands in the epoch of the phase it was issued in, and a host read
takes the epoch two back like every message-passing read. A run whose ranks all run on the host cores (a no-offload
baseline) folds its epochs at the grid, phase by phase, and its run time is counted on the same grid (see
[power.md](power.md)). An access a device interface priced at the replay and the replay then found outside the ROI
leaves `replayPriced` / `replayLat`, as it leaves rd / wr, so the charged view counts the ROI's requests only.

**A rank waiting on a peer does not hold the replay (1.12.176, 1.12.178).** Under thread-MPI the device replay folds an
epoch only once every rank that can still record into it has passed it (the consistent cut, 1.12.17). A rank parked in
a blocking wait -- a receive whose message has not been posted, a barrier whose last rank has not arrived -- announces
the wait, and the cut holds it at the earliest time it can be released instead of at its own frozen clock: a receive
from one source by the oldest message from that source already posted to it (or, with none, by that source's own
bound); a receive from any source by the messages posted to it and the other ranks' bounds; a barrier by its latest
arrival and, until it is released, the bounds of the ranks that have not arrived. A rank that is not waiting is bounded
by its clock. The bounds are solved together at each phase end from the simulated state alone; a wait that cannot be
bounded falls back to the rank's own clock.

**The price is per request and per kind.** Each epoch of the price window weighs its mean latency by its request
count, and an interface's reads, writes and prefetches are priced apart: a read at its reads' latency, a store at the
time the controller took the interface's writes into its write queue (stores are posted: a store holds its core until
then), a host L2 prefetcher's request at the prefetches' latency (they run ahead of the demand traffic and queue behind
it).
A mean below one cycle is priced at one cycle. An access of the second ROI epoch reads the first once it has been
fed instead of the analytical bootstrap.

**The replay is closed-loop, and each core pays what its own accesses measured.** In cut mode a core's accesses
reach the controller as the core can issue them. A blocking core (ALU, in-order) has one access in flight: its next
access goes at its stamp + the balance it has not paid yet, and no earlier than its previous access's completion +
the core's own time between the two (the first access after a message-passing wait: at its stamp + balance). An
out-of-order core has one L1D fill buffer per line miss in flight (the host core's 16, a processing element's 10,
`ooo.l1d_fill_buffers` in the core record). A scratchpad DMA transfer's lines go at the engine's pace from the
transfer's first line, and the core waits for the last. Every access a core waits for adds measured - charged to its
core's balance (an out-of-order core's divided by the overlap of its accesses in the epoch; a DMA transfer once, by
its last line). At the phase barrier a positive balance is added to the core's clock; a negative one is refunded
from the core's later charges, never below zero. A rank entering a message-passing operation settles its balance
first: a delay, or its clock moved back as far as it went since its last synchronization (a rank on a device PE
settles at its rendezvous instead, below). The run time covers what
no barrier applied: each core ends no earlier than its end + its balance, nor than the completion of its last access
it waited for (a blocking core: + its own time after it, unless it waited after it); `roiWallBalanceCycles` is the
part of the run time this adds. The replay statistics report `reconciledAccesses`, `reconciledCycles`,
`reconcileRefunds`, `reconcilePending`, `reconcilePendingMax`, `reconciledCharged`, `ctrlRoiTailTicks` (the last
waited access past the window's close) and `ctrlRoiDrainTicks` (the last request: writebacks and posted stores drain
after the cores). The closed loop holds a core's next access across the controller's channels, so the two-channel
DDR5 replay runs as one instance.

**Message-passing ranks on device PEs settle at the rendezvous (1.12.216).** The controller's clock follows the ranks'
real time: a rank's real time is its clock plus the balance it has not paid yet (the shift its accesses are released
at); every record reaches its core's queue as soon as it exists, and at each phase barrier the controller runs to the
earliest time a PE can still release an access at -- a running PE's clock plus its shift, a rank waiting on a peer by
its wake bound on that axis -- so no access is released behind its clock; the epochs are still folded at the cut, for
the prices. An access a rank issues behind the controller's clock all the same (its balance shrank, a rewind) makes
the rank wait for the clock: the wait is added to what the access measured, for the rank's balance (not to the
prices). At a rendezvous the rank waits, taking the phase barriers without running, until the controller has
completed the accesses its settlement needs: at a barrier, every rank's accesses before its arrival, and the release
is the latest settled arrival; at a receive, the sender's accesses before its send and the receiver's before its
receive, and the message arrives at the send time plus the sender's balance then (both ways, an over-charge no
further back than the sender's last rendezvous), plus the contention wait and the latency, while the receiver's own
balance moves its clock both ways (no further back than its last rendezvous: a receive or a release) and it goes on
at the later of the two. A rank waiting for its settlement does not bound the controller (the accesses it waits for
complete after the controller's clock); once settled it goes on no earlier than the controller's clock. When no rank
could move in a phase and the controller could not run, it runs without the bound to the next completion of a
waiting rendezvous' accesses. A rank waiting for its settlement does not count as one that could move: it moves only
once the controller has run for it; nor does a rank parked in message passing count by its phase state: it moves when
its wait ends. The run log's `[settle]` line counts the settled barriers and messages, these
unbounded runs, the held PE-phases, the balance applied at the rendezvous, how far the senders' balances moved the
arrivals, and the accesses issued behind the controller's clock. A rank on a host core, and a rendezvous outside the
ROI, keep the settling at message-passing entry.

**DDR5 host commands at bank-parallel placements (1.12.169, 1.12.173).** At `BANK` and
`BANK_GROUP` placement the replayed controller is bank-parallel: a PE's data stays inside its
chip, so the controller lifts the column-to-column constraints above the PE's tier. The host's
requests (a host baseline, or the host of a co-simulation) go through the same controller, and
on DDR5 they keep the standard's timing (JEDEC JESD79-5). A host column command holds its
sub-channel's data bus for BL/2 clocks (nBL = 8 tCK for BL16), and between host column commands
the controller keeps the same-bank-group column spacing (tCCD_L, tCCD_L_WR), the read/write
turnaround (tWTR_S/L and the tCCD_S composites) and the rank switch; the chips of a lock-step
rank count as one rank. Host reads and writes are batched as a controller batches them: write
mode starts when more than 32 host writes are held in a 48-entry per-channel write queue
(Chatterjee et al., HPCA 2012, Table 1) or no host read is held, and ends when fewer than 16
are held while a host read waits. A PE's commands neither wait for the bus nor occupy it; the
other standards keep their timing.

**Activation window (tFAW) in the replay ((1.12.273)).** The replay holds JEDEC's four-activate window (Ramulator2's
nFAW) where the standard places it: per rank on DDR3, DDR4, DDR5 and LPDDR5 (per chip with the chip split), per
pseudo-channel on HBM2 and HBM3. The bank-parallel controller keeps it with the other ACT-to-ACT limits (tRRD),
refresh and precharge-all. GDDR6 has no rank level: its channel is the device, and it places tFAW, tRRD_S, the
precharge-all timing and the refresh spacing at the channel level; there the bank-parallel controller lifts only the
column-to-column constraints and keeps the rest ((1.12.274); before, it lifted the channel's ACT and column constraints
with the command bus, and a GDDR6 run at `BANK` or `BANK_GROUP` placement held none of them but tRRD_L). At the end of
the run the replay's activation-window line counts the ACTs
issued, those under a kept window, those that waited for it (the window was the last of their constraints to clear
and cleared after the request arrived) with the DRAM cycles it held them, and the ACTs issued inside a full window
(0 where the window is enforced); a second line counts the ACTs with no window on their path. The configuration
warning for more than four activation streams per rank says which case a run is in.
Both rules come from configuration ((1.12.276)): each technology's part record (`params/dram/<tech>.yaml`) states
`bank_parallel.channel_is_device` (`true` for GDDR6 only) and `activation_window` (`enforce` in every shipped record),
and the run's YAML overrides either (`pim.mc.bank_parallel.channel_is_device`, `pim.mc.activation_window`: `auto` takes
the record). The log names each value and where it came from. With the window off the replay removes every window
constraint and the warning and the activation-window line say so; a decoupled host memory keeps its standard's window.

**A memory of type Ramulator is a replayed controller too (1.12.147).** With
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
`LOGIC_DIE`, plus `HOST_MC` (PEs share the host memory controller, a replayed controller since 1.12.147; see above). `CHIP`
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
| SRAM | `SUBBANK -> BANK` (one die: at BANK each element and the banks of its slot are one local group under the die (1.12.259); there is no bank-group, chip, rank or channel tier to place at; `memory.local_path` chooses the group's path, its local bus, its router or its shared bus on the network) |
| STT_MRAM, PCM, RERAM | `MAT -> BANK` (as SRAM) |

**Where an element's data lives on SRAM/NVM.** A page belongs to the element that touches it first
(first-touch placement), and on SRAM and NVM an element owns several banks (fig2: 256 banks of 64 KB, 16
elements, 16 banks each). The element's pages, in the order it touches them, form one address space over its
own banks, cut into rows of one mat row of the bank: consecutive rows go to consecutive banks (column low, bank
next, row high, the JEDEC order). The mat row is the array model's: NVSim's subarrays across the mat times the
subarray's columns (PCM 256 B, ReRAM 128 B and STT-MRAM 128 B at the default 64 KB bank), CACTI's subarrays per
mat row times the subarray's columns for SRAM (64 B). The run prints it as `[placement] <tech> mat row`. Thread stacks
follow the same rule inside their owner's banks. A row the array model cannot source keeps whole-unit blocks
(pages_per_unit contiguous pages per bank), as DRAM does.

**Exactly one tier below the bank, per family (1.11.73).** The word for that
tier is the family's own, and each family has only one:

| Family | Tier below the bank | What it is | Count and width come from |
|---|---|---|---|
| DRAM | `SUBARRAY` | the local-sense-amp row stripe (512 rows; 1024 on HBM) | the preset's `bank_rows` / the subarray height; width = the global sense-amp datapath of the architecture object |
| SRAM | `SUBBANK` | CACTI's line of mats activated together to produce one data word; its width is the bank's (`num_do_b_subbank = out_w`) | CACTI geometry: mats per access, `out_w` |
| NVM | `MAT` | NVSim's mat, the unit it H-trees into a bank | NVSim: `numRowMat x numColumnMat`, `mat.numDataBit` (cached per part since 1.11.73) |

`SUBARRAY` on SRAM/NVM is accepted as the legacy spelling (same level, the run
names the canonical word once); `SUBBANK` on a non-SRAM part or `MAT` on a
non-NVM part names a tier the part does not have and is refused. The L0 count
key (`memory.subarrays_per_bank` / `subbanks_per_bank` / `mats_per_bank`) and
the `noc.levels` L0 key (`subarray` / `subbank` / `mat`) follow the same rule
(1.11.74). NVSim itself defines bank, mat and subarray and nothing else, so no
NVM has a subbank; CACTI defines the subbank for SRAM. Note that
CACTI's and NVSim's *own* "subarray" is a smaller thing than the DRAM
subarray -- it is the array slice below a mat -- and is deliberately not a
PIMID tier: there is one level below the bank, not two. An NVSim cache entry
written before 1.11.73 carries no mat count or width; the run says the tier
is not sourceable, keeps the tree shape it has as a stated count, and
refuses to place PEs at `MAT` until the cache is regenerated (or
`memory.subarrays_per_bank` states a count explicitly, which is then labelled
as user-set exactly as on DRAM).

Finer placement = more PEs, each local to a smaller slice, and a deeper, more
parallel device network. Coarser placement funnels more PEs through shared
datapaths, so they contend more. The detailed model charges this contention for
real -- including cross-rank contention under message-passing (see
[network.md](network.md)).

Access pricing is purely by data location: a PE's access to its own placement
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
comparable. A page no PE has touched is the host's own and is spread over all units (1.12.146): page by page, or ((1.12.282)) in chunks of `host_interleave_bytes` over the bank groups first, then the banks, ranks and channels, as a host memory controller interleaves consecutive lines. The technology's part record sets it (DDR5 256 B, the others page by page); `memory.controller.host_interleave_bytes` overrides. Page by page, a host stream's lines sit in one bank group: on DDR5 consecutive column commands then pay tCCD_L (tCCD_L_WR for writes) and a host write drain is one bank group's. Capacity is not enforced: a PE that touches more pages than its
units hold wraps around within them, as the address map did. Each PE
interface reports `ftPages`, the pages it first touched inside the ROI (pages
claimed during setup are not counted there).

**Message passing: each rank's own copy of the program's data ((1.12.281)).** The ranks of a message-passing run are
threads of one process, so the program's own data -- its and its libraries' code, read-only data, data and bss, and
argv / envp / auxv at the top of the main stack -- exists once, and under first touch it lives with the element that
touched it first: every other rank's read of an argument string, a string literal or a library global crossed the
in-memory network to that element and queued there. With `workload.mpi_image_data: per_rank` (the default) a rank's
access to that data is placed in its own element's memory, at its offset in the object, as each process of a
multi-process program holds its own copy; the main thread's own accesses to its stack keep the stack rule.
`shared` keeps the single copy (the releases before). The data the kernels build (heap, their slots), the threads'
stacks and the MPI library's transport memory are placed as before; host cores are unaffected.

When multiple PEs are placed at a level and no explicit `pim.mapping` is
given, PEs are distributed **distant/strided** across that level's units -- each
PE local to its own evenly-spaced slice (e.g. 4 PEs over 512 banks land on banks
0, 128, 256, 384), never clustered at the front. This keeps per-PE working sets
disjoint and the load balanced; an explicit `pim.mapping` overrides it.

## Characterization cache warehouse

Expensive deterministic backend characterizations (NVSim's multi-minute design
search) are memoized in a central warehouse with `rw/ro/wo/off` modes and an
inspectable manifest: a technology is characterized once and reused by every
later run and sweep process. Details: [cache_warehouse.md](cache_warehouse.md).
