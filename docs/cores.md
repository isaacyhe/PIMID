# PE Core Models

Four core models are available for PIM device PEs (`pim.pe.type`) and, in
system scope, for hosts (`hosts[].core_type`) and devices (`devices[].pe_type`).
The in-order model also runs as the scalar single-issue PE (`issue_width: 1`,
`branch_predictor: none`; see below), which replaced `simple_core` in 1.12.1.

| Value | ZSim engine | What it models |
|---|---|---|
| `compute_unit` | ALU | A datapath, not a processor: a register file, arithmetic units, a result bus and a resident instruction store, with no caches and no speculation. Sized by `pim.pe.lanes` / `operand_width` / `floating_point` / `imem_bytes` and shaped by the scaling factors below. The default device element. Alias: `alu_core` (which the entire existing sweep corpus names, so it is permanent). |
| `in_order_core` | InOrder | Decode-driven in-order pipeline: real RAW-dependency stalls, functional-unit latencies, and issue in program order (no reordering; `pim.pe.issue_width`, default 2), plus the cross-PE memory-contention weave (`CoreRecorder`). Branch prediction per `pim.pe.branch_predictor`: `pag` (default, from the core record) or `none`. Aliases: `InOrder`, `in-order`, `in_order`. |
| `ooo_core` | Out-of-order | Out-of-order superscalar, 4-issue. A processing element's window is Westmere-class (128-entry ROB); a host core's window is the core record's `ooo_host` (Golden Cove: 512-entry ROB, see below). Aliases: `OOO`, `OoO`, `ooo`, `out-of-order`. |
| `null_core` | Null | No timing model: counts instructions (cycles == instrs, IPC = 1) and drops all memory accesses (empty load/store handlers, so no NoC traffic). An IPC = 1 control/upper-bound baseline. Aliases: `Null`, `null`. |

Any other value is rejected with an error listing the valid names.

**Retired: `simple_core` (1.12.1, ticket #114).** `simple_core` and its aliases
`Simple` / `simple` are refused, with a message naming the replacement:
`in_order_core` with `pim.pe.issue_width: 1` and `pim.pe.branch_predictor: none`
(system scope: `devices[].pe_type: in_order_core` with
`devices[].pim.pe.issue_width: 1` and `devices[].pim.pe.branch_predictor: none`;
a host: `hosts[].core_type: in_order_core` with `hosts[].issue_width: 1` and
`hosts[].branch_predictor: none`).
Its IPC = 1 bound modelled no real PE class -- on the 1.11.x corpus it sat
between 0.77x and 1.13x of `in_order_core` in cycles depending on the kernel,
a bound rather than an element. For an IPC = 1 control bound without memory
traffic, `null_core` remains.

## What every model shares, and what none of them are

All four consume the SAME host instruction stream: the emulator executes the real
guest binary and reports retired instruction counts and load/store addresses.
There is no processing-element instruction set anywhere in the simulator.

The compute unit does not decode at all. It charges every instruction the same
scaled cost, so it models no instruction set and cannot distinguish a
floating-point operation from an integer one. What it does model -- and what its
knobs describe -- is the cost of an operation and the cost of reaching data: the
memory-interface path, locality, and the in-memory network. Use it for
memory-bound kernels, which is what it is for.

That boundary is deliberate and worth stating in any write-up: the memory side of
an element is modelled in detail, the compute side crudely. Processing in memory
exists for memory-bound work, so the well-modelled half is the half that
dominates -- but results about compute-bound kernels on these elements do not
follow from this simulator.

## Compute unit scaling factors

`compute_unit` accepts design-point knobs for processing-using-memory (PUM) and
processing-near-memory (PNM) sweeps:

```yaml
system:
  devices:
    - name: device
      pim:
        pe:
          type: alu_core
          compute_factor: 10.0    # cycles per op (e.g. tRAS-scale for PUM)
          access_factor: 0.0      # memory access cost scale (0 = free local access)
          throughput_factor: 1.0  # issue throughput scale
          bit_serial: false       # datapath: false = bit-parallel (default), true = bit-serial
          operand_width: 32       # operand bit-width (affects cycles only when bit_serial: true)
          energy_factor: 1.0      # per-op energy scale for reporting only
```

`compute_factor`, `access_factor`, and `throughput_factor` scale the simulated
cycles; `energy_factor` scales reported energy only. `bit_serial` selects the
datapath model and, together with `operand_width`, sets how operand width affects
compute cost:

- **`bit_serial: false` (default -- bit-parallel):** a full-width ALU processes
  any operand in one step, so `operand_width` does **not** affect cycles (it is
  descriptive). This is backward-compatible -- existing configs are unchanged.
- **`bit_serial: true` (bit-serial PUM):** a W-bit operation costs ~W bit-steps,
  so compute cost scales linearly with `operand_width` (`operand_width: 32` is
  ~32x a single-bit op; `operand_width: 1` is the bit-serial primitive). Use this
  to model processing-using-memory (PUM) datapaths; `compute_factor` sets the
  per-bit-step cost.

## Memory-level parallelism (MLP)

The analytical NoC model divides per-access latency by an MLP intensity `M`
(`noc.mlp`). `M` is a property of the core model, calibrated against the
cycle-accurate `detailed` model; omit `noc.mlp` to use the calibrated default.
See [network.md](network.md).

## Fidelity ladder (scalar in-order vs in-order vs out-of-order)

`in_order_core` is a decode-driven **in-order pipeline**: it consumes the decoded
uops (below) and issues them in strict program order, stalling on real
per-instruction RAW dependencies, functional-unit latencies, and
issue-width/port contention -- with no reordering (mechanism details in the
Notes below). Dependency chains it cannot hide push it above one cycle per
instruction on latency-bound kernels (FP-latency-bound stencil), while
dual-issue plus cross-block overlap pulls it below one cycle per instruction on
kernels with exploitable independence (gemv, histogram); it also carries the
cross-PE memory-contention weave (`CoreRecorder`) and pays branch-mispredict
flush bubbles.

The **scalar single-issue in-order PE** is the same core with
`pim.pe.issue_width: 1` and `pim.pe.branch_predictor: none` (next section): one
uop issued per cycle, no branch prediction. It models a programmable PE with
no branch predictor; it is not a renamed IPC = 1 bound, since it keeps every
stall the in-order core models.

`ooo_core` adds out-of-order issue (a 128-entry ROB + reordering on a processing element; the host's window is below), hiding latency
the in-order core must stall on, so `ooo <= in_order` holds across the kernel
suite (typical gaps 2-3.5x, tracking each kernel's ILP/MLP). Use
`in_order_core` for dependency/issue-accurate in-order timing (scalar or
dual-issue, with or without a predictor), `ooo_core` for the reordered upper
bound, `null_core` for an IPC = 1 control bound.

## Scalar single-issue PE (`issue_width: 1`, `branch_predictor: none`)

```yaml
system:
  devices:
    - name: device
      pim:
        pe:
          type: in_order_core
          issue_width: 1          # one uop issued per cycle
          branch_predictor: none  # pag (default, the core record) | none
```

`pim.pe.branch_predictor` (1.12.1) selects the in-order core's front end:

- `pag` (the default, from the core record `params/core/default.yaml`,
  `in_order.branch_predictor`): the 2-level PAg direction predictor, the
  512-entry BTB and the 16-entry return-address stack described in the Notes.
- `none`: no prediction structure. Fetch always continues at the fall-through
  (static not-taken); nothing is predicted, read or trained. A redirect pays
  the bubble of the stage that first knows its target, with the core record's
  two penalties: a **taken** conditional branch, an indirect jmp/call and a
  return are known at execute and pay the execute-depth flush
  (`in_order.mispredict_penalty_cycles`, 7); a direct call or jmp has its
  target computed by the decoder and pays the decode-depth resteer
  (`in_order.resteer_penalty_cycles`, 4); a not-taken branch is free. The
  counters say so by construction: `mispredBranches == takenBranches`,
  `indirMispreds == indirBranches`, `rasMispreds == rasReturns`,
  `btbMisses == 0`, and the predictor-write counters (`roiBpHistWrites`,
  `roiBpPhtWrites`) stay 0.

The knob applies to `in_order_core` only (refused, rc 2, on any other type:
`ooo_core` always runs its predictor, `alu_core` and `null_core` have none) and
is set per element: `system.devices[].pim.pe.branch_predictor` for a device
(a top-level `pim.pe.branch_predictor` / `pim.pe.issue_width` is the default of
every device node that does not set it, 1.12.2),
`system.hosts[].branch_predictor` for an in-order host (which also takes
`system.hosts[].issue_width`, 1.11.107, so a host can be the scalar PE too;
the flat `host.branch_predictor` / `host.issue_width` of 1.12.2 went with the
flat `scope: cosim` form in 1.12.8).
Issue is per uop, so at width 1 a two-uop x86 instruction takes two issue
slots.

McPAT prices `none` with no predictor (prediction_width 0, no BTB, RAS or
pattern table). With issue width 1 it is the **scalar PE profile**: 1 issue,
1 ALU, 1 MUL, 1 FPU (the FPU goes when `floating_point: false`), no predictor,
at the in-order refill depth (`in_order.mispredict_penalty_cycles`, 7) -- the
cell `simple_core` was priced with, except that cell's depth was 5. The run
prints `[power] in_order_core: scalar PE profile (...)`.
`PIMID_INORDER_NOBRANCH=1` removes the branch feed in this mode too, which makes
every branch free; do not combine it with `none`.

Under thread-MPI, rendezvous clock jumps (barrier/reduce waits) are applied to
the OOO scheduling window via a drain-then-jump bulk advance (1.9.2); the
formerly-pulled OOO+MPI cell class is fully supported since that release.

## Model boundaries (documented, deliberate)

- **In-order memory is blocking**: one outstanding miss at a time, no
  hit-under-miss MLP. Cross-BBL overlap hides functional-unit and dependency
  latency under L1-hit memory, but DRAM-miss chains stay serialized: the
  `CoreRecorder` weave records accesses into a strictly serial event chain
  (`recordAccess` asserts `startCycle >= prevRespCycle`) and the cache has no
  side-effect-free probe to admit hits-only under a miss, so hit-under-miss
  would need an OOOCoreRecorder-style multi-outstanding weave recorder.
- **Wrong-path effects are front-end only, and only on `ooo_core`**: no
  wrong-path uop executes and no wrong-path data access is made on any core.
  `in_order_core` charges its flush/refill bubble and fetches no wrong path.
  `ooo_core` DOES fetch the wrong path of a conditional mispredict into the
  L1I until the branch resolves (at most ceil(penalty x fetch width / line
  size) lines: 5 at the core record's 17 cycles x 16 B over a 64 B line), so
  wrong-path lines occupy the L1I and count in its accesses and in the
  contention weave; an indirect or return target miss fetches no wrong path.
- **Timing microarchitecture is fixed per core model** (`ooo_core`: 4-wide;
  a processing element's window is the Westmere-class 128-entry ROB, compile-time
  default, and a host core's is the core record's `ooo_host`, below;
  `in_order_core`: dual-issue default). The `power.mcpat_overrides` keys (`pipeline_depth`, `issue_width`,
  ...) shape the McPAT power/area model only, never cycle timing -- see
  [yaml_reference.md](yaml_reference.md).
- At large working sets, `ooo_core` can legitimately exceed `in_order_core` on
  branch-heavy irregular kernels: its mispredict redirect is resolution-bound
  -- fetch restarts only after the mispredicted branch completes, and the
  redirect then costs the core record's `ooo.mispredict_penalty_cycles` in
  total (17 cycles, 1.12.0) -- so a load-fed mispredict waits out the load's
  DRAM latency and then pays the full redirect, while a shallow in-order pipe,
  already stalled on the same load, adds only its 7-cycle bubble. This is
  physics, not a calibration error.


### The host core's out-of-order window

A host `ooo_core` (system scope: `hosts[]`, and the host of a co-sim run) is sized from the `ooo_host` section of the core
record `params/core/default.yaml`, as the 4th Gen Xeon Scalable core (Intel Golden Cove) is built:

| Structure | Entries | Source |
|---|---|---|
| Reorder buffer | 512 | 512 entries, up from 352 in Sunny Cove (Chips and Cheese, "Popping the Hood on Golden Cove"; WikiChip) |
| Load queue | 192 | 192 measured on Sapphire Rapids (Chips and Cheese; Intel states 240) |
| Store queue | 114 | WikiChip, Golden Cove |
| Scheduler (instruction window) | 160 | Sunny Cove's size (WikiChip); Intel does not publish Golden Cove's, which is larger |
| L1D fill buffers | 16 | up from 12 (AnandTech, "Golden Cove Microarchitecture (P-Core) Examined") |

A load or store that sends a line request to the next level holds one fill buffer until the line arrives, so the core
overlaps at most 16 line misses (a second access to a line still on its way shares its buffer). `store_buffer: true`
lets a store leave the ROB when it commits and holds its store-queue entry until the line is written, in program order
(the x86 memory model); `false` keeps the ROB entry until the write, as the model did before. The Westmere-class core
this model began with is `rob_entries: 128`, `load_queue_entries: 32`, `store_queue_entries: 32`,
`scheduler_entries: 36`, `l1d_fill_buffers: 0` (unlimited) and `store_buffer: false`; set those values in the record to run
it. A processing element's out-of-order core keeps that Westmere-class window, with 10 L1D fill buffers (`ooo.l1d_fill_buffers`:
Nehalem / Westmere, Intel 64 and IA-32 Architectures Optimization Reference Manual 248966-025, Table 2-7 and sec.
2.3.5); the DRAM replay holds its line misses to the same count. McPAT prices the host core with the same
reorder buffer, scheduler and load/store queue sizes.

### The host core's L2 prefetchers

A host `ooo_core` has the two L2 hardware prefetchers that the Intel 64 and IA-32 Architectures Optimization Reference
Manual (248966-048, "Data Prefetch to the L2 and Last Level Cache", p. 2-17) describes, sized from the `host_prefetch`
section of the core record. They sit between the core's L1D and its L2 (`<host>_l2pf` in the generated ZSim
configuration) and see every L1D request that goes to the L2, loads and stores:

| Parameter | Value | Source |
|---|---|---|
| Streams | 32 | "Detects and maintains up to 32 streams of data accesses"; one forward and one backward stream per 4 KB page |
| Distance | 20 lines | "The streamer can run up to 20 lines ahead of the load request" |
| Prefetches per L2 lookup | 2 | "The streamer may issue two prefetch requests on every L2 lookup" |
| Spatial prefetcher | on | completes "every cache line fetched to the L2 cache with the pair line that completes it to a 128-byte aligned chunk" |
| Superqueue entries | 48 | an L2 miss holds one until its line arrives; 48 on Golden Cove, up from 32 (Chips and Cheese, "Popping the Hood on Golden Cove", which also notes that superqueue entries can be held by prefetch requests) |

- **Streams.** A stream is detected by two requests to one 4 KB page whose line offsets move in one direction; from then
  on each request in that direction prefetches up to two lines beyond the furthest already prefetched, no more than 20
  lines ahead of the request and never past the page.
- **Superqueue.** A demand L2 miss or a prefetch that fetches its line holds one superqueue entry until the line arrives;
  a prefetch is not issued while all 48 are held (the manual: with many outstanding requests the streamer prefetches less
  far ahead).
- **Timing.** A prefetch brings its line into the L2 (and, through the L2's own miss, the LLC). A request that finds the
  line still in flight waits for its estimated arrival; a request that finds it already there pays an L2 hit. The
  prefetch's own memory request is timed and replayed like a demand miss, and its DRAM traffic counts in the memory
  statistics and in power, but no core waits for it: in the contention model it starts with the request that triggered
  it and nothing depends on its end. The memory controller counts the prefetcher's reads as `pfrd` / `pfrdlat` (also
  included in `rd` / `rdlat`), and each prefetcher reports its requests, its useful and late prefetches and the streams
  it detected.
- **In the replayed memory.** A prefetch's request is not one its core waits for: it is not reconciled against the core.
  It holds one of the core's superqueue entries from its release to its completion; the replay releases it, and an access
  the core waits for, only while an entry is free. A request that found a prefetched line is settled against the arrival
  the replay measured. The core's stall on prefetches is the union of its waits, so per core, in the order the waits
  began, the growth of the union of the waits the measured arrivals give less that of the waits the core was charged goes
  to the core's balance (waits the core overlaps count once; a request that found its line already there waits if the
  measured arrival is later). A measured arrival is placed on the consumer's timeline: the prefetch left at its own
  time, so the corrections already settled for the waits the core began after the prefetch was issued come off it (they
  moved the core, not the prefetch). A core whose prefetches arrive late therefore issues its next ones late as well, and
  one whose waits shrink cannot run ahead of the lines it still needs.
- **Not modelled.** The L1D prefetchers (next-line and instruction-pointer stride), code fetches as streamer triggers,
  and prefetching to the LLC only when the L2 is busy (the manual gives no threshold). Golden Cove's own prefetcher
  enhancements are described only as "enhanced data prefetchers for increased memory parallelism"; the streamer's
  published figures are used.

`l2_streamer: false` removes the prefetchers; a processing element's out-of-order core has none.


### The host's caches

A host (system scope: `hosts[]`, and the host of a co-sim run) has the caches of the same Golden Cove core, from the `host`
section of the cache record `params/cache/default.yaml`. A host with no `cache:` block has exactly this hierarchy (the run prints
it: `host caches (cache record, host core)`), and so does a run without a host node where it needs one. A size, associativity or
latency the configuration gives replaces the record's for that host and the rest comes from the record: a level given a size keeps
the record's associativity and load-to-use latency unless it also gives them. When the record's ways do not make a power-of-two
number of sets at the size given, the nearest associativity within a factor of two that does is built and the run says so (32 KB
L1D: 8-way instead of 12; 32768 KB L3: 16-way instead of 15); if none does, the run is refused with a message that names the size
and the sizes that fit. `scripts/check-host-cache-defaults.py` checks the cases against the record. A processing element's caches
are the record's `levels`.

**The L3 has three forms.** Omitted: the record's, one slice per core (below). An explicit total size, `l3_kb: 32768`: that size,
shared by all the cores, with the record's 15 ways (adapted as above) and 33 ns unless `l3_ways` or `l3_latency_ns` is given.
`l3: none` (or `l3_kb: 0`): no last-level cache, and only that is. The L2's misses then go to the memory path, whose host-path
adder keeps the published cache path (no L3 is timed); the power model prices no L3, so there is no L3 leakage or dynamic power,
and the uncore-load term, which is 0.31 W per MB of last-level cache, is 0 (the uncore floor, 18.5 W per socket, stays); the run
prints `L3: none`. Any other value of `l3` is refused. Load-to-use is what a load that hits at that
level measures, from its issue to its data.

| Level | Size | Ways | Load-to-use | Source |
|---|---|---|---|---|
| L1D | 48 KB | 12 | 5 cycles | Intel 64 and IA-32 Architectures Optimization Reference Manual 248966-050 (Golden Cove: 48 KB data cache; Table 2-9, first level 5 cycles); Chips and Cheese, "Going Armchair Quarterback on Golden Cove's Caches" (12-way) |
| L1I | 32 KB | 8 | the array time (CACTI) | Wikipedia, Golden Cove |
| L2 | 2 MB per core | 16 | 16 cycles | Intel manual (2 MB on server parts); Chips and Cheese, "Sapphire Rapids: Golden Cove Hits Servers" (2 MB, 16 cycles) |
| L3 | one 1.875 MB slice per core, shared, non-inclusive | 15 | 33 ns | Chips and Cheese, same article (1.875 MB per core slice, "around 33 ns", non-inclusive); Wikipedia, Sapphire Rapids; Intel, Hot Chips 29 (non-inclusive L3) |

The ways of the four levels are the CPUID leaf 4 values of the QEMU cache model for Sapphire Rapids (qemu commit 8d69fc21,
"i386/cpu: Introduce cache model for SapphireRapids"): 12, 8, 16 and 15. The line is 64 B.

The L3 scales with the cores the host models: an N-core host has N slices of 1.875 MB (1 core 1.875 MB, 4 cores 7.5 MB,
16 cores 30 MB), shared by all its cores, and N stops of the host fabric. The simulator's set count is a power of two, so a core
count that is not one builds the next power of two of slices (6 cores: 8 slices, 15 MB).

**The L3 is non-inclusive** (`l3_inclusion`; the record's `non-inclusive`), as the server L3 has been since Skylake-SP (Intel,
"The New Intel Xeon Scalable Processor (formerly Skylake-SP)", Hot Chips 29, 2017, the slide "Inclusive vs Non-Inclusive L3";
Chips and Cheese, "Sapphire Rapids: Golden Cove Hits Servers": "SPR's L3 is clearly non-inclusive"). The L2 is the primary
cache and the L3 an overflow cache, following the three points of that slide:

1. a line read from memory fills the requesting L2 only;
2. an L2 eviction, modified or unmodified, is written into the L3 (a dirty L3 victim that no L2 holds is written back to
   memory, a clean one is dropped);
3. a line one core reads while another core holds it is copied into the L3 as well. A line found in the L3 that no other core
   holds moves to the requesting L2, and a line requested for writing leaves the L3.

A snoop filter tracks every line the L2s hold: their holders, whether one holds it exclusively, and whether it is dirty. It
holds no data and never invalidates an L2 to make room (it is sized by what the L2s hold), so coherence across the cores is
exact (it is priced as its own array, [power.md](power.md)). A request for a line that only another core's L2 holds is
answered by that L2 (the L3's latency, then the other L2's).
A core can keep about its L2 plus the L3 on chip (2 MB + 1.875 MB at one core). `l3_inclusion: inclusive` builds the L3 as a
superset of the L2s instead: an L2 then holds at most what the L3 holds, which at 1.875 MB per core is less than the 2 MB L2,
so at one core the L3 adds nothing.

A latency is charged where it belongs. The L1D charges its own to every access (5 cycles), the L2 the part beyond the L1D
(11), the L3 the part beyond the L2 (33 ns at the host clock less 16 cycles). The L3's value already contains the on-die
network a line crosses, so the host fabric's mean round trip is taken out of it (the port takes the fabric's shortest round
trip out of every request, the cache the difference of the mean and the shortest): a load that hits the L3 measures 33 ns at
any number of cores. The idle host memory latency (the host-path adder of each memory technology) is reduced by the same
33 ns, the cache path the L3 now times. The simulator's L2 and L3 have no MSHR limit of their own; the lines a core has in
flight are limited by the L1D's fill buffers (the core's `l1d_fill_buffers`).

The power model prices the same arrays. CACTI builds only power-of-two associativities, so the 12-way and 15-way arrays are
priced as 16-way with the same capacity. The L3 is priced as one bank per slice. A last-level cache above 64 MB (more than 34
cores) is priced as replicated 2 MB slices, and the host ring's links span a die that holds all of them. The processing elements' caches, and a host whose configuration gives its own cache block,
keep CACTI's array time as their latency, which is far below a load-to-use (1 cycle for a 256 KB array at 22 nm and 2 GHz).

The old host (32 KB 8-way L1D, 32 KB 4-way L1I, 256 KB 8-way L2, a 32 MB 16-way L3, CACTI's array times of 1, 1, 1 and 6 cycles
as latencies) is the cache block `l1d_kb: 32`, `l1d_ways: 8`, `l1d_latency_ns: 0.5`, `l1i_kb: 32`, `l1i_ways: 4`,
`l1i_latency_ns: 0.5`, `l2_kb: 256`, `l2_ways: 8`, `l2_latency_ns: 0.5`, `l3_kb: 32768`, `l3_ways: 16`, `l3_latency_ns: 3.0`,
`l3_inclusion: inclusive`.

## Notes

- `null_core` issues NO memory or network traffic -- its load/store handlers are
  empty, so it drops every access. It therefore cannot drive memory- or
  network-only studies (use `alu_core` with `access_factor`/`compute_factor`).
  Its role today is an IPC = 1 control/upper-bound baseline. Note: PIMID does not
  switch core models mid-run, so `null_core` cannot yet fast-forward non-ROI
  regions into a detailed core; true fast-forward (a mid-run core switch, a
  separate mechanism from trace/replay) is not supported.
- In-order/out-of-order cores automatically upgrade a `simple` memory controller to
  `weavesimple` for correct weave-phase interaction.
- `in_order_core` reuses the same in-tree x86 decoder as `ooo_core` (below) to
  drive an in-order scoreboard: per-register ready-cycle tracking, dual-issue in
  program order, functional-unit port contention, and load-use stalls (no
  reordering). The scoreboard carries across basic-block boundaries and drains
  only at mispredict flushes and scheduler boundaries
  (join/phase/context-switch). Branch modeling (`branch_predictor: pag`, the
  default; `none` is described above) matches `ooo_core`: a 2-level PAg
  predictor for conditional direction plus a 512-entry BTB (indirect jmp/call
  targets) and 16-entry return-address stack, all fed with real outcomes. A
  conditional mispredict and an indirect jmp/call or return target miss charge
  the execute-depth flush bubble (the target is known only at execute); a taken
  direct branch (jcc, call rel) that misses the BTB charges the shorter
  decode-depth resteer bubble (1.11.98 ruling (a); 7 and 4 cycles
  from the core record `params/core/default.yaml`, set per run with
  `core.in_order.mispredict_penalty_cycles` / `core.in_order.resteer_penalty_cycles`;
  env overrides `PIMID_INORDER_MISPRED_PENALTY` / `PIMID_INORDER_RESTEER_PENALTY`;
  disable all branch modeling with `PIMID_INORDER_NOBRANCH=1`). (1.11.44: the legacy IPC = 1
  `PIMID_INORDER_NODECODE` escape hatch is REMOVED -- its A/B purpose ended
  with the 1.4.x validation, and release-vs-release builds are the baseline
  method. The in-order core has exactly one timing model.) The issue width is configurable via `pim.pe.issue_width`
  (default 2, valid 1-6 -- the core's six issue ports; any other width is
  refused since 1.12.2, where it used to run at 2; env `PIMID_INORDER_WIDTH`
  overrides YAML). Diagnostics per
  in-order core: `uops`, `decodedBbls`, `syntheticBbls`, `depStalls`,
  `issueStalls`, `branches`, `takenBranches` (1.12.1), `mispredBranches`,
  `mispredStallCycles`, `indirBranches`, `indirMispreds`, `rasReturns`,
  `rasMispreds`, `directBranches`, `btbMisses`, `roiBpHistWrites`,
  `roiBpPhtWrites`, `repDrainedLoads/Stores`, `memMismatchLoads/Stores`.
- Under QEMU user-mode execution the plugin decodes each guest x86 instruction
  into ZSim `DynUop`s (register read/write sets, latency class, functional-unit
  port, load/store markers) with a minimal in-tree x86-64 decoder
  (`external/zsim/src/x86_decoder.h`). This drives the out-of-order core's
  dependency-driven pipeline (128-entry ROB, 4-wide issue, port contention,
  load/store queues, register scoreboard) instead of the old synthetic 1-CPI
  path, so `ooo_core` now models ILP and memory-level parallelism: on
  compute-bound kernels it retires faster than `in_order_core` (typical gaps
  2-3.5x under HBM3/16-PE/detailed-NoC; the gap tracks the kernel's ILP/MLP).
  The decoder covers the common
  integer/SSE/SSE2/AVX forms precisely (full integer ALU incl. group-1 immediate
  arithmetic and shifts, load-op/rmw, lea, push/pop/call/ret, mov-imm,
  movzx/movsx/movsxd, jcc/cmov/setcc, SSE/SSE2 scalar+packed FP add/mul/div/sqrt/
  cvt/compare, packed-integer logic/add/sub/mul/shift/shuffle/pack, pmovmskb,
  bsf/bsr, BMI2 bzhi/pdep/pext, vpbroadcast, lock-prefixed atomics/cmpxchg/xadd/
  xchg as fenced rmw, div/idiv with the true rdx:rax pair via a merge uop +
  divide uop, and widening mul rd={rax,rdx}). rep movs/stos use a documented
  block-copy model: register-side dependency uops plus the QEMU-delivered
  accesses through a drain after the block's last uop, counted as
  `repDrainedLoads/Stores`. The drain is serial only on `in_order_core` (each
  access waits for the previous response); on `ooo_core` the drained accesses
  are issued together, cost no issue cycles and only extend the block's
  completion, which a mispredicted terminator of that block redirects from. The
  generic-uop fallback is ~0.0% of dynamic instructions (only serializing ops:
  syscall/cpuid/rdtsc/xsave). Unrecognized instructions' memory accesses are
  absorbed by a tolerant load/store drain, so counts never desync (measured
  memMismatch is <0.05% of memory ops). Branch modeling: the direction predictor
  is driven by the real per-branch direction (resolved from the actual next TB),
  and indirect jmp/call targets and returns are predicted by a 512-entry
  direct-mapped BTB (last-seen-target) plus a 16-entry return-address stack --
  wrong targets pay the same front-end redirect as a conditional mispredict.
  The redirect is resolution-bound and its TOTAL cost is the core record's
  `ooo.mispredict_penalty_cycles` (17 cycles, Westmere-class; set per run
  with `core.ooo.mispredict_penalty_cycles`, at least 9): fetch restarts
  after the mispredicted branch uop completes (execute-complete, not ROB
  retirement), placed so that the first correct-path uop dispatches no
  earlier than the branch's resolution + the penalty. The pipeline's stage
  model already charges 9 of those cycles after any fetch restart
  (fetch->decode 3 + issue->dispatch 6), so the restart itself is placed
  penalty - 9 = 8 cycles after resolution; through 1.11.x the restart was at
  resolution and a mispredict cost those 9 cycles alone (1.12.0). An L1I
  miss on the correct path adds its latency on top. The same value sizes the
  wrong-path fetch (Model boundaries, above).
  Diagnostics per out-of-order core:
  `uops`, `decodedBbls`, `syntheticBbls`, `approxInstrs`, `mispredBranches`,
  `indirBranches`, `indirMispreds`, `rasReturns`, `rasMispreds`,
  `repDrainedLoads/Stores`,
  `memMismatchLoads/Stores`. Env toggles: `PIMID_OOO_NODECODE=1` forces the legacy
  synthetic path; `PIMID_OOO_NOBRANCH=1` disables ALL branch modeling (direction, BTB, RAS);
  `PIMID_OOO_DUMP=1` profiles which opcodes hit the generic fallback (weighted by
  dynamic count); `PIMID_OOO_DEBUG=1` traces the first decoded BBLs. Other core
  types ignore the decoded uops and never receive branch callbacks, so their
  timing is byte-for-byte unchanged.
