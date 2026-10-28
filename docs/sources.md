# Sources -- every external number's provenance

One row per externally sourced quantity: what it is, the value or band in
use, the source, and where the code consumes it. Local copies of sources
live in the (unpublished) local document archive of the development tree;
entries below name the document. Rule of the house: a quantity
with units of time/energy/rate/fraction/area is either a spec primitive, a
runtime measurement, or it appears here with a source and (where the
sources disagree) a band.

## DRAM standards (JEDEC)

| Quantity | Value in use | Source | Consumed at |
|---|---|---|---|
| DDR3 timing preset (tRAS/tRP/tBurst overrides) | 35 / 13.75 / 5.0 ns (DDR3-1600K) | JESD79-3D | `ramulator_wrapper.cpp` per-tech overrides |
| DDR3 termination scheme (R7 rd/wr split) | SSTL-135; read loop RON 34 + RTT_NOM 40, write loop 34 + RTT_WR 120 | JESD79-3D Tables 38, 41 (topology); Micron MT41K p.32 IDD conditions ("RON RZQ/7 (34); RTT,nom RZQ/6 (40); RTT(WR) RZQ/2 (120)") | `pimid_energy.h` termination rows; CACTI-IO injection (rtt1/rtt2 per direction) |
| DDR4 electricals (R7 rd/wr split) | POD12; read 34+40, write 34+120 | JESD8-24 (topology); Micron MT40A p.315 IDD conditions | CACTI-IO injection (`cacti_io_wrapper.cpp`) |
| DDR5 electricals (R7 rd/wr split) | POD at 1.1 V; read 34+40, write 34+120. NOTE: no "POD11" JESD8-* standard exists (family is POD18/15/135/125/12/10, all held locally); the 1.1 V point is JESD79-5's own | Micron DDR5 core sheet p.453 IDD conditions (MR5 RZQ/7 drivers, MR35 RTT_NOM RZQ/6, MR34 RTT_WR RZQ/2) | CACTI-IO injection |
| GDDR6 electricals (R7 rd/wr split) | POD135; read 40+60, write 40+120; MR1 default = termination DISABLED, IDD operating point priced | Samsung K4Z80325BC p.166 (driver 40 / termination 60 characteristics), p.144 IDD conditions ("All ODTs are enabled with ZQ/2", ZQ=240), p.49 MR1; JESD250D (topology) | CACTI-IO injection |
| Controller-side write driver RON (DDR4/DDR5) | applied 34 ohm, inside the sourced host range 30-50 ohm | Intel 743844-015 Table 87 p.208 (DDR5: RON_UP/DN(DQ) 30-50 ohm, RODT(DQ) 30-240) and Table 86 p.207 (DDR4: 30-50, RODT 40-200); range stated at the row, point chosen within it | `pimid_energy.h` (ron_wr) |
| Controller-side write driver RON (GDDR6) | applied 40 ohm, a member of the sourced host set {40/48/60} | Achronix Speedster7t GDDR6 User Guide UG091, Table 3 p.16: "DQ driver impedance (RON) 40/48/60 ohm" (host PHY; CA RON same set, CA term 60/120/240) -- closes the R7 split's last stated assumption (2026-09-04) | `pimid_energy.h` (ron_wr) |
| LPDDR5 termination topology | LVSTL, VSS-referenced; DQ ODT default = Disable | Micron public LPDDR5X datasheets (Y52P, Y52Q, Y4BM); JESD209-5C Table 84 p.144 | `pimid_energy.h` (rtt=0 encodes the JEDEC default; sourcing residue N8 CLOSED 1.11.63) |
| LPDDR5 VDDQ | 0.50 V TYP (0.30 V ODT-off; range 0.47-0.57 V) | Micron LPDDR5X y52p p.1 Features and Table 18 note 5 p.43 (a vendor source that cannot be disclosed publicly, formerly cited here, is no longer cited, user ruling 2026-09-26) | CACTI-IO injection (`cacti_io_wrapper.cpp`), `pimid_energy.h` |
| LPDDR5 driver RON | 40 ohm | Micron IDD-table Note 4 ("Output load = 5pF; RON = 40 ohms; TC = 25 C"): y52p p.43, y52q p.33, y4bm p.25 | CACTI-IO injection, `pimid_energy.h` |
| DQ termination duty (POD driven-LOW / LVSTL driven-HIGH fraction of data-pin time) | MEASURED per run on the lines that cross each interface (64 B read at the DRAM line read / writeback, popcount per byte); replaces the 0.5 in the CACTI-IO DDR4-type data-pin term and in the scheme table; DQS strobes stay at 0.5; SSTL is data-independent | The run itself (user ruling 2026-10-25); counters defined in `external/zsim/src/dq_data_stats.h` | `pe_memory_interface.h`, `mem_ctrls.cpp`, `ramulator_mem_ctrl.cpp` (counters), `ramulator_wrapper.cpp` measuredDataDuty, `extio.cc` dq_data_low_duty |
| Host memory-controller PHY idle (DDR4/DDR5) | DERIVED: (a + b x rate) per 16 DQ, the least-squares line through measured LPDDR4X PHY idle rails (vdd2_ddr + vddq_ddr, system idle, controller clock-gated) 22.9 / 14.1 / 7.8 mW at 3733 / 1866 / 625 MT/s (vdd2_ddr 18.9 / 9.6 / 3.8 + vddq_ddr 4.0 / 4.5 / 4.0; each table's vddq_ddr row is on its "...continued" part), scaled to the channel width; + the CACTI-IO PLL static (the measured rails exclude the PLL). No measured DDR4/DDR5 host-PHY idle is published | NXP AN13917 Rev. 4.0 (i.MX 93 Power Consumption Measurement), Table 6 p.12, Tables 28 / 29 / 31 pp.38-41 | `cacti_io_wrapper.cpp` computeDramIO (phy_static_power_mw) |
| Host memory-controller PHY idle (HBM3) | DERIVED: 56.6 mW per stack (1024 DQ) at 6400 MT/s = hard macro 36 mW x (rate / 3200) x (0.66 / 0.75)^2 + I/O 14 mW x (0.30 / 1.2)^2, charged per channel (3.54 mW per 64-DQ channel x 16 per stack). Process (7 nm) not applied; no separate PLL (inside the measured hard macro). No HBM3 PHY idle is published | Measured HBM2E PHY idle: S. Hwang, K. Chae, T. Yeo et al. (Samsung Foundry), "A 3.2 Gbps/pin HBM2E PHY with Low Power I/O and Enhanced Training Scheme for 2.5D System-in-Package Solutions", Hot Chips 32 (2020) poster, slide 10 (7 nm, 0.75 / 1.2 V, 8 channels, IDLE hard macro 36 + I/O 14 mW); HBM3 PHY supplies VDD 0.66 / VDDQ 0.30 V: Chae et al., JSSC 59(1) 2024, Table II | `cacti_io_wrapper.cpp` computeDramIO (phy_static_power_mw, phy_static_sourced); `ramulator_wrapper.cpp` getInterfacePhyStaticMW (x channels) |
| Host memory-controller PHY idle (HBM2) | DERIVED: 41 mW per stack (1024 DQ) at 2400 MT/s = hard macro 36 mW x (rate / 3200) + I/O 14 mW (HBM2 VDDQ 1.2 V = the measured I/O supply; no HBM2 host-PHY core voltage is published, 0.75 V kept), charged per channel (5.125 mW per 128-DQ channel x 8 per stack) | Hwang, Chae, Yeo et al. (Samsung Foundry), Hot Chips 32 (2020) poster, slide 10 (as the HBM3 row); HBM2 VDDQ 1.2 V: JESD235 | `cacti_io_wrapper.cpp` computeDramIO; `ramulator_wrapper.cpp` getInterfacePhyStaticMW |
| Host memory-controller PHY energy per bit (HBM2 / HBM3 host memory) | HBM3 0.29 pJ/bit read / 0.25 write (PUBLISHED, used per bit at the run's rate); HBM2 0.56 pJ/bit read / 1.07 write (DERIVED: rate factor 1, a per-bit energy; voltage factor 1, HBM2 VDDQ 1.2 V = the measured I/O supply, core 0.75 V kept; each direction at the slide's published figure, user ruling 2026-10-28). Replaces McPAT's own PHY dynamic; the controller is emitted at McPAT's interposer tier (phy_class 1) | HBM3: Chae et al., JSSC 59(1) 2024, Table II; HBM2: Hwang, Chae, Yeo et al., Hot Chips 32 (2020) poster, slide 10 (energy row: 0.56 read / 1.07 write, hard macro + I/O) | `cacti_io_wrapper.cpp` hbmHostPhyEnergyPJBit; `main.cpp` applyHbmHostPhyEnergy; McPAT `memoryctrl.cc` MCPHY (phy_pj_bit_rd / _wr) |
| RDIMM command/address path into the register (RCD) | DDR5: 10 RCD inputs per sub-channel (7 DCA, parity, 2 CS), POD 1.1 V, loop host RON_CMD 40 (DERIVED midpoint of 30-50) + 15 pre-RCD + RCD input termination 60 (default); DDR4: 25 lines to VDD/2, 1.2 V, loop 35 (DERIVED midpoint of 25-45) + 15 (DERIVED) + 60 (DERIVED: no DDR4 RCD input termination value is published) | Renesas RRG5004x short-form datasheet R10DO0013EU0101 Rev.1.01 p.1; JESD305B.01 PDF p.32; X. Li, "Leveraging DDR5 RCDs for High-Speed DDR5 Protocol Analysis", ISQED 2025, Sec. II.B p.4; Intel 743844-015 Tables 86 / 87 pp.207-208; Micron DDR4 RDIMM Core Rev. B 12/2021 p.14 note 5 | `cacti_io_wrapper.cpp` (energy_pj_per_bit_term_ca_rdimm), `ramulator_wrapper.cpp` getTerminationEnergyNJ |
| RDIMM register (RCD) power, DDR4/DDR5 | DERIVED (no DDR4/DDR5 RCD power is published): static = the DDR3 RDIMM register PLL + output clocks 0.36 / 0.21 W at DDR3-1333 / 800, EXTRAPOLATED linearly in the data rate beyond the measured 800-1333 MT/s (DDR4-2400: 1.8x, DDR5-4800: 3.6x the highest measured rate), x (VDD_RCD / 1.5 V)^2 with DDR4 RCD 1.2 V, DDR5 RCD 1.1 V; output drive per access: DDR4 the DIMM command/address network to VTT (the CACTI-IO parameter set's CA network), DDR5 15 outputs x 1.1 V x 16.4 mA LOW half the time over the 64 B sub-channel burst | D. David, C. Fallin, E. Gorbatov, U. Hanebutte, O. Mutlu, "Memory Power Management via Dynamic Voltage/Frequency Scaling", ICAC 2011, Table 3; Renesas 4RCD0124KC0 short-form datasheet R10DO0025EU0100 p.1; Montage M88DR5RCD01 / RCD03 product briefs PB0013 / PB0015; US 10,401,899 B2 (Table 1 text) | `ramulator_wrapper.cpp` getRcdStaticMW / getRcdDriveEnergyNJ; ledger `<host>.memory_rcd` |
| DQ/DQS series stub per memory topology | DDR3 DIMM 15 ohm; DDR4 UDIMM/SODIMM 15 ohm +/-5% (DDR4 RDIMM: the same DIMM value, no RDIMM-specific public figure); DDR5 RDIMM 15 ohm; DDR5 UDIMM/SODIMM none; soldered point-to-point none | Altera AN-520 (May 2009) p.10; Samsung DDR4 UDIMM M378A / SODIMM M471A data sheets Rev 1.1 Jun 2018 pp.10-12, note 1; JESD305B.01 (DDR5 RDIMM) PDF p.32 "DQ / Strobe ... 15 ohm ... Series Resistor"; UniIC DDR5 UDIMM DQ maps (2022, 2025) p.14 and SK hynix DDR5 UDIMM (2021) p.12 / SODIMM (2022) p.13; AMD UG863 v1.12 DDR5 signals ("None, uses ODT to 40 ohm"); Micron TN-40-40 Rev. H p.31 Table 12 note 1 | `cacti_io_wrapper.cpp` resolveDramTopology (CACTI-IO rs1_dq / rs2_dq); `memory.topology` |
| Data bus inversion per standard | DDR4 x8/x16 read + write: a byte with more than four 0 bits is inverted, DBI_n LOW; DDR5: none in either direction; LPDDR5 read + write DBI-DC: a byte with more than four 1 bits is inverted, DMI HIGH (MR3 OP[6]/OP[7], JEDEC default Disabled); GDDR6 read + write DBIdc: the POD rule (MR1 OP8 RDBI / OP9 WDBI) | Micron MT40A 8Gb DDR4 data sheet "Data Bus Inversion" PDF p.179, "DBI During a READ Operation" p.180; JESD79-5D v1.41 pin table PDF p.41 (DM_n/TDQS_t) and 4.8.1 p.181; JESD209-5C MR3 PDF p.177 and 7.4.10 pp.311-314; JESD250D MR1 PDF p.60, WRITE p.95, READ p.111, "Data Bus Inversion (DBIdc)" pp.120-121 | `ramulator_wrapper.cpp` dbiApplies; `power.models.dram_dbi` |
| LPDDR5 DQ ODT (Rtt) | Disable (Default); ladder RZQ/1..RZQ/6 = 240/120/80/60/48/40 ohm (RZQ=240), 111B RFU; NT-ODT also defaults off | JESD209-5C Table 84 p.144 (acquired 2026-08-24) -- supersedes the 1.11.52-1.11.59 assumption chain (240 ohm assumed from host-side Intel 743844-015 Tbl 89 RODT range) | `pimid_energy.h` (rtt=0, LVSTL branch returns 0 for it), `cacti_io_wrapper.cpp` (row sourced=true); ODT-on systems use power.termination_pj_per_bit |
| HBM3 I/O rails | VDDQ 1.1 V TYP, VDDQL (TX driver output stage) 0.4 V TYP; unterminated (IOUT = 0 mA, Ctotal = 2.5 pF); driver strength specified in CURRENT (8/10/12/14 mA, MR6) not ohms | JESD238B.01 Table 70 p.152 (cl.7.2), cl.9.1 p.164/166, Table 17 p.34 | not injected -- the (vddq, rtt, ron) injection shape has no rtt/ron to give for HBM |
| Power-down entry/exit threshold (E17) | DDR3/DDR4 6.0 ns; DDR5/LPDDR5 7.5 ns | JESD79-3D (tXP max(3nCK,6ns)); JESD79-4; JESD79-5; JESD209-5 | `gapPowerDownResidency()` in `main.cpp`; settable via `memory.power_down_threshold_ns` |
| HBM3 refresh completeness (nRFCSB fill) | tRFC upper bound, stated; JESD238B locally available for the real per-density values (upgrade queued) | JESD238B | `external/ramulator/.../HBM3.cpp`, `HBM2.cpp` |
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
| HBM3 data-dependent burst energy ((1.12.81)) | pJ/bit = 2.759 + 1.192 dq + 1.331 tsv + 0.72 bg, relative, tsv = bg = 0.5 | Olgun et al., MICRO 2026 Sec. 6.2; github.com/CMU-SAFARI/HBM-Power case_studies/hbm3e_validation/configs/HBM3_6400_power_datapattern.json (the authors' validated HBM3E transfer) | params/dram/hbm3.yaml data_pattern |
| DDR4 data-dependent burst energy ((1.12.82), transfer) | read k +0.1733, write k -0.0835 per (1s fraction - 0.5), as DDR3 | Ghose et al., SIGMETRICS 2018 Table 5 and Sec. 3.2 (trends "should apply to DDR4"); DDR4 measurement: NO PUBLISHED SOURCE FOUND | params/dram/ddr4.yaml data_pattern |
| DDR5 data-dependent burst energy ((1.12.83), transfer) | read k +0.1733, write k -0.0835 per (1s fraction - 0.5), as DDR3 | Ghose et al., SIGMETRICS 2018 Table 5 (DDR3); planar-core assumption, no author statement for DDR5; DDR5 measurement: NO PUBLISHED SOURCE FOUND | params/dram/ddr5.yaml data_pattern |
| LPDDR5 data-dependent burst energy ((1.12.84), transfer) | read k +0.1733, write k -0.0835 per (1s fraction - 0.5), as DDR3 | Ghose et al., SIGMETRICS 2018 Table 5 (DDR3); planar-core assumption, no author statement for LPDDR5; LPDDR5 measurement: NO PUBLISHED SOURCE FOUND | params/dram/lpddr5.yaml data_pattern |
| GDDR6 data-dependent burst energy ((1.12.85), transfer) | read k +0.1733, write k -0.0835 per (1s fraction - 0.5), as DDR3 | Ghose et al., SIGMETRICS 2018 Table 5 (DDR3); planar-core assumption, no author statement for GDDR6; GDDR6 measurement: NO PUBLISHED SOURCE FOUND | params/dram/gddr6.yaml data_pattern |
| DDR3 / HBM2 data-dependent burst energy ((1.12.61), V6) | DDR3 read k +0.1733, write k -0.0835 per (1s fraction - 0.5); HBM2 pJ/bit = 3.1798 + 1.04 dq + 1.4875 tsv + 1.41 bg, relative, tsv = bg = 0.5 | Ghose et al., SIGMETRICS 2018 Table 5, Fig. 15/16; Olgun et al., MICRO 2026 Sec. 5.3 and github.com/CMU-SAFARI/HBM-Power docs/explanation/data-pattern-model.md | params/dram/ddr3.yaml, hbm2.yaml data_pattern |
| DDR3 x16 IDD column ((1.12.48), user ruling 2026-10-25) | IDD0 66, IDD3N 47, IDD4R 235, IDD4W 171 mA (x16, DDR3-1600, MAXIMA); IDD2N / IDD2P1 / IDD5B "All" | Micron MT41K 4 Gb DDR3L (4Gb_DDR3L.pdf Rev. Q 12/17) Table 20 Die Rev E p.43 | params/dram/ddr3.yaml idd.by_width.x16 |
| DDR3L measured/datasheet ratio per current | three-vendor means IDD0 0.427, IDD2N 0.566, IDD3N 0.367, IDD4R 0.736 (I/O-corrected), IDD4W 0.542, IDD5B 0.829 (band: vendor A/B/C, e.g. IDD3N 0.234/0.532/0.334); fast power-down IDD2P1 cuts the measured IDD2N by 65.8/30.6/48.7% (A/B/C; ratio 0.342/0.694/0.513, mean 0.516). DDR3 component set: standby 0.566, active-standby premium 0.367/0.566, activate excess 0.43 (IDD0 loop), burst excess 0.736 rd / 0.542 wr, refresh excess 0.829, power-down 0.29224 = 0.566 x 0.51633 ((1.12.47), user ruling 2026-10-25; was 0.566) | Ghose, Yaglikci, Gupta, Lee, Chandrasekar, Ma, Mutlu, "What Your DRAM Power Models Are Not Telling You: Lessons from a Detailed Experimental Study", Proc. ACM Meas. Anal. Comput. Syst. (SIGMETRICS) 2018, https://arxiv.org/abs/1807.05102 (50 DDR3L-1600 modules, 3 vendors) | `pimid_energy.h` `componentFactorsFor("DDR3")` (CALIBRATED); the two-generation set |
| DDR4 measured/datasheet ratio per current | IDD0 0.540 (1120->605 mA), IDD2N 0.576 (1040->599), IDD3N 0.350 (1540->539), IDD4R 0.553 (1750->967), IDD4W 0.430 (1590->684); refresh not calibrated; energy-level ratios 0.25-0.51 (median 0.47). DDR4 component set: standby 0.576, premium 0.350/0.576, activate excess 0.540, burst excess 0.553 rd / 0.430 wr, refresh excess 0.83 (Ghose), power-down 0.576 | Shi et al., "Calibrating DRAMPower Model for HPC: A Runtime Perspective from Real-Time Measurements", arXiv:2411.17960 (v3), Fig. 4(b) (Samsung M393A1G43DB0-CPB DDR4-2133 8 GB RDIMMs, HDEEM, Haswell node) | `pimid_energy.h` `componentFactorsFor("DDR4")` (CALIBRATED; ratios only, not the per-DIMM values); the two-generation set |
| Two-generation component factors | precharged standby 0.57; active-standby premium 0.36/0.57 (IDD3N_t = IDD2N_t + premium x (IDD3N - IDD2N), >= IDD2N_t); activate excess (IDD0 loop minus its TN-41-01 baseline) 0.5; burst excess 0.6 rd / 0.5 wr; refresh excess (IDD5B - IDD2N) 0.83 (Ghose only); power-down 0.57 | the two rows above (user ruling 19:30); assumption: newer generations are guardbanded as DDR3L and DDR4 were. [The first, per-current pass (IDD0 0.48 ... IDD2P 1.00) was withdrawn: DDR5 IDD0 fell below IDD2N] | `pimid_energy.h` `componentFactorsFor()`: DDR5 (3200/4800/5600), LPDDR5, LPDDR5X, GDDR6 (DERIVED); none for HBM2/HBM3 |
| GDDR6 IDD maxima | per device, x32, 1.35 V, HC14: IDD0 430, IDD2N 310, IDD3N 460, IDD4R 1220, IDD4W 1470, IDD5 790, IDD2P 230, IDD7 1640 mA; tRFCab 120 ns (Table 91); row = device/2 per channel (maxima), typical by the two-generation component set; activate 3.24 nJ/channel ACT from IDD7 - IDD4R, x activate-excess 0.5 = 1620 pJ | Samsung K4Z80325BC 8Gb GDDR6 SGRAM C-die Rev. 1.3 Table 82 PDF p.146, Tables 90-91; derivation [derivations/hbm_gddr6_idd.md](derivations/hbm_gddr6_idd.md) section 2 | `pimid_energy.h` GDDR6 row (DERIVED) |
| HBM2 measured IDD (base of HBM2 and HBM3) | 36-stack means / 8 per channel: IDD0 140.6, IDD2N 135.9, IDD3N1 133.4, IDD4R 463.8, IDD4W 355.6, IDD5B 189.3 mA at 1.2 Gb/s/pin on PC0 only, IDD5 loop tRFC 266.7 ns | CMU-SAFARI HBM-Power artifact, branch "artifact", https://github.com/CMU-SAFARI/HBM-Power/tree/artifact, accessed 2026-09-26 (paper not yet public): data/all_idd_measurements.csv, sources/table3/configs/HBM2_1.2Gbps_timing_BL4.json, power_measurement.cpp; its "ipp" column is a card-power residual (platform.cpp), NOT used | `pimid_energy.h` HBM2 row (MEASURED, burst at constant energy per bit x 307.2/76.8) and HBM3 row (DERIVED) |
| HBM2 per-bit energy cross-check | 3.92 pJ/bit per access; row activation 909 pJ; I/O 0.80 of 3.48 pJ/b | O'Connor et al., "Fine-Grained DRAM: Energy-Efficient DRAM for Extreme Bandwidth Systems", MICRO-50 2017 p.3, Table 3 p.10 | HBM3 row derivation (I/O share f_io 0.23) and check |
| LPDDR5 / LPDDR5X IDD maxima (VDD2H) | IDD0 53, IDD2N 32.5, IDD3N 40, IDD4R 390, IDD4W 265, IDD52H 205, IDD2P 3.9 mA, x16, 7500 Mb/s, 16 Gb die; LPDDR5 row: burst excess x 6400/7500 (stored), typical by the two-generation component set; LPDDR5X row: unscaled, same set (technology not selectable: no public AC timing set) | Micron Y52P LPDDR5X SDRAM (Rev. H 03/2025, public) Table 18 PDF pp.42-43; tRFCab 280 ns Table 6 p.11. Checked for an AC timing table and found none: Micron automotive LPDDR5X 561b Y52Q DDP (MT62F512M64D2, Rev. G 05/2025, https://www.mouser.com/catalog/specsheets/Micron_10-16-2025_561b-y52q-ddp-auto-lpddr5x.pdf, 33 pp.) -- only density-dependent refresh (Table 4) and 16-bank 3733/4267 Mb/s RL/WL/nWR (Table 8); the rest deferred to General LPDDR5/LPDDR5X Specifications 2/3 | `pimid_energy.h` LPDDR5 (DERIVED) and LPDDR5X rows |
| HBM per-stack floor | NONE since (1.12.46) (user ruling 2026-10-25): the 821.3 mA is the board current with the HBM2 stack powered off, subtracted by the measurement; per-channel currents = (stack mean - 821.3) / 8 (was a 783.3 mA floor, 939.96 -> 469.98 mW HBM2 / 430.82 mW HBM3 per stack) | Olgun et al., MICRO 2026 (arXiv:2610.14244) Sec. 4.3; github.com/CMU-SAFARI/HBM-Power config/HBM2_1200MTs/power.json, docs/explanation/idd-derivation.md |
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
| HBM2 density | 8Gb user / 96mm^2 (12x8mm, 20nm) | Sohn et al., ISSCC 2016 18.2 / JSSC 52(1) | same |
| DDR3 density | 4Gb/30.9mm^2 (SK Hynix 23nm) | ISSCC 2012 paper 2.3 press kit | same; 1.12.25: now the same density as the simulated die (DDR3_4Gb_x8), so the die area is this die's 30.9 mm^2. The current source is Micron's 4 Gb die Rev E: same density, other vendor -- NO PUBLISHED SOURCE FOUND for the Micron Rev E die area |
| DDR3 default part (1.12.19, user rulings 2026-10-24) | 4 Gb x8 DDR3-1600 at VDD = VDDQ = 1.5 V, SSTL-15; DDR3L 1.35 V selectable (`memory.dram.ddr3_variant`) | Micron MT41K 4Gb_DDR3L Rev. R Table 20 p.44 "Die Rev. E for 1.35/1.5V Operation" = MT41J 4 Gb automotive DDR3 Rev. B Table 20 p.44 (identical x8 1600 column); JESD79-3E Tables 22/38/41, Figure 10 | Org DDR3_4Gb_x8 (was DDR3_8Gb_x8 against 4 Gb currents) |
| DDR3 tRFC (1.12.25) | 260 ns default (`trfc_basis: datasheet`), 300 ns (`jedec`) | MT41K Rev. R Table 58 p.86 "tRFC - 4Gb MIN = 260"; JESD79-3E Table 61 p.158 | Timing (Ramulator tRFC override) and refresh energy; was 350 ns (the 8 Gb value) |
| Array fraction of die | BLSA 8-15%, LWD 5-10% (structure statement) | Vogelsang, MICRO 2010 | on-pitch boundary rationale (L116/N1) |

## DRAM-process compute (the family + pitch model)

| Quantity | Value in use | Source | Consumed at |
|---|---|---|---|
| Family factors (fa/fd/fl) | derived per run from CACTI .dat hp vs comm-dram columns at the configured node/corner/temperature | CACTI 7 technology tables; the DRAM columns themselves are CACTI-D (Thoziyoor et al., ISCA 2008) | `periphFamilyFactors()` |
| SUBARRAY pitch factor | band [1.25, ~4]; default 1.25 | low end: Samsung FIMDRAM, ISSCC 2021 25.4 (~1.5mm^2/PCU, HBM2/1y, lean SIMD); upper: UPMEM ~10x density claim / family factor (Hot Chips 31; process facts in Gomez-Luna et al., arXiv:2105.03814) | `main.cpp` pitch derivation (N3) |
| DRAM generation feature size F | 1x 19 / 1y 17.5 / 1z 15.5 / 1a 14 / 1b 12.5 nm; cell = 6F^2 buried-wordline | industry generation naming (TechInsights usage); 6F^2 architectural fact | `dramGenFeatureNm()` (reporting) |
| On-die PE process | pinned to generation table (N2 ruling) | measured non-cancellation (this project, gate 1161I5) | config pin + family sites |
| DPU metal-layer constraint | DRAM process ~3 metal layers vs >10 logic | Gomez-Luna et al. quoting UPMEM | pitch band rationale |

## Links / interconnect

| Quantity | Value in use | Source | Consumed at |
|---|---|---|---|
| PCIe gen3/4/5 pJ/bit | gen3 4.0 - 6.8; gen4 6.0 - 7.35 (1.93 TX-only, a bound below); gen5 7.6 - 11.4 | Analog Bits figures (T. Simon, SemiWiki, 2021-12-02: Gen3 4 pJ/bit, ~6.8 mW/Gbps at 28 nm; Gen4 6 pJ/bit, 117.7 mW/lane at 16 Gb/s; Gen5 8LPP 7.6 pJ/bit); Bichan et al. (Intel), CICC 2020, 11.4 pJ/bit incl. PLL; Bae et al., MDPI Electronics 10(1):68, 2021 (TX only) | `linkEnergyBandPJPerBit()` |
| NVLink pJ/bit | C2C 1.3; NVLink 3/4 1.17 - 1.30 carried (NO PUBLISHED SOURCE FOUND for the board-level PHY; a lower bound) | NVIDIA Hot Chips 34 blog, 2022-08-19 (C2C 1.3 pJ/bit); Poulton et al., JSSC 54(1), 2019 (GRS 1.17 pJ/b) | same |
| SerDes ASPM L1 (priced between transfers) | 25 mW per lane per end (20-30 mW; 10-25 mW in 2012); L1.2 ~40 uW per x4 PHY (not used) | Synopsys technical bulletins (R. Solomon 2015; S. Knowlton 2012); carried to Gen4/5, CXL, NVLink, UALink (DERIVED) | McPAT `iocontrollers.cc` |
| SerDes L0 (entry timer, L1 exits) | lanes x line rate x the class's pJ/bit, each end | DERIVED (a link in L0 transmits at line rate) | same |
| ASPM L1 exit / entry timer | 5 us / 8 us | Intel IDF 2010 PCIS002 ("L1: <5us"); Microchip AN2949 (4-8 us); DesignWare L1 entrance latency 8000 ns, Tegra194 (linux-pci, 2026-05) | `kLinkL1ExitNs` / `kLinkL1EntryNs` (main.cpp), plugin `linkL1ExitCharge` |
| UCIe idle (clock-gated) | 0.05 / 0.33 of the full-rate per-bit power | Shivnaraine et al. (Microsoft), ISSCC 2026 8.3; UCIe 1.1 KPI savings >= 85% | McPAT `iocontrollers.cc` |
| UCIe pJ/bit (LEGACY band) | 0.25 - 0.5 | UCIe consortium figures | same; used only with `power.models.ucie_link: false` |
| UCIe analog PHY, 3 nm | advanced 0.29; standard 0.52 pJ/bit (measured) | Melek et al. (Cadence), IEEE JSSC vol. 61 no. 7, July 2026, DOI 10.1109/JSSC.2026.3651425 | McPAT `iocontrollers.cc` (two-node rows, 1.12.18) |
| UCIe analog PHY, 22 nm | standard 1.2 pJ/bit (measured, 48 Gb/s, PLL not integrated); advanced 0.669 DERIVED = 1.2 x 0.29/0.52 | Mondal et al. (Intel), ISSCC 2026 8.1, DOI 10.1109/ISSCC49663.2026.11409120; package ratio from Cadence above | same |
| D2D digital interface, 3 nm | 0.10 pJ/bit = 0.33 - 0.23 (UCIe-like, not the UCIe adapter) | Shivnaraine et al. (Microsoft), ISSCC 2026 8.3, DOI 10.1109/ISSCC49663.2026.11409276 | same |
| D2D digital interface, 22 nm | 0.192 pJ/bit DERIVED, lower bound = 0.10 x CACTI C_inv x Vdd^2 (22/32 nm, each at its own feature size; 0.284 before) x 0.51/0.111; guard band 0.178 ([SB17] Table 2 20 nm row) .. 0.231 (analog node ratio) | Stillmaker & Baas, Integration 58 (2017) 74-81, Table 2; CACTI tables | same |
| D2D digital 22 nm internal references (never applied) | analog node ratio 0.231; [SB17] Table 2 20 nm row 0.10 x 0.198/0.111 = 0.178 (user ruling 2026-10-24: the former TSMC-press reference was circular and is removed) | Intel/Cadence above; Stillmaker & Baas, Integration 58 (2017) 74-81, Table 2 | same (guard: WARNING outside [min, max]) |
| UALink pJ/bit | 4.5 - 6.0 (host SerDes with DSP, 112G class; no 200G figure published) | Bernadskiy et al., arXiv 2510.15893, 2025 (the former 3.5 is not in it) | same |
| Interposer I/O | 0.3 pJ/bit (app toggle) / 0.80 (50%) | O'Connor et al., MICRO-50 2017 Table 3 | MC interposer tier (D2) |
| SerDes per-lane rates | PCIe 8/16/32 GT/s; CXL 3.0 64 GT/s; NVLink 3.0 50G x 48, 4.0 100G x 36, C2C 40G x 90; UALink 200G x 4 | PCI-SIG; NVIDIA (A100 blog 2020, H100, Hot Chips 34); UALink 1.0 | `CactiIOWrapper` specFor (one table for timing and energy) |
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
  and a 3D-stacked DRAM sample config). Since 1.12.18 its TSV model
  (`TSV(Coarse)`) is invoked inside the McPAT fork (`external/mcpat/noc.cc`)
  for the die crossings of a stacked part's (HBM) in-memory fabric: each
  chip-tier transfer is charged the die boundaries it crosses under
  `memory.channels_per_die`, with TSV leakage and area from the built channel
  routers (`power.models.fabric_tsv`). Its 3D DRAM array path (`is_3d_mem`)
  is still not used: the HBM array is priced by the part record.
- CACTI-D (Thoziyoor et al., ISCA 2008) -- the DRAM technology extension.
  It supplies the `comm-dram` and `lp-dram` cell/device types, i.e. the
  columns the ENTIRE DRAM-periphery family factor is a ratio against
  (fa/fd/fl = comm-dram vs hp/lstp/lp-dram), the DRAM array model behind
  the JEDEC-calibrated die areas, and the lp-dram column the E2 corner
  ruling maps a low-power on-die PE onto.
Their internal constants are theirs; where PIMID overrides or injects a
value, the row above carries the source, and the injection prints it.

## Local archive

The development tree's local document archive holds copies of: JESD79-3D,
JESD212C, JESD232A, JESD235B ballout, JESD238B (HBM3), JESD239E, JESD250D
(GDDR6), JESD270-4A, JESD330-4, assorted JESD8-* interface standards, the
Micron LPDDR5X datasheets, Sohn 2016/2017, Vogelsang 2010, O'Connor 2017,
the UPMEM Hot Chips 31 deck, and Gomez-Luna 2021. The archive is not part
of the published tree; this file is.

## Corroborations and normative upgrades (2026-09-04 document sweeps)

| Item | Corroborating/upgrading source | Effect |
|---|---|---|
| DDR5 preset chain (bins 3200AN/BN/C = 24/26/28, nRAS 52, nWR 48, nRTP 12, nCCD*/WTR formulas, all nine orgs, rounding algorithm, VDD/VPP) | JESD79-5D v1.41 (published Nov 2025): Table 283 p.390, Table 334 p.449, Tables 4-7 p.7, cl.13.2 p.447-448, Table 196 p.330 | Micron-transcription citations upgraded to NORMATIVE; the Q3'16 ballot draft's differing bins are superseded (odd CLs eliminated, NOTE 12 p.407) |
| DDR5 R7 electricals (RON 34/40/48 @ RZQ=240; RTT_WR default 240; RTT_NOM_WR/RD default 80; RTT_PARK default OFF; IDD conditions RZQ/7 + RZQ/6 + RZQ/2) | JESD79-5D MR5 p.38, MR34 p.61, MR35 p.62, Table 315 NOTE 2 p.431, Tables 191/245 pp.325/366 | All four R7 anchors now NORMATIVE; the "self-contained 1.1 V point, no JESD8-*" claim is confirmed (the standard never references any JESD8-* document) |
| HBM2 refresh model (tREFI 3.9 us; tRFC 260 ns @4H / 350 ns @8H) | AMD PG276 v1.0 p.23 -- vendor-authored controller guide | Independent second-vendor confirmation of the JESD235D-anchored values; also sources tREFI halving at 85-95 C |
| HBM temperature-refresh ladder (4x/2x/1x/0.5x/0.25x tREFI by TEMP[2:0]) | Intel UG-20031 Table 30 p.66 + Agilex M HBM2E IP UG Table 5 p.17; AMD DS923 note 16 p.5 (>=4x above 95 C) | Sourced across three vendors; candidate future modeling item (current refresh model is temperature-flat) |
| HBM3 tRFCab 260 ns at the emitted HBM3_4Gb org | JESD238B.01 Table 93 PDF p.179: tRFCab keyed by CHANNEL density -- 4 Gb/ch 260, 8 Gb/ch 350, 16 Gb/ch 450 | Community flag (ramulator2/gem5 "350 ns") resolved: they describe an 8 Gb/channel org; both readings are the same table, different rows |
| HBM3 energy tRFCab and refresh (1.12.80) | tRFCab 350 ns; refresh 60.06 nJ per channel REF (IDD5 - IDD2N = 156.0 mA at 1.1 V) | JESD238B.01 Table 93 printed p.165 (16 Gb/die 8-High, 8 Gb per channel: 350 ns) and Table 4 (8-High: SID + BA[3:0], 32 banks per PC, 16384 rows, 3.9 us); refresh energy per REF proportional to the rows per REF (derivation sec. 1.3): 2 x 30.03 nJ (DERIVED) | Was 260 ns / 30.0 nJ, the 4 Gb/channel (4-High) layout. No HBM3/HBM3E IDD table is published (JESD238B.01 Tables 90/91 are templates) |
| In-order PE integer datapath (1.12.96) | ALU 5,555 GE per busy cycle; ID operand logic 3,601 GE; multiplier 18,728 GE at 0.170 toggles per gate per multiply | Open-flow synthesis of CV32E40P b353438f (cv32e40p_alu + popcnt + ff_one, cv32e40p_id_stage + controllers, cv32e40p_mult; Nangate 45 nm OCL); per-multiply activity from the zero-delay gate-level simulation (0.113) x the glitch ratio 0.099 / 0.066 | DERIVED; FPnew Fig. 4 (RI5CY Int Mult 18 kGE) agrees |
| In-order PE forwarding, latch register file, front end (1.12.95) | MUX2_X1 A/B 0.945598 fF, S 1.919942 fF, Z 2.853287 fJ; DLH_X1 G 0.985498 / D 0.914139 fF; CLKGATETST_X1 5.710753 / 15.494020 fJ per cycle; front end 144 flip-flops | Nangate 45 nm OCL typical liberty (1.10 V), first table entries; structure from CV32E40P (openhwgroup) rtl/cv32e40p_prefetch_buffer.sv, _prefetch_controller.sv, _aligner.sv, _controller.sv, _register_file_latch.sv | Node-scaled as the published flip-flop (DeepScaleTool) |
| In-order PE logic gate counts (1.12.95) | IF 3.02, ID 3.94, CSR 3.41, LSU 1.02 kGE (combinational) | Open-flow synthesis of CV32E40P b353438f (sv2v + yosys + Nangate 45 nm OCL), 1 GE = NAND2_X1 0.798 um^2 | DERIVED; Schuiki et al. IEEE TC 2021 Fig. 9 and FPnew (Mach et al. TVLSI 2021) Fig. 4 printed beside |
| Cell-level logic activity and wire load (1.12.95) | 0.099 toggles per net per cycle; wire load 1.62 (45 nm), 1.83 (15 nm), 1.61 (7 nm) -> 32 nm 1.69, 22 nm 1.76 | Gate-level power of the placed-and-routed CV32E40P (OpenROAD-flow-scripts; Nangate45, NanGate 15 nm OCL, ASAP7), timing-annotated simulation, OpenSTA with and without the routed parasitics | DERIVED; 15 nm parasitics estimated from layer R / C derived from FreePDK15 (arXiv 2009.04600 Table III, Sakurai-Tamaru), not extracted |
| Routed-core flip-flops and repair buffers (1.12.95) | 1,257 flip-flops (1,062 enable, 195 plain); enable duty non-CSR 0.349, CSR 0; repair / hold buffers 0.188 (45 nm), 0.300 + 0.198 (7 nm) of the logic energy | Same routed CV32E40P and gate-level runs | DERIVED; log-node interpolation 32 nm 0.245, 22 nm 0.307 |
| HBM1/HBM2 timing structure + I/O energy class | SK hynix Hot Chips 26/28 decks: tRC 40-48 ns, tCCD 1 CK, 2 KB page, per-gen speeds, I/O energy ratios | Vendor-authored public corroboration for the HBM2 rung |
| HBM2E PHY energy | Samsung HC32 poster p.11: measured WRITE 1.07 / READ 0.56 / IDLE 0.02 pJ/b at 0.75/1.2 V | Validation band for the interface-energy layer |
| HBM2 measured IDD + model-error band | CMU-SAFARI HBM-Power artifact: 36-chip IDD distributions; DRAMSim3-class HBM2 power model = 20.7% MAPE (all-0s) / 39.8% (random) vs silicon | The citable error bar for simulator-derived HBM energy; per-chip spread 1.6-1.8x standby |
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
| HBM3 PHY energy | 0.25 pJ/bit write / 0.29 read at 9 Gb/s/pin, VDD 0.66 V / VDDQ 0.30 V. SCOPE: SoC-side PHY incl. digital back-end (TRX, strobe control, per-bit de-skew, read FIFO, VT tracking, DFI logic, addr/cmd interface, DFT) -- THE DRAM DIE IS EXCLUDED | Chae et al., Samsung Foundry, JSSC 59(1) 2024 Table II p.239 | Used three ways: (1) PRICED as the HBM3 host memory-controller PHY energy per bit, its own scope (`cacti_io_wrapper.cpp` hbmHostPhyEnergyPJBit, McPAT MCPHY); (2) its supplies 0.66 / 0.30 V scale the HBM3 host PHY idle (computeDramIO); (3) UNSCALED as the stand-in for the DRAM end's per-access I/O energy, `params/dram/hbm3.yaml` idd.io_share (no DRAM-side HBM3 I/O energy is published; the measured DRAM VDDQ band 0.19-0.27 pJ/bit is its printed guard; the DRAM's 0.40 V VDDQL against the PHY's 0.30 V would be 1.8x under CV^2, not applied because Table II gives no VDD/VDDQ split). (1) and (3) are the two ends of the link, each charged once |
| HBM3E vs HBM3 | "bump map footprint, the number of channel and I/Os, and the operation voltage, are identical to the latest HBM3" -- corroborated by the paper's own table (ballmap 7.08 x 8.82 mm, bump pitch 96 x 110 um, chip 11 x 11 mm, rails all identical). EXCEPTIONS: density (24 Gb dies, 16-high -> 48 GB) and row address RA<0:13> -> RA<0:14> | Lee et al., SK hynix, ISSCC 2024 13.4 p.238-239 | Justifies modelling HBM3E as HBM3 at a raised pin rate -- for the INTERFACE, not for capacity or row count |
| HBM termination | "ODT not allowed in HBM (static power)"; interface "CMOS, un-terminated" | ISCA 2025 tutorial slide 46 (Song, Samsung); Chun JSSC 2021 Table I p.200 | Third independent corroboration of HBM termination = 0 (with JESD238B cl.9.1) |
| HBM2 system power split | SoC PHY 33.3% / DRAM core 37.4% / DRAM interface + channel IO 29.3%, at 2 Gb/s streaming reads, 1024 DQ | ISCA 2025 Tutorial slide 15, sourced "Rambus Inc." (Woo/Elsasser, Rambus) | VALIDATION band for the ONE-FABRIC three-way projection. Condition: read-streaming best case, not mixed traffic |
| Cross-generation tCCD in ns | DRAM core frequency pinned <= 200 MHz since DDR2; tCCD_L pinned at 5 ns since DDR2-800; prefetch absorbs all data-rate scaling | ISCA 2025 Tutorial slide 130 | Structural cross-check; JEDEC gives tCCD_L in tCK per bin, this gives the ns invariant |
| tCCD_L family split, with cause | ~5 ns where the IO sense amp sits at the bank end (DDR/LPDDR) vs ~2.5 ns mid-bank (GDDR/HBM) | ISCA 2025 Tutorial slide 56 | Physical justification for our per-family tCCD_L values -- not derivable from the standards |
| DDR5 latency-under-load validation target | DDR5-6400, 8 BG x 4 banks, 1/2 ranks, 32-entry R/W queues, 67/33 R/W, closed page, random: 54 ns unloaded; saturation ~17.5 GB/s (32 banks) and ~23.3 GB/s (64 banks) of 51.2 GB/s peak | ISCA 2025 Tutorial slides 93-95 (DRAMSys) | VALIDATION -- a fully specified, reproducible performance target; standards contain no performance data |

## Round-5 calibration corrections (1.11.66, 2026-09-18)

| Item | Value now | Source | Note |
|---|---|---|---|
| GDDR6 clock relation | 8 bits/pin per CK: rate 14000 MT/s, tCK = 8E6/rate = 571 ps, nBL 2, nCCDS 2 | Samsung K4Z80325BC Table 91 (tCK 0.57 ns at 14 Gbps); JESD250D Table 1 (CK 1.5 GHz <-> 12 Gbps). Proof: every preset cycle count reproduces Samsung's 14 Gbps AC set at 0.57 ns within 1-2% and nothing at 1.00 ns | CORRECTS a 1.11.63 regression (2 bits/pin assumed) that 1.11.65 propagated into tWTR. All seven presets' rate column now equals the priced rate; the static rate table is demoted to a cross-check |
| GDDR6 tRFCab | 120 ns (all densities) | Samsung K4Z80325BC Table 92 PDF p.158 (16 Gb: the same 120 ns, estimated from a vendor source that cannot be disclosed publicly) | Was DDR4's 360/220 (impl/energy) |
| GDDR6 ns getters | derived from the preset (14.85/13.7/14.85/30.3 ns) | preset x 571 ps | The 14.8/28.0 literals were the vendor values; the derivation now reproduces them |
| DDR3 org transcription | 65536 rows x 2048 cols (x8) | DDR3.cpp 1.11.63 rows | Transcription had gone stale (131072 x 1024); density product hid it |
| DDR5 banks, 8 Gb x8 | 8 BG x 2 = 16 | JESD79-5D Table 4 printed p.7 (BA0); 32 begins at 16 Gb (Tables 5-7) | Arch object + main.cpp table were at 32; preset was right |
| DDR5 speed grades | 3200AN (8 Gb) / 4800B / 5600B (16 Gb); default 4800 (5600 in 1.12.18 only; 4800 again since 1.12.19, user ruling 2026-10-24) | JESD79-5D Tables 283/287/289 (bins), 335/336 (per-speed AC), 71 (16 Gb tRFC1 295), 73, cl.13.2 rounding | The Micron dies are B-bin parts (-48B, -56B) |
| DDR5 IDD, 4800B / 16 Gb | IDD0 103, IDD2N 92, IDD3N 142, IDD4R 377, IDD4W 349, IDD5B 277, IDD2P 88 mA | Micron MT60B Rev A addendum Table 6 pp.17-19 | Default grade (again since 1.12.19, user ruling 2026-10-24; 5600 was the default in 1.12.18). VDD typical via the component factors; VDDQ/VPP at the published maxima (no published ratio) |
| DDR5 IDD, 5600B / 16 Gb | 53 / 49 / 91 / 218 / 241 / 377 / 47 mA | Micron MT60B Rev D addendum Table 8 pp.18-20 | Default grade in 1.12.18 only (Rev D Table 8 has no 4800 column); selectable with `ddr5_speed_grade: 5600`. VDD typical via the component factors; VDDQ/VPP at the published maxima (no published ratio) |
| DDR5 IDD, 3200 / 8 Gb | 55 / 34 / 42 / 148 / 168 / 120 / 20 mA | UNSOURCED (no held datasheet publishes a 3200 column) | Stated; 2-3x below either addendum on standby |
| DDR4 IDD5B | 457.4 mA (1.11.91) | Derived from Micron MT40A Rev B Table 148 p.332 IDD5R 53 / IDD2N 34 at tRFC/tREFI 350/7800 | 1.11.66 had 362 from a Rev A IDD5R against another revision's standby (R8-9) |
| DDR4 IDD row, x8 2400 (1.11.91) | IDD0 48, IDD2N 34, IDD3N 43, IDD4R 135, IDD4W 123, IDD2P 25 mA; IPP2N = IPP3N 3 mA at VPP 2.5 V | Micron MT40A (8gb_ddr4_dram.pdf Rev. N) Table 148 Die Rev. B p.332; IPP3N applies to all IDD2x/3x/4x per note 22 p.334; VPP p.1 | User ruling R8-9. No IDDQ rows published (n/a) |
| DDR3 IDD row, x8 1600 (1.11.91) | IDD0 55, IDD2N 32, IDD3N 38, IDD4R 157, IDD4W 125, IDD5B 235, IDD2P1 (fast exit) 32 mA | Micron MT41K (4Gb_DDR3L.pdf Rev. Q) Table 20 Die Rev. E p.43 (Rev. R p.44; 1.12.25: priced at 1.5 V by default, the table being for 1.35/1.5 V) | User ruling R8-9. No VPP rail, no IDDQ rows |
| IDD3N basis (1.11.91) | ALL_BANKS DDR3/4/5; ONE_BANK HBM2/HBM3/GDDR6; UNVERIFIED LPDDR5 | DDR5 core sheet Table 387 p.450; MT40A Table 136 p.317; MT41K Table 13 note 3 p.36; JESD235D PDF p.109; JESD238B.01 Table 83 PDF p.165; JESD250D Table 65 PDF p.165 | User ruling R8-2 (a); LPDDR5 condition table (JESD209-5C) not readable here |
| DDR5 IDDQ / IPP, 4800B (1.11.91) | IDDQ3N 31, IDDQ4R 57, IDDQ4W 198 mA at VDDQ 1.1 V; IPP2N 6, IPP3N 7 mA at VPP 1.8 V | MT60B Rev A Table 6 pp.17-19; VDDQ/VPP core sheet p.1 | User ruling R8-8 (a). Core sheet p.448: IDDQ "cannot be directly used to calculate IO power" (stated) |
| DDR5 IDDQ / IPP, 5600B (1.11.91) | IDDQ3N 70, IDDQ4R 218, IDDQ4W 271; IPP2N 7, IPP3N 8 mA | MT60B Rev D Table 8 pp.18-20 | |
| LPDDR5 IDDQ / VDD1 (1.11.91) | IDD3NQ 0.6, IDD4RQ 111.9 (typical, note 4), IDD4WQ 0.6 mA at VDDQ 0.5 V; IDD2N1 1.5, IDD3N1 2.8 mA at VDD1 1.8 V (priced in the IPP slot) | Micron Y52P LPDDR5X (public) Table 18 PDF pp.42-43, x16, 7500 Mb/s (item 11 re-source) | No VPP ball in LPDDR5 (p.1 rails) |
| HBM2/HBM3 IDDQ (1.11.91) | BAND 0.19-0.27 pJ/bit on reads (midpoint charged) | Cho ISSCC 2018 Fig.12.3.1 driver figure (below) | JESD238B.01 PDF p.164: HBM IDDQ vendor-simulated, not published |
| DDR4 tRFC1, 8 Gb | 350 ns | Micron MT40A p.369 | Was 360 |
| HBM2 IDD row | IDD0 141, IDD2N 136, IDD3N 133, IDD4R 464, IDD4W 356, IDD5B 189 mA per channel | CMU-SAFARI HBM-Power 36-chip means / 8 channels; conversion per JESD235D cl. 9.1 printed p.100 | User ruling R8 #10; measurement-class. IDD2P 7 stays unmeasured. Model stack standby 1.31 W vs measured 1.37 W |
| HBM3 arch core clock | 1600 MHz (stamped from tCK 625) | JESD238B.01 Table 92; Ryu JSSC'23 p.1052; Park JSSC'23 p.259 | Was 3200 (rate/2) |
| HBM2/HBM3 arch ns timings | stamped from preset (HBM2 16.66/16.66/15.0/33.3; HBM3 16.25 x3 / 33.125) | preset x tCK | The R6 stamp now covers all five object-bearing techs |
| dramRowBytes | preset cols x dq / 8 (DDR3 2048, DDR4/5 1024, LPDDR5 2048, GDDR6 2048, HBM2/3 1024 B) | preset org rows; HBM: JESD235D/238B "Page Size per PC 1 KB" | Was a per-generation guess wrong on three of seven |
| DDR4 IPP5B (user ruling 2026-10-25, second supply rails) | 47.571 mA = IPP3N 3 + (IPP5R 5 - 3) x tREFI 7800 / tRFC 350 (DERIVED, as IDD5B from IDD5R): 39.0 nJ VPP per REF per die | Micron MT40A Rev B Table 148 p.332 (IPP5R 5 mA, IPP3N 3 mA) + note 22 p.334; VPP 2.5 V p.1 | Was not priced (no IPP5B row) |
| LPDDR5 VDD1 active rows (user ruling 2026-10-25) | IDD01 3.8, IDD4R1 18.0 -> 15.77, IDD4W1 16.5 -> 14.49 (burst excess x 6400/7500, as VDD2H), IDD51 24.0 mA at VDD1 1.8 V, in the IPP slot | Micron Y52P LPDDR5X Table 18 pp.42-43, x16, 7500 Mb/s | Maxima on both ends (no published typical ratio for VDD1) |
| GDDR6 VPP (user ruling 2026-10-25) | per channel: IPP2N = IPP3N = IPP4R/W 5, IPP5B 38.5 mA at VPP 1.8 V; activation 216 pJ = 1.8 V x (IPP7 31 - IPP3N 10) mA / 175 M ACT/s per device (IDD7 route) | Samsung K4Z80325BC Rev 1.3 Table 82 p.146 (x32, HC14) + note 2; Table 90 pp.152-155 (IDD7 loop, one ACT per channel per 20 nCK); Table 79 p.136 (VPP) | Was not priced; IPP0 14 not used (ill-conditioned IDD0 route) |
| LPDDR5 tREFI (energy row) | 3906 ns | JESD209-5C 3.906 us; = LPDDR5.cpp tREFI_BASE | Was 3904 |
| Organization shape check | transcription banks/rows/cols bound field-by-field to the instantiated IDRAM at every wrapper init | -- | Structural; PIMID_ORG_BREAK proves it fires |

## Host core awake idle (reference line, not charged)

| Quantity | Value in use | Source | Consumed at |
|---|---|---|---|
| Haswell-EP package C1E / C3 / C6 power per core | 1.91 / 1.17 / 0.13 W/core; C1E - C6 1.78, C3 - C6 1.03 W/core (DERIVED: least-squares slopes over 34 SKUs, 4..18 cores; C1E line 18.5 W + 1.91 W/core, rms 3.7 W) | Intel Xeon E5-1600/2600/4600 v3 Product Families Datasheet Vol. 1, 330783-002 (June 2015), Sec. 2.10, pp.42-44 (C1E and C3 "characterized but not tested"; C6 at Tcase 50 C) | `main.cpp` host awake-idle reference line (printed, in no total) |
| Skylake-SP core CC1 - CC6 power per core | 1.21 W/core (DERIVED: 12.1 W over 10 cores, RAPL package) | Antoniou et al., "AgilePkgC: An Agile System Idle State Architecture for Energy Proportional Datacenter Servers", HPCA 2023 (arXiv 2204.10466), Sec. 5 (Xeon Silver 4114, 14 nm) | same line |
| Zen 2 core C1 - C2 power per core | 0.09 W/core (system AC) | Schoene et al., "Energy Efficiency Aspects of the AMD Zen 2 Architecture", IEEE Cluster 2021 (arXiv 2108.00808), Sec. VI-A (2x EPYC 7502, 7 nm) | same line |
