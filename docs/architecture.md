# Architecture

![PIMID system architecture](figures/PIMID_arch.png)

*(Regenerate with `python3 figures/gen_pimid_arch.py` -- PDF for the paper,
PNG for this page.)*

PIMID is an execution-driven, cycle-accurate simulator for Processing-in-Memory
architectures. A workload binary runs under QEMU user-mode emulation; a TCG
plugin (`libzsim_qemu.so`) intercepts every instruction and drives ZSim
microarchitectural models (cores, caches, memory controllers, NoC), which are
backed by Ramulator2, CACTI, NVSim, McPAT, and Garnet.

## Simulation flow (exec mode)

```
workload binary
   `- QEMU user-mode (TCG)
        `- libzsim_qemu.so plugin -- every instruction/memory access
             `- ZSim core model (alu / in_order / ooo / null)
                  `- caches -> PE memory interface
                       |- NoC model (analytical | detailed Garnet)
                       `- memory backend (Ramulator2 / CACTI / NVSim)
```

## Simulation methods

| Method | Description | Requires |
|---|---|---|
| `exec` | Execution-driven cycle-accurate (QEMU + ZSim) | plugin-capable `qemu-x86_64`, workload binary |
| `trace-gen` | Record a trace for later replay: every memory access, every executed basic block (address, count, size, domain), the ROI markers, offload brackets and barriers, plus the blocks' instruction bytes in `<trace>.blocks` (1.11.100) | same as exec |
| `trace` | Replay a recorded trace through the models (no QEMU): blocks decoded with the execution path's decoder, each block's terminator classified and its outcome fed to the core's predictors as the execution plugin feeds it, the DRAM controller replay folded at the end as there, every thread joined to its core with the phase ended only when every joined core has crossed it (the scheduler barrier's condition, driven by one host thread), roi_begin rebases every counter and roi_end freezes them, device-domain events run on the device cores, barriers go through the phase barrier (1.11.100) | trace file (+ `.blocks`) |
| `synthetic` | Garnet synthetic traffic injection (no workload at all) | nothing |

## Scopes

The nodes decide (1.12.9; the `scope` key and `--scope` are retired, dropped
with a warning):

- a **device run** -- a system with no hosts: one `system.devices[]` entry, a
  single PIM device with PEs in the memory hierarchy, run on the device path
  ([yaml_reference.md](yaml_reference.md#configuration-shape-1128));
- a **co-simulation** -- a system with `system.hosts[]`: hosts (with caches) +
  devices connected by a system interconnect; supports host->device offload
  ([cosim.md](cosim.md)).

## Two-fabric system view (co-sim)

A co-sim system has **two fabrics joined by one bridge**:

```
        host cores                              device PEs
           |                                        |
      host crossbar          host<->device       device H-tree
   (analytic; 1-hop,        BRIDGE (protocol      (JEDEC-derived,
    contention at ports)  --  x phy; boundary  --  placement-driven
           |                traffic only)             tree)
       host main memory                          device memory tech
       (M/D/1, host-path adder)                  (PE-MI + Garnet)
```

- **Device H-tree** -- the device side of DRAM technologies is always a tree
  derived from the JEDEC organization ([network.md](network.md)). Every PE
  memory access is priced here (own unit = 0 hops; elsewhere = tree distance).
- **Host crossbar** -- classic flat topology; at 1 core it degenerates (no
  fabric), at multi-core it adds a fixed one-hop latency, with host memory
  bandwidth as an aggregate M/D/1 queue. Host core loads/stores are priced
  here (out-of-ROI code, and the whole kernel under NO_OFFLOAD baselines).
- **Bridge** -- the two-layer (`protocol` x `phy`) join. It carries ONLY
  boundary traffic: launch cmd/ack, Case-1 coherence flush, Case-2 DMA. It is
  where host-core traffic and device-PE traffic meet; neither fabric's ordinary
  traffic crosses it. The device-internal fabric and the host<->device
  interconnect remain two separate Garnet instances; the analytic host tier
  and bridge charges compose over them. See [cosim.md](cosim.md).

## Execution models for parallel workloads

- **OpenMP**: one process; all threads share one simulator instance and one
  Garnet network (real cross-thread contention).
- **MPI**: PIMID forks N QEMU children with `libpimid_mpi.so` preloaded; ranks
  exchange data over a shared-memory mailbox (no external mpirun). Each rank
  simulates its own network instance.

## Reproducibility

**Given a deterministic instruction stream, the simulator is exact.** Repeated
runs of a single-threaded workload produce bit-identical results -- cycles and
access counts, every digit -- when they are launched the same way.

**The launch environment is part of the run.** The guest inherits pimid's
environment block (plus `workload.env` and the OpenMP settings below), the
environment strings are part of the guest's initial memory image, and the
simulated address layout follows them: a different environment can place the
workload's data at different addresses -- which addresses share a bank, a
row, a channel. The instruction stream does not change; a memory-bound
result can. Measured: 500 more bytes of environment moved a 65536-element
serial `stream_triad` on HBM3 by 0.36% with identical instruction streams;
small shapes (`bfs` 4096 vertices, `stream_triad` 1000, `gemv` 256) did not
move. In 1.12.15 an in-order `gemv` 256 with one PE did: one binary with
0 / 64 / 4096 bytes of extra environment gave 540746 / 540951 / 540805 cycles
(each repeated exactly). The path of the build tree is part of that environment, so
two builds of one source in folders whose paths differ in length differ the
same way. So:

- **Results from different jobs are compared no finer than 0.5%.** Two jobs
  rarely share an environment byte for byte (the job id, the working
  directory and the scheduler's variables all live in it).
- **When comparing two builds, run OLD and NEW from one working directory with
  one environment.** Then a single-threaded comparison is exact again, and any
  difference is the builds'.

**A parallel workload is timed by the model.** Its threads are real threads
inside the emulator, but their simulated time does not come from the host's
interleaving. Each core and PE keeps its own clock from its ROI baseline;
every NoC record and DRAM request is stamped on that ROI-relative clock; a
thread woken by another, or taking a lock word another released, moves its
clock to max(own, the waker's or releaser's time) and takes that core's
origin; and at each phase barrier the records are folded at the consistent
cut (the earliest ROI-relative time over the cores still running or waiting).
Accesses are priced from frozen epochs (`simulation.det_epoch_phases`): an
access reads an earlier epoch's per-pair NoC latency and DRAM service latency
by epoch index from its own clock, never a live value. Message-passing ranks
use the same mechanism and also meet at their rendezvous. The run time is the
ROI's elapsed wall-clock time, the latest ROI end on the cores' own clocks.

**Regressions can be gated exactly.** Run the single-threaded build and
require bit-identical output; it costs one run. The settings below keep the
parallel runtime from resizing its team and from spinning at barriers, so
that a waiting thread's time is the model's synchronisation, not host-side
spinning.

## Guest OpenMP runtime settings

PIMID sets four environment variables in the guest before launching a workload.
They are applied to EVERY workload, not only ones declared `openmp`, because
their absence changes results and a workload's declared type is not a reliable
guide to whether it uses OpenMP -- the reference kernels include OpenMP binaries
that no configuration declares. They are inert for a program with no OpenMP
runtime, so applying them unconditionally costs nothing.

| variable | value | why |
|---|---|---|
| `OMP_NUM_THREADS` | PE count (`host.num_cores` for a system-scope `PIMID_COSIM_NO_OFFLOAD` baseline, where the host runs the kernel) | Without it libgomp sizes its team from `omp_get_num_procs()`, which under user-mode emulation reports the **host machine's** core count. The team would then depend on which machine the job landed on. |
| `OMP_DYNAMIC` | `FALSE` | Permits the runtime to resize the team at run time. With it enabled the runtime, not the configuration, chooses the team size and therefore the work each thread does. |
| `OMP_WAIT_POLICY` | `PASSIVE` | Threads sleep at a barrier instead of spinning. A spinning thread executes instructions, and the simulator charges cycles for them -- so the reported time would include spinning whose duration the host kernel decides. |
| `GOMP_SPINCOUNT` | `0` | Removes the residual spin before a thread sleeps, for the same reason. |

An explicit `workload.env` entry overrides any of them; PIMID sets them without
overwrite so a deliberate choice always wins.

**These settings change results.** Enabling them on a configuration that
previously ran without moved reported cycles by several percent and memory reads
by a fraction of a percent -- the latter because a dynamically-resized team does
a different amount of work per thread. Numbers produced before and after this change are therefore not
comparable, and a run's provenance should record which behaviour applied.

