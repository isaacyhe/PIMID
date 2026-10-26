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
| interconnect | Garnet | McPAT (routers; links over CACTI wires since 1.12.18, energy-efficient repeaters since 2026-10-25) | McPAT |
| stacked-die crossings (HBM TSVs) | Garnet | CACTI-3DD, in the McPAT fork (1.12.18) | CACTI-3DD |
| clock distribution | -- | McPAT fork, Wattch model (1.12.18) | -- |
| memory controllers | Ramulator2 (replayed) | McPAT | McPAT |
| host-device link (`interposer`) | link model | McPAT fork, UCIe class (1.12.18) | McPAT |

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

The knobs are `pim.pe.lanes`, `pim.pe.operand_width`, `pim.pe.floating_point`,
`pim.pe.imem_bytes` and `pim.pe.spm_bytes` / `pim.pe.spm_staging` -- see the
configuration reference, which also records where a knob reaches only one half
of the model and warns rather than pretending otherwise.

Both of the element's stores are plain RAMs in McPAT: the resident instruction
store and, since 1.12.18, the data scratchpad (`pim.pe.spm_bytes`, a 64-bit
word), each with no tag array and no miss, fill or prefetch buffers. The
scratchpad is read and written by the counts the run measured (with
`pim.pe.stack: spm` these include the stack's accesses); at 0 bytes the
element has no data store at all. Until 1.12.18 the element described no data
store, so McPAT built one from its parser defaults (a one-byte cache). With
`spm_staging: dma` the element also carries the scratchpad's DMA engine, built
from McPAT's own models: a one-line buffer (16 words: CACTI builds no array
below 16 rows), its three 64-bit counter registers (priced as McPAT prices a
register file, at CACTI's smallest 64-bit array of 16 rows, so the three-register
set is overstated by that floor) and one integer adder. Per line moved it charges
a write and a read of each of the buffer's 16 words, three register reads and
writes and three adder operations.

## Instruction decode (1.12.28)

User rulings 2026-10-24. McPAT's instruction decoders were the largest single
cost of a processing element (13 pJ per instruction at 32 nm, 5.8 at 22 nm),
because upstream McPAT loads every decoded line with 1024 maximum-width gates
(100 F NMOS each; its own note: "TODO: this number 1024 needs to be
revisited") -- more switched capacitance per instruction than a whole small
RISC core (RI5CY, Gautschi et al., IEEE TVLSI 2017, Table V: 26.28 uW/MHz at
65 nm and 1.08 V for the entire core).

- **Decoder output load: DERIVED.** A decoded line drives the core's own
  control structure: the pipeline-register sink count McPAT builds the core's
  clock tree over (`num_piperegs x num_pipelines`, the "pipeline registers" of
  the `[mcpat clock]` line), each a minimum-size gate.
- **Every core is an x86 core.** The workloads are x86 binaries the timing
  cores execute, in-order `alu_core` processing elements included, so McPAT
  decodes in two sequencer passes, keeps its x86 prefix decoder, and latches
  the micro-opcode in the pipeline.
- **No separate operand decoder.** McPAT's operand decoder re-decodes the
  register specifier that the register file's own row predecoder and decoder
  already decode inside every read and write CACTI charges; it is priced at
  zero, and each run prints the register file's decoder share as the evidence.
- **Guard.** Each core prints `[mcpat decode]`: decode energy per instruction
  plus the core's clock network per cycle at activity 1 (per instruction at
  IPC 1, the basis of the published figure; the run's own clock per
  instruction would charge idle and stalled cycles to the instructions) against Horowitz, ISSCC 2014, Fig. 1.1.9 (45 nm, 0.9 V: a simple
  in-order processor at 70 pJ/instruction, of which I-cache 25, register file 6
  and "Control" 39 pJ), scaled with Stillmaker & Baas, Integration 58 (2017),
  Eq. (6)/(9), Table 5 (45 nm HP/LP at 0.9 V to the run node's rows at its Vdd;
  22 nm uses the nearest row, 20 nm). The value is McPAT's, at the logic table,
  before any DRAM-periphery family factor. Until the clock-tree model is
  reviewed (user ruling 2026-10-25) the comparison is INFORMATION: the line
  prints the reference band and the ratio to it, and the checker reports
  `core.decode_clock_pJ_instr` unjudged (NOREF). Out-of-order cores have no
  reference (the figure is an in-order core).

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

**The host memory's controller is measured too (1.12.18, ruling 17).** The host
memory is replayed through Ramulator2 like the device memory (see
[memory.md](memory.md)), so the host-originated accesses are priced from the
replayed controller's counters, not from the stated 0.5 row-miss fallback:
their row-miss fraction is the host requests' own share of the controller's
row hits / misses / conflicts, and their activates are COUNTED (each ACT once
at the energy model's activate + precharge energy; the per-access terms then
carry the column burst only). Since 1.12.20 the replayed controller also models DRAM precharge power-down (memory.power_down; default ON for DDR3/DDR4/DDR5, where a published controller default exists, OFF elsewhere): a rank (a channel on a standard without a rank level) with no queued request and no command for memory.power_down_idle_clocks memory clocks (default 64, AMD BKDG 42301 p.366) enters power-down, and the first command after it waits tXP. The energy model credits the measured unit-cycle residency at IDD2P instead of the standby current; the Background line is the reduced figure and a separate 'power-down credit' line states the saving (never a negative ledger entry). Refresh still interrupts power-down (the counted refresh energy is unchanged). The array's refresh is charged as counted in
place of the datasheet duty, and the IDD3N share uses the controller's
measured bank-open time. Since 1.12.18 (user ruling 2026-10-23, "IDD2N, state interrupted") one refresh command on one IDD unit costs VDD x (IDD5 - IDD2N) x tRFC, plus VPP x (IPP5B - IPP2N) x tRFC where the record has an IPP row (DDR4, DDR5, GDDR6; on LPDDR5 the 1.8 V VDD1 rail in the IPP slot; user ruling 2026-10-25, second supply rails: DDR4 IPP5B is DERIVED from the distributed-refresh IPP5R as its IDD5B is from IDD5R): JEDEC measures IDD5 with every bank precharged and a REFab needs them precharged, so the refresh interrupts whatever state the unit held and its excess is over precharged standby (it was IDD5 - IDD3N). The datasheet duty it replaces is the same IDD2N-relative excess, so after the substitution every background state stands at IDD2N for the refresh duty and nothing is counted twice. HBM records: HBM2's IDD5 is IDD2N plus the measured SAFARI refresh excess (now priced exactly); HBM3's IDD2N = IDD3N, so its refresh is unchanged. On a shared array there is one controller, so these
array-wide terms are measured once over all traffic and charged once; the
refresh commands are charged to the HOST memory controller in McPAT (refresh
is the host controller's job, as on UPMEM, HBM-PIM and AiM; the PEs' memory
interfaces issue none), never to both. A decoupled host memory's own
controller gets its own counted refresh commands. Coherence-flush writebacks
go to the host memory controllers at flush time and count once in their
writes (`power.models.flush_writebacks_replayed`); with the host replayed,
their activates and row state are the replayed controller's. These energies are computed in the energy model
(`pimid_energy.h`, through `RamulatorWrapper`); the report passes the counts.
`system.hosts[].memory.dram_replay: false` restores the unmeasured host path.

Non-DRAM technologies (SRAM, STT_MRAM, PCM, RERAM) are charged in system scope
by the same function device scope uses (1.12.18): CACTI 7 for SRAM, NVSim for
the NVMs, per-access energy on the measured accesses of every class plus the
array leakage (x banks, after any measured array gating), in watts on the run's
wall clock. Their area was already in System Total Area and is not added twice.

## Node scaling of published per-op logic energies (1.12.33)

The published per-operation energies (FPU, integer ALU and multiplier, the
DMA adder) scale from their source node to the run node with the
published-silicon ratio of DeepScaleTool (Sarangi & Baas, ISCAS 2021; a fit
to Intel silicon, Holt ISSCC 2016), log-log between its table nodes. The fit
is voltage-less: each node at its nominal supply. Nodes outside 7-130 nm
keep the CACTI ratio (C_inv x Vdd^2), printed. CACTI arrays and wires stay
CACTI's, Vdd-aware.

## PE core energy: integer ops, bypass, store buffer (1.12.32)

(user rulings 2026-10-25, after a diagnostic run of standalone McPAT at
print level 5 on the kept XML.) Integer add/logic energy is linear in
width from Horowitz ISSCC 2014 Fig. 1.1.9's published 8- and 32-bit add
(0.03 / 0.1 pJ at 45 nm, node-scaled): 16 bit on the line between the
points, 64 bit = 2 x 32 bit (DERIVED); an ALU access the decoder does not
classify by width is one 64-bit add, and McPAT's Wattch-derived "1.15 nJ"
per-op literal is no longer used for the ALU or the DMA adder (multiply
keeps the published 8/32-bit points and McPAT's value elsewhere). The
bypass buses' length uses each unit's height = sqrt of its published area
(DERIVED; units without a published area keep McPAT's literal), and the
in-order FP bypass bus is one FP datum wide. An in-order device PE's
load/store structure is a store buffer: one write and one read per store,
none per load (DERIVED); OOO cores keep McPAT's queue. A device PE's
instruction store, data store and DMA energy are printed
(`[mcpat stores]`) so the PE is judged on its core alone, as RI5CY's
Table V is.

## Clock distribution (1.12.18)

Upstream McPAT ships its clock-network calls commented out and the class they
name does not exist, so no component carried clock-tree power. The McPAT fork
now prices one per core, cache, memory controller, NoC router and pipelined
link, following the clock model of D. Brooks, V. Tiwari, M. Martonosi,
"Wattch: A Framework for Architectural-Level Power Analysis and
Optimizations", ISCA 2000: global clock wiring, clock buffers and clocked
node load, E = C Vdd^2 per cycle. The wiring is an H-tree over the
component's McPAT area whose leaves reach each clocked sink, total length
L = 1.5 D (sqrt(N) - 1) for side D and N sinks (H-tree distribution after
Bakoglu, 1990, ch. 8; Wattch's own clock-line length is this sum at three
levels). Wire capacitance, repeaters (the clock buffers) and their leakage
come from CACTI's repeated-wire model at the run's node and device type, and
the pin load from McPAT's register cell, so the model scales with the node
through CACTI's tables and carries no constant of its own. The sinks are
only structure counts McPAT/CACTI already make: a core's pipeline registers,
a cache's or controller's array port registers (McPAT's own shared-cache
rule), a router's matrix-arbiter flip-flops, a link's inserted pipeline
registers. Pin energy McPAT already charges inside the pipeline is not
charged again.

(next, user rulings 2026-10-25) A CORE's tree spans its CLOCKED LOGIC only,
DERIVED as the pipeline-register area McPAT builds for the units that exist
plus the integer ALU(s), the multiplier/divider and the instruction decoders
(the `[mcpat clock] coreN: tree span` line prints the parts). The FPU and the
arrays (instruction and data stores, register files, queues, TLBs,
predictors) are not spanned: they time their own accesses inside CACTI, and
until this release their area put a 22 nm alu_core PE's flops about 18 um of
repeated global wire apart (core area 0.431 mm^2, 68% of it the FPU). The
pipeline-register count no longer carries upstream McPAT's unsourced x1.5
"control and interrupt registers" rule of thumb, and each stage latches the
opcode as an encoded micro-op field (its bit width) instead of one line per
opcode value. An in-order core's later stage registers are DERIVED from
what they carry, the field set of the textbook pipelined datapath (Patterson
& Hennessy, Computer Organization and Design, the five-stage
pipeline-register figure): ID/EX = control + PC + two source operands +
immediate + three register specifiers; EX/MEM = control + branch target +
zero flag + ALU result + store data + destination register; MEM/WB =
control + load data + ALU result + destination register, with the data
fields the core's integer width and the control field the encoded micro-op.
Upstream McPAT latched the whole instruction into ID/EX and 8*2*int_data_width
extra bits (sixteen data words, unsourced) into each of EX/MEM and MEM/WB.
The OOO branch has no such term (its stage registers already follow the
renamed operands and tags).

(next, user ruling 2026-10-25, F2) LEAF CLUSTERS. The register sinks of a
tree are grouped into leaf clusters of k = `power.clock.leaf_cluster_sinks`
(default 30, the OpenROAD TritonCTS sink-cluster size cap; IBM US 8,458,634
B2: a local clock buffer drives at most 24, 26 or 32 latches). The GLOBAL
H-tree, CACTI's repeated global wire, spans the tree area and ends at the
ceil(N/k) cluster buffers: L_g = 1.5 D (sqrt(N/k) - 1). Each cluster's LOCAL
net is unrepeated CACTI local wire, an H-tree to its k sinks, 1.5 d
(sqrt(k) - 1), over the cluster's area d^2. In a core the tree area is
clocked logic (the flops sit among that logic), so a cluster's area is its
share of it, A / ceil(N/k); in the other trees (caches, controllers,
routers, links, whose area includes their arrays) it is k McPAT register
cells. Each cluster has a LOCAL CLOCK BUFFER; (next, user ruling
2026-10-26, CLK2) it is a two-inverter clock buffer priced from published
liberty data: input C = C_load / 10 (C_load = the cluster's net + its
register clock pins; OpenROAD TritonCTS grows a sink cluster to at most 10x
the sink buffer's input capacitance), internal energy 0.5 x C_load x Vdd^2
per cycle, so input + internal = 0.6 x C_load x Vdd^2. A liberty clock
buffer at electrical effort 10 costs 0.49-0.59 of C_load Vdd^2 (Nangate45
BUF/CLKBUF, ASAP7 BUFx2-x6f, typical corner), and the leaf buffers of a
placed-and-routed 1,940-register RISC-V core (OpenROAD flow, Nangate45 and
ASAP7, buffers sized by load, no dummy loads) cost 0.61-0.64. Its leakage
is two CACTI inverters at the node. (Through 1.12.61 it was one inverter
sized at stage effort 4, input C_load / 4, output diffusion only.) The
global part (wire, repeaters, buffer inputs) switches every powered cycle;
the local nets, buffer internal energy and register pins are gated by the
measured activity
(`power.models.clock_global_local`). Each tree prints the split and the
ratio of its local load (nets + buffer outputs + pins) to the global
network itself (wire + repeaters), beside the published observation that
the local clock buffers' load is generally more than ~10x the clock
network's own (arXiv 1401.0015): printed, not judged.

(next, user ruling 2026-10-26, CLK1) In a core tree the local clock nets
are priced at MEASURED SILICON wire capacitance instead of CACTI's local
wire: 0.200 fF/um at 32 nm (Natarajan et al., IEDM 2008: the measured
median at M2, 112.5 nm pitch) and 0.169 fF/um at 22 nm (DERIVED: Auth et
al., VLSI 2012, "13-18% lower capacitance than 32nm", midpoint), log-log
between them. Silicon comb structures are measured with the neighbours
grounded, the expected coupling of a clock net among signal wires that
switch independently or stay quiet; CACTI's local wire carries a
worst-case Miller factor of 1.5 on both sidewalls (0.246 / 0.239 fF/um at
32 / 22 nm). Leaf clock nets are routed with default rules, unshielded
(OpenROAD clock_tree_synthesis applies its non-default rule to every level
except the leaf nets). At other nodes no published silicon value is
carried and the CACTI local wire is kept; the tree prints which it used.

(next, user ruling 2026-10-25 A) THE FLIP-FLOP is re-sourced from two
published open standard-cell libraries, node-scaled: Nangate 45 nm Open
Cell Library DFF_X1 (1.10 V) and ASAP7 DFFHQNx1 RVT (0.70 V), from their
liberty files (CK and D pin capacitance; CK internal energy per cycle with
Q held and with Q toggling; Q/QN and D internal energy per transition; the
smallest slew/load entry). Energies scale with Stillmaker & Baas 2017
Eq. (6)/(9), Table 5 (the nearest row: 22 nm -> 20 nm), capacitances as
energy / V^2; the value applied is the midpoint over the device-type row
pairs of the library NEAREST the node (Nangate at 22 and 32 nm, ASAP7 below
17.7 nm), and the bracket over both libraries is printed (`[mcpat dff]`).
Per register per cycle the clock costs C_CK V^2 + the CK internal energy
with Q held; a toggling cycle adds the CK difference, the Q and D internal
energies and half C_D V^2 (the next register's D pin). The same register
is used by McPAT's pipeline registers and by the clock tree's pins (one
cell for every core, an X1 flop); liberty energy includes short-circuit,
so no sckt factor is applied to it; leakage and area stay McPAT's. 22 nm:
CK pin 0.224 fF (McPAT's NAND2 register: 0.113 fF in a PE, 1.57 fF in the
host), clock 1.01 fJ/cycle (0.082 / 1.14), bracket over both libraries
CK [0.185, 1.31] fF, clock [0.83, 3.02] fJ/cycle. Core clock trees print a
local/global load ratio of 7.4-9.2x (PEs) and 3.2x (host). The die-level trunk keeps one leaf per
component tree root (its leaves are not registers). Runtime clock energy is gated by each component's MEASURED
activity (Wattch's conditional-clocking style cc2: clock power scales with
measured port usage), and the ungated value is printed beside it as a
reference; peak power is ungated. Not priced: the memory-controller
backend/PHY and router pipeline
registers (no register count exists for them; the run prints NO PUBLISHED
SOURCE FOUND), the link controller. `power.models.clock_network: false`
turns the model off.

PIMID 1.12.18, idle halt (user ruling 2026-10-23,
`power.models.idle_halt`): a core or PE with nothing to run is halted as
real chips are (x86 HLT / C1, ARM WFI) and its whole clock tree, global part
included, is off while halted; leakage stays. The running time is the
timing model's own per-core count (a device PE's busy cycles, a host core's
zsim unhalted cycles); McPAT prices the cores as one homogeneous group, so
the core tree's global part is charged times the group's mean running
fraction. Controllers, routers and caches keep serving and keep
global-on / local-gated.

PIMID 1.12.18, per-die clock trunk (`power.models.clock_die_trunk`): the
die-level tree joining the component trees is priced once per die (and
clock domain) with the same model: an H-tree over the die whose leaves are
the component trees' roots, CACTI Global wire at the die's node, charged
every cycle any component on the die is clocked. It ends at each
component's clock entry, so it overlaps no component tree, and it is its
own ledger entry per die (`device.<die>.clock_trunk`,
`<host>.die.clock_trunk`).

PIMID 1.12.19, host on-chip network (`power.models.host_noc`,
`system.hosts[].noc.topology`): the host CPU's uncore fabric is priced as
the topology the timing model ran -- by default the published Nehalem-EX
ring (Kottapalli and Baxter, Hot Chips 21, 2009: two counter-rotating
rings, stops shared by core/cache, 32 B data ring per direction, one stop
per clock), optionally the crossbar or a shared bus -- from the data
messages and hops the timing model measured; flit 256 b. It replaces the
leakage-only placeholder that `<host>.noc` carried through 1.12.18; control
messages are not priced on the ring and the crossbar (a lower bound); a
1-core host has no fabric. Ring link length (user ruling 2026-10-25):
2 x sqrt(host die area) / stops, the loop of the Nehalem-EX floorplan (two
columns of core/LLC-slice rows, the rings through the slice columns; Rusu,
ISSCC 2009 / JSSC 2010) on a square die -- DERIVED, no published stop spacing
in mm; McPAT's upstream 2-D mesh formula gave half of it.

(user ruling 2026-10-25, "Measured"), wire activity: every NoC link, bus
and TSV wire is charged C V^2 per 0->1 transition (the DSENT / ORION 2.0
physical convention), on the MEASURED data: zsim reads each fabric packet's
64 B line from the guest memory at the access, headed by PIMID's own 64-bit
packet header word with its real field values (user ruling 2026-10-26):
destination and source node 12 b each, type 2 b, tag ceil(log2 noc.mlp) b,
and on write requests the line address relative to the device window in the
remaining bits (34 at the defaults; counted as the line address's low bits);
message class and length are implied by the type. It is the same 64-bit header the fabric timing moves -- and counts the 0->1 transitions between successive flits of each
source port, per flit width (zsim groups nocWire, hostFabWire). Random data
gives 0.25 per wire-bit; McPAT's per-bit C V^2 is scaled by the measured value
at runtime (peak keeps the full charge). Unreadable runs (multi-process,
trace-driven, analytical NoC) use 0.25 and say so.

PIMID 1.12.18, per-die process (`system.devices[].process`,
`power.models.per_die_process`): each component is priced at the die it
sits on -- the McPAT fork gives every component the InputParameter
(node, CACTI device type) of its die; see the YAML reference.
1.12.18 (user ruling 2026-10-24, final): PEs on a LOGIC die -- a DDR-family
RANK/CHANNEL placement (a DIMM-buffer-style die), the HBM logic/base die, any
device whose PEs are priced in the logic family under the per-die process
rule -- stay on `hp` when `power.device_corner` is not written, and the WHOLE
logic die (PEs, NoC routers, memory controllers) is priced at one corner (one
die, one process). Two alternatives were tried and rejected: `lop` raised the
PE leakage (CACTI's 22 nm lop off-current exceeds hp's above ~60 C: 17.85 ->
29.66 mW/PE at 350 K) and `lstp` cut the leakage to 0.17 mW/PE but raised the
dynamic power to 16.1 mW/PE at one instruction per cycle, above the published
6.1-14.3 mW band. A written `power.device_corner`,
`system.devices[].process.logic_die.device_type` or
`power.mcpat_overrides.device_type` still chooses. The run prints a
clock-feasibility check at the corner in use (the clock is never changed): the
McPAT fork's `[mcpat timing]` line (every array CACTI sized for the core --
register files, buffers, DMA arrays -- against the cycle/access targets of the
PE clock, PASS/FAIL) and the PE scratchpad's CACTI access time against one PE
cycle. McPAT has no logic (ALU/bypass) delay model, so the datapath logic is
not checked, and the run says so.

## The host-device link (UCIe)

Since 1.12.18 an `interposer` link is priced by a UCIe link class inside the
McPAT fork's link controller model (`external/mcpat/iocontrollers.cc`); PIMID
only passes the parameters (package, lanes, lane rate, the die's process node)
and the measured crossing bytes. Sources:

- [UCIe11] D. Das Sharma, "The UCIe 1.1 Specification: Future Applications of Chiplets", UCIe Consortium, 2023 (uciexpress.org white paper), table "UCIe 1.0/1.1: Characteristics and Key Metrics": data rates 4, 8, 12, 16, 24, 32 GT/s; 16 lanes per
  cluster (standard) / 64 (advanced). (Its 0.25 / 0.5 pJ/b power-efficiency
  targets are no longer applied: they are specification targets with no node.)
- [HC23] UCIe Consortium, "UCIe Protocol", Hot Chips 2023 UCIe Tutorial ("All examples show 64B data width on FDI"; "Protocol Layer presents 64B of the Flit on FDI").
- [CDN26] D. T. Melek et al. (Cadence), "A 0.29 pJ/b 5.27 Tb/s/mm UCIe Advanced Package Link With 2.5-D CoWoS Interposer and a 0.52 pJ/b 0.448 Tb/s/mm UCIe Standard Package Link With Organic Substrate in 3 nm FinFET", IEEE JSSC vol. 61, no. 7, July 2026, DOI 10.1109/JSSC.2026.3651425: "The link operates under a core supply (VDD) of 0.75 V, and a high supply (VDDH) of 1.2 V, and a TX driver supply (VCCIO) of 0.45 V. One UCIe-AP module consumes a total of 297 mW of measured analog power, which corresponds to 0.29 pJ/bit analog power efficiency" (64 lanes x 16 Gb/s; PLL in the TX common block); standard package: "the PHY exhibits an analog power efficiency of 0.52 pJ/bit running at 16 Gb/s/pin for a 25 mm channel".
- [INT26] S. Mondal et al. (Intel), "A 48Gb/s/lane 1.24Tb/s/mm UCIe-Compliant Die-to-Die Link Over 30mm Standard Package", ISSCC 2026, paper 8.1, DOI 10.1109/ISSCC49663.2026.11409120: "Fabricated in 22nm FinFET CMOS"; "At VDD=0.7V (VDDH=0.85V for TX pre-driver)"; "energy efficiency of 1.2pJ/b (0.45, 0.69, and 0.06pJ/b in TX, RX, and PI+QH, respectively; a PLL is not integrated in this work)".
- [MS26] R. Shivnaraine et al. (Microsoft), "A 0.23pJ/b 24Gb/s Modular D2D Interface With Zero Wake Penalty Clock Gating in 3nm", ISSCC 2026, paper 8.3, DOI 10.1109/ISSCC49663.2026.11409276: "0.33pJ/b (0.23pJ/b PHY only) at 24Gb/s - power is quoted at 0.75V for 24Gb/s"; its digital interface "manages traffic flow control (credit based), performs calibration tasks during initialization, houses error correction, and redundancy/repair logic".
- [SB17] A. Stillmaker and B. Baas, "Scaling equations for the accurate prediction of CMOS device performance from 180 nm to 7 nm", Integration, the VLSI Journal, vol. 58, pp. 74-81, 2017, Table 2 (cross-check only).

**Two process nodes (user ruling 2026-10-23).** The per-bit energy is ANALOG
PHY + DIGITAL INTERFACE, from the row of the die's node (pJ/bit):

| node | package | analog PHY | digital interface | total |
|---|---|---|---|---|
| 3 nm | advanced | 0.29 [CDN26] | 0.10 [MS26] | 0.39 |
| 3 nm | standard | 0.52 [CDN26] | 0.10 [MS26] | 0.62 |
| 22 nm | standard | 1.2 [INT26] | 0.192 DERIVED (lower bound) | 1.392 |
| 22 nm | advanced | 0.669 DERIVED | 0.192 DERIVED (lower bound) | 0.862 |

- digital, 3 nm = 0.33 - 0.23 = 0.10 [MS26]. It is a UCIe-LIKE digital
  interface (flow control, calibration, error correction, repair), not the
  UCIe D2D adapter itself.
- digital, 22 nm DERIVED = 0.10 x E22/E7 = 0.10 x 1.923 = 0.192, the
  published DIGITAL chain (user ruling: "use the most accurate one, the other
  two as internal reference, calibrations, and guards"): CACTI C_inv x Vdd^2
  22 nm (0.172 fF, 0.8 V) / 32 nm (0.325 fF, 0.9 V) = 0.418 (each node's minimum
  inverter at its own feature size; until this release both were drawn at the
  run's 22 nm, giving 0.220 fF, 0.618 and 0.284), times [SB17]
  Table 2 FO4 inverter energy 32 nm 0.51 fJ / 7 nm 0.111 fJ = 4.595 ([SB17]
  has no 22 nm row). It is a LOWER BOUND: the published chain stops at 7 nm,
  and the 7 -> 3 nm step is taken as 1.
- Internal references, printed and never applied (user ruling 2026-10-24): (a)
  the analog node ratio, 0.10 x 1.2 / 0.52 = 0.231; (b) [SB17] Table 2's own
  20 nm row (Multi-Gate, 0.9 V, FO4 inverter 0.198 fJ), the nearest published
  node to 22 nm, in place of the CACTI step: 0.10 x 0.198 / 0.111 = 0.178. The
  former reference (b), a TSMC press chain, was derived from the applied value
  itself (circular) and is removed. Guard: the run prints a WARNING (not fatal)
  when the applied value falls outside [min, max] of the references
  (0.178 .. 0.231).
- analog, 22 nm advanced DERIVED = 1.2 x (0.29 / 0.52): Cadence's same-node
  advanced/standard package ratio applied to Intel's 22 nm standard package.
  Caveats, printed at runtime: 48 vs 16 Gb/s, PLL not integrated (Intel) vs
  included (Cadence), 30 vs 25 mm standard-package reach, two vendors.
- **Cross-check against McPAT** (printed, never charged): McPAT's own link
  controller dynamic for the same link (lanes x GT/s at the derived clock),
  per bit, beside the applied digital interface.
- **Node selection**: `power.link.process_node_nm` (22 or 3; anything else is
  refused at load; the key alone does not mark the link timing as configured). Absent, McPAT uses the node of the die it prices
  (`power.tech_node_nm`, default 22); a die at any other node is refused,
  naming the two supported nodes. 3 nm is not a CACTI node, so the key changes
  only the link's per-bit energy. `power.link.pj_per_bit_override` replaces the
  whole per-bit figure (analog + digital) and needs no row.

What McPAT computes for the interposer class (UCIe advanced package, 64 lanes,
16 GT/s):

- **Controller clock**: `power.link.controller_clock_mhz` when given;
  otherwise the D2D adapter clock, lanes x rate / (8 bits x 64 B FDI) =
  64 x 16 / 512 = 2000 MHz, derived in McPAT and printed by it.
- **link.phy** (analog PHY + digital interface): measured bytes crossed x 8 x
  (analog + digital) pJ/bit, charged once, at the end that prices the link, and
  booked as the ledger's own `link` entry. Peak (TDP) dynamic is the link at
  full rate, lanes x GT/s x pJ/bit. The UCIe PHY is a parallel AFE, so no SerDes
  term is charged.
- **link.controller**: McPAT's controller area and leakage. Its DYNAMIC term
  is not charged: McPAT's figure (Cadence ChipEstimate, "the PHY, the data link
  and transaction layer", modelled per lane) covers the same PHY and
  adapter-level logic the analog + digital pJ/bit prices, and no published
  figure splits it. The run prints the amount left out. NO PUBLISHED SOURCE
  FOUND: transaction-layer dynamic energy above the FDI; it is not priced.

PCIe, CXL, NVLink and UALink keep their per-bit path (the pJ/bit selected from
the cited band, bytes x pJ/bit inside McPAT, McPAT's SerDes dynamic removed).
NVLink and UALink need `power.link.controller_clock_mhz`: they have no published
controller clock, and the run is refused without one. `power.models.ucie_link:
false` restores the pre-1.12.18 interposer pricing.

## Totals and the power ledger (1.12.18)

Every reported total is the sum of its scope's components, each priced from
measured activity. The device `--- System Total ---` is logic (McPAT) plus
memory (array dynamic + static: background, VPP, counted refresh, PHY
static) over the run window; until 1.12.18 device scope printed the DRAM
terms beside a logic-only total. Each run prints a ledger: every term the
total adds, once, under a physical-component key with the model that priced
it (`logic.pe_cores`, `logic.noc`, `logic.memory_controllers`,
`device.<die>.clock_trunk`, `memory.array.dynamic`, `memory.array.static`;
in system scope also `link` and `<host>.memory_controllers_phy`). The run
stops (`[ledger] FATAL`, exit code 5) and reports no total when a key is
registered twice, when McPAT's components do not add up to McPAT's total,
when the ledger does not add up to the printed total, or when a component
the scope must price is missing. No share-of-peak convention prices anything:
a memory controller is McPAT leakage + its measured transactions + one
transaction-engine pass per counted refresh command
(`power.models.mc_command_ops`), and its idle clocking is the clock model's.
Every model added in 1.12.18 has a `power.models.*` switch (see the YAML
reference); a model switched off is printed as not priced by user option.

## Measured, and assumed

Measured from the run: element and host activity counts, cache accesses and
misses, interconnect packets and hops, memory reads and writes, cycles.

**Assumed -- worth knowing before quoting a result:**

- **Operations without a published energy.** The instruction mix is counted
  by class (since 1.11.10), and since 1.12.18 FP add/multiply/FMA and 8/32-bit
  integer add/logic/multiply are priced per operation from published energies
  (`power.models.fpu_published`, `power.models.int_alu_published`). FP
  divide/sqrt, x87, 16/64-bit integer operations and integer divide have no
  published value (the run prints NO PUBLISHED SOURCE FOUND) and keep McPAT's
  per-operation energy.
- **Functional-unit area (1.12.19, `power.models.fu_area_published`).**
  McPAT's FPU/ALU/MUL areas are literals (an ARM VFP+NEON block of 4.47 mm^2
  at 90 nm for an embedded FPU: 0.29 mm^2 at 22 nm with layout overhead,
  four times a whole 65 nm RI5CY core). They are replaced per unit by
  published cell areas chosen by datapath width: FP 32 bit 69 kGE (RI5CY
  transprecision FPU), FP 64 bit 247 kGE (Ariane transprecision FPU, device
  scope), 32-bit ALU 10 kGE and multiplier 18 kGE (S. Mach et al., "FPnew",
  IEEE TVLSI 2021, GF 22FDX, 1 GE = 0.199 um^2, Fig. 4 and Fig. 6, Table IV),
  scaled x (F / 22 nm)^2 (the feature-size rule McPAT uses for its own
  literal). Leakage follows the area. The divider is counted once (in the
  RI5CY ALU). 64-bit integer units and the host x86 FPU (FP/SIMD datapath >=
  128 bit) have no matching published area (NO PUBLISHED SOURCE FOUND) and
  keep McPAT's literal. The RI5CY FPU carries FP16 lanes and no divide/sqrt;
  the multiplier includes RI5CY's dot-product units: both printed. Every run
  prints the McPAT literal (reference) beside the value used.
- **Interface energy** per access is an inherited constant, not a measurement.
- **Power gating** has no temporal weighting of its own.
- **Outside the simulated chips.** Voltage-regulator and power-supply
  conversion loss, the board and cooling are not modelled; no total includes
  them.

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

A single-threaded workload is exact: repeated runs give bit-identical cycles
and access counts, so a regression can be gated by requiring bit-identical
output from one run.

A parallel workload is timed by the model, not by the host's interleaving.
Every core and PE keeps its own simulated clock, counted from its ROI
baseline, and every NoC record and DRAM request is stamped on that
ROI-relative clock. A thread woken by another, or taking a lock word another
released, first moves its clock to max(own, the waker's or releaser's time)
and takes that core's origin, so threads joined by a synchronisation share
one time axis. At each phase barrier the records are folded at the
consistent cut, the earliest ROI-relative time over the cores still running
or waiting (sleeping, exited and vacated cores are exempt). Epochs of
`simulation.det_epoch_phases` phases are frozen as they are folded, and an
access is priced from an earlier epoch's frozen per-pair NoC latency table
and DRAM service latency, read by epoch index from the access's own clock,
never from a live value. The run time and every time-based power term use
the ROI's elapsed wall-clock time, the latest ROI end on the cores' own
clocks. See [network.md](network.md) and the YAML reference
(`simulation.det_epoch_phases`).
