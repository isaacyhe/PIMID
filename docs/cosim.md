# Host-Device Co-simulation

Co-simulation (a system with `system.hosts[]`; 1.12.9: no scope key) runs a general-purpose **host** and a
**PIM device** as one system: the host is a real CPU (out-of-order cores,
cache hierarchy, its own main memory), the device is processing elements
inside memory. An offloaded kernel migrates from the host onto the device
PEs and back; the boundary costs -- launch, coherence, and the host<->device
bridge -- are priced explicitly. The device-side execution is the *identical*
code path a standalone device run (no hosts) uses (same PE memory interfaces,
same Garnet/analytical NoC, same JEDEC-based memory timing), so device-only
cycles are unchanged by co-simulation.

A co-simulation system is declared under `system.hosts[]` / `system.devices[]`. The
flat spelling of 1.12.2 to 1.12.7 (`scope: cosim`, a top-level `host:` block
and the device keys at the left margin) is retired: since 1.12.8 a hardware
block at the margin is dropped with a warning, and since 1.12.9 a `scope:`
word is ignored with a warning (see
[yaml_reference.md](yaml_reference.md#host-block-host-scope-cosim----retired-in-1128)).

There are **no special co-simulation workloads**: any ordinary ROI-marked workload
runs in co-simulation unchanged. In a standalone device simulation only the ROI
(the kernel) is counted; in co-simulation the host phases and the boundary costs are
added around that same region.

```bash
./build/pimid --method exec \
    --config examples/cosim/host_device_basic.yaml \
    --workload benchmarks/pim_kernels/gemv/gemv_omp --size 512 \
    --workload-type openmp
```

> **Power.** A co-simulation reports `total_power_W` and `total_energy_nJ` beside its
> cycles, as a device run does (since 1.7.6). Power is on by default; `--no-power` gives a
> faster timing-only run.

## System model

- **Host.** The host of the paper's co-simulation experiment (Fig. 5) is a **single
  OoO core** at 2 GHz (a 1-core host has no fabric: core + MC + bridge ports); the
  `system.hosts[]` defaults are `ooo_core`, 4 cores, 3000 MHz. Under **OMP** offload the host
  busy-waits on the device fence during the kernel (decision 1). Under
  **MPI** the finalized model (1.8.7) is different: the MPI ranks **are** the
  device PEs, so the host is not a busy-waiting rank-0 master -- it does the
  per-rank prep before the window and the final readback after it (see
  [Co-simulation MPI](#co-simulation-mpi-the-per-rank-device-window)). That experiment
  uses a single OoO core; an **in-order host core is also supported** (since 1.7.7; see
  [Limitations](#limitations)).
- **Device = PIM.** PEs (default `alu_core`) placed at a memory level (BANK,
  SUBARRAY, ...) behind the device's own memory technology and NoC. `in_order_core` and
  `ooo_core` elements run as device elements too ((1.12.287)): the device node's cores, whatever
  their model, run the offloaded kernel, and a cache-based element's caches drain to its home
  memory interface through the device network, as in a device run. They count cycles on the
  global clock, so their co-simulation needs the device at the host's clock; a run with two
  clocks is refused (compute-unit elements take any clock).
- **Two fabrics, one bridge.** The device-internal H-tree (JEDEC-based) and
  the host crossbar are separate; the host<->device **bridge** joins them and
  carries only boundary traffic (launch cmd/ack, coherence flush, Case-2 DMA).
  See [architecture.md](architecture.md) and [network.md](network.md).
- **One process, node workloads.** The system runs as one simulated process.
  `system.hosts[].workload` and `system.devices[].workload` take `binary`,
  `args` and `env`; a node without a `binary` inherits the top-level binary
  and args. A node's `env` map (read since 1.11.106; documented earlier and
  ignored) joins that one process's environment, the top-level
  `workload.env`: nodes are applied in file order, hosts before devices, and a
  name already set with a different value takes the node's value and the run
  prints a NOTE naming both values. An `env` that is not a map of
  `NAME: value` is refused.

### Co-simulation vs device run

| | device run (no hosts) | co-simulation (`system.hosts[]` present) |
|---|---|---|
| host | none (a hidden host prepared the layout, uncounted by design) | real OoO host: cores, caches, own main memory |
| measured region | ROI (kernel) only | task region: flush/launch/kernel/busy-wait/readback |
| boundary costs | none | launch + coherence + bridge, all explicit |
| device cycles | reference | the same device model, not the same experiment (see Parity below) |

Both are systems (1.12.8): a device run declares its one device under
`system.devices[]` and no hosts, a co-simulation adds the `system.hosts[]`
list, and the nodes alone decide (1.12.9: the `scope` key and `--scope` are
retired, dropped with a warning). The flat `scope: cosim` spelling went in 1.12.8.

**Parity.** The device side of a co-simulation is the device model a device run
uses -- the same elements, scratchpad, memory interfaces, network and memory --
and under OpenMP its elements execute the same instructions and make the same
number of memory accesses. It is not the same experiment, so its cycles are not
the device run's:

- the host initialises the data, so by default the elements place it by first
  touch inside the measured region, where a device run's elements claim it while
  initialising, before the region (`system.page_placement`, below);
- the host's dirty lines are flushed to the shared memory as the region opens
  and compete with the elements' first accesses;
- the region includes the kernel launch and the coherence flush, and under
  message passing each rank enters with its own flush and launch, so the ranks
  start staggered; the message-passing window also carries memory accesses a
  device run does not make (about twice the device run's in the cells below).

Measured with 8 `alu_core` elements at BANK, 2 GHz, one in-order host core,
HBM3 and DDR4, on histogram, gemv, stream_triad and bfs, default placement: the
OpenMP element busy cycles of a co-simulation lie 8% to 84% above the device
run's, and the message-passing kernel windows (`roiCycles`, the window without
the post-ROI collective tail) between 16% below and 209% above. A larger
difference, or, under OpenMP, a difference in the elements' instruction counts
or scratchpad accesses, means a second device model has crept in -- file a bug.

**Data placement (`system.page_placement`).** `first_touch` (the default) places
a page the host initialised at the first element that touches it inside the
region; until then the host's own accesses see it interleaved over the device's
units, page by page. `standalone` places the data as a device run does: every
page a host thread touches before the region is claimed by the element that
thread will run on (thread i on element i modulo the element count), so the
elements' first touches inside the region (`ftPages`) are those of the device
run. Placement is then the device run's, but the time is not: in the
measurements above bfs comes within 1% (DDR4) to 12% (HBM3) of the device run,
histogram, gemv and stream_triad stay where they were or move further away
(the flushed dirty lines and, under message passing, the window's extra
accesses now land on the units the elements read), up to 10x on the
message-passing histogram. Compute-unit (`alu_core`) elements only.

**Clocks.** Every nanosecond-to-cycle conversion uses the clock of the node
that ticks the cycles: the host's caches and memory path at the host node's
clock, the device's hierarchy ladder (every rung, the `HOST_MC` rung included
since 1.12.5) at the device node's. The top-level `system.frequency_mhz` is
only the run's reference clock (in the flat form retired in 1.12.8 the
top-level clock was the device's).

**Memory speed.** The device node's `memory.dram.ddr5_speed_grade` is the run's
DRAM part. A memory declared beside the device (`system.memories[]`, a memory
without PEs) is the host's own main memory (1.12.10): its `memory.technology`
and `memory.dram.ddr5_speed_grade` set the host memory's idle latency,
per-channel bandwidth and array energy, so the host memory and the PIM device
may run at different speeds; the device's figures do not move. One host, one
device and one host memory in this build; `system.hosts[].mem` is retired.

**Power.** In a co-simulation `--method exec` and `--method trace` price the
nodes the same way, each from its own fields (the per-node path; trace since
1.12.5).

## The measurement window (task region)

Co-simulation vs baseline compares the **task region**, not device-kernel-only and
not the whole process. The window opens when "inputs are ready in host
memory" and closes when "results are visible in host memory":

- **Co-simulation** opens before the first boundary action; the flush/invalidate,
  launch cmd/ack, kernel execution, host busy-wait (OMP offload), and readback
  all sit inside it. At `roi_begin` the plugin snapshots each core's baseline
  (`markRoiBegin`) so the reported cycles cover only the task region, not the
  pre-ROI array-init/setup. Under **MPI** the window is per-rank and the
  device-resident collective tail replaces the single host busy-wait -- see
  [Co-simulation MPI](#co-simulation-mpi-the-per-rank-device-window).
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

In the single-launcher offload the device's time axis starts at the doorbell, on
the launcher's host-core clock. The flush and launch accesses the launcher waits
for are priced by the closed-loop DRAM replay after the doorbell. When the replay
finds them slower than charged (the core's DRAM balance), or one of them completes
past the doorbell, the doorbell is late by that amount, and so are the start and
the end of every device PE: the run time takes the move, together with the device
PEs' own balances, and the run prints it (`[roi] the DRAM balance of doorbell host
core ... moves the device's start by ... cycles`).

Under **co-simulation MPI** there is no single launcher: every rank charges its own
flush + launch on its host core at its kernel-entry barrier and migrates
**itself** into its device PE (the per-rank device window, 1.8.4/1.8.5). See
[Co-simulation MPI](#co-simulation-mpi-the-per-rank-device-window).

A rank's device kernel starts no earlier than its launch completes. The launch
completes at the host core's ROI-relative clock once the rank's flush and
launch charges are retired; the rank's PE waits until that point, converted to
the PE's clock (the wait is waiting, not activity, and is a sync point of the
DRAM replay). Ranks that share a host core launch one after another, so with
fewer host cores than ranks the last rank starts after the whole launch train:
16 ranks on one 2 GHz host core start up to about 83 us apart. A launch that
completes before the ROI baseline is outside the region and costs the device
nothing. The run time, the DRAM replay and the priced window therefore cover
the launch train together.

### 2. Coherence -- address-space visibility (`system.coherence`)

Coherence follows **address-space visibility**, not physical placement.

- **Case 1 -- `mode: unified`** (default; PEs dereference host pointers,
  as in the paper's HBM3/DDR5 experiment). Before the device reads DRAM the host must make
  its caches consistent. At `roi_begin`, on the host core, all as one charge:
  `flush(inputs)` writes back dirty lines (real writeback traffic) and
  `invalidate(outputs)` discards stale output lines (placed at BEGIN to kill
  the mid-kernel stale-eviction hazard). Since 1.11.62 the cost is measured,
  per arriving rank: each participating rank performs its own walk over the
  host cache registry, cleaning dirty lines (M -> E) and charging exactly the
  delta dirtied since the previous walk, plus the per-rank fixed cost
  (each core issues its own flush instruction and stalls):

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

Baselines **never** flush (the host keeps using its caches), so this cost falls
on the PIM side only. Defaults: `flush_fixed_ns: 200`,
`writeback_bw_gbs` auto = host memory aggregate bandwidth (per-channel x
channels). The footprint has no default: it is measured, and a configured
`footprint_bytes` is refused (above).

### 3. Host network -- the analytic host fabric

The host fabric is configured under `system.hosts[].noc`. It is analytic, one
of three topologies (1.12.23): the **ring** (default; the published
Nehalem-EX ring, one stop per core and per host memory controller, 1 cycle
per stop), the **crossbar** (uniform one hop, `hop_cycles`) or a **bus**; any
other topology word is refused (rc 1).

- A **1-core host has no fabric** -- a crossbar degenerates at a single core
  (core -> caches -> MC direct). The core-to-memory path is the calibrated
  analytic memory latency (see [cosim_calibration.md](cosim_calibration.md)).
- A **multi-core host** (the 4/16-core baselines) adds the fabric's latency
  on the host memory path (ring: the stops crossed; crossbar: `hop_cycles`,
  core clock); port
  contention is already priced by the host memory controller (the replayed
  Ramulator2 controller since 1.12.18, see section 6; the M/D/1 queue when the
  replay is off), so the fabric stays analytic.

> **Shipped behavior (1.12.23).** No host Garnet is instantiated: a
> `noc.model` other than `analytical` (`detailed` included) is accepted with a
> warning and runs the analytical model of the chosen topology. The fabric is
> priced in McPAT as the topology the timing ran, from the data messages and
> hops it measured (`power.models.host_noc`, 1.12.19; see
> [power.md](power.md)). Until 1.12.18 a host NoC had no power model, and
> until 1.12.22 every host fabric ran as the crossbar.

### 3b. Host-device link class (`power.link.link_type`, `system.network.links[].type`)

The link class prices the crossing's energy (pJ/bit band per class) and its
controller, and fills the link's timing for the fields the configuration did
not set. Since 1.11.103 the class defaults to the
attachment the device's placement implies: an on-package part (an HBM stack,
SRAM, the NVMs) with `attachment: internal` crosses an `interposer`; a
DDR-family part (DDR3/DDR4/DDR5/LPDDR5/GDDR6) with `attachment: internal` is
reached over its own DRAM channel, the `dram_channel` class -- bandwidth =
channels x channel width x the preset's data rate, base latency = tRCD + tCL,
header 0, no separate link energy (the channel's I/O and termination are in
the DRAM energy model) and no link controller (the host memory controller is
priced as itself); an `external` device crosses `pcie_gen5`. A named class
overrides the default and the run prints a `[link]` note naming both.
`attachment` accepts `internal` or `external` only: any other word is refused
at load (1.11.106; it used to run as `external` and take the PCIe class).
`power.pcie` is the legacy name of the `power.link` block (giving both is
refused); the class list, with `pcie_gen3` added in 1.11.106, is in
[yaml_reference.md](yaml_reference.md#link-types-co-simulation).

**The interposer link is a UCIe link (1.12.18).** Its energy is computed inside
the McPAT fork: measured crossing bytes x 8 x (analog PHY + digital interface)
pJ/bit from the row of the die's process node -- 22 nm (default: 0.669 + 0.192,
both computed -- from Intel ISSCC 2026 8.1 and Cadence JSSC 2026, and from
Microsoft ISSCC 2026 8.3 scaled by CACTI and Stillmaker & Baas 2017) or 3 nm (`power.link.process_node_nm: 3`: 0.29 + 0.10, measured);
any other node is refused. Sources and derivations in
[power.md](power.md#the-host-device-link-ucie). Its controller clock defaults to the D2D adapter clock, 64 lanes x
16 GT/s / (8 x 64 B FDI) = 2000 MHz, with the 64 B FDI width from UCIe Consortium, "UCIe Protocol", Hot Chips 2023 UCIe Tutorial ("All examples show 64B data width on FDI"; "Protocol Layer presents 64B of the Flit on FDI");
`power.link.controller_clock_mhz` overrides it for any class. The `[xing]` line
then says that the pJ/bit is applied in McPAT, and McPAT's `[link] UCIe` lines
print the row, its derivation, bytes, pJ/bit, joules and watts, and the
McPAT-controller cross-check. `power.models.ucie_link: false` runs the
earlier path. See [power.md](power.md#the-host-device-link-ucie).

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
| LPDDR5 | native | pcb | 12.8 GB/s | 12 ns | 4 | 200 ns |
| GDDR6 | native | pcb | 32 GB/s | 10 ns | 2 | 200 ns |
| HBM2 | native | interposer | 38.4 GB/s | 30 ns | 8 | 230 ns |
| HBM3 | native | interposer | 51.2 GB/s | 30 ns | 16 | 335 ns |
| SRAM | loadstore | on_die | 128 GB/s | 2 ns | 1 | 30 ns |
| STT_MRAM | loadstore | on_die | 128 GB/s | 2 ns | 1 | 30 ns |
| ReRAM | ddr_t | pcb | 25.6 GB/s | 18 ns | 1 | 250 ns |
| PCM | cxl_mem | serdes | 64 GB/s | 200 ns | 1 | 300 ns |

The interposer latency is **wire + command-interface/MC pipeline** (~5-6 ns
wire + ~24-25 ns pipeline), not a SerDes tail -- HBM has no SerDes. The NVM
trio (SRAM/STT on_die, ReRAM pcb, PCM serdes) deliberately spans the attach
ladder; overriding to a common attach isolates the media alone.

### 5. Memory topology -- the device as main memory, or a separate host memory

The node lists decide whether the PIM device is the host's main memory (1.12.10):

- **Hosts and a device** -- the device **is** the host's main memory
  (`is_default_mem` true, the default); host tech = device tech by construction
  (preserving the host-tech = device-tech matrix), and ordinary host traffic lands
  in it.
- **Hosts, a memory and a device** -- the `system.memories[]` entry is the host's
  own main memory, a plain non-PIM memory of any of the 11 technologies with its own
  `memory.technology` and `memory.dram.ddr5_speed_grade`; the device is
  accelerator-side memory only (`is_default_mem` derived false; a written
  `is_default_mem: true` is ignored with a warning). Host-memory technology then
  becomes an explicit variable, and only true host<->device traffic crosses the
  bridge. A device with `is_default_mem: false` and no memory entry is refused.

The `host.mem` / `system.hosts[].mem` block that named the separate host memory from
1.7.4 to 1.12.9 is retired: it is dropped with a warning.

The two are orthogonal to the coherence Case 1/2 knob (natural pairings
true<->Case 1, false<->Case 2, but independent -- CXL can expose unified
addressing over non-default memory). **Baselines always run on the device's
tech** regardless (no host-tech confound).

**Array energy of a shared memory (`true`, 1.11.89).** The one array is
charged once, but its accesses are priced by origin. PE-originated accesses
use the PE placement's DQ answer (on-die placements pay no termination) and
the row-buffer miss fraction measured by the PE memory interface.
Host-originated accesses and the coherence-flush writebacks always pay DQ
termination (a host access drives the DQ pins; HBM's termination is zero by
citation, JESD238B cl. 9.1) and, having no row counters, use the stated 0.5
activate/precharge fallback. The background descends by the array's measured
idle residency with or without `memory.power_down` (that flag only adds the
IDD2P step), exactly as in a device run; on a shared array the residency
counts device-MC and host-MC activity (the union is printed as a band and its
least-idle end used), and the device-MC gap histogram is lowered by the most
the host's accesses could take from it. A decoupled array uses its own
controller's counter only.

**IDD3N share, IDDQ and IPP (1.11.91).** The share of background priced at
active standby (IDD3N) is the measured bank-open fraction -- Ramulator2's
`bankOpenCycles / bankOpenWindow` (memory cycles with >= 1 bank open, the
active state of the IDD method) -- when the controller that saw all of the array's
accesses exported it; the idle residency above is then the power-down input
only. The PE memory interface keeps no bank state, so a shared array with
PE-originated traffic, every decoupled device array and every device
run print `bank-open fraction UNMEASURED (...)` and keep the traffic-phase
share (1 - idle residency). Accesses that cross the DQ pins (the termination
rule) also pay the DQ output rail, printed `iddq=` on the DQ interface line;
the VPP standby is printed `+ipp=` on the Background line and counted in the
system total.

### 6. Host memory pricing -- calibrated host-path adder

Host main-memory idle latency is a **physical composition**
(tRCD + tCAS from the memory model) **plus a calibrated host-path adder** so
the effective latency matches measured real sockets (DDR5 ~110 ns, HBM3
~235 ns). The adder is host-role only -- device (PE) pricing never routes
through it, so PIM-side timing stays clean. It ships both as an aggregate
(`memory.latency_adder_ns`) and as an optional four-way decomposition
(`memory.host_path { fabric_ns, coherence_ns, mc_pipeline_ns, phy_ns }`).
Full calibration methodology and anchors:
[cosim_calibration.md](cosim_calibration.md).

**The host memory is replayed through Ramulator2 (1.12.18).** Like
the device memory, every host access is recorded per epoch and replayed through
a Ramulator2 controller at the epoch boundary; the host access is priced at the
previous epoch's measured service latency of its interface (bank, row hit /
miss / conflict, queueing at the bank, refresh, turnaround, the burst) **plus
the host-path adder**, which holds no DRAM timing, so tRCD + tCAS is not
counted twice. The aggregate M/D/1 queue (tRCD + tCAS + adder + queue at
per-channel x channels GB/s) prices only the bootstrap, the accesses before
the first replayed epoch. Where the device is the host's memory
(`is_default_mem`, the default), the host's interface joins the device's
replay instance (numbered after the device interfaces, Ramulator source_id 1),
so the shared array has one controller and one refresh schedule, and a host
access lands on the bank the device's own placement gives that line. A
decoupled host memory (`system.memories[]`) gets its own replay instance of
its technology, with its channel count, one rank and Ramulator's own address
mapper. The controller's row hits / misses / conflicts and activates are
split by origin, so the host's share is measured. Message passing (thread-MPI)
uses the same cut mode as the device replay. The run prints
`[mem] host DRAM controller: ...` and, with `PIMID_DEBUG_REPLAY`, the host
share (`[replaydbg] ... host share`). `system.hosts[].memory.dram_replay:
false` restores the M/D/1 host controller and the line says so; it changes
timing (the latency of every host access) as well as the energy inputs. The
replay is off, and says why, for a non-DRAM host memory, for trace-method MPI,
and on a shared array whose device memory is not replayed.

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
from the environment at power time, so a recomputation of a baseline's power
must run with the variable set too.
When the host memory is the device's (the shared array), the baseline's memory
replay runs on the host clock: the host cores are its only requesters and stamp
their records on that clock. It used to run on the device elements' clock,
which spread the host's requests over a DRAM timeline f_host / f_dev times too
long and raised the run time by that factor; a baseline whose host and device
clocks are equal was not affected (changelog, 1.12.272). An offload
run keeps the device clock; when the device elements run on another clock than
the host cores, they advance phase length x f_dev / f_host of their own cycles
per phase (fewer on a slower device, more on a faster one), so a phase is the
same stretch of time on both sides, and the host's stamps join the device's axis
scaled by f_dev / f_host: the host's requests reach the shared array at their own
rate (changelog, 1.12.277). The global clock is the host's (it replaces
sys.frequency when a faster device set that), what a host core spends is converted
at the host's clock, and the device's epochs span at least one device phase. A
co-simulation runs alu_core and null_core device elements at any clock, and
in_order_core and ooo_core device elements at the host's clock ((1.12.287)); one whose
device runs those at another clock stops at configuration.
This yields the host-only baseline cells (1/4/16 OoO host cores). See
[examples.md](examples.md) for the `baseline_host_{1,4,16}core.yaml` configs
and [benchmarks.md](benchmarks.md) for the experiment shape.

**Host parallelism.** The baseline parallelises across the configured host cores for
**both** programming models: OpenMP sizes its team to `system.hosts[].num_cores`, and
MPI runs `system.hosts[].num_cores` ranks as guest threads of one process
(thread-MPI, the same emulation a device run uses). Before 1.8.0 a co-simulation
or host baseline ran MPI as a single rank and capped its cores silently (exit 0);
message-passing results of a co-simulation or host baseline from before 1.8.0 are
single-rank and must be regenerated. Device runs were never affected (changelog,
1.8.0).

## Limitations

- **In-order host supported (since 1.7.7).** Both `ooo_core` and `in_order_core`
  work as the co-simulation host; the busy-wait and boundary-charge accounting is
  core-type agnostic. (Before 1.7.7 an in-order host tripped the recorder's
  `startCycle >= prevRespCycle` assertion on the migration path; changelog, 1.7.7.)
- **One host, one device, at most one host memory (1.12).** A co-simulation
  is one host and one PIM device, optionally with the host's own main memory
  as one `system.memories[]` entry (1.12.10). Multi-node systems -- several
  hosts, devices or memories, a host-only system, a node reaching another
  node's memory beyond the 1.12 wiring -- are not supported and are refused
  at load. The address keys (`address`, `allowed_address_ranges`, 1.12.12)
  are accepted and checked. See the README, "What 1.12 simulates".
- **Detailed host NoC not wired** (decision 3 note above): the host fabric is
  analytic (`ring`, `crossbar` or `bus`); a host `noc.model` other than
  `analytical` warns and runs the analytical model.
- **Device-side weave phase quantum.** With in-order / OOO weave cores on
  the device, a phase boundary lands within one phase of the contention it
  ends (<=0.4% at production scale). This is a device-model effect,
  independent of the host core type.
- **Phase-exclusivity assumed.** Host and PEs are temporally disjoint by
  construction in serial offload (host touches memory only outside the ROI,
  PEs only inside), so the only shared cost at the boundary is the flush. This
  holds for the shipped kernels; streaming/overlapped offload would need explicit
  host<->PE channel contention (future).

## MPI semantics

`libpimid_mpi.so` provides the MPI ABI (Init, Send/Recv, Sendrecv, Bcast,
Reduce/Allreduce, Barrier, Scatter/Gather, Allgather/Allgatherv, Alltoall)
over a POSIX shared-memory mailbox. `MPI_PROC_NULL` send/recv are no-ops per
the standard, so halo-exchange patterns run without deadlock. Collectives run
MPICH's algorithms, chosen as MPICH's intra-communicator selection does with its
default thresholds (binomial, scatter-allgather, recursive doubling, Bruck,
ring, Rabenseifner, pairwise; Thakur, Rabenseifner and Gropp, IJHPCA 2005), and
reduce-style ops apply the builtin reduction operators. The shim's own transport
memory (mailboxes, staging) is not priced: a message costs the sender's read of
its buffer, the Garnet transfer and the receiver's write (1.12.17). The receiver
writes its buffer after the message has arrived, and no mailbox lock is held
across priced work (1.12.177). A send never
waits for the receiver; messages the receiver has not taken yet wait in its
staging, which grows as needed, so how fast the host runs a rank's thread
neither stops a run nor changes its results (since 1.12.196).

Under `noc.model: detailed`, all ranks drive one logical Garnet network:
message payloads and every rank's memory traffic contend on the same tree.
For the exec method the ranks are core-threads inside one process sharing one
in-process Garnet directly (the shared record-log / consistent-cut path is the
trace method's per-rank-process realization -- see [network.md](network.md) and
its 1.9.0 pricing note). Each rank also occupies its own placement slice of the
device (rank r's address space maps into PE r's region), so rank-local data is
near-data by construction.

## Co-simulation MPI: the per-rank device window

In a co-simulation the MPI model is **ranks-are-PEs** (finalized in
1.8.7): each MPI rank is a device PE, computes on its PE, and the host is
involved only at the final readback. The measurement window is a **per-rank
device window**, not a single host-driven offload:

- **Window open -- every rank migrates itself (1.8.4/1.8.5).** At its own
  kernel-entry barrier each rank charges its flush + launch on its host core
  (its own program order -- no wall-order race), then migrates **itself** into
  its device PE and computes there. Window open is thus a per-rank event (1.8.4
  replaced a rank-0-only ROI; 1.8.5 reinstated the per-rank flush and launch
  charges; changelog). No rank is charged the entry barrier's pre-ROI wait: the
  release of the barrier that synthesizes the ROI baseline is discarded by every
  rank alike (1.12.176). A rank that reaches its kernel after rank 0's `roi_end`
  still enters the window at its next block or barrier, charged its own flush and
  launch; the window drains only when every rank has entered and left, and the
  statistics freeze at the last rank's marker (1.12.170).
- **Window close -- zero migrations (1.8.6/1.8.7).** No rank migrates back to
  the host at the closing barrier. The ranks are device-resident PEs, so the
  post-ROI inter-PE collective tail (closing barrier + Reduce + Finalize)
  **executes on the device**: the ranks reduce among their PEs (1.12.17:
  MPICH's binomial or Rabenseifner reduce) and rank 0 holds the result. The host's only remaining role is reading back the result. Removing
  the device->host closing legs is also what eliminates the 1.8.6 exit race by
  construction (below). Since 1.12.3 the tail is reported and **not priced**
  (counting contract below).
- **`protocolTail` and `roiCycles` per-PE stats (1.8.7, 1.12.3).** Two per-PE
  receipts: `protocolTail` = final PE cycles minus that PE's closing-barrier
  marker, and `roiCycles` = the marker itself, the PE's kernel-window cycles
  (`roiCycles + protocolTail == cycles` for every marked rank; `roiCycles` is 0
  for a rank that was never marked). Neither alters `cycles`. Emitted **only**
  in co-simulation runs (host + device cores present) and stored in fixed global
  arrays, so a device run's `Core` object size and `initStats` stay
  byte-identical (a `Core` field or an unconditional stat would shift heap
  layout and perturb a device PE).

**Counting contract.** Host cores are counted across the task region around the
kernel. Device PEs are counted within their per-rank kernel windows: the window
opens where the rank leaves its kernel-entry barrier (after the barrier's own
cost, since 1.12.199) and closes at its closing-barrier marker. The device wall clock is the maximum over PEs of `cycles -
protocolTail`, and energy, the device fabric and the power-gating residency
window are priced over it (1.12.3). The post-kernel collective
tail is outside the priced window: during it the waiting ranks' clocks follow
the global simulation clock, so its length is host thread scheduling, not
modelled device work. On the 1.12.1 and 1.12.2 test cells it was 91 to 98% of
the reported device cycles, and the same HBM3 cell reported 11.8M and 3.1M
device cycles in two runs with an identical 276k-cycle kernel window. Through
1.12.2 the tail was inside the window, so every co-simulation MPI device cycle and
energy figure before 1.12.3 carries it. The run prints one `[cosim] device
kernel window` line naming the trimmed window, the number of marked PEs and
the largest tail. A rank with no closing barrier after its kernel (bfs) is
never marked; its window is the whole run, as before. Co-simulation OpenMP cells and
the no-offload baseline report no tail and are unchanged. The kernel window
is what a co-simulation's message-passing device side is compared with a device
run on (see Parity above).

### Fixes folded into the co-simulation MPI window (1.8.6/1.8.7)

- **Exit-protocol heap corruption (1.8.6).** The 1.8.4 closing device->host
  migrate-out freed a rank's core slot mid-handler while a peer immediately
  started its contention-sim phase pass; under the MPI serial weave
  (`simulation.parallel` -> serial) the two overlapping handlers iterated
  glibc-heap state unsynchronized and corrupted the process heap (the
  "malloc(): unaligned tcache chunk" / QEMU SIGSEGV / Garnet "No output port for
  vnet" panics right after the last rank drained). Fix: drop the closing
  migrate-out entirely -- the drained rank finishes its short post-ROI protocol
  on its device PE. (The mirror migrate-in at window open never corrupts.) 1.8.7
  then removed the last residual host legs, including rank 0's `roi_end`
  migration, making the window strictly zero-migration.
- **tid/cid ROI-baseline aliasing (1.8.7, defect #15 class).** `roiRelCycles(tid)`
  read the ROI baseline table by rank id (`tid`) while it is written by **core
  index** (`cid`). In a co-simulation a rank's `cid != tid`, so for `tid` 0..3 the
  slot aliased the host cores' ~16.5M pre-ROI (MPI_Init-era) clock; `roiRel` then
  clamped to 0 and collapsed the SEND/RECV rendezvous (the reduce root
  accumulated senders' stamps instead of advancing to their max). Fix: index the
  baseline by the rank's **actual** core (`cids[tid]`) so `getCycles()` and the
  baseline share one clock axis; a per-rank cache holds the last valid-cid
  baseline so a transient cid-invalid window cannot flip `roiRel` back to the
  buggy path. Scoped to co-simulation (`g_cosim_mode`): a device run keeps the `tid`
  indexing verbatim (correct there; `cids[tid]` would tie it to the core a rank happens to
  run on).
- **Cross-axis invariant guard (1.8.7).** At the recv rendezvous, if a single
  message's advance exceeds the ROI span the clock axes are mismatched -- the
  guard warns and caps rather than advancing a PE by a garbage delta (the
  defect-13 / 1.7.7 tripwire lineage; it eliminated a rare ~4% `2^32`
  rendezvous overflow that had tripped Garnet's "event too far into the future"
  assert).

### Known limitations of the message-passing window

- `MPI_TIME` / `MPI_ADVANCE` handlers fall outside the magic-op window that
  `isMpiMagicOp()` checks, so the barrier max-arrival ADVANCE path is currently
  dead code.
- `mpiNocRoiBase` (max over all cores) picks up host cores' pre-ROI clocks in
  co-simulation (the "idle cores near 0" assumption breaks with host cores present).
- Device-run per-rank stamp skew from the shared per-core-slot baseline table
  (deterministic, present in all prior data). All three point at one redesign:
  a per-rank baseline table.

## Diagnostics

| env | prints |
|---|---|
| `PIMID_COSIM_TRACE=1` | timestamped thread-lifecycle events (scheduler start/join, offload migrations) to stderr |
| `PIMID_DEBUG_HOSTMEM` | host memory idle-latency composition (physical + host-path adder, per component) |
| `PIMID_DEBUG_COHERENCE` | the deterministic Case-1 flush charge the plugin will apply at roi_begin |
| `PIMID_DEBUG_LAUNCH` | the deterministic launch charge (doorbell + dispatch + bridge cmd/ack) |

These last three are printed at config emission, so the charges can be inspected
without running a simulation.
