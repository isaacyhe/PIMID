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

- `scope: device` -- a single PIM device: PEs in the memory hierarchy.
- `scope: system` -- multi-node: hosts (with caches) + devices connected by a
  system interconnect; supports host->device offload ([cosim.md](cosim.md)).
- `scope: cosim` -- system scope with its two nodes given by the flat keys:
  the `host:` block is `system.hosts[0]` (read as a declared host node) and
  the top-level device keys are the device node (1.12.2; see
  [yaml_reference.md](yaml_reference.md#host-block-host-scope-cosim)).

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
move. So:

- **Results from different jobs are compared no finer than 0.5%.** Two jobs
  rarely share an environment byte for byte (the job id, the working
  directory and the scheduler's variables all live in it).
- **When comparing two builds, run OLD and NEW from one working directory with
  one environment.** Then a single-threaded comparison is exact again, and any
  difference is the builds'.

**A parallel workload does not repeat.** Its threads are real threads inside the
emulator, the host kernel decides how they interleave, and the emulator reflects
that faithfully. A parallel program on real hardware does not repeat its
interleaving either, so this is the workload's property rather than a defect in
the simulator. Total work stays stable across runs; simulated time moves.

The size of that movement is not a constant. It depends on the workload, the
machine, and how loaded the machine is -- measurements of the same configuration
on one node have ranged from a fraction of a percent to several percent depending
on what else was running. Quote it as a measured bound for the specific study,
not as a property of the simulator.

Two things follow:

- **Differences smaller than the run-to-run variation are not findings.** Measure
  the variation for the configuration in question before claiming an effect near
  its size.
- **Regressions can be gated exactly.** Run the single-threaded build and require
  bit-identical output. That is a far sharper check than judging a parallel run
  through its own noise, and it costs one run.

The settings below reduce this variation substantially -- they stop the parallel
runtime resizing its team and stop its threads spinning at barriers -- but they
cannot remove it, because its source is the workload.

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
| `OMP_DYNAMIC` | `FALSE` | Permits the runtime to resize the team at run time. With it enabled the team, and therefore the work each thread does, can differ between runs of the same binary. |
| `OMP_WAIT_POLICY` | `PASSIVE` | Threads sleep at a barrier instead of spinning. A spinning thread executes instructions, and the simulator charges cycles for them -- so the reported time would include spinning whose duration the host kernel decides. |
| `GOMP_SPINCOUNT` | `0` | Removes the residual spin before a thread sleeps, for the same reason. |

An explicit `workload.env` entry overrides any of them; PIMID sets them without
overwrite so a deliberate choice always wins.

**These settings change results.** Enabling them on a configuration that
previously ran without moved reported cycles by several percent and memory reads
by a fraction of a percent -- the latter because a dynamically-resized team does
a different amount of work per thread. They also reduce run-to-run variation
substantially. Numbers produced before and after this change are therefore not
comparable, and a run's provenance should record which behaviour applied.

**What they do not fix.** A multi-threaded guest still interleaves differently
between runs, because the host kernel schedules those threads and the emulator
faithfully reflects that. Residual run-to-run variation in reported cycles is
expected; total work is stable. Treat differences smaller than the variation as
noise rather than as findings.

