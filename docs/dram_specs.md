# DRAM technology specifications in PIMID

PIMID's DRAM timing comes from **Ramulator2** presets. This document records
what those presets model, how they map to JEDEC standards, and the known gaps
between the simulated parts and the latest shipping silicon.

## Supported technologies

| Tech | Ramulator2 org preset | Timing preset | Data rate | Per-channel BW | Channels (PIMID) | Stack/part BW |
|---|---|---|---|---|---|---|
| DDR3 | `DDR3_8Gb_x8` | `DDR3_1600K` | 1600 MT/s | 12.8 GB/s | 1 | 12.8 GB/s |
| DDR4 | `DDR4_8Gb_x8` | `DDR4_2400R` | 2400 MT/s | 19.2 GB/s | 1 | 19.2 GB/s |
| DDR5 | `DDR5_8Gb_x8` | `DDR5_3200AN` | 3200 MT/s | 25.6 GB/s | 1 | 25.6 GB/s |
| LPDDR5 | `LPDDR5_8Gb_x16` | `LPDDR5_6400` | 6400 MT/s | 12.8 GB/s | 1 | 12.8 GB/s |
| GDDR6 | `GDDR6_8Gb_x16` | `GDDR6_2000_1350mV_double` | 16 Gb/s/pin | 32 GB/s | 2 | 64 GB/s |
| HBM2 | `HBM2_4Gb` | `HBM2_2.4Gbps` | 2.4 Gb/s/pin | 32 GB/s | 8 | 307 GB/s |
| HBM3 | `HBM3_4Gb` | `HBM3_6.4Gbps` | 6.4 Gb/s/pin | 102 GB/s | 16 | 819 GB/s |

HBM gen-1 is **not supported** (removed) -- use HBM2 or HBM3. An unknown
`memory.technology` value is a hard error (PIMID exits rather than silently
falling back to DDR4).

## JEDEC verification (2026-05)

Each preset was checked against the relevant JEDEC standard / vendor
datasheet. Physical row timings (tRCD, tRP, tRAS, tRC) are constant in ns for
a generation; cycle counts follow from the clock period (tCK).

| Tech | Standard | Verdict |
|---|---|---|
| DDR3-1600K | JESD79-3 | OK matches (11-11-11, tRAS 35 ns, tRC 49 ns) |
| DDR4-2400R | JESD79-4 | OK matches (16-16-16, tRAS 32 ns, tRC 45 ns) |
| DDR5-3200AN | JESD79-5 | OK within one speed-grade (nCL 24 vs spec-AN 22; ~1.25 ns) |
| LPDDR5-6400 | JESD209-5 | OK matches (tRCDpb ~18 ns, tRAS ~42 ns) |
| GDDR6 16 Gb/s | JESD250 | OK matches vendor specs (tRCD ~14.8 ns, tRC ~45 ns) |
| HBM2 | JESD235C | **fixed** -- see below |
| HBM3 | JESD238 | **fixed** -- see below |

### HBM2 / HBM3 correction

Ramulator2's stock `HBM2_2Gbps` / `HBM3_2Gbps` presets (still upstream as of
this writing, flagged `// TODO: Find more sources`) encoded physically
impossible row timings: they implied tRCD ~ 7 ns, tRAS ~ 17 ns, tRC ~ 19 ns --
roughly **2x faster than any real HBM part** -- and HBM3 used the wrong data
rate (2.0 Gb/s instead of the JESD238 launch rate of 6.4 Gb/s).

PIMID replaces them with presets derived from JEDEC spec-minimum physical
timings (JESD235C / JESD238): **tRCD = tRP = 16 ns, tRAS = 33 ns, tRC = 49 ns,
tCL = 16 ns, tWR = 16 ns, tFAW = 16 ns, tRFC = 220 ns (4 Gb), tREFI = 3.9 us**.
Cycle counts are computed at the correct clock period:

- `HBM2_2.4Gbps`: tCK = 833 ps -> nRCD 20, nRAS 40, nRC 59, nRFC 265
- `HBM3_6.4Gbps`: tCK = 312 ps -> nRCD 52, nRAS 106, nRC 158, nRFC 706

This was verified *not* to be reproducible by linearly scaling the old
preset -- scaling would have propagated the wrong base timing. The values come
from the JEDEC physical numbers directly.

## Known gaps vs. latest silicon

PIMID's presets capture one JEDEC data point per generation. Real parts have
moved well past these:

| Tech | PIMID models | Latest shipping (~2025) |
|---|---|---|
| DDR4 | 2400 MT/s | up to 3200 MT/s |
| DDR5 | 3200 MT/s | 8400+ MT/s |
| LPDDR5 | 6400 MT/s | LPDDR5X 10700 MT/s |
| GDDR6 | 16 Gb/s | GDDR6X/GDDR7 up to 24-32 Gb/s |
| HBM2 | 2.4 Gb/s (307 GB/s) | HBM2E 3.6 Gb/s (461 GB/s) |
| HBM3 | 6.4 Gb/s (819 GB/s) | HBM3E 9.8 Gb/s (1229 GB/s); HBM4 (2048 GB/s) |

These are deliberate scope boundaries, not bugs: a timing simulator models a
fixed JEDEC snapshot. HBM2E / HBM3E / HBM4 are not modeled. To target a
newer part, add a new Ramulator2 preset with that part's JEDEC timings and
point the wrapper at it.

## Capacity reporting

`capacity_` in the wrapper reflects the **single-rank part** that the org
preset describes (e.g. HBM2 = 4 Gb/channel x 8 channels = 4 GiB), not a
deep multi-die stack. Real HBM stacks (4-Hi ... 16-Hi) reach 8-64 GB by
stacking dies; PIMID does not model die-stacking depth. Treat the reported
capacity as per-modeled-rank, not per-physical-stack.
