# Host-Device Co-simulation

Co-simulation (`scope: system`) runs a general-purpose **host** and a
**PIM device** as one system: the host is a real CPU (out-of-order cores,
cache hierarchy, its own main memory), the device is processing elements
inside memory. An offloaded kernel migrates from the host onto the device
PEs and back; the boundary costs -- launch, coherence, and the host<->device
bridge -- are priced explicitly. The device-side execution is the *identical*
code path a standalone `scope: device` run uses (same PE memory interfaces,
same Garnet/analytical NoC, same JEDEC-derived memory timing), so device-only
cycles are unchanged by co-sim.

There are **no special co-sim workloads**: any ordinary ROI-marked workload
runs in co-sim unchanged. In a standalone device simulation only the ROI
(the kernel) is counted; in co-sim the host phases and the boundary costs are
added around that same region.

```bash
./build/pimid --method exec --scope system \
    --config examples/cosim/host_device_basic.yaml \
    --workload benchmarks/pim_kernels/gemv/gemv_omp --size 512 \
    --workload-type openmp
```

> **Power + energy (1.7.6).** Co-sim runs now emit `total_power_W` and
> `total_energy_nJ` alongside cycles, exactly like the device-only figures --
> the host OoO McPAT power path is fixed (a missing `fp_issue_width` XML param
> had left the FP issue queue with zero ports, crashing power analysis; earlier
> 1.7.x co-sim had to run `--no-power`, cycles-only). Power is on by default;
> `--no-power` remains available for fast timing-only iteration.

## System model

- **Host.** The co-sim host is a **single OoO core** at 2 GHz (crossbar
  degenerates to core + MC + bridge ports). Under **OMP** offload the host
  busy-waits on the device fence during the kernel (decision 1). Under
  **MPI** the finalized model (1.8.7) is different: the MPI ranks **are** the
  device PEs, so the host is not a busy-waiting rank-0 master -- it does the
  per-rank prep before the window and the final readback after it (see
  [Co-sim MPI](#co-sim-mpi-the-per-rank-device-window)). The default (and the
  fig5 cells) is a single OoO core; an **in-order host core is also supported**
  as of 1.7.7 (the recorder `memRespCycle` skew fix -- see
  [Limitations](#limitations)).
- **Device = PIM.** PEs (default `alu_core`) placed at a memory level (BANK,
  SUBARRAY, ...) behind the device's own memory technology and NoC.
- **Two fabrics, one bridge.** The device-internal H-tree (JEDEC-derived) and
  the host crossbar are separate; the host<->device **bridge** joins them and
  carries only boundary traffic (launch cmd/ack, coherence flush, Case-2 DMA).
  See [architecture.md](architecture.md) and [network.md](network.md).

### `scope: system` vs `scope: device`

| | `scope: device` | `scope: system` (co-sim) |
|---|---|---|
| host | none (a hidden host prepared the layout, uncounted by design) | real OoO host: cores, caches, own main memory |
| measured region | ROI (kernel) only | task region: flush/launch/kernel/busy-wait/readback |
| boundary costs | none | launch + coherence + bridge, all explicit |
| device cycles | reference | **identical** (parity invariant) |

**Parity invariant** (enforced by validation): the device-side cycles of a
co-sim run agree with a standalone device-scope run of the same kernel. If
they ever diverge, a second device model has crept in -- file a bug.

## The measurement window (task region)

Co-sim vs baseline compares the **task region**, not device-kernel-only and
not the whole process. The window opens when "inputs are ready in host
memory" and closes when "results are visible in host memory":

- **Co-sim** opens BEFORE the first boundary action; the flush/invalidate,
  launch cmd/ack, kernel execution, host busy-wait (OMP offload), and readback
  all sit INSIDE it. At `roi_begin` the plugin snapshots each core's baseline
  (`markRoiBegin`) so the reported cycles cover only the task region, not the
  pre-ROI array-init/setup. Under **MPI** the window is per-rank and the
  device-resident collective tail replaces the single host busy-wait -- see
  [Co-sim MPI](#co-sim-mpi-the-per-rank-device-window).
- **Baseline** (see [NO_OFFLOAD](#no_offload-baseline)) wraps the equivalent
  host compute, including MPI communication.

Both **exclude** process startup, input generation, warmup, and teardown --
identical shared work that would only dilute ratios toward 1. A kernel-only
window would omit exactly the boundary costs the design charges (flattering
PIM); a whole-process window would bury the signal.

## The six boundary decisions

The 1.7.x design settled six subsystems. All are realized as deterministic,
config-driven charges emitted into the ZSim config and applied by the plugin.

### 1. Launch -- doorbell + cmd/ack + busy-wait (`system.launch`)

Offload launch models a **user-mode doorbell** (no syscall) plus a
runtime/OS software dispatch, then a command packet across the bridge and an
acknowledgement back. It is charged on the **host core at the offload
doorbell** (`roi_begin` / `WORK_BEGIN`), before the device migration, so it
lands on the host timeline inside the task window:

```
launch_cost = doorbell_ns + dispatch_ns
            + bridge_crossing(cmd_bytes) + bridge_crossing(ack_bytes)
```

Defaults: `doorbell_ns: 300`, `dispatch_ns: 5000` (~5 us), `cmd_bytes: 64`,
`ack_bytes: 64` -- the low end of the famous ~5-20 us GPU kernel-launch band
(driver + runtime dispatch). DDR5 vs HBM3 differ **only** in the bridge
component; doorbell and dispatch are shared.

Completion is **busy-wait only** on the device-written fence (CUDA-default:
no IRQ mode, no polling knob). In the single-launcher (OMP / serial) offload
this is honest by construction -- the single launcher thread is either on the
host or on the device PE, never two places at once and never racing ahead to
do other host work during the device compute (there is none). The host is
therefore self-penalized for the wait exactly as a CUDA busy-wait would be.

Under **co-sim MPI** there is no single launcher: every rank charges its own
flush + launch on its host core at its kernel-entry barrier and migrates
**itself** into its device PE (the per-rank device window, 1.8.4/1.8.5). See
[Co-sim MPI](#co-sim-mpi-the-per-rank-device-window).

### 2. Coherence -- address-space visibility (`system.coherence`)

Coherence follows **address-space visibility**, not physical placement.

- **Case 1 -- `mode: unified`** (default; our HBM3/DDR5 figure: PEs
  dereference host pointers). Before the device reads DRAM the host must make
  its caches consistent. At `roi_begin`, on the host core, all as one charge:
  `flush(inputs)` writes back dirty lines (real writeback traffic) and
  `invalidate(outputs)` discards stale output lines (placed at BEGIN to kill
  the mid-kernel stale-eviction hazard). Since 1.11.62 the cost is MEASURED,
  per arriving rank: each participating rank performs its own walk over the
  host cache registry, cleaning dirty lines (M -> E) and charging exactly the
  DELTA dirtied since the previous walk, plus the per-rank fixed cost
  (each core issues its own flush instruction and stalls -- ruling 5a):

  ```
  flush_cycles(rank k) = flush_fixed_ns + ceil(delta_k / writeback_bytes_per_cycle)
  sum over ranks of delta_k = every dirty episode, charged exactly once
  ```

  There is no configured footprint: the size is measured from the caches at
  each arrival (a configured `footprint_bytes` is refused at config time --
  it could only abort the run it claimed to configure). History: 1.11.40
  replaced the configured upper bound with a measurement; 1.11.57 de-duplicated
  it across ranks via a shared epoch; 1.11.62 replaced the epoch with
  per-arrival delta walks so late-dirtied data is charged by the next walker
  rather than lost. `PIMID_FLUSH_TRACE=1` prints each walk (rank, delta,
  cumulative).

  `roi_end` charges nothing explicit: host output reads are natural cold
  misses, already priced by the cache model.

- **Case 2 -- `mode: separate`** (even if the same physical DRAM). Cache
  bypass (uncached / WC window or DMA staging); the cost lives in the transfer
  path. No flush is charged -- the two-layer bridge bulk-DMA path already
  prices the crossing. A named configuration, parked for the discrete-device
  (Mode-2) studies.

Baselines **never** flush (the host keeps using its caches), so this cost is
PIM-side only and honesty demands it. Defaults: `flush_fixed_ns: 200`,
`footprint_bytes: 16777216` (16 MiB), `writeback_bw_gbs` auto = host memory
aggregate bandwidth (per-channel x channels).

### 3. Host network -- crossbar, same fidelity ladder as the device

The host fabric is a first-class subsystem with the same two-tier ladder as
the device (analytical | detailed) and identical config syntax
(`system.hosts[].noc`). Default topology is **crossbar** (uniform one-hop;
contention at ports, not hops).

- A **1-core host has no fabric** -- a crossbar degenerates at a single core
  (core -> caches -> MC direct). The core-to-memory path is the calibrated
  analytic memory latency (see [cosim_calibration.md](cosim_calibration.md)).
- A **multi-core host** (the 4/16-core baselines) adds a fixed one-hop
  crossbar latency (`hop_cycles`, core clock) on the host memory path; port
  contention is already priced by the host MC M/D/1 model, so the fabric stays
  analytic.

> **Shipped-behavior note.** Only the **analytical** host tier is wired in
> 1.7.4: `system.hosts[].noc.model` is parsed and echoed, but a detailed host
> Garnet instance is *not* instantiated (the second-Garnet host tier is a
> later 1.7.x increment). `model: detailed` on a host is currently inert.

### 4. Bridge -- two-layer protocol x phy (`system.bridge`)

The host<->device link is a **two-layer** model: `protocol`
(native | ddr_t | cxl_mem | loadstore) selects per-transaction overhead
terms; `phy` (on_die | pcb | interposer | serdes) selects width / rate / wire
+ command-interface pipeline latency. A crossing costs:

```
bridge_cycles = phy_latency + protocol_overhead + ceil(bytes / aggregate_bytes_per_cycle)
```

where aggregate bandwidth = per-channel GB/s x channels. All fields are
optional and default from the **device** memory technology (the bridge always
keys off the device tech). Illegal combinations are config errors:

- `cxl_mem` requires `serdes`
- `loadstore` requires `on_die`
- `native` is forbidden on `serdes` (native needs the tech's own MC class)

Per-tech defaults (BW reuses the simulator's channel rates):

| tech | protocol | phy | BW/ch | latency | ch | uncached |
|---|---|---|---|---|---|---|
| DDR3 | native | pcb | 12.8 GB/s | 20 ns | 1 | 200 ns |
| DDR4 | native | pcb | 19.2 GB/s | 20 ns | 1 | 200 ns |
| DDR5 | native | pcb | 25.6 GB/s | 18 ns | 1 | 200 ns |
| LPDDR5 | native | pcb | 12.8 GB/s | 12 ns | 1 | 200 ns |
| GDDR6 | native | pcb | 32 GB/s | 10 ns | 2 | 200 ns |
| HBM2 | native | interposer | 38.4 GB/s | 30 ns | 8 | 230 ns |
| HBM3 | native | interposer | 51.2 GB/s | 30 ns | 16 | 335 ns |
| SRAM | loadstore | on_die | 128 GB/s | 2 ns | 1 | 30 ns |
| STT_MRAM | loadstore | on_die | 128 GB/s | 2 ns | 1 | 30 ns |
| ReRAM | ddr_t | pcb | 25.6 GB/s | 18 ns | 1 | 250 ns |
| PCM | cxl_mem | serdes | 64 GB/s | 200 ns | 1 | 300 ns |

The interposer latency is **wire + command-interface/MC pipeline** (~5-6 ns
wire + ~24-25 ns pipeline), NOT a SerDes tail -- HBM has no SerDes. The NVM
trio (SRAM/STT on_die, ReRAM pcb, PCM serdes) deliberately spans the attach
ladder; overriding to a common attach isolates the media alone.

### 5. Memory topology -- `is_default_mem` + `host.mem`

Each attached device carries `is_default_mem`:

- **`true`** (default) -- the PIM device **is** the host's main memory; host
  tech = device tech by construction (preserves the host-tech = device-tech
  matrix). Ordinary host traffic lands in it. A `host.mem` block here is
  redundant and ignored with a warning.
- **`false`** -- the device is accelerator-side memory only; the host **must**
  supply a `host.mem` block (technology + optional capacity/BW/channels), a
  plain non-PIM memory of any of the 11 techs. Omission is a **config error**
  (no silent DDR5 fallback). Host-memory tech then becomes an explicit
  variable, and only true host<->device traffic crosses the bridge.

The two are orthogonal to the coherence Case 1/2 knob (natural pairings
true<->Case 1, false<->Case 2, but independent -- CXL can expose unified
addressing over non-default memory). **Baselines always run on the device's
tech** regardless (no host-tech confound -- the retired v140 mistake).

**Array energy of a shared memory (`true`, 1.11.89).** The one array is
charged once, but its accesses are priced by origin. PE-originated accesses
use the PE placement's DQ answer (on-die placements pay no termination) and
the row-buffer miss fraction MEASURED by the PE memory interface.
Host-originated accesses and the coherence-flush writebacks always pay DQ
termination (a host access drives the DQ pins; HBM's termination is zero by
citation, JESD238B cl. 9.1) and, having no row counters, use the stated 0.5
activate/precharge fallback. The background descends by the array's measured
idle residency with or without `memory.power_down` (that flag only adds the
IDD2P step), exactly as in device scope; on a shared array the residency
counts device-MC and host-MC activity (the union is printed as a band and its
least-idle end used), and the device-MC gap histogram is lowered by the most
the host's accesses could take from it. A decoupled array uses its own
controller's counter only.

### 6. Host memory pricing -- calibrated host-path adder

Host main-memory idle latency is a **physical composition**
(tRCD + tCAS from the memory model) **plus a calibrated host-path adder** so
the effective latency matches measured real sockets (DDR5 ~110 ns, HBM3
~235 ns). The adder is host-role only -- device (PE) pricing never routes
through it, so PIM-side timing stays clean. It ships both as an aggregate
(`memory.latency_adder_ns`) and as an optional four-way decomposition
(`memory.host_path { fabric_ns, coherence_ns, mc_pipeline_ns, phy_ns }`).
Host memory bandwidth is a single aggregate M/D/1 queue at
per-channel x channels GB/s, so one DDR5 channel saturates under many host
cores while HBM3's 32x-wider aggregate stays unsaturated. Full calibration
methodology and anchors: [cosim_calibration.md](cosim_calibration.md).

## NO_OFFLOAD baseline

`PIMID_COSIM_NO_OFFLOAD=1` turns the ROI/WORK magic ops into **stat markers
only**: no device migration, no bridge/coherence/launch charge, no
offload-driven device pricing. The unmodified OMP/MPI kernel runs on the host
cores end to end against the host memory technology, from the **same binary**
as the offload run. ROI begin/end still delimit the measured task region.
On the power side (1.11.89) the declared device node and both ends of the
link are **not priced** -- nothing ran on the device and nothing crossed the
link -- so System Total power and area are the host plus the memory array,
which stays priced because the host uses it. The report says so in one line
(`[power] NO_OFFLOAD baseline: device node ... NOT priced`). The mode is read
from the environment at power time, so a re-derivation of a baseline's power
must run with the variable set too.
This yields the host-only baseline cells (1/4/16 OoO host cores). See
[examples.md](examples.md) for the `baseline_host_{1,4,16}core.yaml` configs
and [benchmarks.md](benchmarks.md) for the experiment shape.

**Host parallelism (fixed 1.8.0).** The baseline parallelises across the
configured host cores for **both** programming models: OpenMP sizes its team to
`host.num_cores`, and MPI runs `host.num_cores` ranks as guest threads of one
process (thread-MPI, the same emulation the device scope uses). Before 1.8.0
this was broken two ways in system scope, both silently and with exit 0:

- Thread-MPI was never wired outside device scope (no `libpimid_mpi.so`
  preload, no `PIMID_MPI_RANKS`), so the guest resolved MPI against the system
  MPI runtime and ran as a **single rank** -- host baselines *and* co-sim.
- `host.num_cores` was read only from a top-level `host:` block, never from
  `system.hosts[]`, so it stayed at its default and capped OMP threads, MPI
  ranks, and the devorg `--pes` injection.

System-scope MPI results produced before 1.8.0 are single-rank and must be
regenerated. Device-scope sweeps were never affected.

## Limitations

- **In-order host supported (fixed 1.7.7).** Both `ooo_core` and
  `in_order_core` now work as the co-sim host. Earlier 1.7.x required an OoO
  host: the in-order pipeline's CoreRecorder event chain asserts
  `startCycle >= prevRespCycle`, but `CoreRecorder::cSimEnd` folds a contention
  skew into the phase-1 clock while InOrderCore's private `memRespCycle` cursor
  never received it, so an in-order host on the migration path tripped that
  recorder assertion. The 1.7.7 one-line fix advances `memRespCycle` by the
  same skew, so the cursor and the recorded clock stay consistent. (The
  busy-wait / boundary-charge accounting is core-type agnostic.)
- **Single device in 1.7.x.** One PIM device (plus an optional separate
  `host.mem`). Multiple attached memory devices are a later increment; the
  schema already generalizes but the plumbing is not built.
- **Detailed host NoC not wired** (decision 3 note above): the analytical
  host tier is shipped; the detailed host Garnet is a later increment.
- **Device-side weave +-1-phase quantum.** The device-side weave (in-order /
  OOO weave cores) has a residual +-1-phase QUANTUM nondeterminism in how a
  phase boundary lands relative to contention (<=0.4% at production scale).
  This is a device-model effect, independent of the host core type, and is
  deferred to the 1.8 PDES rework -- it is NOT fixed by 1.7.7.
- **Phase-exclusivity assumed.** Host and PEs are temporally disjoint by
  construction in serial offload (host touches memory only outside the ROI,
  PEs only inside), so the only shared cost at the boundary is the flush. This
  is honest for our kernels; streaming/overlapped offload would need explicit
  host<->PE channel contention (future).

## MPI semantics

`libpimid_mpi.so` provides the MPI ABI (Init, Send/Recv, Sendrecv, Bcast,
Reduce/Allreduce, Barrier, Scatter/Gather, Allgather/Allgatherv, Alltoall)
over a POSIX shared-memory mailbox. `MPI_PROC_NULL` send/recv are no-ops per
the standard, so halo-exchange patterns run without deadlock. Collectives are
**timing-faithful, not numerics-faithful**: reduce-style ops transmit all
chunks but do not apply the reduction operator -- workloads needing actual
reduced values should compute them on the root from received chunks.

Under `noc.model: detailed`, all ranks drive ONE logical Garnet network:
message payloads and every rank's memory traffic contend on the same tree.
For the exec method the ranks are core-threads inside one process sharing one
in-process Garnet directly (the shared record-log / consistent-cut path is the
trace method's per-rank-process realization -- see [network.md](network.md) and
its 1.9.0 pricing note). Each rank also occupies its own placement slice of the
device (rank r's address space maps into PE r's region), so rank-local data is
near-data by construction.

## Co-sim MPI: the per-rank device window

Under `scope: system` the co-sim MPI model is **ranks-are-PEs** (finalized
1.8.7): each MPI rank IS a device PE, computes on its PE, and the host is
involved only at the final readback. The measurement window is a **per-rank
device window**, not a single host-driven offload:

- **Window open -- every rank migrates itself (1.8.4/1.8.5).** At its own
  kernel-entry barrier each rank charges its flush + launch on its host core
  (its own program order -- no wall-order race), then migrates **itself** into
  its device PE and computes there. Window open is thus a per-rank event; the
  1.8.4 fix replaced a broken rank-0-only ROI (the defect was four layers deep:
  rank-0-only `roi_begin`; a `ROI_BEGIN` thread-branch early-return skipping the
  offload; a parked-opener wall-order race; and non-thread-safe migration under
  the rank stampede). 1.8.5 reinstated the per-rank flush + launch charges that
  1.8.4 had deferred.
- **Window close -- ZERO migrations (1.8.6/1.8.7).** No rank migrates back to
  the host at the closing barrier. The ranks are device-resident PEs, so the
  post-ROI inter-PE collective tail (closing barrier + Reduce + Finalize)
  **executes and prices ON THE DEVICE**: rank 0 collects on its PE and lands at
  the reduce-completion makespan (the gather is included); ranks `1..N-1` show a
  deterministic short tail. The host's only remaining role is reading back the
  result. Removing the device->host closing legs is also what eliminates the
  1.8.6 exit race by construction (below).
- **`protocolTail` per-PE stat (1.8.7).** An explicit per-PE receipt = final PE
  cycles minus that PE's closing-barrier marker. It is visibility only and never
  alters `cycles`. Emitted **only** in co-sim runs (host + device cores
  present) and stored in a fixed global array, so device-scope `Core` object
  size and `initStats` stay byte-identical (a `Core` field or an unconditional
  stat would shift heap layout and perturb a device PE).

**Counting contract.** Host cores are counted across the task region around the
kernel; device PEs are counted within their per-rank kernel windows (window open
at each rank's kernel-entry barrier through the device-resident collective tail).
The device-side cycles remain identical to a standalone device-scope run (the
parity invariant).

### Fixes folded into the co-sim MPI window (1.8.6/1.8.7)

- **Exit-protocol heap corruption (1.8.6).** The 1.8.4 closing device->host
  migrate-out freed a rank's core slot mid-handler while a peer immediately
  started its contention-sim phase pass; under the MPI serial weave
  (`simulation.parallel` -> serial) the two overlapping handlers iterated
  glibc-heap state unsynchronized and corrupted the process heap (the
  "malloc(): unaligned tcache chunk" / QEMU SIGSEGV / Garnet "No output port for
  vnet" panics right after the last rank drained). Fix: drop the closing
  migrate-out entirely -- the drained rank finishes its short post-ROI protocol
  on its device PE. (The mirror migrate-IN at window open never corrupts.) 1.8.7
  then removed the last residual host legs, including rank 0's `roi_end`
  migration, making the window strictly zero-migration.
- **tid/cid ROI-baseline aliasing (1.8.7, defect #15 class).** `roiRelCycles(tid)`
  read the ROI baseline table by rank id (`tid`) while it is written by **core
  index** (`cid`). In system scope a rank's `cid != tid`, so for `tid` 0..3 the
  slot aliased HOST cores' ~16.5M pre-ROI (MPI_Init-era) clock; `roiRel` then
  clamped to 0 and collapsed the SEND/RECV rendezvous (the reduce root
  accumulated senders' stamps instead of advancing to their max). Fix: index the
  baseline by the rank's **actual** core (`cids[tid]`) so `getCycles()` and the
  baseline share one clock axis; a per-rank cache holds the last valid-cid
  baseline so a transient cid-invalid window cannot flip `roiRel` back to the
  buggy path. Scoped to co-sim (`g_cosim_mode`): device scope keeps the `tid`
  indexing verbatim (correct AND deterministic there; `cids[tid]` would couple
  it to run-to-run PE assignment).
- **Cross-axis invariant guard (1.8.7).** At the recv rendezvous, if a single
  message's advance exceeds the ROI span the clock axes are mismatched -- the
  guard shouts and caps rather than advancing a PE by a garbage delta (the
  defect-13 / 1.7.7 tripwire lineage; it eliminated a rare ~4% `2^32`
  rendezvous overflow that had tripped Garnet's "event too far into the future"
  assert).

### Known residuals (deferred to a later 1.9.x)

- `MPI_TIME` / `MPI_ADVANCE` handlers fall outside the magic-op window that
  `isMpiMagicOp()` checks, so the barrier max-arrival ADVANCE path is currently
  dead code.
- `mpiNocRoiBase` (max over ALL cores) picks up host cores' pre-ROI clocks in
  co-sim (the "idle cores near 0" assumption breaks with host cores present).
- Device-scope per-rank stamp skew from the shared per-core-slot baseline table
  (deterministic, present in all prior data). All three point at one redesign:
  a per-RANK baseline table.

## Diagnostics

| env | prints |
|---|---|
| `PIMID_COSIM_TRACE=1` | timestamped thread-lifecycle events (scheduler start/join, offload migrations) to stderr |
| `PIMID_DEBUG_HOSTMEM` | host memory idle-latency composition (physical + host-path adder, per component) |
| `PIMID_DEBUG_COHERENCE` | the deterministic Case-1 flush charge the plugin will apply at roi_begin |
| `PIMID_DEBUG_LAUNCH` | the deterministic launch charge (doorbell + dispatch + bridge cmd/ack) |

These last three are login-node observable (printed at config emission), so
you can inspect the charges without running the simulator on a compute node.
