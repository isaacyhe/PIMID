# PE Core Models

Five core models are available for PIM device PEs (`pim.pe.type`) and, in
system scope, for hosts (`hosts[].core_type`) and devices (`devices[].pe_type`).

| Value | ZSim engine | What it models |
|---|---|---|
| `compute_unit` | ALU | A datapath, not a processor: a register file, arithmetic units, a result bus and a resident instruction store, with no caches and no speculation. Sized by `pim.pe.lanes` / `operand_width` / `floating_point` / `imem_bytes` and shaped by the scaling factors below. The default device element. Alias: `alu_core` (which the entire existing sweep corpus names, so it is permanent). |
| `simple_core` | Simple | Coarse functional core: IPC = 1 plus serial memory latency. Fast, approximate. Aliases: `Simple`, `simple`. |
| `in_order_core` | InOrder | Decode-driven in-order pipeline: real RAW-dependency stalls, functional-unit latencies, and dual-issue in program order (no reordering), plus the cross-PE memory-contention weave (`CoreRecorder`). Aliases: `InOrder`, `in-order`, `in_order`. |
| `ooo_core` | Out-of-order | Out-of-order superscalar (Westmere-class: 128-entry ROB, 4-issue). Aliases: `OOO`, `OoO`, `ooo`, `out-of-order`. |
| `null_core` | Null | No timing model: counts instructions (cycles == instrs, IPC = 1) and drops all memory accesses (empty load/store handlers, so no NoC traffic). An IPC = 1 control/upper-bound baseline. Aliases: `Null`, `null`. |

Any other value is rejected with an error listing the valid names.

## What every model shares, and what none of them are

All five consume the SAME host instruction stream: the emulator executes the real
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

## Fidelity ladder (simple vs in-order vs out-of-order)

`simple_core` is an **IPC = 1** model: one instruction per cycle plus full
blocking memory latency, with no instruction- or memory-level parallelism. It is
the optimistic per-instruction bound and the fast approximation.

`in_order_core` is a decode-driven **in-order pipeline**: it consumes the decoded
uops (below) and issues them in strict program order, stalling on real
per-instruction RAW dependencies, functional-unit latencies, and
issue-width/port contention -- with no reordering (mechanism details in the
Notes below). Dependency chains it cannot hide push it *above* simple's
IPC = 1 (FP-latency-bound stencil), while dual-issue plus cross-block overlap
pulls it *below* on kernels with exploitable independence (gemv, histogram); it
also carries the cross-PE memory-contention weave (`CoreRecorder`) and pays
branch-mispredict flush bubbles. Genuinely distinct from `simple` -- not a
rename.

`ooo_core` adds out-of-order issue (128-entry ROB + reordering), hiding latency
the in-order core must stall on, so `ooo <= in_order` holds across the kernel
suite (typical gaps 2-3.5x, tracking each kernel's ILP/MLP). Use
`in_order_core` for dependency/issue-accurate in-order timing, `ooo_core` for
the reordered upper bound, `simple_core` for the fast IPC = 1 approximation.

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
- **No wrong-path effects**: a mispredict charges the flush/refill bubble but
  wrong-path fetches do not pollute caches. Standard for simulators of this
  class.
- **Timing microarchitecture is fixed per core model** (`ooo_core`:
  128-entry-ROB/4-wide Westmere-class, compile-time; `in_order_core`: dual-issue
  default). The `power.mcpat_overrides` keys (`pipeline_depth`, `issue_width`,
  ...) shape the McPAT power/area model only, never cycle timing -- see
  [yaml_reference.md](yaml_reference.md).
- At large working sets, `ooo_core` can legitimately exceed `in_order_core` on
  branch-heavy irregular kernels: its mispredict redirect is resolution-bound,
  so a load-fed mispredict pays a DRAM-latency-long refill that a shallow
  in-order pipe does not pay on top of its load-use stall. This is physics, not
  a calibration error.

## Notes

- `null_core` issues NO memory or network traffic -- its load/store handlers are
  empty, so it drops every access. It therefore cannot drive memory- or
  network-only studies (use `alu_core` with `access_factor`/`compute_factor`).
  Its role today is an IPC = 1 control/upper-bound baseline. Note: PIMID does not
  switch core models mid-run, so `null_core` cannot yet fast-forward non-ROI
  regions into a detailed core; true fast-forward (a mid-run core switch, a
  separate mechanism from trace/replay) is planned for 1.7.x.
- In-order/out-of-order cores automatically upgrade a `simple` memory controller to
  `weavesimple` for correct weave-phase interaction.
- `in_order_core` reuses the same in-tree x86 decoder as `ooo_core` (below) to
  drive an in-order scoreboard: per-register ready-cycle tracking, dual-issue in
  program order, functional-unit port contention, and load-use stalls (no
  reordering). The scoreboard carries across basic-block boundaries and drains
  only at mispredict flushes and scheduler boundaries
  (join/phase/context-switch). Branch modeling matches `ooo_core`: a 2-level PAg
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
  (default 2; env `PIMID_INORDER_WIDTH` overrides YAML). Diagnostics per
  in-order core: `uops`, `decodedBbls`, `syntheticBbls`, `depStalls`,
  `issueStalls`, `branches`, `mispredBranches`, `mispredStallCycles`,
  `indirBranches`, `indirMispreds`, `rasReturns`, `rasMispreds`,
  `repDrainedLoads/Stores`, `memMismatchLoads/Stores`.
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
  accesses through a serial drain, counted as `repDrainedLoads/Stores`. The
  generic-uop fallback is ~0.0% of dynamic instructions (only serializing ops:
  syscall/cpuid/rdtsc/xsave). Unrecognized instructions' memory accesses are
  absorbed by a tolerant load/store drain, so counts never desync (measured
  memMismatch is <0.05% of memory ops). Branch modeling: the direction predictor
  is driven by the real per-branch direction (resolved from the actual next TB),
  and indirect jmp/call targets and returns are predicted by a 512-entry
  direct-mapped BTB (last-seen-target) plus a 16-entry return-address stack --
  wrong targets pay the same front-end redirect as a conditional mispredict.
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
