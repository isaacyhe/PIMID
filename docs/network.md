# Network Models

PIMID has exactly two NoC models (`noc.model`):

## `detailed` -- cycle-accurate Garnet (the default)

The full Garnet 3.0 engine (extracted from gem5): routers, virtual channels,
credit-based flow control, deadlock-free routing.

- **Synchronous NoC accounting, always.** Garnet batches drain inline at
  phase boundaries, so every access is charged the latency the model actually
  computes -- and, with the fixed thread order (1.12.123) and the fixed
  thread-MPI rank order (1.12.126), the same in every run. There is no
  approximate fast path inside `detailed`; if you need speed over cycle
  accounting, use `noc.model: analytical`.
- **Rare MPI hang: resolved in 1.0.8.** Versions up to 1.0.7 could rarely
  (~8% of runs) hang a multi-rank detailed-MPI simulation: a rank was
  captured parked forever in a process-shared mutex acquisition on a free
  mutex (its futex wake lost under syscall interception). The MPI transport
  is now kernel-wait-free -- CAS spinlocks with usleep backoff for critical
  sections, polling with predicate recheck for empty/full/barrier waits --
  so lost wakes are impossible by construction. Validated with 120
  consecutive runs of the previously freeze-prone cells across two
  partitions, zero hangs. `PIMID_MPI_TRACE=1` still dumps per-rank
  transport wait events to /tmp for diagnosis.
- **ONE logical Garnet across MPI ranks.** All MPI ranks drive a single
  logical network; cross-rank packets (memory traffic and MPI message payloads
  alike) physically contend on shared links. There is NO isolated per-rank
  network mode. Two realizations:
  - **exec / thread-MPI (the only exec-method MPI model, 1.8.3).** The N ranks
    are core-threads inside **ONE process** sharing **ONE in-process Garnet**
    directly (`sharedNoc() == null`) -- the same single-network machinery the
    OpenMP/device path uses, not a per-rank record log. (1.9.0's fold-dump
    instrumentation corrected the earlier assumption that thread-MPI went
    through the per-rank shm consistent-cut; it does not.)
  - **trace method (per-rank processes).** Ranks run as separate processes and
    drive one logical network via a launcher-owned shared record log: every
    rank publishes its network-traversing accesses `{src, dst, cycle}` and at
    its own phase drain replays the identical merged multi-rank stream through
    its local Garnet replica. A drain resets Garnet and replays a record
    window, so N replicas of the same merged stream ARE one network. No
    barriers and no coordinator: ranks blocked in MPI, exited ranks, and
    pre-ROI ranks are exempt from the merge cut (deadlock-free by
    construction); if the shared log cannot be created or attached the run
    refuses to start.
  Records are stamped on the ROI-relative clock (the floor-free cross-rank
  axis); the message rendezvous (`send_time + contention_wait + NoC_latency`,
  1.3.1) is unchanged.
- **One timeline inside a process too (1.5.6).** Per-core cycle counters are
  private work clocks (an init-heavy thread runs far ahead of fresh workers),
  so single-process (OpenMP/device) batch records are stamped on the global
  phase clock with a deterministic intra-phase offset -- same-phase packets
  genuinely coexist in the replay. (`PIMID_NOC_DIAG_SPAN=1` prints per-drain
  batch spans for diagnosis.)

- **DRAM device networks are sparse placement-driven trees (1.5.3).** A DRAM
  device's internal datapath is a hierarchical distribution fabric, not a
  mesh. PIMID regenerates a CUSTOM tree per simulation from the PE placement
  (on the detailed model it is written as `/tmp/pimid_<tech>_pe<N>_<pid>.topo`,
  beside the run's zsim config, and removed when the run exits -- 1.12.2; it
  used to be written into the working directory and left there):
  only PE-hosting branches are materialized down to the placement level, and
  every empty region hangs ONE abstract endpoint at its maximal-empty-subtree
  root -- so an access to a non-PE region still travels the real tiered
  distance (same bank as a PE < other bank-group < other channel) while the
  region's device time comes from the aggregate DRAM model. The emitter and
  the runtime routing share one builder (`sparse_htree.h`), so endpoint ids
  cannot drift. Node count scales with PEs x tree depth, not device size:
  sparse placements simulate fast, and a fully-placed device grows the
  complete tree naturally. Accesses are priced purely by data LOCATION: an
  access to the PE's own unit traverses zero network hops and pays no network
  latency (1.5.5); everything else pays the Garnet-measured distance.
  Per-link latency/width derive from the technology's JEDEC organization
  (per-channel bandwidth x channel count from Ramulator2); per-technology
  bandwidth is modeled by NI-side flit count; tree routing uses up/down
  virtual-channel classes and is deadlock-free (validated drain-complete
  across packet counts, technologies, and traffic patterns).
- **Two-attachment nodes are links ((1.12.231)).** A node of the tree with exactly
  two attachments -- its parent and one router child, or its parent and one
  endpoint -- whose two links have the same width and the same clock (the
  ladder's own width and clock, before the 128-bit flit clamp) arbitrates
  nothing and changes nothing in the datapath, so it is not built as a
  router. The writer emits one link from the node below it to the first
  router above, its latency the sum of the links it replaces, marked `span`:
  its credits return in one cycle, since a link has no buffer of its own
  along the way. `lseg` lines (which Garnet skips) keep each replaced link's
  level, width and stack die, so the per-level link counts, the link service
  and the die crossings are those of the links the tree describes. A
  two-attachment node where the width or the clock changes is a converter and
  stays a router, and so do the root and every in-die fabric node; the
  `[htree] two-attachment nodes` line gives the counts per level.
  `noc.single_path_as_link: false` builds every node as a router. A `CUSTOM`
  file the user writes is built as written. The analytical model's tier walk
  applies the same rule: such a node adds the latency of the links it
  replaces and no router stages; converters, merges and branch routers keep
  theirs.
- **Links at full rate ((1.12.233)).** On the tree the writer emits, every link
  carries one flit per cycle at its width when nothing contends for it: the
  buffer each link feeds holds the link's credit round trip per VC -- link
  latency + the receiving router's pipe stages + the credit latency + 1 (a
  network interface receives in one stage and, as a sender, reads its
  credits one cycle late) -- and never less than the network's depth (one
  full data packet). Rate limits then come only from the flit count at the
  port width and from contention at the routers. The tree runs 4 VCs per
  vnet by default, 2 per up/down class, so a packet can follow the one ahead
  of it on a link before that one's tail has freed its VC downstream (with
  one VC per class back-to-back short packets ran at about 5/8 of the link
  rate); `noc.vcs_per_vnet` sets it. The topology file says so
  with a `buffers rtt` line. The same rule holds on every detailed fabric
  (see [Buffer depth](#buffer-depth)); an explicit `noc.buffers_per_vc`
  keeps that one depth on every link.
- **SRAM and NVM devices: local groups under the die ((1.12.247)).** At the
  `BANK` placement an SRAM, PCM, ReRAM or STT-MRAM device with no flat
  topology named is one die on which each processing element's interface and
  the banks of its slot form one local group on a local bus, and the groups
  hang on the die's router, the tree's root. The device has no bank-group,
  rank, channel or system tier: `memory.organization` is refused on it, and so
  is a placement at `BANK_GROUP`, `CHIP`, `RANK`, `CHANNEL` or `LOGIC_DIE`. An
  access inside the element's slot crosses its local bus in both models: the
  request and the reply each cost one device cycle plus their flits beyond
  the first at the bus width, and the element's controller books the bus in
  slots ((1.12.257)): the request takes the first free slot from its issue, the
  bank starts when the request has arrived (and is free), and the reply takes
  the first free slot from the bank's answer, so a long array access holds no
  bus. The bus is priced as a link at the group tier, with no router.
  An access to another element's slot, and every message between elements,
  crosses the die router. The local bus and the die links run at the device
  clock and carry the bank's access width (the 512-bit line the array model
  characterises the bank for); one crossing costs one device cycle. Both
  models build the same tree, so the rules above hold on it: each group is a
  link to the die router (one router in all), the die router is priced at
  its port count, links run at full rate with 4 VCs per vnet, and the fabric
  clock is the device clock. Until this release such a device ran a flat
  `MESH_2D` with one router per bank (256 for the 256-bank devices), on which
  an element's banks sat on different mesh nodes, and the analytical model
  priced a different shape (every bank in one group, and rank and channel
  tiers the device does not have). The `MAT` (`SUBBANK`) placement keeps the
  flat fabric.
- **`memory.local_path` ((1.12.258)).** How an SRAM or NVM element reaches the
  banks of its own group, at the `BANK` placement. `bus` (the default) is the
  shared local bus above. `garnet` builds the group as a router instead: every
  bank of the group is its own network endpoint, the element's interface is
  its own port on the group's router, and the group routers hang on the die
  router. In the detailed model every access, inside the group included, is a
  request and a reply message on the network, and the bank starts when the
  request has arrived. The analytical model prices an access inside the group
  along the same path: the element's port into the router carries the
  requests and the router's port back to it the replies, one flit per cycle,
  each message costs the network interfaces, a router stage per router it
  crosses, a link per link between two routers and its flits beyond the
  first, and the bank
  starts when the request has arrived; an access to another group's bank is
  priced as any routed access. Every link of the die, a bank's link to its
  group router included, costs one device cycle and is the bank's access
  width wide, clamped at `noc.flit_size_bits` as every link is: with the
  default 128-bit flit a line reply is 5 flits on the router path against 2
  on the 512-bit local bus. The rules above hold (a bank leaf is a link,
  full rate, 4 VCs per vnet, the device clock as the fabric clock); a group
  router has a port per bank, one for the element and one to the die router
  (18 for a group of 16 banks), and the power model's router classes stop at
  16 ports, so it is priced as a 16-port router. `garnet_bus` ((1.12.259)) builds
  the same endpoints with each group's router a shared bus: one transfer per
  cycle over all its ports, granted in turn, in the tree's up/down VC classes
  and with the routers' pipe and buffer rules (the topology file's
  `bus <level>` line). The analytical model books the requests and the
  replies of an access inside the group on that one medium. The power model
  prices each group's bus as a bus, its wire and an arbiter over its ports
  (CACTI's matrix arbiter, charged per access), and the group's link to the
  die router as a link. The key is refused on a DRAM device, at the `MAT` /
  `SUBBANK` placement, and with a named flat `noc.topology`.
- **Posted writes: `memory.write_buffer_entries` ((1.12.266)).** On an SRAM or NVM
  device at the `BANK` placement each element's controller has a write
  buffer, 32 entries by default. A write to a bank of the element's own slot
  is acknowledged one cycle after the controller has taken it into the
  buffer, on every local path (`bus`, `garnet`, `garnet_bus`) and in both
  models: the write then travels the path and waits for its bank (the bus
  slots or the router ports are booked, the detailed model still sends its
  request and its ack). The banks serve reads first: a read is served
  ahead of the posted writes still waiting for its bank, which start after
  it; a write that has started is not interrupted. While more than 80% of
  the entries are taken, the oldest writes keep their place, a read waiting
  for them, until at most 80% would remain (the baseline PCM controller of
  PreSET, ISCA 2012). A read of a line a buffered write holds is answered
  by the controller from the buffer one cycle after its issue, with no path
  or bank access. An entry is free again when its write's ack would have
  reached the element; a write that finds every entry taken waits for the
  earliest of them. Fences and barriers do not wait for the buffer, as on
  Intel Optane DC persistent memory, whose fences wait for a store to reach
  the controller's write pending queue, not the media. A write to another
  element's slot is acknowledged by its bank, as before; the other
  elements' use of a bank (see [memory.md](memory.md)) also serves reads
  first. `0` keeps blocking writes. The run prints the depth on a
  `[hierarchy] ... element controllers: posted writes` line, and each
  element's statistics count its posted writes (`wbPosted`), those that
  found the buffer full (`wbFull`) and the cycles they waited for an entry
  (`wbFullCycles`).
- Flat topologies remain available for non-DRAM device memories, when named: `MESH_2D`
  (alias `MESH`), `TORUS_2D`, `RING`, `CROSSBAR`, `FAT_TREE`, `BUS`,
  `H_TREE`, `CUSTOM` (file-defined). Since 1.11.106 `noc.topology` accepts
  exactly this list, in any case, for every memory family, and refuses any
  other word at load (an SRAM/NVM device used to run an unknown word as a
  1 x N mesh with XY routing in silence); a DRAM device then applies its own
  rule, the tree or the in-die fabric below. The system network
  (`system.network.topology`) accepts `CROSSBAR`, `MESH_2D` (`MESH`),
  `TORUS_2D`, `RING`, `FAT_TREE` and `BUS`, upper-cased at load (so the
  printed `System Network:` line is upper-case). The host fabric is analytic:
  `ring` (default), `crossbar` or `bus` (below).
- **The in-die fabric (1.11.103, ruling 9).** A `MESH_2D`, `RING` or
  `CROSSBAR` named on a DRAM device is that fabric INSIDE EACH DIE, over the
  die's organisations at the placement tier (the grid of a chip's bank
  groups x banks [x subarrays], `gridW x gridH` from the part record's
  counts). Every grid node is a router at the placement level with its own
  endpoint (the PE placed there, else an aggregated endpoint fronting that
  organisation); the die's exit to its chip router is node (0,0) (MESH,
  RING) or the hub (CROSSBAR); hops come from the grid positions (dimension
  order on a MESH, the shorter arc on a RING, node-hub-node on a CROSSBAR);
  links take the placement tier's ladder rung; leaving a die charges the
  tiers the grid replaced, as the tree would. The chip, rank and channel
  tiers above the die keep the tree. The per-access walk, the analytical
  census (mean cost, mean links, hotspot) and McPAT's router census (every
  grid node and hub is a priced switch at the placement tier) take the same
  path. The detailed model runs the CROSSBAR and refuses a MESH or RING it
  cannot route deadlock-free by TABLE; the analytical model prices all three.
  (1.12.143, user ruling 2026-10-28) On the analytical model the fabric's energy
  follows the same walk: the grid tier's switches are priced as routers (not
  as a shared bus); the die's node links (MESH/RING node-node, CROSSBAR
  node-hub) are one link instance priced from the node-link traversals the
  walk counts, at the built link count and the mean node-link length (each
  built link's Manhattan distance in node pitches, pitch = sqrt(die area /
  nodes per die); a CROSSBAR spoke runs to the hub at the die centre); and
  the die's exit -- one up-link per gridded die at the grid tier and at every
  tier the grid replaced, as the timing charges it -- is priced on the tree's
  links. Until then the node links rode a bus priced alike for all three
  fabrics, and the exits above the grid tier were reported as not priced.
  The same release makes the per-access walk reach the die: a target unit on
  a gridded die is looked up by the chip and every component below it (its
  grid node), where the lookup used to stop at the first tier the grid
  replaced, find no endpoint and fall back to the arithmetic ladder -- so the
  analytical timing never crossed the die fabric and MESH_2D, RING and
  CROSSBAR ran identically.
- **NoC energy (1.11.92).** McPAT charges one router access per FLIT per
  ROUTER crossed. The network counts true flits (ceil(message bits / source
  port width), 5 for a 576-bit data message on a 128-bit port) and routers
  crossed (links + 1 per packet); `Total hops` stays the link count, the
  latency input. On the detailed DRAM tree each level's accesses are
  MEASURED: the Garnet routers' own crossbar counters, attributed to levels
  by the `rlevel` lines the tree builder writes into the `.topo` file, then
  re-cut at the level's own datapath width from the sourced ladder. The
  analytical NoC counts the same quantities from its tier walk. A stats file
  without per-level counts falls back to the old preset split and the
  `[NoC] level` line says `PRESET`; a run with fabric traffic and no count at
  all, or a detailed run with no stats file, is refused (exit 3). Levels with
  no branch router and no endpoint price no router (their links are priced,
  see [power.md](power.md)). Since 1.12.150 a flat
  fabric Garnet built (an SRAM/NVM device's `MESH_2D`, say) is priced as that
  fabric -- every router it built, from its measured crossbar flits, and on a
  `MESH_2D` its links -- and the router model is calibrated per shape on
  published routers (see [power.md](power.md)). Such a fabric's nodes are the
  device's units (shared I/O): an element sits at its home unit's node, an
  access goes to the node of the unit it reads, and a message between elements
  runs between their home nodes ((1.12.246); they used to take the hierarchy
  tree's endpoint ids, which put every bank without an element on one node).

## `analytical` -- closed form

Per-access effective latency:

```
t_eff = max( (L + W_q) / M ,  P * D / c )
```

- `L` -- unloaded latency: hop-count round trip + serialization + memory access.
  For DRAM the per-hop link latency derives from the technology's
  **per-channel** bandwidth (the DQ datapath one access actually traverses),
  hop-normalized to the per-channel subtree depth; aggregate multi-channel
  bandwidth enters through the floor term instead.
- `W_q` -- M/D/1 queuing contention at the bottleneck channel, computed from
  the aggregate injection rate spread over the topology's channels
- `M` -- memory-level-parallelism intensity (`noc.mlp`, at least 1; omitted, a
  DRAM device uses 10, one value for every core type, calibrated for `alu_core`,
  and an SRAM or NVM device the PE's own window: 1 for the blocking `alu_core`
  and `in_order_core`, the L1D fill buffers for `ooo_core`)
- `P * D / c` -- aggregate-bandwidth floor: P PEs sharing c channels of
  deterministic service time D

`M = 1` degenerates to a fully serial latency model; large `M` approaches the
bandwidth roofline. The default `M` is calibrated against `detailed`.
Runs at analytical speed (~seconds) -- use it for large design-space sweeps,
and `detailed` for ground truth.

## Thread-MPI per-access pricing (measured feedback, 1.9.0)

For thread-MPI under `detailed` Garnet, per-access memory latency is priced
from **measured** Garnet congestion, not from the analytical closed form.
Ranks share one in-process Garnet (above), so an access can be charged the
contention the replay actually observed. The feedback is **epoch-frozen**
rather than read live:

- Garnet congestion is sampled once per N-phase **epoch** on the per-core
  ROI-relative axis; ROI-gated recording, single-actor complete-bucket folds.
  An access in epoch `k` prices each of its two messages at **its own
  source-destination pair's** frozen latency of its class, read from the
  newest epoch that is wholly folded, `min(k-2, last folded epoch)` -- a keyed
  table lookup, never a live rolling scalar. Each pair's value is its mean
  over the last 8 folded epochs (`noc.mpi_pair_window_epochs`); a pair with no sample
  there takes the ladder cost of its distance on the built tree. The trace method's
  per-rank-process shared record log keeps the epoch's mean over all
  messages, read at `k-1`.
- Until (1.12.237) an access was priced at the epoch's mean over every replayed
  message, read at epoch `k-2` only. The mean gave every rank the same cost per
  access, so ranks running the same code could fall into step and inject in
  bursts; and a rank running ahead of the slowest one read an epoch not yet
  folded and fell to the static average. Which of the two happened depended on
  the guest's memory layout.
- This replaces a live per-drain EWMA read, which depended on how many drains
  a rank had blended and was read-depth inflated. On the epoch-mean model the
  frozen level sat **+5-12% above the analytical floor**.
- Since 1.12.18 single-process runs (serial, OpenMP, co-sim OpenMP) use the
  same mechanism: per-core ROI-relative stamps, the consistent cut, and an
  epoch-frozen per-pair table read at epoch `k-2`
  (`simulation.det_epoch_phases`, one phase by default since (1.12.187); see
  the YAML reference).

**Escape hatch.** `PIMID_MPI_ANALYTICAL_PRICING=1` forces the old analytical
override (the topology-pure, load-independent `2 * nocAvgOneWayLatency`) for
A/B comparison. It is no longer the default MPI model.

## Host fabric and the host<->device bridge (co-sim)

In a co-simulation the device H-tree above is joined to a **host fabric**
by a **bridge**. The two are priced separately and traffic is charged to
exactly one of them:

- **Device H-tree** prices every device-PE memory access (as above).
- **Host fabric** (`system.hosts[].noc`, default `ring` / `analytical`)
  prices host-core loads/stores. A 1-core host has NO fabric (core -> caches
  -> MC direct). A multi-core host runs the topology it names (1.12.23):
  `ring` (default; the published Nehalem-EX ring, one stop per core and per
  host memory controller, 1 cycle per stop), `crossbar` (one hop,
  `hop_cycles`, core clock) or `bus`; any other word is refused (rc 1). Port
  contention is priced by the host memory controller (the replayed Ramulator2 controller since 1.12.18; the M/D/1 queue with `system.hosts[].memory.dram_replay: false`), so the fabric
  stays analytic: no host Garnet is instantiated, and a `noc.model` other
  than `analytical` is accepted with a WARNING. The same topology is priced
  in McPAT from the data messages and hops it measured
  (`power.models.host_noc`, 1.12.19; see [power.md](power.md)). Host
  memory bandwidth comes from the replayed Ramulator2 controller (since
  1.12.18; with `system.hosts[].memory.dram_replay: false`, a single
  aggregate M/D/1 queue at per-channel x channels GB/s), so one DDR5
  channel saturates under many host cores while HBM3's wide aggregate does
  not.
- **Bridge** (`system.bridge`, two-layer `protocol` x `phy`) prices only the
  host<->device boundary traffic -- launch cmd/ack, Case-1 coherence flush,
  Case-2 DMA. A crossing costs `phy_latency + protocol_overhead +
  ceil(bytes / aggregate_bytes_per_cycle)`. Neither fabric's ordinary traffic
  crosses it. Defaults derive from the device memory tech; the per-tech table
  and illegal-combo rules are in [cosim.md](cosim.md).

## Synthetic traffic mode

`--method synthetic` injects parametric traffic directly into Garnet -- no
workload or QEMU needed:

```yaml
synthetic:
  pattern: uniform        # uniform, bit-complement, tornado, neighbor,
                          # transpose, bit-reverse, bit-rotation, shuffle,
                          # memory-directed (PE -> memory-node hotspot)
  injection_rate: 0.1     # or injection_rate_min/max/step for a sweep
  packets: 10000
  warmup: 1000
  power: true             # default: follows power.enabled / --power / --no-power
```

Reports total cycles, average latency, and throughput per injection rate.

The pattern aliases `bitcomp`, `bitrev`, `bitrot` and `memdir` are accepted;
any other `pattern` is refused at load (1.11.106; it used to run `uniform`
with a warning). Defaults: 0.1 flits/node/cycle, 10000 packets, 1000 warmup
packets. A sweep runs when `injection_rate_min` and `injection_rate_max` are
both given with 0 < min < max (`injection_rate_step` defaults to 0.02).
`power` (bool) turns power analysis on or off for the synthetic run; absent,
it follows `power.enabled` and the `--power` / `--no-power` flags.

## Per-level and bridge overrides

When PEs sit deep inside a DRAM device (`placement: BANK` or `SUBARRAY`),
memory traffic does not cross one flat network -- it climbs a hierarchy:

```
subarray -> bank -> bank_group -> chip -> rank -> channel -> system
```

Each rung is physically different silicon: a subarray's local wiring, the
bank I/O, the shared channel DQ bus. Real DRAM datapaths are asymmetric --
wide and parallel near the arrays, narrow and shared toward the channel.
PIMID exposes that structure through two override mechanisms. If you never
touch them, the per-technology JEDEC-based defaults apply; they exist for
expert tuning and design-space exploration.

### Level overrides (`noc.levels.<level>`)

Give any hierarchy level its own network parameters instead of the global
`noc.*` settings. Valid level names: `subarray`, `bank`, `bank_group`,
`chip`, `rank`, `channel`, `system`.

```yaml
system:
  devices:
    - name: device
      noc:
        model: detailed            # global default
        levels:
          bank:
            model: simple          # this level: per-level analytical path
            link_width_bits: 64    # narrow bank I/O
            frequency_ghz: 2.4
          chip:
            model: detailed        # this level: full Garnet
            virtual_channels_per_vn: 8
```

Per-level knobs include `model`, `topology`, `link_width_bits`,
`frequency_ghz`, `latency_cycles`, `router_bypass`, `virtual_networks`,
`virtual_channels_per_vn`, and input/output buffer depths. A router's timing
is not a per-level YAML setting: it is the router's pipeline stages (see
[Router pipeline stages](#router-pipeline-stages)); `router_latency` and
`router_pipeline` under `noc.levels`, `noc.bridges` or `noc.gateways` are
refused at load.

### Bridge overrides (`noc.bridges.<boundary>`)

The boundaries BETWEEN adjacent levels are modeled as bridges -- the
mux/TSV/bus transition points where the datapath changes width and clock.
Valid boundary names: `subarray_bank`, `bank_bankgroup`, `bankgroup_chip`,
`chip_rank`, `rank_channel`, `channel_system`.

```yaml
system:
  devices:
    - name: device
      noc:
        bridges:
          bank_bankgroup:
            lower_width_bits: 64   # bank side of the crossing
            upper_width_bits: 8    # serialized toward the bank group
            latency_ns: 1.0
          chip_rank:
            model: detailed
```

A bridge override is the natural place to model serialization chokepoints
(e.g. a wide internal prefetch funneling into a narrow external bus) without
touching either adjacent level.

### Router pipeline stages

A router's timing is one number: its pipeline stages, the cycles a flit
spends in it (Garnet's router latency, `Router::get_pipe_stages()`).

- `noc.router_pipe_stages` (default 1) sets every router of the device
  network: the detailed network's routers on every topology (the generated
  DRAM tree, `H_TREE`, `MESH_2D` and the rest, and a `CUSTOM` file), the
  per-hop router term of the analytical model, and the router stages the
  power model prices. The analytical DRAM hierarchy charges the same count at
  every level.
- A stage lasts one cycle of the router's own clock: its level's
  `noc.levels.<level>.frequency_ghz`, else the memory technology's clock. With
  `router_bypass` on (the default), a router at a single-hop level (a bus or
  crossbar, or a single node) takes one stage fewer when it has more than one.
  The analytical hierarchy has always charged a router this way. The
  generated DRAM tree runs at the fabric clock (below), so it gives each
  level's routers their stages times the fabric clock over the level's clock,
  rounded up, and never less than one fabric cycle per stage. The two models
  then charge a router the same time to within one fabric cycle.
- Per level, a topology file can set a level's routers with a
  `pipe <level> <stages>` line (below), in network cycles. The generated DRAM
  tree writes one for each level whose routers come out at a number other than
  `noc.router_pipe_stages` (none when no level is slower than half the fabric
  clock and no stage is bypassed), and prints the levels it set. A
  hand-written file runs as written: every stage one network cycle, no bypass.
- `system.network.router_pipe_stages` (default 1) is the same setting for
  the host system network, which has no topology file.
- `noc.router_latency` and `system.network.router_latency` are the old
  names, read as aliases with a NOTE; giving a key under both names is
  refused.

The per-level and bridge `router_latency` / `router_pipeline` YAML keys are
gone. The pipeline names they took (`full`, `reduced`, `simple`, `minimal`,
for 4, 3, 2 and 1 stages) never reached a latency: no model read them, so a
configuration that set one ran exactly as without it. A per-level
`router_latency` set the analytical hierarchy's router cycles at that level;
that is now the level's `pipe` line on the detailed network, and the global
stage count on the analytical one. A bridge's crossing keeps its own
`latency_ns` / `latency_cycles`: in the tree a bridge is the link between a
level's router and its parent, which has no pipeline.

### Fabric clock (`noc.fabric_clock_mhz`)

The generated DRAM tree's network runs in its own clock domain, the fabric
clock. A DRAM datapath runs at the memory's clocks (about 0.8 to 14 GHz), much
faster than a PE clock, and a link or router stage charged as one PE cycle
would cost several times its real time.

- The default fabric clock is the fastest inner level clock of the memory's
  hierarchy (subarray, bank, and bank group where the part has bank groups):
  DDR3 0.8 GHz, DDR4 1.2 GHz,
  DDR5-3200 1.6 GHz, LPDDR5 0.8 GHz, GDDR6 1.75 GHz, HBM2 1.2 GHz, HBM3
  1.6 GHz. `noc.fabric_clock_mhz` sets it.
- A link costs the fixed latency the analytical hierarchy charges one crossing
  of a level (the level's `latency_cycles` at its clock), for the level of the
  router it enters going up: an endpoint's link its router's level, a
  router's up-link its parent's level. A link standing in for nodes that are
  not routers costs the sum of the links it replaces. The time is converted to
  fabric cycles and rounded up, at least one cycle per link. The file's header
  prints the fabric clock and the per-level link cycles.
- A router stage lasts one cycle of its level's clock, converted to fabric
  cycles the same way (above).
- The records the network replays are stamped in device cycles. An injection
  goes to the first fabric edge at or after its time, and a measured latency
  goes back to device cycles rounded up. Both conversions are exact integer
  arithmetic, made per record and per packet, so nothing drifts across
  phases.
- Bandwidth is unchanged: the link service the run time is floored at (each
  packet occupies a tree link for its bits over the link's width) stays in
  device cycles. The network itself carries one flit per fabric cycle per
  link.
- The power model prices the network at the fabric clock, and its link and
  router duty are counted against fabric cycles.
- Every other detailed topology (`MESH_2D`, `H_TREE`, a hand-written `CUSTOM`
  file and the rest) and the host system network run at the device clock, as
  before.

### Topology file (`noc.topology_file`)

A `CUSTOM` topology is read from a text file, one entry per line; `#`
starts a comment. Ids are 0-based. A DRAM device writes its own file (the
generated tree above); a hand-written file is used as written.

| Line | Fields | Meaning |
|------|--------|---------|
| `routers <N>` | count | The routers, ids `0 .. N-1`. Required. Router 0 is the root when the links form a tree. |
| `endpoints <M>` | count | The endpoints (network interfaces), ids `0 .. M-1`. Absent: one per router. |
| `ext <ep> <router> [latency] [width] [span]` | latency cycles (`-1` or absent: `noc.link_latency`), width bits (`0` or absent: `noc.flit_size_bits`), `span` as for `int` | Endpoint `ep`'s link to its router. The endpoint cuts a message into flits of this width. |
| `int <src> <dst> [weight] [latency] [width] [span]` | weight (default 1, the routing cost), latency, width as for `ext`; `span`: the link stands in for nodes that are not routers (written by the DRAM-tree writer, after the latency and width) | One direction of a router-to-router link; give both directions for a two-way link. The link carries one flit per cycle and returns credits over its own latency; a `span` link returns them in one cycle. |
| `buffers rtt` | - | Every link's receiving buffer per virtual channel holds the link's credit round trip (link latency + the receiving router's pipeline stages + credit latency + 1), so an uncontended link carries one flit per cycle. Written by the DRAM-tree writer unless `noc.buffers_per_vc` is given. A file without it follows the network's rule all the same ([Buffer depth](#buffer-depth)). |
| `rlevel <router> <level> [branch]` | level `0` subarray, `1` bank, `2` bank group, `3` chip, `4` rank, `5` channel, `6` system; branch `1` = two or more router children | The hierarchy level of a router: what `pipe` lines and the per-level traffic counts are keyed by. |
| `rlinkw <router> <bits>` | width bits | The width of a router's link to its parent, for the link-occupancy floor and the link power. |
| `rdie <router> <die>` | core die | The stacked die a chip-slot router sits on (HBM), for the die crossings. |
| `lseg <r\|e> <id> <level> <bits> <die> <parent level>` | written by the DRAM-tree writer | Accounting only: the links a `span` link replaced (their level, width, die and parent level), for the per-level traffic and link pricing. Garnet ignores it. |
| `pipe <level> <stages>` | level as for `rlevel`, stages >= 1 | Every router the `rlevel` lines put at that level runs `stages` pipeline stages instead of `noc.router_pipe_stages`. A level no router sits at, a second line for a level, or stages below 1 are refused. |

When the links form a tree, the routers route up to the common ancestor and
down, with separate virtual-channel classes for the two phases
(deadlock-free); any other graph is routed by shortest path. A small
two-level tree with its leaves at the bank level, three stages there:

```
routers 3
endpoints 2
int 0 1 1 1 128
int 1 0 1 1 128
int 0 2 1 1 128
int 2 0 1 1 128
ext 0 1 1 128
ext 1 2 1 128
rlevel 0 6 1
rlevel 1 1 0
rlevel 2 1 0
pipe 1 3
```

A packet between the two endpoints crosses routers 1, 0 and 2: 3 + 1 + 3
router cycles, plus the four links.

### Buffer depth

Every detailed fabric sizes the buffer each link feeds, per virtual channel, at
the link's credit round trip: link latency + the receiving router's pipeline
stages + the credit latency + 1. A network interface receives in one stage and,
as a sender, reads its credits one cycle late. The depth is never below one
data packet. An uncontended link then carries one flit per cycle, and rate
limits come only from the flit count at the port width and from contention.
This holds on the generated DRAM tree, `MESH_2D`, `TORUS_2D`, `RING`,
`CROSSBAR`, `FAT_TREE`, `BUS`, `H_TREE`, a hand-written `CUSTOM` file (built
as written; only its buffers follow the rule), the detailed host network and
the synthetic mode.

An explicit depth wins:
- `noc.buffers_per_vc` sets every buffer of the device network, as given:
  Garnet's wormhole flow control needs no whole packet per virtual channel.
  Earlier releases raised it to one data packet.
- `noc.levels.<level>.input_buffer_depth` sets the input buffers of the
  routers at that level, and `output_buffer_depth` the buffers fed by the
  links leaving that level's routers (the input setting wins where both apply).
  Levels are the tree's levels, or a `CUSTOM` file's `rlevel` levels; a
  topology without levels ignores them. A per-level depth is used as given,
  also where it is below `noc.buffers_per_vc`. The analytical DRAM hierarchy
  reads the same keys, as before.
- `system.network.input_buffer_depth` sets every buffer of the detailed host
  network. Its routers run 4 virtual channels per vnet by default, as the DRAM
  tree (`system.network.virtual_channels_per_vn`, which also sets the simple
  model's queueing capacity).

To run the depths of an earlier release, give the depth that release built:
one data packet, or `noc.buffers_per_vc` where that was larger. With the
default 576-bit messages and 128-bit flits that is `noc.buffers_per_vc: 5` on
every device topology and in the synthetic mode, and
`system.network.input_buffer_depth: 4` on the detailed host network; earlier
releases also ran `noc.vcs_per_vnet: 2` on the generated DRAM tree and
`system.network.virtual_channels_per_vn: 1` on the host network.

### When to use which

- **Sweep a single bottleneck** -- override one bridge (e.g. shrink
  `bank_bankgroup` width) and watch the bandwidth knee move.
- **Mixed fidelity** -- keep the contended level on `detailed` Garnet and let
  quiet levels run the cheap per-level analytical path.
- **What-if datapaths** -- give a level a non-default topology or clock to
  model a hypothetical device organization.

The complete key list with defaults is in
[yaml_reference.md](yaml_reference.md) (sections "Per-Level Overrides" and
"Bridge Overrides").
