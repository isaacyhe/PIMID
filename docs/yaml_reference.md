# PIMID YAML Configuration Reference

Complete reference for all YAML configuration keys supported by PIMID.

## Table of Contents

- [Top-Level Keys](#top-level-keys)
- [Workload Configuration](#workload-configuration)
- [PIM Configuration](#pim-configuration)
  - [Processing Elements](#processing-elements-pimpe)
  - [PE Placement](#pe-placement-pimplacement)
  - [PE-to-Memory Mapping](#pe-to-memory-mapping-pimmapping)
  - [Distributed Memory Controllers](#distributed-memory-controllers-pimmc)
- [Memory Configuration](#memory-configuration)
  - [Memory Timing Override](#memory-timing-override)
  - [Memory Controller](#memory-controller)
- [Cache Configuration](#cache-configuration)
- [NoC Configuration](#noc-configuration)
  - [Per-Level Overrides](#per-level-overrides-noclevels)
  - [Bridge Overrides](#bridge-overrides-nocbridges)
- [Simulation Parameters](#simulation-parameters)
- [Power Analysis](#power-analysis)
  - [McPAT Overrides](#mcpat-overrides)
  - [PCIe Configuration](#pcie-configuration)
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
- [Enumerations](#enumerations)
- [Override Rules](#override-rules)
- [Examples](#examples)

---

## Top-Level Keys

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `scope` | string | `"device"` | Simulation scope: `device` or `system` (`cosim` accepted as deprecated alias for `system`) |
| `name` | string | `"PIMID_Simulation"` | Configuration name (appears in banner) |
| `description` | string | `""` | Configuration description |

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
| `workload.env` | map | `{}` | Environment variables injected into the workload process. |
| `workload.type` | string | `"serial"` | Workload type: `serial`, `openmp`, or `mpi`. |
| `workload.mpi_ranks` | int | `0` | Number of MPI ranks. 0 = auto (defaults to `pim.pe.count`). CLI `--mpi-ranks` overrides. |

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
| `pim.pe.core_type` | string | — | Alias for `pim.pe.type`. |
| `pim.pe.count` | int | `4` | Number of processing elements. |
| `pim.pe.frequency_mhz` | int | `2000` | PE clock frequency in MHz. Overrides `system.frequency_mhz`. |

**ALU Scaling Factors** (only meaningful for `alu_core`):

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `pim.pe.compute_factor` | double | `1.0` | Cycles-per-instruction multiplier. Higher = slower compute. |
| `pim.pe.access_factor` | double | `1.0` | Cycles per load/store. `0.0` = free local access (PUM). |
| `pim.pe.throughput_factor` | double | `1.0` | Parallelism divider on instruction count. |
| `pim.pe.bit_serial` | bool | `false` | Datapath model. `false` = bit-parallel (operand width has no cycle cost). `true` = bit-serial PUM (compute cost proportional to `operand_width`). |
| `pim.pe.issue_width` | int | `2` | In-order core issue width (uops issued per cycle, program order). Valid 1-6 (clamped to the 6-port FU model; out-of-range falls back to 2); practical range 1-4 -- real in-order cores are 2-3 wide, and beyond 4 the ports and RAW chains bind first. Applies to `in_order_core` only; env `PIMID_INORDER_WIDTH` overrides YAML. |
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
| `pim.placement.level` | string | `"BANK"` | Where PEs sit in the memory hierarchy. See [Placement Levels](#placement-levels). |
| `pim.placement.connection` | string | `"shared_io"` | PE-memory connectivity: `shared_io` (PE shares memory org's NI) or `separate_endpoints` (PE has own NI). |
| `pim.placement.local_link_latency` | int | `2` | Local link latency in cycles (only for `separate_endpoints`). |

**Placement determines the 7-level hierarchy position.** PEs at `BANK` level are at hierarchy level L1 (bank). Non-`HOST_MC` placements require a `pim.mc` section.

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
| `pim.mapping.pes_per_mem_org` | int | `0` | M:1 mapping — M PEs share each memory org. Mutually exclusive with `mem_orgs_per_pe`. |
| `pim.mapping.mem_orgs_per_pe` | int | `0` | 1:N mapping — each PE covers N memory orgs. Mutually exclusive with `pes_per_mem_org`. |
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
| `pim.mc.type` | string | `"simple"` | PE-MC type: `simple` (M/D/1 queuing always active; the only PE-MC model). |
| `pim.mc.pes_per_mc` | int | `1` | Number of PEs sharing each memory controller. |
| `pim.mc.local_latency` | int | `-1` | Local access latency in cycles. `-1` = auto-derived from memory technology. |
| `pim.mc.bandwidth_mbs` | int | `-1` | Bandwidth in MB/s . `-1` = auto-derived from Ramulator. |
| `pim.mc.groups` | list | `[]` | Per-group overrides. Each entry: `{id, type, local_latency, bandwidth_mbs}`. |

**Required** for all placement levels except `HOST_MC`. Without PE-MCs, every memory access would traverse the full hierarchy to the host MC.

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
| `memory.banks` | int | `4` | Number of memory banks. DRAM techs enforce minimum (DDR4 >= 16/chip). |
| `memory.subarrays_per_bank` | int | `4` | Subarrays per bank. |
| `memory.latency` | int | `-1` | Override memory latency in cycles. `-1` = auto from external models. |
| `memory.dram.device_width` | string | `x8` | DRAM device width (`x4`/`x8`/`x16`) for the DDR family. Selects the org preset row and, since 1.11.66, the org Ramulator instantiates -- the two are bound by a live shape check. |
| `memory.dram.ddr5_speed_grade` | int | `4800` | DDR5 speed grade, one of `3200`, `4800`, `5600` (since 1.11.66). Selects ONE part: timing preset (`DDR5_3200AN` / `DDR5_4800B` / `DDR5_5600B` -- the B bins at 4800/5600 because the Micron MT60B dies whose IDD currents are used are -48B/-56B parts), org (8 Gb at 3200, the 16 Gb MT60B die at 4800/5600), IDD row (Micron Rev A / Rev D addenda), and data rate. The 3200 row's IDD is unsourced (no held datasheet has a 3200 column) and is stated so. Any other value is a FATAL configuration error. |
| `memory.dq_turnaround` | bool | `true` | Charge the shared DQ bus a direction-reversal penalty (JEDEC tWTR) between a write and a read. Since 1.11.65 the penalty is DERIVED as nWTR_L x tCK from the Ramulator timing preset the run selects (DDR3 7.5 ns, DDR4 7.5, DDR5 10.0, LPDDR5 12.5, GDDR6 6.28, HBM2 8.33, HBM3 8.125 at the shipped presets; GDDR6 read 11.0 in 1.11.65 from a wrong clock, corrected in 1.11.66) and printed at load; earlier releases carried a hand-written table that had drifted 1.75x low for GDDR6. Set `false` for a design with a dedicated PIM interconnect and no shared bus. |

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

Alternative key names: `memory.timing.subarray_read_ns`, `memory.timing.subarray_write_ns`.

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
| `memory.controller.type` | string | `"auto"` | Controller type: `auto`, `simple`, `weavesimple`, `ramulator` (`md1`/`weavemd1` were removed -- M/D/1 queuing is always active in `simple`). |
| `memory.controller.bandwidth` | string | `""` | Bandwidth with units. Formats: `"25.6 GB/s"`, `"19200 MB/s"`, `"6400"` (bare number = MB/s). |
| `memory.controller.bound_latency` | int | `-1` | Bound latency for Weave controllers (cycles). |
| `memory.controller.ramulator_config` | string | `""` | Path to custom Ramulator2 YAML config. Overrides auto-generated config. |

**Auto-derivation rules** (when `type: auto`):
- DRAM technologies → `ramulator` (cycle-accurate Ramulator2)
- SRAM → `simple` (fixed latency from CACTI)
- NVM (STT-MRAM, PCM, ReRAM) → `simple` (fixed latency from NVSim)
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

### L1 Data Cache (`cache.l1d`)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `cache.l1d.size_kb` | int | `32` | L1D size in KB. |
| `cache.l1d.ways` | int | `8` | L1D associativity. |
| `cache.l1d.latency_ns` | double | `-1.0` | Override CACTI-derived latency (ns). Requires all 3 override params. |
| `cache.l1d.energy_nj` | double | `-1.0` | Override access energy (nJ). |
| `cache.l1d.static_power_mw` | double | `-1.0` | Override leakage power (mW). |

### L1 Instruction Cache (`cache.l1i`)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `cache.l1i.size_kb` | int | `16` | L1I size in KB. |
| `cache.l1i.ways` | int | `4` | L1I associativity. |
| `cache.l1i.latency_ns` | double | `-1.0` | Override latency (ns). |
| `cache.l1i.energy_nj` | double | `-1.0` | Override energy (nJ). |
| `cache.l1i.static_power_mw` | double | `-1.0` | Override leakage (mW). |

### L2 Cache (`cache.l2`)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `cache.l2.enabled` | bool | `true` | Enable L2 cache. |
| `cache.l2.size_kb` | int | `2048` | L2 size in KB (per instance if clustered). |
| `cache.l2.ways` | int | `16` | L2 associativity. |
| `cache.l2.count` | int | `1` | Number of L2 instances. `>1` = clustered (independent L2s). |
| `cache.l2.latency_ns` | double | `-1.0` | Override latency (ns). |
| `cache.l2.energy_nj` | double | `-1.0` | Override energy (nJ). |
| `cache.l2.static_power_mw` | double | `-1.0` | Override leakage (mW). |

### L3 Cache (`cache.l3`)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `cache.l3.enabled` | bool | `false` | Enable L3 cache (requires L2 to be enabled). |
| `cache.l3.size_kb` | int | `4096` | L3 size in KB. |
| `cache.l3.ways` | int | `16` | L3 associativity. |
| `cache.l3.latency_ns` | double | `-1.0` | Override latency (ns). |
| `cache.l3.energy_nj` | double | `-1.0` | Override energy (nJ). |
| `cache.l3.static_power_mw` | double | `-1.0` | Override leakage (mW). |

**Cache override rule**: All 3 params (`latency_ns`, `energy_nj`, `static_power_mw`) must be provided per cache level to override CACTI. Partial overrides are ignored with a warning.

**Note**: `alu_core` PEs skip cache hierarchy creation entirely (no L1/L2/L3).

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
| `noc.topology` | string | `"MESH_2D"` | Network topology. See [NoC Topologies](#noc-topologies). |
| `noc.routing` | string | `""` | Routing algorithm. Empty = auto-derived from topology. See [Routing Algorithms](#routing-algorithms). |
| `noc.model` | string | `"detailed"` | Network model: `detailed` (Garnet cycle-accurate; the default) or `analytical` (closed-form hop-count + M/D/1 + MLP). No other values are accepted. |
| `noc.router_latency` | int | `1` | Router traversal latency in cycles. |
| `noc.link_latency` | int | `1` | Link traversal latency in cycles. |
| `noc.mlp` | int | auto | Memory-level-parallelism intensity `M` of the analytical model (`t_eff = max((L + W_q)/M, P*D/c)`). Omit for the calibrated per-core default. |
| `noc.vcs_per_vnet` | int | `4` | Virtual channels per virtual network. |
| `noc.buffers_per_vc` | int | `4` | Buffers per virtual channel. |
| `noc.topology_file` | string | `""` | Path to topology file (required for `CUSTOM` topology). |
| `noc.routing_table_file` | string | `""` | Path to routing table file (for `TABLE` routing). |
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
| `noc.levels.<level>.router_bypass` | bool | — | Enable router bypass. |
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

(`noc.gateways` accepted as deprecated alias for `noc.bridges`.)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `noc.bridges.<name>.model` | string | `""` | Bridge model: `simple`, `detailed`, or `""` (auto). `md1` accepted as alias for `simple`. |
| `noc.bridges.<name>.count` | int | `-1` | Number of bridges. |
| `noc.bridges.<name>.lower_width_bits` | int | `-1` | Lower-side link width in bits. |
| `noc.bridges.<name>.lower_frequency_mhz` | double | `-1.0` | Lower-side link frequency. |
| `noc.bridges.<name>.upper_width_bits` | int | `-1` | Upper-side link width in bits. |
| `noc.bridges.<name>.upper_frequency_mhz` | double | `-1.0` | Upper-side link frequency. |
| `noc.bridges.<name>.fifo_depth` | int | `-1` | FIFO depth between levels. |
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
| `power.tech_node_nm` | int | device node (`22`) | Process (technology) node in nm applied to **all** power domains. Sets both the device and host node in one place (uniform iso-process study). |
| `power.device_tech_node_nm` | int | `22` | Device-only process node override. Leaves the host node untouched. |
| `power.host_tech_node_nm` | int | inherits device | Host-only process node override. When unset, the host **inherits the device node** (uniform process); the legacy hardcoded 7nm host default is removed. |

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
| `power.temperature_c` | int | -- | Same knob in Celsius (`+273`); the 10 K grid applies after conversion. |

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
| `power.termination_pj_per_bit` | float | unset (model) | Prices BOTH read and write DQ termination at the stated pJ/bit, overriding the read/write split loops. Use it to state an ODT-on operating point for LPDDR5 (whose JEDEC default is ODT disabled, JESD209-5C Table 84) or a non-default RTT for any DDR family. Wired since 1.11.63; earlier releases named the key in console messages but did not parse it. |

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
| `pipeline_depth` | int | Pipeline stages (auto: alu=5, in_order=14, ooo=19). |
| `issue_width` | int | Issue width (auto: alu/in_order=1, ooo=4). |
| `num_alus` | int | Number of ALUs. |
| `num_muls` | int | Number of multipliers. |
| `num_fpus` | int | Number of FPUs. |
| `device_type` | int | McPAT device type parameter. |
| `longer_channel_device` | int | 0=short channel, 1=long channel. |
| `number_hardware_threads` | int | Hardware threads per core. |
| `interconnect_projection_type` | int | NoC projection type. |
| `core.pipeline_duty_cycle` | double | Pipeline duty cycle (0-1). |
| `mc.peak_transfer_rate` | int | MC peak transfer rate (MT/s). |
| `mc.databus_width` | int | MC databus width (bits). |
| `mc.number_ranks` | int | Number of memory ranks. |
| `noc.<level>.duty_cycle` | double | NoC level duty cycle. |
| `noc.<level>.chip_coverage` | double | NoC level chip coverage. |
| `noc.<level>.total_accesses` | double | NoC level total accesses. |

### PCIe Configuration

```yaml
power:
  pcie:
    enabled: true
    model: simple
    base_latency_ns: 500.0
    bandwidth_GBs: 63.0
    num_lanes: 16
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `power.pcie.enabled` | bool | `true` | Enable PCIe power modeling. |
| `power.pcie.model` | string | `"simple"` | PCIe timing model: `simple` (includes M/D/1 queuing). `md1` accepted as alias. |
| `power.pcie.base_latency_ns` | double | `500.0` | Per-transaction overhead (ns). |
| `power.pcie.bandwidth_GBs` | double | `63.0` | Peak bandwidth (GB/s). |
| `power.pcie.num_lanes` | int | `16` | Number of PCIe lanes. |
| `power.pcie.num_units` | int | `1` | Number of PCIe units. |
| `power.pcie.num_channels` | int | `16` | Number of channels. |
| `power.pcie.duty_cycle` | double | `0.01` | PCIe duty cycle (0-1). |
| `power.pcie.total_load_perc` | double | `0.01` | Total load percentage (0-1). |

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
| `system.tech_node_nm` | int | `22` | Technology node (nm). Also available as `technology.node_nm`. |

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
| `system.hosts[].name` | string | — | Host node name (required). |
| `system.hosts[].core_type` | string | `"ooo_core"` | Host core type. |
| `system.hosts[].num_cores` | int | `4` | Number of host cores. |
| `system.hosts[].frequency_mhz` | double | `3000.0` | Host frequency. |
| `system.hosts[].tech_node_nm` | int | inherits device (`22`) | Host technology node. Valid: 22/32/45/65/90 positive list -- any other value is a fatal error (see Process-node resolution). |
| `system.hosts[].memory.technology` | string | `"DDR4"` | Host memory technology. Under `is_default_mem: true` (default) it is forced = the device tech; under `false` it is set from `host.mem.technology`. |
| `system.hosts[].memory.bandwidth_mbs` | int | `-1` | Per-channel host memory bandwidth (MB/s). `-1` = auto from tech. |
| `system.hosts[].memory.channels` | int | `-1` | Host memory channel (MC) count. `-1` = auto from tech (DDR5 c=1, HBM3 c=16). |
| `system.hosts[].cache.l1d_kb` | int | `32` | Host L1D size. |
| `system.hosts[].cache.l1i_kb` | int | `32` | Host L1I size. |
| `system.hosts[].cache.l2_kb` | int | `256` | Host L2 size. |
| `system.hosts[].cache.l3_kb` | int | `0` | Host L3 size; `0` disables the L3. |
| `system.hosts[].workload` | map | — | Per-host workload (binary, args, env). Inherits top-level workload if empty. |

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
| `system.hosts[].noc.topology` | string | `"crossbar"` | Host fabric topology. |
| `system.hosts[].noc.model` | string | `"analytical"` | `analytical` (shipped). `detailed` is parsed but currently inert -- no host Garnet is instantiated (later 1.7.x increment). |
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
| `system.hosts[].mem.capacity_gb` | double | `-1` | Advisory capacity (GB); user's realism responsibility. |
| `system.hosts[].mem.bandwidth_gbs` | double | auto | Per-channel bandwidth (GB/s); converted to MB/s. Absent = auto from tech. |
| `system.hosts[].mem.channels` | int | `-1` | Host memory channel count. `-1` = auto from tech. |

### Devices

```yaml
system:
  devices:
    - name: hbm_pim
      type: compute            # compute (has PEs) or memory (no PEs)
      attachment: interposer   # or pcie_gen4, pcie_gen5, cxl_2_0, cxl_3_0
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
        topology: MESH_2D
        model: simple
      workload:
        binary: ./pim_kernel
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `system.devices[].name` | string | — | Device name (required). |
| `system.devices[].type` | string | `"compute"` | Device type: `compute` (has PEs) or `memory` (memory-only, no cores). |
| `system.devices[].attachment` | string | `"pcie_gen4"` | Interconnect: `pcie_gen4`, `pcie_gen5`, `cxl_2_0`, `cxl_3_0`, `interposer`. |
| `system.devices[].pe_type` | string | `"alu_core"` | PE core type (compute devices only). |
| `system.devices[].num_pes` | int | `0` | Number of PEs (compute devices only). |
| `system.devices[].frequency_mhz` | int | `1000` | Device frequency. |
| `system.devices[].tech_node_nm` | int | `22` | Device technology node. |
| `system.devices[].memory` | map | — | Device memory config (`technology`, etc.). |
| `system.devices[].is_default_mem` | bool | `true` | `true`: this PIM device IS the host's main memory (host tech = device tech by construction). `false`: accelerator-side memory only -- the host MUST supply a `host.mem` block (else config error). |
| `system.devices[].pim` | map | — | Device PIM config (`placement`, `mc`, `mapping`). |
| `system.devices[].noc` | map | — | Parsed but currently inert (reserved). |
| `system.devices[].cache` | map | — | Device cache config. |
| `system.devices[].workload` | map | — | Per-device workload. Inherits top-level if empty. |

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
| `system.network.topology` | string | `"crossbar"` | System-level topology. |
| `system.network.model` | string | `"simple"` | Network model: `simple` (includes M/D/1), `detailed`. `md1` accepted as alias for `simple`. |
| `system.network.link_width_bits` | int | `512` | Link width in bits. |
| `system.network.frequency_ghz` | double | `1.0` | Network frequency in GHz. |
| `system.network.latency_cycles` | int | `5` | Base latency in cycles. |
| `system.network.router_latency` | int | `1` | Router latency. |
| `system.network.virtual_channels_per_vn` | int | `1` | VCs per virtual network. |
| `system.network.input_buffer_depth` | int | `4` | Input buffer depth. |
| `system.network.output_buffer_depth` | int | `4` | Output buffer depth. |
| `system.network.links` | list | `[]` | Per-link overrides: `{src, dst, type, lanes, base_latency_ns, bandwidth_GBs}`. **Superseded by [`system.bridge`](#host-device-bridge-co-sim)** for the host<->device boundary charge in co-sim; still parsed for backward compatibility. |

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

The same family words apply to the two other places that name L0
(1.11.74): the L0 count under `memory` -- `subarrays_per_bank` (DRAM; legacy
spelling on SRAM/NVM, accepted with a NOTE), `subbanks_per_bank` (SRAM
only), `mats_per_bank` (NVM only) -- and the L0 entry under `noc.levels` --
`subarray`, `subbank`, `mat`. Give exactly one of each; a word that names a
tier the technology does not have is refused.
| `BANK` | L1 | PE at bank level |
| `BANK_GROUP` | L2 | PE at bank group level |
| `CHIP` | L3 | PE at chip/die level |
| `RANK` | L4 | PE at rank level |
| `HOST_MC` | — | PE shares host memory controller (no PE-MC needed) |

### Network Models

| Value | Description |
|-------|-------------|
| `analytical` | Closed-form per-access timing: `t_eff = max((L + W_q)/M, P*D/c)` — hop-count unloaded latency `L`, M/D/1 contention `W_q`, memory-level parallelism `M` (`noc.mlp`). |
| `detailed` | Cycle-accurate Garnet simulation. |

No other values are accepted (legacy names `simple`, `md1`, `calibrated`, `calqueue`, `curve`, `injector`, `parallel` were removed).

Under `detailed`, thread-MPI per-access latency is priced from **measured**
Garnet congestion via epoch-frozen deterministic feedback (1.9.0); OMP keeps
the rolling-EWMA live feedback. Env `PIMID_MPI_ANALYTICAL_PRICING=1` forces the
analytical override on the MPI path (A/B only; not the default). See
[network.md](network.md) "Thread-MPI per-access pricing".

### Link Types (system scope)

| Value | Description |
|-------|-------------|
| `nvlink_3_0` / `nvlink_4_0` / `nvlink_c2c` | 100-700 ns | 50-450 GB/s | proprietary accelerator fabrics |
| `ualink_1_0` | ~100 ns | ~450 GB/s | accelerator fabric |
| `pcie_gen4` | PCIe Gen 4 (16 GT/s per lane) |
| `pcie_gen5` | PCIe Gen 5 (32 GT/s per lane) |
| `cxl_2_0` | CXL 2.0 (PCIe Gen 5 based) |
| `cxl_3_0` | CXL 3.0 (PCIe Gen 6 based) |
| `interposer` | Silicon interposer (low latency, high bandwidth) |

---

## Override Rules

1. **CLI overrides YAML**: `--workload` overrides `workload.binary`, `--mpi-ranks` overrides `workload.mpi_ranks`, etc.
2. **5-param memory override**: All 5 memory params must be provided to override external models. Partial = warning + external model used.
3. **3-param cache override**: All 3 cache params (`latency_ns`, `energy_nj`, `static_power_mw`) must be provided per level.
4. **Auto-derivation**: `-1` or unset values are auto-derived from the memory technology via external models.
5. **Backward compatibility**: `noc.gateways` = `noc.bridges`; `scope: cosim` = `scope: system`. For `system.network.model`, `noc.bridges.*.model`, and `power.pcie.model` only, `analytical`/`md1` are accepted as aliases of `simple`; the top-level `noc.model` accepts only `analytical` | `detailed`.

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
  topology: TORUS_2D
  model: simple
  vcs_per_vnet: 8
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
  model: simple
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

### System Scope with Host + Two Devices

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
      tech_node_nm: 5
      memory: { technology: DDR5 }
      cache:
        l1d_kb: 48
        l2_kb: 1280
        l3_kb: 32768
  devices:
    - name: hbm_pim
      type: compute
      attachment: interposer
      pe_type: alu_core
      num_pes: 64
      frequency_mhz: 1200
      memory: { technology: HBM3 }
      pim:
        placement: { level: BANK }
        mc: { type: simple }
      workload:
        binary: ./pim_kernel
    - name: sram_accel
      type: compute
      attachment: interposer
      pe_type: alu_core
      num_pes: 16
      memory: { technology: SRAM }
      pim:
        placement: { level: BANK }
        mc: { type: simple }
      workload:
        binary: ./sram_kernel
  network:
    topology: crossbar
    model: simple
    links:
      - src: cpu0
        dst: hbm_pim
        type: interposer
      - src: cpu0
        dst: sram_accel
        type: interposer
```
