# Sources -- every external number's provenance

One row per externally sourced quantity: what it is, the value or band in
use, the source, and where the code consumes it. Local copies of sources
live in the (unpublished) `misc/` archive of the development tree; entries
below name the archive file where one exists. Rule of the house: a quantity
with units of time/energy/rate/fraction/area is either a spec primitive, a
runtime measurement, or it appears here with a source and (where the
sources disagree) a band.

## DRAM standards (JEDEC)

| Quantity | Value in use | Source | Consumed at |
|---|---|---|---|
| DDR3 timing preset (tRAS/tRP/tBurst overrides) | 35 / 13.75 / 5.0 ns (DDR3-1600K) | JESD79-3D (misc/JESD79-3D.pdf) | `ramulator_wrapper.cpp` per-tech overrides |
| DDR3 termination scheme (R7 rd/wr split) | SSTL-135; read loop RON 34 + RTT_NOM 40, write loop 34 + RTT_WR 120 | JESD79-3D Tables 38, 41 (topology); Micron MT41K p.32 IDD conditions ("RON RZQ/7 (34); RTT,nom RZQ/6 (40); RTT(WR) RZQ/2 (120)") | `pimid_energy.h` termination rows; CACTI-IO injection (rtt1/rtt2 per direction) |
| DDR4 electricals (R7 rd/wr split) | POD12; read 34+40, write 34+120 | JESD8-24 (topology, misc/); Micron MT40A p.315 IDD conditions | CACTI-IO injection (`cacti_io_wrapper.cpp`) |
| DDR5 electricals (R7 rd/wr split) | POD at 1.1 V; read 34+40, write 34+120. NOTE: no "POD11" JESD8-* standard exists (family is POD18/15/135/125/12/10, all in misc/); the 1.1 V point is JESD79-5's own | Micron DDR5 core sheet p.453 IDD conditions (MR5 RZQ/7 drivers, MR35 RTT_NOM RZQ/6, MR34 RTT_WR RZQ/2) | CACTI-IO injection |
| GDDR6 electricals (R7 rd/wr split) | POD135; read 40+60, write 40+120; MR1 default = termination DISABLED, IDD operating point priced | Samsung K4Z80325BC p.166 (driver 40 / termination 60 characteristics), p.144 IDD conditions ("All ODTs are enabled with ZQ/2", ZQ=240), p.49 MR1; JESD250D (topology) | CACTI-IO injection |
| Controller-side write driver RON (DDR4/DDR5) | applied 34 ohm, inside the sourced host range 30-50 ohm | Intel 743844-015 (misc/) Table 87 p.208 (DDR5: RON_UP/DN(DQ) 30-50 ohm, RODT(DQ) 30-240) and Table 86 p.207 (DDR4: 30-50, RODT 40-200); range stated at the row, point chosen within it | `pimid_energy.h` (ron_wr) |
| Controller-side write driver RON (GDDR6) | applied 40 ohm, a member of the sourced host set {40/48/60} | Achronix Speedster7t GDDR6 User Guide UG091 (misc/achronix_speedster7t_gddr6_user_guide_ug091.pdf), Table 3 p.16: "DQ driver impedance (RON) 40/48/60 ohm" (host PHY; CA RON same set, CA term 60/120/240) -- closes the R7 split's last stated assumption (2026-09-04) | `pimid_energy.h` (ron_wr) |
| LPDDR5 termination topology | LVSTL, VSS-referenced; DQ ODT default = Disable | Micron datasheets (misc/315b-441b-*.pdf, misc/MICT-S-A0025741931-1.pdf); JESD209-5C Table 84 p.144 (misc/JESD209-5C.pdf) | `pimid_energy.h` (rtt=0 encodes the JEDEC default; sourcing residue N8 CLOSED 1.11.63) |
| LPDDR5 VDDQ | 0.50 V TYP (0.30 V ODT-off; range 0.47-0.57 V) | Micron LPDDR5X y52p p.1 Features and Table 18 note 5 p.43 (a confidential-marked sheet formerly cited here is no longer cited, user ruling 2026-09-26) | CACTI-IO injection (`cacti_io_wrapper.cpp`), `pimid_energy.h` |
| LPDDR5 driver RON | 40 ohm | Micron IDD-table Note 4 ("Output load = 5pF; RON = 40 ohms; TC = 25 C"): y52p p.43, y52q p.33, MICT-S p.30, y4bm p.25 | CACTI-IO injection, `pimid_energy.h` |
| LPDDR5 DQ ODT (Rtt) | Disable (Default); ladder RZQ/1..RZQ/6 = 240/120/80/60/48/40 ohm (RZQ=240), 111B RFU; NT-ODT also defaults off | JESD209-5C Table 84 p.144 (misc/JESD209-5C.pdf, acquired 2026-08-24) -- supersedes the 1.11.52-1.11.59 assumption chain (240 ohm assumed from host-side Intel 743844-015 Tbl 89 RODT range) | `pimid_energy.h` (rtt=0, LVSTL branch returns 0 for it), `cacti_io_wrapper.cpp` (row sourced=true); ODT-on systems use power.termination_pj_per_bit |
| HBM3 I/O rails | VDDQ 1.1 V TYP, VDDQL (TX driver output stage) 0.4 V TYP; unterminated (IOUT = 0 mA, Ctotal = 2.5 pF); driver strength specified in CURRENT (8/10/12/14 mA, MR6) not ohms | JESD238B.01 Table 70 p.152 (cl.7.2), cl.9.1 p.164/166, Table 17 p.34 | not injected -- the (vddq, rtt, ron) injection shape has no rtt/ron to give for HBM |
| Power-down entry/exit threshold (E17) | DDR3/DDR4 6.0 ns; DDR5/LPDDR5 7.5 ns | JESD79-3D (tXP max(3nCK,6ns)); JESD79-4; JESD79-5; JESD209-5 | `gapPowerDownResidency()` in `main.cpp`; settable via `memory.power_down_threshold_ns` |
| HBM3 refresh completeness (nRFCSB fill) | tRFC upper bound, stated; JESD238B locally available for the real per-density values (upgrade queued) | JESD238B (misc/JESD238B.01.pdf) | `external/ramulator/.../HBM3.cpp`, `HBM2.cpp` |
| HBM die population minimum | 2 channels per core die (8ch->4, 16ch->8) | JESD235/238 | `memorySystemDieCount()` |
| IDD background/state currents | per-preset IDD2N/IDD2P/IDD3N etc. | JEDEC per-generation + Micron datasheets, rows commented individually | `pimid_energy.h` |

## IDD rows: one typical value per quantity, by component (1.11.91 item 11)

User rulings 2026-09-26 18:44-19:00 (one absolute number per quantity) and
19:30 (factors act on COMPONENTS -- the quantities the IDD loops measure --
not on each current). The rows of `iddTableFor()` store the datasheet
maxima; `typicalFromSpec()` derives the typical currents with the row's
`componentFactorsFor()` set. The chain and the bands are in the code
comments and in the changelog; the print-out carries one word (MEASURED /
DERIVED / CALIBRATED).

| Anchor | Value in use | Source | Consumed at |
|---|---|---|---|
| DDR3L measured/datasheet ratio per current | three-vendor means IDD0 0.427, IDD2N 0.566, IDD3N 0.367, IDD4R 0.736 (I/O-corrected), IDD4W 0.542, IDD5B 0.829 (band: vendor A/B/C, e.g. IDD3N 0.234/0.532/0.334); IDD2P1 given only as ranges. DDR3 component set: standby 0.566, active-standby premium 0.367/0.566, activate excess 0.43 (IDD0 loop), burst excess 0.736 rd / 0.542 wr, refresh excess 0.829, power-down 0.566 | Ghose, Yaglikci, Gupta, Lee, Chandrasekar, Ma, Mutlu, "What Your DRAM Power Models Are Not Telling You: Lessons from a Detailed Experimental Study", Proc. ACM Meas. Anal. Comput. Syst. (SIGMETRICS) 2018, https://arxiv.org/abs/1807.05102 (50 DDR3L-1600 modules, 3 vendors) | `pimid_energy.h` `componentFactorsFor("DDR3")` (CALIBRATED); the two-generation set |
| DDR4 measured/datasheet ratio per current | IDD0 0.540 (1120->605 mA), IDD2N 0.576 (1040->599), IDD3N 0.350 (1540->539), IDD4R 0.553 (1750->967), IDD4W 0.430 (1590->684); refresh not calibrated; energy-level ratios 0.25-0.51 (median 0.47). DDR4 component set: standby 0.576, premium 0.350/0.576, activate excess 0.540, burst excess 0.553 rd / 0.430 wr, refresh excess 0.83 (Ghose), power-down 0.576 | Shi et al., "Calibrating DRAMPower Model for HPC: A Runtime Perspective from Real-Time Measurements", arXiv:2411.17960 (v3), Fig. 4(b) (Samsung M393A1G43DB0-CPB DDR4-2133 8 GB RDIMMs, HDEEM, Haswell node) | `pimid_energy.h` `componentFactorsFor("DDR4")` (CALIBRATED; ratios only, not the per-DIMM values); the two-generation set |
| Two-generation component factors | precharged standby 0.57; active-standby premium 0.36/0.57 (IDD3N_t = IDD2N_t + premium x (IDD3N - IDD2N), >= IDD2N_t); activate excess (IDD0 loop minus its TN-41-01 baseline) 0.5; burst excess 0.6 rd / 0.5 wr; refresh excess (IDD5B - IDD2N) 0.83 (Ghose only); power-down 0.57 | the two rows above (user ruling 19:30); assumption: newer generations are guardbanded as DDR3L and DDR4 were. [The first, per-current pass (IDD0 0.48 ... IDD2P 1.00) was withdrawn: DDR5 IDD0 fell below IDD2N] | `pimid_energy.h` `componentFactorsFor()`: DDR5 (3200/4800/5600), LPDDR5, LPDDR5X, GDDR6 (DERIVED); none for HBM2/HBM3 |
| GDDR6 IDD maxima | per device, x32, 1.35 V, HC14: IDD0 430, IDD2N 310, IDD3N 460, IDD4R 1220, IDD4W 1470, IDD5 790, IDD2P 230, IDD7 1640 mA; tRFCab 120 ns (Table 91); row = device/2 per channel (maxima), typical by the two-generation component set; activate 3.24 nJ/channel ACT from IDD7 - IDD4R, x activate-excess 0.5 = 1620 pJ | Samsung K4Z80325BC 8Gb GDDR6 SGRAM C-die Rev. 1.3 (misc/52839ef626add36b171d2cdf17bacfd2.pdf) Table 82 PDF p.146, Tables 90-91; derivation _1166audit/IDD_DERIVATION_HBM3_GDDR6.md section 2 | `pimid_energy.h` GDDR6 row (DERIVED) |
| HBM2 measured IDD (base of HBM2 and HBM3) | 36-stack means / 8 per channel: IDD0 140.6, IDD2N 135.9, IDD3N1 133.4, IDD4R 463.8, IDD4W 355.6, IDD5B 189.3 mA at 1.2 Gb/s/pin on PC0 only, IDD5 loop tRFC 266.7 ns | CMU-SAFARI HBM-Power artifact, branch "artifact", https://github.com/CMU-SAFARI/HBM-Power/tree/artifact, accessed 2026-09-26 (paper not yet public): data/all_idd_measurements.csv, sources/table3/configs/HBM2_1.2Gbps_timing_BL4.json, power_measurement.cpp; its "ipp" column is a card-power residual (platform.cpp), NOT used | `pimid_energy.h` HBM2 row (MEASURED, burst at constant energy per bit x 307.2/76.8) and HBM3 row (DERIVED) |
| HBM2 per-bit energy cross-check | 3.92 pJ/bit per access; row activation 909 pJ; I/O 0.80 of 3.48 pJ/b | O'Connor et al., "Fine-Grained DRAM: Energy-Efficient DRAM for Extreme Bandwidth Systems", MICRO-50 2017 (misc/MICRO_2017_Fine_Grained_DRAM.pdf) p.3, Table 3 p.10 | HBM3 row derivation (I/O share f_io 0.23) and check |
| LPDDR5 / LPDDR5X IDD maxima (VDD2H) | IDD0 53, IDD2N 32.5, IDD3N 40, IDD4R 390, IDD4W 265, IDD52H 205, IDD2P 3.9 mA, x16, 7500 Mb/s, 16 Gb die; LPDDR5 row: burst excess x 6400/7500 (stored), typical by the two-generation component set; LPDDR5X row: unscaled, same set (technology not selectable: no public AC timing set) | Micron Y52P LPDDR5X SDRAM (misc/315b-441b-561b-563b-y52p-sdp-ddp-qdp-8dp-lpddr5x.pdf, Rev. H 03/2025, public) Table 18 PDF pp.42-43; tRFCab 280 ns Table 6 p.11. Checked for an AC timing table and found none: Micron automotive LPDDR5X 561b Y52Q DDP (MT62F512M64D2, Rev. G 05/2025, https://www.mouser.com/catalog/specsheets/Micron_10-16-2025_561b-y52q-ddp-auto-lpddr5x.pdf, 33 pp.) -- only density-dependent refresh (Table 4) and 16-bank 3733/4267 Mb/s RL/WL/nWR (Table 8); the rest deferred to General LPDDR5/LPDDR5X Specifications 2/3 | `pimid_energy.h` LPDDR5 (DERIVED) and LPDDR5X rows |
| HBM per-stack floor | HBM2 783.3 mA x 1.2 V = 939.96 mW per stack; HBM3 x 1.1/1.2 = 861.6 mW; one absolute value, priced once per stack in every state | CMU-SAFARI HBM-Power artifact data/no_hbm_idd2_measurements.csv (IDD2, one channel enabled: 821.3 mA) against all-channel IDD2 1087.4 mA -> 38.0 mA/channel + 783.3 mA floor; artifact IDD_ours_allzeros.json "off-power (778.1)". Reading (user ruling option (c)): inside the package -- logic-die PHY, clock trees, always-on circuits | `pimid_energy.h` `stack_floor_mw`, `stackFloorSystemMW()`; Background line `+stack=` (included) |
| HBM2E/HBM3E pJ/bit bracket | HBM2E ~4.3 pJ/bit, HBM3E 20% lower (~3.44) | Moon, Son, Lee (SK hynix), IEDM 2023 paper 15-6, handout p.2 | HBM3 row check (derived 3.92 pJ/bit at miss 1/16) |
| DRAM device width population (x4/x8/x16 -> chips/rank) | 16/8/4 per 64-bit channel | JEDEC channel arithmetic (spec primitive) | `memorySystemDieCount()`, `backgroundUnits()` |
| Array energy population method | energies x devices-per-access | Micron TN-41-01 method | `devicesPerAccess()` in `pimid_energy.h` |

## Die area / density (measured silicon)

| Quantity | Value in use | Source | Consumed at |
|---|---|---|---|
| DDR4 full-die density | 0.296 Gb/mm^2 (SK Hynix D1z) | TechInsights/SemiAnalysis | `vendorDieDensity()` |
| DDR5 density | 0.315 Gb/mm^2 (Micron D1a, 8Gb/25.41mm^2) | TechInsights | same |
| LPDDR5 density | 16Gb/43.98mm^2 (Samsung D1z) | TechInsights | same |
| GDDR6 density | 8Gb/37.03mm^2 (Samsung K4Z80165BC) | TechInsights floorplan | same |
| HBM3 density | 0.160 Gb/mm^2 (SK Hynix) | SemiAnalysis | same |
| HBM2 density | 8Gb user / 96mm^2 (12x8mm, 20nm) | Sohn et al., ISSCC 2016 18.2 / JSSC 52(1) (misc/sohn2016.pdf, misc/sohn2017.pdf) | same |
| DDR3 density | 4Gb/30.9mm^2 (SK Hynix 23nm) | ISSCC 2012 paper 2.3 press kit | same |
| Array fraction of die | BLSA 8-15%, LWD 5-10% (structure statement) | Vogelsang, MICRO 2010 (misc/vogelsang2010.pdf) | on-pitch boundary rationale (L116/N1) |

## DRAM-process compute (the family + pitch model)

| Quantity | Value in use | Source | Consumed at |
|---|---|---|---|
| Family factors (fa/fd/fl) | derived per run from CACTI .dat hp vs comm-dram columns at the configured node/corner/temperature | CACTI 7 technology tables; the DRAM columns themselves are CACTI-D (Thoziyoor et al., ISCA 2008) | `periphFamilyFactors()` |
| SUBARRAY pitch factor | band [1.25, ~4]; default 1.25 | low end: Samsung FIMDRAM, ISSCC 2021 25.4 (~1.5mm^2/PCU, HBM2/1y, lean SIMD); upper: UPMEM ~10x density claim / family factor (Hot Chips 31, misc/HC31_1.4_UPMEM.FabriceDevaux.v2_1.pdf; process facts in Gomez-Luna et al., arXiv:2105.03814, misc/gomezluna2021_upmem_benchmarking.pdf) | `main.cpp` pitch derivation (N3) |
| DRAM generation feature size F | 1x 19 / 1y 17.5 / 1z 15.5 / 1a 14 / 1b 12.5 nm; cell = 6F^2 buried-wordline | industry generation naming (TechInsights usage); 6F^2 architectural fact | `dramGenFeatureNm()` (reporting) |
| On-die PE process | pinned to generation table (N2 ruling) | measured non-cancellation (this project, gate 1161I5) | config pin + family sites |
| DPU metal-layer constraint | DRAM process ~3 metal layers vs >10 logic | Gomez-Luna et al. quoting UPMEM | pitch band rationale |

## Links / interconnect

| Quantity | Value in use | Source | Consumed at |
|---|---|---|---|
| PCIe gen3/4/5 pJ/bit | gen3 4.0; gen4 1.93 (TX-only) - 6.0; gen5 7.6 - 11.4 | published PHY surveys, per-entry comments | `linkEnergyBandPJPerBit()` |
| NVLink pJ/bit | 1.17 - 1.30 | NVIDIA NVLink4 claims | same |
| UCIe pJ/bit | 0.25 - 0.5 | UCIe consortium figures | same |
| UALink pJ/bit | 3.5 (200G-class SerDes row) | D1 sourcing | same |
| Interposer I/O | 0.3 pJ/bit (app toggle) / 0.80 (50%) | O'Connor et al., MICRO-50 2017 Table 3 (misc/MICRO_2017_Fine_Grained_DRAM.pdf) | MC interposer tier (D2) |
| SerDes per-lane rates | PCIe 8/16/32 GT/s; NVLink4 100G; UALink 200G | PCI-SIG / vendor | `memoryctrl.cc` PIMID block |
| PIPE controller width convention | 32-bit -> gen3 250 / gen4 500 / gen5+CXL 1000 MHz | Intel PIPE spec rev 7.1 | link controller clock |
| Link bandwidth | rate x lanes x encoding (derived, no constant) | spec primitives | `linkBandwidthGBs()` |

## Tools' own models (integrated components)

See the README component table: the seven vendored tools plus THREE
distinct models shipping inside the CACTI tree, each with its own paper and
each load-bearing for PIMID:
- CACTI-IO (Jouppi et al., ICCAD 2012) -- off-chip interface: termination,
  PHY power, IO area.
- CACTI-P (Li et al., ICCAD 2011) -- sleep-transistor / Vcc_min power
  gating; its gated endpoints and retention ratios (periphery 0.35, cell
  0.65 of Vdd) are the authority behind every PIMID power-gating number.
- CACTI-3DD (Chen et al., DATE 2012) -- 3D die-stacked DRAM with a TSV
  model (`external/cacti/TSV.cc`, the CACTI3D paths in `uca.cc`/`io.cc`,
  and a 3D-stacked DRAM sample config). VENDORED AND COMPILED BUT NEVER
  INVOKED by PIMID: nothing in `src/` sets `is_3d_mem`, so our HBM stacks
  are priced today with the interposer I/O band (O'Connor) and a JEDEC
  die-count population, with no TSV area or TSV energy term at all. Same
  shape CACTI-IO had before 1.11.42 harnessed it; logged as an open item
  rather than left silent.
- CACTI-D (Thoziyoor et al., ISCA 2008) -- the DRAM technology extension.
  It supplies the `comm-dram` and `lp-dram` cell/device types, i.e. the
  columns the ENTIRE DRAM-periphery family factor is a ratio against
  (fa/fd/fl = comm-dram vs hp/lstp/lp-dram), the DRAM array model behind
  the JEDEC-calibrated die areas, and the lp-dram column the E2 corner
  ruling maps a low-power on-die PE onto.
Their internal constants are theirs; where PIMID overrides or injects a
value, the row above carries the source, and the injection prints it.

## Local archive

The development tree's `misc/` folder holds local copies of: JESD79-3D,
JESD212C, JESD232A, JESD235B ballout, JESD238B (HBM3), JESD239E, JESD250D
(GDDR6), JESD270-4A, JESD330-4, assorted JESD8-* interface standards, the
Micron LPDDR5X datasheets, Sohn 2016/2017, Vogelsang 2010, O'Connor 2017,
the UPMEM Hot Chips 31 deck, and Gomez-Luna 2021. `misc/` is not part of
the published tree; this file is.

## Corroborations and normative upgrades (2026-09-04 document sweeps)

| Item | Corroborating/upgrading source | Effect |
|---|---|---|
| DDR5 preset chain (bins 3200AN/BN/C = 24/26/28, nRAS 52, nWR 48, nRTP 12, nCCD*/WTR formulas, all nine orgs, rounding algorithm, VDD/VPP) | JESD79-5D v1.41 (misc/JESD79-5D.pdf, published Nov 2025): Table 283 p.390, Table 334 p.449, Tables 4-7 p.7, cl.13.2 p.447-448, Table 196 p.330 | Micron-transcription citations upgraded to NORMATIVE; the Q3'16 ballot draft's differing bins are superseded (odd CLs eliminated, NOTE 12 p.407) |
| DDR5 R7 electricals (RON 34/40/48 @ RZQ=240; RTT_WR default 240; RTT_NOM_WR/RD default 80; RTT_PARK default OFF; IDD conditions RZQ/7 + RZQ/6 + RZQ/2) | JESD79-5D MR5 p.38, MR34 p.61, MR35 p.62, Table 315 NOTE 2 p.431, Tables 191/245 pp.325/366 | All four R7 anchors now NORMATIVE; the "self-contained 1.1 V point, no JESD8-*" claim is confirmed (the standard never references any JESD8-* document) |
| HBM2 refresh model (tREFI 3.9 us; tRFC 260 ns @4H / 350 ns @8H) | AMD PG276 v1.0 p.23 (misc/amd_pg276_axi_hbm_controller_product_guide.pdf) -- vendor-authored controller guide | Independent second-vendor confirmation of the JESD235D-anchored values; also sources tREFI halving at 85-95 C |
| HBM temperature-refresh ladder (4x/2x/1x/0.5x/0.25x tREFI by TEMP[2:0]) | Intel UG-20031 Table 30 p.66 + Agilex M HBM2E IP UG Table 5 p.17 (misc/); AMD DS923 note 16 p.5 (>=4x above 95 C) | Sourced across three vendors; candidate future modeling item (current refresh model is temperature-flat) |
| HBM3 tRFCab 260 ns at the emitted HBM3_4Gb org | JESD238B.01 Table 93 PDF p.179: tRFCab keyed by CHANNEL density -- 4 Gb/ch 260, 8 Gb/ch 350, 16 Gb/ch 450 | Community flag (ramulator2/gem5 "350 ns") resolved: they describe an 8 Gb/channel org; both readings are the same table, different rows |
| HBM1/HBM2 timing structure + I/O energy class | SK hynix Hot Chips 26/28 decks (misc/paper_hc26/hc28_*.pdf): tRC 40-48 ns, tCCD 1 CK, 2 KB page, per-gen speeds, I/O energy ratios | Vendor-authored public corroboration for the HBM2 rung |
| HBM2E PHY energy | Samsung HC32 poster (misc/paper_hc32_*.pdf) p.11: measured WRITE 1.07 / READ 0.56 / IDLE 0.02 pJ/b at 0.75/1.2 V | Validation band for the interface-energy layer |
| HBM2 measured IDD + model-error band | CMU-SAFARI HBM-Power artifact (misc/zen_hbm2_idd_*.csv): 36-chip IDD distributions; DRAMSim3-class HBM2 power model = 20.7% MAPE (all-0s) / 39.8% (random) vs silicon | The citable error bar for simulator-derived HBM energy; per-chip spread 1.6-1.8x standby |
| HBM2 measured latency/timing | Shuhai TC'21 (hit/closed/miss 106.7/122.2/137.8 ns); DSN'24 read-disturbance artifact (tRAS 29 ns, tREFI 3.9 us, tRC ~45-48 ns on real parts) | Row-buffer state machine and refresh-model validation anchors |

## Vendor silicon papers (ISSCC/JSSC, acquired 2026-09-17)

Six vendor-authored device papers, cited under RIKEN's IEEE Xplore license.
These are the strongest public source class for the parts whose datasheets
are NDA-only. Note what they do and do not contain: vendor papers publish
bandwidth, voltages, organization, die/package geometry, energy-per-bit for
the PHY, and measured operating corners -- they do NOT publish JEDEC-style
core AC timing tables or IDD tables.

| Item | Value | Source | Use |
|---|---|---|---|
| HBM2 org bank/row split | 2 Gb/ch: 8 banks/PC, RA[13:0]; 4 Gb/ch: 16 banks/PC, RA[13:0]; 8 Gb/ch: 16 banks/PC, RA[14:0]; 1 KB page/PC at every density | JESD235D Table 4 p.6, Table 5 p.8 | CALIBRATION -- fixed the 4 Gb and 8 Gb org presets (1.11.64) |
| HBM2 core-die area (2nd vendor anchor) | 81.8 mm^2, 8 Gb 2-channel core die | Cho et al., SK hynix, ISSCC 2018 12.3 Fig.12.3.7 | Band with Sohn's 96.00 mm^2: an 8 Gb HBM2 core die is ~82-96 mm^2 across vendors |
| HBM2 per-TSV driver current | ~880 uA (multi-drop) -> ~610 uA (spiral P2P), 1.0 V, 3.3 Gb/s PRBS; derived ~0.27 -> ~0.19 pJ/bit for the TSV driver alone | Cho ISSCC 2018 Fig.12.3.1 (chart-read; the pJ/bit derivation is ours) | 1.11.91 (R8-8): charged as the HBM IDDQ BAND on DQ-crossing reads -- IDD is the VDDC rail only and IDDQ is not published (JESD238B.01 PDF p.164); was "validation only" |
| HBM2 refresh | 8K / 32 ms -> tREFI 3.90625 us; page 1 KB/PC | Cho ISSCC 2018 Fig.12.3.7 | Corroborates the JESD235D-anchored refresh model |
| HBM2E clock + latency | tCK 0.4 ns measured at 5 Gb/s/pin and VDD 1.1 V; RL41 | Chun et al., Samsung, JSSC 56(1) 2021 Fig.18(a) p.207 | Reference rung; HBM2E is not a modelled technology |
| HBM3 CK domain | tCK = data_rate / 4, stated by BOTH vendors: 2 nCK = 1 ns at 8 Gb/s (Ryu JSSC 58(4) p.1052); 1 tCK = 571.4 ps at 7 Gb/s (Park JSSC 58(1) p.259) | Ryu 2023; Park 2023 | CONFIRMS the 1.11.63 CK-domain correction; refutes the rate/2 convention common in open-source models |
| HBM3 column spacing | tCCDS = 2 nCK (different bank group), tCCDL = 4 nCK (same bank group) | Ryu JSSC 58(4) p.1052 | First vendor corroboration of our nCCDS 2 / nCCDL 4 |
| HBM3 organization | 16 ch x 2 pCH x 32 DQ, BL8, 32 B granularity, 16 banks/pCH in 4 BG, 16384 rows, 1 KB page/pCH, 2 Gb/pCH -> 16 Gb core die | Ryu p.1052/1057; Park p.257/264; Lee ISSCC 2024 Fig.13.4.6 p.239 | Closes arithmetically across three independent papers; our preset is equivalent (different factorization) |
| HBM3 rails | VDD/VDDQ/VDDQL/VPP = 1.1 / 1.1 / 0.4 / 1.8 V; JEDEC minimum operating voltage 1.067 V | Ryu p.1052; Park p.264; Lee p.239 (3/3 agree) | CALIBRATION |
| HBM3 PHY energy | 0.25 pJ/bit write / 0.29 read at 9 Gb/s/pin, VDD 0.66 V / VDDQ 0.30 V. SCOPE: SoC-side PHY incl. digital back-end (TRX, strobe control, per-bit de-skew, read FIFO, VT tracking, DFI logic, addr/cmd interface, DFT) -- THE DRAM DIE IS EXCLUDED | Chae et al., Samsung Foundry, JSSC 59(1) 2024 Table II p.239 | VALIDATION for a controller-PHY term ONLY; never as DRAM I/O energy. Note Chae's 0.30 V VDDQ vs the DRAM papers' 0.40 V VDDQL -- 1.8x under CV^2 |
| HBM3E vs HBM3 | "bump map footprint, the number of channel and I/Os, and the operation voltage, are identical to the latest HBM3" -- corroborated by the paper's own table (ballmap 7.08 x 8.82 mm, bump pitch 96 x 110 um, chip 11 x 11 mm, rails all identical). EXCEPTIONS: density (24 Gb dies, 16-high -> 48 GB) and row address RA<0:13> -> RA<0:14> | Lee et al., SK hynix, ISSCC 2024 13.4 p.238-239 | Justifies modelling HBM3E as HBM3 at a raised pin rate -- for the INTERFACE, not for capacity or row count |
| HBM termination | "ODT not allowed in HBM (static power)"; interface "CMOS, un-terminated" | ISCA 2025 tutorial slide 46 (Song, Samsung); Chun JSSC 2021 Table I p.200 | Third independent corroboration of HBM termination = 0 (with JESD238B cl.9.1) |
| HBM2 system power split | SoC PHY 33.3% / DRAM core 37.4% / DRAM interface + channel IO 29.3%, at 2 Gb/s streaming reads, 1024 DQ | ISCA 2025 Tutorial slide 15, sourced "Rambus Inc." (Woo/Elsasser, Rambus) | VALIDATION band for the ONE-FABRIC three-way projection. Condition: read-streaming best case, not mixed traffic |
| Cross-generation tCCD in ns | DRAM core frequency pinned <= 200 MHz since DDR2; tCCD_L pinned at 5 ns since DDR2-800; prefetch absorbs all data-rate scaling | ISCA 2025 Tutorial slide 130 | Structural cross-check; JEDEC gives tCCD_L in tCK per bin, this gives the ns invariant |
| tCCD_L family split, with cause | ~5 ns where the IO sense amp sits at the bank end (DDR/LPDDR) vs ~2.5 ns mid-bank (GDDR/HBM) | ISCA 2025 Tutorial slide 56 | Physical justification for our per-family tCCD_L values -- not derivable from the standards |
| DDR5 latency-under-load validation target | DDR5-6400, 8 BG x 4 banks, 1/2 ranks, 32-entry R/W queues, 67/33 R/W, closed page, random: 54 ns unloaded; saturation ~17.5 GB/s (32 banks) and ~23.3 GB/s (64 banks) of 51.2 GB/s peak | ISCA 2025 Tutorial slides 93-95 (DRAMSys) | VALIDATION -- a fully specified, reproducible performance target; standards contain no performance data |

NOT CITABLE (held for background only): `misc/DRAM Lecture Tomishima.pdf`
carries "Intel Confidential - Internal Use Only" on 85 of its 90 pages. No
number from it may appear in a config, a figure, or the manuscript with
that deck as its provenance.

## Round-5 calibration corrections (1.11.66, 2026-09-18)

| Item | Value now | Source | Note |
|---|---|---|---|
| GDDR6 clock relation | 8 bits/pin per CK: rate 14000 MT/s, tCK = 8E6/rate = 571 ps, nBL 2, nCCDS 2 | Samsung K4Z80325BC Table 91 (tCK 0.57 ns at 14 Gbps); JESD250D Table 1 (CK 1.5 GHz <-> 12 Gbps). Proof: every preset cycle count reproduces Samsung's 14 Gbps AC set at 0.57 ns within 1-2% and nothing at 1.00 ns | CORRECTS a 1.11.63 regression (2 bits/pin assumed) that 1.11.65 propagated into tWTR. All seven presets' rate column now equals the priced rate; the static rate table is demoted to a cross-check |
| GDDR6 tRFCab | 120 ns (all densities) | Samsung Table 92 PDF p.158; SK hynix H56G42A Table 67 PDF p.157 | Was DDR4's 360/220 (impl/energy) |
| GDDR6 ns getters | derived from the preset (14.85/13.7/14.85/30.3 ns) | preset x 571 ps | The 14.8/28.0 literals were the vendor values; the derivation now reproduces them |
| DDR3 org transcription | 65536 rows x 2048 cols (x8) | DDR3.cpp 1.11.63 rows | Transcription had gone stale (131072 x 1024); density product hid it |
| DDR5 banks, 8 Gb x8 | 8 BG x 2 = 16 | JESD79-5D Table 4 printed p.7 (BA0); 32 begins at 16 Gb (Tables 5-7) | Arch object + main.cpp table were at 32; preset was right |
| DDR5 speed grades | 3200AN (8 Gb) / 4800B / 5600B (16 Gb); default 4800 | JESD79-5D Tables 283/287/289 (bins), 335/336 (per-speed AC), 71 (16 Gb tRFC1 295), 73, cl.13.2 rounding | The Micron dies are B-bin parts (-48B, -56B) |
| DDR5 IDD, 4800B / 16 Gb | IDD0 103, IDD2N 92, IDD3N 142, IDD4R 377, IDD4W 349, IDD5B 277, IDD2P 88 mA | Micron MT60B Rev A addendum Table 6 pp.17-19 | Default grade |
| DDR5 IDD, 5600B / 16 Gb | 53 / 49 / 91 / 218 / 241 / 377 / 47 mA | Micron MT60B Rev D addendum Table 8 pp.18-20 | |
| DDR5 IDD, 3200 / 8 Gb | 55 / 34 / 42 / 148 / 168 / 120 / 20 mA | UNSOURCED (no held datasheet publishes a 3200 column) | Stated; 2-3x below either addendum on standby |
| DDR4 IDD5B | 457.4 mA (1.11.91) | Derived from Micron MT40A Rev B Table 148 p.332 IDD5R 53 / IDD2N 34 at tRFC/tREFI 350/7800 | 1.11.66 had 362 from a Rev A IDD5R against another revision's standby (R8-9) |
| DDR4 IDD row, x8 2400 (1.11.91) | IDD0 48, IDD2N 34, IDD3N 43, IDD4R 135, IDD4W 123, IDD2P 25 mA; IPP2N = IPP3N 3 mA at VPP 2.5 V | Micron MT40A (8gb_ddr4_dram.pdf Rev. N) Table 148 Die Rev. B p.332; IPP3N applies to all IDD2x/3x/4x per note 22 p.334; VPP p.1 | User ruling R8-9. No IDDQ rows published (n/a) |
| DDR3 IDD row, x8 1600 (1.11.91) | IDD0 55, IDD2N 32, IDD3N 38, IDD4R 157, IDD4W 125, IDD5B 235, IDD2P1 (fast exit) 32 mA | Micron MT41K (4Gb_DDR3L.pdf Rev. Q) Table 20 Die Rev. E p.43 | User ruling R8-9. No VPP rail, no IDDQ rows |
| IDD3N basis (1.11.91) | ALL_BANKS DDR3/4/5; ONE_BANK HBM2/HBM3/GDDR6; UNVERIFIED LPDDR5 | DDR5 core sheet Table 387 p.450; MT40A Table 136 p.317; MT41K Table 13 note 3 p.36; JESD235D PDF p.109; JESD238B.01 Table 83 PDF p.165; JESD250D Table 65 PDF p.165 | User ruling R8-2 (a); LPDDR5 condition table (JESD209-5C) not readable here |
| DDR5 IDDQ / IPP, 4800B (1.11.91) | IDDQ3N 31, IDDQ4R 57, IDDQ4W 198 mA at VDDQ 1.1 V; IPP2N 6, IPP3N 7 mA at VPP 1.8 V | MT60B Rev A Table 6 pp.17-19; VDDQ/VPP core sheet p.1 | User ruling R8-8 (a). Core sheet p.448: IDDQ "cannot be directly used to calculate IO power" (stated) |
| DDR5 IDDQ / IPP, 5600B (1.11.91) | IDDQ3N 70, IDDQ4R 218, IDDQ4W 271; IPP2N 7, IPP3N 8 mA | MT60B Rev D Table 8 pp.18-20 | |
| LPDDR5 IDDQ / VDD1 (1.11.91) | IDD3NQ 0.6, IDD4RQ 111.9 (typical, note 4), IDD4WQ 0.6 mA at VDDQ 0.5 V; IDD2N1 1.5, IDD3N1 2.8 mA at VDD1 1.8 V (priced in the IPP slot) | Micron Y52P LPDDR5X (public) Table 18 PDF pp.42-43, x16, 7500 Mb/s (item 11 re-source) | No VPP ball in LPDDR5 (p.1 rails) |
| HBM2/HBM3 IDDQ (1.11.91) | BAND 0.19-0.27 pJ/bit on reads (midpoint charged) | Cho ISSCC 2018 Fig.12.3.1 driver figure (below) | JESD238B.01 PDF p.164: HBM IDDQ vendor-simulated, not published |
| DDR4 tRFC1, 8 Gb | 350 ns | Micron MT40A p.369 | Was 360 |
| HBM2 IDD row | IDD0 141, IDD2N 136, IDD3N 133, IDD4R 464, IDD4W 356, IDD5B 189 mA per channel | CMU-SAFARI HBM-Power 36-chip means / 8 channels (misc/zen_hbm2_idd_36chips_summary.csv); conversion per JESD235D cl. 9.1 printed p.100 | User ruling R8 #10; measurement-class. IDD2P 7 stays unmeasured. Model stack standby 1.31 W vs measured 1.37 W |
| HBM3 arch core clock | 1600 MHz (stamped from tCK 625) | JESD238B.01 Table 92; Ryu JSSC'23 p.1052; Park JSSC'23 p.259 | Was 3200 (rate/2) |
| HBM2/HBM3 arch ns timings | stamped from preset (HBM2 16.66/16.66/15.0/33.3; HBM3 16.25 x3 / 33.125) | preset x tCK | The R6 stamp now covers all five object-bearing techs |
| dramRowBytes | preset cols x dq / 8 (DDR3 2048, DDR4/5 1024, LPDDR5 2048, GDDR6 2048, HBM2/3 1024 B) | preset org rows; HBM: JESD235D/238B "Page Size per PC 1 KB" | Was a per-generation guess wrong on three of seven |
| LPDDR5 tREFI (energy row) | 3906 ns | JESD209-5C 3.906 us; = LPDDR5.cpp tREFI_BASE | Was 3904 |
| Organization shape check | transcription banks/rows/cols bound field-by-field to the instantiated IDRAM at every wrapper init | -- | Structural; PIMID_ORG_BREAK proves it fires |
