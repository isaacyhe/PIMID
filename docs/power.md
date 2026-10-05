# Power and Area

Every number here comes from a tool, driven by measured activity. Where something
is assumed rather than measured, this page says so -- the assumptions matter as
much as the results.

## What prices what

| component | timing | power | area |
|---|---|---|---|
| processing elements | zsim | McPAT | McPAT |
| element memory interfaces | queueing model | McPAT | McPAT |
| memory array (DRAM) | Ramulator2 | Ramulator2 | CACTI |
| memory array (NVM) | NVSim | NVSim | NVSim |
| caches | zsim | McPAT | CACTI |
| interconnect | Garnet | McPAT | McPAT |
| memory controllers | -- | McPAT | McPAT |

Power analysis is on by default; `--no-power` disables it for fast timing sweeps.
`pim.pe.energy_factor` scales reported per-operation element energy and does not
affect timing.

## The reference class

A power model prices a die against a measured population, and that choice matters
more than any single parameter. McPAT offers two: server processors, where the
undifferentiated-core term is a curve fitted to Niagara, Merom, Penryn and
Opteron die measurements; and embedded parts, calibrated against ARM designs and
a parametrized-processor study.

**Device scope selects the embedded population. Host scope selects the server
one.** A processing element on a memory die is not a fragment of a server
processor, and pricing it as one made the undifferentiated term -- which carries
no dynamic power at all, only leakage -- larger than everything the description
actually named.

This is not a discount. It is the other of the two populations the tool was
calibrated against, and the one a memory-die element belongs to.

**It does not claim in-memory logic is cheap.** The literature is consistent that
logic built in a memory process is slower and larger than the same logic in a
logic process. Since 1.11.2/1.11.12 that penalty is MODELLED: the placement x
technology matrix selects a DRAM-periphery process family for elements that
live on DRAM silicon (subarray..chip on DRAM technologies, and the channel
tier on channel-centric LPDDR/GDDR/HBM), and McPAT itself applies the family
factors -- area x2.44 (22nm class; x2.46 at the 32nm DDR3 class), dynamic
x0.82/x0.66 and the comm-dram leakage rebase -- derived from CACTI's own
hp vs comm-dram device columns at 350K. Rank/channel placements on DDR-class
parts and the HBM base die keep the logic family (their logic sits on a
buffer or base die), as do all SRAM/NVM technologies (BEOL over logic CMOS).
The applied factor and its uncertainty band are printed by every run.

## The element

The element is composed from what it is, rather than borrowed from a processor
with fields turned down. Its arithmetic follows the datapath -- one unit of each
kind per lane, since a lane that cannot multiply or cannot do floating point
stalls on kernels for which the timing model charges no stall. Its instruction
store is an explicitly sized resident memory. Its datapath width is the same
field the timing model charges through, not a second one.

The knobs are `pim.pe.lanes`, `pim.pe.operand_width`, `pim.pe.floating_point` and
`pim.pe.imem_bytes` -- see the configuration reference, which also records where a
knob reaches only one half of the model and warns rather than pretending
otherwise.

Residual, stated rather than hidden: the instruction store is still built by a
cache constructor, so it carries a tag array and single-entry miss, fill and
prefetch structures a scratchpad does not have. That overstates it.

## The memory array

**There is one memory, and it is charged once.** This build models one host and
one memory: the memory either *is* the processing device, or it is a plain
non-PIM main memory and the run is host-only. In a co-simulation the host's
accesses and the elements' accesses land on the same silicon, so the charge is
their sum, applied once. A host term beside a device term would price that
silicon twice.

If two nodes ever name different memory technologies the run stops, rather than
charging one and dropping the other -- that is a topology this build does not
model, and mispricing it silently would be worse than refusing.

Non-DRAM technologies are not charged in system scope yet, and say so rather than
printing a number shaped like a DRAM one.

## Measured, and assumed

Measured from the run: element and host activity counts, cache accesses and
misses, interconnect packets and hops, memory reads and writes, cycles.

**Assumed -- worth knowing before quoting a result:**

- **The instruction mix.** Nothing counts floating-point, multiply or integer
  instructions; the emulator knows the opcode and the plugin discards it before
  the timing model sees it. The power model splits non-branch instructions by a
  fixed ratio and drives the execution bypass with fixed fractions. A
  memory-bound kernel and a compute-bound one therefore receive the same mix.
- **Interface energy** per access is an inherited constant, not a measurement.
- **Power gating** has no temporal weighting of its own.

**Host power gating is credited from a measured union (1.11.106).** With
`system.hosts[].pg: true` the host gates as one piece, cores and host memory
controller together, so it is idle only when nothing in it is busy. Its idle
residency is 1 minus the MEASURED union of core-active and host-MC-active
phases: zsim's union tracker counts the phases in which any core retired or
the host memory controller took an access. The earlier credit used
max(core-active, MC-active), which understates activity and so over-credits
the gating, while the printed text called it conservative. A stats file
without the union counter falls back to the max, and the `[pg]` line says that
it over-credits. Configurations without `hosts[].pg` are unaffected.

## Substituted values are refused

A number a linked tool could not produce is not replaced by a plausible one in
silence. Since 1.11.90 McPAT counts what it substitutes -- a clamped or
unsolved CACTI array, a sanitised router field, a link left at zero power, a
non-finite reduction factor -- and the run refuses (rc 3). Since 1.11.106 the
CACTI wire model joins them. At some nodes it returns a non-positive power for
a wire, and the wire used to be priced at ZERO power and ZERO delay without a
word, silently removing it from a cache, array or router characterisation. A
wire of non-zero length (or a wire type whose own characterisation is
non-positive at the node) priced that way now prints a
`[cacti] WARNING: wire ...` line and is counted; the CACTI solve that met it
is refused (rc 3), and a McPAT run carries the count back to the parent, which
refuses as it does for the other substitutions. A zero-length wire is exempt:
it has no power and no delay to substitute. `PIMID_ALLOW_ARRAY_CLAMP=1`
reports anyway, with the warnings printed.

## Reproducibility

Given a deterministic instruction stream the simulator is exact: repeated runs of
a single-threaded workload produce bit-identical cycles and access counts.

Parallel workloads vary between runs. That variation belongs to the workload, not
the simulator -- the host kernel schedules the guest's threads and the emulator
reflects that faithfully, exactly as a parallel program on real hardware does not
repeat its interleaving. Total work stays stable; simulated time moves.

Two things follow. Differences smaller than that variation are not findings. And
any regression can be gated against a single-threaded build and required to be
bit-exact, which is a sharper check than judging a parallel run through its own
noise.
