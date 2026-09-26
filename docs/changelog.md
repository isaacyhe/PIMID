# Changelog / defect ledger

Release + defect ledger for the co-sim MPI window, the measured-feedback MPI
pricing model, and the OMP critical-path metric. One entry per release; each
gives the defect fixed, a one-line root cause, and a data-impact note (which
sweep generations the fix invalidates or corrects). Authoritative source is the
release commit messages; deeper design rationale for 1.9.0 is in
`docs-dev/DESIGN_190_PDES.md`.

## 1.11.90 -- a number the tool could not produce was still substituted in silence

Found by a read-only audit of the tree (2026-09-26) for the one class the
project refuses everywhere else: a value replaced, clamped or defaulted
without a word. None of the sites below fires on any corpus config (every
smoke log is clean), so no corpus number moves. The point is that none of
them can fire silently in the future. Plus one found by the fleet smoke the
same day (part C).

**(A) The power path.** Seven substitutions between McPAT/CACTI and the
report:
- `extractResults()` read every Processor-level aggregate as
  `isfinite(v) ? v : 0.0`, so one NaN router or block zeroed a whole
  component's dynamic or leakage and the rest was reported as the total. The
  runtime, peak and core-breakdown reads now REFUSE a non-finite value,
  naming the component and the field (the forked child prints the FATAL; the
  run stops with rc 3, the 1.11.88 rule). An undescribed positional stub is
  zeroed outright rather than read: it is excluded from every total. The
  per-level NoC read divided by 1.0 s when the execution time was not
  positive, printing Joules as Watts, and its leakage was unguarded: both
  refuse now.
- McPAT's ArrayST sanitiser clamped every NaN/inf/negative power field to 0,
  and an array CACTI found no organisation for was left at zero power and
  area; the router sanitiser in noc.cc did the same to router fields; the
  interconnect returned early at zero power (catch(...) twice, and an
  upstream assert(power > 0) turned into a return that also skipped the
  circuit-switching scaling, the long-channel leakage and the power-gating
  endpoints -- and `x <= 0` let a NaN through); the long-channel and
  power-gating reduction factors replaced a NaN (or a zero rail) by 1.0 or 0.
  Each now prints one `[mcpat] WARNING` line naming the structure and the
  field the first time, and counts it in a substitution ledger
  (basic_components.h). The counts travel back to the parent in the fork's
  result blob (format stamp PIMIDBP2), and the parent REFUSES the run (rc 3)
  when any is non-zero, unless `PIMID_ALLOW_ARRAY_CLAMP=1`, which reports
  anyway and prints the counts.
- The router buffer's AREA fallback (bare cells, no periphery) when the
  Mat's area is non-finite -- 30 lines below the 1.11.87 energy refusal --
  now refuses the same way, naming the node and the geometry.
- `power.mcpat_overrides.device_type` accepted 3 (lp-dram) and 4
  (comm-dram), which at 22 nm price the logic at Vdd 0. It is refused at
  config load unless it is 0 (hp), 1 (lstp) or 2 (lop); the lp-dram column is
  reached through `power.device_corner` on a DRAM-periphery placement, as
  before.
- The 1.11.81 router tripwire text still said the buffer solve was broken
  and reached DDR3 alone. It now describes the 1.11.87 state: the buffer
  converges at every node measured, and the tripwire is a cross-check that
  would fire only on a new imbalance.
- Five PIMID-added comments carried em-dashes; ASCII now, as are the
  other files this release touches (README.md, docs/yaml_reference.md,
  qemu_trace_plugin.c: dashes, arrows and the README tree drawing).
For the gate, `PIMID_MCPAT_FAULT=<site>` (array, router, link, reduction,
extract, noclevel, bufarea) injects a non-finite value at that site and says
so on an `[inject]` line; unset, nothing changes.

**(B) Config loading.** Five silences in the YAML loader:
- yaml-cpp's `as<T>(fallback)` returns the fallback whenever the conversion
  fails, so a present key of the wrong type ran the default under the user's
  setting (`pe.count: sixteen`, `ddr5_speed_grade: DDR5-3200`,
  `temperature_c: 97.0`, `max_instructions: 1e9`, an empty value), and `0`/`1`
  were not booleans (`floating_point: 0` stayed true). All 142 such reads now
  go through `yamlInt` / `yamlI64` / `yamlU32` / `yamlDouble` / `yamlBool`,
  each with its own default and key path: an ABSENT key takes the default
  exactly as before; a PRESENT key that does not convert is refused with rc 2
  naming the path, the literal text and the type expected; booleans accept
  0/1. The conversion is yaml-cpp's own, so every value that converted before
  converts to the same number. `pim.pe.frequency_mhz` (read without a
  fallback, so it already refused on `500.0` with a line/column message) now
  names the key too.
- `pim.placement.level`: an unknown or lowercase word ran BANK. Refused, with
  the list (SUBARRAY/SUBBANK/MAT, BANK, BANK_GROUP, CHIP, RANK, CHANNEL,
  LOGIC_DIE, HOST_MC); lowercase is refused, not upper-cased, like every
  other enum here.
- Twelve enum knobs that took any word, siblings of 1.11.85's three:
  `pim.mc.placement`, `pim.mapping.mode`, `noc.ring_direction`,
  `noc.bridges.*.model`, `system.devices[].type`, `system.coherence.mode`,
  `system.network.model`, `system.network.links[].type`,
  `power.link.link_type` (and `power.pcie.*`), `power.link.model`,
  `system.devices[].noc.model` and `workload.type`. Each refuses an unknown
  word at load, rc 2, with the accepted list. `noc.bridges.*.model:
  analytical`, documented as an alias of simple, fell through to AUTO; it is
  now the alias the documentation says.
- `scope: cosim` parsed a `system:` block's hosts and devices and then
  replaced them with one host and one device synthesized from top-level keys
  (host 4 cores at 3000 MHz unless `host:` says otherwise). With declared
  nodes it is refused (use `scope: system`); without, the legacy synthesis
  stays and prints a NOTE naming what it built.
- Precedence: `system.frequency_mhz` (parsed later) beat
  `pim.pe.frequency_mhz`, and the YAML beat `--scope` and `--power-report`,
  each against the documentation. The code now follows the documentation --
  the PE key sets the PE clock, the command line wins -- with a NOTE when two
  given values differ.
docs/yaml_reference.md: CHANNEL and LOGIC_DIE are in the placement table;
`devices[].noc` is no longer called inert (model and topology are used);
`attachment` distinguishes only `internal` from anything else;
`devices[].pim.mapping`, `power.pcie.num_channels` (when `num_lanes` > 0) and
`power.pcie.num_units` (system scope) are marked NOT READ; `device_type`
lists its three values.

**(C) Shared objects from another build were loaded in silence.** Found by
the fleet smoke: `findPimidMpiLib()` anchored on `getPimidRoot()`, which
strips the executable's directory at "/build"; a binary run from a directory
without "/build" in its path fell through to `./libpimid_mpi.so` and
`./build/libpimid_mpi.so` relative to the CWD, and the repo root's stale
build/ tree (its pimid reports 1.6.4) supplied the MPI shim to a 1.11.88
binary and plugin: MPI rows with cycles NA, one rank hung for 50 minutes.
`findQemuPlugin()` had the same shape. Now (1) each of `libpimid_mpi.so`,
`libzsim_qemu.so`, `libpimid_trace.so` and `libpimid_plugin.so` carries a
version stamp (`@(#)PIMID_COMPONENT <name> <version>` and an exported
`<component>_component_version()`); the loader reads the stamp from the
candidate file -- not by dlopen, which would run the MPI shim's
`__libc_start_main` interposer and the QEMU plugin's constructors inside
pimid -- and REFUSES (rc 2) a candidate whose version differs or that has
none, naming its path and both versions; (2) the search is beside the binary
(and its `external/zsim/` for the QEMU plugins), then `$PIMID_ROOT/build` if
PIMID_ROOT is set, and nothing else; not found is a FATAL listing every path
tried; (3) every component prints `[load] <name>: <path> (<version>)` once,
in both scopes (system scope printed no plugin line before). The linked
`libpimid_plugin.so` is checked from the path the dynamic linker mapped.
`--check-components` runs the same lookup and exits, without simulating.

DATA IMPACT: none on any corpus config: every substitution and silence above
is now a refusal; measured by loading all 370 fleet configs rc 0 (280 device
scope, 90 system scope), their `--print-mem-info` output byte-identical to
1.11.89 apart from the tree path. The one behaviour change a valid config
can see is `noc.bridges.*.model: analytical` (now simple, as documented;
used by no corpus config), and a run's log gains the four `[load]` lines.

Gate 1199A, with arm A0 re-evaluated as 1199B: A0 demanded total power
identical to 1.11.89's on a full run, whose cycle count jitters (0.23% here),
so the time-divided dynamic term moved 0.25% while leakage and area were
identical to the digit; 1199B holds leakage and area exact and the dynamic
term and cycles within 1%, and expects the two [load] lines an OpenMP run
prints rather than three. The injected-link arm was re-evaluated as 1199C:
the injected NaN propagates into the core component's total, so the child's
own non-finite guard refuses first (exit 21 in the child, rc 3) before the
parent's substitution count is read; the arm expected only the parent's
message, and either is the refusal. Gate 1199A. Each refusal needs a config that TRIGGERS it (rc 2 or 3, named
message; OLD accepts it) and the corpus must load unchanged:
- A1-A5, A7 (power path): the device-scope smoke shape with
  `PIMID_MCPAT_FAULT` = `extract` (`FATAL: McPAT produced a non-finite
  runtime dynamic power`), `noclevel` (`FATAL: NoC level 0 has execution
  time 0`), `array` (`[mcpat] WARNING: array` ... `clamped to 0`), `router`
  (`[mcpat] WARNING: router`), `link` (`[mcpat] WARNING: link`),
  `reduction` (`[mcpat] WARNING: power-gating leakage reduction was nan`) --
  each rc 3 with `McPAT substituted numbers it could not produce` for the
  four ledger sites -- and `bufarea` (`[cacti] FATAL: the router
  input-buffer Mat returned a non-finite AREA`, rc 3); the `array` shape
  again with `PIMID_ALLOW_ARRAY_CLAMP=1` completes, printing
  `PIMID_ALLOW_ARRAY_CLAMP=1: reporting power although`.
- A6: `power.mcpat_overrides.device_type: 3`, rc 2,
  `device_type = 3 is not a device corner`.
- B1: `pim.pe.frequency_mhz: 500.0` (rc 2, `pim.pe.frequency_mhz = '500.0'
  is not an integer`), `pim.pe.count: sixteen`, `simulation.max_instructions:
  1e9`, `power.temperature_c: 97.0`, `memory.dram.ddr5_speed_grade:
  DDR5-3200`, `cache.l2.enabled: maybe`, `cache.l2.size_kb:` (empty); and the
  FIRES side of 0/1: `pe: { type: null_core, floating_point: 0 }` is refused
  by the existing null_core rule (OLD read 0 as the default true and
  accepted it).
- B2: `level: bank` and `level: BANKK`, rc 2, `is not a placement tier`.
- B3: each of the twelve knobs with a misspelt word, rc 2, `is not a value
  this build has`; `noc.bridges.*.model: analytical` loads.
- B4: a system-scope corpus config with `scope: cosim`, rc 2, `scope: cosim
  with a system: block`; a device config with `scope: cosim` prints `the
  nodes were SYNTHESIZED from top-level keys`.
- B5: `system.frequency_mhz: 1000` beside `pim.pe.frequency_mhz: 500`
  prints `the PE clock is 500 MHz`; `--scope device` on a YAML saying
  `system` and `--power-report summary` on one saying `verbose` print
  `the command line wins`.
- C: a copy of the binary alone in a directory, PIMID_ROOT unset,
  `--check-components`: rc 2, `libpimid_mpi.so not found`; with the repo
  root's pre-1.11.90 `build/libpimid_mpi.so` beside it: rc 2, `version NONE
  (no version stamp`; with a 1.11.90 shim whose stamp is edited to 1.11.91:
  rc 2, `is version 1.11.91, this binary is 1.11.90`; an old
  libpimid_plugin.so preloaded: rc 2; the build tree itself: rc 0 and four
  `[load]` lines.
- Parity: `--print-mem-info` on all 370 fleet configs rc 0 and identical to
  1.11.89 (all seven technologies, device and system shapes), and one full
  device-scope run whose power is identical to 1.11.89's -- the run that
  also shows no `[mcpat] WARNING` fires on a supported configuration.

## 1.11.89 -- system scope priced a different memory from device scope, and every scope priced the setup

Found by a read-only audit of the system-scope power path
(`runPerNodePowerAnalysis`, `reportSharedMemoryArrayEnergy`) before the
90-cell system-scope fleet (co-sim DDR5/HBM3 x 5 kernels x omp/mpi, the
NO_OFFLOAD baselines at 1/4/16 host cores, and the DDR5-3200 co-sim
companions), plus one defect in the plugin that touches every cell. All
confirmed by reading the code; the fleet was held until they shipped.

**(1) The measured row-miss fraction never reached system scope.** Device
scope has weighted the activate/precharge share of every access by the run's
own PE-MI `rowHits`/`rowMisses` since 1.11.52 (D003). The system-scope
reporter never called `setRowMissFraction`, so every co-sim access was priced
at the 0.5 fallback while the measurement sat in the dump. Accesses are now
priced by origin: PE-originated accesses at the MEASURED fraction (the same
`[mem] row-buffer miss fraction MEASURED x` line device scope prints),
host-originated accesses and flush writebacks at the stated 0.5 fallback with
a line saying why -- the host memory controller exports no row counters, and
one side's measurement is not lent to the other. No new default: 0.5 is the
fallback device scope already states for an unmeasured run, and it does not
refuse there, so neither does this.

**(2) The idle descent was gated on flags the fleet never sets.** Device
scope computes the DRAM idle residency unconditionally (1.11.20, D15: an idle
controller closes its pages whether or not power gating was asked for; the
`memory.power_down` flag only adds the IDD2P step). System scope gated the
whole descent -- residency and the E17 gap histogram -- on
`memory.power_down || pim.mc.pg`, so every fleet cell reported flat IDD3N
standby while its device-scope twin descended (smoke: 1339.46 -> 940.16 mW
at residency 0.98 in device scope, flat in system scope). The gate is gone.
The residency also counted the wrong controller: it read the device-MC
counter alone, for a shared array that the host MC drives too, and weighed
that counter against host+device accesses -- which on a host-only baseline
(device MC 0, host traffic > 0) would have refused the descent as a dead
counter. One arming rule now serves both scopes (`pgCounterArmed`: a counter
is a measurement if it advanced or its OWN side made no accesses). A shared
array's phase residency counts device-MC and host-MC activity; zsim exports
each count but not their per-phase union, so the union is printed as a band
and its least-idle end is used (exact whenever either side is zero). The gap
histogram records device-MC events only, so on a shared array it is lowered
by the most the host's accesses can take from it (each host access or flush
writeback breaks at most one usable gap, < 2B cycles for B the first usable
bucket) and the bound is printed. A decoupled array uses its own controller
only. If the dump carries no host-MC counter, the run says so and uses the
device-MC counter alone.

**(3) Host accesses to the shared array paid no DQ termination.** 1.11.52
(A019) fixed the decoupled host array and left the coupled branch pricing
every access with the PE placement's answer, so at BANK placement the host
CPU's reads, writes and flush writebacks to the device DRAM were reported as
"on-die placement: no DQ crossing". A host access always drives the DQ pins.
Each access origin now carries its own answer; HBM's termination stays zero
by citation (JESD238B cl. 9.1) and the report now says so on this path too.

**(4) Baselines priced a device that never ran and a link nothing crossed.**
docs/cosim.md promises the NO_OFFLOAD baseline "no offload-driven device
pricing"; the per-node loop priced every node with cores anyway -- the
declared 8-PE device block (~0.04 W, 6.43 mm^2 on the 16-core DDR5 smoke) and
both pcie_gen5 link controllers landed in System Total power and area. Under
`PIMID_COSIM_NO_OFFLOAD` in system scope (recognised the way every other site
recognises it) the device node and the link are skipped with one line saying
so; the memory array stays priced, because the host uses it. The mode is
read at power time: a power re-derivation of a baseline must set it too.

**(5) In-order PEs were priced single-issue while simulated dual-issue.**
zsim's `InOrderCore` runs `issueWidth` 2 by default and honours
`pim.pe.issue_width` (and `PIMID_INORDER_WIDTH`); McPAT was handed a
hard-coded `issue_width = 1` for every in-order element in both scopes. It is
now handed the width the core resolves, by the core's own precedence, which
reaches the XML's fetch/decode/issue/peak/commit widths (the in-order profile
emits `config_.issue_width` directly) and is printed on the core description
(`Profile: DEVICE_INORDER (issue_width=N)`, `<node> (InOrder issue_width=N,
...)`). `simple_core` stays 1 (IPC-1 by construction);
`power.mcpat_overrides.issue_width` still wins.

**(6) Traffic counters were never rebased at ROI begin, so setup traffic
was priced over kernel time.** Every scope. The cores rebase their own
instructions and cycles at `roi_begin` (`markRoiBegin`), and the power-gating
window opens there (1.11.18), but the plugin records from process start
(`in_roi` defaults on) and the cache, memory-controller and PE
memory-interface counters, and Garnet's flit counters, had no baseline: they
counted the serial pre-ROI array initialisation as well as the kernel, and
power divided them by the kernel's wall clock. The audit's evidence: a co-sim
host with 4 ROI instructions and 1.18 M L1D accesses, and one active host-MC
phase in 109. For a stream kernel the init writes are the same order as the
kernel's traffic, so cache dynamic, memory-array dynamic and NoC dynamic
power were all inflated, in every cell. Now `Counter` and `VectorCounter`
snapshot a base at `roi_begin` (`roiRebase`, forwarded through the
aggregates; cores are not in the rebased set, they already rebase) and
report the delta; Garnet's counters are zeroed there and its statistics
header says `[ROI only ...]`; the plugin prints one line, `[roi] traffic
counters rebased at roi_begin (...): N stat groups, pre-ROI memory-controller
accesses dropped: X, pre-ROI NoC flits dropped: Y`, at each of its three
baseline sites (legacy roi_begin, thread-MPI first roi_begin, synthesized
first-barrier baseline). A run with no `roi_begin` reports the whole run and
the Garnet header says so.

Also, from the cloud review of 1.11.88: the unknown-top-level-section FATAL
now lists the sections from the set it checks (the literal list omitted
`synthetic`); the router comment that still promised an analytical fallback
now says the run refuses; and the seven "McPAT failed / FATAL / exit(3)"
blocks are one helper, `fatalMcPATFailure`, each site keeping its wording.

DATA IMPACT (system scope only, except (5)):
- co-sim array dynamic energy, both technologies: moves with each kernel's
  measured row-miss fraction -- DOWN where the PE-MI measures a mostly
  row-hit stream (below 0.5), UP where it measures a miss-heavy one;
- DDR5 co-sim and baseline background power: DOWN (the descent now happens;
  on DDR5 the gap histogram supersedes, lowered by the host-access bound);
  HBM3 background moves DOWN as well, by the phase-granular residency only
  (no tXP is tabulated for HBM, so the gap histogram is refused there as in
  device scope);
- DDR5 baseline and co-sim array energy: UP, by DQ termination on every
  host-originated access and flush writeback (HBM3 unchanged by this term,
  zero by citation);
- baseline totals and area: DOWN by the inert device block and both link
  controllers;
- coremodel `in_order_core` cells (both scopes): core power and area UP, by
  the doubled issue width;
- EVERY cell, both scopes, by (6): cache dynamic, memory-array dynamic and
  NoC dynamic power DOWN by the pre-ROI share of their traffic (largest for
  stream_triad and histogram, whose initialisation touches every element;
  smallest for bfs and stencil, whose kernels re-touch the data many times);
  cycles unchanged (they were ROI-scoped already). Measured by gate 1198A
  A8 on the smoke shape (HBM3, stream_triad 40000 elements, 16 alu_core):
  1,166,956 memory-controller accesses dropped at roi_begin; Garnet flits
  1,172,482 -> 21,477 (98% of the flits were pre-ROI: process start-up,
  OpenMP runtime and array initialisation, all simulated); array dynamic
  0.5 -> 0.1 mJ; cycles 7,037,535 -> 7,044,327 (run jitter). On the corpus
  shapes the kernel's share is larger (the working set is 256 MB and the
  start-up cost is fixed), but the pre-ROI share is not small for any
  kernel, and every NoC and array dynamic figure in every previous corpus
  carried it.
Device-scope DRAM numbers are unchanged by (1)-(4) by construction: the only
device-scope edit in their path replaces an expression with the helper
holding the same expression.

Gate 1198A, re-evaluated in part as 1198B: 1198A's A3 and A4 read a
device-scope line that system scope prints differently (the firing lines
were present, the quantity read NA), and A5 compared total power, which
fix (6) lowers in the same run by more than the wider issue raises the
core; 1198B re-runs those three with the system-scope extractors and, for
A5, the core area, which the rebase does not touch. Each fix needs a FIRES
side against 1.11.88:
- A1 (fix 1): a co-sim cell whose PE-MI row-miss fraction is far from 0.5:
  NEW prints `row-buffer miss fraction MEASURED` in the system report and its
  per-access read energy differs from OLD's in the direction the fraction
  predicts; OLD prints no such line in system scope. A1b: a baseline prints
  `row-buffer miss fraction NOT MEASURED for the host-originated accesses`
  and matches OLD's per-access energy to the digit.
- A2 (fix 2): the DDR5 co-sim smoke with neither flag set: OLD background is
  flat IDD3N, NEW prints `[pg] DRAM idle residency` and a background below
  OLD's; the HBM3 twin prints the phase-granular line and no gap supersession;
  a 16-core DDR5 baseline descends with NO `UNAVAILABLE` line (device MC 0 is
  armed). A2b: with `memory.power_down: true`, NEW's descent is at or below
  the no-flag NEW value.
- A3 (fix 3): a DDR5 co-sim at BANK placement: NEW prints
  `host-originated: a host access always drives the DRAM's DQ pins` with a
  non-zero termination and array dynamic above OLD's; the HBM3 twin prints
  the zero-by-citation line and an unchanged termination term.
- A4 (fix 4): a 16-core DDR5 baseline: NEW prints `NO_OFFLOAD baseline:
  device node` ... `NOT priced`, has no `Link controller` block and no device
  node line, and its System Total power and area are below OLD's by the
  device block plus the two controllers; the same config WITHOUT the
  environment variable prices the device (the arm must fire only in the
  mode).
- A5 (fix 5): a coremodel `in_order_core` cell: NEW prints `McPAT issue width
  2 = the timing model's` and `DEVICE_INORDER (issue_width=2)`, the emitted
  XML carries `issue_width` 2, and core power is above OLD's; `simple_core`
  and `ooo_core` twins byte-identical in power.
- A6 (fix 6): an unknown section's FATAL lists `synthetic`; each McPAT
  refusal trigger from gate 1197B still prints its exact second line and rc 3.
- A7 parity: device scope on all seven technologies (DDR3, DDR4, DDR5,
  LPDDR5, GDDR6, HBM2, HBM3) byte-identical to 1.11.88 at config load
  (`--print-mem-info`), and a full alu_core run's core power identical to
  1.11.88's; in_order device-scope cells differ only in core power.
- A8 (fix 6): a device-scope stream_triad run: NEW prints the `[roi] traffic
  counters rebased` line with a non-zero dropped count, its memory-controller
  rd+wr total is BELOW OLD's by that count, its Garnet header says `[ROI
  only`, and its cycles agree with OLD's within run jitter; a workload with
  no roi_begin prints `[whole run` and matches OLD's counters.

## 1.11.88 -- a refused McPAT run could still land in the corpus looking complete

Found by the cloud review of 1.11.87's diff, which asked what the new
`std::exit(2)` in the router does once `computePower()`'s fork isolation
turns it into a caught `std::runtime_error`. Answer: in DEVICE scope the run
stops (1.11.52, gate 1162C C4). In SYSTEM scope it did not. Three catch
sites printed one stderr line and went on:

- the per-node loop of system scope: `result.valid` stayed false, the node
  was pushed anyway, and the System Total loop skips invalid nodes -- so a
  co-sim cell whose device McPAT failed reported a total with the device
  costing nothing, exit 0;
- the host McPAT of the device-with-host path: the System Power Summary was
  simply not printed, exit 0;
- the NoC tool in sweep mode: nothing was printed at all and the next rate's
  row followed, so a sweep table could carry a hole.

All three now stop with the 1.11.52 FATAL and rc 3. Nothing in the corpus
is affected by construction: no cell has ever printed "McPAT failed" (the
fleet census greps for it), and the 22 nm nodes converge. The point is that
the rule is now one rule.

**Found by this release's own gate (1197A, arm A0), on both binaries.** The
gate's trigger is a PE clock of 50 MHz, which the YAML loader accepts and
McPAT's validator refuses. In device scope that refusal did not produce the
1.11.52 FATAL: `initialize()` is where the validator runs, it sat OUTSIDE the
try around `computePower()`, and the run died on an uncaught exception with
a core dump (rc 134). The same at the host site and in the NoC tool. All
three `initialize()` calls are now wrapped with the same FATAL and rc 3.

**Also found by the gate (arm A2): the documented `synthetic:` section was
refused.** 1.11.76 made an unknown top-level YAML section fatal and listed
the fourteen sections the parser reads; `synthetic` (docs/network.md, read
by `--method synthetic`) was not on the list, so the NoC tool's sweep could
not be configured from YAML at all since 1.11.76. Added.

Also in this release: the analytical estimate that followed the router's
new exit was unreachable and is deleted, and the comment above it, which
still said the 65 nm failure's cause was not located, is corrected -- it
was the unset wire type, measured in 1.11.87's A/B.

Gate 1197B (1197A was cancelled after it found the two defects above; the
binary was rebuilt). The trigger is a PE clock of 50 MHz: the YAML loader
accepts any positive frequency and McPAT's own validator refuses a core
clock below 100 MHz. A0 device scope at that clock: OLD core-dumps (rc 134,
"terminate called"), NEW refuses (rc 3, FATAL). A1 a system-scope run at
that clock: OLD exits 0 with a System Total that omits the device, NEW stops
with rc 3 and the FATAL naming the node. A2 the NoC tool at that clock: OLD
core-dumps there too (rc 134; the same uncaught initialize(), R6-22 at
its third site -- gate 1197B's first draft expected the softer "McPAT
failed, exit 0" and FAILED on that expectation, re-evaluated as 1197C),
NEW stops with rc 3. A2b the NoC tool's
documented sweep section: OLD refuses it at config load (rc 2, "unknown
top-level section"), NEW loads it and prints the sweep rows at 500 MHz. A3
the corpus co-sim shape still completes; A4/A5 device- and system-scope
loads byte-identical on all corpus shapes; A6 one full DDR3 run's NoC power
within 5% of 1.11.87's. The host site has no firing arm: no plain config
makes host McPAT alone fail (the host has no NoC and takes the device's or
the corpus's 2 GHz clock), so it is covered by code reading of the same
pattern, and said so here.

## 1.11.87 -- R6-10 resolved: the router read a wire type nobody set

Audit round 6 found DDR3 reporting 16.73 W of on-die fabric power against
0.14-0.34 W for the other six technologies, and traced it to McPAT's router
input buffer: CACTI's Mat returned NaN for it at 32 nm (DDR3's table), and a
silent fallback substituted a number ~1267x larger. This release fixes what
the router hands the Mat. There were two defects, and the second is the one
that matters.

**(1) `wtype` was stack garbage.** `Router::buffer_stats()` default-constructs
its `DynamicParameter`, and that constructor initialises three fields; `wtype`
is not among them. The Mat builds its subarray output wire on that value. Wire
dispatches on it -- Global..Global_30 pick a repeated-wire table, Low_swing a
different model -- and its `else { assert(0); }` is unreachable: it is the else
of `if (wt != Low_swing) ... else if (wt == Low_swing)`, so any other value
takes the first branch, matches no inner case, and falls out with
`repeater_spacing` and `repeater_size` never assigned. The Mat's output-driver
stage then divides by one uninitialised double and multiplies by another.
Measured: `wtype = 1072483532` at 32 nm. Consequences, all now explained: the
Mat converged at some nodes and returned NaN/inf at others with no physical
pattern; two routers in one run disagreed (NaN against inf at 65 nm); and the
set of failing nodes MOVED when unrelated locals were added to the function (45
nm converged in one probe build and failed in the next); and even the values
that "converged" were not trustworthy, being whatever the garbage divided out
to. Every other `DynamicParameter` in the tree goes through the full
constructor, which sets the field; this was the only default construction. The
field did not exist when upstream McPAT wrote this router against CACTI 6.5 --
it arrived with this fork's CACTI 7.0 integration (de23ed6d), and mat.cc's own
commented-out original used `g_ip->wt`. The fix is that line: the wire type the
NoC constructor already chose for this interface (Global_30 when Embedded,
which is every device-scope run; Global otherwise).

**(2) The bitline sense voltage was the full rail.** An upstream FIXME set
`V_b_sense = Vdd`. CACTI's own SRAM path uses 5% of the cell rail floored at
80 mV. With the hp corner the cell and peripheral rails read the same table
column, so the Mat's bitline-restore log had a zero denominator on every
node. Corrected to CACTI's convention. On its own this moved the 22 nm
buffer 2.53e-11 -> 2.19e-11 J (-13%) but did NOT stop the NaN at 32 nm,
which is how (1) was found.

**The fallback now refuses instead of substituting.** Where both the Mat and
the analytical estimate could be evaluated, the estimate was ~1267x above
the Mat on the shipped path and ~800x below it once its own
microns-for-metres unit error is corrected. A stand-in that far from the
model it replaces is a different answer, not a fallback. If the Mat still
returns non-finite after both fixes, the run stops with a FATAL naming the
node and the buffer geometry, the way 1.11.79 stops on a negative array
energy, rather than publish a number under the Mat's name.

DATA IMPACT: DDR3's fabric power collapses into the family, and every
technology's NoC power moves. Measured with the release binary on the corpus
shape at 32 nm (DDR3): total dynamic 14.664 W under 1.11.86 -> 0.157 W, a
93x collapse, L2 bank-group 8.286 -> 0.0066 W, no FATAL. Every DDR3 power
number in the previous corpus is superseded. At 22 nm the shipped build's
garbage `wtype` happened to be 0 (= Global) -- which is the only reason those
runs ever converged -- while the interface's wire type is Global_30, so the 22 nm technologies move as
well. Measured with the probe build on the corpus shape at 22 nm, shipped
garbage (0 = Global) against the interface's own Global_30:

    buffer per access   2.190e-11 -> 1.212e-11 J   -45%
    L2 bank-group       0.00750  -> 0.00434 W      -42%
    L4 rank             0.00455  -> 0.00263 W      -42%
    L1 bank bus         0.00210  -> 0.00209 W      unchanged (no router)
    total dynamic       0.1060   -> 0.1006 W       -5%

So every 22 nm technology's NoC power falls by roughly 40%, and its total by
a few percent; DDR3's falls 93x. Every NoC and total-power figure in the
previous corpus is superseded. Array energies are untouched (this release is
NoC-only; 1.11.86 owns the array).

Gate 1196A: DDR3 NoC dynamic collapses >100x and lands within 5x of DDR4's;
the 22 nm technologies' NoC dynamic moves within a band set from the
measurement above; the now-refusing fallback fires on nothing supported; the
1.11.81 buffer/crossbar tripwire clears on DDR3; array energy, refresh and
die area are unchanged on all four; device scope is byte-identical on all
seven.

## 1.11.86 -- R6-11 resolved: the rows were right, the baseline was wrong

1.11.79 refused DDR5 at 4800 and 5600 because their activate energy came out
negative, and said the cause was either a mis-transcribed IDD row or a formula
that does not transfer to a 32-bank part. It was the second.

**The rows are correct.** Read against the cited tables at the x8 column, which
is this model's device width -- Micron MT60B 16Gb Die Rev A Table 6 (p.17-19)
and Die Rev D Table 8 (p.18-20):

    part          IDD0   IDD2N   IDD3N     IDD0 > IDD3N
    DDR5-4800      103     92     142           no
    DDR5-5600       53     49      91           no

Micron really does specify IDD3N above IDD0 on both parts, and nothing is
wrong with them. A 32-bank device draws more with all banks open than it does
cycling one.

**The baseline was wrong.** Micron TN-41-01 prices activate and precharge by
subtracting, from the IDD0 loop, the background that loop would have drawn:

    E = Vdd x (IDD0 x tRC - IDD3N x tRAS - IDD2N x (tRC - tRAS))

IDD0 is JEDEC's ONE-BANK activate-precharge current: it cycles a single bank
while the others sit precharged. IDD3N is specified with ALL banks active. So
the subtracted term describes a different device state from the one IDD0 was
measured in, and over-subtracts by what the other banks' active standby costs.
On an 8-bank DDR3 that error is small and the result stays positive, which is
why the formula has stood. On a 32-bank DDR5 it exceeds the term itself.

The baseline is now the standby the loop actually runs at, derived from the
two specified points and linear in the number of open banks:

    IDD3N(1 bank) = IDD2N + (IDD3N - IDD2N) / banks_per_device

which reduces to the published formula at one bank and recovers TN-41-01's
intent on every part. The bank count comes from the run's own organisation,
so it follows the preset rather than a literal.

**DATA IMPACT: every DRAM array energy moves, and this invalidates the
existing corpus energies.** The shift is exactly
`(IDD3N - IDD2N) x tRAS x (n-1)/n`, measured per-access read energy on one
shape:

    DDR3      11.425 -> 13.512 nJ   +18.3%
    DDR4       6.893 ->  7.863 nJ   +14.1%
    DDR5-3200  7.739 ->  8.755 nJ   +13.1%
    DDR5-4800  refused -> 8.887 nJ  now runs
    DDR5-5600  refused -> 3.820 nJ  now runs
    LPDDR5     1.148 ->  1.332 nJ   +16.0%
    GDDR6      0.673 ->  0.952 nJ   +41.5%
    HBM2       0.856 ->  0.801 nJ    -6.4%
    HBM3       0.308 ->  0.368 nJ   +19.5%

HBM2 falls rather than rises because it is the one row whose IDD3N sits below
its IDD2N (133 against 136, measured silicon -- see R6-16), so its correction
term is negative. That is the row whose own per-chip spread makes its activate
energy undetermined; this release does not change that and R6-16 stays open.

1.11.79's refusal is kept as a guard. It should no longer fire on any
supported part, and it will still catch a future row that breaks the identity.

Gate 1195A.

## 1.11.85 -- audit round 6, part eleven: three enums that took any word

**R6-20: an unrecognised value on three documented enum knobs was accepted
and then ignored, in silence.** Measured on 1.11.84, config-load scope:

    memory.controller.type: bogus      rc=0, output BYTE-IDENTICAL to `auto`
    pim.placement.connection: bogus    rc=0, silently ran shared_io
    noc.routing: BOGUS_ROUTE           rc=0, silently derived from topology

None of the three said anything. So a typo -- `ramulater`, or
`separate_endpoint` without the s -- ran a DIFFERENT model from the one asked
for and reported success.

`pim.pe.type` already refuses an unknown value with rc=1, and 1.11.76 closed
this exact class for unknown YAML SECTIONS. The enum VALUES are now brought
into line with both: each of the three refuses with rc=2 and lists what it
does accept. `noc.routing` left empty remains legal and still means "derive
from the topology"; `md1` and `weavemd1` remain accepted and still warn that
they were removed.

Found by sweeping the knobs, the same method that produced everything from
R6-10 onward. This one came from asking a simple question of four enums --
what does a nonsense value do? -- and getting three different wrong answers
and one right one.

DATA IMPACT: NONE for any configuration that was already valid. Every
documented value on all three knobs behaves exactly as before; only
undocumented ones change, from silent acceptance to a refusal. No corpus
config uses an undocumented value.

Gate 1194A.

## 1.11.84 -- audit round 6, part ten: a knob that never steered anything

**R6-19: `memory.banks` is INERT for every DRAM technology, the run never
said so, and the reference table said the opposite.**

Found by sweeping the knob, which no audit round had done. At config-load
scope on DDR4:

    banks: 16, 32, 128, 256, 1024    BYTE-IDENTICAL output
    banks: 1  against  banks: 16     differ by ONE line -- the warning itself

and the same on DDR3, LPDDR5 and GDDR6 (16 against 64: zero differing lines;
2 against 16: one). The organisation comes from the preset's derived slot
count; `memory.banks` only ever decided whether a warning printed.

So a DDR4 cell configured `banks: 16` has always simulated the preset's 128
banks, and neither the run nor the documentation said so. Worse, the
reference table's per-technology rows claimed `banks: 16` ran "16, as asked"
on DDR3, DDR4, DDR5-3200, LPDDR5 and GDDR6. Measured, it runs 64, 128, 128,
16 and 32 -- four of those five rows were wrong. The LPDDR5 row was right by
coincidence: that part's preset count IS 16, so nothing is substituted and it
is the one DRAM technology where `banks: 16` means what it says.

EVERY CORPUS CONFIG SETS `banks: 16`, so every DRAM cell's configuration
understates the device it simulated by 8x on DDR4. That is a reading of the
configs, not a defect in the results -- the runs were always the preset's,
which is the R1/R6 rule that the preset is the authority.

The substitution now announces in BOTH directions rather than only when the
request is below the per-chip minimum. The message names what was asked for,
what is used, the organisation behind it, whether the request was under the
minimum, and that the knob IS honoured for SRAM and the NVMs where no preset
fixes the organisation. The reference table is corrected in the same release
and says what it was corrected from.

DATA IMPACT: NONE. No value, no organisation and no model changes -- every
DRAM run already used the preset's count. What changes is that a run whose
`memory.banks` does not match now says so, which is every corpus cell. The
cell runner counts WARNING lines, so this will raise the count on DRAM cells
by one.

Gate 1193A.

## 1.11.83 -- audit round 6, part nine: a legal config that dumped core

**A 4-bit DRAM device, asked for with the default NoC model, aborted the
process instead of running or refusing.** `memory.dram.device_width: x4` is a
documented value, accepted by the wrapper's own validator alongside x8 and
x16, and a 4-bit device is the mainstream server DRAM part. Asked for, it
threw an uncaught `std::invalid_argument` out of the cycle-accurate H-tree
builder --

    Link width must be byte-aligned (multiple of 8) and >= 8 bits (got 4 bits)

-- and the process died with SIGABRT and a core dump, after the configuration
had been echoed and the run had started.

Found by sweeping the device-width axis, which no audit round had done. The
first cell of the sweep aborted.

MEASURED, DDR3 with x4:

    BANK placement, noc.model detailed      SIGABRT, core dumped
    RANK placement, noc.model detailed      SIGABRT, core dumped
    BANK placement, noc.model analytical    rc=0, 0.661991 W

So it is the combination that fails, not the device; a coarser placement does
not help; and the analytical model simulates this part today.

The guard inside the builder is CORRECT and stays -- Garnet's links are
byte-granular and a 4-bit rung cannot be built. What was wrong was where the
user found out. The run now refuses at CONFIG LOAD, in seconds, naming the
knob, the model that cannot take it, both ways forward (switch to the
analytical NoC, or use an 8-bit or 16-bit device), and the fact that this is
a limitation of the network model rather than of the part.

Same shape as 1.11.79, which moved a negative-energy refusal from the end of
a run to config load for the same reason: a configuration that cannot work
should say so before it consumes anything.

DATA IMPACT: NONE for any configuration that previously produced a result. No
x4 run has ever produced a number on the detailed NoC -- it produced a core
dump -- so nothing is withdrawn. x4 on the analytical NoC is untouched and
still works. No corpus cell uses x4.

Gate 1192A.

## 1.11.82 -- audit round 6, part eight: three zeros and a knob, explained

Three observability gaps found by round 6's late scans, batched because each
is a string and none changes a number.

**R6-17: the temperature knob moves less than a reader expects, and the run
said only the smallest part of it.** Swept 300/350/400 K on DDR3, DDR4 and
DDR5 -- an axis no audit round had touched. Identical behaviour on all three:
per-access array energy 1.00x, refresh EXACTLY 2.00x, background 1.1-1.3x,
leakage 20-39x. The refresh doubling is real and sourced (JESD halves tREFI
above 85 C; 400 K is 127 C). The flat array energy is by construction --
`iddFor(tech, T)` uses its temperature argument for exactly one thing,
scaling tREFI, and returns every IDD column unchanged, because those columns
are the datasheet's stated-condition values. That reading of JEDEC is
defensible and is NOT changed here. What was not defensible is that the run
printed only "leakage rows snapped to the nearest 10 C step", naming the
smallest of the three effects, so that someone setting
`power.temperature_k: 400` to model a hot device would get a bit-identical
array energy and no hint that this is what a temperature means here. The note
now states all three.

**Three technologies print a termination energy of exactly zero, and all
three are right, but only the source said why.** Found by exercising RANK
placement, which no corpus cell uses and no audit round had run: at BANK the
run correctly reports "no DQ crossing" and the whole interface path is
untouched. At RANK, DDR3/DDR4/DDR5/GDDR6 give 12.360/3.045/1.283/0.661 nJ per
read, falling by generation as they should -- and LPDDR5, HBM2 and HBM3 give
0.000. Those zeros are sourced (LPDDR5's DQ ODT default is Disable, JESD209-5C
Tbl 84; HBM rides an interposer unterminated, JESD238B cl. 9.1) but the
citations lived in the source, so in a log a sourced zero looked exactly like
an unmodelled one. The line now carries its own citation, the same way 1.11.77
made the stamps say what they overrode.

**The NoC level lines added in 1.11.80 did not name their node.** The function
has two call sites, and the system-scope one loops over DEVICE nodes, so two
devices' level lines would have run together. Device scope is unchanged.

DATA IMPACT: NONE. Three output strings and one optional argument; no value,
no model and no configuration changes. Gate 1191A.

## 1.11.81 -- audit round 6, part seven: a router that cannot be a router

**R6-10: McPAT's router input BUFFER is priced ~1300x too high at 32 nm, and
nothing said so.** Round 6 measured an 889x step in NoC dynamic power between
CACTI's 22 nm and 32 nm tables on one configuration with the access count
held equal (flits within 0.001%). It is not the tables: in the same runs the
cores scale 1.97x and the memory controller 1.45x across that step, smoothly,
off exactly those tables. Printing the router's three per-access
sub-component energies localised it to one of them:

    node    buffer          crossbar        arbiter        router area
    22 nm   2.52878e-11 J   2.43908e-12 J   3.80320e-13 J    309347
    32 nm   3.20495e-08 J   4.97276e-12 J   7.52750e-13 J    577069
    ratio      1267x            2.04x           1.98x          1.87x

The crossbar, the arbiter and the area all scale physically. The buffer does
not. The ratio that needs no cross-node comparison at all is the one inside a
single router: the buffer costs 10.4x its own crossbar at 22 nm and 6445x at
32 nm. A buffer an order of magnitude above the crossbar it feeds is an
ordinary router; three orders above is not.

WHAT IT MEANS FOR PIMID. DDR3 is the only technology whose die generation
(3x/2x) pins it to the 32 nm table, and that pin sets the McPAT node as well
as the array table. So DDR3 alone reports a fabric power about 100x too high
-- 16.73 W total and 89.07 W peak against 0.139-0.344 W and 0.53-0.74 W for
the other six technologies, which are at 22 nm and are fine. No DRAM die
dissipates 16 W of on-die fabric.

ALSO ELIMINATED, each by measurement rather than argument: traffic; geometry;
the power aggregation (every sum reconciles to 0.0004%); every parameter in
22nm.dat against 32nm.dat (largest ratio 2.2x); the `-C_junc` zero that
22nm.dat carries and the other node tables do not (restoring it moved total
power 1.3%, not 939x -- and it is upstream CACTI, never edited in this repo);
missing table sections; and the DRAM-periphery family factors.

THIS RELEASE WARNS. It does not clamp, scale or refuse. Each router now
cross-checks its buffer against its own crossbar and, above 100x, prints what
it found, what the measured ratios are on both tables, that the defect is in
the buffer solve rather than the technology inputs, that DDR3 is the only
technology this reaches, and that the number is not usable. The threshold is
a tripwire, not a model: 100x sits between the two measured ratios with an
order of magnitude of margin either side.

It warns rather than repairs because the repair is a ruling that has not been
made -- fix the buffer solve, unpin the fabric from the die generation while
keeping the pin for the array, or refuse the affected runs -- and each of
those moves numbers the corpus carries. What was not defensible was emitting
the figure in silence, which is what every DDR3 power run did until now.

DATA IMPACT: NONE. No value changes; this adds one warning line per affected
node per run. The cell runner counts WARNING lines, so affected cells become
visible in the census rather than having to be remembered.

WHAT IT FIRES ON, stated precisely. At each technology's DEFAULT node it
fires on DDR3 and on nothing else, because DDR3 alone is pinned to the 32 nm
table. It ALSO fires on any technology whose node is set coarse explicitly,
which is correct and was measured: SRAM at `technology.node_nm: 22` reports
0.509 W and does not warn; the same SRAM at 45 nm warns at 7972x and reports
255.2 W. So the guard is a node guard, not a DDR3 guard, and it protects
every configuration that reaches the broken region rather than only the one
that reaches it by default.

Gates 1190A and 1190B. The warning fires on DDR3 under 1.11.81 and does not
under 1.11.80; it carries all seven required facts; DDR4 and HBM3 at 22 nm do
not warn on either binary; device scope is byte-identical on all seven
technologies.

1190A's remaining arm failed, and the arm was wrong rather than the binary:
its list of "deterministic" quantities included the three NoC level dynamic
powers, which are activity-scaled -- runtime dynamic is per-access energy
times a measured access count -- so they move with an OMP run's cycle jitter.
1190B re-scored it on the recorded logs in two halves. The ten genuinely
deterministic quantities (per-access read and write energy, DQ interface,
refresh, three areas, the CACTI stanza) are IDENTICAL. The three NoC level
powers are checked as a band rather than an equality, and moved 2.66%, 2.66%
and 2.66% -- the same factor on all three, which is the signature of an
unchanged per-access energy scaled by a slightly different access count, and
better evidence of no model change than equality on a noisy quantity.

## 1.11.80 -- audit round 6, part six: the comment said it printed them

**R6-13: `buildNoCLevelsForMcPAT` documented that it printed every derived
parameter with its formula, and printed none of them.** The function's doc
comment had read, since it was written:

    All derived parameters (duty_cycle, chip_coverage) are printed with
    formulas and can be overridden via YAML power.mcpat_overrides.

The override half was true. The printing half was not. Across its ~290 lines
the function contained exactly one output statement, reporting how many
levels were skipped as pass-through wire; grepping all seven technologies'
full-run logs for `duty` returned nothing. Three quantities that McPAT scales
its NoC power by -- `total_accesses`, `duty_cycle`, `chip_coverage` -- were
derived from measured traffic and handed over in silence, and a YAML override
of any of them was equally silent.

This is the same defect shape the round has now closed three times: 1.11.76
made an unknown YAML section refuse instead of being discarded, 1.11.77 made
the density and timing stamps say what they overrode, and 1.11.78 removed
three stale `yaml_reference` rows. A claim in a comment is a claim.

Each emitted level now prints its three derived values WITH the arithmetic
that produced them, and says when YAML replaced one. The flat-NoC branch
prints the same line, because the stale claim was about the function and a
run down the other path must not be the silent one.

COVERAGE, STATED RATHER THAN IMPLIED: the gate exercises the hierarchical
branch on two technologies and does not exercise the flat one. That is not a
hole that can be closed with a config -- `hierarchy_enabled` is set false in
exactly one place, when `createInternalDRAMNetwork()` returns null, so the
flat branch is a failure fallback and no corpus cell reaches it. Its print is
defensive and untested, and is declared here as such.

**And the range nobody was holding it to.** `duty` is documented in this same
function as a "fraction of peak bandwidth [0,1]" and nothing checked it
against that. McPAT scales a level's peak/TDP term linearly with it, so a
level whose duty came out above 1.0 would be priced past saturation -- and
with nothing printed, silently. It now WARNS, naming the level and the three
inputs (accesses, net cycles, nodes) that produced the value.

It deliberately does NOT clamp. A clamp would move a number the paper
carries, on a path that has never been seen to fire, and this project's rule
is to announce a substitution rather than perform one quietly -- the rule
1.11.79 followed when it refused a negative energy instead of guessing an IDD
value.

DATA IMPACT: NONE. No derived value changes; the release adds output lines
and one conditional warning. On every shape measured in round 6 the duty is
orders of magnitude below 1.0 (roughly 4e6 packets shared across levels
against 1.2e7 cycles x 9 nodes), so the new warning is latent on the current
corpus. Verified by gates 1189A and 1189B.

1189A scored 8 arms PASS and one FAIL, and the FAIL was the arm's fault, not
the binary's: it demanded that a 1.11.80 full-run log be BYTE-IDENTICAL to a
1.11.79 one once the new lines were stripped. All 315 differing lines were
measured per-run quantities from a nondeterministic OMP run -- per-core cycle
and latency counters, the phase-sampled `[ChanBW]` and `[GarnetBatch]` lines,
the `[E17]` gap histograms (whole-run cycles 437390000 against 437410000,
0.005% apart), the scheduler watchdog line, and a `.topo` filename carrying
the PID. Full-run OMP cycles are not bit-stable, so byte-identity was the
wrong standard to hold them to.

1189B replaced that arm with two that assert the same claim correctly:
DEVICE SCOPE, which IS bit-deterministic, byte-identical across all seven
technologies (including DDR5, where both binaries refuse the default 4800
grade and the arm asserts matching behaviour rather than success); and the
twelve deterministic `--power` quantities that appear only in a full run --
per-access read and write energy, DQ interface energy, refresh, three area
figures and the CACTI stanza -- all identical. `Background` was excluded from
that list, with cause: it is weighted by the measured power-down residency
and so inherits the gap histogram's jitter, 478.689 against 478.683 mW.

## 1.11.79 -- audit round 6, part five: a negative energy stops being a number

**R6-11: DDR5 at its default grade reports a NEGATIVE array energy on
workloads that miss the row buffer often enough, from a per-activate energy
that is negative on every workload.**
A CORRECTION TO THE FIRST STATEMENT OF THIS FIX, made before it shipped. The
sign is workload-dependent, because the activate term is weighted by the
MEASURED row-buffer miss fraction. Same device, same placement, same bank
count, same preset, only the workload size changed:

    stream_triad 1 000 000   miss fraction 0.76693    read -0.344 nJ
    stream_triad   200 000   miss fraction 0.625946   read +0.984 nJ

That makes the defect worse rather than milder: the same unphysical
per-activate energy sits behind both, and on the positive cell it is
invisible. It also means this release refuses more than the cells that would
have printed a negative -- the refusal reads the IDD row at config load, so
it blocks every DDR5-4800 and DDR5-5600 run regardless of access mix. That is
deliberate: a negative activate energy is not physical whatever happens to
mask it downstream.
 Found by the seven-technology full-run smoke on 1.11.78, which runs
each fig3 shape with `--power`: DDR5 printed `Per-access: read=-0.344 nJ,
write=-1.164 nJ` and `Total dynamic: -5.5 mJ`. DDR3 and DDR4 on the same
shape printed 100.0 and 66.4 mJ.

The cause is arithmetic, and it is exact. Micron TN-41-01's activate and
precharge energy is

    E = Vdd x (IDD0 x tRC - IDD3N x tRAS - IDD2N x (tRC - tRAS))

which presumes IDD0 -- a cycling one-bank activate/precharge current --
exceeds the standby currents it subtracts. Seven of this tree's nine IDD rows
satisfy that. The two 16 Gb DDR5 rows added in 1.11.66 from the held MT60B
addenda do not:

| row | IDD0 | IDD2N | IDD3N | IDD0 > IDD3N |
|---|---:|---:|---:|---|
| DDR5-3200 (8 Gb) | 55 | 34 | 42 | yes |
| **DDR5-4800 (16 Gb, the default)** | **103** | **92** | **142** | **no** |
| **DDR5-5600 (16 Gb)** | **53** | **49** | **91** | **no** |
| DDR4 / DDR3 / LPDDR5 / GDDR6 / HBM2 / HBM3 | | | | yes |

At 4800B: `IDD0 x tRC = 103 x 48.256 = 4970` against
`IDD3N x tRAS + IDD2N x (tRC - tRAS) = 6041`, so the term is -1178 pJ and it
dominates the positive burst term.

**Which side is wrong needs a ruling, so this release does not guess.** Either
the rows mis-transcribe the addenda, or they are right and TN-41-01 does not
transfer to a 32-bank DDR5 part whose all-banks-active IDD3N can legitimately
exceed a one-bank IDD0. Both readings need the datasheet. What is not in
question is that a negative energy must not be printed as though it were a
measurement, so the run now REFUSES, naming the technology, the value, the
formula, the row's own currents and both possible causes.

**This BLOCKS DDR5 at grades 4800 and 5600**, which is deliberate and is the
point: the corpus's DDR5 is 4800, and those cells were producing a negative
energy into the CSV. DDR5 at 3200 is unaffected (its row is physical), as are
the other six technologies -- verified, all seven load clean except the two
DDR5 grades that cannot produce a physical number. The refusal is raised at
CONFIG LOAD, not when the power report is finally printed, so a corpus cell
fails in seconds instead of simulating for hours and then aborting.

If the preferred outcome is instead to keep DDR5 cycles and withhold only the
energy, that is a smaller change on the same check -- say so and it will be
made; it was not assumed, because a partially-filled power report is easier
to misread than a refusal.

## 1.11.78 -- audit round 6, part four: three documentation rows that had gone stale

Documentation only -- no source file outside `docs/` changes, so every number
and every run is identical to 1.11.77. Round 6's last lane checked the
`yaml_reference` defaults column against the code constructor. Most of it is
accurate (scope, memory.technology, pim.pe.count, pim.pe.frequency_mhz,
pim.placement.level, noc.model, technology.node_nm and power.temperature_k
all match). Three rows did not.

- **`memory.banks`** said "DRAM techs enforce minimum (DDR4 >= 16/chip)",
  naming one technology and leaving out the consequence. Three technologies
  SUBSTITUTE their full count when a config asks for less, and one of them
  joined the group in 1.11.72: at grade 4800 or 5600 a DDR5 config that says
  `banks: 16` simulates 256. The row now carries the measured per-technology
  minimum and what `banks: 16` actually runs, for all eight cases including
  both DDR5 grades.
- **`memory.dram.device_width`** documented its default as `x8`. The code
  default is UNSET, which resolves to each technology's own JEDEC default --
  x8 for DDR3/DDR4/DDR5 but x16 for LPDDR5 and GDDR6, which have no other
  preset upstream. The row now says so, notes that HBM refuses the key
  outright, and records that an unrecognised value is refused since 1.11.76.
- **`memory.subarrays_per_bank`** read "Subarrays per bank" with a default of
  `4`. The 4 is a starting value that the run replaces: DRAM derives the
  count from the preset's `bank_rows` over the subarray height, and since
  1.11.73 a non-DRAM technology takes it from its own array model. The row
  now says that, that setting the key overrides the derivation, and that the
  family spellings `subbanks_per_bank` and `mats_per_bank` exist since
  1.11.74.

## 1.11.77 -- audit round 6, part three: every stamp says what it overrode

Two findings, both from asking the same question 1.11.72 raised: which of
this object's literals does the preset overwrite, and does anyone see it
happen? No number moves anywhere -- the only change to any run's output is
four added NOTE lines across three technologies.

**R6-9: three stamps write over the architecture object, and two of them did
it in silence.** `applyPresetBankGroupingToArchitecture()` has announced its
override since 1.11.72 -- that release exists because a silent one let an
object literal describe a different part for six releases. Its two siblings
did not:

- The DENSITY stamp's announcement was guarded by `dt != "DDR4" && dt !=
  "DDR5" && not HBM`, and said "<tech> has no architecture object ... every
  other field is still DDR4-2400's". Both halves are false: DDR3, LPDDR5 and
  GDDR6 have owned objects since 1.11.68/69/70, and the exclusion hid the one
  place the stamp actually moves something -- at the default grade DDR5
  simulates the 16 Gb part while its object literal says 8 Gb, so every
  default DDR5 run silently re-stamped 1024 -> 2048 MB.
- The TIMING stamp never announced at all. Measured, it moves three of the
  seven: DDR5 15/15/15/32.5 -> 16.224/16.64/16.224/32.032 ns (its literal is
  the 3200AN bin, the run simulates 4800B), HBM2 16/16/12.5/28 ->
  16.66/16.66/14.994/33.32, HBM3 16/16/10/24 -> 16.25/16.25/16.25/33.125.
  Those are exactly the literals 1.11.66 replaced -- the numbers have been
  right since then, and the correction has been invisible since then too.

Both now announce, once per technology and preset, in the same shape as the
grouping NOTE: old values, new values, the preset they came from. Silence
means the literal already agreed, which is true for DDR3, DDR4, LPDDR5 and
GDDR6, and for DDR5 at grade 3200. Output cost: DDR5 +2 lines, HBM2 +1,
HBM3 +1, the other four unchanged; nothing removed, and every added line is
a NOTE.

**R6-8: two objects were never checked against their own preset.**
`applyPresetTimingsToArchitecture()` returned early for LPDDR5 and GDDR6,
under a comment reading "LPDDR5 and GDDR6 own no object (they borrow DDR4's
as an organization proxy)". That was true when it was written in 1.11.66 and
false from 1.11.69 and 1.11.70, which gave each of them one. The list was
never extended -- while the `owns_object` line eight lines below it already
named them, so one function disagreed with itself about which technologies
own an object. They were therefore the only objects in the tree whose timing
literals were never verified against the preset they claim to describe. No
number was wrong, because the per-technology getters short-circuit to
`preset_timing_` for those two; what was missing is that the object agreed
with the getter by luck rather than by construction. Extending the list is
byte-for-byte identical on all seven technologies, which is both the proof
that the literals were right and the reason it is safe. Three stale comments
asserting the vanished DDR4-proxy arrangement are corrected with it.

## 1.11.76 -- audit round 6, part two: a misspelled section is no longer silent

Four items from the same round (`_1166audit/R6_findings.md`). No number
moves: all seven DRAM technologies are byte-identical against 1.11.75 in
device scope, and every config in this tree still loads (14 of 14 checked,
including the three shipped co-simulation examples).

**R6-7: a misspelled top-level section was silent, and it discards everything
inside it.** PIMID reads its YAML key by key and never asks what it did not
read, so `memroy:` for `memory:` gave rc 0, no warning, and a run in which
every memory setting -- technology, banks, device width, speed grade -- fell
back to its default. The run is now refused, naming the section and listing
the fourteen the parser reads. That is the same rule 1.11.57 (B036) applied
to a value that could not be honoured, raised to a whole section.

Scope, stated plainly: this catches a misspelled SECTION, not a misspelled
key inside one. `memory.bankz`, `pim.placemnt` and `pim.pe.frequenci_mhz` are
still accepted in silence. A complete key schema cannot be written correctly
by hand here -- about 250 of the tree's YAML reads go through intermediate
node variables rather than the root chain, so a hand-built whitelist would be
incomplete and would refuse or warn on valid keys, which is worse than the
silence it replaces. The nested case stays an open finding; for the fleet the
cheap and safe guard is generator-side, comparing each generated config's key
SET against a reference of the same shape, and the re-sim plan's pre-flight
now says so instead of claiming the validator already catches this.

**R6-L1: one function validated one of its two arguments.**
`setRunWideKnobs()` refused a bad DDR5 grade but recorded `device_width`
raw, leaving it to be validated a few hundred lines later; a wrapper built in
between took `presetWidthBits(w, 0) -> 0` and fell back to x8 in silence. The
tech-independent half of the check now runs where the knob is recorded; the
per-technology legality (LPDDR5 x16 only, GDDR6 no x4) stays where the
technology is known.

**R6-L2: `probeNonDramL0()` returned in silence** when the model factory
declined, while every other failure path in it prints why the tree kept an
unsourced shape. It now says so.

**R6-5: the two families' L0 bandwidths are computed on different bases, and
the run now says which.** SRAM divides its subbank width by CACTI's random
cycle time, the throughput bound; NVSim reports latencies and energies only
-- there is no cycle or restore time anywhere in its `FunctionUnit` -- so an
NVM mat divides by its share of the bank READ LATENCY and is optimistic by
whatever the array's restore costs. Both are the best each tool offers, and
printing the basis keeps a reader from comparing 31.4 GB/s against 19.4 GB/s
as though they were the same quantity.

**Also recorded, NOT fixed (needs a ruling).** Now that L0 is sourced for the
non-DRAM families, the per-technology table that still supplies levels 1 and
up contradicts the same array model at the very next rung: SRAM's L1 bank
link is 64 bits at 20 GB/s in the table, while CACTI gives that bank a
512-bit word every 2.037 ns, i.e. 31.4 GB/s. The table was equally unsourced
before 1.11.73, so this is not a regression -- but the disagreement is newly
visible and it sits at the corpus's own placement tier.

## 1.11.75 -- audit round 6: the cap asks for a time, and each bridge side follows its own level

Audit round 6 (`_1166audit/R6_findings.md`) covers the 1.11.66..1.11.74 diff
(27 files, ~2100 insertions) plus two tree-wide sweeps of defect classes the
1.11.73 CACTI find exposed. Two REAL findings, both fixed here; two DISCUSS
items recorded for a ruling; ten areas audited clean and listed so the next
round does not re-walk them.

**R6-2: the non-DRAM memory-controller bandwidth was built from a host cycle,
not from the array.** The `[bw] <tech> M/D/1 cap` derivation called
`getMemoryLatencyCycles()`, which ends in `round(latency_ns * freq / 1000)`
clamped to at least 1, and then converted that whole cycle back to
nanoseconds. At the corpus's 500 MHz the quantum is 2 ns -- the same order as
an entire non-DRAM array access -- so the round trip did not round the number,
it replaced it. The cap is written into the generated ZSim config as the
memory controller's `bandwidth`, so it is the service rate of every fig2 cell.
Measured, fig2 shape (256 banks, 64 B line):

| tech | array model | cap used | cap now | was |
|---|---|---|---|---|
| SRAM | 0.21856 ns | 2.0 ns | 74 963 426 MB/s | 8 192 000 (9.1x low) |
| PCM | 2.83215 ns | 2.0 ns | 5 785 004 MB/s | 8 192 000 (41.6% high) |
| STT_MRAM | 3.29773 ns | 4.0 ns | 4 968 266 MB/s | 4 096 000 (17.6% low) |
| RERAM | 3.48713 ns | 4.0 ns | 4 698 419 MB/s | 4 096 000 (12.8% low) |

SRAM's was a clamp rather than a rounding: 0.219 ns is 0.109 cycles, which
rounds to zero and is then raised to one whole cycle. `getMemoryLatencyCycles()`
now reports the unquantised nanoseconds through an optional out-parameter; the
return value still rounds, because ZSim needs an integer, and the rounded
figure remains the fallback with a NOTE if a model declines to report a time.

**R6-1: the non-DRAM sourced L0 link never reached its bridge.** 1.11.57
(C005) established that "bridge[i] spans level i and level i+1, and its
ingress and egress links ARE those two levels' links", but the loop wrote
BOTH sides only when BOTH adjacent levels were sourced -- invisible while the
only caller sourced all seven levels at once. 1.11.73 gave SRAM and the NVMs a
sourced L0 by populating index 0 alone, so level 0 was re-described and bridge
0 was not: measured, SRAM's L0 moved 128 -> 512 bits and STT-MRAM's 64 -> 512
while the bridge ladder stayed `5/5/5/5/5/5`, bridge 0 still serialising over
the table's 64-bit ingress at the table's clock. The two halves of one
boundary described different buses -- the defect C005 exists to prevent,
reintroduced for the non-DRAM families by this project's own release. Each
side now follows its own level independently, which is bit-identical when
every level is sourced and correct when only some are. Bridge 0 on SRAM goes
5 -> 6 PE cycles: its router term now ticks at the subbank's own 0.49 GHz
(512 bits per CACTI cycle time) instead of the table's assumed clock.

**Measured.** All seven DRAM technologies are byte-identical in device scope
against 1.11.74 -- the DRAM ladder sources every level, so the bridge change
is a no-op there, and the bandwidth fix touches only the non-DRAM branch.
DDR5 in system scope is byte-identical too.

**Neither fix moves the corpus, and that is measured, not assumed.** fig2 is
BANK placement, so bridge 0 sits below the PE and is not traversed; and the
cap, though it was wrong by up to 9.1x, does not bind at the fig2 working
set. SRAM full-run cycles are bit-identical (1 229 802 both). PCM was run
3 x 3: new 1 257 542 / 1 257 542 / 1 260 057, old 1 257 542 / 1 260 078 /
1 257 542 -- the two populations interleave, the means separate by 0.0006%
against a within-binary spread of 0.20%, and the single-pair -0.40% the gate
first reported was scatter. What the fixes correct is a number the run
reports and emits into the generated ZSim config as the memory controller's
bandwidth; it would bind on a bandwidth-bound configuration, and it is wrong
in the ledger and in the emitted config until fixed either way.

**Recorded for a ruling, NOT changed here.** (1) One SRAM array is
characterised twice in a single run, 11.2x apart, and both numbers are live:
the flat path asks CACTI for a 1-way RAM (0.2186 ns) and feeds the bandwidth
cap, while the plugin `SRAMModel` asks for an 8-way cache in 8 banks
(2.4404 ns) and feeds the PE tier latency. This is the re-sim plan's decision
6, now quantified. (2) DDR5 is modelled as one 64-bit channel while JESD79-5
defines two independent 32-bit sub-channels per DIMM; unlike GDDR6, whose two
channels sit inside one device, DDR5's are formed from different device groups
and Ramulator2's own organisation is `Ch = 1`, so the present value describes
what is simulated and the gap belongs in the documentation.

## 1.11.74 -- the tier below the bank is named consistently everywhere

Follow-up to 1.11.73 after the user's review (2026-09-20): "make sure the
namings are correct, and remove any residuals." No number moves.

**What was still wrong.** The SRAM and NVM models still exposed their L0
tier through accessors named `getSubarrayReadLatency` /
`supportsSubarrayPIM` (SRAM) and `getSubarray{Read,Write,SetWrite,
ResetWrite}Latency` / `supportsSubarrayPIM` (NVM); the NVSim wrapper still
carried `getSubarrayLatency()` -- NVSim's sub-mat subarray, a tier below the
one we stop at, with no caller left; the L0 network factory and its table
were `createSubarrayNetwork` / `getSubarrayParams` for every family; the
network factory printed "Subarrays per bank" for SRAM and NVM; the SRAM and
STT-MRAM architecture headers described a Chip -> Bank -> Mat -> Subarray
ladder and kept an uncalled subarray-to-subarray H-tree helper; and the two
other YAML places that name L0 -- the count `memory.subarrays_per_bank` and
the `noc.levels` key `subarray` -- accepted only the DRAM word.

**Now.** SRAM: `getSubbankReadLatency`, `supportsSubbankPIM`. NVM:
`getMat*Latency`, `supportsMatPIM`. NVSim wrapper: the sub-mat latency
accessor, its cached member and its XML field are gone (an older cache file
carrying the field is ignored). Network: `createLevel0Network` /
`getLevel0Params`, and the factory prints Subbanks / Mats / Subarrays per
bank by family. Headers: SRAM is Chip -> Bank -> Subbank, with Mat and
Subarray marked CACTI-internal geometry; STT-MRAM is Chip -> Bank -> Mat,
with the subarray marked NVSim-internal; the dead H-tree helpers are
removed (DRAM's stays, DRAM's tier IS the subarray). YAML:
`memory.subbanks_per_bank` (SRAM) and `memory.mats_per_bank` (NVM) are
accepted beside the legacy `subarrays_per_bank`; `noc.levels.subbank` /
`noc.levels.mat` beside `subarray`; exactly one of each, the wrong family's
word is refused, the legacy word on SRAM/NVM prints the same one-line NOTE
the placement level does, and the L0 probe reports which key set the count.
CACTI's and NVSim's own internal names (`getSubarrayRows/Cols`,
`getSubarraysPerMat`, `subarray_output_drv_ns`, `wordlines_per_subarray`)
are kept: they are the tools' terms for geometry below the tier and are
labelled as such, not tiers.

**Measured** (gate 1184A, run 385709 against a full 1.11.73 runtime
snapshot; three arm criteria corrected on the recorded logs: a regex that
took the 0 in "L0" as the count, a factory print that --print-mem-info
never reaches, and the old snapshot logging its own cache path). Seven
DRAM technologies byte-identical; SRAM identical; STT-MRAM, PCM and ReRAM
identical apart from the cache-path provenance lines. Aliases:
`memory.subbanks_per_bank: 4` on SRAM sets the count to 4 and says which
key did (the old binary ignored the key and kept 8); the same key on PCM
is refused; the legacy key on STT-MRAM prints the NOTE and applies;
`noc.levels.mat` on STT-MRAM applies (old binary: ignored); `noc.levels.
subbank` on DDR4 refused; two L0 keys refused. Full runs: SRAM SUBBANK
cycles identical to the old binary; PCM MAT cycles within 0.17% with the
placement provenance now "NVSim read path @ mat" (old: "@ subarray").

## 1.11.73 -- one tier below the bank, and it is the family's own

User ruling (2026-09-20), after the inside-the-bank discussion: go exactly
ONE level below the bank for every technology, no more -- SRAM has the
subbank, the NVMs have the mat, the DRAMs have the subarray.

**The defect.** Below the bank, the SRAM and NVM models described tiers the
parts do not have and left the tier they do have unsourced. The SRAM
extractor carried TWO levels (`subarray_access_ns`, `mat_access_ns` = the
same number plus the H-tree), named after CACTI's internal geometry rather
than any placement tier; its datapath widths were literals (128 / 64 bits);
its "mats per bank" accessor returned CACTI's active-mat count under the
wrong name. The NVM extractors priced L0 from `bank->mat.subarray.
readLatency` -- the CACTI-style slice BELOW the mat -- while the mat itself
(the unit NVSim H-trees into a bank) was cached but unused; mat width and
mat count were never cached, so from the pregenerated cache they were
unknown and were filled by literals (64 bits; 8 or 4 per bank). And the
placement tree took its non-DRAM L0 count from the UnifiedConfig
constructor's 4 and its L0 link from a per-technology table row (SRAM 128
b / 40 GB/s, NVM 64 b / 9.6-12 GB/s) that described no part. Every level
below the bank was called "Subarray" regardless of family.

**The fix.** One tier below the bank per family, named, counted, sized and
priced from the family's own tool:
- SRAM: `SUBBANK` -- CACTI's line of `num_act_mats_hor_dir` mats activated
  together for one data word. Its width IS the bank's (`parameter.cc`:
  `num_do_b_subbank = out_w`), its count is CACTI's mats-per-bank over
  mats-per-access (Ndwl x Ndbl / num_submarray_mats / num_active_mats), its
  latency is the in-array component path already extracted (a PE at the
  subbank skips the bank H-tree), its bandwidth is `out_w` per CACTI random
  cycle time. `SRAMTiming.subbank_access_ns` replaces the subarray/mat
  pair; the mat energy multiplier (x1.3) is gone with the tier.
- NVM (STT-MRAM, PCM, ReRAM): `MAT` -- NVSim's mat. Latency from the
  already-cached `bank->mat.readLatency`; width `mat.numDataBit` and count
  `numRowMat x numColumnMat` are now captured live AND cached (two new XML
  fields, `-1 = absent`); bandwidth is the mat's share of the NVSim bank
  read bandwidth (mat width / word width). All `subarray_*` timing, energy
  and datapath fields of the three NVM architectures are renamed `mat_*`.
  An entry written before this release cannot source width or count: the
  run says so, keeps the tree shape it has as an UNSOURCED count, and
  REFUSES to place PEs at `MAT` until the cache is regenerated or
  `memory.subarrays_per_bank` states a count (labelled user-set, as on
  DRAM). No literal stands in.
- DRAM: `SUBARRAY`, unchanged.
- The plugin contract gains `l0UnitsPerBank()`, `l0WidthBits()`,
  `l0BandwidthGBs()` (-1 = not sourceable) and `tierName(Tier, tech)`;
  `Tier::SUBARRAY` stays the L0 enum. The tree's non-DRAM L0 count and link
  now come from the model (`probeNonDramL0`, `applySourcedLadder` on level
  0 only; levels 1+ keep the table and the run says so). `levelName`, the
  McPAT NoC level names and the hierarchy summary print the family's word.
- YAML: `pim.placement.level` accepts `SUBBANK` (SRAM) and `MAT` (NVM);
  `SUBARRAY` on SRAM/NVM is accepted as the legacy spelling with a one-line
  NOTE; `SUBBANK` on a non-SRAM part or `MAT` on a non-NVM part is refused
  (before this release an unknown level word fell silently to BANK).

**Measured** (device scope, corpus per-bank unit 64 KB, 64 B line, 22 nm,
350 K). SRAM: CACTI puts the whole 512-bit word in ONE line of mats per
CACTI bank, and the plugin model runs CACTI with the 64 KB unit split into
8 banks, so the PIMID bank has 8 subbanks (8 CACTI banks x 1), each 512
bits wide, 31.4 GB/s (out_w per 2.04 ns random cycle); the table said 4 x
128 b / 40 GB/s. NVM: the regenerated
NVSim characterizations give ONE mat per 64 KB bank, `numDataBit` 512. In
NVSim the bank supplies the whole word and each ACTIVE mat carries word /
active-mats of it; with a single mat the share is the whole word, so here
the mat link equals the bank read bandwidth (a larger bank would show
narrower mats and a proportionally smaller mat link): STT-MRAM
19.4, PCM 22.6, ReRAM 18.4 GB/s; the table said 4 x 64 b / 12, 9.6, 11.2
GB/s. Regeneration moved no bank-level number (mat latency 3.30 / 2.83 /
3.49 ns and the bank figures reproduce the old cache to printed precision).
So for the part the corpus characterizes, the tier below the bank is
near-degenerate: full-width units, one per NVSim bank and one per CACTI
bank -- which is what the tools say for this array, not a preset. DDR4, DDR5
and HBM3 are byte-identical against 1.11.72; the four non-DRAM BANK cells
are identical in every key line (array latencies, L1+ links, bridge ladder).
The one visible change at BANK placement is the L0 entry of the printed
level-latency ladder (its link is now the model's, e.g. SRAM 2 -> 4 PE
cycles), a tier below the PE that a BANK traversal does not cross; gate 1183
M1 checks that on full-run cycles. On the old cache MAT placement is
refused, as designed.

**Found by the gate's full run, FIXED here: the SRAM component fields were
read out of a struct CACTI never fills.** Gate 1183A's first SUBBANK full
run took 2.07e10 cycles (BANK: 1.23e6) with a placement latency of 5.5e+05
ns. Every per-component CACTI accessor -- decoder, bitline, sense-amp,
subarray-output and H-tree delays, the matching energies, array/column/
wordline leakage, cell/subarray area -- read `uca_org_t::data_array`, the
`results_mem_array` copy that the `cacti_interface()` path never populates,
so they returned whatever memory held (measured: decoder 217 ns, subarray
output driver 550 us, sense amp 36 fs, H-tree 0.4 fs, against an access
time of 2.44 ns from the same result). Latent since 1.11.23 introduced the
component sum; unreachable in any corpus cell because only SUBARRAY
placement on SRAM priced it, and the printed "Inner-bank datapath latency:
550220 ns" was in every SRAM log unread. All of them now read the chosen
solution's live `mem_array` (`data_array2`, the object `getAccessTime()`
belongs to); the asserted "wordline = 30% of bitline" term added on top of
CACTI's own sum is gone (CACTI folds wordline drive into its decoder/bitline
stages); and the extractor refuses the SUBBANK tier (-1, unsourceable) if
the in-mat sum is not below the array access time. Measured after the fix:
in-mat path 0.83 ns + H-trees 0 ns (CACTI builds no H-tree stage at this
8-bank geometry) against access 2.44 ns, residual 1.61 ns in routing and mux
stages; the SUBBANK full run completes at ordinary cycle counts. No BANK
number depends on these accessors (bank latency is `getAccessTime()`, bank
leakage is `getLeakagePower()`), which gate 1183A's B1/M1 arms check.

**Found alongside, NOT fixed here (DISCUSS before the fleet):** the SRAM
plugin model characterizes the 64 KB per-bank unit as an 8-way
set-associative CACHE split into 8 CACTI banks (`SRAMModel` defaults,
`is_cache = true`), while the flat path in `getMemoryLatencyCycles`
characterizes the same unit as a 1-way RAM ("SRAM as memory, not cache",
`is_cache = false`, CACTI's default bank split). Since 1.11.25 the fig2 SRAM
BANK latency has come from the plugin model, i.e. from the cache-mode run
(tag path included). Aligning the model to RAM mode changes every fig2 SRAM
number and the subbank count (the split would then be CACTI's own), so it is
a separate release under a user ruling, not a rider on this one.

**Corpus impact.** fig2 cells are BANK placement: the L0 tier is below the
PE and is never traversed, so device-scope output is numerically identical
except the new L0 lines and the L0 name; DRAM is byte-identical. Only
SUBBANK/MAT-placement runs (none in the corpus) change. The NVSim cache for
the four corpus NVM parts is regenerated on a compute node as part of this
release.

## 1.11.72 -- DDR5 counts all its banks

Found while tabulating the hierarchy under the bank for the pre-fleet
discussion (2026-09-20): the fourth thing the 1.11.66 grade knob did not
reach, after 1.11.67's three.

**The defect.** 1.11.66 moved DDR5's default part to 4800B / 16 Gb. That
part has 8 bank groups x 4 banks = 32 banks per chip (JESD79-5D Table 4;
Ramulator preset DDR5_16Gb_x8), and the timed device, the transcription and
the live shape check all agreed on 32. But the architecture object's
literals still said 2 banks per group (the 8 Gb part), and so did main.cpp's
per-technology table. Three consumers read those instead of the
transcription: `getBanksPerBankGroup()`/`getBankGroupsPerChip()` -- the
power population and the system-scope placement oracle -- and the placement
tree itself. So every default-grade DDR5 cell put its 16 PEs over a tree of
128 bank organisations on a 256-bank device, priced 16 banks per chip in the
power model, and printed a tFAW note for half the streams. Bank SIZE was
coincidentally right (16 Gb / 32 = 64 MB = 65536 rows x 1 KB), which is why
nothing downstream complained. The tree-coverage invariant could not see it:
both of its inputs came from the same table -- the "verified only against
itself" case its own comment warns about.

**The fix, the R1 pattern.** The preset row is the authority. The
transcription now carries the grouping (`bank_groups`, `banks_per_group`),
checked at construction against its own bank count and at run time against
the device Ramulator instantiated (pseudo-channels x bank groups, and banks
per group); `applyPresetBankGroupingToArchitecture()` stamps the object
beside the density stamp; and main.cpp's DDR5 row follows the grade
(`banks_per_bg = grade == 3200 ? 2 : 4`). Every technology is stamped; for
six of seven the object already agreed and nothing moves.

**Measured.** DDR5 at the default grade: the tree now covers 256 bank
organisations (was 128); at grade 3200, 128 as before; x16 covers 64 (was
32). Because the 16 Gb chip has 32 banks, the corpus shape `banks: 16` now
trips the same minimum-bank guard HBM2/HBM3 already trip, and num_banks
becomes the technology's 256 (it was accepted at 16 before) -- every
default-grade DDR5 cell changes shape, not just its tree. Cycles: a 3 x 3
A/B at 100k elements moved +2.7% with the row-miss fraction identical to six
figures; the gate's 3 x 3 at 1M elements moved the mean +0.18% inside a 7.4%
within-binary spread. The cycles movement is therefore size-dependent and
NOT separable from OMP run-to-run scatter -- it is reported, not claimed;
the change is structural and its evidence is device scope. DDR3, DDR4,
LPDDR5, GDDR6, HBM2 and HBM3 are byte-identical in device scope.

**Found alongside, NOT fixed here (latent, pre-existing since 1.11.67 made
DDR5's ladder adoptable):** `memory.dram.device_width: x4` aborts on DDR5 --
the x4 chip-DQ rung is 4 bits and the Garnet link builder refuses a link
under 8 bits (`Link width must be byte-aligned`). Same on 1.11.71. Not a
corpus configuration (x8 throughout); recorded in the fleet's open list.

Data impact: DDR5 at grades 4800 and 5600 -- every such cell's
element-to-organisation mapping, per-bank power population and tFAW
accounting change; cycles move a few percent. Grade 3200 and every other
technology are unchanged.

## 1.11.71 -- two silences, made audible

Two small items, both about the run telling the truth about itself; neither
moves a number.

**A links[] entry without src or dst is refused (E-queue N10).** In
`system.network.links[]` both endpoints defaulted to the empty string and the
entry was kept, so a block with a missing or mistyped endpoint matched no node
pair and its bandwidth, latency and type overrides were discarded without a
word -- the run used the default link and reported nothing. A user who writes
a links entry is describing the fabric they want measured; dropping it
silently is the same class of defect as the unknown-key silence. It is now
FATAL, naming which endpoint is missing, matching the `lanes` refusal that
already sits in the same block.

**The early "In-Memory Network Initialized" block says it is provisional.**
The last piece of audit B006. InternalDRAMNetwork prints its per-level widths
from its constructor, which runs before main.cpp decides whether the run's
architecture object reconciles with its preset; when it does -- and since
1.11.70 that is every technology -- buildHierarchy replaces every one of those
widths with the sourced ladder. 1.11.57 added the superseding line
("Hierarchy link ladder from the <tech> architecture object") but left the
earlier block unlabelled, so a reader or a log parser still met two different
fabrics under one technology name with no indication which one ran. The block
now says, at the place that prints it, that its widths are the per-technology
table defaults and that the adopted ladder below supersedes them.

Data impact: none. Every technology is byte-identical in device scope apart
from the label text itself; the N10 refusal only reaches a configuration
that was previously silently mis-simulated.

## 1.11.70 -- GDDR6 stops borrowing DDR4's shape, and a latent shape error falls out

Last of the three object-less technologies (audit 1.11.57 B001). With this
release EVERY technology in the lineup owns an architecture object, and the
DDR4-proxy branch is unreachable for the shipped seven; it stays only as the
announced fallback for a technology added without one.

`createGDDR6_14000_Verified()` is transcribed from JEDEC JESD250D section 4.1
Table 19, the 8 Gb x16 column: 2 channels per device, 4 Gb per channel, array
pre-fetch 256 bits per channel, BA[3:0] = 16 banks per channel, R[13:0] =
16384 rows, C[5:0], page size 2 K. Same part as the preset GDDR6_8Gb_x16.
The prefetch width is stated twice in the standard (section 2.1 features and
Table 19), so it is VERIFIED here as it is for LPDDR5. The clock follows
GDDR6's own divisor, tCK = 8E6 / rate = 571 ps, which 1.11.66 established:
the object's core clock is 1751 MHz where DDR4's proxy said 1200.

**THE INVARIANT CAUGHT A LATENT SHAPE ERROR, and it was fatal on first
contact.** With the ladder adoptable for the first time, the channel-anchored
projection built the tree from the preset's TWO channels and covered 32 bank
organisations, while the slot count derived 16 -- the tree-coverage assertion
refused the run outright. The cause was not in the new object: main.cpp's
per-technology table gave GDDR6 `chips_per_rank = 1`, although that field
carries channel multiplicity in this tree and the table's own note says so
four lines below for HBM ("chips_per_rank = channels per stack, HBM2 8,
HBM3 16"). GDDR6 was the one multi-channel part left at 1. It is now 2, per
JESD250D 4.1: "GDDR6 addressing is defined for a single channel with devices
having 2 channels/device". The error could not fire before this release,
because GDDR6 ran on the placeholder ladder and the tree was never built from
the preset's channels.

Data impact: GDDR6 only. Its ladder is adopted for the first time (16-bit
channel widths replacing DDR4's 64-bit ones), the reported per-channel
bandwidth is 28 GB/s against an aggregate of 56 over two channels, the core
clock moves 1200 to 1751 MHz, and the placement tree now spans 32 bank
organisations rather than 16 -- the device's real bank count across both
channels, which changes the element-to-organisation mapping for every GDDR6
cell. DDR3, DDR4, DDR5, LPDDR5, HBM2 and HBM3 are byte-identical in device
scope.

## 1.11.69 -- LPDDR5 stops borrowing DDR4's shape

Second of the three object-less technologies (audit 1.11.57 B001); see
1.11.68 for the shape of the defect. `createLPDDR5_6400_Verified()` is
transcribed from JEDEC JESD209-5C section 2.2.4 Table 6, the 8 Gb column of
"x16 Mode Addressing for BG Mode (4 Banks / 4 Bank Groups)": 4 banks per
group, 4 bank groups, 32768 rows, 2048-byte page, array pre-fetch 256 bits.
That is the part the preset LPDDR5_8Gb_x16 simulates.

**Better sourced than the DDR objects in one place.** JEDEC publishes
LPDDR5's array pre-fetch width (Table 6, "Array Pre-Fetch 256"), so the
global sense-amplifier datapath here is VERIFIED rather than inferred from
DAS-MICRO'15 as it is on DDR3 and DDR4, and it is four times the DDR figure.
The burst arithmetic agrees independently: B0-B3 over x16 is also 256 bits.

**LPDDR5 IS NOT A DIMM, and that is what makes the check pass.** One x16 die
fronts the channel, so `chips_per_rank` is 1 and the rank bus IS the 16-bit
channel, where the DDR parts assemble a 64-bit rank from eight x8 devices.
Reading DDR4's object gave LPDDR5 a 64-bit rank and 19.2 GB/s against a rate
table deriving 12.8, which is precisely the reconciliation failure that
refused its ladder. The object now reports 12.8 GB/s at the rank and channel
scopes, and the ladder is adopted.

**On the column count, recorded so it is not "fixed" later:** Table 6 says 64
columns, the Ramulator preset says 1024. They agree. JEDEC counts 256-bit
fetch boundaries and Ramulator counts 16-bit device words; both give the same
2 KB page.

The bank serialisation width stays ESTIMATED and marked NOT DOCUMENTED, held
equal to the DDR parts' for the reason given in 1.11.68, and the two
hierarchical access times are DERIVED as bank access plus one burst rather
than copied from DDR4's estimates.

Data impact: LPDDR5 only. Its ladder is adopted for the first time, and every
LPDDR5 hierarchy width, bandwidth and level latency moves from DDR4's
borrowed 64-bit values to its own 16-bit ones; the reported rank bandwidth
falls 19.2 to 12.8 GB/s, which is the part the cycles were always counted
for. DDR3, DDR4, DDR5, GDDR6, HBM2 and HBM3 are byte-identical in device
scope. GDDR6 is the last one still reading DDR4's object; it is 1.11.70.

## 1.11.68 -- DDR3 stops borrowing DDR4's shape

1.11.67 measured what an unadopted link ladder costs: up to +102% in cycles
on DDR5. Three technologies were still in that state BY DESIGN. DDR3 is the
first of them to be fixed.

**The defect (audit 1.11.57 B001, open since).** RamulatorWrapper built an
architecture object for DDR4/DDR5/HBM2/HBM3 only and handed DDR3 the
DDR4-2400 one from an unannounced else-branch. Every width, internal
bandwidth and derived hierarchy figure reported for DDR3 described DDR4, the
run printed "there is no DDR3 architecture object in this tree", the
reconciliation check failed, and the per-level ladder was refused. DDR3 ran
on the per-technology placeholder table the source itself labels a
design-specific placeholder.

**The fix.** `createDDR3_1600_Verified()`, transcribed from JEDEC JESD79-3D
section 2.11.5 for the 8 Gb x8 part the preset `DDR3_8Gb_x8` simulates:
8 banks, row A0-A15 (65536 rows), column A0-A9 + A11 (2048 columns), page
size 2 KB, 8n prefetch. The ns timings are the DDR3_1600H bin (nCL/nRCD/nRP
9, nRAS 28 at tCK 1.25 ns) written as the arithmetic that produces them, so
the preset stamp is a check rather than a change. DDR3 joins the set whose
data rate is stamped from the preset, which is what makes the reconciliation
check pass.

**DDR3 HAS NO BANK GROUPS**, and the ladder now says so. `bank_groups_per_chip`
is 1, and `getBankGroupPortBits()` returns the bank serialisation width
unchanged when a part has no bank groups, instead of the x2 bank-group-port
multiplier. That multiplier stays exactly where 1.11.57 left it for every
part that HAS bank groups: unsourced, announced on every run, and
deliberately unchanged because moving it would move the whole corpus. What
changes here is narrower and is settled by the object rather than by taste:
a part with one bank group has no bank-group port to be twice as wide as a
bank's. DDR4 has 4 bank groups, DDR5 and both HBM stacks 8, so no adopted
ladder moves.

**What is still estimated, stated plainly.** The internal stages carry the
same declared-estimate status they carry on DDR4 and DDR5: the global
sense-amplifier width is INFERRED from DAS-MICRO'15, and the bank
serialisation width is ESTIMATED and marked NOT DOCUMENTED, held equal to
DDR4's so that a DDR3-versus-DDR4 comparison does not turn on an invented
difference between two unsourced numbers. The two hierarchical access times
are DERIVED here rather than copied from DDR4's 60/80 ns estimates: both are
the bank access plus one burst, which is the floor the wrapper's own
`getChipAccessLatency()` already computes, and adding an unsourced rank hop
on top would repeat the shape 1.11.23 and 1.11.57 removed elsewhere.

Data impact: DDR3 only. Its ladder is adopted for the first time, so every
DDR3 hierarchy width, bandwidth and level latency changes from DDR4's
borrowed values to its own, and the level-2 rung halves (16 to 8 bits) for
the reason above. DDR4, DDR5, LPDDR5, GDDR6, HBM2 and HBM3 are byte-identical
in device scope. LPDDR5 and GDDR6 still read DDR4's object and still declare
their ladders unsourced; they are 1.11.69 and 1.11.70.

## 1.11.67 -- the part the knob names, at every door

The re-sim pre-flight ran the corpus configs through `--print-mem-info` on
the 1.11.66 binary before a node was spent on them, and DDR5 failed its own
reconciliation check on every default run. Three defects, one root: the
1.11.66 speed-grade knob (R8 #9) moved the DDR5 default part to 4800B but did
not reach everything that describes the part. Each fixed and validated
separately, device scope, old-vs-new on the same configs.

1. **The architecture object kept the factory rate.**
   `applyPresetTimingsToArchitecture()` stamped timings, clock and burst but
   not `data_rate_mtps`, which stayed at the literal 3200. The 1.11.56 check
   (architecture rate x width x channels must reproduce the rate table's
   bandwidth) therefore failed at 4800 -- 25600 vs 38400 MB/s -- and, by the
   1.11.57 B001/B002 rule, the per-level link ladder was NOT adopted: every
   default DDR5 run fell back to the placeholder table ("88 bits, 26.4 GB/s"
   at the channel) and said so. The rate is now stamped from the preset's own
   `rate` column for the four technologies that own an object (DDR4, DDR5,
   HBM2, HBM3). DDR3 is deliberately excluded: it reads DDR4's object as a
   proxy, and stamping its rate would make the check pass and DDR4's ladder
   be adopted under a DDR3 provenance line -- the false claim 1.11.57 refused.
   Validated: at 3200 / 4800 / 5600 the new binary prints "Hierarchy link
   ladder from the DDR5 architecture object 256/8/16/8/64/64/64" and
   "PROJECTED from the sourced ladder ... invariant checked", zero mismatch
   warnings; the old binary fires 7/2/7 and "NOT adopted". DDR4, HBM2, HBM3
   (rate stamp == literal) and DDR3/LPDDR5/GDDR6 (not stamped) are
   byte-identical old vs new.

2. **A retired note came back, and lied.** `sayPresetRate()` compared the
   static rate table (3200) against the preset and printed "DDR5 is modelled
   at 3200 MT/s (energy, termination and bandwidth)" -- a claim that has been
   false since 1.11.63 made `modelledRateMTs()` (the preset) the one authority
   for all three, and one line above `modelledRateMTs()` saying the opposite.
   The lambda is now a no-op (the per-tech branches keep naming their preset
   in one place); the disclosure survives, once and correctly, in
   `modelledRateMTs()`. The static table's DDR5 row follows the default part
   (4800) so the default run is quiet and 3200 / 5600 say "the static rate
   table still says 4800 MT/s and is ignored". Validated at all three grades;
   old fires 7/2/7 false notes.

3. **The knob reached eleven doors of eighteen.** main.cpp constructs
   `RamulatorWrapper` at eighteen sites; `applyDramKnobs()` (1.11.66) was
   wired into eleven. The other seven constructed the DEFAULT part: the
   latency helper behind `getMemoryLatencyCycles()`, `reportBandwidthScopes()`
   (the ladder gate itself), the chip/bank oracle, the host-MC bandwidth and
   the multi-node M/D/1 rate queries. Measured at grade 3200 on 1.11.66:
   cycles at 3200AN, but access latency 66 cycles and "rank bus 38.4 GB/s"
   (the 4800B part) and the ladder gate evaluated on the wrong object.
   Invisible at the default grade, which is why gate 1176A D9 passed. The four
   run-wide knobs (device width, DDR5 grade, temperature, termination
   override) are now recorded ONCE, `RamulatorWrapper::setRunWideKnobs()`,
   right after the YAML block where their last assignment lives, and every
   constructor starts from them; `applyDramKnobs()` stays as the idempotent
   per-instance path. Validated: 3200 now reports 25.6 GB/s, 60 cycles, the
   8 Gb org and zero 16 Gb mentions; 5600 reports 44.8 GB/s, 65 cycles; the
   default run is byte-identical to the state after fix 2; the six other
   technologies byte-identical to 1.11.66 (the un-knobbed instances now also
   run at the config's 350 K instead of the member's 358 K -- same refresh
   rung, no observable moves).

Not fixed here, stated: DDR3, LPDDR5 and GDDR6 have no architecture object of
their own (1.11.57 B001) and run on the declared placeholder ladder; whether
to build the three objects from the standards now in hand before the corpus
re-sim is a ruling for the re-sim plan. The wrapper's early "In-Memory Network
Initialized" block prints the per-technology TABLE widths before adoption is
decided, so it disagrees with the adopted ladder on a run that adopts one --
cosmetic, listed.

Data impact: DDR5 only, and it is large. Default runs now simulate on the
sourced link ladder instead of the placeholder table, and the cycle count
rises by an amount that depends on workload size (stream_triad, 16 PE, BANK,
detailed NoC): +18.0% at 1M elements, +33.1% at 400k, +102.3% at 100k --
always the same direction, always far outside the run-to-run band. Gate 1177D
measured that band in the same runs: 2.38% over six repeats, with the
new-vs-old mean difference at 0.70%, inside it. Non-default grades now
describe one part throughout.

DDR4 -- and by extension every technology whose object already reconciled --
is UNCHANGED, established three ways: device-scope output byte-identical
(bit-deterministic), the new binary reproducing the old binary's exact
access-counter vector (rd/wr/remoteAcc/instrs) in repeated runs, and the
cycle means agreeing inside the measured band. DDR3, LPDDR5 and GDDR6 are
byte-identical in device scope.

Gate history, recorded because it cost four runs: 1177A passed 7 of 8 arms;
its M1 arm failed three times on ARM defects, not product defects -- a
timeout, an exact-cycle-equality assertion on a metric that is not bit-stable
under OMP thread scheduling, a single-run counter comparison that sampled
workload jitter, and a drift threshold with no source behind it. 1177D
asserts what the metrics carry: an exact counter-vector match, and tolerances
measured from the old binary's own drift in the same run.

## 1.11.66 -- round five: the register, the shape, and a clock put back

Round 5 audited 1.11.60 through 1.11.65 in three lanes -- code, gates,
calibration -- and this release closes everything it found: twelve REAL
defects, six latent, five cosmetic, and the gate arms that had passed
without proving anything. Two user rulings (R8): the HBM2 IDD row becomes
measured silicon, and the DDR5 speed grade becomes a setting. Discipline for
the whole release was "fix one, validate one" (user decree): every item
below carries an explicit PASS on a stated observable before the next was
touched.

THE REGRESSION I SHIPPED, PUT BACK. GDDR6's clock relation is 8 bits per
pin per CK (WCK runs at 4x CK with DDR on WCK; Samsung K4Z80325BC Table 91:
"tCK 0.57 ns" AT 14 Gbps; JESD250D Table 1: CK 1.5 GHz <-> 12 Gbps), not
the 2 bits per pin the 1.11.63 CK-domain pass assumed for it. The proof is
arithmetic: every cycle count in GDDR6_2000_1350mV_double reproduces
Samsung's 14 Gbps AC set at 0.57 ns within 1-2% -- nRCDRD 26 = 14.82
(tRCDRD 15), nRP 26 = 14.82 (15), nRAS 53 = 30.2 (30), nRC 79 = 45.0 (45),
nRFCpb 105 = 59.9 (60), nREFI 3333 = 1.90 us -- and reproduces NOTHING at
1.000 ns. Upstream's 570 ps had been right. Re-mirroring the column to 1000
ps in 1.11.63 kept the counts and changed the clock, so the timing model
became a 2 Gbps/pin part with every row timing 1.75x slow, and 1.11.65 then
derived the DQ turnaround from it: the 11.0 ns that release called a
correction replaced the right number, 6.27. The row's "2000" was never a
data rate; the rate column now carries the pin rate PIMID prices (14000),
tCK derives as 8E6/rate = 571 ps, nBL and nCCDS move to the 8-bit domain
(2 and 2), rate_id is re-keyed, and the per-run "upstream ships no timing
bin at the modelled rate" note falls silent because all seven presets' rate
columns now equal the priced rate -- the static rate table is demoted from
authority to cross-check (modelledRateMTs()). The GDDR6 ns getters, which
round 5's code lane had flagged as 0.55x low, turn out to have been the
vendor values all along; they are now DERIVED from the corrected preset and
reproduce themselves (14.85 / 13.7 / 14.85 / 30.3 ns).

GDDR6 tRFC: 360 ns (impl) and 220 (energy) were DDR4's columns. Both
vendor sheets give tRFCab = 120 ns at 8 Gb and 16 Gb alike (Samsung Table
92; SK hynix H56G42A Table 67). Refresh occupancy 18.9% -> 6.3%.

THE SHAPE CHECK -- the structural fix behind four defects. Every
organization cross-check this tree had was a PRODUCT identity (density ==
banks x rows x cols x dq; capacity == chip x dies), and a transcription
carrying half the banks and twice the rows satisfies every one of them.
That is how the HBM2 org shipped wrong for three releases, how the DDR3
transcription went stale against the 1.11.63 preset unnoticed (131072 x
1024 where the preset says 65536 x 2048 -- 256 subarrays per bank instead
of 128, a 1 KB page where the part has 2 KB), and how DDR5 sat at 2x the
part. checkTranscribedOrganizationShape() binds the transcription FIELD BY
FIELD -- banks, rows, columns -- to the device Ramulator actually
instantiates from the named preset, at every wrapper's initialize(),
including the parameter oracles that never build a Ramulator instance (a
throwaway one is constructed purely to read its organization). dram.h is
C++20 and PIMID is C++17, so the read goes through a small probe compiled
inside the Ramulator library (pimid_org_probe). A mismatch is FATAL;
PIMID_ORG_BREAK proves it fires on all seven technologies. The check earned
its keep before the release closed: it found the oracle-side config emitter
missing DDR5's required RFM parameter group (the co-sim emitter always had
it -- two emitters, one part), a per-device-vs-per-channel bank-counting
ambiguity for GDDR6, and the DDR-family config orgs hardcoding x8 while the
transcription followed the run's device width, so an x4 run transcribed one
part and simulated another.

DDR5, 8 Gb x8, has 16 banks, not 32. JESD79-5D Table 4 printed p.7: "BG
Address BG0~BG2 | Bank Address in a BG BA0 | 8 / 2 / 16"; 32 (BA0~BA1)
begins at 16 Gb. The preset said 16 and 1.11.64 had verified it; the
architecture object and main.cpp's per-tech table said 32, with a 1.11.61
note asserting the opposite of the standard. Both authorities now carry
JEDEC's shape; the false "requires at least 32 banks" guard stops firing;
16 x 64 MB reproduces the object's 1024 MB chip size.

DDR5 SPEED GRADE IS A SETTING (user ruling R8 #9): memory.dram.
ddr5_speed_grade in {3200, 4800, 5600}, default 4800. One grade selects ONE
part -- timing row, org, IDD row, rate. The held Micron MT60B addenda are
16 Gb, B-bin dies (Rev A is marked -48B = DDR5-4800B 40-39-39; Rev D is
-56B = 5600B 46-45-45), so the new timing rows are the B bins, computed
with the procedure that reproduces the 3200AN row field-for-field
(JESD79-5D Tables 287/289 for the bins, 335/336 for per-speed AC, Table 71
for the 16 Gb tRFC1 295 ns, clause 13.2 rounding, NOTE 8 for nRC), and
their IDD rows are Table 6 (Rev A: 103/92/142/377/349/277/88 mA) and Table
8 (Rev D: 53/49/91/218/241/377/47). The 3200 row keeps the 8 Gb part and its
previous currents with the gap STATED: no held datasheet publishes a 3200
column. Plumbing: the wrapper carries the grade; an energyKey() "DDR5-4800"
selects the IDD row while baseTech() keeps every family-level decision on
the bare technology; the grade rides applyDramKnobs() to every oracle and
threads through the system-scope report and the co-sim DRAM model.
Consequence at the default: DDR5 moves to the 4800B bin and the 16 Gb org
-- a different part than 1.11.65 simulated, by design.

THE HBM2 IDD ROW IS MEASURED SILICON (user ruling R8 #10). The row it
replaces (28/17/21/80/90/65 mA) traced to no vendor table -- HBM2 sheets
are NDA-only and JESD235D prints its IDD value columns empty. The CMU-SAFARI
HBM-Power artifact measured the JEDEC IDD loop patterns on 36 real HBM2
stacks; JESD235D cl. 9.1 (printed p.100) says measurements are taken with
all channels active and "shall be given per channel", so stack mean / 8 IS
the datasheet quantity: IDD0 141, IDD2N 136, IDD3N 133, IDD4R 464, IDD4W
356, IDD5B 189. The model's 8-channel stack standby now computes to 1.31 W
against the measured 1.37 W (it had been 0.20 W). IDD2P was not measured
and its 7 mA is stated as the one unsourced column.

DDR4: IDD5 = 155 mA had no source. Micron MT40A tabulates IDD5R (the
distributed figure, p.318); converting the same Rev A row the other columns
come from gives IDD5B = 50 + 14 / (350/7800) = 362 mA -- exactly the value
upstream Ramulator2 ships in DDR4.cpp's Default preset, an independent
reproduction. Refresh line 6.1 -> 17.2 mW. And tRFC1[8 Gb] 360 -> 350 ns
(MT40A p.369), which the energy row had carried all along.

dramRowBytes -- the stride of the MEASURED row-miss fraction that feeds the
activate term -- was a per-generation guess wrong on three of seven (DDR3
1 KB where the part has 2 KB; HBM2/HBM3 2 KB where JESD235D/238B print
"Page Size per PC 1 KB"). It is now the preset's cols x dq / 8, read through
PresetOrganization::rowBytes(), which had existed since 1.11.61 with no
caller, and is verified by the shape check.

The R6 timing stamp now reaches HBM2 and HBM3 and stamps the core clock
and burst as well as the four ns timings. HBM3's object carried
clock_freq_mhz = 3200 -- the rate/2 convention both HBM3 makers' silicon
papers refute -- and it drove HBM3's inner ladder rungs AND, through the L0
reference anchor, every technology's Garnet latencies; it is 1600 now. HBM2
tRP 12.5 -> 15.0, tRAS 28 -> 33.3, tBurst 3.33 -> 1.67 ns; HBM3 tRP 10 ->
16.25, tRAS 24 -> 33.1. Validated on a NEW "[mem] <tech> model inputs:"
line in --print-mem-info that prints the stamped ns timings, core clock,
burst and refresh factor -- the model's inputs, directly observable -- after
an energy-based validation FAILED for the right reason: three fixes
(dramRowBytes, the stamp, the IDD row) converge on HBM2's per-access energy,
and one observable cannot attribute three causes.

LATENT AND COSMETIC, all closed: the ONE FABRIC invariant no longer checks a
layer against the rung it was set from -- it asks the consumer's own
layerForLevel() which layer a tier lands in and refuses if emitter and
consumer disagree (the drift it was built to catch); applyDramKnobs() is
the one place an oracle wrapper is configured (width, grade, temperature,
termination) so effectiveDramBanks() and the die-area oracle stop reading an
x8 part in an x16 run; the HBM3 object's 4 bank groups per channel becomes
8; DRAMModel forwards setTemperatureK to its owned wrapper (the refresh
ladder had been dead on the co-sim device path) and gains setDramPartKnobs;
interfaceAreaWithheld() is read where the area is reported instead of
inferring the cause from a proxy; LPDDR5's energy-row tREFI 3904 -> 3906 to
match the impl and the standard; the temperature-ladder code tags say
1.11.65, which is when it shipped; the two nominal temperature defaults are
explained at the field; yaml_reference documents the 10 K grid (only 360 K
and 370 K cross the ladder thresholds) and the two memory.dram keys.

Gate 1176 follows the round-5 checklist: device-scope observables (bit-
deterministic), every arm with a FIRES side, every grep verified against a
live log before submission, configs checked against the validators. It also
carries asserting arms for the two 1.11.64 claims that shipped without one
(the DDR5 JESD79-5D fixes; GDDR6 host RON) and for the 1.11.60 Fbw topology
header that gate 60 G4 left unread.

Data impact: GDDR6 (every row timing 1.75x faster than 1.11.65; tWTR 11.0 ->
6.28; refresh 3x less; activate energy via the derived getters, small);
DDR5 (new default part: 4800B / 16 Gb / Rev A IDD -- cycles, background and
refresh all move); HBM2 (IDD 3-8x; tBurst halves; rowBytes halves; bank
count from 1.11.64); HBM3 (core clock halves on the inner ladder; tRP/tRAS
stamped; rowBytes halves); DDR3 (subarrays 256 -> 128, page 2 KB); DDR4
(refresh 2.8x). LPDDR5 is unchanged except tREFI 2 ns. The corpus re-sim on
>= 1.11.66 is the gate before any CAL number is quoted; a written re-sim
plan for approval is the next item.

## 1.11.65 -- the bus turns at the preset's clock, and refresh feels the heat

Two items the vendor-document campaign left queued, landed together because
only one of them moves a shipped number.

THE DQ TURNAROUND PENALTY IS DERIVED. Since 1.10.6 the in-memory network
has charged the shared DQ bus a direction-reversal penalty (JEDEC tWTR)
between a write and a read, from a hand-written per-technology ns table --
"nWTR_L x tCK, transcribed from the preset", as its comment said, with the
1.11.52 audit already noting that nothing read it at run time and a preset
change would not move it. It had drifted: GDDR6's 6.27 ns was 11 ck x 0.570,
computed from a tCK that 1.11.63 corrected to 1.000 ns, so GDDR6's
turnaround was priced 1.75x LOW against the preset the run simulates.
HBM3's 8.11 survived only because the same release halved its cycle count
and doubled its tCK. The R6 cure, as for LPDDR5's ns getters in 1.11.63:
the preset transcription gains an nWTR column (nWTRL where the family
splits it, nWTR for DDR3, and for DDR5 the Max(16nCK, 10ns) term of JESD79-5D
Table 334's tCCD_L_WTR composite), and main.cpp derives the ns as nWTR x
tCK at load, prints the derivation, and REFUSES (exit 2, with the reason)
if the transcription lacks nWTR or its tCK disagrees with the impl -- a
stale transcription can no longer price a different clock. The gate proves
the refusal fires through the PIMID_TWTR_BREAK fault hook. Values at the
shipped presets: DDR3 7.5, DDR4 7.5, DDR5 10.0, LPDDR5 12.5, GDDR6 11.0
(was 6.27), HBM2 8.33, HBM3 8.125 ns. GDDR6 is the material move; HBM3
also shifts 8.11 -> 8.125 (the table had rounded 13 x 0.625) -- 0.2%,
recorded so "only GDDR6" is not read as exact. [Round 5 correction: the
GDDR6 value 11.0 was itself WRONG -- see 1.11.66, which restores 6.28.]

REFRESH FOLLOWS TEMPERATURE. config.temperature_k reached McPAT, CACTI and
NVSim, and never the DRAM refresh duty: a 105 C run priced the same refresh
power as a 45 C one. Every DRAM family refreshes twice as often above
85 C, and HBM four times as often above 95 C; the sources were assembled
by the September document sweeps and are now applied as a multiplier on
tREFI at the one choke point every refresh consumer passes through
(iddFor -> stateWithRefreshMW / refreshMW / backgroundMW /
backgroundUnitMW / backgroundSystemMW). DDR3/4/5, LPDDR5 and GDDR6: 0.5x
tREFI above 85 C (JESD79-5D Table 70's 3.9 -> 1.95 us; JESD79-3E and the
Micron sheets' extended-temperature 2x; JESD209-5C Table 240 NOTE 2 and
the MR4 derating; JESD250D's temperature-sensor refresh). HBM2/HBM3: 0.5x
at 85-95 C and 0.25x above 95 C (AMD PG276 p.23; AMD DS923 note 16 ">= 4x
above 95 C"; Intel UG-20031 Table 30 and the Agilex M HBM2E IP guide's
identical TEMP[2:0] ladder). The cold-end rungs those controllers expose
(2x and 4x SLOWER refresh below vendor-specific thresholds) are NOT
credited -- a refresh-power discount on a threshold no standard fixes
would be an invented benefit. Above 105 C no source specifies a rate; the
last rung is held. The IDD currents are not temperature-scaled: datasheets
give them at a fixed case temperature with no derating curve, so the
refresh RATE is the only normatively temperature-dependent term.
Temperature is set on the wrapper at both oracle sites -- device scope and
the system-scope report -- before initialize(), the two-site lesson of
1173B. The default 350 K = 77 C is inside the nominal range, so every
existing result is bit-identical; the ladder engages only when a config
states power.temperature_c above 85.

Documentation: docs/yaml_reference.md gains the temperature ladder, the
dq_turnaround derivation, and -- overdue since 1.11.63 --
power.termination_pj_per_bit, which had been wired but never documented.

Data impact: GDDR6 only (turnaround penalty 6.27 -> 11.0 ns on every
write-to-read turn of the shared bus). Nothing else moves at default
temperature. The corpus re-sim on >= 1.11.65 remains the gate before any
CAL number is quoted.

## 1.11.64 -- vendor silicon, and the layout that density hid

The release that reads the parts' own papers. Six ISSCC/JSSC device papers
arrived (RIKEN IEEE Xplore) alongside the published JESD79-5D, and between
them they corrected an organization defect, confirmed two derivations,
retired a staged change that would have been a bug, and answered a
modelling question that had been queued for a user ruling.

HBM2 ORGANIZATION (JESD235D Table 4, printed p.6; Table 5 p.8). The 4 Gb
and 8 Gb org presets each carried HALF the banks and TWICE the rows the
standard specifies -- 8 banks/pseudo-channel over 32768 rows where JEDEC
gives 16 banks over 16384 (and, at 8 Gb/channel, 16 over 32768). The
density product closed exactly either way, which is why it survived every
completeness and capacity cross-check this tree has; what it described was
a part with half the bank-level parallelism and twice the row space. On a
bank-placement PIM model that is first-order: half the banks to interleave
over, and a diluted row-hit rate at fixed footprint. The 2 Gb preset was
already right and is unchanged.

The fix is THREE-SITE, and each of the two sites beyond the obvious one
was found by something failing rather than by reading the code.

Site 2, the wrapper's own transcription of the org preset:
getPresetRowsPerBank() -- sole authority for subarrays_per_bank, the
in-memory tree shape, the tree-coverage assertion and pages_per_unit
(ruling R4) -- reads that transcription, not the Ramulator source. The
first build after the preset edit was green and moved NOTHING.

Site 3, the ARCHITECTURE OBJECT, and this one was a defect this release
introduced before gate 1174A caught it. Bank ROWS come from the preset
(-> pages_per_unit) but bank COUNT comes from the architecture object
(effectiveDramBanks -> total_units). With only the preset corrected, the
1174A logs showed --pages-per-unit halving 65536 -> 32768 while
--total-units stayed at 128: the modelled capacity HALVED. Neither the R1
capacity cross-check nor the R4 derivation covers a bank-count
disagreement between the two authorities, so nothing warned. The DDR5
entry in dram_architecture_v2.h had in fact predicted this exact hazard
-- "moving the bank count is a separate change with a separate blast
radius (the CACTI area query and effectiveDramBanks())" -- and the
warning was read only after tripping over it. The object now carries
JEDEC's 8 bank groups per channel (4 per pseudo-channel x 2 PC = 32
banks) and bank_size_mb 32 -> 16 MB to match. Gate 1174B asserts the
product directly: units must double, pages per unit must halve, and
their product must hold.

WHAT DELIBERATELY DID NOT CHANGE, and why the staged plan was dropped.
The 1.11.63 changelog staged "HBM3 org DQ 128 -> 64 + columns 64 -> 128"
and an equivalent HBM2 re-factoring. Checking the vendor organization
against ours showed the re-factoring would have been a DEFECT: dq 128 x
2n prefetch = 256 b = 32 B is exactly JEDEC's access granularity (32 DQ x
BL8), and halving dq without moving the prefetch halves it to 16 B. Our
factorization (128 x 64 x 2n) and JEDEC's (32 x 256 x 8n) produce the same
page size, the same granularity and the same density; only the bookkeeping
differs. The staged change is dropped, documented at both presets, and if
the factorization is ever aligned it must be the triple, not the pair.

HBM3, CONFIRMED BY VENDOR SILICON. Two independent makers state the CK
domain outright: Ryu et al. (Samsung, JSSC 58(4) p.1052) give a 2 nCK bus
window "equivalent to 1 ns at 8 Gb/s/pin", and Park et al. (SK hynix, JSSC
58(1) p.259) give "1tCK ... 571.4 ps for the 7-Gb/s operation" -- both
tCK = data_rate/4, confirming the 1.11.63 CK-domain correction and
refuting the rate/2 convention much of the open-source community uses.
Ryu also gives tCCDS = 2 nCK and tCCDL = 4 nCK, matching this tree's
values exactly. The ISCA 2025 tutorial (Woo/Elsasser, Rambus) supplies the
physical reason HBM's column spacing is half DDR's: the IO sense amp sits
mid-bank rather than at the bank end. Their organization (16 ch x 2 pCH x
32 DQ, 16 banks/pCH in 4 groups, 16384 rows, 1 KB page/pCH, 2 Gb/pCH)
closes arithmetically to the advertised 16 Gb die and 16/24 GB cubes
across three independent papers, and matches ours in every derived
quantity.

THE TSV QUESTION, ANSWERED WITHOUT A RULING. 1.11.63 raised "PIMID models
no vertical-interconnect energy" as a decision item, since HBM's charged
DQ interface is zero. Cho et al. (SK hynix, ISSCC 2018 12.3 Fig.12.3.1)
measure per-TSV driver current on real HBM2 silicon -- ~880 uA multi-drop
vs ~610 uA spiral point-to-point at 1.0 V, 3.3 Gb/s PRBS, i.e. roughly
0.27 -> 0.19 pJ/bit for the driver alone -- which looks like the missing
term. It is not: the IDD columns are per-channel DEVICE currents, and an
IDD4R/IDD4W measurement is taken at the stack's supply balls with a burst
in flight, so the TSVs are inside the device under test and their current
is already inside the measured burst. That is precisely the structural
difference from DDR-class parts, whose DQ bus leaves the package and
terminates externally. No term is added; the measurement is recorded as a
decomposition insight for validation. HBM termination = 0 now has three
independent confirmations (JESD238B.01 cl.9.1; ISCA 2025 slide 46 "ODT not
allowed in HBM (static power)"; Chun JSSC 2021 Table I "CMOS,
un-terminated").

DDR5, the three JESD79-5D defects (carried from 1.11.63's verification
pass): nFAW's x4 and x16 rows were SWAPPED against the standard's
page-size keying (Tables 4-7 give x4 = 1 KB, x16 = 2 KB; Table 334 gives
tFAW(1K) = 32, tFAW(2K) = 40 nCK) -- the old x16 = 32 was 8 nCK more
permissive than the standard allows, a real timing violation for a
hand-written x16 org, while x8, which every shipped preset uses, was
correct; nREFI applied the MINIMUM-parameter rounding algorithm to a
MAXIMUM parameter (clause 13.2 prescribes round-down with no correction
factor), 6222 -> 6240 nCK; and nREFSBRD carried Table 73's 30 ns in the
CYCLE column, 37.5% short of the JEDEC minimum, now derived at load like
every other refresh timing (48 nCK). Flagged, not changed: nCCDL_WR
selects 16 vs 32 by DQ width where Table 334 conditions tCCD_L_WR2 on
"second write not RMW" -- the width heuristic stands in for x4
on-die-ECC RMW and is documented as a modelling choice.

PROVENANCE. GDDR6's host-side write-driver RON is sourced (Achronix
Speedster7t UG091 Table 3 p.16: "DQ driver impedance (RON) 40/48/60 ohm";
our 40 is in the set), which retires the last stated assumption in the R7
read/write termination split -- every electrical input is now a sourced
value or a point inside a sourced range. A second HBM2 die-area anchor
joins Sohn's: Cho's 81.8 mm^2 for an 8 Gb 2-channel core die, recorded as
an 82-96 mm^2 cross-vendor band rather than a point. docs/sources.md gains
a vendor-silicon-papers section with the scope caveats that matter --
notably that Chae's 0.25/0.29 pJ/bit is a SoC-side PHY figure with the
DRAM die excluded, and must never be used as DRAM I/O energy.

NOT CITABLE, recorded so nobody is tempted: misc/"DRAM Lecture
Tomishima.pdf" carries "Intel Confidential - Internal Use Only" on 85 of
its 90 pages. Background reading only -- no citation, no figure, and no
number from it in any config's provenance.

STAGED, not bundled: the temperature-dependent refresh ladder. It is now
sourced across three vendors (Intel's TEMP[2:0] 4x/2x/1x/0.5x/0.25x tREFI
in the Stratix 10 MX and Agilex M HBM guides; AMD DS923's ">= 4x above
95 C"; PG276's tREFI halving at 85-95 C) and our refresh model is
temperature-flat, but it is a cross-cutting feature and ships in its own
attributable release.

Data impact: every HBM2 result. Bank count per channel doubles (16 -> 32),
rows per bank halve, bank_size_mb halves, the in-memory tree loses half
its leaves (32 -> 16 in the reference co-sim cell) and pages_per_unit
halves with total_units doubling to hold capacity -- so the address-to-unit
map, PE locality and hop distance all move. On cycles the honest statement
is narrower than "everything moves": the memory-timed weave result moves
(1174A measured +3.8% on the reference cell with only half the fix in),
while per-core simulated cycles move on some cores and not others,
depending on whether that core's traffic is layout-sensitive. DDR5 moves
by +0.29% on nREFI plus two corrections that are inert at the shipped
presets. HBM3, LPDDR5, GDDR6, DDR3 and DDR4 are unchanged -- their
entries here are provenance and documentation. The corpus re-sim on
>= 1.11.64 remains the gate before any CAL number is quoted.

## 1.11.63 -- the standards arrive, and the presets answer to them

The calibration release. R6 (2026-08-24, user): "we mainly rely on the
ramulator models, but these models must be carefully calibrated to
JEDEC/vendor spec/data we have. PIMID itself provides no such thing." A
document campaign brought the full standards into misc/ -- JESD209-5C
(LPDDR5/5X, 676 pp), JESD235D (HBM1/HBM2, 213 pp), the complete POD family
(JESD8-19/20A/21C/24/25/30A = POD18/15/135/12/10/125), JESD305B.01, the
Micron DDR3L/DDR4/DDR5 full datasheets, a Samsung and an SK hynix GDDR6
part datasheet -- and every preset field they can check was checked. What
remains uncheckable is now a named vendor-NDA list (HBM3 part spec; HBM2
IDD), not a fog.

Part A/B (calibration audit + R6 batch). The audit (75 calibrated / 42
drifted / 233 uncheckable at the start) drove: GDDR6 tREFI 7800 -> 1900 ns
(refresh occupancy 4.6% -> 18.9%) and nCCDS 4 -> 8 with the tREFI/nRFCpb
column swap unwound; HBM3 re-derived into its CK domain (tCK = 625 ps,
fCK = rate/4 per JESD238B Table 92; nCCDS 2 -> DQ utilisation 57% -> 100%,
bandwidth uncap 1.75x); LPDDR5 IDD re-based on the Micron MT62F VDD2H
column (~3x); DDR3_8Gb_x8 org corrected; DDR3/DDR5 ns rows re-keyed to the
simulated bins (the DDR4 tRC bin-stamp, +1.1%, is the recorded judgment
call -- gate 1173D measured its co-sim consequence at ~2%). R6 batch: the
wrapper's capacity_/bandwidth_ literals are DERIVED from preset org x
timing; HBM3 chip capacity follows the preset (core die 1024 MB, stack
8 GiB -- the standing capacity-mismatch warning is gone, and gate 1173A
proved it fired before); HBM bank_size -> pages_per_unit; HBM3 CACTI-IO
channel width 128 -> 64; workload.mpi_ranks now implies type mpi (the
1172-gate footgun); B018 masked.

Part C/D (JESD209-5C). The RTT saga closes: Table 84 p.144 gives DQ ODT
"000B: Disable (Default)" -- the JEDEC default operating point is
UNTERMINATED, below the retired band's floor, and the 240-ohm assumption
chain (1.11.52-1.11.59) dissolves. rtt = 0 is the sentinel; the LVSTL
branch prices no DC loop for it (the gate against dividing into rpd+0 is
load-bearing); getTerminationEnergyBandNJ is retired. The AC verdicts: 17
of 22 fields exact (all four tRFCab/pb densities, tREFI 3906, nBL16 2);
nCL 20 -> 17 and nCWL 11 -> 9 -- both were the NEXT frequency bin's row
(1100B) where 6400 Mb/s belongs to 1011B; nCCD split into nCCDS 2 /
nCCDL 4 (BG mode is mandatory above 3200 Mb/s, and same-bank-group
columns were cycling 2x too fast); tPBR2ACT 7.5 ns at every density (8.0
appears nowhere in the standard); nCS 3; four timing-constraint formulas
corrected to the standard's own expressions; LPDDR5 rounding switched to
plain RU() per the standard's notes (numerically identical at 1250 ps).

Part E (R7, user ruling: read/write split termination). The old single
loop (rtt 48 / rpd 40 for DDR4/DDR5) matched no JEDEC default and no IDD
condition, and DDR5's citation named a standard that does not exist
("POD11" -- the POD family has no 1.1 V member; JESD79-5 defines that
point itself). The two directions are different circuits and are priced
separately now: reads drive the DRAM's RON into the RX termination
(RTT_NOM class), writes drive the controller's RON into the DRAM's
RTT_WR. The anchors are the vendors' own IDD measurement conditions --
identical across all three DDR-family datasheets (RON = RZQ/7 = 34,
RTT_NOM = RZQ/6 = 40, RTT_WR = RZQ/2 = 120; MT41K p.32, MT40A p.315,
DDR5 core p.453) -- so the interface and the IDD-sourced array energy
describe the same register settings. GDDR6: rd 40+60 (Samsung K4Z80325BC
p.166 characteristics), wr 40+120 (p.144: "All ODTs are enabled with
ZQ/2"). CACTI-IO carries the split natively (rtt1/rtt2_dq_read/write; a
second extio_power_term pass with iostate = READ); a sourced row with
zero termination (LPDDR5) is NOT injected -- CACTI-IO would divide by it
and the inf would have ridden the substitution gate into the report.
Host-side write RON is a sourced RANGE with the applied value inside it
(Intel 743844-015 Tbl 86/87: 30-50 ohm; 34 applied); GDDR6's host RON is
the one remaining stated assumption. Reads now cost MORE than writes
(smaller loop resistance; DDR4: 3.045 vs 2.276 nJ/64B at RANK) -- the
one-number-for-both era ends. power.termination_pj_per_bit, advertised in
three console messages since 1.11.59, turned out to be parsed by NOTHING;
it is a real YAML key now, and gate 1173B-C proved both halves (the OLD
binary demonstrably ignores it; the NEW binary prices both directions at
the stated point).

Part F (JESD235D). HBM2's row was annotated "JESD235C spec-minimum
tRP=16ns, tRC=49ns" -- numbers that appear NOWHERE in the standard.
Table 58 (the only tRC/tRAS/tRP figures JESD235D prints) gives 15/33/48
ns: nRP 20 -> 18, nRC 60 -> 58, and the 63a identity fixes (nRC 59 -> 60,
nCCDS 3 -> 4) turn out to have patched correct identities onto wrong
anchors -- both re-corrected against the standard. nBL 4 -> 2 (PC mode
BL4 = 4 UI = 2 CK; the old 4 described a 128-byte burst and the 64-byte
access identity is restored); nCCDS 2, nCCDL 4; nRTW 17 (NOTE 23 closed
form; inert); nRREFD 8 -> 10 (a units error: JEDEC's 8 ns carried as 8
cycles); nREFI floor (tREFI is a MAX); nRFCSB gets its own published
table (160 ns, was borrowing tRFC at 1.63x); tREFISB's table was shifted
one density column and is re-anchored with the NOTE-29 identity as
cross-check. IDD stays vendor-only -- JESD235D's IDD tables publish EMPTY
value columns by construction. STAGED for 1.11.64 (R2 precedent): the
HBM2 org rework -- JEDEC gives 16 banks/PC x 16384 rows at 4 Gb/channel
where the org carries 8 x 32768 (density closes, layout does not), plus
the dq-role/prefetch/column corrections that go with it.

Gates (1173A-D). 1173A: its own five sim arms were vacuous (BANK
placement = no DQ crossing; wrong cycles pattern) -- the FIRES discipline
caught the gate, not the code. 1173B: E3 caught the override knob
HALF-WIRED (parsed, never applied -- the setter calls were missing); D1
exposed that device scope's M/D/1 never consumes the Ramulator preset
rows. 1173C: the co-sim cells exposed the real finding -- LPDDR5's
preset corrections were reaching NOTHING, because the wrapper's hardcoded
per-tech ns literals (18/18/18/42) feed every LPDDR5 cycle consumer; the
R6 cure derives the four getters from the transcribed preset
(21.25/18.75/18.75/42.5 ns), and the co-sim moved +6.0% once they did.
Also fixed there: every co-sim run printed a 3-line parse-failure scare
for an empty OPTIONAL override path. 1173D: all arms pass; the LPDDR5
and HBM2 co-sim cells are bit-deterministic across runs, the DDR4 co-sim
cell shows ~0.4% run-to-run scheduling wobble (recorded; its bin-stamp
movement is ~2%, above the wobble).

Data impact: every LPDDR5 number (cycles via RL/WL/nCCD and the derived
ns; array energy via the re-based IDD and the derived tRC; interface
termination -> 0 by citation), every HBM2 number (nBL/nRP/nRC/nCCD
column throughput; refresh tables), HBM3 bandwidth (1.75x uncap), GDDR6
refresh occupancy (4x), DDR3/DDR5 bin re-keys, and all DDR/GDDR
termination energies (the R7 split). The corpus re-sim on >= 1.11.63
remains the gate before any CAL number is quoted. GDDR6's ns-literals
vs preset tension (documented in-file) and the HBM2 org rework are the
named 1.11.64/round-5 items.

## 1.11.62 -- the flush walks at every arrival

Rulings R5a/R5b (2026-08-22, interactive). The 1.11.57 epoch design measured
once, at the first rank's arrival, and shared the footprint out in slices --
which lost whatever ranks 2..N dirtied after that walk (round-4 D002), kept a
stale-slice hazard at re-open (D003), and carried slice/remainder machinery
whose only job was sharing one measurement.

Now every arriving rank performs its OWN walk: measure and clean (M -> E, the
fused takeDirtyLineIds walk, per-cache bottom-CC locking one at a time, tag
read inside the lock, the 1.11.60 filter-array invalidation preserved), and
charges exactly the DELTA dirtied since the previous walk. Cleaning IS the
de-duplication, so the D4 invariant holds by construction -- with the unit
stated precisely: each line's DIRTY EPISODE is charged once, and a line the
guest re-writes between walks is charged again because it really is written
back again. Late-dirtied data is charged by the next walker instead of never.
The slices, the remainder arithmetic and all six epoch variables are deleted
(consumer check re-run: nothing outside the file read them).

R5a: the per-rank FIXED cost stays, ruled physical -- each rank's core
executes its own flush instruction and stalls; the shared walk is the
simulator's shortcut, and round-4 D001 is thereby ruled-intended. The bytes
N-slope was the artefact, and it stays gone.

The interrupted first implementation was assessed edit-by-edit against
released 1.11.60 rather than trusted or discarded: all nine partial edits
held; three unfinished pieces were completed (the last live slice reference
in a header comment, the dirty-episode precision above, and the falsifiable
prediction: flush bytes rise or hold against the epoch design, never fall,
modulo N6b).

Docs: docs/cosim.md and docs/yaml_reference.md described the 1.11.40-era
formula and a footprint_bytes key that has been REFUSED since then -- two
mechanisms ago. Both now describe the per-arrival walk, its history, and the
PIMID_FLUSH_TRACE hook.

Gates 1172A/B/C. The first two failures were the GATE's, recorded here in the
spirit of the FIRES rule: 1172A used an invented "ranks:" YAML key, so the
multi-rank arm ran single-rank and one walk was correct behaviour for what
executed; 1172B used the real key (workload.mpi_ranks) without its required
sibling (workload.type: mpi), exposing a genuine footgun -- the CLI flag
implies the type, the YAML key is silently inert without it (queued for
1.11.63). 1172C, with thread-MPI actually engaged: four walks, cumulative =
sum of deltas verified per line, device-scope cell bit-identical, and the
bytes prediction within the N6b tolerance -- AT the tolerance edge (256 B = 4
lines below the epoch design across four ranks' guest layouts), stated here
rather than silently accepted; the re-sim's multi-rank census will show
whether that wobble is layout noise, as N6b says, or a residue worth chasing.

## 1.11.61 -- the object describes the part the preset simulates

The density batch: four rulings made interactively (2026-08-22) after the
density investigation, plus the R6 principle stated 2026-08-24 that governs
them all: PIMID relies on the Ramulator models, those models are calibrated to
JEDEC and the vendor data in misc/, and PIMID ITSELF PROVIDES NO NUMBERS.

R1 -- DDR-family density follows the preset. The architecture objects
described devices 4x-32x less dense than the presets the wrapper simulates
(an LPDDR5 run reported 2.75 mm^2 of DRAM silicon while claiming 8 GiB).
DDR4/DDR5 factories re-based on their presets; DDR3/LPDDR5/GDDR6, which have
no object of their own, get preset density stamped per-technology by a new
applyPresetDensityToArchitecture() -- at DDR4/DDR5 it reproduces the factory
literals exactly, which is the check. Measured die areas now land on the
density rows' own source parts: DDR5 25.40 vs Micron D1a's 25.41; GDDR6 37.03
vs Samsung K4Z80165BC's 37.03. bank_size_mb drives pages_per_unit, so the
workload's contiguous block moves 16x-64x -- re-simulation required, which is
why this release is STAGED alone.

R2 -- HBM keeps CORE-DIE semantics (HBM2's 1024 MB die area lands on the Sohn
ISSCC-2016 measurement exactly), and a capacity cross-check now sits beside
the 1.11.56 speed-bin check: chip_size_mb (x stacked dies for HBM) must
reproduce the preset capacity. It FIRES ON EVERY HBM3 RUN by design -- three
authorities disagree there (object 16 GiB / preset 8 GiB / wrapper literal
4 GiB) -- and the R6 principle resolves it for 1.11.63: the wrapper literal
has no standing, the object follows the preset. The check needed a
transcription of Ramulator's cpp-local org rows; the transcription self-checks
(banks x rows x cols x DQ must reproduce the declared density, refuses loudly
otherwise). Reference-anchor wrappers (the B012 HBM3 anchors constructed
inside every detailed run) are anchor-quiet: gate 1171A caught them
misattributing the HBM3 warning into DDR4 and HBM2 logs.

R3 -- the GDDR6 channel is 16 DQ (JESD250D sec 2.2 p.3, Tbl 19 p.18, Tbl 80
p.177), not the 32-bit device width. bandwidth_ -- the M/D/1 service rate,
the NoC cap and the host MC cap -- halves from 112 to 56 GB/s: every GDDR6
cell was simulated against a memory twice as fast as the part. First-order
GDDR6 result change. The ladder verdict is unchanged (still not adopted;
GDDR6 still has no object of its own to reconcile).

R4 -- bank_rows is DERIVED from the preset (getPresetRowsPerBank, keyed by
the configured device width), replacing a literal table that had drifted 2x
for DDR3 (65536 vs 131072), HBM2 (65536 vs 32768) and HBM3 (32768 vs 16384).
The four consistent technologies reproduce identically under the derivation
-- the ruling's own acceptance test. An unreadable preset now REFUSES rather
than reinstating a literal. subarrays_per_bank: DDR3 128->256, HBM2 64->32,
HBM3 32->16; SUBARRAY placement verified rc=0 on all seven, so the
tree-coverage assertion holds under the new counts.

STILL OPEN, queued for 1.11.63 under R6: HBM3 chip capacity to the preset;
HBM bank_size_mb (4 MB, unruled by R1/R2, describes nothing simulated) to the
preset (32/16 MB -- moves HBM pages_per_unit 8x/4x); wrapper
capacity_/bandwidth_ literals derived from preset org x timing; HBM3's
128-vs-64 channel width in the CACTI-IO table; the DDR3/DDR5/HBM3 timing
drifts the investigation found.

Gate 1171A caught the anchor-warning misattribution; 1171B all five arms
green: LPDDR5 die 2.75 -> 21.99 mm^2, GDDR6 112 -> 56 GB/s, derivations
exact, capacity check fires on HBM3 and only there, SRAM bit-identical.

## 1.11.60 -- one fabric, two consumers; and the interface revert

Audit round 4 (47 REAL, 9 latent, records in _1164audit/) audited the newest
code hardest and found the newest code wanting: the 1.11.58 interface wiring
was a double-count, the 1.11.59 ladder unification was wired from the wrong
end, and the A001 fix of 1.11.57 had moved the temperature keys one nesting
level instead of out. This release lands the 26 small fixes, REVERTS the
interface charging (user-approved), and replaces the ladder patching with the
user-approved ONE FABRIC design.

### The interface revert (A001/A002/A009/A010)

1.11.58 added CACTI-IO's driver+PHY energy and IO area to the charged totals,
presented as closing an omission. Round 4 showed both ends of the wire were
already counted: McPAT is told withPHY=1 at exactly the placements where
crosses_dq charges the new term (the controller-side PHY was already priced),
and the memory die area is a FULL-DIE JEDEC calibration that already contains
the DRAM's IO ring. The 4.2x energy rise and the 69% area rise the 1168 gates
measured were the overlap, not a correction -- and the area figure was a
WHOLE-CHANNEL count (extio.cc sums DQ+DQS+CA+CLK circuits) labelled per-die
and multiplied by the die population: 47.91 - 28.36 = 19.55 = 2.444 x 8,
exactly. Charged interface is termination-only again -- the corpus basis --
and CACTI-IO's figures print as INFORMATIONAL lines that enter no total.
Measured at the gate: interface 12.959 -> 3.070 nJ, with-memory 28.36 mm^2.
No published number was ever affected; the re-sim had not run on 1.11.58/59.

### One fabric, two consumers (the design, user-approved)

Two models -- the analytical ladder and Garnet -- are the product. Two private
FABRIC DESCRIPTIONS were the bug: each model read its own widths and clocks,
and they disagreed (DDR4 chip link: 8 bits in one, 192 in the other). Four
reconciliation attempts (1.11.56-59) each failed because no design stated what
each description means. Now:

- ONE description: the 7-rung sourced ladder, each rung printed WITH its
  upstream reference -- the architecture object's own VerifiedValue status and
  citation, never restated. The honest rungs say so: DDR4's bank rung prints
  "ESTIMATED: NOT DOCUMENTED!", the bank-group rung "ASSERTED: bank
  serialization x 2".
- Consumer A (analytical): unchanged. Gate-proven bit-identical.
- Consumer B (Garnet): the topology is a NAMED PROJECTION of the description,
  CHANNEL-ANCHORED per sparse_htree.h's own 1.10.3 definition: L3 = the
  channel link, L2/L1 one and two tiers inward, L0 = the remaining inner
  rungs folded at their MINIMUM bandwidth (the bottleneck). 1.11.59's map
  {0,1,2,5} was leaf-anchored -- read from the wrong end -- so every DDR
  part had its rank-tier link drawn at the bank-group rung (round-4 B001).
  DDR4 now projects to 8/8/64/64; HBM3 folds with L3 = one 64-bit
  pseudo-channel.
- Units declared: the topology file carries RAW BITS (round-4 B002: a
  REF-normalised score had been written into a field Garnet reads as bits;
  DDR4's 256-bit rung went in as "24"). Garnet's 128-bit flit bound is a
  stated clamp at the consumer; the description keeps true widths.
- One penalty (round-4 B003): the Fbw message inflation is REMOVED. Width
  alone carries the bandwidth difference -- DDR4's 8-bit chip rung turns a
  576-bit access into 72 flits where HBM3's 64-bit channel takes 9. The old
  code applied REF/per_chan twice (port AND message), so the cross-technology
  flit ratio was the product of two bandwidth ratios.
- THE INVARIANT: each 1:1 layer must carry exactly its source rung's
  bandwidth; violation is FATAL. Proven able to fire at the gate via a fault
  hook (PIMID_FABRIC_BREAK) -- five vacuous passes this cycle came from
  checks whose firing was never demonstrated, so firing is now part of the
  gate contract.

### The small fixes (26)

A005: the temperature keys and their range validator moved to the TOP level of
the config load -- 1.11.57 had moved them from inside noc.model to inside
memory:, the same defect one level down; none of the six shipped co-sim
examples has a top-level memory: block, so the knob stayed inert in exactly
the scope A001 was raised about. Verified: 999 K now refused (rc=2) with no
memory: block; the released binary accepts it. B013: an out-of-bounds read of
hierarchy_bridge_latency[6] on a six-element array, at the one site of three
without the guard. C006: every latched McPAT diagnostic evaluated in the
parent BEFORE the setters ran, so a fully measured run printed "no measured
load/store mix ... UNSOURCED fractions" -- diagnostics now run once, in the
parent, after the setters, before the fork. A007: memory.array_pg on DRAM is
refused as its own comment always claimed (a DRAM array cannot be gated; it
must refresh). B014 moves a number: the McPAT NoC duty cycle was scaled by
top-level/device clock -- 4x on the shipped co-sim. D004: the flush's M->E
clean now clears the L1 filter array, so post-flush stores re-dirty their
lines instead of evicting silently clean. D005: LOGIC_DIE placement gets its
own row-model case (HBM3: 32 -> 512 open-row registers). Plus: A003 A011 A012
A013 B011 B016 B017 C001 C002 C003 C007 C008 C009 C011 C012 C013 C015 D008
D009, each tagged in place.

### Known-open, awaiting rulings (density investigation, _1164audit/DENSITY_INVESTIGATION.md)

The DRAM architecture objects describe devices 4x-32x less dense than the
presets the wrapper simulates; blast radius is exactly two consumed numbers
(pages_per_unit, die area). "Density follows preset" is strongly evidenced
for the DDR family and needs a user ruling for HBM (whose chip_size_mb is
already the correct CORE-DIE capacity -- die area lands on the Sohn
ISSCC-2016 measurement exactly). Also found and open: GDDR6's
dramChannelWidthBits returns the 32-bit DEVICE width where JESD250D defines a
16-DQ channel, so its M/D/1 service rate is 2x high -- a first-order GDDR6
result error; and main.cpp's bank_rows table is 2x off against the simulated
preset for DDR3/HBM2/HBM3. These land after the rulings, not silently here.

Gate 1170A, seven arms, all green: projection correct both families, invariant
FIRES on demand, analytical arm bit-identical, revert exact, 7/7 rungs print
their references.

## 1.11.59 -- the eight that were left, and one ladder per run

The items still open after 1.11.58, closed. Three came out of round 3 and were
never fixed (I reported round 3 as fully closed at 1.11.57; it was not -- B005,
B014 and C018 had no fix, and my verification scan read the absence of a
citation tag as "covered elsewhere" without checking each one).

### One ladder per run (B005)

A detailed DRAM run had TWO live per-tier ladders. `sys.hierarchy`'s levels
came from the sourced ladder -- what the element is charged -- while the Garnet
CUSTOM topology was drawn from a per-technology table inside the H-tree
builder. They disagreed: DDR4's chip tier is 192 bits in that table against the
8 bits an x8 part has and which the sourced ladder carries. So the
cycle-accurate network was SIMULATING a different fabric from the one being
PRICED, in the same run. 1.11.56 said those tables "survive only as the
fallback for a technology with no architecture object"; this copy was not a
fallback, it was the only authority for the topology.

The builder now takes its four layers from ladder rungs 0, 1, 2 and 5, and the
channel count from the technology. Where no ladder was adopted -- the object
did not reconcile with its preset -- the table stands and the run says the
widths are placeholders, not tool-sourced. That is a real fallback.

### A tier with no routing hops still has to be crossed (B014)

`avgHops()` returns 0.0 for "bus" and "crossbar" and for a single node, and the
fixed per-level latency was multiplied by it. For a DDR part that is L0, L1,
L2, L4, L5 and L6 -- six of the seven levels -- so the sense-amp and
column-decode time at L0, the mux arbitration at L1/L2, the rank-switching tRR
at L4 and the PCB trace at L5 contributed NOTHING to any traversal that crossed
them, at every technology. Six of the seven emitted levelLatency values were
pure width serialisation. Zero hops means no ROUTING, not no crossing: a bus
still has to be driven and sensed. The fixed cost is charged once plus one per
additional hop; genuine multi-hop topologies are unchanged.

The second half of the same finding is stated rather than changed: adopting the
sourced ladder RE-TIMES every per-level fixed latency, because those constants
are cycle counts at a clock the ladder is free to change. DDR5's L3 "I/O driver
+ package delay" of 5 cycles was authored at 1.2 GHz (4.17 ns) and becomes 2.08
ns at 2.4 GHz. A package delay is a TIME; expressing it in cycles is the
underlying defect, and correcting the constants would move the corpus.

### The per-level latencies are printed (new)

These seven numbers govern every hierarchy traversal the timing model charges,
and they existed only inside the generated ZSim config -- written to a
temporary directory and discarded. No run's own output showed what it charged.
Gate 1169A's B014 arm could not test the fix for exactly this reason: it
grepped for a string that is not in any log and failed EMPTY, testing nothing.
A quantity this load-bearing should be visible in the run that used it.

### Access times that had been wrong since 1.1.1 (C016)

HBM2 and HBM3 carried `subarray_access_ns` and `bank_access_ns` literals
contradicting their own cited timings -- HBM3's subarray said 20.0 ns against a
tRCD+tCAS of 32.0. The history, checked rather than guessed: release 1.1.1
raised HBM tRCD/tCAS to the JESD235C/JESD238 spec minima and did not move the
derived literals, which are exactly the pre-1.1.1 sums (25.0 = 12.5+12.5,
20.0 = 10+10). They were consistent when written and wrong in every release
since. NOT caused by 1.11.56's speed-bin work, which moved only the rate, the
core clock and the burst. Both fields are computed from the timings now and
cannot drift again. Latent today (the extractor overwrites them), so no corpus
number moves.

### A device width that changed nothing (C018)

`setDeviceWidth()` wrote a string that three array-energy sites read. Every
ladder accessor -- chip I/O, rank bus, chips per rank, the bandwidths -- read
per-factory literals with chip DQ fixed at 8, so the width changed none of
them, while the extractor stamped the result "extracted from Ramulator device
configuration (x4/x8/x16)". A false provenance claim. The object honours the
width now: chip DQ is the configured width, chips per rank is rank bus / width,
the prefetch datapath follows, and DDR4/DDR5 bank groups halve at x16. At x8
every value reproduces the factory exactly, so nothing in the corpus moves;
x4 and x16 move, and they were wrong.

### The rest

- **F021**: `coherence.footprint_bytes` is refused at config time. The footprint
  is measured at ROI entry, and the timing model panics on any configured
  value, so the key could only abort the run it claimed to configure -- while
  the debug line printed an "(OVERRIDE)" branch describing an override that
  cannot take effect.
- **F034**: HBM2's `nREFISB` read the refresh-TIME table instead of the
  same-bank refresh-INTERVAL table -- 260 ns where the interval is 4875 ns,
  18.75x too frequent. HBM3 got this in 1.11.51 and its twin did not.
- **F037-class**: an unguarded `density_id == -1` indexed three constexpr
  tables at [-1] -- a non-crashing out-of-bounds read yielding an adjacent
  constant -- in nine more DRAM implementations. All now refuse at
  construction. Two presets change behaviour: `LPDDR5_32Gb_x16` and
  `GDDR6_32Gb_x8/_x16` passed the density check, did the [-1] read on every
  instantiation, and never produced a defined timing; no sourced 32 Gb refresh
  row exists to extend the tables with (JESD250D leaves GDDR6 tRFC
  vendor-specific; Micron's LPDDR5 tables stop at 16 Gb because 32 Gb parts are
  multi-die packages), so they refuse loudly instead. PIMID selects only the
  8 Gb presets, so no configuration here is affected.
- **E010, F018, F035, F036**: the McPAT reference XMLs declare withPHY=1 again
  (D3 made the flag authoritative and thereby deleted the PHY from parts that
  drive off-chip DIMMs); cache registration is role-aware, so a DEVICE node's
  dirty lines are no longer written back on a HOST core and billed to HOST
  DRAM; two dead preset columns and an unsatisfiable guard removed.
- **B013**: the 1.11.56 flattening guard was structurally unsatisfiable. The
  behaviour was right; the condition read as though the other case existed.
  CORRECTION (1.11.60, audit round 4 B016): this was recorded as closed and no
  edit was made -- the guard was still in `src/main.cpp` unchanged. Audit round
  4 re-established that it is genuinely dead (`hierarchy_enabled` is set true
  unconditionally hundreds of lines earlier, and the only path that leaves it
  false returns before this point) and DELETED it in 1.11.60. No emitted value
  changes; the ledger entry above was wrong about the release, not about the
  finding.
- **B016**: the host array's latency was characterized at a literal 350 K while
  its bandwidth, in the same function for the same array, used the configured
  temperature.
- Dead code with wrong numbers deleted: the legacy v1 `DRAMArchitecture`
  carried the same drifted HBM literals and had no callers.

### LPDDR5 termination: a band, not a point

The DQ-interface correction landed in 1.11.58 for every technology with an
exact CACTI-IO map -- DDR3, DDR4, DDR5 and GDDR6. (The 1.11.58 entry said
GDDR6 was excluded. That was wrong and is corrected there.) LPDDR5 is the one
technology whose Rtt this tree cannot source.

A search of everything freely available found the encoding MR11 OP[6:4] = 000B
for "ODT disabled" (YM5XCBQ3B2-T16 64 Gb LPDDR5, IDD note 2, p.10) and
corroborated RON = 40 ohm, and found NO ohm ladder and NO stated default: full
datasheets are either image-only or defer to a Micron General AC/DC
specification not held here, JESD209-5 is paywalled, and the only sources
quoting the ladder are secondary. Every PDF fetched this session -- 51
documents, about 1500 pages -- was machine-scanned for MR11 and DQ ODT; one
page matched, the ODT-disabled note above.

So the termination is now reported as a BAND, which is what the project's own
rule requires of a quantity the tools cannot produce: RODT(DQ) = 30-240 ohm
(Intel 743844-015 Table 89 p.211, typical column EMPTY) against RON = 40 ohm
gives a loop-resistance ratio of 4.0. The applied value stays the 240 ohm end
-- the weakest termination and therefore the LOWEST energy the range permits --
and the run prints the interval and its provenance beside it. An unsourced
point becomes a sourced interval with a stated direction.

Closing it to a point needs the MR11 ODT ladder AND a stated default or a
vendor-stated operating point; the ladder alone would not do it, because which
rung applies is a controller choice.

### Data impact

B005 changes the simulated Garnet topology on every detailed DRAM run, and
B014 changes the fixed-latency term at every bus and crossbar tier. On the HBM3
reference cell the NET is -8.3% device cycles (14.32M -> 13.13M): the two pull
opposite ways and B005 dominates, because the sourced rungs carry the higher
per-rung clocks C003 derived. C018 moves x4 and x16 configurations only. C016
and the fork items move nothing today.

Gates 1169A then 1169B. 1169A's B014 arm grepped for a string that exists only
in a discarded temp file and failed empty; the fix for that is the per-level
latency print above, not a looser assertion.

One honest limit on 1169B's P4 arm: it confirms the seven latencies are now
printed and well-formed (HBM3: 1/2/2/1/2/2/2 PE cycles at the device clock),
but it CANNOT A/B them against 1.11.58, because the comparison binary does not
print them at all -- the observable is what this release added. So P4 verifies
visibility and sanity, not the direction of the B014 change. B014 itself rests
on code inspection and on the argument in the source: with the old formula a
zero-hop tier's latency was serialisation alone, since the fixed term was
multiplied by avgHops() == 0. The first release that can A/B this is the next
one.

## 1.11.58 -- the interface correction reaches a number, and the units are one

Three items that 1.11.57 left explicitly open, each flagged at the time as
needing a decision rather than a silent change.

### The DQ interface stops being termination-only

`getInterfaceDynamicEnergyNJ()` and `getInterfaceAreaMM2()` had NO CALLERS.
1.11.40 modelled driver switching, PHY and IO area, and the accessors were
never consumed -- so the correction sat in the source and reached no reported
number. Every DQ-interface figure this simulator has ever printed, including
the corpus behind the manuscript, was TERMINATION ONLY.

Both are wired in now, at device and system scope, and the split is printed
rather than folded into one number. Measured on a DDR4 RANK cell:

  termination 3.070 nJ + driver/PHY 9.889 nJ = 12.959 nJ per access (4.2x)
  IO area 2.444 mm^2/die; the comparable total 28.36 -> 47.91 mm^2

Termination was always the small term -- driver and PHY are 76% of the
interface on this part -- which is what the 1.11.40 note meant and what no
result showed.

WHAT THIS DOES NOT FIX, stated plainly because it is the case the original
correction existed for: the accessors return zero unless CACTI-IO has an EXACT
parameter map. CORRECTION (1.11.59): this entry originally said such a map
exists for "DDR3, DDR4 and DDR5 and not for LPDDR5, GDDR6 or HBM". GDDR6 was
wrong -- its electrical row is sourced (POD135, JESD8-21C Cl.D) and its
exact_map is true, so GDDR6 DOES receive the driver+PHY term. The map exists
for DDR3, DDR4, DDR5 and GDDR6; it does not for LPDDR5 (RTT unsourced) or for
HBM2/HBM3 (no electrical row at all). LPDDR5 -- the technology the 1.11.40 note says was understated by
roughly 71x -- receives NOTHING from this change: measured, driver+PHY 0.000
nJ, interface unchanged at 0.036 nJ. What changed for it is that the run now
SAYS the interface is still termination-only and why, instead of reporting
termination as though it were the whole interface. Closing it needs LPDDR5
driver/PHY parameters this tree has no source for, and none were invented.

### One field, one unit (D022/D043 -- the gated change memory_model.h asked for)

The recorded finding was that `getTotalEnergy()` adds JOULES to a picojoule
sum and returns picojoules under a nanojoule contract. Tracing the assignments
found something worse underneath it: in all three NVM models
`read_energy_`/`write_energy_` held TWO DIFFERENT UNITS depending on which
path ran -- picojoules per byte from the architecture object, nanojoules per
access from NVSim. One field, two meanings, differing by 1000/bytes-per-access
(125x at the 64-bit default). Whichever path happened to execute decided what
the number meant, and no consumer could tell.

All four models are normalised to nanojoules per access AT ASSIGNMENT, so the
field has one unit; `getTotalEnergy()` returns nanojoules with the leakage
term converted (watts x cycles, one cycle being one nanosecond -- the
convention legacyNsAsCycles() states, not an estimate of a clock these models
do not have); and the three `printStats` lines that said "pJ" say "nJ".
`memory_model.h`'s contract paragraph, which asked for exactly this as one
gated change rather than a rider on a latent pass, records that the
implementations now obey it. A polymorphic consumer is safe for the first
time.

### The bank-group rung is declared, not invented

`getBankGroupPortBits()` multiplies the bank serialisation width by an
unsourced 2, and since 1.11.56 that reaches a reported number through the
hierarchy link ladder. The architecture object carries no bank-group datapath
field, and nothing in JEDEC fixes a bank-group port width -- a bank group is
not an interface boundary. Adding a field with a "source" would have been
fabrication, so the assumption is DECLARED instead: the ladder line now states
that L2 is asserted, not sourced, and that the other six rungs come from the
architecture object, and the assumption is entered in the stated-constant
register. Six of seven rungs are sourced; the provenance line no longer claims
seven. No number moves.

### Data impact

Every DRAM cell whose accesses cross the DQ pins gets a higher interface
energy (4.2x on DDR4) and a larger area total (1.69x on the same cell). This
is a correction of an omission, not a re-attribution: the term was absent, not
mis-assigned. LPDDR5, GDDR6 and HBM are unchanged and now say why. The
NVM-model unit normalisation moves nothing today -- those accessors still have
no reachable consumer -- but it is what makes one safe.

Gate 1168A, then 1168B. M4's first form asserted only that LPDDR5 "completes
and reports a number" and passed while the correction was doing nothing at
all for it -- the same weak-arm failure as a vacuous pass, in a quieter form.
It was rewritten to assert the substantive claim (driver+PHY > 0 with no note
for DDR4; == 0 WITH the note for LPDDR5) and re-run.

## 1.11.57 -- audit round 3, and the latent traps closed

Round 3 audited the tree again, four auditors in parallel, and found **50 REAL
defects and 12 latent ones**. The uncomfortable part is where they were: rounds
1 and 2 audited years-old code and found 60 and 127; round 3 audited mostly the
previous day's work and found 50, a far higher density per line. Ten of the
worst were introduced by 1.11.55 and 1.11.56 -- the two releases that closed
rounds 1 and 2.

This release also closes the **82 LATENT findings** round 2 recorded but did
not fix, at the user's instruction. A latent defect is one that cannot
currently produce a wrong number: the path is unreachable, the wrong value is
multiplied by zero, or the function has no callers. They are fixed so that
whoever makes such a path live does not inherit a silent defect.

### The knobs that were never read (A001)

`memory.power_down`, `memory.power_down_threshold_ns`, `memory.array_pg` and
`power.temperature_k/_c`, together with the temperature range validator, sat
inside `if (yaml_cfg["noc"]["model"])`. The indentation had shown it all along
-- a 4-space block inside a 16-space body -- but the consequence is
scope-shaped, and that is why no gate caught it: every DEVICE config in the
corpus sets a top-level `noc.model`, and every CO-SIM config declares the model
under `system.devices[].noc.model` instead. So on exactly the shape
co-simulation uses, all four knobs were read from a branch that never executed.

The 1.11.52 temperature work (D055/C001) and the 1.11.52 `memory.power_down`
scope-parity fix (A005) were therefore both inert in co-simulation -- the one
scope they were written for. Measured: the released 1.11.56 binary accepts
`temperature_k: 999` silently; this one refuses it with exit 2.

### The check that could not run (C002)

1.11.56 added a cross-check so that the DRAM architecture object and the
Ramulator preset could never again describe different parts. It sat in
`parseConfiguration()` behind `if (dram_arch_)`, and `initialize()` calls
`parseConfiguration()` FIRST and populates `dram_arch_` afterwards. The pointer
was always null. It never executed once.

Gate 1166D's K11 arm reported "mismatch warnings=0" and passed. That was a
vacuous pass -- zero warnings because nothing ran, not because nothing was
wrong. Moved below the population, it would have caught C001 on its first run.

### The half-applied speed-bin fix (C001)

1.11.56 changed the three DRAM architecture objects to name the preset this
tree simulates and left the CACTI-IO rate table at DDR5 4800 and HBM2 2000 --
a table sitting directly under the D002 comment that states the principle.
DDR5's electrical map is injected and sourced, so `exact_map` is true and
CACTI-IO's figure REPLACES the scheme table: DDR5 termination energy was
quoted for a part 1.5x faster than the one whose cycles are counted.

### The ladder, corrected (B001, B002, C003, C005, C007)

1.11.56's sourced link ladder was right in principle and wrong in five places.

- B001: `RamulatorWrapper` builds an architecture object for DDR4, DDR5, HBM2
  and HBM3 only, and hands DDR3, LPDDR5 and GDDR6 the DDR4-2400 one from an
  unannounced else. Measured, DDR3/DDR4/DDR5/LPDDR5/GDDR6 all printed the
  identical ladder `256/8/16/8/64/64/64` under a line reading "from the GDDR6
  architecture object" -- a false provenance claim, the category this project
  treats most seriously, and 1.11.56 wrote that line.
- B002: the reconciliation check that detects exactly this ran sixty lines
  later and only warned, after the ladder had consumed the numbers.
  It now runs FIRST and GATES adoption. A technology whose object does not
  reconcile with its own simulated preset keeps the per-technology table --
  which the source already labels a placeholder -- and the run says plainly
  that its ladder is not tool-sourced. Refusing to claim provenance is the
  honest outcome; fabricating objects for the three missing technologies is
  not.
- C003: the bank and bank-group rungs were stored literals while every other
  rung derived from width x clock, so back-deriving frequency as
  `bandwidth*8/width` clocked HBM3's bank tier at 1.0 GHz against its own
  3.2 GHz core clock. Derived now: HBM3 16.0 -> 51.2 GB/s (a 64 B L1/L2
  crossing 4.00 -> 1.25 ns), DDR5 2.4 -> 1.6, HBM2 8.0 -> 9.6, DDR4 unchanged
  as the one self-consistent row.
- C007: `channel_databus_bits` meant ONE channel for DDR and the WHOLE STACK
  for HBM. That is the same family-dependent meaning that broke the
  reconciliation test in 1.11.56. It means one channel for every family now,
  and the cross-check multiplies by the channel count. HBM3's L5 rung was 16x
  too wide; HBM2's 8x.
- C005: the bridges joining those levels were still the placeholder table, so
  every tier crossing contradicted the levels on both sides of it.

### One clock, asked once (B003, A007)

1.11.56 fixed a unit error in the hierarchy latencies and introduced a clock
error. The ns-to-cycle conversion used `config.frequency_mhz`, which in system
scope is the top-level `system.frequency_mhz` -- the adoption block copies the
device node's technology, PE count, placement, banks and NoC model, and not its
frequency. On the shipped co-sim example that is 2000 MHz against a 500 MHz
device: a 4x overcharge on every hierarchy traversal, summed in one integer
with an array latency correctly quoted at the device clock. The NoC duty-cycle
window (A016, 1.11.52) had the same defect independently. Both now ask one
function, so a third site cannot invent a third answer.

### The flush mechanism, replaced (D001, D002, D003)

1.11.55's three coherence-flush fixes were each locally reasonable and jointly
wrong, so the mechanism is replaced rather than patched.

F016 divided the footprint by the rank count on both the cycle and byte sides;
F017 then cleaned the GLOBAL registry M->E; and the charge runs once per rank.
The first rank to arrive charged F/N and wiped the dirty set, and ranks 2..N
measured nothing -- so the slices summed to **F/N instead of F**, an N-fold
understatement (16x on a 16-rank cell) of the flush bytes and of the flush's
DRAM writeback energy, aimed straight at the pecount sweep D4 exists to
protect. Separately, F015's last-level counting rested on an inclusion argument
that zsim breaks on purpose: a GETX hitting an E line upgrades silently in the
child, so an ordinary read-modify-write line is M in the L1D and E in the LLC
BEFORE any flush. F015 was blind to that working set from its first flush.
And F017 turned a latent unlocked read walk into an unlocked WRITE while other
ranks were still executing, which could lose dirty bits and trip a compiled-in
assert.

Now: measurement and cleaning are ONE locked walk that de-duplicates by
ADDRESS, which is correct whatever the hierarchy does; the per-rank charge is
an explicit epoch whose slices sum to the footprint exactly once, with the
integer remainder to the opener so the sum is exact to the byte; and the walk
holds each cache's own bottom-CC lock, one at a time, never two at once.

### Everything else

Scope parity: the placement never reached the array query in co-simulation
(B053, 1.11.56) and the dual-McPAT host read its clock, all four cache sizes,
memory technology and process node from a legacy block an explicit `system:`
config never writes (A005) -- 3000 MHz against the node's 2000, a 1024 KB L2
against its 256, a phantom 8 MB L3, DDR4 controller parameters for a DDR5 host.
That same host was priced on the "aggressive" metal stack the config validator
tells users no process can build, applied by omission (A006).

In every co-simulation the memory controller and the M/D/1 service rate were
derived for SRAM -- the constructor default -- because `getMemControllerConfig`
ran before the device's technology was adopted (B009). The shipped host/device
example, with a DDR5 host and a DDR4 device and no SRAM anywhere, opened with
`SRAM M/D/1 cap = ... 512000 MB/s`.

The row-hit "measurement" indexed a per-device JEDEC page with a rank address
(D006, up to 8x on the miss fraction, ~19% on DDR4 per-access read energy) and
tracked one open row per placement UNIT rather than per bank (D008, up to 3.4x
at RANK/CHANNEL/CHIP). The last site still dividing by an invented NVM access
time was found and removed (B012). `pages_per_unit` and `num_banks` were both
corrected where 1.11.56's own slot-count change had broken them (B010, B011).

Of the 82 latent findings: unit errors that were waiting to become live (mW
into W, J into nJ, a fallback 1e9 too large and in the wrong unit, an unsigned
underflow); silent fallbacks made to announce themselves or refuse; invented
values replaced by the tool that could answer, or labelled as unsourced where
none could; nine YAML keys that were parsed and never read, each now either
wired up or rejected at parse time; and dead code deleted, including
`SimConfigParser`/`ComprehensiveSimulator`, which fabricated results under
external-model banners, and `NVMModel`, which the factory never built.

One latent was fixed the OTHER way: D030 said a wire capacitance and its
comment disagreed by 1000x. The value was right and the comment's unit was
wrong -- fixing it as the finding proposed would have introduced a real 1000x
error.

### Data impact

Everything already true of 1.11.56 remains true, and this release moves more.
Largest first: the flush footprint and its writeback energy (Nx on multi-rank
co-sim); the hierarchy traversal clock in co-simulation (4x on the shipped
example); the HBM bank and channel ladder rungs (3.2x and 16x); the row-miss
fraction and the per-access array energy that follows it; DDR5 termination
energy (1.5x); the co-sim memory controller and its M/D/1 cap; the host's
clock, caches and metal stack on the trace path.

The corpus must be re-simulated on this train. That was already the plan; the
reasons are now larger.

Gate 1167A. Every arm that tests a diagnostic asserts it FIRES on a case built
to trip it before asserting it is silent on a case built not to -- three arms
in the 1166 series passed vacuously, reporting zero warnings from checks that
could not execute, and twice that hid a real defect.

## 1.11.56 -- the last 65 of audit round 2, and the tools get asked

The closing block of audit round 2. Round 1 emptied the FIX-PRE-FLEET queue
(1.11.46-1.11.51); round 2 raised 254 findings, of which 127 verified REAL.
This release closes the remaining 65 and the round with it. The theme is one
sentence: where a tool in this tree can answer a question, ASK IT, and where
none can, SAY SO instead of writing a number down.

### The hierarchy ladder becomes physical (D064, D065, B051, B041, B060)

- D064/D065: the seven per-level link widths came from ten per-technology
  tables the file itself labels "design-specific placeholders ... Current
  values may be inverted". They contradicted the DRAM oracle, which carries
  the real ladder per tier. DDR4 L3 -- the level named "chip DQ pins" -- was
  192 bits against the 8 an x8 part has, a factor of 24; DDR5 L5 was 88 bits,
  a width chosen to land near 26.4 GB/s and a bus no DRAM has. The
  architecture object is now the one authority (GSA datapath, bank
  serialisation, bank-group port, chip DQ, rank bus, channel bus) and the run
  prints the ladder it used. YAML overrides still win; the tables survive only
  as the fallback for a technology with no architecture object.
- B051: those levels do not share a clock -- since D064 the DQ tiers run at
  the data rate and the array tiers at the core clock -- yet seven level
  results and six bridge results were SUMMED into one integer spent as PE
  cycles. The bridge term was worse: a router cycle count, a nanosecond
  constant and a beat count went into one number. Both now return TIME, and
  the conversion happens once, at the PE clock. Bridge crossings are
  store-and-forward, so the serialisation cost is the SLOWER of ingest and
  egress rather than a beat count at the narrower width.
- B041: the tiered ladder was flattened to a single number with zeroed
  bridges whenever the topology was still MESH_2D -- which is a proxy for
  "the analytical model is in use", because the detailed path sets CUSTOM.
  An analytical and a detailed run of the same device were comparing two
  HIERARCHY models, not two network models. The flattening now applies only
  where there is no tiered ladder.
- B060: RANK, CHANNEL and LOGIC_DIE placements all collapsed to one slot --
  RANK by an explicit "one rank" that ignored memory.ranks_per_channel, the
  other two by having no branch at all. A 16-PE LOGIC_DIE HBM3 cell was
  described as sharing ONE memory organisation, every PE mapped to org 0, and
  pages_per_unit became the whole device in one block. The three top tiers now
  count ranks and channels; the finer tiers keep the one-rank frame the
  placement tree is built in. (Gate 1166B caught the first attempt, which
  multiplied the fine tiers by the channel count too: arithmetically
  defensible, but it took an HBM3 BANK cell from 512 organisations to 8192 and
  the tree-coverage check refused the run -- correctly, since the tree would
  then have priced a sixteenth of the memory the config described.)

### Ask the tool (B004, B012, B010, B018, D031)

- B004: the M/D/1 bandwidth cap of every SRAM/NVM memory was banks x line
  divided by an INVENTED access time -- 10 ns for STT-MRAM, 50 for PCM, 20
  for ReRAM, 2 for SRAM -- while NVSim and CACTI were being queried for the
  same array's read latency a few thousand lines away. The cap now comes from
  the array model at the run's node, corner, temperature and line size, and
  the run states the arithmetic. Where the characterization is unavailable it
  says so and derives no cap rather than substituting one.
- B012: "the fastest per-channel DRAM in the lineup" was a literal at three
  sites (51200 MB/s twice, 115.2 GB/s once), each scaling something reported.
  Both are now read from the HBM3 preset, so a preset change moves them.
- B010: pages_per_unit was derived from a fixed 128 KB subarray while
  memory.subarray_height is a real, resolved knob. Setting subarray_height:
  1024 doubled the physical subarray and left the workload's contiguous block
  at 128 KB, so PE locality and routed hop distance were computed against a
  geometry the config did not ask for.
- B018: every SRAM/NVM latency query hardcoded a 512-bit access while
  system.cache_line_size reaches ZSim as sys.lineSize. A 128 B run simulated
  128 B traffic against an array characterized for 64 B, and array access
  time is close to linear in output width above the bitline.
- D031: the six NVSim delay accessors returned fixed fractions of the mat
  read latency (0.10/0.20/0.45/0.15/0.05/0.05). They now read NVSim's own
  terms. Two structural facts are stated rather than faked: NVSim has no
  separate wordline term, and columnDecoderLatency is MAX'd against the row
  decoder so it is not the additive column term.

### Refuse rather than invent (D032, B028, B029, B015)

- D032: a failed NVSim characterization set valid_ = false and returned, and
  every query returns 0.0 in that state -- so a reachable configuration
  (PCM at either low-power corner, measured 4/4 failures at 22 nm / 64 KB)
  printed an NVM array of 0.000 nJ read, 0.000 nJ write, 0.000 mW leakage and
  0.000 mm^2 with one stderr line to say why. The failure now propagates.
  Callers that can degrade already catch it and say so; the power block, for
  which the array IS the memory power, is fatal like the McPAT path.
- B028: naming the controller explicitly ("memory.controller.type: simple" on
  an NVM device -- the controller the code would have chosen anyway) jumped
  over the SRAM/NVM bandwidth derivation entirely, leaving the struct default
  of 6400 MB/s standing as the memory's physical cap.
- B029: two catch-alls dropped user input silently. A non-numeric
  power.mcpat_overrides value vanished; one malformed key under synthetic:
  discarded pattern, packets, injection rate and the sweep together, and the
  run reported the built-in defaults under the requested experiment's name.
  Both are config errors now.
- B015: the host link controller's load fraction took a 0.01 default on the
  system-scope trace path, where the per-node path MEASURES it. Nothing
  measured one percent. With no crossing statistics the load is zero, stated;
  leakage is still priced.

### Say what is assumed (B005-B009, B011, B013, B014, B016, B042)

Eleven quantities in the config path are neither measured nor tool-sourced
and all of them are live: the per-technology host-path latency splits, the
host/device bridge table, the flush and kernel-launch fixed costs, the
system-link presets, the analytical H-tree scaling constants, the
network-interface overhead, the MLP degree, and three bare cycle counts.
There is no tool in this tree that answers "how long does a kernel launch
take", and inventing a citation would be worse than admitting there is none.
The run now DECLARES them -- value, overriding key, and what each governs --
for the ones in effect, so a reader can see which numbers rest on an
assumption. B042 joins it: three different quantities were emitted under the
name "the memory's bandwidth" (rank bus, aggregate, per-channel), differing
by up to 16x on HBM3 with nothing saying whether that is the channel count
doing its job. The three are now stated with their scopes and their
reconciliation is checked.

### Scope parity (B053-B058, B061, B059, B046, B049, B057)

Six features existed in device scope and were inert in co-simulation:

- B053: the placement never reached the array query, so the 1.11.25 tier
  model was inert for every system-scope run and a SUBARRAY-placed and a
  CHIP-placed PE were charged the identical access.
- B054: pim.pe.bit_serial and pim.pe.issue_width were parsed into the global
  config, which the system-scope emitter does not read -- a bit-serial
  element was co-simulated as a parallel one (32x per-op cycles at
  operand_width: 32).
- B055: the "PEs at this tier require a memory controller" error could never
  fire in system scope, because the flag it tested was a tautology.
- B056: reportFpWithoutFpu returned at its first line even when hasFpu=false
  had been emitted, so the contradiction it exists to surface was invisible
  in exactly the scope that can configure it.
- B058: setting devices[].noc.model overwrote every per-tier noc.levels
  choice -- and did NOT when the key was absent, so the clobber depended on
  an unrelated key being present.
- B061: the PCIe cycle conversion used the top-level system clock while the
  host cores that spend those cycles run at the reference clock. With the
  reference co-sim that is a 1.5x understatement of every offload transfer.
- B059: the on-DRAM-die node pin overwrote an invalid technology.node_nm
  before the "unconditional" validator saw it, so a sweep over {22, 28, 32}
  silently produced identical numbers at 22 and 28 for on-die placements --
  the exact plateau 1.11.51 set out to remove.
- B046: two core-type normalisers had already drifted; "compute_unit_pe" was
  a valid element in one scope and a fatal error in the other. One function.
- B049: noc.clock_mhz reads as a fabric override and sets the whole device
  clock, overriding an explicit system.frequency_mhz because it is parsed
  later. The effect stands (there is no separate NoC domain here) and the run
  now says what moved.
- B057: --print-mem-info answered from inside the argv loop, before any YAML
  was read, so every config at a node other than 22 nm got a printed access
  latency for 22 nm silicon -- and the composed co-sim driver copies that
  number.

### Report honestly (A012, A024-A031, C-block, D-block)

- A012: the upper-level NoC fan-in ladder fell back to a literal ratio of 2
  where the true ratios are the rank and channel counts. Live on every
  analytical run, and it divides NoC area and leakage at the top two levels:
  HBM3's channel level got 2x too few nodes and its system level 4x too many.
- A029/D074: Refresh, Background and Leakage were printed as three peer lines
  when they are one quantity -- Background CONTAINS refresh, and a DRAM
  array's standby current IS its leakage. Adding them double-counts refresh
  and doubles standby, about 2.1x. Now nested under Background and named.
- A024: the gap-residency refusal was silent at both call sites, so on GDDR6
  and HBM the reader could not tell "refused, no sourced tXP" from "applied
  and agreed". A025: a parsed pipeline_duty_cycle override was echoed as
  "[OVERRIDE from YAML]" and cannot reach McPAT, which derives the ratio
  itself. A028: the power-down message named pim.mc.pg, a key 1.11.45 split
  off and this branch has never read. A030: the printed pJ/bit band was the
  table's even when an override replaced the value. A031: the per-die
  factorisation beside the System Total described only the last technology
  priced, so on a decoupled system it did not multiply out to the number in
  front of it.
- C002: the pcie_gen4 link energy took the midpoint of a band whose lower end
  is a TX-only bound, not a full-link figure. The bound is excluded from the
  applied value: 3.965 -> 6.0 pJ/bit.
- C006: the L2/L3 duty cycles were unsourced fractions of a base that is
  1.0 by construction. Measured now (accesses per cycle at that instance).
  McPAT reads cache duty_cycle only on the TDP branch, so reported PEAK power
  moves and runtime dynamic does not.
- C032: the 1.11.29 "report only what the user defined" rule lived in the
  printer and not in the total, so stub components left the breakdown and
  stayed in the sum. One predicate for both; totals move down.
- C036: an l1i with cache.l2.enabled: false emitted L2_config with a zero
  capacity, which kills the McPAT child with exit 21. That config runs now.
- C010/C014/C025/C027/C030/C033/C034/C017/C023/C003/C008: dead hooks removed,
  false header contracts rewritten to describe what the code does, and
  one-time warnings added where a constant could not be sourced.
- D054: a hardcoded 1 GHz turned tool SECONDS into "cycles" in four places.
  The models carry nanoseconds now; the legacy cycle-returning entry points
  convert at one documented helper that states the 1 cycle/ns is the ABSENCE
  of a clock, not an estimate of one.
- D006/D037/D045/D046/D058/D062: an approximate IDD2P column under a header
  claiming part-number sourcing; six DRAM generations printed as distinct
  labels while all are characterized from the same 22 nm CACTI table; a
  reset latency of write x 0.3; a hardcoded "30x read" printed over a
  computed number; an inverted leakage basis; and a SET latency that silently
  falls back to the generic write path on every cached characterization.
- D073: the ASCII-only guardrail. The census was larger than the finding
  stated -- 193 non-ASCII bytes in main.cpp alone. src/, include/,
  benchmarks/, examples/ and docs/figures/ are now byte-clean outside
  external/.

### One part, one speed bin -- three more technologies (found by B042's check)

B042's reconciliation check is the first thing in this release to find
something the audit did not. It fired on HBM3: the rank bus reported 32 GB/s,
the channel 512 GB/s, and the aggregate 819 GB/s over 16 channels, i.e. 51.2
per channel. Two of those cannot both be a channel.

The cause is D002's defect at three more technologies. A DRAM run has two
sources of truth -- the Ramulator preset, which decides the CYCLES the
simulator counts, and the architecture object, which decides every bandwidth
it REPORTS -- and nothing held them together:

  HBM2   architecture 2000 MT/s   preset HBM2_2.4Gbps    (1.2x)
  DDR5   architecture 4800 MT/s   preset DDR5_3200AN     (1.5x)
  HBM3   architecture 4000 MT/s   preset HBM3_6.4Gbps    (1.6x)

Only DDR4 agreed. So a DDR5 device's reported mem.bandwidth -- the M/D/1
service rate the timing model queues against -- described a part 1.5x faster
than the one whose cycles were being counted, and since D064 that same rate
also sets the hierarchy link ladder. The preset is the authority (it is what
produces the cycles), so each architecture object now names the bin the tree
actually simulates, with its core clock and burst time following. The ns
timings are absolute and stay.

A cross-check now runs at wrapper initialisation: channel width x data rate
must reproduce the preset's aggregate, or the run says that it is counting
one part and pricing another. A future preset change that forgets the
architecture object fails there instead of quietly re-describing the part.

B042's own check needed a second pass, for the same reason it was worth
adding. Its first form compared the architecture object's "channel" bandwidth
against rank x ranks_per_channel, which is a DDR reading: for HBM that field
is the whole STACK (16 pseudo-channels of 64 bits) and the field called
"rank" is one pseudo-channel. So it reported HBM3 as broken after the rate
fix had already repaired it. The test is now the relation that holds whatever
each tier is named -- aggregate == rank bus x ranks per channel x channels --
which is also precisely the relation the speed-bin drift broke (HBM3:
51.2 x 1 x 16 = 819.2 GB/s, against the preset's 819).

### Data impact

Number-moving, in rough order of size: the three speed-bin corrections
change every reported bandwidth (and therefore the M/D/1 cap and the link
ladder) for DDR5, HBM2 and HBM3; the hierarchy ladder and its clock domain
(D064/D065/B051) change every hierarchy traversal on a DRAM device --
measured 12.74M -> 13.14M cycles on the HBM3 reference cell, and 323395 ->
349318 on the OoO device cell;
B004 changes the M/D/1 cap of every SRAM/NVM cell; D031 changes NVM
inner-bank latencies; C002 changes gen4 link energy by 1.51x; A012 changes
NoC area and leakage on analytical runs; C006 changes reported peak power;
C032 lowers system totals; B061 changes every co-sim offload transfer.
The whole corpus needs re-simulation on this train, which was already the
plan. B060, B058, B028 and C036 unblock configurations that previously
misreported or refused to run; D032 turns a silent zero into a refusal.

Gates 1166A, 1166B and 1166C.

## 1.11.55 -- the coherence flush stops charging for work it never did

The F block of audit round 2 (zsim fork). The flush trio are one story: the
same footprint was inflated three independent ways, and it drives BOTH host
cycles and the bytes priced as DRAM writebacks.

- F015: summing dirtyBytes over every registered cache DOUBLE-COUNTS an
  inclusive hierarchy. When a child writes back,
  MESIBottomCC::processWritebackOnAccess drives the PARENT's state E->M, so
  one dirty 64 B line is M in both L1D and L2 and was counted as 128 B (3x
  with an L3). Counted at the last level now, which holds each dirty line
  exactly once; the run prints which level it chose and states the
  inclusion assumption (a non-inclusive hierarchy would make it an
  under-count). Measured: 54912 -> 33920 B.
- F016: the flush TIME used the whole global footprint while the flush BYTES
  used the per-rank slice -- and the charge runs once per rank. So the
  timing half carried exactly the slope in N that D4 removed from the energy
  half, aimed at the pecount sweep D4 exists to protect. One working set,
  one division. Measured with F015: 6120 -> 3934 flush cycles.
- F017: the flush measured the dirty set, charged for it, and left every
  line in M -- so offload #2 re-charged the same working set, and both host
  cycles and flushBytes grew linearly in the number of offloads for data a
  real flush had already written back. The function's own comment argued
  the dirty set "is a different number at each one", which is precisely what
  the code prevented. Lines now go M -> E: memory is current, the line stays
  valid (a writeback flush, not a wbinvd). The charge is unique dirty data;
  cleaning touches every level holding a copy, and the two figures are
  labelled as the different quantities they are.
- F002: GapHist::arm() reset the span but not the buckets, and it fires at
  EVERY ROI_BEGIN -- so a multi-ROI workload accumulated gap cycles across
  all of them while the span described only the last. The residency ran past
  1.0 and was silently clamped, presenting as a plausible 100% idle.
- F004: firstCycle held the ARM cycle while the recorded gaps telescope from
  the FIRST EVENT, so the span was longer than the samples cover and the
  derived residency was biased low by the head of the ROI.
- F008: under thread-MPI, ROI_BEGIN returns before the legacy path's
  pgres.markRoi(), so the PG counters and the devMC gap histogram were armed
  at a later event -- or never, when roi_begin precedes any barrier or
  send/recv, leaving every bucket empty and silently falling back to the
  phase-granular residency.
- F001/F003/F006/F007 (truth-in-comment on a LOAD-BEARING path, since 1.11.51
  reads this histogram back into DRAM background power): dropped samples
  MERGE gaps rather than shrinking them, so they inflate residency and lower
  background power -- the opposite of what the note claimed; a bucket
  straddling the threshold is skipped entirely, an UNDER-estimate where the
  comment said over; "nothing reads this back into the model" is false; and
  two lines reported "% of run" against an ROI-span divisor.
- F005/F009 recorded as TRAPS with the exact conditions that would make them
  live (a device topology terminating at a grand MC; a crossing site before
  roi_begin), rather than "fixed" where nothing reachable differs today.

Also: PIMID_QEMU pins the guest emulator. Each tree otherwise prefers its own
external/qemu build, so an A/B gate silently compared two emulators -- worth
a few accesses of guest-stream difference, which moves NoC duty.

N6b (recorded in the audit queue, binding on gate design): this engine's
local/remote split is sensitive to guest ADDRESSES, and env padding, object
size, in-run host work and path length each perturb them. On the ooo_dev cell
that is ~19 packets in 25396 (0.075%) and power in the 5th significant digit
-- two orders MORE sensitive than N6 recorded, because this cell's packet
count is 54x smaller. Cycles were identical in every run. A cross-BINARY gate
arm therefore cannot use bit-equality: cycles exact, power within 1e-4
relative.

Gate 1165A/B/C.

## 1.11.54 -- the fork stops charging one thing twice

The E block of audit round 2 (the McPAT fork) plus two zsim ROI-window
findings. Three of these are double-counts; one is a ruling that CONFIRMS
the current behaviour and rewrites its false justification.

- E008: the SerDes was charged twice. McPAT's PCIe dynamic contains
  SerDer_dyn whenever withPHY is set -- and PIMID sets withPHY for every
  link class except interposer -- while 1.11.41 also ADDS the measured
  bytes x pJ/bit for the same silicon. The 1.11.41 rationale ("different
  silicon from the SerDes") holds only at withPHY=0. Our band is the
  better-sourced of the two (it names its process and whether clocking is
  included), so it stands and McPAT's SerDes DYNAMIC leaves the RUNTIME
  basis; AREA and LEAKAGE keep it, because the SerDes is real silicon that
  really leaks. The peak/TDP basis also keeps it: there we add no pJ/bit
  term, so nothing is duplicated. The correction announces the watts it
  removes and the duty they were computed at -- it scales with link
  utilisation, so it is invisible on the reference cell (duty 0.002) and
  dominant on a busy link (34.7 mW/channel at gen5, 217 mW at UALink,
  against a controller term of a few mW).
- E004: gate leakage was scaled by the SUBTHRESHOLD ratio. Gate leakage is
  oxide tunnelling (McPAT: cmos_Ig_leakage; CACTI: I_g_on_n rows), not
  subthreshold conduction, and PIMID reports it. It now takes its own ratio
  -- and the MEASUREMENT that came with the fix is recorded: CACTI carries
  NO gate-leakage data for the DRAM device columns (I_g_on_n is 0 for
  lp-dram and comm-dram at 22, 32 and 45 nm), so the ratio is unsourced
  today and the run says so. The subthreshold ratio stands in for it, which
  is directionally defensible -- a periphery device is high-Vt AND
  thick-oxide, and both mechanisms collapse together -- with the magnitude
  stated as unsourced.
- E001: onPitchLogicShare() added corepipe again after McPAT had already
  DISTRIBUTED it into ifu/lsu/exu/mmu (core.cc adds
  corepipe*num_pipelines/4 to each; the direct add is disabled). At the
  emitted num_pipelines=1 that over-counted the pipeline by 0.25*corepipe;
  measured logic share 0.184867 -> 0.183517. Its adjacent finding too: the
  [tech] line printed the pitch as a flat fa x pitch (3.05556x on the
  SUBARRAY/HBM3 cell) while the transform applies
  fa x (1 + (pitch-1)*share) = 2.5574x -- a 19.5% gap between the printed
  claim and the real factor, under a comment promising "the factor actually
  APPLIED".
- E002 RULED (user), behaviour CONFIRMED, justification REWRITTEN: the
  earlier comment exempted the core's SRAM arrays from the pitch penalty on
  the grounds that "CACTI's tables declare cell area device-independent".
  That misreads the tables -- area_cell is 146 (292 for the 2-port cell) in
  EVERY device column at EVERY node because it is expressed in F^2, the
  ITRS convention, not because a periphery device leaves an SRAM cell
  unchanged; CACTI simply has no data for SRAM-in-DRAM-periphery.
  Physically an SRAM cell there DOES suffer: six ordinary transistors on
  thick-oxide, long-channel, relaxed-rule devices, whose larger F is
  precisely what the l_phy ratio measures -- so area_cell = 146*F^2 at the
  periphery F IS the fa scaling. fa therefore stays on the whole core,
  arrays included (no numbers move). The PITCH exemption survives as the
  narrower claim it always should have been: must the circuit be DRAWN ON
  the array's bitline pitch, which is layout geometry, not device size.
- F008: under thread-MPI, ROI_BEGIN returns before the legacy path's
  pgres.markRoi(), so the power-gating counters and the devMC gap histogram
  were armed at a later event -- or never, when roi_begin precedes any
  barrier or send/recv, leaving every bucket empty and silently falling back
  to the phase-granular residency. Both handlers now open the window where
  they open the ROI.
- F004: GapHist::firstCycle held the ARM cycle while the recorded gaps
  telescope from the FIRST EVENT, so spanCycles() returned a denominator
  longer than the samples cover and the derived residency was biased low by
  the head of the ROI. The first event now defines the span's start.

Gate 1164A/B/C. BANK/HBM3 device cell bit-identical vs 1.11.53.

## 1.11.53 -- the interconnect stops substituting what it only cross-checks

The C block of audit round 2 (power wrappers): seven verified-REAL findings,
all of them cases where a number was allowed to stand for more than it was.

- C020: the CACTI-IO substitution gate had widened from "exact map" to
  "exact OR injected", and the paragraph justifying it says "its electrical
  layer is sourced" -- which is FALSE for LPDDR5 by our own record (its
  RTT = 240 ohm is flagged UNSOURCED in the same table; Micron gives VDDQ
  and RON, not Rtt). An assumption was silently REPLACING the DQ termination
  energy, the interface dynamic energy and the IO area. Substitution now
  requires a fully sourced injection; an unsourced one is still computed and
  reported as a cross-check, and says so.
- C021: two false claims in the code -- "its 14 Gb/s rate will be refused"
  and "GDDR6 refused above; no parameter set to give a structure to". The
  fit-range check is on the BUS CLOCK (7000 MHz against an 8000 MHz
  ceiling), so GDDR6 passes and is fully modelled; with no GDDR6 branch it
  fell into the DDR default of a 25-pin command bus amortised over 32 data
  lanes -- the same fixed-CA-over-few-lanes shape the LPDDR5 note records as
  producing ~71 pJ/bit against a real 3-6. GDDR6 now carries its own
  structure (32 DQ, WCK/EDC per byte, ~12-pin JESD250 CA bus): 0.632
  nJ/access at RANK placement.
- C022: the injection was partial while the source string announced
  "ELECTRICALS INJECTED". rtt2 is the far-end termination of the same DQ net
  and is now injected with rtt1; rs1/rs2, rtt_ca and z0 stay borrowed, and
  the string says which is which.
- C031: an FPU-less element could be charged twice. main.cpp zeroes
  num_fpus from the NODE's floating_point flag, but the wrapper's ALU branch
  OVERWROTE it from pe_has_fp (a different flag, set elsewhere), so the XML
  could carry `lanes` FPUs while the soft-float fold -- which gates on
  config_.num_fpus == 0 -- ALSO charged the FP ops as integer work. The
  caller's explicit zero now wins, and the FP register file and issue width
  follow the same decision. Gate: device ALU emits FPU_per_core 0; in co-sim
  the device is 0 and the host keeps its FPU.
- C016/C007: the "warned fallback" that never warned. Three blocks
  substitute unsourced fractions of the retired instruction count when a run
  carried no measurement (mispredicts 1%, loads/stores 20/10%, int/fp/mul
  70/10/5%) and all three were silent, so a run priced on invented activity
  looked exactly like a measured one. One latched warning names what was
  missing and what stood in.
- C011: on_dram_die defaults to 1 and two of the four setNoCLevels call
  sites left it there -- handing a HOST's fabric the DRAM-periphery
  transform. Both now take the family answer.
- C013: the CoreBreakdown blamed its own gap on undiffCore, which is IN the
  block sum. The real gap is population (one core vs all) times leakage
  basis (runtime vs peak, ~6x apart per 1.11.33) -- about two orders on a
  16-PE device. Both lines now state their population and basis.

Gate 1163A/B + G5. HBM3 device cell bit-identical vs 1.11.52 (0.383167 W),
as expected: HBM has no injection set, so none of these apply to it.

## 1.11.52 -- the second audit round: the model stops assuming what it can measure

Audit round 2 (six parallel auditors, 254 findings) was VERIFIED finding by
finding against the tree before anything was changed: 103 REAL, 110 LATENT
(true as written, but no reachable configuration makes them differ today), 3
WRONG, 18 already fixed. This release lands the first 30 REAL ones, chosen
by how far they move a number. Verification records: `_1162audit/V_*.md`
(LOCAL).

**The three that move device numbers most.**
- D024: `DRAMModel` hardcoded `MemoryTechnology::DDR4` and handed its
  Ramulator wrapper a default "DDR4", while the factory routed all seven
  DRAM generations into it. Every non-DDR4 PE at SUBARRAY/BANK/CHIP
  placement was therefore timed on the DDR4-2400 ladder -- subarray 26.6 ns,
  bank 39.9 ns -- and the log printed that as the technology's own number.
  The technology now reaches the model and the wrapper.
- D003: the array energy weighted its activate/precharge term (the dominant
  one: ~1.42 nJ against ~0.39 nJ of burst on DDR4) by a hardcoded
  ROW_MISS_FRAC = 0.5. The PE memory interface already sees every access and
  the unit it lands in, so the open row is observable there: one last-row
  register per unit, exported as rowHits/rowMisses, and the array model uses
  the run's own miss fraction. Unmeasured runs say so and the 0.5 becomes a
  stated fallback rather than a hidden default.
- B001: the PE-MI's local access latency was a per-technology table in
  CYCLES -- not a property of an array. The same silicon was charged 10
  cycles at any PE clock: 20 ns at 500 MHz, 2.5 ns at 4 GHz, an 8x swing
  across a frequency sweep, on the base term of every PE-MI access. It now
  comes from the array model at the PE's own tier and node, in ns, converted
  at the PE clock.

**One part, one rate (D002/D016).** DDR4 had its array half priced at 2400
MT/s and its DQ-termination half at 3200 (termination 1.33x understated);
the rate table now names the part this tree actually simulates (DDR4-2400 --
the Ramulator preset, the architecture object and the wrapper's own 19.2
GB/s all agree), and POD12 is an interface standard so the electricals are
untouched. DRAM bandwidth is derived (rate x channel width x channels)
instead of a literal that had drifted from it: DDR5 was 20% low, GDDR6 1.75x
low. Where upstream Ramulator2 ships no timing bin at the modelled rate
(DDR5 above 3200), the run says so once instead of hiding two numbers that
disagree.

**Temperature (user ask) reaches the whole machine (D055, C001).** The new
`power.temperature_k` / `temperature_c` knob was priced into cores and
caches while every memory array stayed pinned at 350 K. It now reaches all
four plugin models, the SRAM/NVM latency queries, the array power/area
queries and the DRAM die-area query -- after temperature joined the NVSim
cache key, since otherwise a 400 K query would be served the cached 350 K
design (the LSTP-served-HP failure of 1.11.49). The accepted range follows
the LINKED TOOLS, each read from its own source: CACTI checks 300..400 K in
steps of 10 (io.cc:2259) and NVSim indexes a 300..400 K table with no bounds
check at all (Technology.h:81-84), so the intersection is enforced and the
refusal cites both. And C001: the DRAM-periphery leakage factor read the
I_off row as Celsius (T-273) where CACTI indexes it as an offset from 300 K
(parameter.cc:175) -- at the default 350 K it took the 380 K row, understating
the leakage ratio by ~34% on every on-DRAM-die run.

**Cross-scope divergence closed structurally (A001-A009).** 1.11.17 tried to
end it by COPYING device-scope logic into the per-node path; the copy had
drifted on all three of its claims (corner refused instead of mapped to
lp-dram; area factor from literals 2.44/2.46 instead of the tables; a
literal 700 MHz clock guard instead of the calculated ceiling) while its
comment said "Same behavior as the device-scope site". The copy is deleted
and both scopes call one owner. Also unified: MC count per placement-tree
region, memory.power_down no longer dead unless pim.mc.pg is set, the
unarmed-counter refusal, the die organisation (printed die == die added to
the total), and the SRAM/NVM tool inputs.

**Reports that described two different machines.**
- A015: background power counted ONE rank while die area counted the whole
  populated system, so the Power and Area lines differed by ranks x
  channels. Background is now population-scaled on the same basis as area.
- A020: a memory with no accesses returned silently, deleting its background
  from System Total -- but standby and refresh do not stop. On the reference
  co-sim cell the memory term is 1.12 W of 2.34 W.
- A018/A019: two predicates answered "does this access drive off-package DQ
  pins?" and disagreed at the interposer tiers; and the DEVICE's placement
  decided whether the HOST's own DIMM paid termination. One predicate now,
  asked per memory.
- D036: the system-scope die area printed as "CACTI x k" is, at the stock
  organisation, algebraically capacity/density with zero CACTI content. The
  number is right (the vendor anchor is the sourced one); it now says what
  it is.

**Silent substitution (B019/B025/B026/B027, A021, A016/A032, A010/A011,
B038/B039/B043).** A YAML error thrown midway through applying a config
printed "Warning" and ran on with a half-applied machine -- now fatal. An
unrecognised network-model name silently became SIMPLE -- now refused. The
NoC duty cycle divided measured traversals by cycles of a different clock,
and skipped levels dropped their share of the measured hop count while
leaving chip coverage below 1. The flat NoC branch still carried the
pre-1.9.22 shape (hardcoded flit width, PE clock for the network's, packets
where McPAT counts router traversals -- a measured 2.04x undercount). Host
NoC coverage fractions and the host MC clock were unsourced literals. And
three "two authorities for one quantity" cases: the on_dram_die predicate
missing the family owner's guard, PCIe protocol overhead emitted from
different sources depending on an unrelated toggle, and topology helpers
using ceil-sqrt in one place and truncating sqrt in two others.

DATA IMPACT: device timing and array energy both move (D024, B001, D003,
D002), so the corpus must be re-simulated on >= 1.11.52. Gate 1162F.

## 1.11.51 -- the FIX-PRE-FLEET queue closes; the N-notes and E-residues with it

The final FIX-PRE-FLEET batch (16 items), the open N-notes that were
mechanical, and the two E-residues. The ledger's fix queue is now EMPTY;
what remains open is N2/N3 (pending a user ruling), N6 (root cause), and
the N8 sourcing residue.

**Batch I -- the last 16 ledger items.**
- L70: SRAM/NVM access latency was characterized at a HARDCODED 22 nm while
  energy/area used the configured node -- one array, two processes. Both
  legs fixed: the flat helper takes a required, validated node from every
  caller, and the plugin tier models (which carried their own divergent
  compiled-in defaults: SRAM/STT 22, ReRAM 32, PCM 90 nm, with dead config
  knobs nothing ever set) take it via setTechNodeNm().
- L75: the tech-node positive-list ran only inside power analysis, so
  --no-power accepted an invalid node. Validated unconditionally at config
  load, for the global node, the host node, and every system node.
- L76: generic "DRAM" is now an ALIAS resolved at the single
  canonicalization point (-> DDR4, the preset that times it). It used to
  hit three different truths: Ramulator priced it DDR4, one host table
  classed it DDR5, and a device-scope validator rejected it outright.
- L87: the JEDEC die-area anchor arithmetic moved INTO the tool
  (vendorAnchorAreaMM2). One caller copy divided MBIT by a MB/mm^2 density
  -- an 8x error -- and still carried the phantom x1.12 E29 removed.
- L88/L247 reconciled: the 1.11.14 scope-gate rationale claimed to guard
  McPAT's cache queries; McPAT never constructs the wrapper. The gate is
  real -- for PIMID's own non-DRAM queries -- and now says so.
- L105 (with L68): ONE factor authority. The wrapper's XML emission derived
  the family factors at (table,table,hp,350K) while main printed them at
  the configured node/corner/temperature -- the APPLIED factor disagreed
  with the printed one whenever any input was non-default. Every call now
  names all four inputs. L68's "free logic-node knob" is thereby consistent
  end-to-end -- and the N2 MEASUREMENT below shows it is still live.
- L142: the NVM retention-free gating floor (2%, invented) is replaced by
  CACTI's own sleep-transistor state (Vcc_min/Vdd = 0.35), the same
  authority the SRAM path cites, stated as the tool-backed bound.
- L207: OOO retire-site attribution -- the measured mix, the PG activity
  mark and the FP-emulation charge read the NEXT basic block while the
  cycle cost came from the RETIRED one. All three now read the retired
  block.
- L208/L209 reconciled: both already fixed (1.11.48 measured-mix wiring;
  1.11.16 base-class syntheticInstrs).
- L214: floating_point=false now removes the FPU from the POWER model on
  every profile (OOO priced 2 FPUs it did not have); explicit overrides
  still win.
- L215/L223: the FP-emulation ENERGY fold -- on an FPU-less element each FP
  op becomes fp_emul_cycles integer-op equivalents through the integer-ALU
  stat, so soft-float work rides the same datapath factors real integer
  work rides instead of costing nothing.
- L243: 1.11.9's claim that device scope already counted the memory die was
  FALSE; device scope now prints the same populated "With memory:" total,
  making the two scopes comparable.
- L255 reconciled: the cCycles ROI rebase is correct; its 1.11.9 note
  understated the blast radius (every core, every scope). Recorded.
- L262: the coupled/decoupled regression arm now asserts the quantities the
  diff can move (the decoupled banner + per-technology memory pricing),
  not node McPAT power.

**N-notes closed.**
- N1 (user-raised at E7): PITCH IS GEOMETRY, NOT FAMILY. The SUBARRAY pitch
  penalty applies beside SRAM/NVM arrays too -- gating hoisted out of the
  DRAM-periphery branch; family-0 dies take it on the same measured
  on-pitch logic share (L116 boundary).
- N2 (measured, then RULED 2026-08-17): the logic-node cancellation the
  1.11.21 design intended is PARTIAL ONLY -- same HBM3/BANK cell at 22 vs
  45 nm: core area 2.0x apart, total power 8.8x. User ruling: settings must
  be MEANINGFUL -- a PE on the DRAM die takes the DIE's generation (class
  ladder -> its CACTI table, announced when it overrides the knob); a PE
  off the die (buffer chip, MC die, HBM base die, host) keeps
  technology.node_nm as the real logic-process choice. Implemented at both
  family-1 power sites AND at config load for device scope, so TIMING
  queries (cache-latency probes, array latency) see the same node as the
  power model -- one node per die, everywhere. Corpus cells sit at 22 on 22-table techs: no numbers move
  there; 32-table classes (DDR3-era) now price at their own generation.
- N3 (RULED: "go with the banded form"): the SUBARRAY pitch factor gains a
  DERIVED, BANDED default from published silicon when the user sets no
  hypothesis: [1.25 .. ~4] -- low end the FIMDRAM measurement (ISSCC 2021
  25.4, lean SIMD, our default), upper end UPMEM-implied (HC31 ~10x density
  claim / family factor; general-purpose CPU incl. 3-metal routing loss) --
  printed with the generation's own array pitch 2F from cell area = 6F^2
  (1x 38 / 1y 35 / 1z 31 / 1a 28 / 1b 25 nm). power.subarray_pitch_factor
  remains the explicit override; non-DRAM arrays get no invented default.
- N6 ROOT-CAUSED (probe gate N6, one job): the detailed cell's job-scoped
  three-valued divergence is GUEST ENVIRONMENT LAYOUT, not a simulator
  race -- two identical runs in one job are bit-identical; padding the
  environment by 256 B produced a third value (270245 -> 269540 cycles,
  25467 -> 25380 packets) and 1 KiB a fourth: each slurm job's env block
  (SLURM_JOB_ID at minimum) shifts guest stack addresses and a handful of
  cache-set mappings. The within-job A/B gate method stays mandatory;
  cross-job equality constants stay invalid.
- N5: ComponentType::FULL_SYSTEM deleted -- declared, never populated, zero
  consumers, an invitation to read zeros.
- N7 was closed earlier (measured flush footprint); marked.
- N9: HBM2/HBM3 nRFCSB was never filled, so EVERY instantiation of the
  Ramulator HBM models died in the completeness check (the HOST_MC
  SIGABRT). Filled with tRFC as a stated upper bound (no public per-density
  tRFCsb; unused under AllBank refresh); HBM3's nREFISB also read the wrong
  table (upstream copy-paste -- its own tREFISB_TABLE sat unused).
- N10: a declared link can no longer be silently ignored: src/dst-less
  entries apply by inference in a system with a device; every other skip
  warns and names what was kept.

**E-residues closed.**
- E17: the measured devMC gap histogram leaves the log and enters the model.
  The plugin exports the raw log2 buckets as stats; the orchestrator applies
  the SOURCED per-generation tXP (DDR3/4 6 ns, DDR5/LPDDR5 7.5 ns, JESD79/
  209; settable via memory.power_down_threshold_ns; GDDR6/HBM refuse -- no
  sourced tXP) with each qualifying gap paying the threshold once. The
  10k-cycle phase residency -- which measured r_idle=0 while ~90% of ROI
  cycles sat in usable gaps -- is superseded where the histogram exists.
  NoC gating stays measurement-only: no sourced wake penalty, none invented.
- E31 residue: the System Total memory term prints its wall clock, so the
  term is reconstructable from the report alone.

The one 1161I2 red arm (J1, tier latency at 22 vs 45 nm printing equal)
was an extractor-precision artifact: the tool's own cached
characterizations differ (read latency 3.298e-9 s @22 vs 3.786e-9 s @45,
~/.cache/pimid/nvsim/nvm_t0_c65536_n{22,45}_w512.xml) -- the node is live
end-to-end; the 1-decimal log print and cycle quantization hid it.

Gate 1161I + 1161I2 + N6 probe; predictions in the gate headers.

## 1.11.50 -- the family transform stops at the die edge

FIX-PRE-FLEET Batch H: the DRAM-periphery family transform's scope inside the
McPAT fork (L74/L80/L103/L104/L112/L115/L116). The 1.11.12 transform priced
everything it touched with the same three device factors; this release makes
each factor stop where its physics stops.

- L74: the family blanketed EVERY NoC level in a multi-level run, including
  the rank/channel/system fabrics the placement matrix itself declares to be
  logic (buffer die or host board). Each XML NoC instance now carries an
  on_dram_die flag from that same matrix (subarray..chip on die; channel
  on die only for channel-centric parts; rank and system always off-die),
  and processor.cc transforms only the flagged levels. The aggregate is
  rebuilt from the mixed-family parts with the constructor's own pppm
  arithmetic, so total and breakdown cannot disagree.
- L103: within an on-die level, the dynamic factor is device physics (CACTI
  comm-dram vs hp columns, gate-capacitance dominated) and cannot price the
  WIRE share of fabric dynamic -- metal capacitance does not follow the
  device. The link/bus dynamic share is restored to its metal price after
  the transform; leakage stays transformed for both shares (link drivers
  are periphery devices). A bus-type subarray/bank level is all link, so
  its dynamic is untouched -- the pass-through-wire fabric the 1.10.5
  census flagged.
- L104: the MC PHY, when built, is an empirical fit (die-photo area curve,
  mW/Gb/s dynamic; interposer tier from O'Connor MICRO-50 2017) -- a
  device-column ratio cannot transform a measured curve. Its dynamic and
  area are restored after the transform; its leakage (gate counts x Isub)
  stays transformed. Inert at the D2/D3 on-die tiers, where withPHY=0.
- L116: the transistor-pitch penalty applied to the WHOLE core, including
  the SRAM arrays whose cell area CACTI's tables declare device-independent.
  The on-pitch logic share is now measured from McPAT's own breakdown (exu
  minus its register-file and scheduler arrays, plus pipeline and undiff
  core) and the penalty applies to that share only, with the boundary
  stated (IFU decode logic stays classed with its arrays).
- L80 reconciled: the co-sim link's device end stays untransformed for a
  stated reason now covering every family class -- HBM reaches the host
  through its logic base die, DDR controllers are host-side, and a
  channel-centric LPDDR/GDDR module reaches the link through an interface
  chip; the SerDes endpoint is logic silicon in all three.
- L112 reconciled: the dead `sc & 2` cache arm was already removed with the
  scope mask in 1.11.34 (E10); with Private_L2=1 the shared-l2 Component is
  zero and its transform a no-op.
- L115 reconciled: the rebuilt Processor totals are consciously unread --
  1.11.29 verified nothing consumes them and completed them anyway; marked.

Gate 1160H (old = released 1.11.49, new = this, same job): see gate header
for the eight stated predictions.

## 1.11.49 -- readers read the last dump; the corner reaches what it claims

FIX-PRE-FLEET batches F (L220/L248/L249/L250) and G (L59/L69/L77/L119).

**Batch F -- stats readers and estimates.**
- L248/L249: the 1.11.9 last-dump-wins rule finally reaches the two readers it
  missed. The MPI rank summary took the FIRST mpi_ranks cycle lines -- the
  first dump's stale mid-run values whenever a periodic dump preceded the
  final one; the OMP critical-path summary accumulated EVERY dump, inflating
  Total and the PE count by the dump count (max survived; mean and Total
  lied). Both now segment on the "===" separators exactly as the main parser
  has since 1.11.9.
- L220: the IPC=1 cycle fallback estimated a DURATION from the all-core
  instruction SUM -- inflated by the PE count since 1.11.9 made instrs a sum.
  Now instrs/cores, stated in the note it prints.
- L250 (under the E26 ruling): the unmeasured-host synthesis -- nine fixed
  divisors of the DEVICE instruction count posing as a host -- is REFUSED.
  A system-scope dump with no host counters is an invalid input.

**Batch G -- power.device_corner reaches what its documentation claims.**
- L69: L2/L3 components hardcoded device_type=0 (hp) in the same XML whose
  system level carried the user's corner. The caches now follow the corner.
- L77: NVSim's device roadmap was hardwired HP, so the corner silently did
  nothing for every NVM technology -- the one family where it is genuinely
  selectable (real ITRS HP/LSTP/LOP columns; no DRAM-periphery refusal
  applies). Wired via NVMConfig.device_corner.
- L119: the HOST never received the corner -- the exact case 1.11.13 says
  the knob exists for. TWO independent gaps, both found by gate: (a) the
  dual-McPAT device-scope path never mapped it onto host_cfg; (b) in
  system-scope co-sim the per-node corner block was DEVICE-only, so host
  nodes kept device_type=0 -- and even after mapping, the mcpat_overrides
  block clobbered it back to 0 via its literal-0 default (the "corner
  applied" line printed while the XML still carried 0). The override default
  is now the corner-derived value already on the config -- an explicit
  per-node device_type override still wins. The host is logic; the corner
  maps directly, no refusal.
- L59: CACTI's periphery/tag ITRS flavors were hardwired ITRS-HP for every
  query; they now follow the corner where config is in scope (the SRAM
  main-memory query). The data-array CELL type is a different axis and is
  untouched. The no-config helper queries keep HP -- that residue is L70's
  hardcoded-node finding, still open and tracked.
- L106 reconciled: the "divergent system-scope copy" was already fixed by
  1.11.17 (the per-node block runs the corner map, refusal, and guard);
  the TRIAGE tag was stale.

Defaults preserve prior emissions exactly (corner=hp emits the same 0s), so
nothing moves unless the knob is used -- gate-asserted bit-stability.

## 1.11.48 -- the census tells integer SIMD from floating point (E batch)

FIX-PRE-FLEET L201 and L210: classification honesty in the decoder census.
Timing classes are untouched -- these are census corrections only, and the
gate's conservation arm proves it.

- L201: C_VECALU/C_VECMOV (and packed-integer C_FMUL) collapsed into the FP
  bucket, so paddb/pxor-class INTEGER SIMD was priced as floating point and
  charged soft-float emulation on FPU-less elements. A vecInt marker at the
  four packed-integer decoder sites routes them to the int/mul buckets the
  work belongs to; genuinely-FP vector ops stay FP.
- L210: x87 (D8-DF) fell through to C_GENERIC and the census's INTEGER
  default -- real floating point priced as integer and ESCAPING the
  soft-float charge, the mirror image of L201. Classed FP (still approx: no
  per-op x87 uop model exists and none is invented).

Gate 1159F: on the reference workload exactly 54 instructions redistribute
fp -> int with the total classified CONSERVED (462,267 both sides -- the arm
that catches a classifier that loses work rather than reclassifying it);
timing bit-identical on the alu cell; the FPU-less OOO cell's cycles drop
10,669,864 -> 10,667,638, matching 54 x 40 emulation cycles (2,160) plus the
N6 jitter floor. (The gate's V2 arm printed empty from a one-argument call to
a two-argument extractor -- the value verified directly from the log.)

Ledger: the TRIAGE tags for all Batch A-E items are flipped to CLOSED in this
release.

## 1.11.47 -- waits fetch nothing, framing is traffic, the mix reaches the units

FIX-PRE-FLEET batches C and D (L176/L183/L190/L199/L200/L203).

**Batch C -- crossing energy.**
- L183: the MPI-barrier synthetic BBL still carried bytes = barrierLat*4,
  manufacturing phantom instruction-fetch traffic proportional to barrier
  latency -- 1.11.7 zeroed the other synthetic BBLs and missed the DOMINANT
  one in thread-MPI co-sim. A wait fetches nothing; bytes = 0.
- L176, both halves: an explicit power.pcie.enabled=false is no longer
  force-overridden by the network-links sync, AND the co-sim pricing block --
  which never consulted the flag at all, "the only path a co-simulation
  takes" -- is now gated on it. Gate 1159C caught the second half missing:
  the first fix alone left the link priced (0.015 W controller dynamic) with
  the flag correctly false. Disabled runs state it and keep the link's
  TIMING; only energy pricing is off.
- L190: protocol framing is traffic. The timing model charges
  pcie_header_bytes per transaction; the energy model priced payload only,
  and xingCount was measured, parsed and used for one log line. Framing bytes
  (xingCount x header_bytes) now join the priced traffic at the same pJ/bit,
  and the [xing] parenthetical carries the term so the sum still audits from
  the line itself (reference: h2d 64 + d2h 64 + flush + framing 60 = total).

**Batch D -- the measured mix drives the machine.**
- L199: ialu/fpu/mul_accesses -- the stats that actually drive McPAT's
  execution-unit power -- stayed at 70/10/5% fractions while the decoder
  census sat computed in the same function. Measured classes now reach them
  (census fp class lands in fpu_accesses EXACTLY); the unsourced fractions
  survive only as the warned no-decode fallback.
- L200: mixLd/mixSt were measured, parsed, stored, never used;
  load/store_instructions stayed 20%/10%. Measured now.
- L203: OOOCore never received the soft-float charge (an FPU-less OOO element
  silently kept hardware-float timing) -- it now carries the E23 per-core
  capability and charges at retire, as a serialising penalty (emulated FP is
  a library-call chain the OOO window cannot hide). null_core has NO timing
  model, so floating_point=false there is REFUSED at config time rather than
  accepted-and-ignored.

## 1.11.46 -- the memory system is populated silicon, and one part per technology

FIX-PRE-FLEET batches A and B (audit items L164/L168/L170/L181/L189/L234/L236/
L237/L238/L242/L244/L256). NUMBERS MOVE substantially; every co-sim and DRAM
figure downstream must be re-derived on this release or later.

**Batch A -- the silicon that exists is the silicon that is counted.**
- L237: System Total area adds the POPULATED memory system, not one die. The
  organisation is the one the model already declares: DDR-class chips/rank
  (x4->16, x8->8, x16->4) x ranks x channels; HBM at the JEDEC-minimum 2
  channels per core die (stated lower bound); point-to-point one die per
  channel. Reference co-sim (DDR4 x8): 1 die -> 8 dies, area 3.78 -> 27.03 mm^2
  (= 8 x 3.38 after E29's 1/1.12; the arithmetic cross-checks exactly).
- L238/L256: a CACTI failure or an unsolvable reconfiguration (>32 banks)
  falls back to the vendor density anchor / the preset-organisation figure
  WITH A PRINTED NOTE -- silently deleting the memory from the total is gone.
- L242: SRAM/NVM system memories are priced by the live tools (CACTI, NVSim
  pre-generated cache) instead of contributing zero silicon.
- L236: area is STRUCTURAL -- accumulated for every memory-bearing node
  whether or not the workload touched it; only energy is activity-gated.
- L234/L244: coupled-vs-decoupled is the DECLARED topology
  (device.is_default_mem), not memory-technology string equality whose
  .empty() guards were dead against the "DDR4" default.
- Found en route: the decoupled DEVICE-side memory power never reached System
  Total (E31 wired host-side and coupled only). It does now, on the same wall
  clock. Decoupled reference: memory term 0.95 W.

**Batch B -- the tables describe one part, on its own clock.**
- L164: DDR3's array term was a 1.35 V Micron DDR3L-1600 (the IDD provenance)
  while its termination was a 1.5 V SSTL-15 part. Both are now the SAME
  silicon: DDR3L, SSTL-135 (JESD79-3-1 keeps RZQ=240 and the T38/T41 tables
  at 1.35 V). DDR3 termination drops by (1.35/1.5)^2 = 0.81x.
- L170/L168: DDR3/LPDDR5/GDDR6 borrowed the DDR4-2400 arch struct and its
  TIMINGS fed their array-energy formulas. getTRAS/getTRP/getTBurst now carry
  the same per-tech precedence getTRCD already had: DDR3-1600K from JESD79-3D
  (normative, in hand), LPDDR5-6400 from the Micron datasheets in hand, GDDR6
  from the same 16 Gb/s vendor-spec class getTRCD cites (single point, flagged).
  tBurst is specification arithmetic (beats/rate), not a table.
- L189: the header's claim that the IDD table "mirrors the per-impl
  current_presets" was FALSE and is withdrawn with the measured divergence
  recorded (DDR4.cpp Default {60,50,55,145,145,IDD5B 362} vs the part-sourced
  {58,35,42,140,150,IDD5 155}; the upstream DDR5 preset is a byte-identical
  copy of DDR4's). This table is the authoritative intensive source; IDD5
  here is average-refresh, not upstream's IDD5B burst figure.
- L181: DEVICES PER ACCESS. The IDD columns are per-device currents, but a
  64 B access on a DDR-class 64-bit rank engages every chip in the rank --
  TN-41-01, this file's own formula source, multiplies by the device count
  and we did not. DDR array energies rise by chips/rank (x8 parts: 8x);
  HBM/LPDDR5/GDDR6 are per-channel and unchanged. This closes the basis
  mismatch with the whole-rank termination term the audit caught being
  summed with it.

**N6 refinement (gate 1159 probes):** the detailed cell's nondeterminism is
JOB-SCOPED and three-valued -- three jobs on one binary gave three distinct
power/cycle pairs while three runs INSIDE one job are bit-identical. Gates
comparing old-vs-new within one job remain exact; cross-job constants are
invalid equality references, demonstrated three ways.

**Triage reconciliation:** six FIX-PRE-FLEET items were already closed by the
E-work and are now credited (E28's two-query form, E13's self-calculating
guard, the driver-Ron term, 1.11.16's wrong-BBL fix, decode-for-every-core,
SRAM gating).

## 1.11.45 -- die area loses its phantom 12%; the topology and the global get rules (E27-E30)

**NUMBERS MOVE: every DRAM die area shrinks by exactly 1/1.12 = 10.7%.**
HBM3 reference: 112.00 -> 100.00 mm^2/die (gate 1158h, exact by construction).
This touches the CAL area figures.

E27 (user ruling: multi-memory is v2; the current picture is 1 host + 1
device memory). The 1.9.42 FATAL was deleted with no replacement, so a second
memory-bearing node of the same role was silently DROPPED from the energy
total (host_done/dev_done latches; measured counters exist only as a single
host/device pair, so it had nothing to be priced from anyway). Refused at
config time, naming both nodes, before any simulation launches. Gate 1158f:
the 1h+1d picture is untouched; a 2-device-memory config stops with the error
and never reaches QEMU.

E28. The system-scope die-area "calibration" computed k = jedec/raw and then
raw x k -- algebraically the JEDEC anchor, ALWAYS: the CACTI run was
decorative and a node's bank reconfiguration cancelled exactly. (The finding
blamed device scope; verification shows device scope has had the honest
two-query form since 1.11.14 -- it was SYSTEM scope carrying the cancelling
single-query.) System scope now uses the same two-query form: k from the
preset organisation, area from the node's effective banks.

E29 (user ruling). The x1.12 on the JEDEC anchor is GONE. Its provenance was
lost in the 1.11.14 migration, and it double-counted periphery: the vendor
density rows are measured from REAL DIES, so capacity/density is already a
full-die area. Multiplying a full-die anchor by a periphery allowance charged
it twice.

E31 (user ruling, the queue's last item). System Total AREA has included the
memory die since 1.11.9; System Total POWER excluded it -- two adjacent lines
describing different systems. reportSharedMemoryArrayEnergy() now RETURNS the
wattage it prints (energy terms divided by the SAME 1.9.10 wall clock every
node's McPAT power uses; background is already a rate), the three call sites
accumulate it, and the total prints "+ X memory" so the addition is auditable
from the line itself. Co-sim System Total Power RISES by the memory system's
power -- numbers-moving for every co-sim total. Riding along: the decoupled
and coupled memory calls still passed pg_mc as the DRAM power-down flag; they
now pass memory.power_down, completing the 1.11.41 split at its last three
sites. Reporting gap noted in the ledger: exact external reconstruction of
the memory term needs wall_seconds printed, which the report does not yet do
-- the gate audits the printed total against its own printed parts and bounds
the term from below by the background.

E30 (user ruling). CACTI's g_ip global was left dangling by the whole
ecosystem: cacti_interface/init_interface point it at the caller's object --
sometimes a STACK LOCAL (mcpat interconnect.cc:78) -- and nothing clears it;
CACTIWrapper's destructor deleted the pointee. Everything worked on the
unstated invariant that every reader re-inits before computing. Worst live
case was 1.11.40's own CactiIOWrapper, which saved and RESTORED BY VALUE
THROUGH g_ip -- a write into freed memory once any CACTIWrapper died. Fixed:
CACTIWrapper clears the global after its call and in its destructor when it
owns the pointee; CactiIOWrapper switches to POINTER-SWAP (points g_ip at its
own object, restores the previous pointer, dereferences nothing it does not
own); the ownership rule is stated at the boundary. Bit-neutral by
construction and asserted by gate 1158i.

## 1.11.44 -- the legacy in-order NODECODE path is deleted (user ruling)

1.11.43 made the legacy IPC=1 path honest; this release concludes that the
honest form of a dead path is absence. PIMID_INORDER_NODECODE existed as the
A/B control for the 1.4.x pipeline validation -- byte-identical against
pre-decode releases. That purpose ended when 1.4.x shipped: release-vs-release
builds are the baseline method now, no gate in the current train used the env,
and the path bred audit findings (E25: a year of structurally inert mix/FP
code that looked functional). One core, one timing model.

Removed: InOrderCore::decodeMode and its three branches (legacy bblAndRecord
body, immediate load/store), the plugin's g_inorder_decode_disabled and its
term in the branch-feed condition, and the env read. Decode is unconditional
for in-order; only OOO keeps its kill-switch (decoder-fault triage -- an OOO
core cannot execute without uops -- not a timing mode). docs/cores.md records
the removal.

**Also in this release -- E26 (user ruling: "no measurements should never
happen").** When a co-sim node had no measured per-node counters, its power
was estimated at total_instrs x core_frac -- a uniform-work split, exactly
false for a host driving a device. 1.11.9's instrs-summing had silently
rebased that estimate by ~Ncores while its comment claimed it "preserves prior
behaviour". Both uniform-work fallbacks now REFUSE with an error naming the
node and the missing input: the instruction split, and the parallel
total_cycles x core_frac time split (a run with no derivable wall clock is an
invalid input, not a scaling problem). The dead 26-line per-component
core_frac cache/MC fallback went with its trigger. Dead on every healthy run
-- both groups measure in every co-sim dump this train has produced -- so the
corpus is unaffected (gate 1158e: reference co-sim completes, no refusal
fires, host cycles 17082).

Gate 1158d, 4/4:
  J1 co-sim host 17080 cycles, within noise of the 17082 reference -- the
     default decoded path is untouched.
  J2 an in-order DEVICE cell (the pipeline under ~460k real instructions,
     which no gate had exercised since ALU cells took over) completes at
     941709 cycles with the full census (133909 int+mul / 262260 fp / 66098
     branch per core).
  J3 PIMID_INORDER_NODECODE=1 is a NO-OP: 941709 vs 941701 (the N6 noise
     floor). This is the arm separating "deleted" from "broken" -- the env
     used to select a different timing model entirely, so equality proves the
     path no longer exists.

## 1.11.43 -- the FPU flag is a property of the core group (E23, E24)

peHasFpu/fpEmulCycles were ONE GLOBAL (zinfo->hierarchy) consulted by all
three charging core models, so a co-sim host would take the device PE's
soft-float penalty on its own FP instructions -- pim.pe.fp_emulation_cycles is
a PE property that leaked simulation-wide. Verification sharpened the finding:
in SYSTEM scope the FPU keys were never parsed at all, so the feature was
unreachable in co-sim and the leak sat armed for whoever added the parse.

- Per-group capability: hasFpu/fpEmulCycles are per-core-group config keys,
  stored on each core at construction (setFpuCapability) and charged from
  members. Host groups always emit hasFpu=true (RULING DEFAULT, revisitable:
  a host-side FPU-less machine is not currently expressible). The old globals
  survive only as defaults, so previously emitted configs keep their meaning.
- Per-node reachability: system-scope device nodes now parse
  pim.pe.floating_point / fp_emulation_cycles (like pg), and the emission
  reads the node's values.
- E24 closes as a verified side effect: emitZSimHierarchyBlock returns early
  when the hierarchy is disabled, taking the old global keys with it -- but
  the per-group keys ride the CORES block, which every scope emits
  unconditionally. The knob can no longer silently disappear.

**Host FPU configurable (user ruling), and E25: the legacy in-order path is
honest WITH decode.** The host's FPU is now a host-node key (floating_point /
fp_emulation_cycles at host level), symmetric with the device; an FPU-less
host charges its own emulation cycles. Verified in the emitted config
(host_cores: hasFpu=false, fpEmulCycles=30 -- gate 1158c H4).

E25 verified live and fixed by DECOUPLING: PIMID_INORDER_NODECODE used to
select the legacy IPC=1 timing AND disable the plugin's decoder, so the legacy
path's mixAdd() and the 1.11.11 soft-float charge only ever saw zero class
counts -- deliberately added code that never once executed with nonzero input.
The env now selects the TIMING PATH only; classification stays on. Under the
legacy env the device census now covers 6700/6701 instructions per core
(99.99%, accepted via the <5%-deficit branch). With fp_emulation_cycles = 0
the legacy timing is unchanged by construction: mix counters do not feed
timing. Gate arms note: the host retires ~1 real instruction in this ROI
(glue only, bounded by the gate), so host-side mix claims rest on the device
census plus the per-node plumbing.

Gate 1158, 3/3: reference co-sim inert (host 17082, within N6 noise of
1157b's 17076); an FPU-less device slows 726402 -> 811537 cycles (~68k/core
predicted from the 1707 fp/core census x 40 emul cycles, plus contention);
host bit-identical (17082 = 17082). STATED LIMIT: the host retires zero FP
instructions in this ROI, so the host arm proves no regression but cannot by
itself distinguish global from per-group -- that claim rests on the plumbing
plus the device arm proving the group flag is live.

## 1.11.42 -- the link controller clock is derived (E21)

The link controller clock was a two-valued literal: gen3 got 500 MHz,
EVERYTHING else -- gen4, gen5, CXL, NVLink, UCIe, UALink -- got 1000. The clock
cancels out of the SerDes and byte-driven terms (energy-per-cycle in, watts
out), but it linearly scales the controller digital dynamic that 1.11.41's E20
completion made live, so the literal scaled real reported power for every
non-gen3 link.

It is now derived from specification primitives:
    clock_MHz = lane_rate_GT/s * 1000 / PIPE_datapath_width
The PIPE width is an implementation choice the PIPE spec bounds (8/16/32-bit
per lane; Intel PIPE Architecture Spec rev 7.1); the 32-bit configuration is
the stated CONVENTION, not passed off as a per-generation constant. Under it:
    gen3       250 MHz   (the literal's 500 was 2x HIGH -- the audit's
                          assumption that gen3 was the correct case was wrong)
    gen4       500 MHz   (literal 2x high)
    gen5/CXL  1000 MHz   (literal coincidentally right -- and this is the
                          whole corpus, so no existing number moves)
Non-PIPE families (NVLink, UALink, UCIe) have no sourced controller clock:
they WARN and carry 1000 labelled ASSUMED instead of a silent default.

Gate 1157d. gen5 controller dynamic unchanged (0.015 W); a gen4 cell gives
0.008 W where the literal would have produced the same 0.015 -- the arm that
proves the derivation is live rather than silently dead.

**Found by that gate's first run, logged as N10:** the declared
system.network.links type is SILENTLY IGNORED when the entry omits src/dst --
the host_dev test skips it without warning and the pcie_gen5 default rides
along. The reference config has been in that state throughout; it matched the
default, so nothing ever looked wrong. Defeats D8's "disagreement is impossible
by construction" for exactly those configs. Not fixed here; the gate cell now
declares src/dst.

**E22 closed as fixed-by-1.11.15/16, verified against current code AND the
user's criterion that both host and device mixes rest on real instruction
traces.** Verified per node: the host mix comes from its own decoded stream
(hgrp.mix_*; 10 real instructions in the co-sim ROI -- the true trace of a
host blocked on the offload, scaling an equally tiny term) and the device from
its own (3830 int+mul, 1707 fp, 1147 branch per core x 8 = 53,472 of 53,479
retired; 462,267 classified on device-scope cells). Across 75 recent run logs
the fallback fractions are never in force. Two comment corrections ride along:
the "not yet understood" base mismatch is marked historical (it does not
reproduce -- co-sim measures instrs=10 vs uops=11, same base), and the
"documented" 87.5/12.5 stand-in is relabelled UNSOURCED -- its only
documentation was PIMID citing itself, and changelog 1.11.10 had already
measured the assumption wrong.

## 1.11.41 -- CACTI-IO covers five of seven memory technologies (N8)

**Also in this release -- pim.mc.pg is split into three flags (user ruling).**
One flag drove three unrelated mechanisms; a user could not enable the one they
meant and the reported saving mixed them:
    pim.mc.pg          controller LOGIC gating (Vdd cut, state lost)
    memory.power_down  DRAM JEDEC precharge power-down (IDD2P). NOT gating:
                       the array is never unpowered -- cells are capacitors and
                       must refresh -- so "power gating DRAM" is a category
                       error the old flag name encoded.
    memory.array_pg    array-PERIPHERY gating. Retention-free on NVM
                       (non-volatile cells hold state unpowered); on SRAM the
                       periphery gates and the cell array goes to RETENTION.

**SRAM gains a retention path, from CACTI's own figures.** CACTI defines
Vcc_min for a memory cell as "the lowest vcc for data retention" and derives it
per node: sram_cell.Vcc_min = 0.65*Vdd, peri_global.Vcc_min = 0.35*Vdd
(parameter.cc). Its power-gating model drops a block to Vcc_min, not to zero
(decoder.cc, detalV = Vdd - Vcc_min) -- so the tool's "gated" state IS a
retention state, and the model PIMID lacked was already inside the tool.
Previously the SRAM cell array was held at full Vdd during idle (a stated lower
bound); it now sits at its retention voltage, keeping data:
    leakage = cells*((1-r) + 0.65*r) + periphery*((1-r) + 0.35*r)
Using the VOLTAGE ratio as the LEAKAGE ratio is the conservative direction --
subthreshold leakage falls faster than linearly in Vdd (DIBL), so the credited
saving is an under-estimate. Both ratios are CACTI per-node values, not ours.

**E20 (user ruling: add, do not replace) -- and its completion.** The
byte-driven link energy ASSIGNED over McPAT's PCIe controller runtime dynamic,
justified as "end-to-end pJ/bit". The sourced figures are SerDes papers -- PHY
measurements -- so the controller's digital protocol engine (LTSSM, replay,
flit assembly) is different silicon, and the assignment became an addition.
Gating exposed that the addition alone was INERT: perc_load was forced to 0,
which had already zeroed the controller dynamic before the byte term arrived.
perc_load now carries the MEASURED duty (E19), so controller digital work is
charged at real activity at BOTH ends (each end's logic processes every flit;
the SerDes energy is still charged once, end-to-end). Measured on the reference
co-sim cell: host controller dynamic 0.011 -> 0.015 W, device end 0 -> 0.0032 W
-- about 7 mW of digital link power that was previously deleted.

**Found while gating, logged not fixed:** HBM3 at HOST_MC placement fails in
Ramulator preset generation ("timing nRFCSB is not specified"), on old and new
binaries alike -- a pre-existing config-path gap, now audit N9. The HBM3
no-change claim is instead proven by the BANK-placement cell (gate 1156,
bit-identical).

1.11.40 harnessed CACTI-IO but could only SUBSTITUTE for DDR3 and DDR4, because
every other technology had to borrow a neighbour's electrical parameters and a
borrowed rail voltage is not a model. PIMID already held the missing values.

**PIMID's sourced electricals are injected into CACTI-IO.** VDDQ, RTT and RON
per technology, cited in pimid_energy.h to JESD79-3D T38/T41 (DDR3 SSTL-15,
RZQ=240 so RTT=RZQ/6 and RON=RZQ/7), JESD8-24 POD12 (DDR4), POD11 (DDR5),
JESD8-21C POD135 (GDDR6, RTT programmable via MR6) and the Micron LPDDR5
datasheets (LVSTL, VDDQ 0.5 V ODT-on, RON 40). `IOTechParam::recomputeSwing()`
was factored out of the constructor so the termination network and every swing
re-derive from the injected values. LPDDR5's 0.5 V rail against LPDDR2's enters
each swing as V^2, which was the single largest error in the borrowed setup.

    tech     termination nJ/64B      driver+PHY nJ/64B   IO area mm2
             hand -> CACTI-IO        (never modelled)
    DDR3     2.43243 -> 12.57894     19.42374            3.1360
    DDR4     1.30909 ->  2.13733      9.70775            2.6979
    DDR5     0.73333 ->  1.20052      7.79051            3.4289
    LPDDR5   0.03571 ->  0.07380      3.58644            withheld
    GDDR6    0.33326 ->  0.67124     15.25356            withheld
    HBM2/3   cross-check only -- unterminated interposer, no network to inject

**This fixes the 142x LPDDR5 defect 1.11.40 recorded but could not repair.**
The hand table gave 0.0349 pJ/bit because it modelled only static termination,
which is exactly what LVSTL exists to eliminate. With the electricals injected
LPDDR5 lands near 7.1 pJ/bit total, against a published 3-6 band.

**Area has a narrower valid range than power, and no longer shares its limit.**
The area polynomial's cubic term equals its linear term at sqrt(k1/k3) = 3162
MHz and is 4.9x it by 7000 MHz, which produced an implausible 10.81 mm2 for a
32-bit GDDR6 interface. Above the crossover the area is withheld and says so;
power is unaffected because nothing in the power path reads that polynomial.

**Still borrowed, and stated at every call:** capacitances, bias and leakage
currents, PHY coefficients and the area polynomial remain the neighbouring
family's values. PIMID has no sourced replacements, and inventing them is the
defect this work exists to remove. An injected technology is better-grounded
than a borrowed one, not fully sourced. LPDDR5's RTT=240 is flagged UNSOURCED
in the result, the same way pimid_energy.h flags it -- Micron defers the ohm
table to an AC/DC document we do not hold.

## 1.11.40 -- quantities the simulation reveals stop being constants

One principle, five changes. A value the run can produce must not be a
configured constant, and a physical rate is a BAND, not a number.

**E18 -- the coherence flush crosses the link.** Link energy was priced on 128
bytes (a doorbell and its ack) while `xingFlushBytes` recorded 16,777,216 B
moving at the same boundary. 1.11.15 had removed the flush from the link
because "it rides the memory channel, not the PCIe/CXL PHY" -- true, and it
never said WHERE that memory channel is. In a COUPLED system the shared array
IS the device's DRAM, on the far side of the link, so the bytes traverse the
PHY and land in DRAM: two pieces of silicon, two energies. The flush is now
added to the link byte count in the coupled branch only; the decoupled branch,
where the host flushes to its own memory, is untouched and was already right.
Gate 1152c, 12/12.

**E19 -- the link duty cycle is derived.** `power.pcie.duty_cycle` defaulted to
0.01 with no citation and was wrong in both regimes, in opposite directions:
against the reference co-sim run the true duty is 2.31e-06 without the flush
(0.01 was 4329x HIGH) and 0.303 with it (0.01 was 30x LOW). No constant can be
right across that. It is now `(xbytes / link_bytes_per_s) / wall_seconds`,
measured per run. The YAML key survives only as an explicit override and prints
what the measurement would have been. Gate 1153, 8/8; the derived 0.3 matches an
independent hand calculation of 0.3028.

**N7 -- the coherence footprint is measured.** `coherence_footprint_bytes`
defaulted to a fixed 16 MiB regardless of workload. Measured: the host caches
hold **54,464** dirty bytes -- 308x smaller, and within the 294,912 B the cache
hierarchy can physically hold, which the 16 MiB constant exceeded by 56.9x. It
was not an over-estimate but an impossibility: a flush writes back dirty lines
FROM cache and cannot move more than the cache holds. The flush had been
consuming 1,748,027 of 1,758,987 host cycles -- **99.0% of the host timeline** --
on a 4096-element kernel. Now 6,074 cycles. The footprint is re-measured at
every flush (the dirty set after offload #1 is not the dirty set after #2) and a
configured value is REFUSED rather than silently honoured. Gate 1154.

**Link energy becomes a band.** Each `pJ/bit` was a scalar. They are now
published ranges with provenance, and the report carries the band and the
resulting energy range rather than a midpoint passing for a measurement:
    pcie_gen5   7.6-11.4   Samsung 8LPP 7.6 (excl clocking) .. 10nm CMOS 11.4
                           (incl PLL+clocking) -- the retired 7.0 was BELOW the
                           published minimum
    pcie_gen4   1.93-6.0   1.93 is TRANSMITTER-ONLY, a bound no full link can
                           reach; 6.0 is a full SerDes. A boundary, not a spread
    pcie_gen3   4.0        retires an unsourced 5.0
    nvlink      1.17-1.30  measured PHY (JSSC 2019) .. NVIDIA product figure
    interposer  0.25-0.5   UCIe advanced package -- this range was ALREADY
                           sourced in the comment and then thrown away by
                           returning 0.5
    ualink      3.5        200G short-reach incl SerDes+DSP; retires an assumed
                           8.0 that was 2.3x higher
Entries with one figure are labelled SINGLE POINT, because that is a fact about
our sourcing rather than about the hardware.

**N8 -- CACTI-IO is harnessed for the DRAM interface.** `external/cacti/extio.cc`
is a full off-chip IO model built from extracted parameters (swings,
capacitances, bias/leak currents, termination tables, an area polynomial). It
has been vendored, compiled into libcacti7, and called by NOTHING.
  - Its DRAM paths work: DDR4-3200 x64 gives IO area 2.99 mm2, termination
    262 mW, dynamic 443 mW, PHY 390 mW.
  - Its Serial path had never been executed by anyone and is unusable: the
    branch left rtt/rs/r_on/t_flight at zero so it faulted with SIGFPE on
    construction; `num_mem_clk` divides by `num_clk/2` and a serial link
    legitimately has zero clock pins; and the area polynomial is a FIT over DDR
    frequencies whose cubic term grows 55000x from 800 MHz to 32 GHz, giving
    61 mm2 of IO area for 16 lanes. We repair the construction faults and REFUSE
    above 8000 MHz rather than extrapolating. PCIe/CXL therefore stays on the
    published bands above.
  - DQ termination now comes from the model for DDR3 and DDR4 -- the EXACT
    technology maps -- replacing a hand-written SSTL/POD/LVSTL scheme table.
    DDR3 2.43 -> 9.26 nJ/64B, DDR4 1.31 -> 2.19. Driver-switching and PHY energy
    (20.8 and 10.2 nJ/64B) and IO area are newly modelled; the interface had
    been termination-only.
  - DDR5, LPDDR5 and HBM map only APPROXIMATELY (onto DDR4, LPDDR2 and WideIO),
    so the model is reported as a cross-check and NOT substituted: borrowed
    parameter sets do not survive a sanity check (LPDDR5 lands near 34 pJ/bit
    against a published 3-6). GDDR6 has no parameter set and is refused
    outright. Replacing one unsourced number with another and calling it a
    model is the defect this release exists to remove.
  - The hand table's LPDDR5 entry is separately wrong by ~142x: it models only
    static termination, which is exactly what LVSTL is designed to eliminate, so
    it captured the one negligible term and omitted everything real. That defect
    is live in every LPDDR5 cell and is NOT fixed here -- LPDDR5 has no exact
    map -- it is recorded as audit N8.

**Gate 1156 / 1156b.** The scope claim is the defence of this release, so it is
what the gate tests. HBM3 is bit-identical old vs new (0.0499777 W, 10164680
cycles) -- the approximate map does not substitute, which is the arm that would
have caught an exact-map leak. Co-sim host cycles fall 1,758,995 -> 17,062
(99.0%), footprint measures 54,464 B within the 294,912 B cache capacity, link
bytes are 128 + the measured footprint, and the band prints. Device cycles are
unchanged on every cell.

TWO ARMS FAILED ON WRONG PREDICTIONS OF MINE, not on defects, and both are
worth recording because each encoded an assumption about a cell rather than a
claim about the change:
  - I first tested DDR4 at BANK placement, which reports "on-die placement: no
    DQ crossing, no termination". The PE is INSIDE the DRAM there, so nothing
    crosses the DQ pins and termination is correctly zero -- the arm could
    never have moved. Re-run at HOST_MC placement, where accesses do cross DQ,
    termination goes 1.309 -> 2.185 nJ/access, matching the offline calculation
    (1.30909 -> 2.18526) exactly.
  - I then predicted total power would visibly rise. It does not: that cell
    reports "Total dynamic: 0.0 mJ (rd=0.0 + wr=0.0 + act=0.0 + term=0.0)" on
    BOTH arms. The per-access energy changed; there are no counted accesses to
    multiply it by. Verified at the per-access level instead.

**Data impact.** DDR3 and DDR4 interface energy changes, and only where
accesses actually cross the DQ pins -- on-die placements charge no termination
and are unaffected. Every other technology is untouched. Co-sim results change substantially: the host timeline loses 99%
of its cycles with the 16 MiB flush constant gone, and coupled-system link
energy gains the flush bytes.

## 1.11.38 -- the NoC is marked where the fabric is used (E16)

**Also in this release: the published tree has not configured since 1.11.26.**
`CMakeLists.txt` gained `add_executable(nvsim_warm tools/nvsim_warm.cpp)` in
1.11.26, but `tools/nvsim_warm.cpp` was only ever created in the development
tree -- it was never copied into the release tree and never added to git. Every
release from 1.11.26 to 1.11.37 therefore shipped, to BOTH repositories, a
source tree that fails at the CMake configure step:

    CMake Error at CMakeLists.txt:296 (add_executable):
      Cannot find source file: tools/nvsim_warm.cpp

Anyone cloning either repository since 1.11.26 could not build. The file is
added here. It was found by building the release tree before committing, which
is a required step that had been skipped -- the development tree building
proves nothing about what is being published, because the two trees only
contain the same files if every new file is copied across, and a file that
exists only in the dev tree is invisible to a diff of the files that DO exist
in both.

That same build then caught a second, self-inflicted version of the problem:
`pe_memory_interface.h` was mirrored from the dev tree carrying BOTH this
release's fix and the NEXT release's gap instrumentation, whose `GapHist`
members live in `zsim.h` and `zsim_types.h` -- not mirrored, because they are
not part of this release. Fifteen compile errors, all of the form "has no
member named nocGaps". The release tree now carries the E16-only header. A
release must be cut from the files that belong to IT, not from whatever the
development tree currently holds.


`pgres.noc` counts the phases the on-die fabric was busy; idle residency is
what the NoC is credited for power gating. The marker sat at the TOP of
`PEMemoryInterface::access()`, on the claim that every device access "injects
into the tree fabric".

- **That claim is true for one of the two NoC models.** Detailed Garnet forces
  `wantLocal` false and routes every access, so the blanket marker was right
  there. The ANALYTICAL model does not: an in-coverage access takes the local
  fast path, computes `localAccessLatency + localLinkLat_`, and returns without
  ever generating a packet. Those accesses were marking the fabric busy.

- **The same defect appeared a second time while fixing it.** The degenerate
  no-PE path (`dstEp < 0`) states in its own comment that it prices the access
  as plain Ramulator DRAM with no Garnet traversal -- so it must be marked like
  the local path, not like the routed one it sits inside.

- **The NoC is now marked at the four sites that generate fabric traffic**:
  local + `mcStandalone` (the MC hop is the only fabric a local access uses),
  degenerate + `mcStandalone`, the routed Garnet path, and the analytical
  remote path. `devMC` keeps its unconditional marker at the top of the
  function -- no path out of `access()` skips the memory controller, so that
  one was never wrong.

- **Measured impact (gate 1149b, 8 cells, 20 arms).** On both analytical cells
  the old code marked the NoC busy in 264 of 264 phases while `remoteAcc = 0`:
  every access was local and ZERO packets crossed the fabric. The new code
  marks 0. This is not a small correction to a residency -- it is the
  difference between "fully busy" and "completely idle" on a component that
  carried no traffic at all. The 1.11.39 gap histogram, built for an unrelated
  purpose, reports `[noc] no events` on the same cells: independent
  confirmation from a second instrument.

- **The marker still fires where it should**, which is the arm that separates a
  fix from a deletion: with `mc.placement = standalone` a local access does
  cross to the MC node, and the NoC is marked in 23462 phases -- identical
  before and after. Detailed-cell marking is likewise unchanged (1016 = 1016).
  Cycles and power are identical old vs new on all four cell types.

- **A gating-method defect found by this gate, unrelated to the fix.** Two arms
  compared the detailed cell against constants carried from gate 1147
  (0.0499777 W / 10164680 cyc) and failed -- from BOTH binaries identically.
  A dedicated 3x repeat job then reproduced those constants exactly. Across 8
  runs of the same binary, plugin, config and node, 6 give 10164680 cyc /
  1366290 accesses / 1366102 Garnet packets and 2 give 10164688 / 1366286 /
  1366101. The cell is NOT bit-reproducible: 8 cycles in 10.16M (0.00008%),
  one packet in 1.37M. Negligible for results, fatal for an equality arm.
  Cause is NOT diagnosed -- logged as audit item N6. From this release, gates
  test a change by comparing old vs new WITHIN ONE JOB (exact, and the arm that
  actually tests the change); comparisons against historical constants carry a
  tolerance. All 20 old-vs-new arms here passed exactly.

- **Note on 1-PE cells, found while gating.** Coverage is split among PEs
  (`orgsPerPe = totalUnits / numPEs`, init.cpp:1009), not fixed at the
  placement granularity, so a ONE-PE analytical cell owns the whole memory and
  is 100% local at every placement level. A gate arm predicting BANK placement
  would be remote-dense was wrong for this reason and has been corrected.

## 1.11.37 -- every DRAM state pays its own refresh (E15)

A DRAM unit's background power is split across three JEDEC states -- ACTIVE
STANDBY (IDD3N) while a row is open, PRECHARGE STANDBY (IDD2N) when an idle
controller has closed its pages, PRECHARGE POWER-DOWN (IDD2P) when CKE goes
low. Refresh continues in all three: the device must retain.

- **Refresh was charged against the wrong baseline in two of the three.**
  `refreshMW()` returns the refresh EXCESS over IDD3N -- correct only when
  added to an IDD3N baseline, where `vdd*idd3n + vdd*(idd5-idd3n)*duty`
  reconstructs the interleave. `backgroundUnitMW()` added that one term on top
  of all three states, including the idle fraction whose baseline is IDD2N or
  IDD2P. Since IDD2P < IDD3N, refresh was UNDER-charged during power-down by
  `vdd*(idd3n-idd2p)*duty`.

- **The fix is state-independent.** JEDEC IDD5 is the all-bank auto-refresh
  current measured with banks precharged -- an absolute figure, not an
  increment -- and a device must EXIT power-down to accept a REFRESH command.
  So for the tRFC/tREFI duty fraction the unit draws IDD5 whatever state it
  was holding, and its own state current for the remainder:
        P(state) = vdd * ( idd_state * (1 - duty) + idd5 * duty )
  Each state now pays that. The separate additive refresh term is gone.
  `refreshMW()` survives only as the reported line item, and its comment now
  says what it is relative to. A clamp keeps refresh from ever REDUCING a
  unit's power on a table row where IDD5 < the state current.

- **Data impact: none.** At r_idle = 0 the new expression reduces
  algebraically to the old one, and 1.11.20 measured r_idle = 0 across the
  corpus -- memory-bound kernels keep the controller busy every phase. Gate
  1148 asserts that bit-identity for all seven technologies, and separately
  measures the recovered under-charge at full idle: 0.68 mW per unit on HBM3
  (2.6% of its 26.4 mW per-unit background), 10.8 mW across a 16-channel
  stack; 4.67 mW on GDDR6, 1.21 on DDR3, 1.82 on DDR5. This is a correctness
  fix for future workloads with real idle, not a results change.

## 1.11.36 -- a phase is counted once (E14)

PhaseActivity counts, per component, how many PHASES it was busy in; idle
residency is 1 - activePhases/totalPhases, and that is the fraction of gating
saving the component is credited. lastActivePhase is the "have I already
counted this phase?" marker.

- **The marker could walk BACKWARDS.** touch() CAS'd it to whatever phase the
  calling thread held, so a thread carrying a stale number could rewind it and
  the next touch of an already-counted phase would count again:
        phase 5: A touches -> marker=5, count=1
                 B (holding a stale 4) touches -> marker=4
                 A touches again -> 4 != 5 -> count=2
  An inflated activePhases makes a component look busier than it was, so its
  idle residency comes out too LOW and it is credited LESS gating saving than
  it earned. The in-code comment claimed "a lost race undercounts one phase,
  negligible" -- the real failure was over-counting, by an amount depending on
  thread interleaving.

- **The update is now monotonic**: the marker never rewinds, and a lost CAS
  retries instead of falling through.

- **Scope, checked before changing it.** Cores hold their own tracker
  (core.h pgAct) and cannot race; only the five SHARED trackers are exposed --
  anyCore, sharedCache, noc, hostMC, devMC[] -- which are exactly the ones the
  reported residencies come from. And numPhases advances inside the
  scheduler's BARRIER callback, so threads agree on the phase for essentially
  all of it and the stale window is a few instructions. Real, but rare.

- **Trade-off, stated**: refusing to rewind drops a lagging thread's touch of
  an older phase. The barrier has left that phase, so another thread has almost
  certainly counted it; where it has not, the result is a bounded UNDER-count,
  which under-credits gating rather than over-crediting it.

- **What this does NOT buy: reproducibility.** Which thread wins the CAS still
  depends on interleaving, so the count is bounded and never inflated, not
  identical run to run. Per-thread trackers would give that, at the cost of
  deciding what "the phase was active" means when threads disagree -- logged
  rather than assumed.

TESTED BY A TEST THAT DISCRIMINATES. A simulation gate cannot exercise this:
our cells are 1-PE and nothing races. _1147gate/phaseact_test.cpp drives the
tracker directly, including the exact interleaving the bug needed, and the gate
builds BOTH versions:
    fixed  PASS  backwards touch does not double-count   (got 1, want 1)
    old    FAIL  backwards touch does not double-count   (got 3, want 1)
Concurrent stress -- 8 threads, half deliberately stale, 2000 phases -- counts
82, never exceeding the ceiling.

DATA IMPACT: none. Gate 1147 5/5 with the shipped cell bit-identical
(0.0499777 W, 10164680 cycles), because 1-PE cells never raced.

## 1.11.35 -- the frequency guard calculates its own bound (E13)

E13 observed that the area factor uses l_phy (device LENGTH) while CACTI's
I_on says the periphery device drives less current per micron of WIDTH -- two
different quantities, only one applied. Following it produced a better guard
and corrected three numbers along the way, two of them mine.

- **The bound is CALCULATED, not cited.** A DRAM-periphery PE is slower than
  a logic one by its CV/I delay ratio, computed from the same columns the area
  and dynamic factors already use:
        t = (C_g_ideal + C_fringe) * Vdd / I_on
        22 nm  hp 1.179e-13 s   comm-dram 2.491e-13 s   -> 2.113x
        32 nm                                           -> 1.291x
  ceiling = logic reference / that ratio. Published parts now CHECK the bound
  instead of setting it.

- **CORRECTION 1, mine: the delay penalty is 2.113x, not 2.885x.** An earlier
  draft of this guard used the I_on ratio alone. That overstates it by 37%,
  because the periphery device also has LOWER gate capacitance (1.99e-16
  against 3.27e-16 F/um), which partly offsets its weaker drive. Drive alone
  is not delay.

- **CORRECTION 2, inherited: "UPMEM DPU: 350-466 MHz" appears in no source we
  hold.** It was in the code comment and I repeated it. Devaux, Hot Chips 31
  (misc/) says "4Gb DDR4 2400 DRAM + 8 DPUs @500MHz" (p3, p4) and "14 pipe
  stages needed to reach 500 MHz" (p10). The demonstrated band is 300 MHz
  (FIMDRAM, ISSCC 2021 25.4) to 500 MHz (UPMEM).

- **CORRECTION 3, mine: the first calculated guard was CIRCULAR.** It anchored
  on reference_frequency_mhz, which is the max over ALL nodes -- in device
  scope that is the PE itself, so the ceiling was always the PE's own clock
  divided by 2.113 and every device-scope run warned. A 10% "engineering
  tolerance" added to quiet it was papering over the wrong anchor and has been
  removed; at the time I wrote that a tolerance chosen to silence our own
  configs would be the wrong reason, which is what it was.

- **The anchor is now a user SETTING**: power.logic_reference_mhz, the clock of
  the logic-process design this PE is compared against, validated at parse. In
  co-sim the host node's clock is used when the setting is absent. With no
  anchor at all the bound is NOT evaluated and the run says so, rather than
  inventing a ceiling or comparing the PE against itself.

- **PIMID never derates the clock.** It simulates at the requested frequency
  and reports "the clock is the hypothesis" -- the guard tells you when the
  hypothesis is not reachable, it does not silently change it.

- Also fixed: examples/cosim/host_device_basic.yaml set the DEVICE to 2000 MHz
  -- the host's clock copied down -- which is why that example tripped its own
  warning. Set to 500 MHz, with the demonstrated band cited in the file.

DATA IMPACT: none. Gate 1146 5/5, the shipped cell bit-identical (0.0499777 W,
10164680 cycles). This release changes what the model TELLS you, not what it
computes.

## 1.11.34 -- the link is not PCIe, and the pitch penalty is not the whole die

E10 and E11 ruled together, plus the link-model gaps the user asked to patch.
Everything here except one item is sourced from UCIe or from our own placement
ladder rather than asserted.

- **E11: fa and PITCH are emitted separately.** They were multiplied into one
  scalar, so wherever the area factor went the pitch penalty went too -- onto
  the caches and the memory controller. Those are OFF-PITCH circuits: per
  Vogelsang (MICRO 2010) the on-pitch circuitry is the sense-amp stripes and
  local wordline drivers, laid out on the array's bitline pitch, while a cache
  or MC sits further out and is limited by wiring. Only the PE core abuts the
  array. MEASURED: at pitch 2.0 the device area now goes 8.553 -> 9.573 mm^2,
  a ratio of 1.119. It was 2.0. The penalty was inflating device area by ~79%
  more than physics supports whenever it was set.

- **E10: the scope mask is gone.** Emitted as the literal 15 -- every bit,
  always -- with no configuration surface, so it had one reachable value and
  its bit structure implied a selectability that did not exist. The LINK
  CONTROLLER stays untransformed for a stated reason rather than a missing
  mask bit: our own ladder maps LOGIC_DIE (the HBM base die) to family 0, and
  an HBM device reaches its host THROUGH that base die; for DDR-class parts
  the controller is host-side. Either way it is logic silicon.

- **The link surface is power.link.***, because the model accepts ELEVEN
  classes across five families (pcie_gen3/4/5, cxl_2_0/3_0/mem,
  nvlink_3_0/4_0/c2c, ualink_1_0, interposer) and naming all of them "pcie"
  misdescribes every one that is not. power.pcie.* remains a deprecated alias,
  announced when used, so no existing config breaks.

- **Interposer, sourced from UCIe** (uciexpress.org; Hot Chips 2023 tutorial):
    - 0.25-0.5 pJ/bit for the ADVANCED package (interposer/bridge, <=2 mm
      channels) against 0.5-1 for the standard organic package. Our 0.5 sits
      at the conservative top of the advanced range -- a ballpark before, a
      cited bound now.
    - 64 data lanes per PHY module (advanced) against 16 (standard). An
      interposer was defaulting to a PCIe LANE count, which is a different
      quantity; it now takes its own module structure.
  AND A REVERSAL OF MY OWN PROPOSAL: I argued the interposer should stop
  paying for a PCIe protocol stack. UCIe says otherwise -- its die-to-die
  adapter explicitly carries PCIe flit mode and CXL flit mode, so the
  transaction and data-link logic is genuinely present and only the SerDes is
  not. Removing the stack would have under-counted real silicon. Kept.

- **The host carries one link controller PER ATTACHED DEVICE.** It was
  hardcoded to 1, so a multi-device system reported a single controller's area
  and leakage however many devices hung off it -- and multi_device.yaml is a
  shipped example.

STILL NOT MODELLED, stated rather than invented:
  - CXL coherence logic (user ruled it out of scope for this pass). CXL rides
    the PCIe gen5/6 PHY so its PHY pricing is right; the snoop/state silicon
    beyond it has no public area figure.
  - UCIe PHY AREA. No public mm^2 or gate count exists, so an interposer
    PHY's area is McPAT's PCIe controller minus its SerDes -- a bound, not a
    measurement.
  - PER-DEVICE link attribution. The crossing counters are global, so the
    dynamic term is charged once at the host on the total, and mixed link
    types across devices all take one pJ/bit. Needs per-device counters in the
    plugin; inventing an attribution would be worse than the known limit.

DATA IMPACT: none by default -- nothing in the corpus sets a pitch factor, the
mask was always 15, and the rename is alias-compatible. Gate 1145 holds the
shipped cell bit-identical (0.0499777 W, 10164680 cycles). Configs that DO set
a pitch factor re-price, correctly, by ~1.12x instead of 2x.

## 1.11.33 -- why runtime leakage differs, and why we do not use it

User question: leakage is static, so why does McPAT have a "runtime leakage"
at all, and how can it change? It cannot -- and tracing the 5.97x gap 1.11.32
found gives the answer.

ROOT CAUSE: the EXECUTION UNIT. Measured per core sub-unit on a BANK/HBM3
cell, instrumented and then removed:

    unit          peak W   runtime W    ratio
    ifu         0.001138    0.000818    1.39x
    lsu         0.001641    0.001641    1.00x
    exu         0.031331    0.001515   20.68x   <- 98.9% of the gap
    mmu         0.000417    0.000417    1.00x
    undiffCore  0.001668    0.001668    1.00x
    SUM         0.036196    0.006060    5.973x  == the measured ratio

lsu, mmu and undiffCore agree EXACTLY, which is what leakage must do. Only exu
collapses, because McPAT scales the execution unit's RUNTIME leakage with
utilisation: an unexercised ALU or FPU contributes almost none. That is not how
leakage works -- a powered idle unit leaks the same as a busy one, and gating
is tracked in separate fields (power_gated_leakage). The runtime figure is the
physically wrong one. Our cell is a 1-PE ALU profile, so most execution units
idle and the runtime number falls to a twentieth.

- **PIMID was already right.** extractComponent reads leakage from the PEAK
  basis, which counts every powered device. Switching to rt_power -- which N4
  was contemplating -- would silently drop ~86% of core static power.
- **The guard stays**, reworded from a discovery tool into a REGRESSION guard:
  it names the component, fires once, and states the upstream cause, so a
  future convergence or a new divergence is visible rather than silent.
- **A second, unrelated upstream bug found and fixed on the way.** McPAT's NoC
  scales runtime leakage by 1 while the peak path scales router leakage by
  total_nodes and link_bus leakage by global_linked_ports x total_nodes. Same
  class of error -- static power made to depend on structure only in one basis
  -- and also invisible, since PIMID reads peak. The runtime path now carries
  the same structural multipliers.

DATA IMPACT: none. Bit-identical to 1.11.31/32 (0.0499777 W, 10164680 cycles),
because every leakage number PIMID reports comes from the peak basis and always
has. Gate 1144: 4/4.

CORRECTIONS TO MY OWN CLAIMS, recorded: I asserted the two bases were equal by
construction (wrong -- refuted by the assertion I wrote to check it), then
attributed the gap to the NoC's missing total_nodes multiplier (wrong -- that
bug is real but unrelated; the assertion fires on "core"). The per-sub-unit
measurement is what settled it. N4 is closed by this entry.

## 1.11.32 -- each leakage basis is transformed on its own value (E9)

applyFam read plainLeak from c.power -- the PEAK component -- and wrote it into
the RUNTIME fields too, discarding whatever runtime leakage McPAT had computed.
The gated fields two lines below already read plainGated and plainGatedRt
separately, so the function disagreed with itself: the correct pattern was
present, applied to the gated endpoints and not the active ones.

- **Each basis now reads itself**, mirroring the gated handling.

- **I PREDICTED THIS WAS INERT, AND I WAS WRONG.** I claimed peak and runtime
  leakage were equal by construction, from reading set_pppm: slots 1-3 carry
  the same multiplier in both bases, only slot 0 (dynamic) differs, and no
  component assigns rt leakage independently. Rather than trust that reading I
  asserted it in code, so the model would report a divergence instead of
  silently overwriting it. Gate 1143 fired on the first run:
        peak leakage    0.0361957 W
        runtime leakage 0.0060599 W      -- 5.97x apart
  So the old code was inflating runtime leakage ~6x for every DRAM-periphery
  component. The reading was wrong; the assertion is why we know.

DATA IMPACT: none, and for a reason worth recording. extractComponent takes
runtime_dynamic from rt_power but LEAKAGE FROM comp.power -- the peak basis,
deliberately, per a 1.11.4 comment. So McPAT's runtime leakage has never
reached a reported number, and corrupting it moved nothing. The cell is
bit-identical to 1.11.31 (0.0499777 W, 10164680 cycles). Gate 1143: 3/4, with
the one FAIL being the assertion doing its job.

RAISED AND LOGGED, not fixed here:
  N4 extractComponent mixes bases -- dynamic from runtime, leakage from peak,
     which differ 6x. Leakage is static power, so that gap needs explaining
     before either basis is chosen. Partial trace: core.cc sets power and
     rt_power from the SAME leakage term, so the divergence enters through
     power_t. Not root-caused; mixing bases inside one PowerMetrics is the
     shape of the Joules-vs-Watts defect 1.11.18 found.
  N5 ComponentType::FULL_SYSTEM is declared and never populated, which is why
     E8's rollup was dead code and why completing it moved no number.

## 1.11.31 -- the pitch factor is bounded by silicon, and says when it is ignored (E7)

power.subarray_pitch_factor was the one knob in the 1.11 process surface with
NO validation: 0, negative and 1e9 all parsed, a later guard quietly turned
non-positive into 1.0 (so a typo silently became "no penalty"), it was
discarded without a word at any placement other than SUBARRAY, and it appeared
in no documentation.

WHAT IT IS, now written down. Circuits placed against a memory array must be
laid out ON THE ARRAY'S PITCH -- a sense amplifier cannot be wider than the
bitline pair it serves, however small its transistors are. That is a LAYOUT
constraint, distinct from the device-size penalty the 2.44x area factor
already carries. Vogelsang (MICRO 2010) draws exactly this line between
on-pitch circuitry (sense-amp stripes, local wordline drivers) and off-pitch
circuitry limited by wiring.

- **Floor 1.0.** The knob multiplies a PENALTY; below 1 it is a discount
  claiming the PE is denser than the baseline it is measured against.
- **Ceiling 10.** It compounds into the reported band [fa*p, fa^2*p]; at p=10
  the band top is ~60x, which no silicon supports.
- **Warning above 1.25, from the FIMDRAM anchor.** Samsung's HBM-PIM bounds a
  real DRAM-process compute unit at ~1.5 mm^2 (ISSCC 2021 25.4 + our measured
  HBM2 density); our comparable PE prices at 0.4925 mm^2 in logic, so the
  TOTAL observed penalty is <= 3.05x, leaving <= 1.25x for pitch once the
  2.444x device factor is applied. Their PCU is bank-shared rather than
  subarray-pressed, so this warns rather than refuses.
- **Announced when dropped.** Setting it at BANK placement now prints that it
  was ignored and why, at both construction sites, instead of vanishing.
- **Documented** in yaml_reference.md, along with power.interconnect_projection
  from 1.11.30, which was also undocumented.

DATA IMPACT: NONE. Default behaviour is bit-identical to 1.11.30 -- 0.0499777 W
and 10164680 cycles -- and a factor set at a non-SUBARRAY placement produces
exactly the default number, now with a line saying so. Gate 1142: 6/6.

RAISED DURING THIS RULING, logged as N1-N3 in the DISCUSS queue rather than
folded in here:
  N1 PITCH and DEVICE FAMILY are conflated -- pitch lives inside the family-1
     branch, so it can never apply to SRAM/NVM, whose PEs are logic-family but
     whose subarray-placed circuits are pressed against an array pitch just the
     same. Two different claims.
  N2 tech_node_nm is a meaningless degree of freedom for a DRAM-placed PE: the
     logic node CANCELS for area (proved -- 2.4615 x 1.4444 = 3.5556), which is
     an accidental validation of 1.11.22. Whether fd and fl cancel likewise is
     unchecked.
  N3 A derived pitch factor must come off the DRAM GENERATION ladder, not
     CACTI's logic-node tables: CACTI carries ONE comm-dram cell per table and
     would hand the same pitch to technologies our density table shows differ
     by 4.4x.

## 1.11.30 -- one die, one metal stack (E5)

User ruling on E5: the interconnect projection must MATCH between the two
tools and be user-settable. PIMID pinned CACTI to ic_proj_type = 1
(conservative, commented "required for reliable results") while McPAT
defaulted to 0 (aggressive) and nothing overrode it -- so the same die's wires
were modelled in two ITRS projections: arrays and caches lossier, cores, NoC
and controllers idealised.

- **power.interconnect_projection** (conservative | aggressive) is now one
  surface feeding both tools, validated at parse, with an invalid value refused
  rather than silently defaulted.

- **The default is CONSERVATIVE, on physics rather than caution.** The
  aggressive column sets barrier_thickness = 0 at EVERY node (22/32/45 nm),
  and a copper wire with no diffusion barrier cannot be built -- Cu diffuses
  into SiO2 and the barrier does not scale with the wire. It also holds
  alpha_scatter = 1.00, i.e. no surface or grain-boundary scattering, which
  measurement contradicts once wire dimensions approach the electron mean free
  path (hence conservative's 1.05 at 22 nm against 1.00 at 32/45). Aggressive
  is the ITRS TARGET projection; the industry did not meet it on interconnect
  resistivity. Conservative describes silicon that exists -- which is also what
  the FIMDRAM and Sohn anchors we calibrate area against are measurements of.

DATA IMPACT, measured: the on-die HBM3 cell moves 0.0499058 -> 0.0499777 W,
+0.144%. Only McPAT's side moves; CACTI was already conservative, so the die
area is bit-equal at 112.00 mm^2 and every array/cache number validated this
week is unchanged. Timing untouched.

PREDICTION vs MEASUREMENT, recorded: I predicted "a few percent on
wire-dominated components". The actual delta is 0.144%, an order of magnitude
smaller. Direction and sign were right -- conservative is lossier, so power
rises -- the magnitude was not. Setting aggressive explicitly reproduces the
1.11.29 value EXACTLY (0.0499058), which is what proves the knob is a real
toggle and not a renamed constant.

Gate 1141: 5/5.

## 1.11.29 -- the link controller is reported, and harnessed to our interconnect model

User ruling: instantiate the link controller, report components and totals,
and let the USER's system definition decide the report's shape.

- **E8, as refined: report what was configured.** The family rebuild's
  processor rollup listed core+l2+l3+noc+mcs by hand and omitted l1dir, l2dir,
  nius and flashcontrollers. Each is now included, but GATED on its configured
  count -- reporting a component nobody asked for is its own error.
  flashcontrollers is kept for storage / storage-class compute.
  printComponentBreakdown() gained the same discipline: cores, caches and MCs
  were printed unconditionally, so a device with no L3 printed "L3 Cache: 0 W".

- **System scope reports its components at all.** printComponentBreakdown()
  existed with NO caller, so a co-sim run printed one line per node and nothing
  else -- you could not see what the link, the NoC or the MCs cost. That is why
  the link-share question could not be answered without instrumenting.

- **The link controller's area was computed and dropped.** getComponentArea()
  had no PCIE case and fell through to 0, so 0.18 mm^2 per end never reached
  any total.

- MEASURED, once the report existed: host 0.360 W / 5.66 mm^2, device
  0.050 W / 12.27 mm^2, link controller 0.0075 W and 0.36 mm^2 across both
  ends -- about 1.8% of system power and 2.0% of area. The link is
  LEAKAGE-dominated: its runtime dynamic is 8 uW on this cell.

- **Harnessed to the interconnect model, in the ruled order:**
  1. withPHY from the link CLASS, not the literal 1. An interposer is a wide
     parallel on-package link with no serialiser; it was paying for a full
     off-package SerDes. Measured: its controller drops to 0.094 mm^2 against
     gen5's 0.18 -- 52%, exactly the SerDes area removed. This was the one
     error wrong in KIND rather than degree.
  2. num_channels was ALREADY the user's surface (power.pcie.num_lanes), so
     no change was needed. Recorded rather than silently skipped.
  3. The SerDes lane rate is now a parameter. It was the literal 4 -- PCIe
     2.0's 4 Gb/s, a 2007 standard -- applied to gen5 (32 GT/s), CXL (gen5
     PHY), NVLink (100 Gb/s/pair) and UALink (200G-class) alike. PIMID
     supplies the class's published rate; an unknown class keeps McPAT's 4
     rather than inventing one.

BOUNDARY, stated because the gate could not show it: step 3 affects the
PEAK/TDP path only. Runtime link dynamic is REPLACED by our byte-driven term
(measured crossing bytes x cited pJ/bit, iocontrollers.cc), which is the right
owner, and co-sim does not currently print peak power. So the lane-rate fix is
correct and currently invisible in a co-sim report.

NOT MODELLED, acknowledged rather than invented: CXL's coherence logic. CXL
rides the PCIe gen5/6 electrical PHY, so pricing its PHY as PCIe is right;
what it adds beyond the PHY is snoop/state silicon for which no area source is
in hand. Gate 1140: 5/5.

## 1.11.28 -- one fabric, one process (E3)

User ruling on E3: the synthetic in-memory NoC probe derives its process family
from the same owner the device path uses, instead of hand-building a config.

- **The defect.** The probe (main.cpp, method == "synthetic") built its own
  McPAT config, declared device_scope = true and commented "a synthetic probe
  of the IN-MEMORY network" -- then never set process_family, so it defaulted
  to 0. The same on-die fabric was priced in a LOGIC process while the PEs
  beside it, on the same die, priced as DRAM-periphery: one fabric, two
  processes, in one run.

- **The fix is a single owner.** applyProcessFamily() now holds the
  placement x technology decision and both the device path and the probe call
  it. Two copies of a decision is how they drifted; there is one copy now. The
  probe also stops hardcoding temperature_k = 350, so a temperature surface
  added later reaches both paths rather than one.

- **This became safe only because of 1.11.22.** The factor is a ratio across
  two CACTI tables; until both ends were named, a family-1 probe running at
  tech_node_nm would have been mispriced. E3 was raised before that and had to
  wait for it.

DATA IMPACT: NONE, and stated precisely. The probe runs only under
method == "synthetic", a standalone NoC-characterisation mode; every corpus
cell uses the trace/exec path and never constructs it. Gate 1138 confirms the
on-die cell unchanged at 0.0499058 W and 10164680 cycles, and that an off-die
(RANK) placement still prices as LOGIC -- the arm that would catch an
over-broad fix.

GATE NOTE, recorded because it nearly went the other way: W1 was written to
assert the on-die power MOVED, against a baseline of 0.0498939. That is the
1.11.21/22 value; the cell has read 0.0499058 since 1.11.23. The arm passed on
a stale reference while the number had not moved at all. The conclusion above
comes from tracing the probe's guard, not from that arm.

## 1.11.27 -- the area factor gets a silicon anchor, and it is not the pessimistic one

The DRAM-periphery area factor has shipped since 1.11.2 with an uncertainty
band of [2.44, 5.98]x and a comment saying "the UPMEM die is the only silicon
anchor between them". Reading the UPMEM deck (Hot Chips 31, user-supplied)
showed it carries no die area at all -- so that anchor was qualitative, and the
top of the band had never been tested against anything.

- **PE/core area is now reported.** getComponentArea(CORE) existed and was
  never printed, so the only area a run exposed was Total Area -- which bundles
  NoC, MC, caches and the memory die. A first attempt at this validation
  compared that total against a published per-compute-unit figure and produced
  a meaningless 3.8x; the tell was that the ratio between placements came out
  1.788x when the printed factor was 2.444x, i.e. diluted by components the
  factor does not scale. With PE-only area the ratio is exactly 2.444x, which
  also confirms the factor reaches the quantity it is supposed to scale.

- **The anchor.** Samsung FIMDRAM (Kwon et al., ISSCC 2021 25.4) replaced HALF
  the cell array in each bank with a 16-wide SIMD programmable computing unit
  on a 20nm DRAM process, at unchanged HBM2 physical dimensions: 8 GB -> 6 GB,
  so 2 GB of cell array bought the compute. At our measured HBM2 density
  (Sohn, ISSCC 2016 18.2: 8 Gb / 96 mm^2) that is ~24 mm^2 per die across 16
  PCUs -- about 1.5 mm^2 per PCU, an UPPER bound since some reclaimed area is
  control and routing.

- **The test, and the result:**
      comparable PE in a logic process      0.4925 mm^2
      linear end  (2.444x)  ->  1.2039 mm^2   UNDER the ~1.5 bound
      squared end (5.975x)  ->  2.9428 mm^2   ABOVE the bound by 1.96x
  Silicon contradicts the pessimistic end of our own band and is consistent
  with the linear one -- which is the value we apply. The band print now says
  so and names the paper, instead of citing an anchor that has no number in it.

DATA IMPACT: none. This reports an existing quantity and annotates a print;
the applied factor is unchanged at 2.444x. What changes is that the value now
has evidence behind it rather than a stated preference between two ends.

## 1.11.26 -- LPDDR5 terminates, and it terminates to ground

The termination table declared LPDDR5 "unterminated by design" and returned
exactly 0. Micron's own datasheets say otherwise, in the feature list:
"Programmable VSS on-die termination (ODT)", "Interface-LVSTL 0.5/0.3",
"VDDQ = 0.50V ... 0.30V TYP (ODT off)", RON = 40 ohm
(misc/MICT-S-A0025741931-1.pdf, misc/315b-441b-561b-y52q-*.pdf).

- **LVSTL is a THIRD topology, not the absence of one.** POD terminates to
  VDDQ, SSTL to a VDDQ/2 mid-rail, LVSTL to VSS. Current flows while the
  driver holds the line HIGH -- the mirror of POD -- so duty ~0.5 for unbiased
  data across a loop of driver pull-up plus terminator. Added as its own
  scheme rather than folded into POD, because the conducting state is the
  opposite one.

- **Measured: 0.06975 pJ/bit = 0.0357 nJ per 64 B access**, where the model
  previously charged nothing. Small in absolute terms -- the 0.5 V rail is
  4.8x less V^2 than DDR5's 1.1 V before resistance divides -- but it is a
  real term that was missing, and LPDDR5 is a low-power technology precisely
  because of choices like this one, so reporting it as zero misrepresented
  the reason it wins.

- **RESIDUAL, stated in the code**: Rtt = 240 ohm (RZQ, the LPDDR4/5 ODT
  reference) is the one unsourced input. The part datasheet defers its ohm
  table to Micron's separate "General LPDDR5 Specifications 2: AC/DC and
  Interface", which we do not have. D12's IDD4R/IDD4W calibration can pin it,
  and those IDD tables ARE in hand.

DATA IMPACT: LPDDR5 cells at RANK/HOST_MC placement gain 0.036 nJ per access
of termination energy (on-die placements are unaffected -- termination is
placement-gated since 1.11.5). No other technology moves: gate 1136 holds
DDR5 at 0.733 and DDR3 at 2.432 nJ/64B, and HBM at exactly 0. Gate 1136 4/4.

## 1.11.25 -- placement reaches the array, and the tier gap turns out to be 2.4%

The plugin contract from 1.11.24 is now wired: main.cpp asks the technology's
own model for the tier the PE sits at. Adding a memory technology is
"implement the contract, bind your tool" -- no edit in main.cpp.

- **Placement had never reached the array.** getMemoryLatencyCycles() took no
  placement argument, so a subarray-placed PE and a chip-placed PE were
  charged the SAME 64 KB per-bank access. All of Figure 2's NVM/SRAM tier
  separation came from the network path and none from the array.

- **A correction to 1.11.23, and to what this release first claimed.** The
  subarray tier was built on getDecoderDelay() and four siblings, described in
  the code as NVSim component delays. They are not: they return INVENTED
  percentages of mat.readLatency (10/20/45/15/5, summing to 0.95). Composing
  them replaced an invented x1.2 with an invented x0.95. NVSim resolves the
  real thing -- SubArray derives from FunctionUnit, Mat holds a SubArray -- so
  bank->mat.subarray.readLatency is now read directly.

- **The ladder had to go into the CACHE, not just the result tree.** NVSim
  characterization costs minutes, so the set is PREGENERATED and a normal run
  never touches NVSim. A field not stored in the cache does not exist for the
  corpus. subarray_latency_s and mat_latency_s are now captured at
  characterization and carried through the XML. Absent (older cache) leaves
  them at -1 and the tier reports UNSOURCEABLE rather than being filled.

- **Three configuration mismatches, each caught by an arm.** The plugin path
  must be configured identically to the path it replaces or it characterizes a
  different device: technology string ("STT_MRAM" fell through
  parseMemoryTechnology to UNKNOWN -- two vocabularies that had drifted apart);
  capacity (the model defaults to 256 MB against the live path's 64 KB
  per-bank unit -- 148x apart in read latency); access width (64 b against
  512 b, which selects a different pregenerated cache entry). Fixed with
  setArrayCapacityBytes()/setAccessWidthBits() on the contract.

MEASURED, and it is the headline. With the regenerated cache:
    tech        bank(read)    subarray      mat
    STT-MRAM    3.29773 ns    3.21832 ns    3.29773 ns
    PCM         2.83215 ns    2.76921 ns    2.83215 ns
    ReRAM       3.48713 ns    3.39796 ns    3.48713 ns
Bank latency EQUALS mat latency in all three: a 64 KB array is a single mat,
so there is no bank-level H-tree above it. The subarray is 97.6% of the mat.
So at the per-bank granularity the corpus uses, subarray and bank placement
differ by 2.4% in array access -- not the 20% the old x1.2 asserted, an ~8x
overstatement of the tier separation. The separation visible in Figure 2 comes
from the network, which is correct; the array contributes almost nothing at
this granularity, and now says so.

DATA IMPACT: NVM/SRAM cells re-price by the array-side tier delta (~2.4%
subarray vs bank). DRAM is unchanged -- gate 1134 U4 holds HBM3@BANK at
10164680 cycles. Gate 1134: 6/6.

NVSim cache: the three 64 KB/512 b entries were regenerated to carry the
ladder; the other 12 entries are untouched and still work (their tiers report
unsourceable until regenerated). Backup at ~/.cache/pimid/nvsim.bak.pre-1.11.25.

## 1.11.24 -- a memory technology becomes a plugin, and 322 invented numbers die

User decision: PIMID needs a general memory plugin interface driving all
technologies and the figures. The pieces were already there -- MemoryModel is
a genuine abstract interface with a factory covering every technology -- and
were never wired. This release builds the missing contract and clears what
blocked wiring it. NO BEHAVIOUR CHANGE: gate 1133 is bit-equal to 1.11.23.

- **The uniform tier contract.** Every model already resolved subarray/bank/
  chip, but under incompatible names and arities: getSubarrayAccessLatency
  (DRAM) against getSubarrayReadLatency (SRAM/NVM), with PCM alone carrying
  SET/RESET. main.cpp therefore could not ask a MemoryModel for a placement
  latency without knowing which technology it held -- which is precisely what
  a factory exists to avoid, and why the factory was never called. Added to
  the base interface:
      double getTierLatencyNs(Tier, Op)   -- <0 when unsourceable, never faked
      bool   hasTier(Tier)
      string tierLatencySource(Tier, Op)  -- provenance, per tier and op
  Implemented for all five technologies, each delegating to its own tool:
  DRAM->Ramulator, SRAM->CACTI, STT-MRAM/PCM/ReRAM->NVSim.

- **hasTier() carries the not-DRAM-like ruling structurally.** SRAM and NVM
  report SUBARRAY/BANK/CHIP only; DRAM reports all six. A tier that does not
  exist is now reportable as ABSENT rather than collapsed onto its neighbour
  or filled from a multiplier -- which is what 1.11.23 found and removed.

- **Tool failure REFUSES.** All five models threw out their "extraction
  failed, use factory defaults" path. That fallback is what let unsourced
  specs reach a result indistinguishable from a real CACTI/NVSim read.

- **322 invented literal assignments DELETED**, with the seven orphaned
  factories that held them: createSTTMRAM_Everspin_256Mb (46),
  createSTTMRAM_8MB_22nm (46), createPCM_16MB_90nm (50),
  createReRAM_2MB_32nm_Analog (50), createReRAM_8MB_22nm_Digital (50),
  createSRAM_L3_8MB_22nm (40), createSRAM_LLC_16MB_14nm (40). Not sourced --
  REMOVED, because nothing could derive them. The structs stay: they are the
  plugin contract, filled from the tool, never by hand.

- **The DRAM factories are NOT deleted, and must not be.** They are LIVE:
  RamulatorWrapper::initialize() calls createHBM3_Verified()/
  createDDR4_2400_Verified(), and getTRCD() reads dram_arch_->timing.tRCD_ns
  -- HBM3's JESD238-cited 16.0. Those carry real cited JEDEC timing and need
  REPLACING with reads of the Ramulator timing preset the wrapper already
  names, not removing. Tracked as the 1.11.23 residual.

DATA IMPACT: none, by construction and by measurement. Gate 1133 4/4: HBM3
bit-equal (10164680 cycles, 0.0499058 W), all six technologies rc=0, and no
refusal fired -- confirming no removed fallback was load-bearing.

NEXT (1.11.25): wire main.cpp:150-190 to MemoryModelFactory. That is the
behaviour change -- NVM and SRAM latency becomes a real subarray/bank/chip
ladder instead of one flat tool number per technology, re-pricing every NVM
and SRAM cell and moving Figure 2's tier separation. Gated separately.

## 1.11.23 -- the DRAM latency accessors stop asking themselves

Four accessors were CIRCULAR: each returned the architecture field that
architecture_extractor.h assigns FROM it, so the "primary" branch handed back
a hand-written literal while the real derivation sat unreachable in the
dram_arch_==null fallback. The fallback was the correct path all along.

  getSubarrayAccessLatency()  -> tRCD + tCAS
  getBankAccessLatency()      -> tRP + tRCD + tCAS
  getBankGroupAccessLatency() -> bank (floor; see below)
  getChipAccessLatency()      -> bank + tBurst

- **The bank-group multiplier is gone.** It was `bank * 1.1`: a 10% penalty
  with no source, charged even to technologies that have no bank groups. The
  real term is tCCD_L - tCCD_S, which this wrapper does not expose, so the
  bank latency is reported as the correct FLOOR rather than a manufactured
  penalty. Wiring tCCD through is the follow-up.
- **getChipAccessLatency had no derivation at all** in either branch: a
  literal, or `60.0 // Typical DDR4`. It now composes bank + tBurst.
- **HBM3's hand-written ladder contradicts its own cited timing.** The header
  carries tRCD 16.0 and tCAS 16.0, both cited JESD238, next to
  subarray_access_ns = 20.0 -- but tRCD + tCAS is 32.0. Likewise
  bank_access_ns = 30.0 against tRP + tRCD + tCAS = 42.0. A 12 ns
  disagreement at both tiers, between numbers three lines apart. The
  de-circularised accessors compose from the cited values, so the ladder is
  now consistent with the JEDEC timing it claims to be built on.

DATA IMPACT, measured: the 1-PE HBM3 ALU cell moves 10164714 -> 10164680
cycles (-34, faster) and 0.0498939 -> 0.0499058 W. The direction is the
bank-group multiplier removal dominating on a cell that touches that tier on
every access. Gate 1132: all five technologies complete, rc=0.

SCOPE NOTE, and the reason this release is smaller than intended: the same
defect class was found and fixed in the NVM and SRAM architecture extractors
(NVSim's BANK latency assigned to the SUBARRAY tier, then x1.2 for bank and
x1.1 for chip; PCM reset asserted as write x 0.3 when NVSim resolves
FunctionUnit::setLatency/resetLatency; the whole block stamped VERIFIED and
attributed to NVSim including values NVSim never supplied). That work is NOT
in this release, because the path is DEAD: MemoryModel -- and therefore
SRAMModel/STTMRAMModel/PCMModel/ReRAMModel, the only callers of those
extractors -- is never constructed. main.cpp references MemoryModel zero
times, and a gated STT_MRAM run initialises NVSim and never prints a single
model line. The LIVE NVM/SRAM path (main.cpp:150-190) calls the tools
directly: CACTI getAccessTime() for SRAM, NVSim getReadLatency() for
STT_MRAM/PCM/ReRAM. So no invented multiplier has ever reached a result.
Reviving or deleting that parallel model is a decision, not a patch.

## 1.11.22 -- the factor ratio names both of its tables

A correctness hole in 1.11.21, found while investigating E3 and fixed before
it could reach a result.

- **The periphery factors are a ratio across TWO tables, and 1.11.21 read
  both ends from one.** The numerator (comm-dram) belongs to
  dram_periph_table_nm, which comes from the memory TECHNOLOGY: DDR3 -> 32 nm,
  everything else -> 22 nm. The denominator is the column McPAT actually
  priced at, which is tech_node_nm, the user's logic node from the positive
  list {22,32,45,65,90}. They are independent. 1.11.21 took both from the DRAM
  table, so whenever they differed the denominator was hp at the wrong node --
  l_phy(hp) is 0.009 at 22 nm and 0.013 at 32 nm. The same defect class as
  E1, one level up: a within-table ratio multiplied across tables.

    fa = l_phy(comm-dram @ dram_table_nm) / l_phy(base @ tech_node_nm)

  and identically for fd (capacitance + Vdd^2) and fl (I_off at the
  configured temperature).

- **Measured impact.** DDR3 at the fleet's 22 nm logic node: the area factor
  is 0.032/0.009 = 3.5556, not the 0.032/0.013 = 2.4615 that 1.11.21
  computed. DDR3 was priced 1.44x too low. The dynamic factor changes sign
  relative to unity as well (1.2476, not sub-1): a 32 nm DRAM device against a
  22 nm logic baseline has MORE gate capacitance, which is only visible once
  both ends are named.

- Inert wherever the two nodes coincide, which is every non-DDR3 cell at
  tech_node_nm=22 -- i.e. most of the corpus. It bites DDR3 at any node and
  every technology at a non-22 nm node, both of which the 300-cell fleet
  contains.

The provenance line now names both tables:
  [tech] periphery factors READ: comm-dram from 32nm.dat / baseline from
         22nm.dat at 77C: area 3.55556x, dynamic 1.24758x, leakage 3.07677e-06x

Gate 1131: 10/10, including R6 which asserts the cross-node ratio directly.

## 1.11.21 -- the process factors stop being constants

E1 and E2 of the DISCUSS go-through, ruled by the user with "I want
CORRECTNESS". Both turned out to be one defect seen from two sides.

- **The DRAM-periphery factors are ratios, not constants, and are now read
  from the CACTI table the run is already using.** Each derivation reproduces
  the literal it replaces to six figures, which is how we know it is the one
  the original number came from:
    fa = l_phy(comm-dram)/l_phy(base)                 22nm hp 2.444444 (2.44)
    fd = (C_g+C_fringe) ratio x (Vdd ratio)^2         22nm hp 0.824128 (0.82)
    fl = I_off_n(T) ratio x Vdd ratio                 22nm hp @50C 1.035e-5 (1.0e-5)
  The Vdd^2 in fd is not a double count: applyFam multiplies McPAT's
  already-computed dynamic, and McPAT computed it at the BASE column's Vdd.
- **The leakage factor was evaluated at a temperature no run uses.** The
  literal reproduces the table at 50 C; temperature_k defaults to 350 K =
  77 C. Reading the correct row gives 6.814e-6 against the shipped 1.0e-5 --
  the constant was 47% too high. Not one of the 200 audit findings; it
  surfaced only because deriving the factor forced the question "at what
  temperature". fl now reads the row for the configured temperature.
- **The corner refusal was defeated by an override, and justified by a
  22 nm-only fact at every node.** power.mcpat_overrides.device_type was
  applied AFTER the refusal, so a run printed "refused" and then priced at
  the refused column anyway, keeping an hp-derived factor over a moved
  baseline -- a 1.55x error in fa, silently. With the factors derived, the
  override is no longer a hole: the factor tracks the baseline, so it is
  accepted and produces comm-dram/lstp = 1.5714 with matching fd and fl.
- **A DRAM-periphery low-power corner is lp-dram, not logic lstp.** On a
  periphery placement power.device_corner maps hp -> hp and lstp|lop ->
  lp-dram, the low-power variant the DRAM table actually carries. That column
  is all-zero at 22 nm and real data at 32/45 nm, so the refusal is a
  populated-column CHECK per node rather than a blanket rule.

DATA IMPACT: default cell cycles and DRAM background bit-identical; total
power +0.502%, which is exactly fd's +0.503% correction on a
dynamic-dominated cell. Any run that sets a non-default corner or override
re-prices, correctly, for the first time. Gate 1131: 9/9.

## 1.11.20 -- the memory system's background, and the host's own gate

Second half of the settled model decisions, plus the last sourcing gap in the
area table.

- **D13, population.** Standby and refresh were reported PER UNIT and never
  multiplied up: an HBM stack's 8-16 channels and a DDR rank's 8 chips were
  each charged as one device. `backgroundUnits()` now supplies the count from
  technology and JEDEC device width. It deliberately does NOT read
  `hierarchy_chips_per_rank`, which holds the same numbers but is degenerated
  to 1 for HOST_MC placement, where it is an address-mapping fanout -- reading
  it would have zeroed the correction for exactly the baseline placement.
- **D15, state.** An idle DRAM device sits at PRECHARGE standby (IDD2N), not
  active standby (IDD3N): an idle controller closes its pages whether or not
  power management exists. 1.11.18 identified this and left the baseline at
  IDD3N to preserve pg-off bit-identity, which meant the baseline stayed
  knowingly wrong. The baseline is fixed instead; `pim.mc.pg` now only decides
  whether the descent continues to IDD2P. The tXP slice that cannot reach
  power-down rests at IDD2N, not IDD3N.
- **Unarmed-counter refusal.** If a run has memory traffic but the device-MC
  phase counter never advanced, the residency would come out 1.0 -- the whole
  memory reported idle. The run now says the residency is unavailable and
  reports active standby, instead of printing a number from a dead counter.
- **D6, one background per machine.** System scope reports the same
  population-scaled, state-aware background device scope does.
- **D16.** The SRAM periphery share is derived from a second CACTI query
  (1 - array-only leakage / total leakage) instead of the declared 0.30.
- **D7, the host gates as one piece.** The per-node PG block was DEVICE-only,
  so no system-scope host could gate anything. `hosts[].pg` (default false)
  gates host cores and the host MC together, per the one-piece decree. The
  union of the two activity signals is not counted, so the conservative
  max(core, mc) is used and printed as such: it under-credits, never over.
- **D14, the invariant says what we actually guarantee.** The fixed-BSS
  comment claimed bit-identity ACROSS versions. Measured: identical 1.11.15
  source built in two trees gives 10164688 vs 10164694 cycles. The guarantee
  is same-binary determinism; gates compare within-build pairs. Written down
  rather than restored, because restoring it would not deliver what it
  claimed.
- **HBM2 die density is now measured.** 8 Gb / 96 mm^2 = 10.7 MB/mm^2, from
  Sohn et al., ISSCC 2016 paper 18.2 and IEEE JSSC 52(1):250-260, Jan 2017
  ("the chip size is 12x8mm2"; "each core die has 8 Gb DRAM cell array with
  additional 1 Gb"). This replaces the 1.11.19 placeholder (HBM3 x 0.70),
  which was 34% too dense. Every row in the density table now rests on a
  published measurement.

DATA IMPACT, measured by gate 1130 rather than predicted:

- The background line moves by the POPULATION factor and nothing else:
  16x for HBM3 (26.366 -> 421.858 mW/stack), 8x for a DDR-class rank.
- D15's state descent is INERT across our benchmark suite. It multiplies the
  measured idle fraction, and that fraction is exactly zero: the device-MC
  phase counter reads 1017 active out of a 1017-phase ROI window on
  stream_triad, and the same on gemv. Our kernels are memory-bound, so the
  controller is busy in every phase. D15 is correct and will engage on a
  workload with idle windows; it changes no number in the corpus today. An
  earlier draft of this entry claimed every cell re-prices "in BOTH
  directions" -- the gate refuted that half, and the gate is right.
- pg-off is deliberately NOT bit-identical for the DRAM background line. That
  is D15's point, and gate 1130 Q2 FAILS if the line comes out unchanged --
  the inverse of every previous gate arm.
- Total energy is unaffected (0.4 mJ, identical): the background is reported,
  not integrated into the energy total.
- HBM2 die area moves by 1.34x from the density correction.

## 1.11.19 -- the die area stops being a stack of two errors

Folded into the 1.11.20 commit (1.11.19 was gated but never shipped
separately). The area/energy-basis decisions, and the two defects the gate
found while validating them.

- **D11, full-die density.** The vendor density table was rewritten on a
  full-die basis with a per-row citation, and its ORDERING was inverted: we
  ranked HBM densest, silicon says it is least dense. DDR4 is ~85% denser
  than HBM3.
- **The k-calibration divided megabits by megabytes.** `vendorDieDensity()`
  returns MB/mm^2; the capacity fed to it was in Mbit. Every DRAM die area was
  8x too large. It stayed invisible because the old density table was itself
  ~4-5x too dense and the two errors partly cancelled -- correcting only the
  density exposed it as a 22x jump that breached the reticle limit. Both
  halves are fixed: HBM3 lands at 112 mm^2 (real 16 Gb HBM3 die ~100), DDR5 at
  7.1 mm^2 for a 2 Gb die.
- **A null PHY crashed McPAT.** D3 made the memory controller allocate its PHY
  on `withPHY` alone, but four consumers still tested the old
  `type==0 || (type==1 && withPHY)`. For the new case -- an on-die element MC,
  type 0 with no PHY -- they dereferenced a null pointer and the McPAT child
  died on SIGSEGV, taking the whole power report with it. All five guards now
  match.
- **D2, D3: MC interface tiers.** `withPHY` is honored for type=0, so an
  on-die controller can drop its off-chip driver without switching the backend
  cost model. The interposer PHY constant is corroborated by NVIDIA's measured
  HBM2 breakdown (O'Connor et al., MICRO-50 2017, Table 3: interposer I/O
  0.3 pJ/bit at application toggle rates, 0.80 at 50%).
- **D4** coherence flush charges a per-rank slice; **D8** the timing link type
  is authoritative for power; **D1** ualink_1_0 is priced; **D10** LVDS is
  emitted explicitly rather than inherited from a McPAT default.

DATA IMPACT: every reported DRAM die area changes. Timing is untouched --
the 1-PE ALU cell is bit-equal to 1.11.18.

## 1.11.18 -- power gating measures what it claims to measure

The PG-correctness cluster of the pre-fleet triage, plus a units defect
sitting next to it. Every item here made power gating credit savings it had
not earned, or measure them against the wrong window.

- **CoreBreakdown added Joules to Watts.** A per-core sub-block's
  rt_power.readOp.dynamic is ENERGY over the run -- McPAT converts it only
  when aggregating to the Processor level (processor.cc multiplies the
  core's rt_power by 1/executionTime). The leakage term is already Watts.
  Every printed block weight was therefore wrong by the run length, and the
  blocks-sum-vs-core-total check compared incomparable units. Both the
  weight and the family-scaled variant now divide by executionTime.
- **The shared caches gate on the shared-cache signal.** #84 says so, and
  the counter has been instrumented, exported and parsed since 1.11.8 --
  while L2/L3 were interpolated with the CORE's retirement residency. A core
  can retire out of its L1s for long windows with the LLC untouched.
  Reported as its own `shared$=` residency; falls back to r_core only when a
  run carries no shared-cache counter.
- **Injected timing charges no longer count as retirement.** Barrier, flush
  and launch waits are exactly the windows power gating exists for, and they
  were marking the PE ACTIVE (they arrive as BBLs). The 1.11.16 `synth` flag
  makes the distinction available at all six core retirement sites.
- **The residency and the work it prices share a window.** PG residency was
  whole-run while every activity counter it is weighed against is
  ROI-relative. The five global trackers and the per-core counter now rebase
  at roi_begin, and a new `pgPhaseWindow` stat carries the matching
  denominator (absent on older dumps -> falls back to the whole-run count).
- **Controllers that never marked anything now do.** RamulatorMemory marked
  NO PG activity at all -- a Ramulator-backed machine reported an always-idle
  MC and took the entire leakage as a gating credit, silently. SimpleMemory
  missed dirty writebacks (PUTX). Both fixed; and when a residency still
  comes out exactly 1.0 on a run that moved memory traffic, the credit is
  REFUSED with a printed reason instead of taken.
- **SRAM main memory honors pim.mc.pg.** It sat in the same if/else chain as
  the DRAM descent and the NVM retention floor and did nothing. SRAM is
  volatile, so only the periphery can gate: the periphery share is declared
  (0.30) and gated at the CACTI-class sleep-tx residual (0.35), with the
  cell array keeping full leakage. Both constants printed.
- **The DRAM power-down credit is the power-down delta.** The descent
  interpolated from IDD3N (active standby), but entering IDD2P requires
  precharged banks, and an idle controller closes pages whether or not a
  power-down feature exists -- so the IDD3N->IDD2N step is page policy, not
  power gating. On our own IDD table that over-credited by 1.36x (HBM3) to
  2.00x (GDDR6). The credit is now IDD2N->IDD2P. The baseline stays at
  IDD3N deliberately, so PG-OFF results are unchanged and the model now
  under-credits rather than over-credits.

**What the ROI fix uncovered.** On the 16-PE gate cell the PE residency
falls from 0.932863 to 0.551424 -- not because idleness changed, but because
the window did: the run simulates 6577 phases of which the priced ROI is
**79**. Ninety-nine percent of the phases the old residency averaged over
were setup. Every PG number since 1.11.8, including the "79% reduction at
93% idle" figure recorded as the PG validation, measured the setup phase
rather than the workload. The 93% was the machine waiting to start.

Data impact: any run with `pg: true` re-prices (smaller, more defensible
savings) -- on top of the 1.11.16 correction, PG-enabled results from
1.11.8-1.11.17 should be treated as superseded, and the recorded PG
validation figure retired. PG-OFF runs are unchanged (the gate asserts
cycles AND power bit-identical). The CoreBreakdown fix changes a printed
diagnostic, not the component totals McPAT reports.

## 1.11.17 -- the mechanical half of the 200-finding go-through

The FIX-NOW class of the full-audit triage (the mechanical defects; the 34
model decisions are queued for discussion). Highlights:

- ONE node authority: getCacheLatencyCycles routed through the positive-list
  validator -- an invalid node no longer prices cache TIMING at a silently
  clamped 22 nm while the power path fatally rejects it.
- The printed periphery area factor is the one APPLIED (fa x pitch), and a
  non-hp corner says so when it is applied, not only when refused.
- System-scope device nodes get the same corner refusal/application, area
  band, UPMEM clock guard and device_type wiring as device scope -- the
  per-node path was a divergent copy where power.device_corner was silently
  ignored.
- CHANNEL placement follows the LOCKED per-tech ladder: on channel-centric
  LPDDR/GDDR/HBM the channel tier lives on the DRAM die and is now priced
  DRAM-periphery (DDR-class channel = buffer die stays logic).
- Bus-NoC wire length under the family rides sqrt(dram_periph_area) -- the
  bus was sized for a logic-process die the run then reported 2.44x larger.
- FP-without-FPU report: one shared copy, DEVICE-group census (the host's FP
  stream is no longer pinned on the FPU-less elements), and it prints with
  --no-power too (the timing charge fires either way).
- HOST_MC structural-zero explanation now keyed on the device MEMORY group
  (the core-group flag made it unreachable on exactly the runs it explains);
  co-sim host [Activity] prints the real_instrs McPAT is priced on (raw +
  synth shown).
- Shared-cache PG residency guard matches system-scope "<node>_l2*" names
  (the 1.9.29 node-prefix class, re-made); in-order cCycles is ROI-rebased
  like the OOO core (1.11.9 parity).
- vendorDieDensity: units stated (MB/mm^2; the "8x" comment contradiction
  was bits-vs-bytes) -- the array-region-vs-full-die question is flagged for
  the go-through with a per-row sourcing requirement.
- Docs: the yaml reference states the 22/32/45/65/90 positive list and stops
  demonstrating 7 nm; power.md stops claiming the memory-process penalty is
  missing (it shipped in 1.11.2/1.11.12).

Reclassified FIX-NOW -> DISCUSS with evidence (VERIFY_1126_DISCUSS D11-D14):
vendor density absolute values, termination scheme factors, DRAM background
population (device/rank/stack -- MC release #100), and the Core-layout
invariant (cross-binary +/-6-cycle drift measured on identical source; BSS
migration would not restore cross-version bit-identity).

Data impact: channel-placement cells on LPDDR/GDDR/HBM re-price (family);
family cells' bus-NoC dynamic/leakage shifts (wire length x ~1.56 at 22nm
class); logic cells and defaults unchanged.

## 1.11.16 -- the audit's fixes are audited, and three of them fall

Seven adversarial verifiers (one per 1.11.15 fix claim + a fresh sweep of the
hotfix diff) REFUTED three clusters. The repairs, all in this release:

POWER GATING HAD NO REAL ENDPOINTS. CACTI never assigns
power_gated_leakage anywhere in the vendored fork -- the 1.11.15 enable
repair opened a path to a value nothing writes. For set-associative arrays
(every cache data/tag array) McPAT multiplied that never-written zero, so
the gated endpoint was 0 and leak_eff credited ~100% leakage elimination
-- ANTI-conservative, the opposite of the in-code caveat. Fixed in the
tool: array.cc now computes the analytic Vcc_min/Vdd retention endpoint
for ALL array classes, the same model McPAT's own logic/NoC/MC units
already use. The residual is ~0.35 of active subthreshold, not ~0.

THE GATED ENDPOINT KEPT A DISCOUNT THE ACTIVE ONE DROPPED. applyFam
rebases active leakage on the plain value (the comm-dram ratio already
encodes a long-channel device) but only SCALED the gated fields, so the
default long-channel read compared mismatched bases and over-credited PG
savings ~2.25x. The recorded "15% sleep-tx residual" was
0.35 x long-channel-factor -- an artifact, not an endpoint. Fixed: gated
fields rebase exactly like lines 435-436. applyPG also now interpolates
SUBTHRESHOLD only (a sleep rail does not remove gate-oxide tunnelling),
warns two-sidedly (near-zero endpoints too, on stderr -- the child's
stdout is /dev/null), and scales the per-level NoC breakdown with the
aggregate.

THE CENSUS REJECT PATH EMITTED THE 1.9.28 DEGENERACY. The >5% reject set
mi=mf=0 and "fell through" into the assignment below it -- McPAT was told
0-int/0-fp/100%-branch, strictly worse than the fractions it claimed to
restore. The residual-as-int correction sat inside the print-once latch,
so the number depended on whether a message had printed (and the forked
child's regenerated XML differed from the parent's). Restructured:
census_ok flag, value logic decoupled from printing, fractions recomputed
on the census-branch base so classes always sum to retirement, and a
rejected census reverts its branch class too.

INJECTED CHARGES NOW SUBTRACT ON EVERY CORE TYPE. Barrier/PCIe/drain
charges are cycles-as-instrs; only OOOCore reported syntheticInstrs (by
oooBbl absence, which also misclassified real >1024-insn fallback TBs).
BblInfo carries an explicit synth flag set at the eight injection sites;
the shared mix machinery counts and reports it for alu/simple/null/
in-order/OOO alike, so MPI cells no longer manufacture a census deficit.

Also: MCPHY gated on PLACEMENT alone (SRAM/NVM on-die element MCs lose
the off-chip PHY they never had); per-node pg: keys parse in system scope
(devices[].pim.pe.pg / .pim.mc.pg / .noc.pg) with residencies printed per
node; PE-MI locality line now prints in device scope (where the placement
corpus lives); coherence-flush writebacks use the configured line size and
the "charged" line prints only where a branch lands the charge;
pj_per_bit_override actually overrides; crossing bytes priced on the
first host only, with unpriced links marked in the summary; NullCore
rebases its census at ROI; in-order deferred path counts mix/FP for the
block it simulates; CACTI sleep-tx pointers initialized, inverted
destructor conditions fixed, degenerate-width wakeup no longer +inf, and
the softened compute_gate_area assert says so once on stderr; family
factors deduplicated behind one helper; stale pre-1.11.12 wrapper copy
(src/mcpat_wrapper.*, not in the build) moved to attic/.

Data impact: any 1.11.8-1.11.15 run with pg: true is invalid (PG savings
over-credited); PG-off runs are unaffected. SRAM/STT-MRAM/PCM/ReRAM cells
at subarray..chip placement re-price (off-chip MCPHY removed) -- area and
MC power drop. MPI/co-sim cells re-price their instruction base
(syntheticInstrs now subtracted on ALU PEs). Corpus re-sim (already
gated on this train) covers all three.

## 1.11.15 -- the train audits itself, and fixes what it finds

A 30-agent, 5-round adversarial audit of everything 1.11.2-1.11.14 shipped
returned 200 findings (14 blockers). The blockers, all fixed here:

POWER GATING WAS STILL DEAD END TO END. CACTI's error_checking()
unconditionally overwrote the power_gating enable with five sub-flags
nothing ever sets, so the enable emitted since 1.11.8 never survived into
any array: every gated endpoint was zero (an ideal 100%-savings model on
caches), and on family components the logic-priced gated value exceeded
the family-priced active one, so the clamp silently pinned savings to
ZERO. The 1118 gate misread the byte-identical areas as "area model does
not engage" -- they were proof the enable never arrived. Fixed: the flag
is honored (OR, not overwrite); applyFam scales the gated endpoints with
the same family transform; the clamp prints the inversion it used to
hide; setPGSpec now also fires in system scope.

THE MIX CENSUS WAS DEAD ON THE DEFAULT ELEMENT. Decode was gated on an
OOO/InOrder core existing, so alu_core -- the corpus default -- never
filled the census: the counted mix silently reverted to fractions and the
FP-without-FPU report could never fire, on exactly the cells 1.11.10 and
1.11.11 were built for. Decode is now always on (PIMID_NODECODE escape
hatch); ALU timing is untouched (it consumes instrs and bytes, which the
decoded block carries identically). The census also gains its BRANCH
class end-to-end, replacing the conditional-only core counter, and the
consistency check becomes symmetric: a >5% classified deficit rejects the
measurement loudly, a smaller one is priced as integer and printed --
nothing is silently charged zero execution energy any more.

CROSSING ENERGY OVER-CHARGED THREE WAYS. The coherence flush -- 99.999%
of counted bytes -- is a host writeback to MEMORY and is now charged as
line writebacks on the host-visible array, not as PCIe traffic. The link
is priced ONCE (host end carries the bytes; the device end keeps its
controller with zero transfer) instead of once per endpoint. And McPAT's
MCPHY -- a per-bit off-chip I/O driver on the same accesses the
termination term prices -- is no longer built for on-die element MCs,
which drive no DQ pins (type=1 embedded, no PHY).

THE LINK VOCABULARY NO LONGER KILLS FINISHED RUNS. cxl_2_0/cxl_3_0,
nvlink_3_0/4_0/c2c and interposer now resolve by family; unknown types
warn and price the transfer at zero instead of exiting after the
simulation completed; the override sentinel accepts 0.0 (an interposer
user can say "free").

Also: fam_core_power_ratio_ lost its assignment in 1.11.12, so the
CoreBreakdown printed logic-priced blocks under a family-priced total --
the blocks are now transformed directly (dynamic x fd, leakage x fl).
And the PE-MI locality counters were parsed at ROOT scope while zsim
emits them under pe-mi (MEM scope): always zero, report never printed.
Moved; a run with 698,189 remote accesses on disk now reports them.

Gate 1125 (mi100): ALU timing bit-equal under decode-always (10,164,688
both); census alive on the ALU cell (mixInt 172,842 where 1.11.14
measured zero); PG leakage 5.8e-6 -> 1.2e-6 W at 93% idle, and the
arithmetic back-solves to a gated endpoint of 8.7e-7 W -- a 15%
sleep-transistor residual, i.e. a REAL tool-computed endpoint where the
audit proved a 0% ideal; sleep-tx area now engages; no inversion
warnings; repeats to the digit. Two more never-executed-code faults were
flushed out by the resurrected enable: CACTI's Sleep_tx asserted on
degenerate cells (the 1-entry pure-RAM istore is too narrow to fold a
transistor into) -- both sites now degrade to a zero-area sleep network
in CACTI's own early-return style instead of killing the McPAT child.

DATA IMPACT: broad and intended -- PG becomes real, the counted mix
reaches the default element, crossing energy stops triple-counting.
The remaining 101 defects and 34 risks are triaged in
_1115audit/DIGEST.md (local) for the checkpoint discussion.

## 1.11.14 -- the calibration moves into the tool it calibrates

The border cleanup. The JEDEC k-calibration (1.11.1) was computed by the
caller: PIMID held the vendor-density table, the generation map and the
arithmetic, and applied them to CACTI's output at two separate call sites.
That is model logic in the orchestrator, which the borders rule forbids and
which had already produced one duplicated code path (1.11.9's system-scope
helper). Density, generation map and calibration now live inside the CACTI
fork; both call sites describe the array and ask
CACTIWrapper::getCalibratedDieArea().

The scope gate is the substance, not the packaging. McPAT links this SAME
cacti7 library and issues thousands of cache, register-file and TLB queries
through it -- a DRAM vendor-density factor reaching those would be a
category error with no symptom until someone checked an L2 area. So
calibration requires BOTH a commodity-DRAM main-memory query AND a named
technology; every other query returns raw CACTI, byte-identical to before.
The gate proves it with an SRAM cell rather than asserting it.

Two runs still happen for a calibrated die, and that is the model rather
than an artefact: k is defined against the PRESET organisation while the
reported die is the EFFECTIVE one, so reconfiguring banks moves the area by
CACTI's structural derivative around a vendor-anchored point.

Gate 1124 (mi100): HBM3 die line character-identical (40.78 mm^2/die,
k=0.054, raw CACTI 755.76) and DDR5 likewise (13.90, k=0.538) -- the
migration is numerically inert across technologies with very different k.
The leak arm: non-DRAM area identical (13 mm^2 both) and a DETERMINISTIC
1-PE SRAM cell identical in power and area (0.4 W, 7.1 mm^2). The
16-PE SRAM cell's power is not reproducible across jobs for the SAME
binary -- 3.2 W in gate 1123, 3.3 W here, unchanged 1.11.13 both times --
so that arm now asserts area identity plus a deterministic power cell,
which is what can actually be asserted.

DATA IMPACT: none. Same model, same numbers, computed where it belongs.

## 1.11.13 -- corners exist where the tables have them, and nowhere else

#121 asked for a per-technology corner axis: a speed corner for GDDR6 and
HBM, a mobile corner for LPDDR5 and the NVMs, derived from CACTI's own
device columns and activated where the 1.9.10 IDD data showed one corner
failing. Reading the tables answered the design question before the IDD
check could: each CACTI table carries exactly ONE commodity-DRAM device
column. lp-dram, the only alternative, is all-zero at 22 nm. A speed
corner for HBM periphery cannot be derived from a table that does not
contain one, so it is not derived -- the request is refused there, with
the reason printed, rather than approximated into existence.

Where corners DO exist is the logic family: CACTI carries hp, lstp and
lop, and McPAT already selects between them through sys.device_type. That
choice was simply never exposed, so every logic domain in every run so far
was priced hp without saying so. power.device_corner now exposes it
(hp/lstp/lop, validated, default hp = every existing run bit-identical),
which is what a host or a base-die design actually needs.

The area-factor uncertainty band is printed beside the factor in use:
2.44x is the linear l_phy ratio, the conservative end of a band whose
other end is its square (5.95x), with the UPMEM die as the only silicon
anchor between them. A reader can now see the width of the claim without
reading the source.

DATA IMPACT: none at defaults. Selecting a non-hp corner changes
logic-family components only.

## 1.11.12 -- the DRAM-periphery family becomes a property of the machine

#120 and the model-borders migration together, because they are the same
change. Since 1.11.2 the periphery transform lived in the wrapper, scaling
McPAT's CORE result after the fact -- which meant the on-die fabric and the
element controllers, silicon on the same die as the PEs, kept being priced
in a logic process, and the transform sat outside the tool that owns the
components. Both are now one thing: McPAT's XML carries a DRAM_PERIPHERY
device family (dram_periph_family/area/dyn/leak/scope), and McPAT applies
it internally to the components the scope names -- PE cores, their caches,
the on-die NoC and the element controllers -- then rebuilds its own totals
from the transformed parts. The wrapper describes the family and reads
whatever McPAT produced; it no longer post-scales anything.

Two consequences beyond tidiness. The aggregate and the parts can no longer
drift apart, which is the failure 1.11.0 found in the NoC census and 1.11.4
found in the CoreBreakdown split. And the power-gating endpoints from
1.11.8 arrive already family-priced, active and gated alike, so the
interpolation no longer needs to know about process families at all.

Scope is placement-derived, not global: rank/channel and base-die designs
put their logic on a buffer or base die and stay in the logic family.

Two things the gate caught before this shipped, both worth stating because
they are the release's own subject matter. The per-level NoC objects the
reporting layer reads were not transformed with the aggregate, so the
breakdown printed logic-process leakage under a periphery-priced total --
the exact aggregate-versus-parts drift this change exists to end, appearing
inside it. And the AREA factor was being applied to the fabric, which is a
category error: the pitch penalty is a TRANSISTOR claim, while the 1.10.5
census showed most tree nodes are single-child pass-throughs, i.e. wire,
whose pitch on a DRAM die does not follow the device. Applying it anyway
put a 16-PE add-on at 36 mm^2 against its own 40.78 mm^2 HBM3 die -- 88% of
the memory it is embedded in. Device factors (dynamic, leakage) now apply
to everything on the die; the pitch factor applies only where area is
transistor-dominated. The add-on lands at 28 mm^2, 69% of the die.

DATA IMPACT: device NoC, MC and cache power/area for subarray/bank/BG/chip
placements on DRAM technologies -- previously logic-priced. PE cores land
where they did. Timing untouched.

## 1.11.11 -- an element without an FP unit stops running FP for free

#113. pim.pe.floating_point=false removed the floating-point unit from the
POWER description and nowhere else, and the code said why: "the timing
model charges every instruction identically -- it never sees an opcode".
As of 1.11.10 it does. The decoder's class census travels with each basic
block, so an FPU-less element can now be told what it just executed.

Two things follow. The run REPORTS the contradiction with its measured
size -- "N FP-class instructions executed on an element declared WITHOUT
an FP unit" -- which no configuration could previously discover. And the
emulation becomes chargeable: pim.pe.fp_emulation_cycles adds that many
cycles per FP-class instruction in the timing model (through
sys.hierarchy.fpEmulCycles / peHasFpu into zsim). The default is 0 --
nothing charged, every existing run bit-identical -- because the honest
per-op cost of soft-float depends on the operation and the width (glibc
soft-fp is tens of integer operations), and inventing one constant for
all of them is the failure mode this train exists to remove. The knob is
offered, documented, and left to the user; what is NOT left to the user
is knowing that the situation occurred.

DATA IMPACT: none by default. With the knob set, timing and everything
derived from it move on FPU-less elements that execute FP code.

## 1.11.10 -- the instruction mix is counted, not assumed

#112. Every instruction the decoder handles already carries a class
(x86_decoder.h OpClass: ALU, MOV, LEA, IMUL, IDIV, FADD, FMUL, FDIV, FMA,
VECALU, VECMOV, BRANCH), and every one of those classifications was thrown
away the moment the uops were emitted. The class census is now recorded per
basic block at decode, accumulated per core at retirement (all five core
types, ROI-rebased alongside instrs), exported through zsim's stats tree and
parsed like every other activity counter -- and McPAT is fed the COUNTED
int/mul/fp split instead of the documented 87.5/12.5 stand-in that had been
holding its place since 1.9.28. Load and store uop counts ride along on the
same path.

The stand-in remains the fallback: a core model that never decodes (or a
class that measures zero) keeps the fractions, so nothing regresses where
there is nothing to count. And if the classified count ever exceeds the
retired non-branch count -- the two counters disagreeing on base, the defect
class 1.11.9 root-caused -- the fractions are kept and the disagreement is
printed rather than papered over.

This is what 1.11.9 unblocked: the mix-consistency gate had been rejecting
the measured set because instrs was latched from one core while the activity
counters were all-core sums. With both on the same base, the measurement is
usable for the first time.

Measured on a 16-PE in-order HBM3 cell: 152,799 integer, 113 multiply and
263,787 FP-class instructions of 487,424 retired -- about 63% FP where the
stand-in assumed 87.5% INTEGER. The assumption was not merely imprecise for
this kernel; it was inverted.

DATA IMPACT: core dynamic power wherever the real mix differs from
87.5/12.5 int/fp -- which is everywhere with FP content. Timing untouched:
the deterministic 1-PE cell is bit-equal, and the 16-PE cell's 4% gate
difference was shown to be the cell's own variance, not the release's --
the UNCHANGED 1.11.9 binary varies 6.11% run-to-run on that cell (the two
binaries' first runs agree to 0.03%). Bit-equality arms belong on
deterministic cells; the 1.9.41 finding, re-confirmed.

## 1.11.9 -- co-simulation reports what it measured

#86 and its audit sheet (six blockers). (1) System scope HARD-ABORTED on a
decoupled co-simulation -- host DDR5 plus device HBM3, the standard PIM
configuration -- because a 1.9.42 guard insisted the system had exactly one
memory. Each node's memory is now charged with ITS OWN technology and ITS
OWN counters, which is what the timing side has done since 1.1.0; a shared
memory (device IS the host's memory) names one technology and is priced
once, so those runs are unchanged. (2) The parser accumulated across EVERY
stats dump in zsim.out: zsim appends a full monotonic dump per write, so a
run with a periodic dump plus the final one reported double its accesses.
Only the final dump is read now, and multi-dump files announce themselves.
(3) cCycles was left absolute while cycles was ROI-windowed, and
host_wall_cycles summed the two -- cCycles is now rebased on the same
window. (4) The memory die's AREA never appeared in system scope, so a
co-simulated system's total silicon omitted its largest piece; it is
computed with the same JEDEC-calibrated CACTI model as device scope and
summed in. (5) The PE-MI locality split has been emitted since 1.5.3 and
read by nobody -- the one measurement that says whether a placement kept
its accesses local; it is now parsed and reported. (6) HOST_MC placement
produces no device memory group BY CONSTRUCTION (the elements sit at the
host controller); the report says so instead of printing a bare zero.

And the root cause of a mystery the code itself documented as "not yet
understood" (1.9.28): the parser latched instrs from the FIRST core while
uops, branches and syntheticInstrs were all-core sums. Measured on a 16-PE
HBM3 cell, that is 51,660 against a true 512,788 -- McPAT, which divides by
the core count, modelled every PE as doing a tenth of its work, and the
mix-consistency gate then rejected the measured instruction mix and fell
back to documented fractions. instrs is now summed like every other
activity counter. Cycles remain first-core: a duration, not work.

DATA IMPACT: device-scope core dynamic power rises where instruction-driven
terms dominate (the counter was ~10x low on a 16-PE cell); co-sim runs gain
memory area, per-node memory energy, and locality reporting; any run whose
zsim.out held more than one dump loses the multiplied counts. Timing
untouched.

## 1.11.8 -- power gating: one flag per component, physics per technology

#84, to the spec converged interactively (v7): NO global switch, NO
granularity knob -- the entire config surface is a per-component pg:
true/false at initialization (pim.pe.pg, noc.pg, pim.mc.pg; default
false, so a config with no pg: keys is bit-identical to 1.11.7, enforced
at the XML level). Residency is measured per component where its events
happen (active-phase marking: cores at retirement, shared caches at
access, the fabric at injection, MCs at request -- one compare+branch
per event, exported through zsim's stats tree), and each gated
component's effective leakage is the tool-certified interpolation
leak_eff = active*(1-r) + gated*r: the gated endpoint is McPAT/CACTI's
own sleep-transistor power_gated_leakage (dead plumbing repaired -- the
enable never reached CACTI before), DRAM descends its JEDEC ladder
(IDD2P precharge power-down during MC no-traffic residency, refresh
always on, tXP hysteresis stated as a 0.99 derate), and NVM periphery
gates retention-free to a 2% sleep-transistor floor (non-volatile cells
hold state unpowered -- the one place PG is free). Penalty accounting,
verified empirically at the gate rather than assumed: the sleep-
transistor LEAKAGE endpoints respond to the enable (gated != active !=
zero), but CACTI-P's sleep-transistor AREA overhead does NOT engage in
the vendored path -- pg-on and pg-off areas are byte-identical -- so
area overhead joins wake latency and entry/exit energy on the stated
unmodelled list (all bounded, all printed; revisit post-v2). The report prints per-component
active/gated/residency/effective plus the all-idle shared-domain
comparison line.

DATA IMPACT: none without pg: keys (bit-identical); with them, leakage
and DRAM background only. Timing untouched in all cases.

## 1.11.7 -- crossings are counted, and the link is priced from them

#85, co-sim half -- the audit's largest sheet (10 blockers). The plugin now
COUNTS every host<->device crossing at the moment it happens, bytes in hand:
WORK_BEGIN/END payloads, launch cmd/ack packets, and the coherence-flush
footprint, exported through zsim's stats tree (xingH2DBytes/xingD2HBytes/
xingCount/xingFlushBytes in zsim.out) and parsed latch-last (robust to
multi-dump files). McPAT's PCIe component -- kept per the model-borders rule
and made real: the fork's iocontrollers gains transferred_bytes +
link_pj_per_bit inputs, and runtime link dynamic is computed FROM the
transfer (bytes x 8 x pJ/bit, exact through the Processor's units x
clockRate aggregation; zero traffic = zero dynamic natively). Per-link-type
pJ/bit table (pcie_gen3/4/5, cxl = gen5 PHY + coherence delta, nvlink;
unknown types fatal unless power.pcie.pj_per_bit_override is given,
printed). Both ends of the link now carry a controller (the device end was
never priced); the readout reads the AGGREGATED pcies component (raw-object
read under-reported dynamic by units x clockRate); the link clock comes
from the configured link type, not a hardwired 350 MHz. All of it wired
into runPerNodePowerAnalysis -- the path a real co-simulation executes,
where interface energy was previously unreachable. And the synthetic
timing BBLs (WORK/launch/flush) no longer manufacture phantom instruction
fetch (bytes = 4 x cycles): co-sim timing and cache/DRAM traffic move with
this release BY DESIGN -- that traffic never existed.

DATA IMPACT: all co-sim cells (crossing energy now real; phantom fetch
removed from timing and counters). Device-scope cells untouched.

## 1.11.6 -- audit hotfix 2: CACTI's temperature rows are Kelvin-minus-300

Round 3 of the audit caught the 1.11.4 hotfix's own error: CACTI's I_off
table rows are indexed as (T_kelvin - 300) -- parameter.cc:175 compares
thermal_temp against temperature-300 -- so the model's 350 K is row 50,
and the "80C" row 1.11.4 read is actually 380 K. Corrected: 22 nm leakage
factor 6.8e-6 -> 1.0e-5, 32 nm 2.2e-6 -> 3.1e-6 (higher, still
retention-grade). The audit's column-basis objection (comm-dram is CACTI's
cell-access device, not the periphery) is answered in-code rather than
patched around: comm-dram is the only DRAM-process device column populated
in both tables (lp-dram is all-zero at 22 nm), its ~2.4x delay ratio
reproduces the UPMEM DPU band, and the 32 nm lp-dram alternative agrees on
delay while giving a 4.3x area ratio -- so 2.44/2.46 is the conservative
end of CACTI's own DRAM-process band. Stated as a proxy, because no tool
in the chain carries a true DRAM-periphery logic device.

DATA IMPACT: DRAM-periphery-family PE leakage only (factor ~1.5x up from
1.11.4, on a micro-watt base). Timing untouched.

## 1.11.5 -- the DQ pins are charged once, and only when they are crossed

The audit's interface-energy findings, memory side. (1) The 'interface'
term was vdd*(IDD4R-IDD3N)*tBurst -- bit-identical to the DQ burst current
already inside the array read term (JEDEC IDD4R is measured with outputs
driving), so every access was double-charged. interfaceNJ is deleted; the
interface term is now TERMINATION (ODT) -- the genuinely additional
off-chip energy, computed per I/O standard since 1.9.10 and never charged
until now (dead code resurrected). (2) Termination is placement-aware: an
access from an on-die PE (subarray/bank/BG/chip) never crosses the DQ
pins and carries none; RANK/CHANNEL/HOST_MC accesses do. HBM terminates
nothing at any placement (interposer microbumps -- the model's own zero,
by physics). (3) Writes consult IDD4W: the write burst term is
vdd*(IDD4W-IDD3N)*tBurst plus the activate share, replacing read*1.2.
(4) The total-dynamic line prints 'term=', not the false 'pre='.

DATA IMPACT: memory dynamic energy everywhere -- on-die placements drop
(double-charge removed, nothing added), off-die DDR-class placements
exchange the DQ double-charge for real termination, writes move by
IDD4W/IDD4R. Timing untouched.

## 1.11.4 -- audit hotfix: the leakage factor was read at the wrong temperature

The user's Opus audit fleet (five rounds over the open 1.11.x issues) turned
its first round on the 1.11.2/1.11.3 factor harness itself and found five
defects, all fixed here. (1) The leakage factors were derived from the 0C row
of the CACTI tables while McPAT runs at 350 K (~77C); the 80C row raises the
DRAM-periphery leakage ratio ~7-8x (22 nm: 9.0e-7 -> 6.8e-6; 32 nm: 2.7e-7 ->
2.2e-6). Still retention-grade -- but no longer understated. (2) The 22 nm
and 32 nm factors were derived with inconsistent formulas; both now use one
derivation (I_off*Vdd at 80C, cd/hp), stated in the code. (3) The factor was
applied on top of McPAT's longer-channel leakage discount; comm-dram is
already a long-channel device, so leakage is now rebased on the plain value
(no double discount; peak path identical). (4) On-die L2/L3 sit in the same
DRAM-die silicon as the PE but were still logic-priced; they now carry the
same family factors, power and area. (5) The CoreBreakdown block weights were
captured unscaled and printed against a scaled core total; they now carry the
core-power ratio. Plus: leakage fields NaN-guarded in extraction (the peak
path already was), the factor literals live in one place, and a basis
mismatch (device_type != hp with family factors) warns.

DATA IMPACT: DRAM-periphery-family PE leakage (up ~7-8x from a tiny base)
and on-die cache power/area for placements with device caches. Timing
untouched.

## 1.11.3 -- the periphery device is per-generation, and the istore is a RAM

Two refinements to the 1.11.2 process surface. (1) The DRAM-periphery factor
set is now read from the CACTI table the technology's generation class maps
to, not fixed at 22 nm: DDR3-class periphery uses the 32 nm hp/comm-dram
columns (area x2.46, dynamic x0.66, leakage x2.7e-7, delay ~1.5x), everything
newer the 22 nm columns (x2.44 / x0.82 / x9e-7, ~2.4x) -- different DRAM
generations now carry measurably different periphery devices. Gate-0 also
settled the mechanism question: pricing a whole PE natively in CACTI's
comm-dram device (device_type=4) is rejected by CACTI itself (UCA asserts
readOp.dynamic > 0; the retention device with Vth 1.0 V > Vdd 0.9 V only
works inside the DRAM-array machinery where wordline boost exists). The
factor harness is therefore the mechanism, documented as such. (2) #111: a
PE's instruction store is a resident RAM, not a cache. buffer_sizes[0]=0 now
marks it in the fork's InstFetchU: the array is built pure-RAM (no tag) and
the miss/fill/prefetch buffers -- structures a scratchpad does not have --
are not built at all. Previously the store carried a tag array and three
one-entry MSHR-class buffers whose combined overhead rivals a small imem
itself.

DATA IMPACT: device PE power and area wherever the ALU imem is priced (all
alu_core PEs, both families), and DDR3-placement PEs specifically via the
32 nm factor set. Timing untouched.

## 1.11.2 -- the process surface: every domain on its own silicon

Three related fictions removed. (1) The technology-node knob was clamped only
at the bottom: any value >= 22 passed through, so CACTI silently interpolated
unvalidated blends for intermediate nodes, and its 16nm.dat -- a 25-byte stub
reading "Invalid technology nodes" -- was reachable for 16-21 nm requests.
The knob is now a positive-list: 22, 32, 45, 65, 90 nm (the nodes every
linked tool evaluates from real tables), anything else a fatal config error.
(2) The logic node reached the DRAM die: setting a 45 nm host re-priced HBM3
arrays as 45 nm DRAM silicon. The DRAM array now derives its process from the
technology generation (DDR3=3x/2x on the 32 nm table, everything newer on
22 nm; printed), and power.tech_node_nm names logic domains only. (3) Every
PE was priced as logic-process silicon regardless of where it sits. A
placement x technology matrix now selects the process family: subarray/bank/
bank-group/chip PEs on DRAM technologies are DRAM-periphery devices, scaled
by factors read off CACTI's own 22 nm hp vs comm-dram device columns (area
x2.44 from l_phy, dynamic x0.82 from C*V^2, leakage ~0 from I_off -- the
retention-grade device is the physics, and Ramulator2 carries DRAM-die
background power). Rank/channel/LOGIC_DIE/host PEs and all PEs beside
SRAM/NVM arrays remain logic. Cross-check: the x2.4 CV/I delay ratio puts a
1 GHz logic PE at ~420 MHz, inside UPMEM's published 350-466 MHz band; a
warning fires above 700 MHz. Interim factors; 1.11.3 replaces them with a
real McPAT DRAM_PERIPHERY device family. Subarray placements gain
power.subarray_pitch_factor (default unity).

DATA IMPACT: device PE power and area for BANK/BG/CHIP/SUBARRAY placements
on DRAM technologies (most of the corpus); timing untouched. DRAM die area
unchanged at default configs (same 22 nm table as before for post-DDR3
technologies; DDR3 moves to the 32 nm table).

## 1.11.1 -- one array-area model: CACTI calibrated by JEDEC (user design)

Raw CACTI comm-DRAM areas failed physics (four technologies identical, HBM3
past a reticle); the JEDEC density figure is vendor-anchored but structurally
blind -- it cannot respond when a user reconfigures banks or IO. Each now
contributes what it is good at: k = JEDEC(preset org) / CACTI(preset org),
computed at the technology's own Ramulator2 organisation, and the reported
die area is CACTI(effective org) x k. At the stock organisation this is
exactly the vendor-anchored figure; under reconfiguration it moves by CACTI's
structural derivative. k is printed with the raw value, so a calibrated
number can never pass as a raw tool output. Fallback to plain JEDEC density
when CACTI fails.

Measured k across the seven technologies spans 0.05 to 1.2 -- a factor of 24,
the one-line demonstration of why raw CACTI could not be primary.

DATA IMPACT: reported DRAM die area only; power and timing bit-equal
(verified 10164674 cycles unchanged).

## 1.11.0 -- power and area price the tree that was built, at every level

1.10.5 fixed the top-level network inputs; the hierarchical (levels) machinery
was separate plumbing and never got the cure. It sized every tier from the raw
organisation count -- about 528 endpoints on a sixteen-element HBM3
configuration -- and so priced a 23x23 bus at the bank level and a 12x12 NoC at
the bank-group level: hundreds of routers, 53 of the device's 63 mm^2 and 1.3 W
of leakage, for a tree that actually built ONE branch router.

The tree now reports a per-level census of itself (branch routers and attached
endpoints per tier, derived from the builder's own bookkeeping), and the levels
machinery consumes it. A tier with no arbitration and no endpoints is wire and
is skipped, not priced. Non-DRAM configurations, which build no tree, keep the
old estimator. Measured on 16-element HBM3: NoC area 53.4 -> 5.3 mm^2, device
total 62.9 -> 14.8 mm^2, device leakage 1.8 -> 0.44 W. Timing bit-equal.

Also: the system-scope report now prints per-node area (host socket vs device
add-on) instead of one aggregate; CACTI's DRAM path no longer fails silently on
wide interfaces (block sized from the interface, the relation CACTI enforces);
and CACTI's comm-DRAM die areas are diagnostic only -- cross-checked against
JEDEC density figures they came back nonphysical (four techs identical at
20.9 mm^2, HBM3 at 756 mm^2, beyond a reticle), so the vendor-anchored density
path remains authoritative.

DATA IMPACT: device-scope power AND area for every DRAM configuration with the
detailed network. The phantom-router leakage was ~35% of device power on the
measured configuration.

## 1.10.6 -- the shared channel data bus pays to reverse direction

A DRAM channel's DQ bus is one road: reads and writes take turns, and turning
costs tWTR. A bandwidth-limited link knows the road's width but not the cost
of turning, so mixed read/write streams rode free. The expected penalty --
2 x P(read) x P(write) x tWTR, zero for any single-direction stream, exactly
as the bus behaves -- is now charged per access and lengthens the service
time the channel queue sees. tWTR is each technology's own, from the
Ramulator2 preset the run selects (DDR3/DDR4 7.5 ns, DDR5 10, LPDDR5 12.5,
GDDR6 6.27, HBM2 8.33, HBM3 8.11); the direction mix is measured from the
run's own traffic. Read-to-write is charged at the write-to-read figure, a
stated approximation. memory.dq_turnaround: false disables it, for designs
whose PIM interconnect is not a shared bus.

Two of this fix's own defects were caught by its gate before shipping: the
charge first sat inside the queueing term, where utilisation ~0.02 multiplied
it to zero; and the OpenMP path carried an un-rerouted duplicate of the
pricing formula, so the charge reached only the MPI path. The duplicate is
deleted -- both runtimes now price from one function.

DATA IMPACT: device-scope timing for every DRAM configuration with a mixed
read/write stream. Measured: stream_triad (2.4:1 mix) on HBM3, 1 element,
+4.1% cycles, deterministic. Single-direction streams and dq_turnaround:false
are bit-exact with 1.10.5.

## 1.10.5 -- power describes the fabric the timing model routes on

McPAT was handed one router per element on a ceil(sqrt(elements)) square grid,
and controllers by element grouping. The device contains neither: it has the
placement tree -- sparse, element x depth routers, most of them single-child
pass-throughs that are wire rather than arbitration -- and one
controller-worthy endpoint per region, aggregated regions included, because
memory that only responds still needs sequencing and refresh.

Power now takes the router count from the built tree and charges only branch
routers; pass-throughs are carried by their links. Controllers are counted per
tree endpoint. Sixteen bank-placed elements on HBM3 build one branch router and
fifty pass-throughs -- the square mesh billed sixteen routers for that
configuration, and billed the same fabric for every placement level, which is
the defect: placement changed the machine and power could not see it.

DATA IMPACT: device-scope power for every DRAM configuration with the detailed
network. Direction and size vary by placement (deep trees shed invented
routers; measured: 16-PE BANK/HBM3 3.9 W -> 3.8 W, CHIP unchanged at 3.0 W).
Timing is untouched -- verified bit-equal on the deterministic single-element
configuration.

## 1.10.4 -- the repository states the version it actually is

The README badge still read 1.10.0 while the binary reported 1.10.3, and the
changelog stopped at 1.10.0 -- so three shipped releases were absent from the
documented history and the public landing page advertised a version that had
been superseded twice. Same fault as the hardcoded --version string 1.10.2
fixed: a version written by hand, tracking nothing.

The badge and the changelog are now part of the release, and publish-public.sh
refuses to publish when the badge and the project version disagree, so this
cannot drift again unnoticed. The ledger is renamed changelog.md, since it long
ago stopped being about the 1.8-1.9 train.

## 1.10.3 -- the in-memory fabric is described as the memory is built

The default DRAM fabric was four virtual channels with four-deep buffers, which
describes a packet-switched router a DRAM die does not contain. It is now the
smallest description the routing needs: one UP and one DOWN channel, buffers of
two. Two rather than one, because the tree routing stays deadlock-free only by
keeping the two directions in separate VC classes; a request for one is warned
and lifted.

A link's width now follows the tier it crosses rather than its distance from
the leaf. Choosing by distance made the width depend on how deep the tree
happened to be, so the channel link -- the narrowest in the device -- was priced
with the width of a fat inner datapath whenever the placement was coarse, in the
direction that makes memory look faster than it is.

And noc.levels[<tier>].link_width_bits now reaches the detailed DRAM tree. It
was parsed and then ignored on the default path, so a user modelling a widened
channel silently received the stock part's numbers. A run under an override
says so in the output.

DATA IMPACT: invalidates device-scope network timing for every DRAM technology.
The direction is more cycles (narrower upper links, less buffering); the
magnitude is not established -- both execution models vary more run-to-run
(16% and 20% measured) than the effect -- and belongs to the corpus
re-simulation.

## 1.10.2 -- the repository carries sources, and says which version it is

Two compiled CACTI executables (2.5 MB each) and two upstream PDFs (3.6 MB) were
committed into a source repository; the build referenced none of them. And
--version had answered 1.8.0 since that release, because the string was written
by hand with nothing tying it to the project version. It now comes from
PROJECT_VERSION through a compile definition.

DATA IMPACT: none.

## 1.10.1 -- two configurations the model was letting pass in silence

Placing elements below the rank asks that rank for more concurrent row
activations than the JEDEC four-activate (tFAW) window permits. PIMID does not
refuse -- widening the window is a legitimate design -- but the timing model
does not enforce tFAW, so such a run is optimistic by whatever the window would
have serialised, and now says so.

Requesting a mesh, ring or crossbar on a DRAM device was already overridden to
the hierarchical tree, correctly, and silently. The override is now reported,
naming both the requested fabric and the one used.

DATA IMPACT: none. Both are print-only; single-element runs are bit-exact
against 1.10.0.

## 1.10.0 -- the tree now knows where a channel begins

The placement tree collapses regions with no processing element into a single
aggregated endpoint. That is the right idea: memory nobody computes in does not
need its own network interface, because an interface costs what its link costs
and not what sits behind it.

But nothing stopped the collapse from crossing a channel boundary, and on sparse
placements it did.

### What was wrong

Measured on a single-element stacked-memory configuration: one endpoint stood for
four hundred and eighty organisations. That is fifteen entire channels behind one
interface.

Channels are independent. Fifteen of them have fifteen data buses working in
parallel, and collapsing them into one destination throws that parallelism away.
Worse, the path out to each of them is below the network's endpoint and outside
the memory model, which only covers within a channel -- so nothing priced it at
all.

It stayed invisible because of a naming accident. The hierarchy's level names are
shaped after conventional memory: subarray, bank, bank group, chip, rank,
channel, system. A stacked part has no separate chip dimension, so its channel
count is stored in the chip slot. The aggregation was therefore happening at a
level called "chip", which reads as safely below "channel" while in fact BEING
the channel. The same folding produced a sixteen-fold coverage error earlier in
this release, wearing the same disguise.

### What changed

The channel tier is now always real: every channel gets its own router whether or
not a processing element lives in it. Aggregation then happens strictly within a
channel, where the memory model's coverage holds, and an empty channel gets its
own endpoint -- which is what it physically is, rather than a fifteenth of one
thing.

Which tier carries the channel is detected, not assumed: a technology that folds
its channels into a lower slot is recognised by the same test that caught the
coverage error, so no technology is special-cased by name and a future part that
folds the same way is handled without further change.

The cost is a handful of routers on sparse placements.

### Also in this release

Aggregated endpoints now record what they stand for -- how many organisations,
and at which tier -- and the tree is checked against the organisation count the
rest of the system uses. A mismatch stops the run rather than warning, because
every access cost and every energy figure is computed against this structure.

That check is deliberately made against the count the rest of the system uses,
and not against a second computation of the tree's own. An earlier version
compared the tree only against itself, agreed, and concealed the coverage error
described above.

### Data impact

Timing changes for sparse placements on technologies that fold their channels --
those configurations previously routed to an endpoint that stood for many
channels at once. Fully-populated placements are unaffected, because every
channel already held a processing element and there was nothing to materialise.
Conventional memory is unaffected at every placement, since it does not fold.

## 1.9.44 -- documentation for the whole train

One pass over the documentation for everything the preceding fifteen releases
changed, rather than an edit per release.

### The power and area reference was twelve lines

It named the tools and stopped. It is now an account of what is priced, by what,
and -- the part that was missing entirely -- **what is assumed rather than
measured**. The instruction mix is a fixed ratio because nothing counts
floating-point or multiply instructions; per-access interface energy is an
inherited constant; power gating has no temporal weighting of its own. Those
belong beside the results, not in a reader's inference.

It also records the reference class and what that choice does NOT claim. Pricing
an element against embedded parts rather than server processors is not an
assertion that in-memory logic is cheap. The opposite penalty -- logic in a memory
process being slower and larger -- is a real and still-missing term, and the
document says so rather than letting the change read as a discount.

And the memory rule: there is one memory, and it is charged once.

### What the element is, and is not

The core-model reference described the element as a minimal arithmetic unit
shaped by scaling factors. It is now described as what it became: a datapath with
a register file, arithmetic units, a result bus and a resident instruction store,
sized by its own parameters.

More useful to a reader: every core model consumes the same host instruction
stream, and the element does not decode. It models no instruction set and cannot
tell a floating-point operation from an integer one. What it does model is the
cost of an operation and the cost of reaching data. The memory side of an element
is modelled in detail and the compute side crudely -- which is defensible, since
processing in memory exists for memory-bound work, but it means conclusions about
compute-bound kernels do not follow from this simulator. That belongs in the
documentation and in any write-up.

### Reproducibility

Given a deterministic instruction stream the simulator is exact. A parallel
workload does not repeat, because the host schedules its threads and the emulator
reflects that faithfully -- the variation is the workload's. Its size is not a
constant and must be measured per study rather than quoted as a property of the
tool. Two consequences are stated: differences smaller than the variation are not
findings, and a regression can be gated bit-exact against a single-threaded run.

### A documented alias that no longer exists

The configuration reference listed two spellings of the element type as valid
that had been retired and are now rejected. A reader following the documentation
would have hit an error. Corrected, with the canonical name leading and the
retired ones named as retired.

### Data impact

None. Documentation only.

## 1.9.43 -- a summary line that had not moved with the rest

The instruction counter includes injected timing charges: when a core runs
without decoded micro-operations it advances itself by adding per-block charges
that are counted as instructions but are not code that ran. Every consumer moved
onto the corrected count in an earlier release -- the power model at both of its
call sites, and the per-node activity report. One printed summary line did not,
and still reported the raw total.

For a host that offloads its work and then waits, that raw total is almost
entirely injected charges, so the summary could claim hundreds of thousands of
instructions where a handful executed.

The line now reports executed instructions and names what it excluded. The
whole-run accessor that returns that figure did not exist -- only the per-group
one did -- which is how a single consumer came to compute the answer differently
from every other. It exists now, so the two cannot drift apart again.

### Data impact

None. Nothing downstream reads that line; it is printed, not consumed. Verified:
power and area unchanged.

Verified also that the change is reached: the first attempt at checking it ran a
configuration whose execution never touches that line, which proved nothing. The
second used one that does.

On the configurations we run today the printed figure is unchanged, because the
element model that reaches this summary never takes the synthetic path and its
injected count is zero. The correction matters for configurations where it is
not.

## 1.9.42 -- the memory was not being charged at all in a co-simulation

A co-simulated system reported no memory array energy. Not for the host, and not
for the processing device either -- although the identical configuration run in
device scope reported it. The system total was cores, caches, interconnect and
memory controllers, with the memory itself absent.

Nothing was missing from the models. The array energy computation lived inside
the device-scope path only, and the per-node path never called it.

### One memory, one charge

The ticket that tracked this said host memory energy was missing and should be
added. Building that literally would have been wrong. This simulator models one
host and one memory: the memory either IS the processing device, or it is a plain
main memory with no processing in it and the run is host-only. Either way there
is one array. In a co-simulation the host's accesses and the elements' accesses
land on the same silicon, so the charge is the sum of them, applied once. A host
term beside a device term would have priced that silicon twice -- the same fault
this release train has been removing elsewhere, introduced by the release meant
to fix a gap.

So the technologies the nodes name are checked rather than assumed. If two ever
disagree about what the memory is, the run stops and says so, because that is a
topology this build does not model and charging one of them while dropping the
other would misprice the system without any sign that it had. Several memories,
devices and hosts is a later extension.

The computation is written as its own small routine rather than moved out of the
device-scope path, so that path is untouched and its results are unchanged by
construction rather than by test.

Interface energy is deliberately not charged here. It is already charged
host-side in device scope, and it is an inherited constant rather than a
measurement, so adding it in a second place would compound an approximation
instead of a measurement.

### Data impact

Co-simulated cells gain a memory array energy term they did not have. Device
scope is bit-identical -- verified, and guaranteed by that path being unmodified.

Non-DRAM technologies in system scope say plainly that their array is not charged
there yet, rather than printing a number shaped like a DRAM one.

## 1.9.41 -- the simulator is deterministic; the workload is not

An investigation that ended somewhere other than where it started.

Repeated runs of one configuration, same binary, disagreed on simulated cycles by
a few percent, and two machines disagreed by considerably more. Total work was
stable throughout -- memory read counts agreed to a fraction of a percent, and
every leakage figure was identical to the digit. So the simulator was performing
the same operations and assigning them different times.

### What it turned out to be

Not the simulator. Running the same configuration against the SERIAL build of the
same kernel produces bit-identical results -- cycles and reads, every digit,
across repeated runs. Given a deterministic instruction stream the simulator is
exact.

The variation comes from the workload. The parallel build runs many threads, the
host kernel decides how they interleave, and the emulator reflects that
faithfully. A real parallel program on real hardware does not repeat its
interleaving either. This is therefore a property to state and bound, not a fault
to repair, and the earlier characterisation of it as a simulator defect was
wrong.

Three explanations were eliminated on the way, recorded so they are not
re-investigated: the emulator's host-derived default thread count is never
reached, because the count is set explicitly from the simulated element count;
the simulator was ALREADY running single-threaded in every measurement taken, so
its own threading was never a candidate; and the guest's thread count comes from
the program itself, not from the machine.

### What is fixed

Four settings that control the guest's parallel runtime were applied only to
workloads that DECLARED themselves parallel. The declaration defaults to serial,
and the reference kernels include parallel binaries that no configuration
declares -- so those workloads took the path the code's own comment had warned
about for as long as it existed.

Without those settings the runtime sizes its thread team from the host machine's
processor count, which under user-mode emulation is the count of the machine the
job happened to land on; it may resize that team mid-run; and its threads spin at
barriers rather than sleeping, so the simulator charges cycles for spinning whose
duration the host kernel decides.

They are now applied to every workload. They are inert for a program with no
parallel runtime, and they are necessary for one that has it, so correctness
should not depend on the user having declared the workload accurately. An
explicit environment entry in the configuration still overrides them.

Documented in the architecture reference -- what each setting prevents, that they
change results, and what they do not fix.

### Data impact

This one MOVES RESULTS. Enabling the settings on a configuration that previously
ran without them changed reported cycles by several percent and memory reads by a
fraction of a percent -- the reads because a runtime free to resize its team
distributes work differently. Numbers produced before and after are not
comparable, and every device-scope cell would need re-simulation to be brought
onto the new basis.

Run-to-run variation on parallel workloads fell by roughly a factor of five. It
does not fall to zero and cannot: the remaining variation is the workload's own,
as the serial comparison shows.

### A check that now exists

Any regression can be validated against the serial build and required to be
bit-exact. Every gate run before this one judged its result through noise that
did not need to be there.

## 1.9.40 -- the two halves must agree about what the element is

The previous release gave the processing element a datapath description. This
makes that description agree with the one the timing model already had, and
where the two cannot yet agree, says so out loud instead of leaving it to be
discovered.

### One width, one name

The element's datapath width already existed. The timing model has read it for
many releases, as the operand width, to charge a bit-serial datapath a step per
bit. The previous release gave the power model a SECOND field for the same
physical quantity and parsed it separately -- so asking for a wider element would
have produced a wide datapath in timing and a narrow one in power, silently.

That is precisely the fault this release train exists to remove, and it was
introduced by the release that removed several instances of it. The second name
is withdrawn; the power model now reads the field the timing model reads.
Configurations naming the withdrawn spelling are refused with a message pointing
at the surviving one, rather than having it ignored.

The width is also validated now. It was previously read only by the bit-serial
cycle charge, which clamps it upward, so a nonsensical value was merely inert; it
now sizes register files and result buses, where the same value would abort the
array model.

### Saying so when the halves cannot agree

Three cases remain where the two halves describe different machines and cannot
yet be reconciled. Each is now stated at the point of use rather than left
silent, because silence is how every defect in this train survived.

A datapath narrower than the power model's granularity. The power model works in
32-bit steps, so an 8- or 16-bit element -- a real in-memory design point the
timing model already supports -- is priced as 32-bit. Refusing it would remove a
capability the timing side has, so it warns and names what is overstated.

Lanes without matching throughput. Declaring lanes widens the arithmetic, the
register file and the result bus in the power model, but the timing model
expresses width through its throughput divider. Declaring one without the other
gives an element that pays for many lanes and runs like one. That is a legitimate
thing to model on purpose, so it warns rather than refuses.

An element declared without floating point. This removes the unit from the power
description only. The timing model never sees an opcode, so it will not charge
the software emulation a real part lacking that unit would need -- meaning the
element would run floating-point kernels at full speed with nothing to run them
on. Honest for an integer kernel, not for a floating-point one, and the model
cannot tell which is coming. It warns, and the underlying gap is tracked.

### Documentation

The three parameters added in the previous release were shipped undocumented.
They are documented now, together with a correction: the width parameter's entry
said it was ignored in the non-bit-serial case, which was true until the previous
release and is no longer.

The configuration reference also now states plainly what the compute unit is.
Every element model consumes the same host instruction stream; the compute unit
does not decode, so it models no instruction set and cannot distinguish a
floating-point operation from an integer one. What it does model is the cost of
an operation and the cost of reaching data. That belongs in the documentation
rather than in the reader's inference.

### Data impact

None. No default changes, no existing configuration changes meaning, and the
warnings do not alter any computed value. Verified: a default configuration is
bit-identical to the previous release and emits none of the new warnings.

## 1.9.39 -- the processing element is composed, not borrowed

The element's power and area came from a description of a server processor with
some fields turned down. This replaces that description with one built from what
the element actually is. Four faults, each independently validated.

### The reference class

McPAT prices a die against one of two measured populations. The default is
server processors: the undifferentiated-core term is a curve fitted to
Niagara, Niagara2, Merom, Penryn, Prescott and Opteron die photographs, the
functional units carry desktop areas and energies, and the wires are top-level
global. The other population is embedded parts, calibrated against ARM designs
and Sandia's parametrized-processor study.

The flag selecting between them was never emitted, so it took its default. Every
processing element in every sweep was priced as a fragment of a server die -- by
omission, not by choice. The undifferentiated term alone, evaluated for a short
element pipeline, exceeded everything the description actually named by orders of
magnitude, and it carried most of the element's power. All of it leakage: that
term has no dynamic component at all, which is why it never appeared in any
activity-driven breakdown.

Device scope now selects the embedded population; host scope stays on the server
one, because the host is a server part. This is not a discount applied to make a
number smaller. It is the other of the two populations the tool was calibrated
against, and it is the one a memory-die element belongs to.

Note what this does NOT claim. Nothing here says in-memory logic is cheap. The
literature is consistent that logic built in a memory process is slower and
larger than the same logic in a logic process, and that penalty is a separate
item, still open. This release removes a term that was wrong; it does not add the
one that is missing.

### An element that runs floating-point kernels had no floating-point unit

The compute unit declared one integer unit, no multiplier, and no
floating-point unit -- while the timing model ran three of the five kernels on
it, every one of them single-precision floating point. The two halves described
different machines: the timing side retired floating-point operations in single
cycles on hardware the power side said did not exist, so their energy was never
charged.

The arithmetic is now composed from the datapath: one of each unit per lane,
because a lane that cannot multiply or cannot do floating point stalls on the
kernels we run and the timing model charges no such stall. An integer-only
element remains available as a configuration choice, since two of the kernels are
integer throughout -- but it is now a choice, not a silent default.

### The instruction store was an uninitialised default

McPAT builds an instruction-fetch unit for every core unconditionally. The
element path emitted no parameters for it, so the store was constructed from the
parser's initialisation routine, which fills the entire configuration vector with
the literal value one. The element's instruction supply was a one-byte, one-line,
one-way cache. Nobody had ever looked at it.

It is now an explicitly sized resident instruction memory, direct-mapped, with no
misses -- the program is resident, so there is no refill path to charge. The size
is a configuration knob, because it is the axis that decides which kernels an
element can run at all: a command-driven in-bank engine and a programmable
near-bank one differ mostly here.

RESIDUAL, named rather than hidden: the store is still built by the cache
constructor, so it carries a tag array and single-entry miss, fill and prefetch
structures a scratchpad would not have. That overstates it. Removing them needs a
pure-RAM instruction store inside the fetch unit, which is the remaining half of
this item and is tracked as such.

### Datapath width

The width parameter, which the tool reads only to size register files, queue
entries and result buses, was emitted as sixty-four for every scope. A 32-bit
element therefore carried 64-bit registers and 64-bit result buses. Device scope
now states the element's own width.

### Configuration

Four knobs describe the element: lanes, element width, floating point, and
instruction-memory size. They existed in the power configuration but nothing ever
set them, so every element was described identically regardless of what was
simulated. They are now read from the configuration file, validated, and refused
when given a value the model would not honour. Every configuration written before
this release omits all four and receives the documented defaults, so no existing
configuration changes meaning.

### Data impact

Every device-scope and co-simulation cell moves. Element power and element area
both fall substantially at all three profiles -- compute unit, in-order and
out-of-order -- and the reduction is larger in area than in power. Direction and
cause are the same in each case: the undifferentiated server-die term is gone.

Host-side results are UNCHANGED, bit-identical in dynamic power, leakage and
area, which is the check that matters most here: the reference class follows the
scope and does not leak across it.

Validated separately: the reference class, the functional-unit composition, the
instruction store, the datapath width, that each new knob reaches the model and
changes the result, that an invalid value is refused rather than accepted, and
that the host half of a co-simulation does not move.

## 1.9.38 -- one out-of-order model, not two

The previous release added a separate out-of-order description for processing
elements, distinct from the host's. This withdraws it.

An out-of-order processing element is hypothetical. No shipping in-memory part
has one: the commercial near-bank processor is deliberately in-order with many
hardware threads, using thread-level parallelism rather than speculation to hide
latency, and the in-bank engine of the stacked-memory part is command-driven wide
arithmetic. There is therefore no silicon against which a distinct device variant
could be calibrated, and separating the two bought a name without a difference --
every speculative parameter still came from the same characterised-core
constants.

For a hypothetical machine, describing it as a well-characterised out-of-order
core is the most defensible reference available, and inventing separate device
parameters would introduce exactly the kind of unanchored constant this release
train exists to remove.

One inaccuracy is recorded rather than papered over: the surviving description
carries an instruction-set decode flag appropriate to the host, which a
processing element would not have. It is left in place because correcting it
alone would not make the rest of the description any more applicable, and noted
so the next reader does not mistake it for a considered choice.

### Data impact

None beyond the preceding release. Verified: an out-of-order device cell is
unchanged from that release, and in-order and compute-unit cells are unchanged
from before it. Note also that only two configurations in the corpus name an
out-of-order processing element at all, so the preceding release's change reaches
two exploration cells rather than any swept figure.

## 1.9.37 -- an out-of-order element was described as in-order

The device-scope power path chose between exactly two descriptions: compute unit,
or in-order. A processing element declared out-of-order fell to the second.

That single choice gates every speculative structure in the generated
description -- machine type, reorder buffer, instruction window, register
renaming, physical register count, load/store ordering, floating-point issue
width, and the reorder and rename activity statistics. An out-of-order element
was therefore described with no reorder buffer, no instruction window and no
renaming, while the timing model simulated all of it.

The per-node path used by co-simulation already selected an out-of-order
description correctly. Only the device-scope path did not, which is why the
defect appeared on device-scope cells and not on co-simulation ones -- and why it
survived: the two paths disagreed and nothing compared them.

Fixed by giving the element its own out-of-order description rather than
borrowing the host's, so the profile name no longer misstates what is being
priced.

### Data impact

Device-scope cells configured with out-of-order elements. The direction is
upward: those cells were previously charged for none of the speculative
machinery they were simulated as executing, so they were under-priced. In-order
and compute-unit cells are unchanged, verified against the preceding release.

## 1.9.36 -- the processing element is a compute unit, and its power is a curve fit

Naming, a parametric description, and a measurement that says why the description
is not yet enough.

### The element is a compute unit

"ALU core" was already inaccurate. Three of the five kernels carry
single-precision floating point and two carry integers, so the element has always
needed a floating-point unit. "Compute unit" is also the vocabulary of the field:
the in-bank engine of a shipped stacked-memory part is a programmable computing
unit. Naming the model after what silicon calls it makes it legible to that
reader.

The distinction that matters is that a compute unit is a DATAPATH, not a
processor: a register file, arithmetic units, a result bus and an instruction
store, with no speculation, no dynamic scheduling and no caches. The power tool
offers exactly two core models, out-of-order and in-order, and BOTH describe
processors. That is why no core model fits this element.

The former spelling remains a supported alias, not a deprecation: it names the
entire configuration corpus, and dropping it would invalidate every cell ever
run. The bare short form is retired, having been used by nothing.

Adding the new spelling exposed a defect in passing: core-type normalisation
exists TWICE, once for device nodes and once for the processing-element section,
and neither calls the other. Adding a spelling to one left the other rejecting
it. Both are updated and the duplication is marked for the configuration
tidy-up; until then they must move together.

### The description is now parametric

The generator branched only on whether the profile was out-of-order, so a compute
unit and an in-order core produced BYTE-IDENTICAL input and the element was
charged for a branch predictor, caches, address translation and a scheduler it
does not have. The description is now its own, and parametric: the register file
scales with lane count, floating-point issue follows whether the element has a
floating-point unit, and the load/store path is request issue without queues or
caches.

### And a measurement that says this is not the lever

Reporting the tool's intra-core split required transporting it out of the
subprocess the power computation runs in, whose output is deliberately discarded.
With that in place the split is measurable for the first time, and it says
something uncomfortable: the modelled blocks are UNDER ONE PERCENT of element
core power, at every profile. The remainder is the tool's "undifferentiated core"
term -- a regression on pipeline depth, fitted to commercial parts three
technology generations older, whose in-order branch DECREASES with depth and
reaches zero at a depth shallower than a modern core. A short datapath pipeline is
therefore evaluated off the low end of that fit, where it is largest.

Declaring an element simpler makes it cost more. That is the opposite of the
intent, and it is why sizing the structures correctly changed nothing measurable:
the parametric description above demonstrates its own insufficiency. Confirmed by
experiment, not argued -- a fourfold reduction in register file and buffers moved
no reported figure.

The consequence is a method result rather than a number: an element's power
cannot be corrected by describing the element better, because the dominant term
does not read the description. It has to be composed from the tool's primitives,
which carry no undifferentiated term. Recorded rather than hidden, and the
reported split now states its own denominators so the share cannot be misread as
a removable fraction.

### Data impact

None. The naming is an alias, the parametric description moved no reported
figure, and the split is diagnostic output that nothing consumes.

## 1.9.35 -- a control that could never be set, and a dimension counted twice

Two faults in the in-memory hierarchy description, neither of which changes any
result today and both of which would have, silently, the moment the tree grew.

### A control wired end to end with no way to reach it

The number of ranks per channel is declared in the configuration structure,
written into the generated simulator configuration, and read by the execution
plugin, the trace driver and the analytical hierarchy model. It has no key in any
configuration file. It could therefore never hold anything but its default of
one, so the rank tier of every tree ever built has been a router with exactly one
child -- a pass-through latency hop -- while the surrounding code reads as though
multiple ranks were supported.

The plumbing was complete except for its first link. That link is now in place.
The default is unchanged, so no existing configuration moves.

### The channel dimension booked twice

For stacked memory the chips-per-rank figure is set to the number of channels in
the stack, as its own comment says. The channel count is ALSO supplied separately,
as the fanout at the root of the tree and as the concurrency multiplier on link
widths. The same physical dimension therefore appears in two places.

Nothing multiplies them today. The rank tier is degenerate, and the organisation
size is computed without a channel factor, so every processing element resolves
to the first channel of the first rank and the duplication cancels. It stops
cancelling the instant either tier is given real fanout, and the result would be
a silent eight- or sixteen-fold inflation of the tree -- in the direction that
makes the fabric look larger and costlier than it is.

Rather than restructure the tree, which belongs with the interconnect fidelity
work, the combination is refused: asking for more than one rank per channel on a
stacked technology now fails, naming the inflation factor and what would have to
change first. A latent contradiction that cancels by accident is not a safe thing
to leave for a later reader to rediscover.

### A comment that described something the code does not build

The generated topology was documented as encoding parallel channel subtrees. It
does not. With the rank count fixed at one and no channel factor in the
organisation size, both the channel and the rank routers have a single child. The
channel count is used only as the root fanout in the abstract-endpoint test and as
the link-width multiplier. The comment is corrected, because a false description
is what would let the duplication above read as deliberate.

### Data impact

None. Verified: with a technology and configuration unchanged, hop counts and
reported power are identical either side of this release, measured against a
reference rebuilt from the immediately preceding release including its execution
plugin. The new control defaults to the value that was previously hard-wired, and
the refusal fires only on a combination that was never expressible before.

## 1.9.34 -- interconnect distances were measured between the wrong nodes

The custom-topology hop count walked a router adjacency using endpoint
identifiers. Those are two different numbering spaces, and nothing translated
between them, because the topology reader discarded the lines that carry the
mapping.

The topology file describes three things: how many routers exist, how many
endpoints exist, and two kinds of link -- internal links between routers, and
external links attaching an endpoint to the router it hangs off. The reader
consumed the counts and the internal links and ignored the external ones. No
endpoint-to-router mapping therefore existed anywhere in the simulator, and the
shortest-path search was handed endpoint identifiers to index a structure keyed
by router identifier.

The bounds check in front of that search could not have caught it, and this is
the part worth recording. In a sparse tree the endpoints are FEWER than the
routers, so every endpoint identifier satisfies a test written to reject values
too large for the router array. The guard passed, the search ran between two
unrelated routers, and it returned a plausible distance rather than falling back
to the safe default. A limit that fires only when the wrong identifier space
happens to be the larger one is not a check.

Fixed by keeping the external links as an endpoint-to-router map and translating
both arguments before the search. Identifiers with no mapping still fall back.

Measured on a device-scope cell: packet count and device read count are
IDENTICAL either side of the change, and only the distance credited to each
packet moves. That is the signature the fix should have -- the traffic is the
same traffic, previously walked along the wrong paths. Total hops rise by about
a third, and average hops per packet go from roughly three to roughly four,
which is the physically sensible figure for a tree of this depth; three was too
short for the hierarchy actually built. Interconnect power rises with it, so the
fabric had been under-charged.

### Data impact

Interconnect power and energy wherever the custom topology is used, which is
every technology that builds the placement-driven tree. Hop counts feed the power
model directly, and they were wrong in the optimistic direction. Timing is
unaffected and was verified so: hop counts are a statistics and power quantity,
not a scheduling input, and the packet and access counts are unchanged.

## 1.9.33 -- results that were discarded, never emitted, or guessed at

Eight defects, all of one family. In each case the simulator either had the answer
and threw it away, or did not have it and supplied something plausible instead of
saying so. None of them announced itself; every one produced a number that looked
like a measurement.

### Area was computed, transported, and then discarded

Every reported area was zero, in both scopes, for as long as the artifact tree
records. Not a units error -- that was a different defect, fixed earlier, and the
conversion here is correct. The power computation is run in a forked child for
crash isolation. The child computes the areas and returns them through the result
blob; the parent caches them. But the two accessors still tested the child's
processor object, which in the parent is permanently null, so they returned zero
and the cached values were never read. Every other cached field is read without
that guard, which is exactly why power survived and only area was lost. Both
accessors now test whether the computation completed.

The number this reveals is NOT yet trustworthy, and is documented as such: it is
far larger than the modelled part can plausibly be, for reasons already recorded
as separate defects -- an ALU processing element is priced as a complete in-order
core, and the interconnect is priced over the full organisational tree rather than
the sparse one actually simulated. Area is therefore useful right now as a
DIAGNOSTIC for those two, and must not be quoted as a result until they are fixed.
What the fix does establish is that the machinery is sound: area scales with the
square of the technology node, as it must.

### The device's memory interfaces were never emitted

In co-simulation the processing-element memory interfaces are constructed, wired,
and incremented on the live path -- and then dropped from the list that the sole
statistics-registration loop walks, because that list is cleared so the host's own
controllers can occupy it. They survived only in a side vector kept for wiring. No
device-side memory group therefore appeared in a co-simulation dump at all. The
power model priced the device at zero memory-controller activity while charging
the host's controller for traffic that was partly the device's.

They are now registered from that side vector, in their own aggregate: the
statistics backend requires an aggregate whose children are all the same type, and
the host's controllers already occupy the existing one. This is the largest
numerical correction in the release by a wide margin -- the device's controller
traffic had been priced at nothing.

Note that an earlier release fixed only the READER half of this same problem,
teaching the parser the name the emitter actually uses. Both halves were needed
and only one had landed.

### A node that did nothing was treated as a node we knew nothing about

The per-node power path fell back to a core-count-proportional guess whenever a
node reported no activity. But "this node performed no work" and "we have no
measurements for this node" are different statements, and only the second warrants
a guess. A co-simulation host executes no code during the offload window -- it
prepared its data beforehand and is waiting -- so its measured activity is
legitimately zero, and the fallback then invented instructions for a core that
provably executed none. The test now asks whether the node was OBSERVED, not
whether it was busy. The fallback remains for what it was meant for: a statistics
file with no per-node breakdown at all.

### Substitutions that were made silently

Three more of the same shape, each now stated rather than assumed.

A requested technology node below the floor of the linked models was raised to the
floor without comment, so a study sweeping finer nodes would have received
identical numbers at every point and read as "technology does not matter here".
The substitution is still made -- there is no model to fall back to -- but it now
names the value that was ignored. Note the floor applies to the consumers of those
models: logic, static memory, non-volatile arrays, caches, the interconnect and the
memory controller. Array energy for dynamic memory is keyed on the technology name
through its own per-standard tables and does not read the node at all, which is
correct: those generations are not named in logic-process terms.

A destination-set entry beyond the addressable limit was DROPPED in silence, which
would simulate a fabric quietly missing those endpoints. It now refuses.

A configuration key that reads as a choice of memory-controller model accepted any
value while exactly one implementation exists, the alternatives having been merged
some releases ago. It now says the value had no effect.

### A technology we do not support would have been priced as one we do

The classification of a memory technology into "dynamic memory, priced by the
per-command model" or "everything else, priced by the cell-level model" was written
by EXCLUSION at three sites: anything that was not one of four named non-dynamic
technologies became dynamic memory. Any unrecognised string -- a typo, or a
technology deliberately not supported -- therefore passed through and was priced
with dynamic-memory command tables. A fourth site used the opposite, inclusive
form, so one string could be classified differently at different points in a single
run.

An earlier defect was this same failure for a letter-case mismatch, and was closed
by normalising case without removing the exclusion logic that permitted it. The
normaliser -- the single point every technology string passes through -- now
validates against the supported set and refuses an unknown one, naming what is
accepted. The three exclusion tests are left alone deliberately: each is correct
for canonical input, and validating at the entry changes no existing
classification.

Relatedly, a wrapper header advertised support for three technologies that no
configuration string can reach. The claim is corrected; those remain deliberately
unexposed.

### What this does not establish

Two of these corrections make a previously invisible quantity visible, and
visibility is not accuracy. The area figure in particular is now reportable and
still wrong, for causes recorded elsewhere. The device memory term is newly priced
rather than newly verified.

### Data impact

Device-side memory-controller power and energy in every co-simulation cell, which
had been zero -- the largest change here, and it moves system totals materially
rather than marginally. Host and device shares shift with it. Any node that
legitimately idled was previously credited with invented activity and is now priced
as idle. Reported area changes from zero to a number in both scopes. Timing is
unaffected: the statistics are read after the simulated process exits, and the
statistics-registration change adds a group without altering any scheduling
decision. Configurations naming an unsupported technology now fail where they
previously produced dynamic-memory numbers; no supported technology changes
classification.

## 1.9.29 -- the power model was fed counts that were absent, misattributed, or on the wrong base

1.9.28 corrected the instruction count that reaches the power model and left everything
else it consumes untouched. That was not a complete fix; it was one of several, and
stopping there made another of them worse. This release finishes the work and withdraws
a claim 1.9.28 should not have made.

### Counters that were never read

The configuration writer names each node's caches after the node, so a host's first-level
data cache is emitted under a node-prefixed name and its per-core instances likewise. The
statistics reader matched scope headers against the bare names only, which a prefixed name
never satisfies. No cache scope was ever entered in system scope, every cache counter
parsed as zero, and all cache dynamic power in every co-simulation cell was priced at zero
activity. The device-scope writer emits bare names, so that path parsed correctly and the
defect stayed invisible there.

The same failure appeared a second time, on the memory side. The device processing-element
memory interface registers under one name and the reader tested for a different one that
nothing emits. In device scope those interfaces are the only memory group present, so read
and write counts parsed as zero and the memory-array energy report showed no dynamic
energy at all -- while the same dump carried the full read and write traffic in the groups
the reader had skipped. Published memory energy was not affected: the analysis path reads
the raw counters out of the log rather than trusting that line.

Both are the shape of the interconnect key mismatch fixed in 1.9.24: a name that changed
on one side of an interface and not the other, producing zeros that read as an idle
component rather than as an error. Three instances in one release train is a pattern, and
a name-contract check between the emitters and the reader is recorded for 1.16.

### Counts that were apportioned rather than measured

Everything the model consumes except the instruction count was still divided between nodes
in proportion to their CORE COUNTS: cache accesses at every level, memory-controller
accesses, and the activity counters 1.9.28 had just begun to read. That is not an
attribution. It is an assumption that every core in the system performed identical work,
which is precisely false for a host driving a device.

The reader now learns each node's name from the core-group headers it already parses,
strips that prefix before matching a cache scope, and accumulates every counter into the
owning node's own set as well as the all-nodes total the device-scope path uses. Each node
is priced from its own set; where a node reports no counters the previous behaviour is
kept, so nothing regresses.

Two consequences are worth stating separately. The device is no longer charged for the
host's memory traffic -- under the old split a many-element device beside a single-core
host received most of the host's controller accesses, while its own memory is priced by
the memory model, so it paid for its own memory through one model and most of the host's
through another. And the host node in the device-scope reporting path is no longer
fabricated: that block runs whenever the scope is system, which is exactly when measured
host counters exist, and it derived all of its inputs from fixed divisors of the DEVICE's
instruction count. A comment promised they would be overridden by real statistics when
available; no such override existed.

### A base mismatch, and a claim withdrawn

1.9.28 said it had replaced an invented instruction mix -- fixed shares integer,
floating-point and branch -- with measured counters. It did replace it. What it replaced
it with was worse, and the release note describing it as an improvement was wrong.

The activity counters and the instruction count were not on the same base. The instruction
count was reported through the region-of-interest window; micro-ops, basic blocks and
mispredicted branches were reported as raw whole-run totals. Every ratio formed between
them divided a region-of-interest numerator by a whole-run denominator, which is why some
dumps reported more basic blocks than instructions -- an impossibility that should have
been caught before the counters were used.

Two further problems sat on top. The out-of-order core ran a branch predictor but exported
only its misses, never a branch total, so the mix was built from the basic-block count on
the reasoning that a block terminates at a control transfer. That holds for a real
multi-instruction block; on this decode path a block averages close to a single
instruction, so the block count is nearly the instruction count and the resulting mix
described almost every instruction as a branch -- in device scope it saturated, leaving no
integer and no floating-point operations at all. Separately, the plugin charges coherence
flush, kernel launch and barrier latency by manufacturing basic blocks whose instruction
field carries a CYCLE COUNT, and those cycles land in the instruction count
indistinguishably from executed code.

Fixed by putting the activity counters on the same window as the instruction count in both
core models; by giving the out-of-order core the branch counter it lacked, incremented
where the core already tests for a branch so the predictor call and its ordering are
untouched; and by tracking the injected timing charges separately so the power path can
subtract them. The reported instruction count keeps its existing definition, because it
also feeds the reported instructions-per-cycle and the cycle-based gates, and changing its
meaning would move published numbers.

After the fix the branch rate and micro-ops per instruction are both physical, on
out-of-order and in-order processing elements alike. In co-simulation the host is shown to
execute essentially nothing during the offload window, nearly all of its former
instruction count having been injected timing charges. That is architecturally correct --
the host prepares its data before the region begins and then waits while the device
computes -- and it had previously been described to the power model as executing that
entire count.

Where the counters cannot be reconciled the measured set is refused rather than repaired.
It is used only when the branch count does not exceed the instruction count and the
micro-op count is not below it; otherwise the documented fractions are used and the output
names the offending values. No scale factor was introduced to force agreement, because a
factor chosen to make a ratio look right is the class of invented constant this train
exists to remove.

### What this does not establish

The inputs are now measured rather than absent, misattributed, or drawn from a different
window. That is a correction of inputs, not a validation of the model. McPAT remains
analytical and unvalidated against silicon here, and the absolute figures should not be
read as verified. Several specific errors are gone; the confidence interval around the
result is not thereby known.

### Data impact

All system-scope power and energy from every generation before 1.9.29. Cache dynamic power
was zero and is now non-zero. Host and device shares of the core, cache and controller
terms all change. Device-scope memory dynamic energy was reported as absent and is now
measured. Core power from 1.9.28 onward additionally carried the degenerate mix described
above and is superseded by this release.

Timing is unaffected on every path. The statistics are read from the output file after the
simulated process exits, so nothing here can feed back into the simulation, and the core
changes add counters without altering the predictor call or any scheduling decision. This
was verified by repeated measurement against the previous release rather than by argument
alone.

### Still open, recorded rather than fixed here

The controller for every non-DRAM technology is parameterised as though it drove an early
DDR generation, so the emerging-memory technologies share one interface description that
describes none of them (1.9.30). The host's own memory-array energy is not modelled
anywhere: its controller is priced and the memory behind it is not (1.9.31). An ALU
processing element is priced as a complete in-order core, including an instruction fetch
unit, branch predictor, caches and a floating-point unit it does not have (1.9.32). Area
reports zero in both scopes, root-caused to a stale null guard left behind when the power
computation became subprocess-isolated (1.11). And the reported instruction count for a
co-simulation host remains contaminated by injected timing charges; only the power path is
corrected.

## 1.9.28 -- core power was priced on activity that was absent, invented, and misattributed

The concern that opened this item was that host per-core dynamic power looked
implausibly low for an out-of-order core at the configured clock. It was.
Three separate defects fed the core power model, each independently sufficient
to produce the symptom.

FIRST, the instruction count never arrived. The system-scope (co-simulation)
power path computed a per-node instruction total and then never passed it to
McPAT -- it supplied cycles and busy-cycles only. The count therefore kept its
constructor value of zero, and every core activity statistic in the generated
input was zero: integer, floating-point, branch, committed, and reorder-buffer
reads alike. Core dynamic power was approximately zero BY CONSTRUCTION in every
co-simulation cell. The device-scope path always supplied it.

SECOND, the instruction mix was invented. The activity statistics were built
from fixed fractions of the instruction count -- a fixed share integer, a fixed
share floating-point, a fixed share branch, and a fixed mispredict rate --
applied identically to every workload. A stencil, a graph traversal and a
matrix-vector product were all described to the power model as the same
instruction stream. The simulator measures micro-ops, basic blocks and
mispredicted branches, and none of it was being read.

THIRD, and the largest, work was misattributed between nodes. The per-node path
took a SINGLE core's instruction count and divided it across nodes in
proportion to their CORE COUNTS. With a single-core host beside a many-PE
device, the host was credited with a small fraction of its own work while the
device was credited with a large multiple of work it never performed. The
parser already separated host from device cores for cycle counts; it simply did
not do so for instructions.

Fixed by supplying the count, by reading the measured activity the simulator
already reports (branch counts derived from basic blocks, since a block
terminates at a control transfer, and mispredictions measured rather than
assumed), and by giving each node its own measured instruction total. The
integer/floating split remains an assumption, since retired operations are not
classified, but it now applies to the non-branch remainder rather than to the
whole stream.

Verified on a co-simulation cell: each node is now priced on the instruction
count it actually executed, the host's rising and the device's falling to their
measured values, and reported dynamic power rises accordingly.

What this does NOT establish: that the resulting absolute figures are right.
The inputs are now measured rather than absent or invented, which removes three
specific errors; the underlying power model remains analytical and its output
has not been validated against silicon. Treat the improvement as a correction
of inputs, not as a validation of the model.

Data impact: all core power and energy figures change in system scope. Host
figures rise substantially, device figures fall, and the split between them
changes. Device-scope runs are unaffected by the attribution defect but do gain
the measured instruction mix. Not comparable with earlier generations.

## 1.9.27 -- cycle timestamps exchanged between ranks now carry their clock domain

Defect: ranks exchange rendezvous timestamps as raw CYCLE counts, and the
receiver differenced a sender's stamp against its own clock with no conversion.
A cycle is not a unit of time; it is a unit of a particular core's time. The
subtraction is therefore valid only when both cores' clocks tick at the same
rate -- a property of the configuration file, not of the code. Where the rates
differ the computed wait is wrong by exactly their ratio, and nothing detects
it: the arithmetic succeeds and produces a plausible number.

This was correct for the configurations we happen to run rather than correct by
construction, which is the same failure shape as the NoC statistics parser
earlier in this train: a value that looks right until the assumption behind it
stops holding.

When it can fire today: ranks normally migrate between the host and the device
together, so they share a clock domain and the arithmetic happens to be sound.
The exception is the migration window, where one rank can already be on a device
processing element while another is still on the host. Those two domains
genuinely differ.

Fix: the published timestamp now carries the clock rate it was taken at, and
both consumers -- the rendezvous advance and the receive-side arrival
computation -- convert the sender's stamp into the local domain before
differencing. The rate is written into the padding word that already existed in
the parameter block shared between the guest transport and the plugin, so the
structure's size and field offsets are unchanged and the two definitions stay in
agreement. A missing or equal rate converts to the identity, so the previous
behaviour is preserved exactly wherever the assumption held.

Validated as INERT, which is the appropriate test for a change that should only
engage in configurations we do not currently run: the device-scope determinism
gate reproduces its 1.9.26 values across all five pinned and all five unpinned
control runs, and the co-simulation cell reproduces its 1.9.26 cycle count
exactly, not merely closely. Any movement would have indicated the conversion
engaging where it should not.

Data impact: none for configurations whose host and device clocks coincide,
which is all present ones. Configurations with differing clock rates were
previously computing rendezvous waits incorrectly and will change.

## 1.9.26 -- revert the wake-up snap only where the core is the rank's alone

Restores the transport-wait correction that 1.9.23 removed, this time gated so
it cannot consume another rank's execution. It closes the host-side defect
1.9.21 attempted, without the device-scope regression that forced that release
to be reverted.

Background. Waking a parked rank, the simulator does not set its clock to when
its message or barrier released it: it snaps the clock to the global phase
clock, wherever the simulation as a whole has reached. That value depends on
how far every other simulator thread progressed while this one was parked, so
it imports the running machine into the simulated timeline. The correction is a
PAIR -- revert the snap, then charge the real wait computed from arrival times.
Both halves are required; 1.9.21 removed the first and inflated every
device-scope result.

The revert measures the snap as "clock now minus clock when I blocked". That is
this rank's snap only if the core was ITS ALONE for the whole window. On a
device PE it is, one rank per PE since 1.9.20. On the co-simulation host core,
shared by every rank, other ranks ran on the same clock meanwhile, so the
difference is the snap PLUS their execution -- and reverting it destroys that
work, which is the host-side collapse.

So the revert now requires exclusive ownership: the same core throughout, and
no bind by a DIFFERENT thread in between. The second condition is what the
previous attempt got wrong. It counted every bind to a core, but a rank parks
at a barrier and rebinds to its OWN core on waking, which bumped the count and
made every rank appear to share. Tracking the core's current owner and counting
only foreign binds distinguishes "another thread was here" from "I came back".

Validated on BOTH gates together, which is the pairing 1.9.21 failed and the
two attempts after it could not achieve:

- device scope: the determinism gate returns to its pre-1.9.21 values across
  all five pinned runs and all five unpinned control runs;
- co-simulation: the oversubscribed host configuration reports a plausible
  instructions-per-cycle rate on two independent node types, where before it
  reported an impossible one.

Measured evidence is kept with the maintainers.

Data impact: co-simulation host cycles and host energy change on cells with a
substantial host phase, since the wait correction now applies there correctly
rather than discounting co-resident ranks. Device-scope results are unchanged
from 1.9.23, by construction and by gate.

The durable fix remains to set the clock to the computed wake-up time directly
rather than correcting a snap after the fact. An absolute write cannot consume
another rank's work because it is not a subtraction, and it makes the
shared-versus-exclusive distinction irrelevant. That requires rebasing the
core's pending weave state and is scoped separately.

## 1.9.25 -- stop fabricating host NoC activity

The per-node power path gave every node's network a hardcoded duty cycle with
no traffic behind it. For a device node 1.9.24 replaced that with measured
Garnet activity. For a HOST node there is nothing to replace it with, and the
placeholder was left billing a fixed fraction of peak to a fabric that may not
exist at all: the co-simulation host is single-core by default, where the
on-die network is degenerate (core to caches to memory controller is direct),
yet it was priced as if partially busy.

PIMID does not model a host-side interconnect. The device statistics describe
the in-memory network and pricing a host socket from them would be worse than a
placeholder, so there is no measurement to substitute. The honest treatment is
zero activity: the two-level structure is still emitted, because McPAT's
homogeneous-NoC path crashes CACTI below two levels, so the entry now carries
its leakage and nothing else.

Where a host fabric WOULD carry traffic -- a multi-core host -- the run now says
plainly that the interconnect is not modelled and its activity is not measured,
rather than reporting an invented figure. A single-core host reports that its
fabric is degenerate and priced at zero.

This continues the theme of 1.9.24: a plausible-looking number with nothing
behind it is worse than an explicit gap, because it cannot be audited.

Data impact: host NoC power changes on every system-scope cell. The term was
small, so totals move little, but it is no longer fabricated.

## 1.9.24 -- NoC power was never measured: the stats parser matched the wrong keys

Defect: every NoC power figure the simulator has ever reported was priced from
zero network activity, in BOTH device scope and co-simulation.

Root cause, one line. The Garnet statistics writer emits DOTTED keys
("garnet.total_packets = N"), while the reader compared against bare names
("total_packets"). No comparison ever matched, so every field kept its zero
default. The file was found, opened and read to completion without error, and
yielded nothing -- a silent failure with no warning anywhere. Both power paths
call the same parser, so device-scope sweeps and co-simulation cells were
equally affected.

Consequences that follow from that, each of which had looked like its own
defect:

- Co-simulation fell back to a placeholder NoC activity level, since the
  measured stats it tried to use always parsed as empty. The placeholder was
  not a lazy default; it was covering for a parser that could not return
  anything else.
- Device scope built its NoC levels from measured traffic as designed, but the
  measurement handed to it was always zero, so the levels carried no activity.

Fixed by stripping the dotted prefix before matching, tolerating any prefix
rather than only "garnet.".

Three further corrections in the same area, all of the form "use the
measurement that was already on disk":

- NoC access counts now come from the recorded HOP count rather than the
  end-to-end packet count. McPAT's NoC access statistic counts router
  traversals, so a packet crossing several routers is several accesses;
  counting packets understated activity by the average hop count.
- Flit width now comes from the recorded value instead of a literal.
- The network clock now comes from the recorded value instead of the PE clock,
  which is a different quantity.

Co-simulation additionally now sources its NoC levels through the same builder
the device path uses, instead of emitting a fixed two-entry placeholder. When
usable statistics are genuinely absent it still falls back, but now says so
explicitly rather than reporting a placeholder figure as if measured.

Data impact: every NoC power figure changes, in device scope and
co-simulation alike, because the term was previously priced from zero activity.
Reported totals rise accordingly. This does NOT affect timing -- the parser
feeds the power model only -- so cycle counts are unchanged. Results are not
comparable with earlier generations for power or energy.

Version note: 1.9.22 was planned for this work but never released; 1.9.23
shipped first as a revert, so this lands as 1.9.24 and 1.9.22 is skipped.

## 1.9.23 -- revert 1.9.21 (its device-scope regression outweighed its host fix)

1.9.21 changed how a rank's transport wait is accounted for. It fixed a real
defect on the co-simulation host, where a single core is shared by every rank
and the previous rule discounted co-resident ranks' execution along with the
wait. It also introduced a worse defect on the device.

The wait correction is a PAIR. Waking a parked rank, the simulator does not set
its clock to when its message or barrier released it -- it snaps the clock to
the global phase clock, wherever the simulation as a whole has reached. That
value depends on how far every other simulator thread progressed meanwhile, so
it imports the running machine into the simulated timeline. The correction
reverts that snap and then charges the real wait, computed from arrival times.
1.9.21 removed the revert and left the charge, so the snap survived AND the
computed wait was added on top. Device-scope cycles inflated by roughly a
factor of three, consistently, across a ten-run gate.

An intermediate attempt restored the revert but applied it only where a core
was provably the rank's alone, detecting shared use with a per-core bind
counter. That counter incremented on every bind, including a rank rebinding to
its own core after a barrier, so each rank tripped its own guard and the revert
was skipped almost everywhere. The device-scope figure did not recover.

This release reverts 1.9.21 in full. The accounting sources are restored
byte-for-byte to their 1.9.20 state, and the device-scope determinism gate
returns to its pre-1.9.21 values across all ten runs.

The host-side defect that 1.9.21 set out to fix is therefore STILL PRESENT: on
a shared host core the wait correction discounts other ranks' execution, and a
co-simulation cell with a substantial host phase can report an implausibly high
instructions-per-cycle rate. That is a known open defect, tracked with the
maintainers, and it is the lesser of the two problems -- it affects the host
phase of co-simulation cells, whereas the regression affected every
device-scope result.

The durable fix is not another guard on the difference. It is to set the clock
to the computed wake-up time directly, rather than correcting a snap after the
fact: an absolute write cannot discount another rank's work, because it is not
a subtraction, and it makes the shared-versus-exclusive distinction irrelevant.
That requires rebasing the core's pending weave state, since the simulator's
clock is monotonic, and is scoped as its own release.

## 1.9.21 -- follow the CORE: never subtract another thread's work

Defect: two runs of the same co-simulation cell retired the same instructions
but reported host cycle counts differing by nearly two orders of magnitude, one
of them implying an instructions-per-cycle rate far above the core's issue
width and therefore impossible. Reported energy followed the cycle count, so it
diverged with it.

Root cause. `cycles` is a PER-CORE counter, while ranks and threads are
software constructs. At MPI_COMM_END the plugin rewound the entire
COMM_BEGIN->END delta of that per-core counter as though it were the calling
thread's own transport wait. That is valid only when the thread has the core to
itself. In co-simulation the host is a single core shared by every rank, so the
delta also contains co-resident ranks' execution, and subtracting it removed
real work -- deflating the core rather than discounting a wait.

The sharing was confirmed directly with the diagnostics below: an affected
window showed an unchanged core id, an unchanged Core object and an unchanged
domain, with a second thread resident on the same core. Nothing had migrated.

Fix: follow the core. The reported count is what that core advanced by
SIMULATING WORK, whoever ran on it. Only clock JUMPS are subtracted -- the
parked rejoin fast-forward and the cSimStart/cSimEnd weave resolutions, which
are simulator artifacts rather than execution. They are accumulated per core in
Core::pimidJumpCycles and removed at the reporting site. Work is never
subtracted. The per-thread COMM-window rewind is deleted.

This holds at every subscription ratio, which the earlier attempts did not.
Oversubscribed, the counter legitimately aggregates every thread that ran on
the core and nothing is taken away. Undersubscribed, an idle core never
advances and contributes nothing. Equal, behaviour is unchanged.

Three earlier attempts failed and are recorded so the dead ends are not
re-explored. Rewinding only measured clock jumps left the device scope
inflated, and bimodal within a single binary, because it also dropped the
legitimate corrections. Gating accrual inside the comm window was rejected
before implementation: the two runs retire the same total instructions while
differing greatly in how many fall inside windows, so those are the SAME
instructions misattributed, and gating would have erased real ones. A same-core
guard keyed on the core id passed several runs and then failed, because the
1.9.20 PE pinning returns a rank to its own home PE, so the core id no longer
distinguishes a shared core from an exclusive one.

Validation was PARTIAL when this was committed. One co-simulation cell on the
configuration that previously collapsed returned to a plausible
instructions-per-cycle rate. Further repeats and the device-scope determinism
gate were still queued; the gate is the load-bearing check, since it
distinguishes a correct fix from one that has stopped deflating and begun
inflating. Measured evidence is kept with the maintainers.

Data impact: co-simulation host cycles and host energy change on every cell
with a real host phase, and device-rank cycles change wherever an ALU core was
previously rewound -- ALUCore::pimidRewindCycles moves curCycle itself, so that
rewind corrupted the simulated timeline rather than only the report. Results
are NOT comparable with earlier generations.

Diagnostics, default off: PIMID_COMM_DIAG=1 prints a per-thread census of comm
windows -- their count, what the previous rule would have rewound, what was
actually rewound, and how many instructions retired inside each window. A large
would-be rewind against a nonzero in-window instruction count on a shared core
is the signature of this defect.

## 1.9.20 -- deterministic rank-to-PE placement under thread-MPI (defect #15, partial)

Defect: which device PE a thread-MPI rank landed on was decided by a race, so
two runs of the same cell placed the same work on different PEs.

Root cause. Half the mapping was already deterministic: PE index to Garnet node
is fixed at init (`mems[i]` carries `mcId_ = i`; `PEMemoryInterface` computes
`srcNode = mcId_ % numNodes`). The thread-to-core half was not.
`Scheduler::schedThread` first re-takes the thread's last context if it is still
IDLE -- a race against whoever else wants it -- and otherwise takes
`freeList.front()`, where the free list is ordered by the wall-clock sequence in
which cores were released. Ranks cross that path on every host-to-device
migration, so each run drew a different rank-to-PE permutation. Same work,
different placement, hence different hop distances and different latencies.

Fix: each rank now gets a single-PE affinity mask (its "home"), so
`schedThread` has no other candidate to race for; its selection algorithm is
untouched. The key is `vcpu_index`, NOT `tid`: `tid` is handed out `nextTid++`
on first callback, i.e. in arrival order, so it is raced itself and pinning by
it would only have relabelled the same nondeterminism. Homes are permanent for
the run, so a rank that returns to the host and migrates back reclaims the same
PE. A collision guard falls back to the old unpinned mask if a non-bijective
vcpu set would put two ranks on one PE, which would otherwise deadlock them
against each other across a barrier. `PIMID_NO_PE_PIN=1` disables the whole
thing. Non-MPI (OpenMP, device-only) paths are untouched.

Validated -- placement determinism: PASS. Two co-sim runs on different node
types now put the heavy rank on `src=0` with every PE in the same slot; before
the fix the heavy PE moved from 4 to 10 between the same two runs.

Validated -- run-to-run cycle spread: NO IMPROVEMENT. This is a negative
result and it is stated as one. Paired arms in one job on one node, device
scope, HBM3 detailed stencil_2d 256, differing only by `PIMID_NO_PE_PIN`:

Paired arms in one job on one node, differing only by PIMID_NO_PE_PIN, showed
no reduction in spread: the unpinned arm was if anything marginally tighter,
and the variance ratio was far from significant at this sample size. Measured
values are kept with the maintainers.

The unpinned arm is if anything tighter; the variance ratio is 0.72 where
F(4,4) needs ~6.4 for p=0.05. The co-sim BFS cell agrees: n=5 after the fix
gives a spread statistically indistinguishable from the 1.9.19 baseline,
a variance ratio of 1.27. The placement race was real, but it was not what
drives the cycle variance.

What defect #15 actually is, restated from the same 10 runs:

A single rank's own cycle count is very nearly reproducible, while the
REPORTED figure -- a maximum over all ranks -- is several times noisier.
Measured values are kept with the maintainers.

A single rank simulates very nearly reproducibly. The reported device-cycle
figure is a max over 16 ranks -- an order statistic that amplifies small
per-rank jitter, and whose critical rank alternates (rank 0 was slowest in 3 of
10 runs, and close behind the slowest in the rest). The residual therefore
lives in the cross-rank critical path, not in any rank's own simulation. It is
not a data-dependence artifact either: stencil_2d is a regular kernel and
behaves like BFS here. Compare 1.6.3, where the weave quantum was concluded to
be PDES-fundamental.

Defect #15 REMAINS OPEN. This release removes one confirmed source without
closing it.

Data impact: pinning changes placement relative to the previous raced
assignment, so 1.9.20 device and co-sim results are NOT bit-comparable with
earlier generations. The change is within the run-to-run spread above, but it
is a real change of placement, not noise.

Also in this release, diagnostics only, default off: `PIMID_INJ_DUMP=<path>`
writes a per-source injection census (count, destination sum, cycle sum) at
SimEnd. This is what identified the permutation above and is the regression
test for it.

## 1.9.19 -- docs-only: how 1.9.17 and 1.9.18 were validated

Bit-identical timing was the intended acceptance test for both, and it could
not be run: the message-passing co-simulation path is not reproducible run to
run (defect #15, open). Five repeats of the UNCHANGED binary on one cell
(HBM3, message-passing BFS) span a few percent of device cycles. An earlier
two-run estimate was too small a sample to see this.

The changes were therefore validated against that noise: five repeats per
binary, balanced across two node types so machine effects fall on both arms.

Five repeats per binary, balanced across two node types so machine effects
fall on both arms. The means differ by well under one standard deviation and
the ranges overlap almost entirely: no evidence the changes move results.
Measured values are kept with the maintainers.

The means differ by well under one standard deviation, and the ranges overlap almost
entirely (before 293.25M-301.73M, after 293.60M-300.68M). No evidence the
changes move results. Flush cycles were bit-identical across all ten runs and
host cycles varied negligibly and equally in both arms, so the
deterministic parts of the accounting are untouched.

What is established directly rather than statistically: the hang is gone. With
the fix the cut advances ~1M per fold instead of freezing, folding proceeds,
and the pending set stays near 1.1M records instead of passing 64M.

State the claim as "indistinguishable from baseline within the tool's measured
reproducibility", NOT as "verified identical". It cannot be verified identical
until #15 is fixed.

Follow-up, same instrumentation: two runs of one binary dumping per-fold
membership checksums (`PIMID_DET_EPOCH_DUMP`) diverge in MEMBERSHIP on 100% of
shared phases, starting at phase 0, and in MEASUREMENT on 0% -- there is no
phase where the same record set yields a different latency. The Garnet timing
model is deterministic; what varies is which records reach it.

## 1.9.15 -- docs-only

Documentation for 1.9.12 through 1.9.14 (the entries below, plus the memory
and power notes they touch). No source change; no data impact.

## 1.9.14 -- CACTI area units and quiet latency-only queries

Reporting-only; no model consumes either value, and every consumer found by
inspection is a print statement. No data impact.

- **Area units.** CACTI returns areas in um^2 and cache height/width in um;
  the wrapper forwarded them unchanged while labelling them mm^2 and mm, so
  every reported area was inflated by 10^6 (a 64 KB SRAM bank printed as
  6.8e4 mm^2 instead of 0.068 mm^2). Converted in `getArea`,
  `getCacheHeight`, `getCacheWidth`, `getSubarrayArea`, `getCellArea`.
  Access and cycle times were already correct (CACTI returns seconds; the
  wrapper treats them as seconds).
- **Quiet flag.** The cache-latency helper instantiates CACTI purely for an
  access time, but the shared initializer printed a full banner including
  energies that path never consumes, so logs carried a placeholder-looking
  2 MB / 1 nJ block beside the real per-technology numbers. `SRAMConfig::quiet`
  suppresses the banner for latency-only queries.

## 1.9.13 -- device write accounting: GETX is a write at cacheless PEs

- **Defect #17 -- device stores counted as reads.** The PE memory interface
  classified accesses by coherence request type, counting GETX as a read.
  Correct for a cached requester (the store's memory write appears later as a
  PUTX writeback) but wrong for cacheless device PEs, whose stores arrive as
  GETX with no writeback to follow: every device write was recorded as a read
  and the write counter stayed zero. GETX now increments the write counter at
  that interface; PUTS (clean writeback) remains a non-access.
- **Locality counters folded into read/write totals.** The stats aggregator
  added `localAcc`/`remoteAcc` into `mem_rd`/`mem_wr`. Those are a
  where-split of the same accesses, not a read/write split, so each access
  was charged once as a read and once as a write. The aggregator now uses
  only the true rd/wr counters.
- **Data impact.** Device-scope energy for all generations before 1.9.13:
  read/write mix mispriced (all-reads at the interface, plus the
  double-charge). Timing unaffected. The error is first-order where write
  energy dwarfs read energy -- at the swept 64 KB bank a PCM write costs
  orders of magnitude above a DRAM read per line -- and second-order on DRAM,
  whose read and write burst energies are comparable. Validated by
  measurement: with the fix, total accesses (rd+wr) reproduce the pre-fix
  totals negligibly, i.e. the same traffic correctly labelled, and recovered
  write fractions match kernel semantics (STREAM triad worker PEs at exactly
  1/3).

## 1.9.12 -- NVM per-access width fix + NVSim cache-key hardening

- **Defect #17a -- per-access width.** NVM characterizations were requested
  with `word_width_bits` left at a single 64-bit word while accesses are
  full 64 B lines, undercharging NVM array energy by about 8x and shifting
  modelled latencies (PCM read 1.127 -> 2.832 ns at the swept bank). The
  power path now sets `word_width_bits = cache_line_size * 8` and the three
  timing sites request 512 b, matching the CACTI/SRAM path.
- **Cache-key hardening.** The NVSim characterization cache keyed on
  (type, capacity, process node) without the word width, so pre-fix and
  post-fix entries could alias; the key and the on-disk filename now include
  it (`..._w512.xml`), and capacities print in KB.
- **Data impact.** All NVM (STT-MRAM/PCM/ReRAM) device energies before
  1.9.12 are invalid; corrected per-64 B constants at the swept 64 KB bank
  differ per medium, with PCM's write cost dominating the set, in nJ
  (read/write). Cycle counts shift by a few percent where array timing
  matters and are unchanged where the network dominates.

## 1.9.11 -- docs-only

Documentation for the 1.9.10 energy-model overhaul (this entry, the yaml
reference knob ladder, and the energy-model notes below). No source change;
no data impact.

## 1.9.10 -- energy-model overhaul: system-scope integration fix + tool-measured memory energy

Eight commits; device-scope TIMING is bit-invariant (same-node A/B gate on the
release binary: cycle counts agree to within a rounding-level delta, both rc=0).

- **Defect #16 -- system-scope power integration.** `runPerNodePowerAnalysis`
  priced every node over the first host core's contention-EXCLUDED unhalted
  cycles while feeding full aggregate access counts, producing nonphysical
  system powers (implausibly high bfs baselines; kW-class co-sim host nodes; below-idle
  cells). Fix: each node is priced over true wall-clock time in its own clock
  domain (host wall = max(unhalted + contention) across host cores; device
  wall = max device cycles). Device-scope `runPowerAnalysis` is a different
  function and is unaffected.
- **Configurable process node.** `power.tech_node_nm` (+ `device_`/`host_`
  variants); the host now inherits the device node. Finding: the old hardcoded
  7 nm host literal never reached McPAT (a `max(22, n)` clamp), so all landed
  data was already effectively 22 nm; the change is forward-looking, not
  retroactive.
- **Ramulator2 energy layer, tool-measured.** Root cause of the 0.000-nJ
  energy reports: a never-fed counter behind a cycle-0 guard, plus "INFERRED"
  placeholder `bank_energy_pJ` constants. The energy layer now lives inside
  Ramulator2 (`external/ramulator/src/dram/pimid_energy.h`): JEDEC IDD/VDD
  per-command energies with first-class `current_presets` for all seven DRAM
  standards (DDR3/LPDDR5/GDDR6/HBM2/HBM3 added; the DDR4-class reuse fallback
  is retired), background/refresh power from standby currents, and per-scheme
  termination/ODT (SSTL/POD/LVSTL): DDR3 17.6 / DDR4 9.4 / DDR5 5.25 /
  GDDR6 2.6 / LPDDR5 0 / HBM 0 pJ/bit. The wrapper is a thin reader; the
  relocation was verified value-invariant to the pre-migration table.
- **Off-chip channel, die-boundary split.** CPU-side McPAT MC/PHY (~9 pJ/bit
  at 22 nm) + DRAM-side I/O (0.76) + termination (5.25) = ~15 pJ/bit for DDR5,
  inside the published 15-22 pJ/bit full-channel band; HBM carries no
  termination (interposer), a physics-derived asymmetry.
- **Known boundary.** The device H-tree fabric is priced as a McPAT bus-mode
  wire/repeater datapath (7.3 mW for a 16-PE tree) in the analysis layer that
  produced the published dataset; the binary's own per-node NoC printout still
  uses router-mode pricing, and a runtime `power.noc_model` knob is roadmap
  work, not shipped in this release.
- **Data impact.** Timing: none (gate above). System-scope powers/energies
  produced before 1.9.10 are nonphysical and were re-derived; device-scope
  energies were re-derived onto the measured per-command constants. The
  release's knobs are documented in `docs/yaml_reference.md`.

## 1.9.9 -- docs-only

Documentation for 1.9.8 (this entry, badge). No source change.

## 1.9.8 -- power-derivation fixes (1-PE NoC gating; subarray fan-in overcount)

- **Defect 1.** runPowerAnalysis gated the in-memory-network power on
  num_pes > 1, dropping the H-tree leakage term for every 1-PE device run.
  Fixed: gate also fires on hierarchy_enabled. 1-PE
  pecount power/energy re-derived from existing logs (no re-simulation);
  multi-PE cells bit-identical.
- **Defect 2.** buildNoCLevelsForMcPAT used a hardcoded x2 subarray->bank
  fan-in instead of config.subarrays_per_bank (32 for HBM3), overfeeding
  McPAT ~16x router counts at SUBARRAY placement only (65 W vs a physical
  term). Fixed; SUBARRAY placement rows re-derived; other levels
  bit-identical.
- **Documented (no code change).** Power templates key off microarch class,
  not timing fidelity: simple_core and in_order_core share the single-issue
  template. total_power_W is leakage/config-dominated; activity
  moves only trailing digits; Ramulator2 array dynamic energy is reported
  separately.

## 1.9.7 -- docs-only

Documentation for 1.9.6 (this entry, badge). No source change.

## 1.9.6 -- thread-MPI head-of-line deadlock at >16 ranks

- **Defect.** The in-process MPI transport's per-rank mailbox ring (16 slots)
  deadlocked under source-matched receive when >16 ranks flooded a collective
  root: the receiver would not consume the ring head (wrong source) and the
  wanted sender could not append (ring full). bfs (per-level gathers) at
  32/64 ranks froze within ~5 phases; 8/16 ranks and one-shot-reduce kernels
  never filled the ring. Latent since 1.6; exposed when measured pricing
  (1.9.0) changed rank arrival patterns.
- **Fix.** Unexpected-message staging queue per receiver: when the ring is
  full and the wanted source absent, the head message is staged (freeing the
  slot, waking the sender). Source matching, per-source FIFO, and consumed
  timestamps unchanged -- deadlock-free by construction, deterministic.
  Capacity is now elastic at any rank count.
- **Data impact.** pc_32/pc_64 MPI bfs cells producible (v196 tags); all
  other cells gate-verified unchanged (all within their documented bands,
  cosim clean with exact 16x flush arithmetic).

## 1.9.5 -- docs-only

Documentation for 1.9.4 (this entry, cores.md note, badge). No source change.

## 1.9.4 -- simple_core phantom wall-clock leak under thread-MPI

- **Defect.** simple_core's frozen-clock rewind (COMM_END) lowered curCycle
  directly, but SimpleCore::join() re-pins curCycle to the wall-pumped global
  phase clock on every phase crossing -- the rewind did not stick. Rank clocks
  tracked wall time instead of work; on rendezvous-heavy kernels the phantom
  dominated (bfs: 34.6M of 36.9M cycles). Same family as the 1.9.2 OOO defect:
  per-class rewind paths unsafe against clock re-pinning.
- **Fix.** Accumulate the rewind in pimidPhantomWait and net it out at the
  read sites (getCycles / ROI stat) -- join-immune, byte-inert outside MPI
  frozen-clock waits. simple_core-only; other cores + OMP gate-verified
  unchanged.
- **Data impact.** simple_core MPI cells re-run (v194): bfs 36.9M -> 8.37M
  (physical: between ooo 3.8M and alu 24.5M, just above in_order 8.1M).

## 1.9.3 -- docs-only

Documentation for 1.9.2 (this entry, cores.md OOO bulk-advance note, version
badge). No source change; no data impact.

## 1.9.2 -- window-safe bulk clock advance for OOO cores (1.6.3 boundary closed)

- **Defect.** Under thread-MPI rendezvous, `OOOCore::join()` applied a raw
  `curCycle = targetCycle` jump while parked, bypassing the window-safe
  `longAdvance()`. A 100K-1M+ cycle rendezvous jump orphaned in-flight
  unbounded-window entries behind `curCycle`; the next window rebase computed a
  negative position and tripped the `ooo_core.h` assert (then SIGSEGV). bfs
  (one collective per frontier level) is the reliable trigger; this is the
  1.6.3 "weave quantum" boundary that had kept OOO+thread-MPI cells pulled.
- **Fix.** Route the parked join through `insWindow.longAdvance()` (drain-then-
  jump; retires in-flight uops instead of discarding; byte-identical to the old
  code when the window is empty; drain bounded by the 1024-cycle horizon).
  OOO-only -- ALU/simple/in-order joins untouched (gate-verified).
- **Data impact.** OOO+MPI cells are now simulatable; the coremodel MPI family
  is re-run on 1.9.2 for single-binary consistency (v192 tag). Validation:
  the crashing cell completes at 3.92M cycles (OOO fastest on MPI bfs, as
  latency hiding predicts); non-bfs OOO cells reproduce v190 closely; OMP
  path unshifted. Note: thread-MPI bfs cells carry multi-percent
  run-to-run spread (worst case of the documented weave nondeterminism), so
  bfs census checks use a tolerance band, not bit-equality.

## 1.9.1 -- docs-only

Documentation refresh for the 1.8.3 -> 1.9.0 train (this file, plus co-sim MPI
window, measured MPI pricing, and OMP critical-path metric edits). No source
change; no data impact.

## 1.9.0 -- measured Garnet-fed-back pricing for thread-MPI (epoch-frozen)

- **Change.** Thread-MPI per-access latency now prices from **measured** Garnet
  congestion via epoch-frozen deterministic feedback; the analytical override
  is demoted to an escape hatch (`PIMID_MPI_ANALYTICAL_PRICING=1`). OMP is
  unchanged (rolling-EWMA live feedback).
- **Root cause of what it replaces.** The naive "read the live per-drain EWMA"
  feedback was 26-196% per-core / ~25% makespan nondeterministic (a fast rank
  folds fewer drains into the rolling scalar than a slow one) and read-depth
  inflated. Epoch-frozen table lookup (read epoch `k-1`'s frozen sample, keyed
  by the access's own ROI-relative cycle) removes the read-instant dependence.
  Three membership leaks fixed en route (pre-ROI pollution, startup-skew
  stamping, partial-bucket folds; DESIGN_190 section 10), built on the
  already-deterministic per-phase batch contents (defect #9 venue-independence).
- **Repeatability reality (residual).** Not bit-exact: the one-pass
  measured-feedback loop converges to a **host-dependent fixpoint** --
  within-host reproducible per-core, cross-host systematic; the rank-0 /
  cut-pinning core is host-independent. Fidelity:
  measured level +5-12% above the analytical floor.
- **Data impact.** Thread-MPI detailed sweeps priced with the old analytical
  override should be **regenerated** with measured feedback. MPI sweeps must be
  run **single-venue** for internal consistency (true venue-independent
  bit-exactness needs two-pass replay, future work). OMP and analytical-model
  cells unaffected.

## 1.8.8 -- OMP critical-path cycle summary (core-0-metric defect)

- **Defect.** Device-scope OMP runs had no critical-path aggregation. Both the
  sweep harness (`grep cycles | head -1`) and `parseZSimOutputFile()` latched
  onto the FIRST per-PE `cycles:` line = **core 0 only**, which reflects only
  that PE's active cycles, not kernel completion (the last PE to finish).
- **Root cause.** Core 0 was a low outlier at 16/64 PEs but representative at 32
  (16 PEs: core0 8% low; 64 PEs: 26% low; 32 PEs: representative), so recording
  it manufactured a spurious bfs HBM3 32-PE +28% cycle spike that "reversed" at
  64.
- **Fix.** Device-scope OMP now emits `OMP cycles: <max> (mean, min, pes,
  critical-path max)` plus a parser-compatible `Total: <sum> cycles (max: <max>)`
  (mirroring the MPI path). Purely additive; per-PE lines / `out.cycles` /
  power untouched.
- **Data impact.** Device-scope OMP sweeps parsed via `head -1` / core-0 carry
  the artifact and must be re-parsed against `(max: N)` (or re-run). The results
  harness OMP branch must switch from `grep cycles | head -1` to the `(max: N)`
  line (flagged out-of-scope in the commit).

## 1.8.7 -- co-sim MPI: tid/cid aliasing, zero-migration ranks, guard, protocolTail

- **Defect #15 class -- tid/cid ROI-baseline aliasing.** `roiRelCycles(tid)`
  read the ROI baseline table by **rank id** while it is written by **core
  index**. In system scope a rank's `cid != tid`, so `tid` 0..3 aliased HOST
  cores' ~16.5M pre-ROI (MPI_Init-era) clock; `roiRel` clamped to 0 and
  collapsed the SEND/RECV rendezvous (reduce root accumulated senders' stamps
  instead of advancing to their max). Fix: index by the rank's actual core
  (`cids[tid]`) + a per-rank last-valid-cid cache. Scoped to co-sim; device
  scope keeps `tid` indexing (correct and deterministic there).
- **Finalized model.** Ranks ARE the device PEs; the post-ROI collective tail
  (closing barrier + Reduce + Finalize) is device-resident, executed and priced
  ON THE PE. **Zero migrations** at window close (the 1.8.4/1.8.6 residual
  device->host legs, incl. rank 0's `roi_end` migration, all removed), which
  also eliminates the 1.8.6 exit race by construction.
- **Cross-axis invariant guard (defect-13 / 1.7.7 tripwire lineage).** At the
  recv rendezvous, a single message advance exceeding the ROI span means the
  clock axes are mismatched -- shout and cap rather than advance by a garbage
  delta. Eliminated a rare ~4% `2^32` rendezvous overflow (Garnet "event too
  far into the future").
- **protocolTail stat.** Per-PE receipt (final PE cycles minus closing-barrier
  marker); visibility only, never alters `cycles`. Co-sim-only, fixed global
  array, so device-scope `Core`/`initStats` stay byte-identical.
- **Data impact.** System-scope (co-sim) MPI data produced before 1.8.7 has the
  collapsed rendezvous and must be regenerated; the flaky ~4% overflow is gone.
  Device-scope MPI unaffected (reproduces the 1.8.6 distribution).

## 1.8.6 -- co-sim MPI exit-protocol heap corruption

- **Defect.** The 1.8.4 closing device->host migrate-out freed a rank's core
  slot mid-handler while a peer immediately entered its contention-sim phase
  pass; under the MPI serial weave the two overlapping handlers iterated
  glibc-heap state unsynchronized and **corrupted the process heap** (the
  "malloc(): unaligned tcache chunk" / QEMU SIGSEGV / Garnet "No output port for
  vnet" panics, and rarely the contention_sim "event too far into the future"
  assert, all right after the last rank drained).
- **Root cause (exit race).** Racy closing migrate-out under
  `simulation.parallel` serial weave (one vcpu may touch simulator state, but a
  migrate-out + peer phase pass overlap). The mirror migrate-IN at window open
  never corrupts.
- **Fix.** Drop the closing migrate-out; the drained rank finishes its short
  post-ROI protocol on its device PE (<1% accounting shift).
- **Data impact.** Pre-1.8.6 co-sim MPI runs crashed non-deterministically
  (~1/6 clean); no trustworthy pre-1.8.6 co-sim MPI data. gemv checksum
  unchanged from 1.8.4; histogram checksum 512.

## 1.8.5 -- reinstate per-rank flush + launch charges in the co-sim MPI window

- **Fix.** Reinstated the per-rank flush + launch boundary charges that 1.8.4
  had deferred, so every rank prices flush + launch on its host core at its
  kernel-entry barrier.
- **Data impact.** Negligible at sweep scale.

## 1.8.4 -- defect #14 co-sim MPI ROI window (+ 1.8.3 control-surface tidy)

- **Defect #14 (four layers deep).** Co-sim MPI measured dev=0. Root cause
  spanned: rank-0-only `roi_begin`; a `ROI_BEGIN` thread-branch early-return
  skipping the offload block; a parked-opener wall-order race making
  window-open-time migration impossible; and non-thread-safe
  migration/termination under the rank stampede.
- **Fix.** ROI is a device **window**: every rank charges + migrates ITSELF at
  its kernel-entry barrier (its own program order, no wall races), computes on
  its PE, migrates out at the closing barrier (later removed in 1.8.6/1.8.7);
  window close is a pure migration event. Migration bookkeeping serialized.
- **1.8.3 control surface.** Thread-based MPI rank emulation is the ONLY
  exec-method MPI model (`PIMID_MPI_THREADED` / `PIMID_MPI_PROCESS` deleted,
  legacy per-rank-process exec launcher removed -- per-rank processes remain
  only in the trace method); one knob `simulation.parallel` (default true)
  governs simulator parallelism for both APIs; MPI stays serial for
  determinism; `PIMID_EMIT_CONFIG_ONLY` honored in device scope.
- **Data impact.** Pre-1.8.4 co-sim MPI results measured a single rank / dev=0
  and must be regenerated. Known residuals at ship: deferred per-rank charges
  (fixed 1.8.5) and a nondeterministic post-measurement exit race (fixed 1.8.6).

## 1.8.2 -- defect #13: thread-MPI cycle rewind aborted every co-sim MPI run

- **Defect #13.** `ProcessStats::updateCore()` asserted a core's CYCLE counter
  is monotonic (`cCycles >= lastCoreCycles[cid]`). Under 1.6 thread-MPI it is
  not: a rank parking in a transport wait is rewound at `MPI_COMM_END`
  (`Core::pimidRewindCycles`) to erase wall-dependent wait growth and stay
  bit-exact, so the counter legitimately moves backwards -- the assert fired and
  QEMU took a SIGSEGV, killing every co-sim MPI cell (rc=1).
- **Root cause.** The cycle rewind (cycles-only; instruction count stays
  monotonic) collided with the cycle-monotonicity assert once a host domain and
  offload migration coexisted with real ranks -- first reachable only after
  1.8.0 stopped MPI running single-rank.
- **Fix.** Keep asserting INSTRUCTION monotonicity; mirror a cycle rollback in
  the process total instead of underflowing the unsigned subtraction.
- **Data impact.** Before 1.8.2 every co-sim MPI cell aborted (rc=1) with no
  output -- there is no pre-1.8.2 co-sim MPI data. This is the first release in
  which co-sim MPI ran correctly (16 ranks, no assert/SIGSEGV). Device scope
  unaffected (no host domain).

## 1.8.1 -- docs-only

Documentation for the 1.8.0 system-scope MPI + host-core fixes. No source
change; no data impact.

## 1.8.0 -- defect #12: system-scope MPI ran single-rank; host-baseline cores capped

- **Defect #12a -- host core count capped.** `host_num_cores` is parsed only
  from a top-level `host:` block, but system-scope configs declare the host
  under `system.hosts[]`, so it stayed at its default (4) -- capping OMP
  threads, MPI ranks, and the devorg `--pes` injection regardless of the
  configured count. Fix: sync it from the parsed HOST system node.
- **Defect #12b -- system-scope thread-MPI never wired.** The `libpimid_mpi.so`
  LD_PRELOAD plus `PIMID_MPI_RANKS`/`PIMID_MPI_THREADED` existed only in the
  device-scope branch, so every system-scope MPI run (host baselines AND co-sim)
  resolved MPI against the system runtime and executed as a SINGLE rank
  (rank 0, size 1) silently at exit 0. Fix: wire thread-MPI in system scope and
  force `parallelism=1` there (ranks run by zsim's deterministic round-robin
  core rotation, which never engages at parallelism>1).
- **Data impact.** System-scope MPI results produced before 1.8.0 are
  single-rank (and core-count-capped) and must be regenerated. Device-scope
  sweeps were never affected (device launch path; all edits gate on
  `scope == "system"`).
