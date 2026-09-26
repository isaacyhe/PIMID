# Network Models

PIMID has exactly two NoC models (`noc.model`):

## `detailed` — cycle-accurate Garnet (the default)

The full Garnet 3.0 engine (extracted from gem5): routers, virtual channels,
credit-based flow control, deadlock-free routing.

- **Synchronous NoC accounting, always.** Garnet batches drain inline at
  phase boundaries, so every access is charged the latency the model actually
  computes -- deterministic up to OS thread/process interleaving. There is no
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
  mesh. PIMID regenerates a CUSTOM tree per simulation from the PE placement:
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
- Flat topologies remain available for the host network and non-DRAM device
  memories: `MESH_2D`, `TORUS_2D`, `RING`, `CROSSBAR`, `FAT_TREE`, `BUS`,
  `H_TREE`, `CUSTOM` (file-defined).
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
  no branch router and no endpoint are not priced (pass-through wire; their
  measured traversals are printed as not priced).

## `analytical` — closed form

Per-access effective latency:

```
t_eff = max( (L + W_q) / M ,  P * D / c )
```

- `L` — unloaded latency: hop-count round trip + serialization + memory access.
  For DRAM the per-hop link latency derives from the technology's
  **per-channel** bandwidth (the DQ datapath one access actually traverses),
  hop-normalized to the per-channel subtree depth; aggregate multi-channel
  bandwidth enters through the floor term instead.
- `W_q` — M/D/1 queuing contention at the bottleneck channel, computed from
  the aggregate injection rate spread over the topology's channels
- `M` — memory-level-parallelism intensity (`noc.mlp`; omit for the
  calibrated per-core-model default)
- `P * D / c` — aggregate-bandwidth floor: P PEs sharing c channels of
  deterministic service time D

`M = 1` degenerates to a fully serial latency model; large `M` approaches the
bandwidth roofline. The default `M` is calibrated against `detailed`.
Runs at analytical speed (~seconds) — use it for large design-space sweeps,
and `detailed` for ground truth.

## Thread-MPI per-access pricing (measured feedback, 1.9.0)

For thread-MPI under `detailed` Garnet, per-access memory latency is priced
from **measured** Garnet congestion, not from the analytical closed form.
Ranks share one in-process Garnet (above), so an access can be charged the
contention the replay actually observed. To keep reported cycles
**deterministic** the feedback is **epoch-frozen** rather than read live:

- Garnet congestion is sampled once per N-phase **epoch** on the per-core
  ROI-relative axis; ROI-gated recording, single-actor complete-bucket folds.
  An access in epoch `k` prices from epoch `k-1`'s **frozen** sample, looked up
  by epoch index from the access's own simulated cycle -- a keyed table lookup,
  never a live rolling scalar.
- This replaces the naive "read the live per-drain EWMA" feedback, which was
  **26-196% per-core / ~25% makespan nondeterministic** (a fast rank blended
  fewer drains than a slow one) and read-depth inflated. The frozen level sits
  **+5-12% above the analytical floor** with real congestion priced in.
- OMP single-process pricing is **unchanged**: it keeps the rolling-EWMA live
  feedback (one process, one wall-order, already reproducible for its use).

**Repeatability (measured, dev8 gemv 8-rank HBM3, 4 reps x 2 host classes):**

| axis | spread |
|---|---|
| within-host, per-core | <=0.15% typical (occasional ~0.5% outlier) |
| cross-host, per-core | 2-7% systematic (host-dependent fixpoint) |
| rank-0 / cut-pinning core | host-independent to ~0.05% |

The residual is fundamental: the one-pass measured-feedback loop
(contention -> pricing -> fold membership -> contention) converges to a
**host-dependent fixpoint**, so cores that price from the epoch samples inherit
that host's level. **MPI sweeps must therefore be run single-venue** for
internal consistency; true venue-independent bit-exactness would need two-pass
deterministic replay (future work).

**Escape hatch.** `PIMID_MPI_ANALYTICAL_PRICING=1` forces the old analytical
override (the topology-pure, load-independent `2 * nocAvgOneWayLatency`) for
A/B comparison. It is no longer the default MPI model.

## Host fabric and the host<->device bridge (co-sim)

Under `scope: system` the device H-tree above is joined to a **host fabric**
by a **bridge**. The two are priced separately and traffic is charged to
exactly one of them:

- **Device H-tree** prices every device-PE memory access (as above).
- **Host crossbar** (`system.hosts[].noc`, default `crossbar` /
  `analytical`) prices host-core loads/stores. A 1-core host has NO fabric
  (crossbar degenerates: core -> caches -> MC direct). A multi-core host adds
  a fixed one-hop latency (`hop_cycles`, core clock) on the host memory path;
  port contention is already priced by the host memory M/D/1, so the fabric
  stays analytic. (`model: detailed` is parsed but currently inert -- no host
  Garnet is instantiated; a later 1.7.x increment.) Host memory bandwidth is
  a single aggregate M/D/1 queue at per-channel x channels GB/s, so one DDR5
  channel saturates under many host cores while HBM3's wide aggregate does
  not.
- **Bridge** (`system.bridge`, two-layer `protocol` x `phy`) prices only the
  host<->device boundary traffic -- launch cmd/ack, Case-1 coherence flush,
  Case-2 DMA. A crossing costs `phy_latency + protocol_overhead +
  ceil(bytes / aggregate_bytes_per_cycle)`. Neither fabric's ordinary traffic
  crosses it. Defaults derive from the device memory tech; the per-tech table
  and illegal-combo rules are in [cosim.md](cosim.md).

## Synthetic traffic mode

`--method synthetic` injects parametric traffic directly into Garnet — no
workload or QEMU needed:

```yaml
synthetic:
  pattern: uniform        # uniform, bit-complement, tornado, neighbor,
                          # transpose, bit-reverse, bit-rotation, shuffle,
                          # memory-directed (PE -> memory-node hotspot)
  injection_rate: 0.1     # or injection_rate_min/max/step for a sweep
  packets: 10000
  warmup: 1000
```

Reports total cycles, average latency, and throughput per injection rate.

## Per-level and bridge overrides

When PEs sit deep inside a DRAM device (`placement: BANK` or `SUBARRAY`),
memory traffic does not cross one flat network — it climbs a hierarchy:

```
subarray -> bank -> bank_group -> chip -> rank -> channel -> system
```

Each rung is physically different silicon: a subarray's local wiring, the
bank I/O, the shared channel DQ bus. Real DRAM datapaths are asymmetric —
wide and parallel near the arrays, narrow and shared toward the channel.
PIMID exposes that structure through two override mechanisms. If you never
touch them, the per-technology JEDEC-derived defaults apply; they exist for
expert tuning and design-space exploration.

### Level overrides (`noc.levels.<level>`)

Give any hierarchy level its own network parameters instead of the global
`noc.*` settings. Valid level names: `subarray`, `bank`, `bank_group`,
`chip`, `rank`, `channel`, `system`.

```yaml
noc:
  model: detailed            # global default
  levels:
    bank:
      model: simple          # this level: per-level analytical path
      link_width_bits: 64    # narrow bank I/O
      frequency_ghz: 2.4
    chip:
      model: detailed        # this level: full Garnet
      router_latency: 3
      virtual_channels_per_vn: 8
```

Per-level knobs include `model`, `topology`, `link_width_bits`,
`frequency_ghz`, `latency_cycles`, `router_latency`, `router_pipeline`,
`router_bypass`, `virtual_networks`, `virtual_channels_per_vn`, and
input/output buffer depths.

### Bridge overrides (`noc.bridges.<boundary>`)

The boundaries BETWEEN adjacent levels are modeled as bridges — the
mux/TSV/bus transition points where the datapath changes width and clock.
Valid boundary names: `subarray_bank`, `bank_bankgroup`, `bankgroup_chip`,
`chip_rank`, `rank_channel`, `channel_system`.

```yaml
noc:
  bridges:
    bank_bankgroup:
      lower_width_bits: 64   # bank side of the crossing
      upper_width_bits: 8    # serialized toward the bank group
      latency_ns: 1.0
    chip_rank:
      model: detailed
      router_latency: 2
```

A bridge override is the natural place to model serialization chokepoints
(e.g. a wide internal prefetch funneling into a narrow external bus) without
touching either adjacent level.

### When to use which

- **Sweep a single bottleneck** — override one bridge (e.g. shrink
  `bank_bankgroup` width) and watch the bandwidth knee move.
- **Mixed fidelity** — keep the contended level on `detailed` Garnet and let
  quiet levels run the cheap per-level analytical path.
- **What-if datapaths** — give a level a non-default topology or clock to
  model a hypothetical device organization.

The complete key list with defaults is in
[yaml_reference.md](yaml_reference.md) (sections "Per-Level Overrides" and
"Bridge Overrides").
