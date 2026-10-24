# Co-simulation Calibration

The co-sim boundary and host-memory constants are **literature-anchored** and
**deterministic**. This page documents where each number comes from, how it
enters the simulator, and the honesty boundaries. The mechanics of each charge
are in [cosim.md](cosim.md); this page is the provenance.

## Three-tier evidence

Every calibrated constant sits in one of three tiers:

1. **Measured anchor** -- a real-socket measurement exists for this tech. Used
   directly. (DDR5, HBM2, HBM3 host memory; the uncached/serialized bound.)
2. **Class transfer** -- no direct measurement; priced from a measured sibling
   in the same attach/media class, stated as such. (DDR3/DDR4/LPDDR5 and
   ReRAM/PCM take the DDR5-class fabric; GDDR6 scales a deeper graphics MC;
   HBM2 scales from the HBM3 anchor.)
3. **Structural zero** -- the cost is architecturally absent, not merely
   small. (SRAM and STT-MRAM are on-die load/store, below any SoC fabric --
   host-path adder = 0.)

Class-transfer and structural-zero rows are labeled in the source
(`getHostPathSplit`, `bridgeDefaultsForTech` in `pimid/src/main.cpp`) as
`INFERRED` / on-die so the tier is never silently lost.

## Measured anchors

### Host memory idle latency (cached demand miss)

| tech | default | measured sources |
|---|---|---|
| DDR5 | 110 ns | server idle pointer-chase: SPR ~107, Genoa 118 (73 IO-die + 35 device), Turin 125.6, GNR 131.5; client parts 70-85 |
| HBM2 | ~130 ns | Xeon Max MLC idle: 96-113 (DDR5 mode) vs **121-135** (HBM mode), delta +22-25 ns |
| HBM3 | 235 ns | MI300A (the only shipping HBM3-CPU class), arXiv:2508.12743: CPU->HBM3 **236-241 ns** @4GiB, plateau ~240 from 2GiB (GPU-side 333-350) |

Adjudication (2026-07-09): the HBM3-CPU ~200-235 ns claim is **confirmed**
(slightly conservative vs the 240 measurement); DDR5 80-100 is client-right,
server-class is ~110. Defaults ship at server-class.

### Uncached / pure-access latency

A second, distinct price: a **pure serialized** access with no cache
assistance (UC semantics, one outstanding, TLB/page-walk + closed row).
Measured: 167 ns pointer-chase with TLB, >200 ns on E-cores, ~300 ns loaded;
citable as "180-200 ns, 1.6-1.8x the cached miss".

| tech | uncached default |
|---|---|
| DDR5 | 200 ns |
| HBM3 | 335 ns |

All Case-2 cache-bypass accesses, doorbell polls, and UC-window ops are
charged the **uncached** price -- charging the cached-miss price would
understate the boundary cost ~2x. This is the `system.bridge.uncached_ns`
field; it also retroactively prices the Case-2 "pack once + bulk DMA"
rationale (per-access crossing is ~200 ns each, serialized -- now priced, not
asserted).

## The HBM penalty is command-interface + MC pipeline, NOT SerDes

Real HBM hosts measure ~20-30 ns **worse** idle latency than DDR5 (Xeon Max
~125-130 HBM vs ~105-110 DDR5; A64FX ~120-140) -- HBM trades latency for
width. The penalty is the **low-clocked parallel command/address bus,
pseudo-channel arbitration, and deep MC queueing**. HBM has **no SerDes**.

> **Wording rule:** do not write "PHY/SerDes pipeline" for HBM. The interposer
> `phy` latency (30 ns = ~5-6 ns wire + ~24-25 ns command-interface/MC
> pipeline) is the command interface and MC pipeline, not a serialization
> tail. (SerDes is real only for `cxl_mem`/`serdes`, e.g. PCM.) HBM core
> timings are comparable to DDR5 (HBM2 tRCD is actually better); the delta is
> the interface, not the array.

This is why the PIM contrast is measured, not asserted: the PEs sit **below**
the command interface and pay none of it -- ~235 ns host view vs ~20-30 ns
subarray-PE view = ~10x, evidence-backed.

## Host-path adder: total measured, split inferred

The host memory idle latency is `physical(tRCD + tCAS) + host-path adder`. The
adder is the gap real sockets pay for IO-die/mesh/fabric traversal that the
device timing model does not include (Ramulator2's scope is the device +
scheduler; fabric/coherence/MC-pipeline/PHY are host-side SoC costs, kept OUT
of device timing so PIM-side pricing stays clean).

The adder ships as an aggregate AND as an optional four-way decomposition
(`fabric_ns + coherence_ns + mc_pipeline_ns + phy_ns`):

| tech | fabric | coherence | mc_pipeline | phy | total | tier |
|---|---|---|---|---|---|---|
| DDR5 | 45 | 15 | 13 | 4 | **77** | LOCKED (anchored) |
| HBM3 | 90 | 60 | 45 | 8 | **203** | LOCKED (anchored) |
| DDR4 | 48 | 16 | 15 | 4 | 83 | INFERRED |
| DDR3 | 48 | 16 | 14 | 4 | 82 | INFERRED |
| LPDDR5 | 49 | 16 | 15 | 4 | 84 | INFERRED |
| GDDR6 | 60 | 20 | 34 | 6 | 120 | INFERRED (deep graphics MC reorder) |
| HBM2 | 43 | 29 | 22 | 4 | 98 | INFERRED (interposer, HBM3-scaled) |
| ReRAM | 45 | 15 | 13 | 4 | 77 | INFERRED (DDR5-class fabric) |
| PCM | 45 | 15 | 13 | 4 | 77 | INFERRED (DDR5-class fabric) |
| SRAM | 0 | 0 | 0 | 0 | 0 | structural zero (on-die) |
| STT_MRAM | 0 | 0 | 0 | 0 | 0 | structural zero (on-die) |

> **Honesty note (total measured, split inferred).** The row **total** is
> measurement-anchored (real-socket idle load-to-use). The four-way **split**
> is INFERRED -- no published per-component decomposition exists. Docs state
> the total is measured, the default split is a documented allocation, and any
> `host_path.*` override makes the split the user's own modeling choice. The
> two anchored splits (DDR5, HBM3) and every inferred row sum EXACTLY to the
> shipped total, so switching between the aggregate and decomposed forms never
> changes the effective latency. Setting both the aggregate `latency_adder_ns`
> and any `host_path.*` is a config error (competing totals).

## source -> sim provenance (no runtime feedback)

The constants reach a run through three layers, in order -- there is **no
runtime feedback loop** (the simulator never measures and re-tunes):

1. **Source-level default constants** -- compiled-in per-tech tables:
   `bridgeDefaultsForTech`, `getHostPathSplit`, and the uncached bounds in
   `pimid/src/main.cpp`. This is where the anchors above live.
2. **Live physical composition** -- at config emission the host-memory idle
   latency is composed from the actual memory timing
   (`getMemoryLatencyCycles`, tRCD + tCAS) PLUS the source-level adder, then
   cycle-converted at the host clock. The bridge/coherence/launch cycle counts
   are likewise composed from the ns constants at the host/reference clock.
3. **Config override** -- any field (`system.bridge.*`,
   `system.coherence.*`, `system.launch.*`, `memory.latency_adder_ns`,
   `memory.host_path.*`) overrides its default; partial `host_path` overrides
   merge over the per-tech split. Overrides are the ONLY way a number changes
   at runtime: the composed split is a fixed function of the configuration.

Inspect the composed result on the login node (no compute node needed) with
`PIMID_DEBUG_HOSTMEM`, `PIMID_DEBUG_COHERENCE`, `PIMID_DEBUG_LAUNCH`.

## Bridge attach-latency anchors

| phy | latency | composition |
|---|---|---|
| `on_die` | ~2 ns | core-clock load/store port (SRAM/STT) |
| `pcb` | ~18-20 ns | pipeline-inclusive commodity bus (DDR-class) |
| `interposer` | ~30 ns | ~5-6 ns wire + ~24-25 ns command-interface/MC pipeline (HBM) |
| `serdes` | ~150-250 ns | CXL.mem round trip + flit packetization (PCM, post-Optane) |

Recomposed idle host access (hit / closed / conflict): DDR5 ~60/77/95 ns,
HBM3 ~82/98/108 ns before the host-path adder -- the delta matches measured
machines. The residual ~40 ns under real sockets is the legitimate
no-mesh/no-directory footnote (this model has no coherence directory or mesh
of its own; that traversal is exactly what the host-path adder supplies).

## Power and energy (1.7.6)

Co-sim cells now report `total_power_W` and `total_energy_nJ` on the same
McPAT path as the device-only figures -- there is no co-sim-specific power
methodology, the host cores are priced by the standard McPAT core/cache models
and the device by the usual PE/memory models. This was gated in earlier 1.7.x
by a host-OoO McPAT defect: a missing `fp_issue_width` XML parameter left the
floating-point issue queue with zero ports, so power analysis aborted with
"Must have at least one port" and co-sim had to run `--no-power` (cycles only).
1.7.6 supplies the parameter; co-sim energy is now available by default. The
boundary/host-memory constants on this page are timing constants and are
unaffected by the power path.

## Caveats to state in the manuscript

- The NVM trio (SRAM/STT on_die, ReRAM pcb, PCM serdes) varies media AND
  attach together, as real products do -- it is not a media-only ablation.
  Override to a common attach for that isolation.
- Absolute host latencies are ~40 ns optimistic vs real sockets (no mesh, no
  directory); the host-path adder closes that gap to the measured anchors, and
  the PIM contrast is a ratio, so the residual cancels.
