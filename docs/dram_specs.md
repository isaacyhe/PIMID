# DRAM technology specifications in PIMID

PIMID's DRAM timing comes from **Ramulator2** presets. This document records
what those presets model, how they map to JEDEC standards, and the known gaps
between the simulated parts and the latest shipping silicon.

## Supported technologies

| Tech | Ramulator2 org preset | Timing preset | Data rate | Per-channel BW | Channels (PIMID) | Stack/part BW |
|---|---|---|---|---|---|---|
| DDR3 | `DDR3_4Gb_x8` (1.12.19; was `DDR3_8Gb_x8`) | `DDR3_1600K` (1.12.38; was `DDR3_1600H`) | 1600 MT/s | 12.8 GB/s | 1 | 12.8 GB/s |
| DDR4 | `DDR4_8Gb_x8` | `DDR4_2400R` | 2400 MT/s | 19.2 GB/s | 1 | 19.2 GB/s |
| DDR5 | `DDR5_16Gb_x8` (every grade since 1.12.141) | `DDR5_4800B` (default since 1.12.19; `DDR5_3200AN` and `DDR5_5600B` selectable) | 4800 MT/s | 19.2 GB/s (one 32-bit sub-channel) | 2 | 38.4 GB/s |
| LPDDR5 | `LPDDR5_16Gb_x16` (1.12.44; was `LPDDR5_8Gb_x16`) | `LPDDR5_6400` | 6400 MT/s | 12.8 GB/s | 4 | 51.2 GB/s |
| GDDR6 | `GDDR6_8Gb_x16` | `GDDR6_2000_1350mV_double` | 14 Gb/s/pin | 28 GB/s | 2 | 56 GB/s |
| HBM2 | `HBM2_4Gb` | `HBM2_2.4Gbps` | 2.4 Gb/s/pin | 32 GB/s | 8 | 307 GB/s |
| HBM3 | `HBM3_8Gb_8H` (16 Gb 8-High, 1.12.41; was `HBM3_4Gb`) | `HBM3_6.4Gbps` | 6.4 Gb/s/pin | 102 GB/s | 16 | 819 GB/s |

HBM gen-1 is **not supported** (removed) -- use HBM2 or HBM3. An unknown
`memory.technology` value is a hard error (PIMID exits rather than silently
falling back to DDR4).

## JEDEC verification

Each preset was checked against the relevant JEDEC standard / vendor
datasheet. Physical row timings (tRCD, tRP, tRAS, tRC) are constant in ns for
a generation; cycle counts follow from the clock period (tCK).

| Tech | Standard | Verdict |
|---|---|---|
| DDR3-1600K | JESD79-3 | OK matches (11-11-11, tRAS 35 ns, tRC 49 ns) |
| DDR4-2400R | JESD79-4 | OK matches (16-16-16, tRAS 32 ns, tRC 45 ns) |
| DDR5-3200AN / 4800B / 5600B | JESD79-5D | OK matches the bins (3200AN 24-24-24, Table 283; 4800B 40-39-39; 5600B 46-45-45) |
| LPDDR5-6400 | JESD209-5 | OK matches (tRCDpb ~18 ns, tRAS ~42 ns) |
| GDDR6 14 Gb/s | JESD250D; Samsung K4Z80325BC | OK matches the vendor's 14 Gb/s AC set at tCK 0.571 ns (tRCD ~15.4 ns, tRC ~45.7 ns) |
| HBM2 | JESD235D | **fixed** -- see below |
| HBM3 | JESD238B | **fixed** -- see below |

### LPDDR5 two-step activation

An LPDDR5 ACTIVATE is two commands, ACT-1 then ACT-2 (JESD209-5C 7.3.1), and
the row timings are measured at ACT-2: tRRD, tRCD, tRAS, tRC and tRP
(Figure 94), tFAW (Figure 97) and the end of tRFCab (Figure 134 Note 3).
ACT-2 must follow ACT-1 within tAAD = 8 clocks, and once an ACT-1 is issued
its ACT-2 comes before the next ACT-1 (Table 201 Note 4). The model measures
every row timing at ACT-2 and holds an ACT-1 until its ACT-2 can follow in
the next clock, as Ramulator 2.1 does; successive ACT-1 commands also keep
tRRD apart. An access to a closed bank therefore issues its column command
one clock (1.25 ns at LPDDR5-6400) later than when tRCD was counted from
ACT-1, and an open row is precharged one clock later (tRAS); a row conflict
(precharge, then activate) still costs tRP + tRCD.

### HBM2 / HBM3 correction

Ramulator2's stock `HBM2_2Gbps` / `HBM3_2Gbps` presets (still upstream as of
this writing, flagged `// TODO: Find more sources`) encoded physically
impossible row timings: they implied tRCD ~ 7 ns, tRAS ~ 17 ns, tRC ~ 19 ns --
roughly **2x faster than any real HBM part** -- and HBM3 used the wrong data
rate (2.0 Gb/s instead of the JESD238 launch rate of 6.4 Gb/s).

PIMID replaces them with presets built from the JEDEC physical timings
(JESD235D / JESD238B): **tRCD ~16 ns, tRP 15 ns, tRAS ~33 ns, tRC ~48 ns,
tREFI 3.9 us**. Cycle counts are computed at each standard's CK period (for HBM3
the CK runs at a quarter of the pin data rate):

- `HBM2_2.4Gbps`: tCK = 833 ps -> nRCD 20, nRP 18, nRAS 40, nRC 58, nFAW 20, nREFI 4681
- `HBM3_6.4Gbps`: tCK = 625 ps -> nRCD 26, nRP 24, nRAS 53, nRC 77, nFAW 26, nREFI 6240

The refresh cycle time follows the channel density (Ramulator2's refresh table): tRFC 260 ns at HBM2's 4 Gb per
channel (nRFC 313) and 350 ns at HBM3's 8 Gb per channel (nRFC 560); see [sources.md](sources.md).

This was verified *not* to be reproducible by linearly scaling the old
preset -- scaling would have propagated the wrong base timing. The values come
from the JEDEC physical numbers directly.

## Command spacing taken from the standards

The impl files also carry the command-to-command rules. Where a preset value is not enough they follow the JEDEC
tables or, for GDDR6, the public Samsung K4Z80325BC Rev. 1.3 data sheet (Tables 71, 92 and 93):

| Rule | DDR3 / DDR4 / DDR5 | GDDR6 | HBM3 |
|---|---|---|---|
| READ to WRITE (tRTW) | RL + BL/2 + 2 - WL (DDR5: + 2 more for its two-tCK write preamble) | CLmrs + BL/4 - WLmrs + 3 + (tWCK2DQO(max) - tWCK2DQI(min))/tCK = 27 nCK | (RL + BL/4 - WL + 0.5) tCK + tWDQS2DQ_O(max) - tWDQS2DQ_I(min), rounded up = 20 nCK (JESD238B.01 Table 93 NOTE 18) |
| PRECHARGE to PRECHARGE (tPPD) | DDR5 2 nCK (JESD79-5D) | 1 nCK at 1.35 V, 2 nCK at 1.25 V | 2 nCK per pseudo channel |
| READ / WRITE with auto precharge to PRECHARGE all | tRTP / WL + BL + tWR, as the plain command | as the plain command | as the plain command |
| ACTIVATE to PRECHARGE all; PRECHARGE all to ACTIVATE or REFRESH | tRAS; tRP | tRAS; tRP | tRAS; tRP |
| WRITE to READ in another rank | WL + BL + tCS - RL | - | - |

HBM2's read-to-write turnaround follows JESD235D Table 68 NOTE 23 (17 nCK) and LPDDR5 has a 2 nCK tPPD (JESD209-5C).
GDDR6's bus turnaround is the published strobe-offset difference (0.7 ns; the data sheet gives the PLL-off ranges
and no PLL-on range), the term JESD238B.01 uses for HBM3. Every shipped organisation has one rank per channel, so the
rank-to-rank rules are not reached.

## Known gaps vs. latest silicon

PIMID's presets capture one JEDEC data point per generation. Real parts have
moved well past these:

| Tech | PIMID models | Latest shipping (~2025) |
|---|---|---|
| DDR4 | 2400 MT/s | up to 3200 MT/s |
| DDR5 | 4800 MT/s (5600 selectable) | 8400+ MT/s |
| LPDDR5 | 6400 MT/s | LPDDR5X 10700 MT/s |
| GDDR6 | 14 Gb/s | GDDR6X/GDDR7 up to 24-32 Gb/s |
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
