# PIMID YAML Configuration Reference

Complete reference for all YAML configuration keys supported by PIMID.

## Table of Contents

- [Top-Level Keys](#top-level-keys)
- [Loader Rules](#loader-rules)
- [Workload Configuration](#workload-configuration)
- [PIM Configuration](#pim-configuration)
  - [Processing Elements](#processing-elements-pimpe)
  - [PE Placement](#pe-placement-pimplacement)
  - [PE-to-Memory Mapping](#pe-to-memory-mapping-pimmapping)
  - [Distributed Memory Controllers](#distributed-memory-controllers-pimmc)
- [Core Record Overrides](#core-record-overrides-core)
- [Memory Configuration](#memory-configuration)
  - [Memory Timing Override](#memory-timing-override)
  - [Memory Controller](#memory-controller)
- [Cache Configuration](#cache-configuration)
  - [Cache-Wide Keys](#cache-wide-keys)
  - [CACTI Search](#cacti-search-cachecacti)
- [NoC Configuration](#noc-configuration)
  - [Per-Level Overrides](#per-level-overrides-noclevels)
  - [Bridge Overrides](#bridge-overrides-nocbridges)
- [Simulation Parameters](#simulation-parameters)
- [Synthetic Traffic](#synthetic-traffic-synthetic)
- [Power Analysis](#power-analysis)
  - [McPAT Overrides](#mcpat-overrides)
  - [Host-Device Link](#host-device-link-powerlink-legacy-powerpcie)
- [System Configuration](#system-configuration)
  - [Hosts](#hosts)
  - [Host Memory Path (co-sim)](#host-memory-path-co-sim)
  - [Host NoC (co-sim)](#host-noc-co-sim)
  - [Separate Host Memory (co-sim)](#separate-host-memory-co-sim)
  - [Devices](#devices)
  - [Host-Device Bridge (co-sim)](#host-device-bridge-co-sim)
  - [Coherence (co-sim)](#coherence-co-sim)
  - [Kernel Launch (co-sim)](#kernel-launch-co-sim)
  - [System Network](#system-network)
  - [Legacy Host Block](#legacy-host-block-host)
- [Enumerations](#enumerations)
- [Override Rules](#override-rules)
- [Examples](#examples)

---

## Top-Level Keys

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `scope` | string | `"device"` | Simulation scope: `device` or `system` (`cosim` accepted as deprecated alias for `system`) |
| `method` | string | `"exec"` | Simulation method: `exec` (execution-driven, the default), `trace` (replay a recorded trace; needs `--trace-file`), `trace-gen` (record a trace, no simulation; needs `--trace-file`) or `synthetic` (parametric traffic into Garnet, see [Synthetic Traffic](#synthetic-traffic-synthetic)). CLI `--method` overrides. Any other word is refused (rc 1). |
| `name` | string | `"PIMID_Simulation"` | Configuration name (appears in banner) |
| `description` | string | `""` | Configuration description |

---

## Loader Rules

These rules hold for every key on this page (1.11.106 unless a release is
named).

**Every key must be one the loader reads.** An unknown top-level section has
been refused since 1.11.76. Since 1.11.106 every nested key is checked as
well: a key the loader does not read is refused at load (rc 2), naming the
key and the nearest accepted key at that place, so a misspelt
`memory.technolgy` no longer runs the SRAM default in silence. The maps whose
contents are the user's own are exempt: `workload.env`,
`system.hosts[].workload.env`, `system.devices[].workload.env`,
`pim.mapping.map`, `pim.mc.groups` and `power.mcpat_overrides` (whose names
are checked separately, see [McPAT Overrides](#mcpat-overrides)). A
`system.hosts[]` or `system.devices[]` node accepts only the keys listed under
[Hosts](#hosts) and [Devices](#devices): a device-scope key written inside a
device node (`pim.pe.count`, `pim.mc.local_latency`, `pim.mapping`,
`noc.levels`, `memory.dram`, ...) is refused in this release. 1.11.107 accepts
the device-scope shape inside a node.

**A word is one YAML scalar.** Every string knob refuses a map, a sequence or
an empty value where a word is expected, naming the key path (rc 2). yaml-cpp
used to turn the first two into the key's default and the third into the word
`null`, in silence. Numbers and booleans have refused a value of the wrong
type since 1.11.90; a boolean also accepts `0` and `1`.

**One name per quantity.** Where one quantity has two names, giving both
refuses (rc 2), naming both; the later one used to win in silence. The pairs:

- `power.temperature_k` / `power.temperature_c`
- `technology.node_nm` / `system.tech_node_nm` / `power.tech_node_nm` (any two of the three)
- `pim.pe.core_type` / `pim.pe.type`
- `pim.pe.placement` / `pim.placement`
- `noc.virtual_channels_per_vn` / `noc.vcs_per_vnet`
- `memory.timing.read_latency_ns` / `memory.timing.subarray_read_ns`, and `memory.timing.write_latency_ns` / `memory.timing.subarray_write_ns`
- `noc.bridges` / `noc.gateways`
- `power.link` / `power.pcie`

The L0 words follow the same rule since 1.11.74: one of
`memory.subarrays_per_bank` / `subbanks_per_bank` / `mats_per_bank`, and one
of `noc.levels.subarray` / `subbank` / `mat`.

**A closed vocabulary refuses an unknown word.** Each row below lists the
words its key accepts; any other word is refused at load (rc 2 unless the row
says otherwise) instead of being replaced by a default.

---

## Workload Configuration

```yaml
workload:
  binary: ./path/to/binary
  args: ["--size", "1024", "--threads", "4"]
  env:
    OMP_NUM_THREADS: "4"
    LD_PRELOAD: "/path/to/lib.so"
  type: serial
  mpi_ranks: 4
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `workload.binary` | string | `""` | Path to workload binary. CLI `--workload` overrides this. |
| `workload.args` | list | `[]` | Command-line arguments (YAML list of strings). CLI `--workload` args override. |
| `workload.env` | map | `{}` | Environment variables injected into the workload process. In system scope a node's own `workload.env` joins this map (see [Hosts](#hosts)). |
| `workload.type` | string | `"serial"` | Workload type: `serial`, `openmp`, or `mpi`. |
| `workload.mpi_ranks` | int | `0` | Number of MPI ranks. 0 = auto (defaults to `pim.pe.count`; `host.num_cores` for a system-scope `PIMID_COSIM_NO_OFFLOAD` baseline, where the host runs the kernel). CLI `--mpi-ranks` overrides. |
| `workload.mpich_path` | string | - | Deprecated and ignored, with a warning: PIMID no longer uses `mpirun`. |

---

## PIM Configuration

### Processing Elements (`pim.pe`)

```yaml
pim:
  pe:
    type: alu_core
    count: 8
    frequency_mhz: 1000
    compute_factor: 1.0
    access_factor: 1.0
    throughput_factor: 1.0
    operand_width: 32
    energy_factor: 1.0
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `pim.pe.type` | string | `"in_order_core"` | PE core type. See [PE Types](#pe-types). |
| `pim.pe.core_type` | string | - | Alias for `pim.pe.type`; giving both is refused (1.11.106). |
| `pim.pe.count` | int | `4` | Number of processing elements. |
| `pim.pe.frequency_mhz` | int | `2000` | PE clock frequency in MHz. Overrides `system.frequency_mhz`. `noc.clock_mhz`, when given, overrides both. |
| `pim.pe.pg` | bool | `false` | Power-gate the PE cores and their private caches over their measured idle residency. The shared L2/L3 gate with them unless `cache.pg: false`. |
| `pim.pe.fp_emulation_cycles` | int | `0` | Cycles charged per floating-point-class instruction when `floating_point: false` (software emulation). `0` charges nothing; the run then reports how many FP instructions executed uncharged. A non-negative integer. |
| `pim.pe.arch_int_regs` | int | `16` | Architectural integer registers the power model prices (McPAT's register file). Default 16, the simulated x86-64 guest's count (since 1.11.97; it was 32). The timing model executes x86-64 whatever the value; a set value is printed as the user's. Must be >= 1. In system scope the per-node power path applies it to the host node only; a device node keeps McPAT's built-in 32. |
| `pim.pe.arch_fp_regs` | int | `16` | Architectural floating-point registers the power model prices; as `arch_int_regs`. |
| `pim.pe.element_bits` | int | - | WITHDRAWN: refused (rc 1). The element's datapath width is `pim.pe.operand_width`, which the timing model reads; a second name let the two halves disagree. |

**ALU Scaling Factors** (only meaningful for `alu_core`):

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `pim.pe.compute_factor` | double | `1.0` | Cycles-per-instruction multiplier. Higher = slower compute. |
| `pim.pe.access_factor` | double | `1.0` | Cycles per load/store. `0.0` = free local access (PUM). |
| `pim.pe.throughput_factor` | double | `1.0` | Parallelism divider on instruction count. |
| `pim.pe.bit_serial` | bool | `false` | Datapath model. `false` = bit-parallel (operand width has no cycle cost). `true` = bit-serial PUM (compute cost proportional to `operand_width`). |
| `pim.pe.issue_width` | int | `2` | In-order core issue width (uops issued per cycle, program order). Valid 1-6 (clamped to the 6-port FU model; out-of-range falls back to 2); practical range 1-4 -- real in-order cores are 2-3 wide, and beyond 4 the ports and RAW chains bind first. Applies to `in_order_core` only; env `PIMID_INORDER_WIDTH` overrides YAML. Since 1.11.89 McPAT prices the core at this same resolved width (both scopes; printed as `McPAT issue width N = the timing model's (...)`). |
| `pim.pe.operand_width` | int | `32` | Datapath width in bits, used by BOTH halves of the model. Timing: with `bit_serial: true` compute cost scales linearly with width (a W-bit op = W bit-steps); with `bit_serial: false` it has no cycle cost. Power/area: always sizes the register files, queue entries and result buses. The power model quantises to 32-bit granularity, so a narrower element is priced as 32-bit and says so. |
| `pim.pe.energy_factor` | double | `1.0` | Per-op energy scale factor (reporting only, does not affect timing). |
| `pim.pe.lanes` | int | `1` | Datapath replication. `1` = scalar, `W` = W-wide. Sizes the arithmetic units, register file and result bus in the power/area model. It does NOT speed the element up on its own -- the timing model expresses width through `throughput_factor`, so set both. Declaring lanes without throughput_factor warns, since the result is an element that pays for W lanes and runs like one. |
| `pim.pe.floating_point` | bool | `true` | Whether the element has a floating-point unit. Default true because three of the five kernels are FP32. WARNS when false: this removes the unit from the power description only -- the timing model never sees an opcode, so it will not charge software emulation. Honest for an integer kernel, not for a floating-point one. |
| `pim.pe.imem_bytes` | int | `4096` | Size of the element's resident instruction memory. Spans a command-driven in-bank engine (hundreds of bytes) to a programmable near-bank one (kilobytes); minimum 64. This is the axis that decides which kernels fit on an element. |

**Cycle model**: BBL cycles = `instructions * compute_factor / throughput_factor`, load/store = `access_factor` cycles each.

**What the compute unit is, and is not.** All PE types consume the same x86-64
instruction stream: QEMU executes the guest binary and the plugin reports retired
instruction counts and load/store addresses. The compute unit does not decode --
it charges every instruction the same scaled cost, so it models no instruction
set and cannot distinguish a floating-point operation from an integer one. What
it does model, and what these knobs describe, is the cost of an operation and the
cost of reaching data: the memory-interface path, locality, and the in-memory
network hierarchy. Use it for memory-bound kernels, which is what it is for.

### PE Placement (`pim.placement`)

```yaml
pim:
  placement:
    level: BANK
    connection: shared_io
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `pim.placement.level` | string | `"BANK"` | Where PEs sit in the memory hierarchy. See [Placement Levels](#placement-levels). Case-folded since 1.11.106 (`bank` = `BANK`); a word that names no tier is refused. |
| `pim.placement.connection` | string | `"shared_io"` | PE-memory connectivity: `shared_io` (PE shares memory org's NI) or `separate_endpoints` (PE has own NI). |
| `pim.placement.local_link_latency` | int | `2` | Local link latency in cycles (only for `separate_endpoints`). |

**Placement determines the hierarchy position.** PEs at `BANK` level are at hierarchy level L1 (bank). Non-`HOST_MC` placements require a `pim.mc` section.

The same block is accepted nested as `pim.pe.placement` (`pim.pe.placement.level`,
`.connection`, `.local_link_latency`; the form most examples use). Giving both
`pim.placement` and `pim.pe.placement` is refused (1.11.106).

### PE-to-Memory Mapping (`pim.mapping`)

```yaml
pim:
  mapping:
    mode: uniform
    pes_per_mem_org: 1        # 1 PE per memory org (1:1)
    # OR
    mem_orgs_per_pe: 4        # 4 memory orgs per PE (1:4)
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `pim.mapping.mode` | string | `"uniform"` | Mapping mode: `uniform` (auto-computed) or `explicit` (user-specified table). |
| `pim.mapping.pes_per_mem_org` | int | `0` | M:1 mapping -- M PEs share each memory org. Mutually exclusive with `mem_orgs_per_pe`. |
| `pim.mapping.mem_orgs_per_pe` | int | `0` | 1:N mapping -- each PE covers N memory orgs. Mutually exclusive with `pes_per_mem_org`. |
| `pim.mapping.map` | list | `[]` | Explicit mapping (mode=`explicit`). Each entry: `{pe: <id>, mem_orgs: [<ids>]}`. |

If neither `pes_per_mem_org` nor `mem_orgs_per_pe` is set, PIMID auto-derives a 1:N mapping.

### Distributed Memory Controllers (`pim.mc`)

```yaml
pim:
  mc:
    type: simple
    pes_per_mc: 1
    local_latency: -1         # -1 = auto from technology
    bandwidth_mbs: -1         # -1 = auto from technology
    groups:
      - id: 0
        type: simple
        bandwidth_mbs: 12800
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `pim.mc.type` | string | `"simple"` | PE-MC type: `simple` (M/D/1 queuing always active; the only PE-MC model). Another word is accepted with a WARNING that it has no separate implementation; under a `system.devices[]` node anything but `simple` is refused (1.11.106). |
| `pim.mc.pes_per_mc` | int | `1` | Number of PEs sharing each memory controller. |
| `pim.mc.local_latency` | int | `-1` | Local access latency in cycles. `-1` = auto-derived from memory technology. |
| `pim.mc.bandwidth_mbs` | int | `-1` | Bandwidth in MB/s . `-1` = auto-derived from Ramulator. |
| `pim.mc.groups` | list | `[]` | Per-group overrides. Each entry: `{id, type, local_latency, bandwidth_mbs}`; a group `type` other than `simple` warns, as `pim.mc.type` does. |
| `pim.mc.placement` | string | `"with_core"` | Where each controller sits: `with_core` (beside the PE and its caches) or `standalone` (its own NoC endpoint, one per memory organisation, so even a local access takes one extra hop). Any other word is refused. |
| `pim.mc.clock_gear` | int | `1` | Controller clock McPAT prices, as the DRAM preset's CK x this gear: `1` (an on-die controller) or `2` (a gear-2 controller); any other value is refused (rc 1). A non-DRAM memory has no CK: its controller is priced at half the core clock and the key does not apply. |
| `pim.mc.epoch_replay` | bool | `true` | Replay every memory interface's DRAM requests through Ramulator2 once per epoch and price the previous epoch's measured service latency. Applies to a DRAM technology on the `ramulator` controller, and not to MPI workloads yet (they keep the deterministic M/D/1 service). `false` keeps the analytical (M/D/1) service. The run prints which one it used. |
| `pim.mc.pg` | bool | `false` | Power-gate the memory controller LOGIC over its measured idle residency. It gates neither DRAM (see `memory.power_down`) nor an array (see `memory.array_pg`). |

**Required** for all placement levels except `HOST_MC`. Without PE-MCs, every memory access would traverse the full hierarchy to the host MC.

---

## Core Record Overrides (`core`)

The timing cores' front-end penalties come from the core part record,
`params/core/default.yaml` (since 1.11.97), which records each value's
derivation. A `core:` key replaces one field for the run -- every core of that
type, host and device -- and the run prints it as the user's number.

```yaml
core:
  in_order:
    mispredict_penalty_cycles: 7
    resteer_penalty_cycles: 4
  ooo:
    mispredict_penalty_cycles: 17
    fetch_width_bytes: 16
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `core.in_order.mispredict_penalty_cycles` | int | record (`7`) | `in_order_core` refill bubble for a conditional mispredict, and for an indirect jump, call or return whose target the BTB/RAS missed (all resolved at execute). Also the in-order pipeline depth McPAT prices. Range 0..1000. Env `PIMID_INORDER_MISPRED_PENALTY` overrides it inside the core. |
| `core.in_order.resteer_penalty_cycles` | int | record (`4`) | `in_order_core` decode-depth resteer: a taken direct branch that misses the BTB. Range 0..1000, and not above `mispredict_penalty_cycles`. Env `PIMID_INORDER_RESTEER_PENALTY` overrides it inside the core. |
| `core.ooo.mispredict_penalty_cycles` | int | record (`17`) | `ooo_core`: how far the wrong path is fetched after a mispredict (the redirect itself is charged by resolution time). Wrong-path depth = ceil(penalty x fetch width / line size) lines. Range 0..1000. |
| `core.ooo.fetch_width_bytes` | int | record (`16`) | `ooo_core` fetch width in bytes per cycle. Range 1..64, and not above `system.cache_line_size`. |

A value outside its range is refused at load (rc 1). See [cores.md](cores.md)
for the pipelines these values feed.

---

## Memory Configuration

```yaml
memory:
  technology: DDR4
  banks: 4
  subarrays_per_bank: 4
  latency: -1                 # -1 = auto from external models
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `memory.technology` | string | `"SRAM"` | Memory technology. See [Memory Technologies](#memory-technologies). |
| `memory.banks` | int | `4` | Number of memory banks. Honoured for SRAM and the NVMs. **INERT for every DRAM technology**, which always simulates its preset's full bank count -- see the table below. |
| `memory.subarrays_per_bank` | int | derived | Count of the tier one level below the bank. The `4` in the config struct is only a starting value: on a DRAM technology the run DERIVES the count as the preset's `bank_rows` / subarray height (512 rows, 1024 on HBM), and since 1.11.73 a non-DRAM technology takes it from its own array model (CACTI for SRAM, NVSim for the NVMs). Setting this key overrides the derivation, and the run says which key set it. Family spellings `memory.subbanks_per_bank` (SRAM) and `memory.mats_per_bank` (NVM) are accepted since 1.11.74 -- give exactly one, and the wrong family's word is refused. |
| `memory.latency` | int | `-1` | Override memory latency in cycles. `-1` = auto from external models. |
| `memory.dram.device_width` | string | unset | DRAM device width for the DDR family (`x4`/`x8`/`x16`). Selects the org preset row and, since 1.11.66, the org Ramulator instantiates -- the two are bound by a live shape check. UNSET means each technology's own JEDEC default, which is x8 for DDR3/DDR4/DDR5 and x16 for LPDDR5 and GDDR6; it is not a literal x8. LPDDR5 accepts only x16 and GDDR6 only x8/x16 (their other presets do not exist upstream), and HBM refuses the key outright -- a stacked part has no x4/x8/x16 width. An unrecognised value is refused since 1.11.76. On a non-DRAM technology (SRAM, the NVMs) the key is refused (1.11.106; it used to pass the width check and be ignored). |
| `memory.dram.ddr5_speed_grade` | int | `4800` | DDR5 speed grade, one of `3200`, `4800`, `5600` (since 1.11.66). Selects ONE part: timing preset (`DDR5_3200AN` / `DDR5_4800B` / `DDR5_5600B` -- the B bins at 4800/5600 because the Micron MT60B dies whose IDD currents are used are -48B/-56B parts), org (8 Gb at 3200, the 16 Gb MT60B die at 4800/5600), IDD row (Micron Rev A / Rev D addenda), and data rate. The 3200 row's IDD is unsourced (no held datasheet has a 3200 column) and is stated so. Any other value is a FATAL configuration error. |
| `memory.dq_turnaround` | bool | `true` | Charge the shared DQ bus a direction-reversal penalty (JEDEC tWTR) between a write and a read. Since 1.11.65 the penalty is DERIVED as nWTR_L x tCK from the Ramulator timing preset the run selects (DDR3 7.5 ns, DDR4 7.5, DDR5 10.0, LPDDR5 12.5, GDDR6 6.28, HBM2 8.33, HBM3 8.125 at the shipped presets; GDDR6 read 11.0 in 1.11.65 from a wrong clock, corrected in 1.11.66) and printed at load; earlier releases carried a hand-written table that had drifted 1.75x low for GDDR6. Set `false` for a design with a dedicated PIM interconnect and no shared bus. |
| `memory.subarray_height` | int | per technology (`512`; `1024` on HBM2/HBM3) | Rows per subarray on a DRAM technology: the subarray count is derived as the preset's bank rows / this height unless `memory.subarrays_per_bank` is given. `0` or absent = the per-technology value (measured die geometry). Not used by SRAM or the NVMs. |
| `memory.organization.bank_groups` | int | the technology's | Bank groups per chip in the in-memory hierarchy. `0` or absent = the technology's JEDEC organisation at the selected device width. |
| `memory.organization.banks_per_group` | int | the technology's | Banks per bank group in the in-memory hierarchy; as `bank_groups`. |
| `memory.ranks_per_channel` | int | `1` | Ranks per channel: the fanout of the rank tier. Refused above 1 on HBM2/HBM3, which fold the channel count into chips per rank. |
| `memory.ports_per_bank` | int | `1` | Physical read/write ports per bank (a multiport SRAM; DRAM and NVM parts have one). Bounds the PE memory interfaces (organisations x ports) and sets the array model's port count. A value below 1 is taken as 1. |
| `memory.bank_kb` | int | `64` | SRAM/NVM bank size in KB; device capacity = `memory.banks` x this size. Must be >= 1. A set value is printed as the user's. |
| `memory.power_down` | bool | `false` | DRAM precharge power-down (JEDEC IDD2P) over the measured idle residency. A JEDEC state, not gating: the array stays powered and refreshed. |
| `memory.power_down_threshold_ns` | double | the sourced tXP | Power-down entry threshold (ns) for the gap-measured residency. `<= 0` or absent = the generation's sourced tXP (DDR3/DDR4 6 ns, DDR5/LPDDR5 7.5 ns). GDDR6 and HBM have no tabulated tXP: without this key their gap-measured residency is not used and the phase-granular estimate is reported, with a line saying so. |
| `memory.array_pg` | bool | `false` | Array-PERIPHERY power gating over the measured idle residency: retention-free on the NVMs; on SRAM the periphery gates and the cell array keeps its full leakage. Refused on every DRAM technology (a DRAM cell must be refreshed, so the array cannot be gated), and refused together with any `cache.cacti.power_gating.*` flag (one leakage gated twice). |

Per-technology bank counts, and what a request of `banks: 16` actually runs.

CORRECTED IN 1.11.84, on FOUR of its eight rows. The previous version said
`banks: 16` ran "16, as asked" on DDR3, DDR4, DDR5-3200, LPDDR5 and GDDR6.
Measured, it runs 64, 128, 128, 16 and 32. Only the LPDDR5 row was right, and
it was right by coincidence: LPDDR5's preset count IS 16, so nothing is
substituted there. HBM2, HBM3 and DDR5-4800 were already correct.
`memory.banks` is INERT for every DRAM technology: the organisation comes
from the preset, and the knob only ever decided whether a warning printed.
Measured at config-load scope on 1.11.83 -- DDR4 at `banks:` 16, 32, 128, 256
and 1024 produces BYTE-IDENTICAL output, and `banks: 1` differs from
`banks: 16` by exactly one line, the warning itself. DDR3, LPDDR5 and GDDR6
behave the same (16 against 64: zero differing lines).

Since 1.11.84 the run says so on every DRAM cell whose `memory.banks` differs
from the preset's count, which includes every corpus config (they set 16).
Nothing about those runs changed -- they always simulated the preset's count;
only the silence ended.

| technology | minimum per chip | what `banks: 16` ACTUALLY runs |
|---|---:|---|
| DDR3 | 8 (8 banks/BG x 1 BG) | **64** (8 x 1 x 8 chips/rank) |
| DDR4 | 16 (4 x 4) | **128** (4 x 4 x 8 chips/rank) |
| DDR5 at grade 3200 | 16 (2 x 8) | **128** (2 x 8 x 8 chips/rank) |
| DDR5 at grade 4800 / 5600 | 32 (4 x 8) | **256** (the 16 Gb part's 8 chips x 32) |
| LPDDR5 | 16 (4 x 4) | 16 -- its preset count IS 16, so no substitution |
| GDDR6 | 16 (4 x 4) per channel | **32** (4 x 4 x 2 chips/rank) |
| HBM2 | 32 (4 x 8) per channel | **256** |
| HBM3 | 32 (4 x 8) per channel | **512** |

DDR5 joined the substituting group in 1.11.72: its default part moved to
4800B / 16 Gb in 1.11.66, and that die carries 8 bank groups of 4 banks
where the 8 Gb die carries 8 of 2. A config that says `banks: 16` on
DDR5 therefore simulates 256, and says so.

### Memory Timing Override

To override the external model (Ramulator2/CACTI/NVSim) for memory parameters, **all 5 values must be provided**:

```yaml
memory:
  timing:
    read_latency_ns: 10.0
    write_latency_ns: 10.0
  energy:
    read_energy_nj: 3.5
    write_energy_nj: 3.5
  power:
    static_power_mw: 100.0
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `memory.timing.read_latency_ns` | double | `-1.0` | Read latency (ns). Part of 5-param override. |
| `memory.timing.write_latency_ns` | double | `-1.0` | Write latency (ns). Part of 5-param override. |
| `memory.energy.read_energy_nj` | double | `-1.0` | Read energy (nJ). Part of 5-param override. |
| `memory.energy.write_energy_nj` | double | `-1.0` | Write energy (nJ). Part of 5-param override. |
| `memory.power.static_power_mw` | double | `-1.0` | Static/leakage power (mW). Part of 5-param override. |

Alternative key names: `memory.timing.subarray_read_ns` (= `read_latency_ns`) and `memory.timing.subarray_write_ns` (= `write_latency_ns`). Giving both names of one value is refused (1.11.106).

**Partial override warning**: Providing fewer than 5 parameters triggers a warning; external models are used instead.

### Memory Controller

```yaml
memory:
  controller:
    type: auto                # auto-derived from technology
    bandwidth: "25.6 GB/s"
    ramulator_config: /path/to/ramulator.yaml
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `memory.controller.type` | string | `"auto"` | Controller type: `auto`, `simple`, `weavesimple`, `ramulator`. `md1`/`weavemd1` were removed (M/D/1 queuing is always active in `simple`): they are accepted with a warning and run `simple`. `ramulator` on a non-DRAM technology is refused (1.11.106; it used to run `simple`). Any other word is refused. |
| `memory.controller.bandwidth` | string | `""` | Bandwidth with units. Formats: `"25.6 GB/s"`, `"19200 MB/s"`, `"1 TB/s"`, `"6400000 KB/s"`, `"6400"` (bare number = MB/s). A value that does not parse is refused (1.11.106; it used to keep the default with a warning). On SRAM and the NVMs a value above the array's own cap (banks x line / access time from the array model, printed on the `[bw]` line) is refused (1.11.106; it used to be clamped in silence). On a DRAM technology under `simple`/`weavesimple` a value above the part's rank bandwidth is clamped to it with a warning. |
| `memory.controller.bound_latency` | int | `-1` | Bound latency for Weave controllers (cycles). |
| `memory.controller.ramulator_config` | string | `""` | Path to custom Ramulator2 YAML config. Overrides auto-generated config. |

**Auto-derivation rules** (when `type: auto`):
- DRAM technologies -> `ramulator` (cycle-accurate Ramulator2)
- SRAM -> `simple` (fixed latency from CACTI)
- NVM (STT-MRAM, PCM, ReRAM) -> `simple` (fixed latency from NVSim)
- In-order/out-of-order cores auto-upgrade a `simple` controller to `weavesimple`

---

## Cache Configuration

```yaml
cache:
  l1d:
    size_kb: 32
    ways: 8
  l1i:
    size_kb: 16
    ways: 4
  l2:
    enabled: true
    size_kb: 2048
    ways: 16
    count: 1               # >1 = clustered L2s
  l3:
    enabled: false
    size_kb: 4096
    ways: 16
```

### Cache-Wide Keys

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `cache.mode` | string | `rw` | Mode of the characterization cache (the warehouse of CACTI/NVSim results, not a simulated cache): `rw`, `ro`, `wo` or `off`, with the aliases `on` (= `rw`), `read-only` / `readonly` (= `ro`), `write-only` / `writeonly` (= `wo`) and `none` / `disable` / `disabled` (= `off`), any case. Any other word is refused (1.11.106; it used to run `rw` in silence), whether it comes from this key, `--cache` or `PIMID_CACHE_MODE`. The command line (`--cache`, `--no-cache`) wins over the environment, which wins over this key. See [cache_warehouse.md](cache_warehouse.md). |
| `cache.dir` | string | `<pimid>/cache` | Warehouse root directory. Absent: `cache/` in the PIMID tree, located from the running binary. `--cache-dir` and `PIMID_CACHE_DIR` override it. |
| `cache.enabled` | bool | `true` | `false` = `mode: off`. Read only when `cache.mode` is absent. |
| `cache.pg` | bool | follows `pim.pe.pg` | Whether the shared L2/L3 power-gate (over their own measured residency) when `pim.pe.pg` gates the PEs. It takes effect only together with `pim.pe.pg: true` in device scope: `false` then keeps the shared caches ungated; with `pim.pe.pg` off nothing is gated, whatever this key says. Read outside the `cache.l2` block since 1.11.106 (it used to be read only when a `cache.l2` section was present). |

### L1 Data Cache (`cache.l1d`)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `cache.l1d.size_kb` | int | `32` | L1D size in KB. |
| `cache.l1d.ways` | int | record (`8`) | L1D associativity. Absent: the cache record's (`params/cache/default.yaml`). |
| `cache.l1d.banks` | int | record (slice rule) | Banks CACTI characterises the level with. Absent: the cache record's slice rule, clamp(size / 2 MB, 1, 32), so one bank at 2 MB and below; an integer fixes the count. A count outside CACTI's 1..32 is refused. A cache above 64 MB is built as 2 MB slices of one bank each and does not use the key. |
| `cache.l1d.latency_ns` | double | `-1.0` | Override CACTI-derived latency (ns). Requires all 3 override params. |
| `cache.l1d.energy_nj` | double | `-1.0` | Override access energy (nJ). |
| `cache.l1d.static_power_mw` | double | `-1.0` | Override leakage power (mW). |

### L1 Instruction Cache (`cache.l1i`)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `cache.l1i.size_kb` | int | `16` | L1I size in KB. |
| `cache.l1i.ways` | int | record (`4`) | L1I associativity. Absent: the cache record's. |
| `cache.l1i.banks` | int | record (slice rule) | As `cache.l1d.banks`, for the L1I. |
| `cache.l1i.latency_ns` | double | `-1.0` | Override latency (ns). |
| `cache.l1i.energy_nj` | double | `-1.0` | Override energy (nJ). |
| `cache.l1i.static_power_mw` | double | `-1.0` | Override leakage (mW). |

### L2 Cache (`cache.l2`)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `cache.l2.enabled` | bool | `true` | Enable L2 cache. |
| `cache.l2.size_kb` | int | `2048` | L2 size in KB (per instance if clustered). |
| `cache.l2.ways` | int | record (`8`) | L2 associativity. Absent: the cache record's 8 (since 1.11.95; the device-scope timing model used to build 16 ways while McPAT priced 8). |
| `cache.l2.banks` | int | record (slice rule) | As `cache.l1d.banks`, for the L2. |
| `cache.l2.count` | int | `1` | Number of L2 instances. `>1` = clustered (independent L2s). |
| `cache.l2.latency_ns` | double | `-1.0` | Override latency (ns). |
| `cache.l2.energy_nj` | double | `-1.0` | Override energy (nJ). |
| `cache.l2.static_power_mw` | double | `-1.0` | Override leakage (mW). |

### L3 Cache (`cache.l3`)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `cache.l3.enabled` | bool | `false` | Enable L3 cache (requires L2 to be enabled). |
| `cache.l3.size_kb` | int | `4096` | L3 size in KB. |
| `cache.l3.ways` | int | record (`16`) | L3 associativity. Absent: the cache record's. |
| `cache.l3.banks` | int | record (slice rule) | As `cache.l1d.banks`, for the L3. |
| `cache.l3.latency_ns` | double | `-1.0` | Override latency (ns). |
| `cache.l3.energy_nj` | double | `-1.0` | Override energy (nJ). |
| `cache.l3.static_power_mw` | double | `-1.0` | Override leakage (mW). |

**Cache override rule**: All 3 params (`latency_ns`, `energy_nj`, `static_power_mw`) must be provided per cache level to override CACTI. Partial overrides are ignored with a warning.

**Note**: `alu_core` PEs skip cache hierarchy creation entirely (no L1/L2/L3).

### CACTI Search (`cache.cacti`)

CACTI's design-space search and its power-gating model, as knobs (1.11.95).
They apply to every CACTI solve PIMID makes through its CACTI wrapper (the
cache latencies and the SRAM array). The defaults are CACTI's shipped
`cache.cfg`, and every run prints the search it used on a `[cacti] search:`
line.

```yaml
cache:
  cacti:
    objective: "0:0:0:100:0"
    deviate: "20:100000:100000:100000:100000"
    optimize: ED2
    power_gating:
      array: false
      perf_loss: 0.01
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `cache.cacti.objective` | string | `"0:0:0:100:0"` | CACTI's optimisation weights as five integers `d:dp:lp:ct:a` (delay, dynamic power, leakage power, cycle time, area: the `cache.cfg` order). Anything else is refused (rc 1). |
| `cache.cacti.deviate` | string | `"20:100000:100000:100000:100000"` | CACTI's maximum allowed deviation per term, in the same five-integer form. |
| `cache.cacti.optimize` | string | `ED2` | Search target: `ED2` (or `ED^2`), `ED` or `NONE`. Anything else is refused (rc 1). |
| `cache.cacti.power_gating.array` | bool | `false` | CACTI power-gates the array inside the characterisation. |
| `cache.cacti.power_gating.bitline_floating` | bool | `false` | CACTI floats the bitlines of an idle array. |
| `cache.cacti.power_gating.wordline` | bool | `false` | CACTI gates the wordline drivers. |
| `cache.cacti.power_gating.columnline` | bool | `false` | CACTI gates the column lines. |
| `cache.cacti.power_gating.interconnect` | bool | `false` | CACTI gates the interconnect. |
| `cache.cacti.power_gating.perf_loss` | double | `0.01` | The delay CACTI's gating may cost, as a fraction; must lie in (0, 1) (refused otherwise, rc 2). |

The `power_gating` flags read as booleans since 1.11.106 (`0`/`1` accepted,
any other non-boolean refused; such a value used to become `false` in
silence). Any flag on together with `memory.array_pg: true` is refused (rc 1):
one leakage cannot be reduced twice, by CACTI's gating inside the
characterisation and by PIMID's residency-based gating outside it.

---

## NoC Configuration

```yaml
noc:
  topology: MESH_2D
  routing: XY
  model: detailed
  router_latency: 1
  link_latency: 1
  vcs_per_vnet: 4
  buffers_per_vc: 4
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `noc.topology` | string | `"MESH_2D"` | Network topology: `MESH_2D` (alias `MESH`), `TORUS_2D`, `RING`, `CROSSBAR`, `FAT_TREE`, `BUS`, `H_TREE` or `CUSTOM`, in any case, for every memory family. Any other word is refused (1.11.106; on an SRAM/NVM device it used to run a 1 x N mesh with XY routing in silence). A DRAM device then applies its own rule: the tier tree, or the in-die `MESH_2D` / `RING` / `CROSSBAR` of 1.11.103. See [NoC Topologies](#noc-topologies). |
| `noc.routing` | string | `""` | Routing algorithm. Empty = auto-derived from topology. See [Routing Algorithms](#routing-algorithms). |
| `noc.model` | string | `"detailed"` | Network model: `detailed` (Garnet cycle-accurate; the default) or `analytical` (closed-form hop-count + M/D/1 + MLP). No other values are accepted. |
| `noc.router_latency` | int | `1` | Router traversal latency in cycles. |
| `noc.link_latency` | int | `1` | Link traversal latency in cycles. |
| `noc.mlp` | int | `10` | Memory-level-parallelism intensity `M` of the analytical model (`t_eff = max((L + W_q)/M, P*D/c)`). Absent: 10 for every core type (calibrated for `alu_core` against `detailed`; a placeholder for the cached cores). Must be >= 1: a smaller value is refused (1.11.106; it used to be clamped to 1 in silence). |
| `noc.vcs_per_vnet` | int | `4` | Virtual channels per virtual network. |
| `noc.virtual_channels_per_vn` | int | `4` | The same setting as `noc.vcs_per_vnet` under its other name; giving both is refused (1.11.106). |
| `noc.buffers_per_vc` | int | `4` | Buffers per virtual channel. |
| `noc.topology_file` | string | `""` | Path to topology file (required for `CUSTOM` topology). |
| `noc.routing_table_file` | string | `""` | Path to routing table file (for `TABLE` routing). |
| `noc.ring_direction` | string | `bidirectional` | `RING` direction: `bidirectional` (`bi`) or `unidirectional` (`uni`), any case. Any other word is refused. |
| `noc.clock_mhz` | int | unset | Sets the WHOLE device clock (`sys.frequency`), not a separate NoC clock domain: this tree models one clock for the element, its caches and its fabric. Overrides `system.frequency_mhz` and `pim.pe.frequency_mhz`; the run prints a NOTE naming the key it overrode. |
| `noc.flit_size_bits` | int | `128` | Garnet flit width in bits, a multiple of 8 between 8 and 4096 (refused otherwise, rc 1). Reaches Garnet's network-interface flit and the topology emitter (a ladder rung runs at its true width up to the flit). |
| `noc.header_bits` | int | `64` | Data-message header in bits. The derived data message is `system.cache_line_size` x 8 + this (576 bits for 64 B lines). Must be >= 0. |
| `noc.data_message_bits` | int | derived | Data-message size in bits. `0` or absent = `system.cache_line_size` x 8 + `noc.header_bits`. Sets a packet's flit count (message bits / flit width). |
| `noc.control_message_bits` | int | `64` | Accepted but INERT, with a warning: every fabric injection is a Data message, so no Control message is ever sized by it. |
| `noc.pg` | bool | `false` | Power-gate the device fabric over its measured idle residency. |

### Per-Level Overrides (`noc.levels`)

The 7-level memory hierarchy can have per-level network overrides:

```yaml
noc:
  levels:
    bank:
      model: simple
      link_width_bits: 128
      frequency_ghz: 2.4
    chip:
      model: detailed
      router_latency: 3
      virtual_channels_per_vn: 8
```

Valid level names: `subarray`, `bank`, `bank_group`, `chip`, `rank`, `channel`, `system`.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `noc.levels.<level>.model` | string | `"detailed"` | Per-level model: `detailed` (default) or `simple` (per-level analytical path). |
| `noc.levels.<level>.link_width_bits` | int | `-1` | Link width in bits. `-1` = auto from technology. |
| `noc.levels.<level>.frequency_ghz` | double | `-1.0` | Link frequency in GHz. `-1` = auto. |
| `noc.levels.<level>.latency_cycles` | int | `-1` | Link latency in cycles. |
| `noc.levels.<level>.topology` | string | `""` | Override topology for this level. |
| `noc.levels.<level>.router_latency` | int | `-1` | Router latency in cycles. |
| `noc.levels.<level>.router_pipeline` | string/int | `-1` | Router pipeline: `"full"` (0), `"reduced"` (1), `"simple"` (2), `"minimal"` (3). |
| `noc.levels.<level>.router_bypass` | bool | - | Enable router bypass. |
| `noc.levels.<level>.virtual_networks` | int | `-1` | Virtual networks. |
| `noc.levels.<level>.virtual_channels_per_vn` | int | `-1` | VCs per virtual network. |
| `noc.levels.<level>.input_buffer_depth` | int | `-1` | Input buffer depth. |
| `noc.levels.<level>.output_buffer_depth` | int | `-1` | Output buffer depth. |

### Bridge Overrides (`noc.bridges`)

Bridges connect adjacent hierarchy levels. Each can have independent model and parameters:

```yaml
noc:
  bridges:
    bank_bankgroup:
      model: simple
      lower_width_bits: 64
      upper_width_bits: 8
      latency_ns: 1.0
    chip_rank:
      model: detailed
      router_latency: 2
```

Valid boundary names: `subarray_bank`, `bank_bankgroup`, `bankgroup_chip`, `chip_rank`, `rank_channel`, `channel_system`.

(`noc.gateways` is the deprecated name of `noc.bridges`, read with a warning when `noc.bridges` is absent; giving both is refused since 1.11.106.)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `noc.bridges.<name>.model` | string | `""` | Bridge model: `simple`, `detailed`, or `""` (auto). `md1` accepted as alias for `simple`. |
| `noc.bridges.<name>.count` | int | - | REFUSED at load (rc 2, "parsed but NOT IMPLEMENTED", since 1.11.57): the bridge override interface carries router and link parameters only, so a bridge count would change nothing. |
| `noc.bridges.<name>.lower_width_bits` | int | `-1` | Lower-side link width in bits. |
| `noc.bridges.<name>.lower_frequency_mhz` | double | - | REFUSED at load, as `count`: no bridge clock reaches the model. |
| `noc.bridges.<name>.upper_width_bits` | int | `-1` | Upper-side link width in bits. |
| `noc.bridges.<name>.upper_frequency_mhz` | double | - | REFUSED at load, as `count`. |
| `noc.bridges.<name>.fifo_depth` | int | - | REFUSED at load, as `count`: no FIFO depth reaches the model. |
| `noc.bridges.<name>.latency_ns` | double | `-1.0` | Bridge base latency in nanoseconds. |
| `noc.bridges.<name>.latency_cycles` | int | `-1` | Bridge latency in cycles (backward compat). |
| `noc.bridges.<name>.width_bits` | int | `-1` | Single-width field (sets both lower + upper). |

Bridge router params: same as per-level (`router_latency`, `router_pipeline`, `router_bypass`, `virtual_networks`, `virtual_channels_per_vn`, `input_buffer_depth`, `output_buffer_depth`).

**Bridge latency formula**: `base_latency + max(ingress_time, egress_time)`, where ingress/egress depend on link width and frequency.

---

## Simulation Parameters

```yaml
simulation:
  phase_length: 10000
  max_instructions: 1000000000000
  stats_interval: 100000
  parallel: true
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `simulation.parallel` | bool | `true` | Parallelize the simulation whenever it is safe to do so; `false` forces a serial simulation. OpenMP workloads parallelize to the simulated core count -- results are exact at any simulator thread count, so this is a speed/footprint knob only. MPI workloads are always simulated serially for bit-exact determinism regardless of this key; it is accepted today and starts being honored when parallel MPI simulation becomes safe. |
| `simulation.phase_length` | int | `10000` | ZSim phase length (instructions per phase). |
| `simulation.max_instructions` | long | `1000000000000` | Runaway guard: total-instruction budget across all cores; the run terminates (rc=0, stats dumped) when reached. The default (1e12) is effectively unlimited -- if you lower it, make sure it exceeds the workload's full instruction count, or the run is silently truncated mid-kernel (watch for "Max total instructions reached" in the log / a missing BENCH_DONE). |
| `simulation.stats_interval` | int | `100000` | Statistics collection interval. |
| `simulation.det_epoch_phases` | int | `4` | Epoch length, in phases, of the epoch-frozen NoC feedback that prices thread-MPI accesses under `detailed` (an access in epoch k is priced from epoch k-1's frozen sample; see [network.md](network.md)). Larger = looser, with more lag; `1` = per phase. Must be >= 1. Handed to the timing plugin as `PIMID_DET_EPOCH_PHASES`, which the key overrides. |
| `simulation.mpi_contention_points` | int | `64` | Number of contention points on which the MPI transport reserves message time (keyed by destination PE modulo the count; fewer points, more collisions). Applies under `detailed`. Must be >= 1; the transport caps it at 256 without a message. Handed to the MPI shim as `PIMID_MPI_CONTEND_POINTS`, which the key overrides. |

---

## Synthetic Traffic (`synthetic`)

Read by `--method synthetic` only. `synthetic.pattern`, `injection_rate`,
`injection_rate_min`, `injection_rate_max`, `injection_rate_step`, `packets` and
`warmup` are documented with the mode in [network.md](network.md) ("Synthetic
traffic mode"). An unknown `pattern` is refused (1.11.106; it used to run
`uniform` with a warning).

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `synthetic.power` | bool | follows `power.enabled` | Run power analysis on the synthetic network. Absent: whatever `power.enabled` / `--power` / `--no-power` resolved to. |

---

## Power Analysis

```yaml
power:
  enabled: true
  report_detail: standard
  tech_node_nm: 22          # process node for ALL power domains (host + device)
  # device_tech_node_nm: 22 # optional device-only override
  # host_tech_node_nm: 22   # optional host-only override
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `power.enabled` | bool | `true` | Enable McPAT power analysis. CLI `--power`/`--no-power` overrides. |
| `power.report_detail` | string | `"standard"` | Report level: `summary` (one line), `standard` (component breakdown), `verbose` (+ per-component area, XML dump). |
| `power.tech_node_nm` | int | device node (`22`) | Process (technology) node in nm applied to **all** power domains. Sets both the device and host node in one place (uniform iso-process study). Giving it together with `system.tech_node_nm` or `technology.node_nm` is refused (1.11.106). |
| `power.device_tech_node_nm` | int | `22` | Device-only process node override. Leaves the host node untouched. |
| `power.host_tech_node_nm` | int | inherits device | Host-only process node override. When unset, the host **inherits the device node** (uniform process); the legacy hardcoded 7nm host default is removed. |
| `power.periphery_device` | string | `lstp` | CACTI device column the DRAM-periphery LEAKAGE ratio is taken from: `hp`, `lstp`, `lop` or `comm-dram` (`lp-dram` is unpopulated at 22 nm). Default `lstp` since 1.11.97 (it was `comm-dram`); the area and dynamic factors keep the comm-dram geometry. Any other word is refused (rc 1); a set value is printed as the user's. |
| `power.logic_reference_mhz` | double | unset | Clock (MHz) of the LOGIC-process design a DRAM-periphery PE is compared against. PIMID divides it by the CV/I delay ratio read from the CACTI columns to bound the PE clock, and warns when the requested PE clock exceeds the bound. Unset: in co-sim the host's clock is used; in device scope the bound is skipped with a printed reason. Must be above 0 and at most 20000 (refused otherwise, rc 1). |

**Process-node resolution.** The device node defaults to 22nm (unchanged; device
power flows are bit-identical at default). The host node, when not given its own
value, inherits the device node so a single `power.tech_node_nm` yields a uniform
iso-process study. A per-node YAML `tech_node_nm` under `system.hosts[]` /
`system.devices[]` still wins over these `power.*` knobs.

**Valid nodes (since 1.11.2): 22, 32, 45, 65, 90 nm -- a positive list.** These
are the nodes every linked tool evaluates from real, calibrated tables; 22 nm is
the finest. Any other value is a FATAL configuration error (the old behavior --
silently clamping sub-22 requests to 22 -- is removed; CACTI's 16nm table is a
stub, so the reject guards a live failure mode). DRAM memory domains carry no
free node at all: their periphery class is derived from the memory technology
and printed. `power.device_corner` (`hp` default, `lstp`, `lop`) selects the
CACTI device column for LOGIC-family components; it is refused, with a printed
reason, for DRAM-periphery components (one commodity-DRAM column per table).

`power.interconnect_projection` (`conservative` default, `aggressive`) selects
the ITRS wire projection, and feeds BOTH CACTI and McPAT so one die is modelled
with one metal stack. Conservative is the default on physics, not caution: the
aggressive column sets copper barrier thickness to zero at every node, which no
process can build, and holds the scattering coefficient at 1.00, which
measurement contradicts once wire dimensions approach the electron mean free
path. An invalid value is a FATAL configuration error.

`power.subarray_pitch_factor` (default `1.0`, range `[1.0, 10.0]`) is an
additional AREA penalty for a PE laid out on the memory array's bitline pitch.
It is distinct from the DRAM-periphery area factor: that one prices the
DEVICE (a DRAM-process transistor is larger), this one prices the LAYOUT (a
circuit pressed against the array cannot be wider than the pitch it sits on,
however small its transistors are). Vogelsang (MICRO 2010) draws the same
distinction between on-pitch circuitry -- sense-amp stripes, local wordline
drivers -- and off-pitch circuitry limited by wiring.

It applies ONLY at `SUBARRAY` placement on a DRAM technology, and setting it
anywhere else prints that it was ignored rather than discarding it silently.
Values below 1.0 are refused: the knob multiplies a penalty, so below 1 it
would claim the PE is denser than the baseline it is measured against. Values
above 1.25 are accepted with a warning, because Samsung's FIMDRAM (ISSCC 2021,
paper 25.4) bounds a real DRAM-process compute unit at about 3.05x its
logic-process area, which leaves roughly 1.25x for pitch once the 2.44x device
factor is applied. Their unit is bank-shared rather than subarray-pressed, so
this is a warning and not a refusal.

SRAM and NVM technologies never take this factor: their PEs are LOGIC-family
(SRAM is a logic process; MRAM/PCM/ReRAM place their storage element in the
metal stack above conventional logic transistors), so no periphery transform
is applied at all.

### Operating temperature and refresh (`power.temperature_*`)

```yaml
power:
  temperature_c: 77         # or temperature_k: 350; default 350 K = 77 C
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `power.temperature_k` | int | `350` | Operating temperature in kelvin. Reaches McPAT, CACTI and NVSim (leakage), and since 1.11.65 the DRAM refresh rate. **Legal values: 300..400 in steps of 10** -- CACTI evaluates only those points and the loader refuses anything else (so 373 K = 100 C is rejected; use 370 or 380). Consequently the refresh ladder's 85 C / 95 C thresholds are crossed at exactly 360 K (87 C) and 370 K (97 C). |
| `power.temperature_c` | int | -- | Same knob in Celsius (`+273`); the 10 K grid applies after conversion. Give one of the two: giving both is refused (1.11.106; `temperature_k` used to win in silence). |

**Refresh follows temperature (since 1.11.65).** Every DRAM family refreshes
twice as often above 85 C, and HBM four times as often above 95 C; before
1.11.65 the refresh duty was temperature-flat and a 105 C run priced the same
refresh power as a 45 C one. The multiplier on tREFI is now:

| Family | <= 85 C | 85-95 C | > 95 C | Sources |
|---|---|---|---|---|
| DDR3/DDR4/DDR5, LPDDR5, GDDR6 | 1x | 0.5x | 0.5x (held) | JESD79-5D Table 70 (tREFI 3.9 us -> 1.95 us); JESD79-3E / Micron extended-temperature 2x refresh; JESD209-5C Table 240 NOTE 2 + MR4 derating |
| HBM2, HBM3 | 1x | 0.5x | 0.25x | AMD PG276 p.23 (3.9 -> 1.95 us at 85-95 C); AMD DS923 note 16 (>= 4x above 95 C); Intel UG-20031 Table 30 / Agilex M HBM2E IP UG Table 5 (TEMP[2:0] ladder) |

The cold-end rungs the HBM controllers expose (2x and 4x SLOWER refresh below
vendor-specific thresholds) are deliberately NOT credited: they would grant a
refresh-power discount on a threshold no standard fixes. Above 105 C no source
specifies a rate (HBM CATTRIP is 120 C); the last rung is held. The default
350 K sits inside the nominal range, so results at the default are unchanged
from earlier releases -- the ladder engages only when a config states a
temperature above 85 C. The refresh rate is the ONLY temperature-dependent
term in the DRAM energy model: datasheets specify IDD at a fixed case
temperature and publish no derating curve for the active currents.

### DQ termination override (`power.termination_pj_per_bit`)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `power.termination_pj_per_bit` | float | unset (model) | Prices BOTH read and write DQ termination at the stated pJ/bit, overriding the read/write split loops. Use it to state an ODT-on operating point for LPDDR5 (whose JEDEC default is ODT disabled, JESD209-5C Table 84) or a non-default RTT for any DDR family. Wired since 1.11.63; earlier releases named the key in console messages but did not parse it. The override replaces termination only: since 1.11.91 a DQ-crossing access also pays the DQ output rail (IDDQ, printed `iddq=`), which this key does not change. |

### McPAT Overrides

Override auto-derived McPAT architectural parameters:

```yaml
power:
  mcpat_overrides:
    pipeline_depth: 14
    issue_width: 2
    num_alus: 4
    device_type: 0
    longer_channel_device: 1
    number_hardware_threads: 1
    interconnect_projection_type: 0
```

These overrides feed the **McPAT power/area model only** -- they do NOT change
ZSim cycle timing. The timing microarchitecture is fixed per core model
(`ooo_core` is Westmere-class with a 128-entry ROB and 4-wide issue at compile
time; `in_order_core` issue width defaults to 2, env-tunable via
`PIMID_INORDER_WIDTH`).

| Override Key | Type | Description |
|-------------|------|-------------|
| `pipeline_depth` | int | Pipeline stages (auto: `alu_core` 5, `simple_core`/`null_core` 5, `in_order_core` = `core.in_order.mispredict_penalty_cycles` (7), `ooo_core` 13 = zsim's dispatch stage; it was 14 and 19). |
| `issue_width` | int | Issue width (auto: alu=1, simple_core=1, ooo=4; in_order_core = the width the timing model runs, i.e. `pim.pe.issue_width` resolved as zsim resolves it, default 2 -- since 1.11.89, it was 1). |
| `num_alus` | int | Number of ALUs. |
| `num_muls` | int | Number of multipliers. |
| `num_fpus` | int | Number of FPUs. |
| `device_type` | int | McPAT device corner: 0 (hp), 1 (lstp), 2 (lop). Anything else is refused at config load (1.11.90); CACTI's lp-dram column is reached through `power.device_corner` on a DRAM-periphery placement, not through this key. |
| `longer_channel_device` | int | 0=short channel, 1=long channel. |
| `number_hardware_threads` | int | Hardware threads per core. |
| `interconnect_projection_type` | int | NoC projection type. |
| `core.pipeline_duty_cycle` | double | Pipeline duty cycle (0-1). |
| `mc.peak_transfer_rate` | int | MC peak transfer rate (MT/s). |
| `mc.databus_width` | int | MC databus width (bits). |
| `mc.number_ranks` | int | Number of memory ranks. |
| `noc.<index>.duty_cycle` | double | NoC level duty cycle (the 0-based level index, as the code looks it up; a level word is refused). |
| `noc.<index>.chip_coverage` | double | NoC level chip coverage (0-based level index). |
| `noc.<index>.total_accesses` | double | NoC level total accesses (0-based level index). |

Only these sixteen names are accepted: any other name under
`power.mcpat_overrides` is refused at load (1.11.106; it used to be stored and
never read), and every value must be a number. In the three `noc.<level>.*`
names, `<level>` is the level's index counted from the PE's own level:
`noc.0.duty_cycle` is the PE's level, `noc.1.duty_cycle` the next one up. An
override for a level that is not priced (no branch router and no endpoint) is
reported as ignored.

### Host-Device Link (`power.link`, legacy `power.pcie`)

```yaml
power:
  link:
    enabled: true
    link_type: pcie_gen5
    model: simple
    base_latency_ns: 500.0
    bandwidth_GBs: 63.0
    num_lanes: 16
```

`power.link` is the preferred name of this block. `power.pcie` is its legacy
name (renamed in 1.11.34, since the block carries CXL, NVLink, UALink,
interposer and DRAM-channel classes as well as PCIe): the loader reads
`power.link`, and reads `power.pcie` only when `power.link` is absent, with a
deprecation NOTE. Every key below is read under either name
(`power.pcie.link_type` = `power.link.link_type`, and so on). Giving both
blocks is refused (1.11.106).

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `power.link.enabled` | bool | `true` | Price the host-device link (its energy and its controller). A declared link enables pricing by default; an explicit `false` keeps the link's timing and drops its energy. |
| `power.link.link_type` | string | the attachment's class | Link class: `pcie_gen3`, `pcie_gen4`, `pcie_gen5`, `cxl_2_0`, `cxl_3_0`, `nvlink_3_0`, `nvlink_4_0`, `nvlink_c2c`, `ualink_1_0`, `interposer` or `dram_channel` (see [Link Types](#link-types-system-scope)). Absent: the class follows `system.devices[].attachment`. A named class overrides that default with a `[link]` note; a `system.network.links[].type` is the authoritative type when one is declared. Any other word is refused. |
| `power.link.model` | string | `"simple"` | Link timing model word: `simple` (includes M/D/1 queuing; `md1` is an alias), `analytical` or `detailed`; any other word is refused. Used only when a system is synthesized from top-level keys (`scope: cosim` without declared nodes), where it becomes the system network's model; with declared nodes `system.network.model` governs and this key is not used. |
| `power.link.base_latency_ns` | double | `500.0` | Per-transaction overhead (ns). When not set, the `interposer` class fills 5 ns and `dram_channel` the part's tRCD + tCL. |
| `power.link.bandwidth_GBs` | double | `63.0` | Peak bandwidth (GB/s). When not set, `interposer` fills 256 and `dram_channel` the part's channel bandwidth. |
| `power.link.num_lanes` | int | `16` | Lane count of the link controller McPAT prices (an interposer is counted in 64-lane modules instead). |
| `power.link.num_units` | int | `1` | Link units of the host-link controller. Read only by the system-scope `trace` method's power path; the `exec` co-simulation prices one unit per device, and device scope prices no host link. |
| `power.link.num_channels` | int | `16` | Channel count of the same controller, read on the same path as `num_units` and only when `num_lanes` <= 0 (the lane count is used otherwise). |
| `power.link.duty_cycle` | double | measured | Link duty-cycle OVERRIDE (0-1), printed beside the measured value. Absent: the co-simulation derives it from the measured crossing bytes over the link's capacity and the run's time (since 1.11.40); the `trace` path takes 0.01. |
| `power.link.total_load_perc` | double | `0` | Link-controller load fraction (0-1). Read only by the `trace` method's power path (absent: 0, said so); the `exec` co-simulation uses the measured duty. |
| `power.link.pj_per_bit_override` | double | unset | Link energy in pJ/bit, replacing the class's cited value; the printed band is then omitted (an overridden figure has no sourced range). Also prices a `dram_channel` crossing, which otherwise carries no separate link energy. |
| `power.link.header_bytes` | int | the class's | Protocol framing bytes per transaction: charged by the timing model and priced at the link's pJ/bit. Absent: the class's value (PCIe 20; CXL, NVLink and UALink 16; `interposer` and `dram_channel` 0). `0` is a real value. |
| `power.link.coherence_extra_ns` | double | `0.0` | Average extra latency (ns) of a coherent access over the link. |

---

## System Configuration

System scope (`scope: system`) enables multi-host, multi-device simulation.

```yaml
scope: system
system:
  frequency_mhz: 2000
  cache_line_size: 64
  tech_node_nm: 22
  hosts: [...]
  devices: [...]
  network: {...}
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `system.frequency_mhz` | int | `2000` | Reference system frequency. |
| `system.cache_line_size` | int | `64` | Cache line size in bytes. |
| `system.tech_node_nm` | int | `22` | Technology node (nm). Also available as `technology.node_nm`; giving two of `system.tech_node_nm`, `technology.node_nm` and `power.tech_node_nm` is refused (1.11.106). |

### Hosts

```yaml
system:
  hosts:
    - name: cpu0
      core_type: ooo_core
      num_cores: 4
      frequency_mhz: 3000
      tech_node_nm: 22   # valid: 22/32/45/65/90 (positive list, fatal otherwise)
      memory:
        technology: DDR5
      cache:
        l1d_kb: 32
        l1i_kb: 32
        l2_kb: 256
        l3_kb: 0
      workload:
        binary: ./host_app
        args: ["--mode", "host"]
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `system.hosts[].name` | string | - | Host node name (required). |
| `system.hosts[].core_type` | string | `"ooo_core"` | Host core type. |
| `system.hosts[].num_cores` | int | `4` | Number of host cores. |
| `system.hosts[].frequency_mhz` | double | `3000.0` | Host frequency. |
| `system.hosts[].tech_node_nm` | int | inherits device (`22`) | Host technology node. Valid: 22/32/45/65/90 positive list -- any other value is a fatal error (see Process-node resolution). |
| `system.hosts[].memory.technology` | string | `"DDR4"` | Host memory technology. Under `is_default_mem: true` (default) it is forced = the device tech; under `false` it is set from `system.hosts[].mem.technology`. |
| `system.hosts[].memory.bandwidth_mbs` | int | `-1` | Per-channel host memory bandwidth (MB/s). `-1` = auto from tech. |
| `system.hosts[].memory.channels` | int | `-1` | Host memory channel (MC) count. `-1` = auto from tech (DDR5 c=1, HBM3 c=16). |
| `system.hosts[].cache.l1d_kb` | int | `32` | Host L1D size. |
| `system.hosts[].cache.l1i_kb` | int | `32` | Host L1I size. |
| `system.hosts[].cache.l2_kb` | int | `256` | Host L2 size. |
| `system.hosts[].cache.l3_kb` | int | `0` | Host L3 size; `0` disables the L3. |
| `system.hosts[].cache.l1d_latency_ns` | double | CACTI | Host L1D latency (ns); `<= 0` or absent = CACTI's. |
| `system.hosts[].cache.l1i_latency_ns` | double | CACTI | Host L1I latency (ns); as `l1d_latency_ns`. |
| `system.hosts[].cache.l2_latency_ns` | double | CACTI | Host L2 latency (ns); as `l1d_latency_ns`. |
| `system.hosts[].cache.l3_latency_ns` | double | CACTI | Host L3 latency (ns); as `l1d_latency_ns`. |
| `system.hosts[].cache.l1d_ways` | int | record (`8`) | Host L1D associativity; absent = the cache record's (as `cache.l1d.ways`). |
| `system.hosts[].cache.l1i_ways` | int | record (`4`) | Host L1I associativity; as `l1d_ways`. |
| `system.hosts[].cache.l2_ways` | int | record (`8`) | Host L2 associativity; as `l1d_ways`. |
| `system.hosts[].cache.l3_ways` | int | record (`16`) | Host L3 associativity; as `l1d_ways`. |
| `system.hosts[].cache.l1d_banks` | int | record (slice rule) | Host L1D bank count; absent = the cache record's slice rule (as `cache.l1d.banks`). |
| `system.hosts[].cache.l1i_banks` | int | record (slice rule) | Host L1I bank count; as `l1d_banks`. |
| `system.hosts[].cache.l2_banks` | int | record (slice rule) | Host L2 bank count; as `l1d_banks`. |
| `system.hosts[].cache.l3_banks` | int | record (slice rule) | Host L3 bank count; as `l1d_banks`. |
| `system.hosts[].floating_point` | bool | `true` | Whether the host cores have an FPU (per node: a device's setting does not leak onto the host). |
| `system.hosts[].fp_emulation_cycles` | int | `0` | Cycles the host charges per FP-class instruction when `floating_point: false`. |
| `system.hosts[].pg` | bool | `false` | Power-gate the host as ONE piece: cores and host memory controller share one domain and one idle residency. Since 1.11.106 the residency is 1 minus the MEASURED union of core-active and host-MC-active phases; it used to take max(core, mc), which understates activity and over-credits the gating. A stats file without the union counter falls back to the max and the `[pg]` line says it over-credits. |
| `system.hosts[].workload` | map | - | Per-host workload: `binary`, `args`, `env`. A node without a `binary` inherits the top-level binary and args. |
| `system.hosts[].workload.env` | map | `{}` | Environment for the node's workload, read since 1.11.106 (documented earlier and ignored). The system runs as ONE simulated process, so the map joins that process's environment, the top-level `workload.env`. Nodes are applied in file order, hosts before devices; a name already set with a different value takes the node's value and the run prints a NOTE naming both values. A value that is not a map of `NAME: value` is refused. |

#### Host Memory Path (co-sim)

The host main-memory idle latency is a physical composition (tRCD + tCAS)
plus a **calibrated host-path adder** so the effective idle latency matches
measured real sockets (DDR5 ~110 ns, HBM3 ~235 ns). Host-role only -- never
applied to device (PE) memory. The adder is expressed either as an aggregate
or as a four-way decomposition; see
[cosim_calibration.md](cosim_calibration.md). `PIMID_DEBUG_HOSTMEM` prints the
composition.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `system.hosts[].memory.latency_adder_ns` | double | `-1` | Aggregate host-path adder (ns). `-1` = auto per-tech default (DDR5 77, HBM3 203). |
| `system.hosts[].memory.host_path.fabric_ns` | double | `-1` | IO-die / mesh / interconnect traversal (core->MC). `-1` = per-tech default. |
| `system.hosts[].memory.host_path.coherence_ns` | double | `-1` | Snoop / directory / coherence-engine latency. `-1` = per-tech default. |
| `system.hosts[].memory.host_path.mc_pipeline_ns` | double | `-1` | Memory-controller command pipeline + queueing depth. `-1` = per-tech default. |
| `system.hosts[].memory.host_path.phy_ns` | double | `-1` | PHY / command-interface wire + serialization tail. `-1` = per-tech default. |

Validity: `latency_adder_ns` and any `host_path.*` are **mutually exclusive**
(competing totals) -- setting both is a config error. A partial `host_path`
merges over the per-tech default split (e.g. `coherence_ns: 0` = a
"no coherence machinery" what-if).

#### Host NoC (co-sim)

Host-side fabric. A 1-core host has no fabric (crossbar degenerates:
core->caches->MC direct); a multi-core host adds a fixed one-hop crossbar
latency on the host memory path (port contention priced by the host MC M/D/1).

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `system.hosts[].noc.topology` | string | `"crossbar"` | Host fabric topology. Only the crossbar is modelled: any other word is accepted with a WARNING (1.11.106; it used to be accepted in silence) and the fabric still runs as the analytic crossbar (one uniform hop plus the host memory controller's queue). A host NoC model is a planned 1.1x release. |
| `system.hosts[].noc.model` | string | `"analytical"` | `analytical`. No host Garnet is instantiated: any other word (`detailed` included) is accepted with the same WARNING and runs the analytic crossbar. |
| `system.hosts[].noc.hop_cycles` | int | `4` | Core->LLC/MC one-hop latency (core clock). Applied only when `num_cores > 1`. |

#### Separate Host Memory (co-sim)

Consumed only when the paired device sets `is_default_mem: false`: a plain
non-PIM host main memory of any of the 11 techs (no PE arrays / H-tree / PIM
windows), reusing the same per-tech channel/timing tables. Omitting it when
`is_default_mem: false` is a **config error** (no silent DDR5 fallback).
Ignored (with a warning) when the device is `is_default_mem: true`.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `system.hosts[].mem.technology` | string | (required) | Host main-memory tech (one of the 11). Required when device `is_default_mem: false`. |
| `system.hosts[].mem.capacity_gb` | double | - | REFUSED at load (rc 2, "NOT IMPLEMENTED", since 1.11.57): host nodes carry no address range and no term in this model scales with memory capacity, so the key would change nothing. |
| `system.hosts[].mem.bandwidth_gbs` | double | auto | Per-channel bandwidth (GB/s); converted to MB/s. Absent = auto from tech. |
| `system.hosts[].mem.channels` | int | `-1` | Host memory channel count. `-1` = auto from tech. |

### Devices

```yaml
system:
  devices:
    - name: hbm_pim
      type: compute            # compute (has PEs) or memory (no PEs)
      attachment: internal     # internal or external; the default link class follows
      pe_type: alu_core
      num_pes: 64
      frequency_mhz: 1000
      tech_node_nm: 22
      memory:
        technology: HBM3
      pim:
        placement: { level: BANK }
        mc: { type: simple }
      noc:
        topology: MESH_2D      # inside a DRAM die: the in-die fabric (1.11.103)
        model: analytical
      workload:
        binary: ./pim_kernel
```

A device node accepts only the keys below. A device-scope key written inside
it -- `pim.mapping`, `pim.pe.count`, `pim.pe.type`, `pim.mc.local_latency`,
`noc.levels`, `memory.dram`, ... -- is refused in this release (see
[Loader Rules](#loader-rules)); 1.11.107 accepts the device-scope shape inside
a node.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `system.devices[].name` | string | - | Device name (required). |
| `system.devices[].type` | string | `"compute"` | Device type: `compute` (has PEs) or `memory` (memory-only, no cores). |
| `system.devices[].attachment` | string | `"external"` | `internal` or `external`; any other word is refused (1.11.106; it used to run as `external` and, since 1.11.103, take the PCIe class). Since 1.11.103 it also picks the DEFAULT host-device link class (`interposer` for an internal on-package part, `dram_channel` for an internal DDR-family part, `pcie_gen5` for an external device); a named `system.network.links[].type` / `power.link.link_type` overrides it with a printed note. See [Link Types](#link-types-system-scope). |
| `system.devices[].pe_type` | string | `"alu_core"` | PE core type (compute devices only). |
| `system.devices[].num_pes` | int | `0` | Number of PEs (compute devices only). |
| `system.devices[].frequency_mhz` | int | `1000` | Device frequency. |
| `system.devices[].tech_node_nm` | int | `22` | Device technology node. |
| `system.devices[].memory` | map | - | Device memory: `technology`, `banks` and `ports_per_bank` only. |
| `system.devices[].is_default_mem` | bool | `true` | `true`: this PIM device IS the host's main memory (host tech = device tech by construction). `false`: accelerator-side memory only -- the host MUST supply a `system.hosts[].mem` block (else config error). |
| `system.devices[].pim` | map | - | Device PIM block: `placement.level`; `pe.compute_factor`, `pe.access_factor`, `pe.throughput_factor`, `pe.operand_width`, `pe.energy_factor`, `pe.pg`, `pe.floating_point`, `pe.fp_emulation_cycles`, `pe.bit_serial`, `pe.issue_width`; `mc.type`, `mc.pes_per_mc`, `mc.pg`. Each means what its top-level `pim.*` key means; `mc.type` accepts `simple` only (1.11.106). |
| `system.devices[].noc` | map | - | Device NoC: `model` (`analytical` or `detailed`, as the top-level `noc.model`; anything else is refused; absent = the top-level `noc.model`), `topology` (upper-cased; in this release a node's word is not checked against the `noc.topology` list at load), and `pg` (fabric power gating). |
| `system.devices[].cache` | map | - | Device caches, flat keys only: `l1d_kb`, `l1i_kb`, `l2_kb`, `l3_kb`, and per level `<level>_latency_ns`, `<level>_ways`, `<level>_banks` (as the host's). |
| `system.devices[].workload` | map | - | Per-device workload: `binary`, `args`, `env`. A node without a `binary` inherits the top-level binary and args. |
| `system.devices[].workload.env` | map | `{}` | As `system.hosts[].workload.env`: joins the one simulated process's environment (1.11.106). |

### Host-Device Bridge (co-sim)

Two-layer host<->device link (`protocol` x `phy`). Carries only boundary
traffic (launch cmd/ack, Case-1 flush, Case-2 DMA). All fields optional;
unset fields default from the **device** memory technology. Supersedes the
flat `system.network.links[].type` charge whenever a system-scope config is
emitted. See [cosim.md](cosim.md) for the per-tech default table and
[cosim_calibration.md](cosim_calibration.md) for the anchors.

```yaml
system:
  bridge:
    protocol: native        # native | ddr_t | cxl_mem | loadstore
    phy: interposer         # on_die | pcb | interposer | serdes
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `system.bridge.protocol` | string | auto (device tech) | Per-transaction overhead class: `native`, `ddr_t`, `cxl_mem`, `loadstore`. |
| `system.bridge.phy` | string | auto (device tech) | Physical attach: `on_die`, `pcb`, `interposer`, `serdes`. |
| `system.bridge.bandwidth_gbs` | double | auto | Per-channel bandwidth (GB/s). |
| `system.bridge.latency_ns` | double | auto | Wire + command-interface/MC pipeline latency (ns). |
| `system.bridge.channels` | int | auto | Bridge channel count (aggregate BW = per-channel x channels). |
| `system.bridge.protocol_overhead_ns` | double | auto | Per-transaction protocol handshake (ns). Defaults to the protocol's own overhead (ddr_t 30, cxl_mem 40, native/loadstore 0). |
| `system.bridge.uncached_ns` | double | auto | Pure serialized cross-bridge access (ns); charged to Case-2 / uncached-window ops. |

**Validity (illegal combos = config error):** `cxl_mem` requires `serdes`;
`loadstore` requires `on_die`; `native` is forbidden on `serdes`.

### Coherence (co-sim)

Case-1 (unified address space) flush accounting, charged on the host core at
`roi_begin`. `mode: separate` = Case-2 cache bypass (no flush; the bridge
bulk-DMA path prices the crossing). Baselines never flush.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `system.coherence.mode` | string | `"unified"` | `unified` (Case 1: flush inputs + invalidate outputs) or `separate` (Case 2: cache bypass, no flush). |
| `system.coherence.writeback_bw_gbs` | double | `-1` | Host cache writeback bandwidth (GB/s). `-1` = auto = host memory aggregate BW. |
| `system.coherence.flush_fixed_ns` | double | `200` | Fixed flush/wbinvd latency (ns), charged PER RANK (each core issues its own flush; ruling 5a, 1.11.62). A stated constant -- see the stated-constant register. |

`system.coherence.footprint_bytes` is REFUSED at config time (since 1.11.59;
key retired at 1.11.40): the flush footprint is MEASURED from the host's dirty
cache lines. Since 1.11.62 each arriving rank walks the registry and charges
the DELTA dirtied since the previous walk:
`flush_cycles(rank) = flush_fixed_ns + ceil(delta / writeback_bytes_per_cycle)`.
`PIMID_DEBUG_COHERENCE` prints the resolved fixed charge at emit;
`PIMID_FLUSH_TRACE=1` prints each walk (rank, delta, cumulative).

### Kernel Launch (co-sim)

Offload launch cost tree, charged on the host core at the offload doorbell
(before device migration). `total = doorbell + dispatch + bridge(cmd) +
bridge(ack)`. DDR5 vs HBM3 differ only in the bridge component.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `system.launch.doorbell_ns` | double | `300` | User-mode doorbell write + cmd-packet formation (no syscall). |
| `system.launch.dispatch_ns` | double | `5000` | Runtime/OS dispatch software cost (~5 us; low end of the 5-20 us GPU launch band). |
| `system.launch.cmd_bytes` | int | `64` | Command packet size (host->device, crosses the bridge). |
| `system.launch.ack_bytes` | int | `64` | Acknowledgement packet size (device->host, crosses the bridge). |

Completion is busy-wait only (no IRQ mode, no polling knob).
`PIMID_DEBUG_LAUNCH` prints the resolved charge.

### System Network

Connects hosts and devices:

```yaml
system:
  network:
    topology: crossbar
    model: simple
    link_width_bits: 512
    frequency_ghz: 1.0
    latency_cycles: 5
    links:
      - src: cpu0
        dst: hbm_pim
        type: interposer
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `system.network.topology` | string | `"CROSSBAR"` | System-level topology: `CROSSBAR`, `MESH_2D` (alias `MESH`), `TORUS_2D`, `RING`, `FAT_TREE` or `BUS`. Upper-cased at load since 1.11.106, so `crossbar` is accepted, and the printed `System Network:` line is upper-case. Any other word is refused (it used to price 0.0 hops in the analytical model and run MESH_2D on the detailed multi-device path). |
| `system.network.model` | string | `"simple"` | Network model: `simple` (includes M/D/1), `detailed`. `md1` and `analytical` accepted as aliases for `simple`. |
| `system.network.link_width_bits` | int | `512` | Link width in bits. |
| `system.network.frequency_ghz` | double | `1.0` | Network frequency in GHz. |
| `system.network.latency_cycles` | int | `5` | Base latency in cycles. |
| `system.network.router_latency` | int | `1` | Router latency. |
| `system.network.virtual_channels_per_vn` | int | `1` | VCs per virtual network. |
| `system.network.input_buffer_depth` | int | `4` | Input buffer depth. |
| `system.network.output_buffer_depth` | int | `4` | Output buffer depth. |
| `system.network.links` | list | `[]` | Per-link overrides: `{src, dst, type, base_latency_ns, bandwidth_GBs}`. `src` and `dst` are required (an entry missing either is refused, since 1.11.71), and one of them must name a device node: an entry whose pair names no device is refused (1.11.106; it used to be dropped with a warning). `type` is a [link class](#link-types-system-scope) (absent: the attachment's class) and its preset fills the timing fields not set. `lanes` is REFUSED (rc 2, "NOT IMPLEMENTED", since 1.11.57): no lane count scales the link; set `bandwidth_GBs`. **Superseded by [`system.bridge`](#host-device-bridge-co-sim)** for the host<->device boundary charge in co-sim; still parsed for backward compatibility. |

### Legacy Host Block (`host`)

These keys describe the host of a system synthesized from top-level keys:
`scope: cosim` with no declared `system.hosts[]` / `system.devices[]` (the
device then comes from the top-level `pim:` and `memory:` keys, and the run
prints a NOTE saying so). With declared nodes `scope: cosim` is refused (since
1.11.90); a `scope: system` config describes its host under `system.hosts[]`.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `host.core_type` | string | `"ooo_core"` | Host core type: `ooo_core`, `in_order_core`, `simple_core`, `alu_core` or `null_core`, spelled exactly; anything else is refused (rc 1). |
| `host.num_cores` | int | `4` | Host core count (see also `workload.mpi_ranks`). |
| `host.frequency_mhz` | double | `3000.0` | Host clock (MHz). |
| `host.tech_node_nm` | int | inherits device | Host process node. It sets the same field as `power.host_tech_node_nm` and wins when both are given; absent both, the host inherits the device node. |
| `host.cache.l1d_kb` | int | `32` | Host L1D size (KB). |
| `host.cache.l1i_kb` | int | `32` | Host L1I size (KB). |
| `host.cache.l2_kb` | int | `1024` | Host L2 size (KB). |
| `host.cache.l3_kb` | int | `8192` | Host L3 size (KB). |
| `host.memory.technology` | string | `"DDR4"` | Host memory technology, canonicalised as `memory.technology` is (see [Memory Technologies](#memory-technologies)). |

---

## Enumerations

### Memory Technologies

| Value | Backend | Description |
|-------|---------|-------------|
| `DDR3` | Ramulator2 | DDR3 SDRAM |
| `DDR4` | Ramulator2 | DDR4 SDRAM |
| `DDR5` | Ramulator2 | DDR5 SDRAM |
| `LPDDR5` | Ramulator2 | Low-power DDR5 |
| `GDDR6` | Ramulator2 | Graphics DDR6 |
| `HBM2` | Ramulator2 | High Bandwidth Memory 2 |
| `HBM3` | Ramulator2 | High Bandwidth Memory 3 |
| `SRAM` | CACTI | Static RAM |
| `STT_MRAM` | NVSim | Spin-Transfer Torque MRAM (aliases: `STT-MRAM`, `STTMRAM`, `MRAM`) |
| `PCM` | NVSim | Phase-Change Memory (aliases: `PCRAM`, `3DXPOINT`) |
| `RERAM` | NVSim | Resistive RAM (aliases: `RESISTIVE`, `MEMRISTOR`) |

### PE Types

| Value | ZSim Core | Description |
|-------|-----------|-------------|
| `compute_unit` | ALU | A datapath rather than a processor: register file, arithmetic units, result bus and a resident instruction store; no caches, no speculation. Sized by `lanes` / `operand_width` / `floating_point` / `imem_bytes`. Aliases: `alu_core` (names the entire existing sweep corpus, so it is permanent), `ComputeUnit`, `cu`, `compute_unit_pe`. Bare `alu` and `ALU` were RETIRED and are now rejected. |
| `simple_core` | Simple | Coarse functional: IPC = 1 + serial memory latency, with caches. Aliases: `Simple`, `simple`. |
| `in_order_core` | InOrder | Decode-driven in-order pipeline: real RAW/port stalls, dual-issue (default 2), mispredict bubbles, contention-aware two-phase bound/weave. Aliases: `InOrder`, `in-order`, `in_order`. |
| `ooo_core` | Out-of-order | Decode-driven out-of-order superscalar (128-entry ROB, 4-wide, BTB+RAS branch prediction). Aliases: `OOO`, `OoO`, `ooo`, `out-of-order`. |
| `null_core` | Null | No timing model: IPC = 1 instruction counting; drops all memory accesses. Aliases: `Null`, `null`. |

Any other value (including the removed `timing_core`) is rejected with an error.

### NoC Topologies

| Value | Default Routing | Description |
|-------|----------------|-------------|
| `MESH_2D` | XY | 2D mesh grid |
| `TORUS_2D` | DOR | 2D torus (wraparound mesh) |
| `RING` | SHORTEST | Bidirectional ring |
| `CROSSBAR` | DIRECT | Full crossbar (1-hop) |
| `FAT_TREE` | NCA | Fat-tree (hierarchical) |
| `BUS` | DIRECT | Shared bus |
| `H_TREE` | NCA | H-tree (DRAM-style hierarchy) |
| `CUSTOM` | TABLE | User-defined (requires `topology_file`) |

`noc.topology` accepts every value above for every memory family, in any case,
with `MESH` as an alias of `MESH_2D`; any other word is refused at load
(1.11.106). `system.network.topology` accepts `CROSSBAR`, `MESH_2D` (`MESH`),
`TORUS_2D`, `RING`, `FAT_TREE` and `BUS`, also upper-cased at load. The host
fabric (`system.hosts[].noc`) is always the analytic crossbar.

**Inside a DRAM device (1.11.103, ruling 9)** the datapath above the die is
the tree whatever the configuration says, and a named `MESH_2D`, `RING` or
`CROSSBAR` is the fabric INSIDE EACH DIE: a grid of the die's organisations
at the placement tier (bank groups x banks [x subarrays]), one router with
its own endpoint per node, the die's exit to its chip router at node (0,0)
or at the crossbar's hub, links at the placement tier's ladder rung. The
chip, rank and channel tiers keep the tree and the channel-DQ wall is
unchanged. The analytical model prices all three; the detailed (Garnet)
model runs the `CROSSBAR` and refuses a `MESH_2D` or `RING` (its TABLE
routing with the tree's UP/DOWN classes cannot keep them deadlock-free). A
non-tree fabric named at the `CHIP` tier or above, and `TORUS_2D`, `FAT_TREE`
or `BUS`, are refused: the model does not define them there. Without a named
fabric a DRAM device runs the tree.

### Routing Algorithms

| Value | Description |
|-------|-------------|
| `XY` | Dimension-ordered XY routing (deadlock-free for mesh) |
| `DOR` | Dimension-ordered routing (generalized) |
| `SHORTEST` | Shortest-path routing |
| `DIRECT` | Direct routing (1-hop, for crossbar/bus) |
| `NCA` | Nearest Common Ancestor (for tree topologies) |
| `TABLE` | Table-based routing (from file) |
| `CUSTOM` | User-defined routing |

### Placement Levels

| Value | Hierarchy Level | Description |
|-------|----------------|-------------|
| `SUBARRAY` | L0 | PE one tier below the bank on a DRAM technology (the subarray); accepted as the legacy spelling on SRAM/NVM |
| `SUBBANK` | L0 | PE one tier below the bank on SRAM (CACTI subbank: the mats that form one data word). Refused on other families |
| `MAT` | L0 | PE one tier below the bank on STT_MRAM/PCM/RERAM (NVSim mat). Refused on other families |
| `BANK` | L1 | PE at bank level |
| `BANK_GROUP` | L2 | PE at bank group level |
| `CHIP` | L3 | PE at chip/die level |
| `RANK` | L4 | PE at rank level |
| `CHANNEL` | L5 | PE at channel level (aggregation tier; on-die for the channel-centric LPDDR/GDDR/HBM families) |
| `LOGIC_DIE` | L6 | PE on the logic/base die (HBM) |
| `HOST_MC` | L7 | PE shares host memory controller (no PE-MC needed); the top rung, priced as the host path (since 1.11.94) |

The same family words apply to the two other places that name L0
(1.11.74): the L0 count under `memory` -- `subarrays_per_bank` (DRAM; legacy
spelling on SRAM/NVM, accepted with a NOTE), `subbanks_per_bank` (SRAM
only), `mats_per_bank` (NVM only) -- and the L0 entry under `noc.levels` --
`subarray`, `subbank`, `mat`. Give exactly one of each; a word that names a
tier the technology does not have is refused.

Values are case-folded since 1.11.106 (`bank` = `BANK`). A word that names no
tier is refused at config load (since 1.11.90; it used to run `BANK` in
silence).

### Network Models

| Value | Description |
|-------|-------------|
| `analytical` | Closed-form per-access timing: `t_eff = max((L + W_q)/M, P*D/c)` -- hop-count unloaded latency `L`, M/D/1 contention `W_q`, memory-level parallelism `M` (`noc.mlp`). |
| `detailed` | Cycle-accurate Garnet simulation. |

No other values are accepted (legacy names `simple`, `md1`, `calibrated`, `calqueue`, `curve`, `injector`, `parallel` were removed).

Under `detailed`, thread-MPI per-access latency is priced from **measured**
Garnet congestion via epoch-frozen deterministic feedback (1.9.0); OMP keeps
the rolling-EWMA live feedback. Env `PIMID_MPI_ANALYTICAL_PRICING=1` forces the
analytical override on the MPI path (A/B only; not the default). See
[network.md](network.md) "Thread-MPI per-access pricing".

### Link Types (system scope)

The same eleven classes are accepted by `system.network.links[].type` and
`power.link.link_type`; any other word is refused. The timing columns are the
preset a `system.network.links[]` entry takes for the fields it does not set.

| Value | Base latency | Bandwidth | Header | Coherence extra | Description |
|-------|--------------|-----------|--------|-----------------|-------------|
| `pcie_gen3` | 500 ns | 15.75 GB/s | 20 B | 0 ns | PCIe Gen 3 (PCI-SIG 8 GT/s per lane, x16; since 1.11.106) |
| `pcie_gen4` | 500 ns | 31.5 GB/s | 20 B | 0 ns | PCIe Gen 4 (16 GT/s per lane, x16) |
| `pcie_gen5` | 500 ns | 63 GB/s | 20 B | 0 ns | PCIe Gen 5 (32 GT/s per lane, x16) |
| `cxl_2_0` | 200 ns | 63 GB/s | 16 B | 50 ns | CXL 2.0 (PCIe Gen 5 based) |
| `cxl_3_0` | 100 ns | 126 GB/s | 16 B | 30 ns | CXL 3.0 (PCIe Gen 6 based) |
| `nvlink_3_0` | 700 ns | 50 GB/s | 16 B | 0 ns | NVLink 3.0 (proprietary accelerator fabric) |
| `nvlink_4_0` | 500 ns | 100 GB/s | 16 B | 0 ns | NVLink 4.0 (proprietary accelerator fabric) |
| `nvlink_c2c` | 100 ns | 450 GB/s | 16 B | 50 ns | NVLink-C2C (chip-to-chip) |
| `ualink_1_0` | 200 ns | 100 GB/s | 16 B | 40 ns | UALink 1.0 (accelerator fabric) |
| `interposer` | 5 ns | 256 GB/s | 0 B | 0 ns | Silicon interposer (2.5D on-package: low latency, high bandwidth, no protocol framing) |
| `dram_channel` | tRCD + tCL | channels x channel width x data rate | 0 B | 0 ns | The device's own DRAM channel (1.11.103): a DIMM-resident DDR-family device. Timing from the part record; no separate link energy (the channel's I/O and termination are in the DRAM energy model) and no link controller (the host memory controller is priced as itself). |

**Default class (1.11.103, ruling COSIM-LINK-CLASS).** When no class is
named, the class follows the attachment the device's placement implies: an
on-package part (an HBM stack, SRAM, the NVMs) with `attachment: internal`
takes `interposer`; a DDR-family part (DDR3/DDR4/DDR5/LPDDR5/GDDR6) with
`attachment: internal` takes `dram_channel`; an `external` device takes
`pcie_gen5`. A named class (`power.link.link_type` or a
`system.network.links[].type`) overrides the default and the run prints a
`[link]` note naming both. The defaulted class carries its timing preset for
the fields the configuration did not set, exactly as naming it does.

---

## Override Rules

1. **CLI overrides YAML**: `--workload` overrides `workload.binary`, `--mpi-ranks` overrides `workload.mpi_ranks`, etc.
2. **5-param memory override**: All 5 memory params must be provided to override external models. Partial = warning + external model used.
3. **3-param cache override**: All 3 cache params (`latency_ns`, `energy_nj`, `static_power_mw`) must be provided per level.
4. **Auto-derivation**: `-1` or unset values are auto-derived from the memory technology via external models.
5. **Backward compatibility**: `noc.gateways` is the deprecated name of `noc.bridges`, `power.pcie` the legacy name of `power.link`, and `scope: cosim` = `scope: system`. Giving both names of one block or quantity is refused (see [Loader Rules](#loader-rules)). For `system.network.model` and `noc.bridges.*.model`, `analytical`/`md1` are accepted as aliases of `simple`, and `power.link.model` accepts `md1` for `simple`; the top-level `noc.model` accepts only `analytical` | `detailed`.

---

## Examples

### Minimal Device Config

```yaml
scope: device
workload:
  binary: ./my_benchmark
pim:
  pe: { type: alu_core, count: 4 }
  placement: { level: BANK }
  mc: { type: simple }
memory:
  technology: SRAM
```

### DDR4 with Queuing-Aware NoC

```yaml
scope: device
workload:
  binary: ./benchmark
  args: ["--size", "4096"]
pim:
  pe:
    type: in_order_core
    count: 16
    frequency_mhz: 1500
    placement: { level: BANK }
  mc:
    type: simple
    bandwidth_mbs: 12800
memory:
  technology: DDR4
noc:
  model: analytical          # hop count + M/D/1 queuing + MLP
  vcs_per_vnet: 8            # no topology: a DRAM device's datapath is the tier tree
cache:
  l1d: { size_kb: 64, ways: 8 }
  l2: { size_kb: 4096, count: 4 }
simulation:
  max_instructions: 50000000
power:
  enabled: true
  report_detail: verbose
```

### HBM3 PUM Design Point

```yaml
scope: device
pim:
  pe:
    type: alu_core
    count: 128
    frequency_mhz: 500
    compute_factor: 50.0       # tRAS-bound row activation
    access_factor: 0.0          # Free local SRAM access
    throughput_factor: 256.0    # 256-wide SIMD in subarray
    operand_width: 1            # Bit-serial
    energy_factor: 0.01         # Minimal switching energy
    placement: { level: SUBARRAY }
  mc: { type: simple }
memory:
  technology: HBM3
noc:
  topology: H_TREE
  model: analytical
```

### Multi-Level Hierarchy Tuning

```yaml
scope: device
pim:
  pe: { type: alu_core, count: 32, placement: { level: BANK } }
  mc: { type: simple }
memory:
  technology: DDR4
noc:
  topology: MESH_2D
  model: analytical
  levels:
    bank:
      model: simple
      link_width_bits: 128
      frequency_ghz: 2.4
    bank_group:
      model: detailed         # Garnet for bank-group level
      router_latency: 2
      virtual_channels_per_vn: 8
    chip:
      model: simple
  bridges:
    bank_bankgroup:
      model: simple
      lower_width_bits: 128
      upper_width_bits: 64
      latency_ns: 0.5
    bankgroup_chip:
      model: detailed
```

### System Scope with Host + Device

This build prices one memory per role, so it refuses a second device node
(see [cosim.md](cosim.md), Limitations). The second device of the layout is
kept below, commented, as the shape to restore when per-node memory counters
exist.

```yaml
scope: system
workload:
  binary: ./host_app
system:
  hosts:
    - name: cpu0
      core_type: ooo_core
      num_cores: 8
      frequency_mhz: 3500
      tech_node_nm: 22         # valid: 22/32/45/65/90
      memory: { technology: DDR5 }
      cache:
        l1d_kb: 48
        l2_kb: 1280
        l3_kb: 32768
  devices:
    - name: hbm_pim
      type: compute
      attachment: internal     # on-package: the default link class is interposer
      pe_type: alu_core
      num_pes: 64
      frequency_mhz: 1200
      memory: { technology: HBM3 }
      pim:
        placement: { level: BANK }
        mc: { type: simple }
      workload:
        binary: ./pim_kernel
    # - name: sram_accel       # a second device is refused in this build
    #   type: compute
    #   attachment: internal
    #   pe_type: alu_core
    #   num_pes: 16
    #   memory: { technology: SRAM }
    #   pim:
    #     placement: { level: BANK }
    #     mc: { type: simple }
    #   workload:
    #     binary: ./sram_kernel
  network:
    topology: CROSSBAR
    model: simple
    links:
      - src: cpu0
        dst: hbm_pim
        type: interposer
```
