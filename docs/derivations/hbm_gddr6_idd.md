# IDD rows for HBM3, HBM2 and GDDR6: derivation and sources

This note records where the HBM3, HBM2 and GDDR6 IDD rows of the DRAM part
records come from, with their bands. Sections 0-3 were written for PIMID
1.11.91; sections 1.7-1.10 were added in 1.12.18. A row called "current",
"old" or "recommended" in a section is the row at the time that section was
written; the values in force are the part records `params/dram/hbm3.yaml`,
`params/dram/hbm2.yaml` and `params/dram/gddr6.yaml`. "The tree" means the
PIMID source tree; "the archive" is the local document archive listed in
[docs/sources.md](../sources.md).

Rows as they stood in 1.11.90:

    GDDR6 {1.35, 70, 45, 60, 210, 230, 180, 120.0, 1900.0,  1, 30}
    HBM3  {1.1,  30, 18, 22,  90, 100,  70, 260.0, 3900.0, 16,  7}
    HBM2  {1.2, 141,136,133, 464, 356, 189, 260.0, 3900.0,  8,  7}

Column order: vdd, idd0, idd2n, idd3n, idd4r, idd4w, idd5(B), trfc_ns,
trefi_ns, channels, idd2p.

---------------------------------------------------------------------------

## 0. Summary

1. **GDDR6 does not need to be derived. The tree already holds a vendor IDD
   table for the exact part it simulates.** The archive
   holds the Samsung K4Z80325BC
   datasheet (Rev. 1.3, Jan. 2020, 8 Gb GDDR6 SGRAM C-die, 187 pp.). I
   checked all 187 pages page by page and none carries a "Confidential"
   marking. Its Table 82 (PDF p.146) lists every IDD at 1.35 V, x32 mode,
   HC14 = 14 Gb/s. That is the same density (8 Gb), rate (14000),
   voltage (1.35 V) and AC set as the tree's preset. Table 91 (tRC 45,
   tRAS 30, tRFCab 120 ns) matches GDDR6_2000_1350mV_double cycle for
   cycle. The tree already cites this datasheet for the GDDR6 electricals
   (cacti_io_wrapper.cpp:279, docs/sources.md:19) and for tRFCab
   (GDDR6.cpp:408); only the IDD row never used it. The energy-per-bit
   derivation is kept below (section 2.4), but only as a cross-check,
   and it is the weaker of the two.
   - Recommended row, per device: {1.35, 430, 310, 460, 1220, 1470, 790,
     120, 1900, 1, 230}.
   - The tree's current row is 4-7x below these datasheet maxima. The
     burst excess alone is 150 mA against 760 mA.

2. **HBM3 is derived from the SAFARI HBM2 measurement using JEDEC ratios.**
   - Recommended row: {1.1, 139, 132, 132, 1575, 1102, 237, 260, 3900, 16,
     33}. Bands are in section 1.4.
   - The derived sequential-read energy is 3.92 pJ/bit (band 1.36-4.67).
     SK hynix (IEDM 2023) puts HBM2E at about 4.3 pJ/bit and HBM3E 20%
     lower, at about 3.44 pJ/bit. HBM3 sits between those two
     generations, and the derived value lands inside that bracket.
   - The tree's current HBM3 row gives 0.26 pJ/bit, 13-17x below the
     published figures.

3. **The HBM2 base row has four defects of its own (section 1.1). They
   were found while deriving HBM3 and they change the HBM2 row itself.**
   - (a) SAFARI measured at 1.2 Gb/s/pin (600 MHz DRAM Bender clock), but
     the tree prices those currents with the 2.4 Gb/s preset's burst time.
   - (b) Every SAFARI loop runs on pseudo-channel 0 only of each channel.
   - (c) About 72% of the measured standby current is a stack-level
     "off-power" baseline. SAFARI's own HBM3 extrapolation excludes it.
   - (d) The IDD5 loop's tRFC is 160 nCK = 266.7 ns, not the 350 ns its
     code comment says.
   - Net effect of (a)+(b): the tree prices HBM2 burst at 1.29 pJ/bit,
     while the measurement at its own operating point gives 5.16 pJ/bit
     (2.58 if (b) turns out wrong).

4. **The basis of IDD3N.** All three standards define IDD3N as one
   bank active:
   - JESD235D Table 57 (printed p.101 / PDF p.109)
   - JESD238B.01 Table 83 (printed p.151 / PDF p.165)
   - JESD250D Table 65 (printed p.153 / PDF p.165)
   - Samsung K4Z80325BC Table 82 agrees.

   The 1.11.86 correction, IDD3N(1 bank) = IDD2N + (IDD3N - IDD2N)/banks,
   is therefore wrong for HBM2, HBM3 and GDDR6.
   - On HBM it is numerically harmless. SAFARI measured IDD3N1 and IDD3N16
     separately, and they differ by 1.9 mA per channel.
   - On the GDDR6 datasheet row it inflates the per-device activate
     energy 5.7x: 1215 pJ becomes 6910 pJ.

---------------------------------------------------------------------------

## 1. Derivation 1 -- HBM3 from the measured HBM2 row (strong chain)

### 1.1 The base: what the HBM2 numbers actually are

Source data: the artifact files zen_hbm2_idd_36chips_summary.csv and
zen_hbm2_idd_per_chip.csv. These come from the CMU-SAFARI "HBM-Power"
artifact, https://github.com/CMU-SAFARI/HBM-Power/tree/artifact (fetched
2026-09-26):
- 36 HBM2 stacks on AMD Alveo U55C
- DRAM Bender
- the on-board current sensor on the HBM VDD rail

**Citation status.** The artifact README (fetched) gives no paper title,
authors, venue or year. It only says "An accurate power model for high
bandwidth memory (HBM) building on detailed power consumption
characterization of 36 real HBM2 stacks". The title the project's index
records ("An Experimental Study of High-Bandwidth Memory (HBM) Power
Consumption") could NOT be confirmed from any
fetched page, and no preprint was found. So cite it as a software/data
artifact:

> CMU-SAFARI, HBM-Power artifact repository, branch "artifact",
> https://github.com/CMU-SAFARI/HBM-Power/tree/artifact, accessed
> 2026-09-26 (paper under artifact evaluation; not yet public).

Do not attach authors or a venue until the paper appears.

Facts read from the artifact source (raw files fetched from
raw.githubusercontent.com/CMU-SAFARI/HBM-Power/artifact/...):

- **Clock.** sources/table3/configs/HBM2_1.2Gbps_timing_BL4.json has
  "tCK_ps": 1666.7, i.e. 600 MHz = 1.2 Gb/s/pin. power_measurement.cpp,
  program_idd0():
  "// Assuming 600MHz HBM frequency => 1.67ns per cycle ... tRAS=33ns = 20
  cycles ... tRC=48ns = 29 cycles ... tRP=15ns = 9 cycles".
  So the IDD0 loop runs at tRC = 48.3 ns and tRAS = 33.3 ns.

- **Pseudo-channel coverage.** The file names that
  generate_standardized_csvs.py maps to every IDD test are of the form
  "hbm_idd4r_full_multi_..._0_1_2_3_4_5_6_7_pc0_...". That is channels
  0-7, pseudo-channel 0 only. The loops issue every command with that one
  `pc`: program_idd4r_full() is READ, NOP, READ, NOP to 8 banks of one
  PC, which keeps one 64-bit PC busy at 100%. So the per-channel
  (stack / 8) numbers are one-PC-active numbers, and the IDD4R data rate
  per channel is 64 b x 1.2 Gb/s = 76.8 Gb/s, not 153.6.

- **Refresh loop.** program_idd5() uses "int tRFC = 160;", i.e. 266.7 ns,
  against the comment "tRFC=350ns = 210 cycles" beside it. REF goes to
  one PC.

- **Off-power.** data/no_hbm_idd2_measurements.csv, IDD2 with only
  channel 0 running, averages 821.3 mA (1638 samples). The all-channel
  IDD2 is 1087.4 mA. So:
  - per-channel standby increment = (1087.4 - 821.3)/7 = 38.0 mA
  - stack baseline = 783.3 mA

  This matches the "off-power (778.1)" named in
  sources/table3/configs/IDD_ours_allzeros.json. SAFARI's HBM3 config
  (sources/figure16/configs/HBM3_6400_power_datapattern.json) states
  "off-power excluded". Whether that baseline is DRAM current at all
  (versus other load on the same sensed rail) is not stated anywhere
  fetched.

HBM2 base, per 128-bit channel = stack mean / 8. The per-chip ranges come
from zen_hbm2_idd_per_chip.csv.

| quantity | value | per-chip range |
|---|---|---|
| IDD0 (one bank, tRC 48.3) | 140.6 mA | 108.9-175.1 |
| IDD2N | 135.9 mA | 99.0-173.8 |
| IDD3N (IDD3N1) | 133.4 mA | 98.3-165.9 |
| IDD3N16 | 135.3 mA | 102.1-168.2 |
| IDD4R | 463.8 mA | 394.0-508.2 |
| IDD4W | 355.6 mA | 300.2-400.3 |
| IDD5B (tRFC 266.7) | 189.3 mA | 152.1-228.5 |
| VDD | 1.2 V (JESD235D Table 49, printed p.96: VDDC = VDDQ = 1.2 V typ) | |

Energies derived from the base:
- **Read burst, per bit:** e_r2 = 1.2 x (463.8 - 133.4) mA / 76.8 Gb/s =
  **5.16 pJ/bit**. With the all-zero data pattern (CMU-SAFARI HBM-Power
  beat-pattern IDD data, pattern 0000: 2540.8 mA stack) it is
  2.88 pJ/bit. If the one-PC reading were wrong it would be 2.58.
- **Write burst, per bit:** e_w2 = 1.2 x 222.2 / 76.8 = **3.47 pJ/bit**.
- **Activation:** E_act2 = 1.2 V x 7.65 mA x 48.3 ns = **444 pJ** per
  1 KB activation. The 7.65 mA is the per-chip PAIRED excess
  IDD0 - (20 IDD3N1 + 9 IDD2)/29, divided by 8 (mean 7.65, sd 1.84, sem
  0.31, n = 36; chip range 3.9-11.5 mA, i.e. 226-667 pJ). Pairing the
  chips is what makes this term usable: the unpaired means differ only
  by noise.
- **Refresh:** E_ref2 = 1.2 V x (189.3 - 135.9) mA x 266.7 ns = **17.1 nJ**
  per REF of one PC.

Cross-check of the base against O'Connor et al., "Fine-Grained DRAM:
Energy-Efficient DRAM for Extreme Bandwidth Systems", MICRO-50, 2017
(https://www.cs.utexas.edu/~skeckler/pubs/MICRO_2017_Fine_Grained_DRAM.pdf).
p.3: "each HBM2 access incurs 3.92 pJ/bit of energy (including ECC
overhead)". Table 3, p.10: row activation 909 pJ; pre-GSA 1.51,
post-GSA 1.17 and I/O 0.80 pJ/b "at 50% activity rate".
- Measured activation (444 pJ) is 0.49x that model's.
- Measured burst on the JEDEC-like pattern (5.16) is 1.5x that model's
  column sum (3.48); on the all-zero pattern (2.88) it is 0.83x.

**What this means for the HBM2 row itself.**
The tree charges (IDD4R - IDD3N) x tBurst with tBurst = 1.667 ns, which
is 64 B on a 128-bit channel at 2.4 Gb/s (both PCs). That is 1.2 x 330.4
x 1.667 / 512 = 1.29 pJ/bit, i.e. 4.0x below the measurement's own
5.16 pJ/bit. It is the same "one part, one speed bin" defect the tree
fixed for DDR4/DDR5 (1.11.52 D002).

The fix: move to the preset's rate at constant energy per bit. The HBM2
IDD4R excess becomes 330.4 x (307.2 / 76.8) = 1322 mA per channel
(IDD4W: 889 mA). The band runs down to 2x smaller if the one-PC reading
of the artifact is wrong. This moves HBM2 burst energy 2-4x; it was applied
later (see section 1.10).

### 1.2 Target organisation (JESD238B.01, tree presets HBM3_4Gb / HBM3_6.4Gbps)

- **IDD basis:** per 64-bit channel, VDDC only. JESD238B.01 cl.9.1,
  printed p.150 / PDF p.164, says "IDD currents are measured as
  time-averaged currents with all VDDC microbumps ... tied together" and
  "values in the vendor's datasheet shall be given per channel". IDDQ is
  not included: "DRAM vendors shall provide simulated values"; IDDQL is
  separate.
- **Voltages:** VDDC 1.1 V, VDDQ 1.1 V, VDDQL 0.4 V, VPP 1.8 V (Table 70,
  PDF p.152, printed p.138). HBM2 is VDDC = VDDQ = 1.2 V (JESD235D
  Table 49).
- **Channel:** 64 DQ (Table 1, printed p.3) = 2 PCs x 32 b with 256-bit
  prefetch (cl.3.1.2, printed p.4). HBM2's channel is 128 DQ = 2 PCs x
  64 b.
- **Per channel at 4 Gb/channel:** 16 banks per PC (BA[3:0]), RA[13:0],
  page 1 KB per PC, refresh period 3.9 us (Table 4, printed p.6). This
  is the same bank count, page size and rows as the HBM2 4 Gb/channel
  geometry the SAFARI loops use.
- **tRFCab 4 Gb/channel = 260 ns** (Table 84, printed p.153; an
  "Example" table, but the only tRFC given). tRC/tRAS: preset 79/53 nCK
  x 0.625 ns = 49.4/33.1 ns.
- **Rate:** 6.4 Gb/s/pin. Per-channel line rate with both PCs streaming
  = 64 x 6.4 = 409.6 Gb/s. This is the tree's IDD basis: BL8 x 64 b =
  64 B per tBurst 1.25 ns.

### 1.3 Scaling, term by term

Bands are the product-extremes of the stated exponent and fraction ranges.
Every factor is listed with its source; where none exists this says so.

**IDD2N / IDD3N (background).**
- *Physical content.* Per-channel clocked periphery (CK receiver,
  command decode, clock tree): 38.0 mA of the HBM2 base (from the no-HBM
  split above). The rest is a 783.3 mA stack-level baseline of unknown
  origin (base die, bias, leakage, or non-DRAM load on the sensed rail).
- *Scaling of the clocked part.* x (V3/V2)^a, a in [1,2] (switching
  current is linear in V, and leakage-like terms are stronger), and x
  (fCK3/fCK2)^g with fCK 1600/600, g in [0.5, 1].
- *Scaling of the baseline.* If it counts as DRAM, it is spread over 16
  channels of a same-capacity stack (8 GB: 16 x 4 Gb) instead of 8.
- *Result.* Centre (a = 1.5, g = 1, baseline included): 89.0 + 43.0 =
  **132 mA**. Band **52-138 mA**. The lower half of the band is the
  "off-power is not DRAM" reading, which must be decided together with
  the HBM2 row.
- *IDD3N.* HBM2 silicon shows IDD3N1 = 0.98 x IDD2N and IDD3N16 =
  1.00 x IDD2N, so the open-row increment is inside the noise. Set
  **IDD3N = IDD2N** (band 0.98-1.02 x IDD2N).
- *Cross-check.* SAFARI's own HBM3 config (off-power excluded) has
  IDD2N 38.3 mA/PC = 76.6 mA/channel. That is inside the lower half of
  the band (52-93 without the baseline).

**IDD0 (one-bank activate/precharge at tRC).**
- *Physical content.* Charge per activation is roughly page bits x
  C_bitline x V_array swing, plus wordline and precharge. The page is
  unchanged (1 KB/PC in both standards). No citable source says whether
  the array voltage tracks VDDC, so the exponent is banded:
  E_act3 = E_act2 x (V3/V2)^b, b in [1, 2] (b = 1 if the array rail is
  regulated and its charge is drawn from VDDC; b = 2 if it scales with
  VDDC).
- *Result.* Centre (b = 1.5): 444 x 0.878 = **389 pJ**. The band runs
  190 pJ (chip minimum, b = 2) to 833 pJ (O'Connor's 909 pJ model,
  b = 1).
- *Back to a current.* dI0 = E_act3 / (VDDC x tRC3) = 389 / (1.1 x
  49.4) = **7.2 mA** (band 3.5-15.3), so IDD0 = IDD2N + 7.2 = **139 mA**.
  With IDD3N = IDD2N, the vendor-method form (a vendor method that cannot be disclosed publicly) returns E_act exactly, and the
  1.11.86 bank division has no effect.

**IDD4R / IDD4W (burst).**
- *Physical content.*
  - The array/column path (bitline -> LIO -> GIO -> TSV -> base die) is
    C x V^2 per bit, so its energy per bit does not depend on rate.
  - The I/O share: O'Connor Table 3 gives 0.80 of 3.48 pJ/b = 0.23 for
    HBM2. JEDEC excludes it from IDD for both HBM2 and HBM3 (IDDQ
    "simulated"), but the tree has no separate HBM I/O term, so the row
    must carry it as equivalent VDDC current.
- *Scaling of the energy per bit.*
  e3 = e2 x [(1 - f_io) (V3/V2)^2 k_proc + f_io r_io]
  - f_io in [0.10, 0.25], centre 0.23 (O'Connor).
  - k_proc in [0.85, 1.00]. The only citable anchor is SK hynix's -20%
    from HBM2E to HBM3E (below); (1.1/1.2)^2 already accounts for -16%
    of it.
  - r_io in [0.30, 0.60], the HBM3 TX output stage on VDDQL 0.4 V
    against HBM2's 1.2 V I/O: (0.4/1.2) to about 2x that for serializer
    and strobe overhead. There is no citable number; this is judgement,
    stated as such.
  - Rate: linear current (energy per bit constant) at the centre. The
    lower edge also allows sub-linear current (I ~ rate^0.9 over
    76.8 -> 409.6 Gb/s, x0.846).
- *Result.*
  - Read: **3.87 pJ/bit** (band 1.33-4.57), so IDD4R - IDD3N =
    e3 x 409.6 Gb/s / 1.1 V = **1443 mA** (band 496-1701).
  - Write: 2.61 pJ/bit (band 0.90-3.07), so IDD4W - IDD3N = **970 mA**
    (band 334-1144).
  - The lower edges include the "both PCs were active in the base"
    reading (e2 halved).
- *Cross-check.* SAFARI's HBM3 extrapolation (all-zero pattern, off-power
  excluded) has IDD4R 630.4 mA/PC. Against IDD3N16 36.7 that is
  2 x 593.7 = 1187 mA/channel of excess, 0.82x the centre and inside the
  band.

**IDD5B (all-bank refresh burst).**
- *Physical content.* Energy per REF is proportional to rows refreshed
  per REF x page x V^b.
  - HBM2 4 Gb/channel geometry: 16384 rows x 16 banks per PC over
    8192 REFs per 32 ms = 32 rows x 1 KB per PC per REF.
  - HBM3 4 Gb/channel is identical (Table 4: RA[13:0], BA[3:0],
    3.9 us).
  - An HBM3 per-channel IDD5B has both PCs refreshing; the SAFARI loop
    refreshed one.
- *Result.* E_ref3 = 2 x 17.1 nJ x 0.878 = 30.0 nJ per channel REF.
  dI5 = 30.0 nJ / (1.1 V x 260 ns) = **105 mA**, so IDD5B = 132 + 105 =
  **237 mA**.
- *Band 25-110 mA of excess.* The low end is the U55C part being
  8 Gb/channel: U55C carries 16 GB over two stacks (not verified from a
  fetched page), which would mean twice the rows per REF in the base and
  so half the per-row energy. The low end also takes V^2 and one PC.

**IDD2P (precharge power-down).**
- There is no citable HBM input. JESD238B.01 Table 83 defines IDD2P with
  tCK = tCK(min), i.e. the clock keeps running.
- The only public sourced ratios IDD2P/IDD2N are GDDR6 230/310 = 0.74
  (Samsung T82, PLL/DLL off) and LPDDR5X 3.9/32.5 = 0.12 (Micron Y52P
  Table 18, VDD2H, x16 7500). [Until the next release the low end was a
  ratio of 0.083 estimated from a vendor source that cannot be disclosed publicly.]
- Band 0.12-0.74 x IDD2N = **16-98 mA**. Centre is the geometric midpoint
  of the ratio ends, 0.30, giving **40 mA**. (The shipped HBM3 row has used
  the Chatterjee HPCA 2017 ratio 28/40 since 1.12.20; this band is not read
  by the model.) This stays an un-sourced
  column and must be said so.

### 1.4 HBM3 table

| term | base value (source) | factor(s) with citation | derived | band |
|---|---|---|---|---|
| vdd | 1.2 V (JESD235D T49) | VDDC 1.1 V (JESD238B.01 T70) | 1.1 V | -- |
| IDD2N | 38.0 mA/ch clocked + 783.3 mA/stack baseline (SAFARI no_hbm_idd2 vs IDD2) | (1.1/1.2)^a, a in [1,2]; (1600/600)^g, g in [0.5,1]; baseline over 16 ch (JESD238B.01 T4) | 132 mA | 52-138 |
| IDD3N | IDD3N1/IDD2N = 0.98, IDD3N16/IDD2N = 1.00 (SAFARI) | one bank active (JESD238B.01 T83) | 132 mA (= IDD2N) | 0.98-1.02 x IDD2N |
| IDD0 | E_act 444 pJ per 1 KB (SAFARI paired, tRC 48.3) | (1.1/1.2)^b, b in [1,2]; page 1 KB/PC both (JESD235D T4, JESD238B.01 T4); tRC 49.4 (preset) | 139 mA (IDD2N + 7.2) | IDD2N + 3.5..15.3 |
| IDD4R | 5.16 pJ/b at 76.8 Gb/s/ch (SAFARI, one PC, 1.2 Gb/s) | f_io 0.23 (O'Connor T3); V^2; k_proc [0.85,1] (SK hynix IEDM23); r_io [0.3,0.6] (judgement); 409.6 Gb/s/ch (JESD238B.01 T1 + 6.4 Gb/s) | 1575 mA (IDD3N + 1443) | IDD3N + 496..1701 |
| IDD4W | 3.47 pJ/b (SAFARI) | same factors | 1102 mA (IDD3N + 970) | IDD3N + 334..1144 |
| IDD5B | 17.1 nJ per PC-REF at tRFC 266.7 (SAFARI) | x2 PCs; (1.1/1.2)^b; rows/REF equal (JESD238B.01 T4); tRFC 260 (T84) | 237 mA (IDD2N + 105) | IDD2N + 25..110 |
| IDD2P | none (SAFARI did not measure it) | ratio from sourced families: GDDR6 0.74, LPDDR5X 0.12 | 40 mA | 16-98 |

**Recommended row:**

    if (tech == "HBM3") return {1.1, 139,132,132,1575,1102,237, 260.0, 3900.0, 16, 33};

The absolute bands follow from the table: idd4r 548-1839, idd4w 386-1282,
idd5b 77-248, idd0 56-153.

### 1.5 Cross-check against published HBM3-class pJ/bit

Method: the model's read energy per bit for a sequential stream,
(miss x E_act + E_burst) / 512, at a miss fraction of 1/16 (1 KB page,
64 B accesses). Evaluated with the recommended row through the tree's
own formula (arrayReadNJ; tRC 49.4, tRAS 33.1, tBurst 1.25):

- E_act = 391 pJ
- E_burst = 1.1 x 1443 x 1.25 = 1984 pJ
- **3.92 pJ/bit**, band 1.36-4.67

Background at full bandwidth adds 1.1 x 132 mA / 409.6 Gb/s =
0.35 pJ/bit.

Published figures:

- **SK hynix, primary.** K.-I. Moon, H.-Y. Son, K. Lee (SK hynix),
  "Advanced Packaging Technologies in Memory Applications for Future
  Generative AI Era", IEDM 2023, paper 15-6, handout
  https://iedm23.mapyourshow.com/mys_shared/iedm23/handouts/15-6_Tue_14482.pdf
  (fetched), p.2: "While the power efficiency of HBM2E is about 4.3
  pJ/bit, that of HBM3E is reduced by 20% and HBM4 is expected to be
  reduced by 60% compared with HBM2E." That gives HBM2E about 4.3 and
  HBM3E about 3.44 pJ/bit. HBM3 is between those generations.
- **Secondary, same number.** M. J. Adiletta, G.-Y. Wei, D. Brooks, "RPU
  -- A Reasoning Processing Unit", arXiv:2602.18568 (fetched), p.5: "We
  validate our HBM-CO model against HBM3e [43] reported 3.44pJ/bit",
  where [43] is the IEDM paper above. It is the same number, not an
  independent one.
- **HBM2 anchor.** O'Connor et al. MICRO 2017 (above): HBM2 3.92 pJ/bit.

**Verdict.** The derived 3.92 pJ/bit lands inside the 3.44-4.3 bracket
that brackets HBM3, 0.4% below O'Connor's HBM2 model and 9% below
SK hynix's HBM2E figure. HBM3 should be at or below HBM2E, so a centre
near the top of the bracket is plausible but not conservative-low.

The tree's current HBM3 row gives 0.26 pJ/bit (E_act 643 pJ through the
1.11.86 path, E_burst 93.5 pJ), about 13x below the lowest published
figure. Its IDD4R - IDD3N of 68 mA carries 0.18 pJ/bit of burst.

One caveat on the published numbers: the vendor pJ/bit figures do not
state their access pattern or whether they include background, so they
bracket the result rather than pin it.

### 1.6 Assumptions (HBM3)

- A1. Stack mean / 8 is per channel (JESD235D cl.9.1 printed p.100),
  but the SAFARI loops drive pseudo-channel 0 only (artifact file names
  "_pc0_"). Per-PC energies are therefore taken from the data and
  re-assembled for a two-PC JEDEC HBM3 channel.
- A2. Energy per bit of the burst is rate-invariant (CV^2). The
  sub-linear alternative is inside the band.
- A3. The I/O share of HBM2 burst is 0.10-0.25 (O'Connor T3: 0.23). The
  HBM3 I/O ratio of 0.3-0.6 is judgement, not sourced.
- A4. The 783 mA stack "off-power" is carried in the centre as DRAM
  current, for consistency with the tree's HBM2 row, which includes it.
  If ruled non-DRAM, IDD2N/IDD3N centre is 89 mA and the HBM2 row must
  drop by about 98 mA/channel too.
- A5. The array voltage exponent is b in [1,2]. No HBM3 array-voltage
  figure was held or found.
- A6. The HBM3 target stack is 16 channels x 4 Gb (preset HBM3_4Gb). The
  base's physical density per channel is uncertain (4 or 8 Gb); that
  only affects IDD5B and is banded.
- A7. The derived currents are equivalent VDDC currents that include
  I/O (VDDQ/VDDQL) energy, because the tree prices no HBM I/O separately
  (terminationNJ returns 0 for HBM). A reader comparing them to a vendor
  VDDC-only IDD must subtract that.
- A8. IDD values are 36-chip means at 56-68 C case, not datasheet maxima.

### 1.7 Re-derived I/O share (PIMID 1.12.18)

**Why.** A review found the I/O part of section 1.3, e2 x f_io x
r_io = 5.16 x 0.23 x 0.45 = **0.534 pJ/b** (read; write 3.47 x 0.23 x
0.45 = 0.359), to be ~1.8x the only published HBM3 I/O energy. r_io (0.45,
band 0.3-0.6) was judgement (A3). No DRAM-side HBM3 I/O or IDD figure is
published.

**Anchor.** K. Chae et al. (Samsung), "A 4-nm 1.15 TB/s HBM3 Interface With
Resistor-Tuned Offset Calibration and In Situ Margin Detection", IEEE JSSC
59(1), Jan. 2024 (ISSCC 2023 paper), Table II, column "This work": HBM3,
NRZ, 4 nm, 1920 signals (1024 data), 9 Gb/s/pin, VDD/VDDQ 0.66/0.3 V,
**Energy Efficiency W/R 0.25/0.29 pJ/bit**; functions TRX, strobe control,
per-bit de-skew, read FIFO, VT tracking, DFI logic, address/command
interface, DFT logics. (Same table: the HBM2E PHY of Hot Chips'19, 7 nm,
VDDQ 1.2 V, W/R 1.07/0.56 pJ/bit.)

**Why a host-side figure stands for the DRAM-side I/O.** It is the other
end of the same link: the same 1024 DQ over the same interposer, the same
generation, the same NRZ unterminated signalling and the same rate class,
measured on silicon, per direction. It is the closest published number;
it is a proxy, and the share stays **DERIVED**. Direction: read (DRAM
transmits) takes the PHY's R figure 0.29, write (DRAM receives) the W
figure 0.25.

**Not voltage-scaled.** The HBM3 DRAM drives DQ from VDDQL 0.4 V (SK hynix,
M.-J. Park et al., IEEE JSSC 58(1), Jan. 2023, Sec. VIII: "VDD/VDDQ/VDDQL/
VPP are 1.1/1.1/0.4/1.8 V") against the PHY's VDDQ 0.3 V. A CV^2 swing
factor (0.4/0.3)^2 = 1.78 would apply only to the VDDQ part of the 0.29;
Table II gives no VDD/VDDQ split of the PHY energy (the 0.66 V digital
de-skew / FIFO / DFI / VT logic does not scale with VDDQL), so the factor
cannot be applied to a known share. The anchor is used **unscaled**; this
is stated in the record. (Scaled whole it would be 0.52 pJ/b, i.e. the old
0.534 -- the 1.8x gap is about the size of the unsourced voltage
question.)

**Re-derived burst.** e3 = e2 x (1 - f_io)(V3/V2)^2 k_proc + e_io:

| | array part (unchanged) | e_io was (r_io 0.45) | e_io now (anchor) | share mA now (x 409.6 / 1.1) | IDD4x - IDD3N |
|---|---|---|---|---|---|
| read | 5.16 x 0.77 x 0.8403 = 3.34 pJ/b (record 1244.1 mA) | 0.534 pJ/b, 198.9 mA | **0.29 pJ/b** | **108.0 mA** | 1352.1 (was 1443) |
| write | 3.47 x 0.77 x 0.8403 = 2.25 pJ/b (record 836.3 mA) | 0.359 pJ/b, 133.7 mA | **0.25 pJ/b** | **93.1 mA** | 929.4 (was 970) |

Record (params/dram/hbm3.yaml): idd4r 1532.0 -> **1441.1**, idd4w 1059.0 ->
**1018.4** (the array mA kept exactly as before; only the I/O part moves),
idd.io_share 198.9/133.7 -> **108.0/93.1 mA** with `anchor_read_pj_bit`
0.29, `anchor_write_pj_bit` 0.25, `channel_gbps` 409.6 and
`anchor_source`. The loader refuses a share that is not anchor x
channel_gbps / vdd (to 0.05 mA), and the earlier checks stay (share >= 0,
inside the burst excess, provenance DERIVED/MEASURED).

**Which figure prices the DQ-crossing I/O (the IDDQ band vs the anchor).**
The merged IDDQ band 0.19-0.27 pJ/b (reads only) is Cho et al., SK hynix,
ISSCC 2018 12.3, Fig. 12.3.1: the per-TSV DRIVER current (multi-drop 880 uA,
spiral P2P 610 uA at 1.0 V, 3.3 Gb/s) -- the core-to-base-die TSV link of an
HBM2, chart-read, converted by the tree. The anchor is the HBM3 DQ link
itself, read off a table, measured, both directions. For the energy of an
access that crosses the DQ the anchor is the more accurate figure (right
link, right generation, tabulated, per direction); the band measures a
different link (the TSV path, which section 1.3 already counts inside the
array burst, bitline -> LIO -> GIO -> TSV -> base die). Rule applied
(the most accurate figure in the total, the other as a guard): **the anchored
share prices the crossing I/O, reads AND writes** (what the burst gives up is
charged back once to a crossing access; an on-die PE access pays the array
alone), and **the band is the guard**. Consistency: write 0.25 is inside
0.19-0.27 (the band is a read-driver figure, so the run prints it as "not
covered"); read 0.29 is 1.07x the top -> the run prints a WARNING. HBM2's
record is not touched (it keeps the band as its priced value).

Side effect, stated: before this section a host write to HBM3 had its
133.7 mA share removed from the burst and nothing charged it back (the band
is reads only), so a crossing write paid no I/O at all; now it pays 0.25 pJ/b.

**Guard against published older-generation totals (printed, never tuned).**
Every HBM3 run prints `[guard]` lines. The derived total read energy is
sequential 64 B reads crossing the DQ at the channel's full 409.6 Gb/s, one
ACT per 1024 B row: array burst 3.34 + DQ I/O 0.29 + activate 0.05 +
standby 0.24 + refresh 0.02 + stack floor 0.13 = **4.07 pJ/b** (it was
4.01 with the band midpoint 0.23 as the I/O). References (record
`read_energy_guard`), expected band [(1 - 0.15) x ref, ref] per reference:

| reference | pJ/b | source | band | 4.07 is |
|---|---|---|---|---|
| HBM1 | 3.22 | SK hynix Hot Chips 26 (2014): 3.3 W at 128 GB/s | 2.74-3.22 | 1.26x, **WARNING** |
| HBM2 | 3.22 | Sohn et al., JSSC 52(1) 2017, Sec. VI: 6.6 W at 256 GB/s | 2.74-3.22 | 1.26x, **WARNING** |
| HBM2E | 4.3 | Moon et al. (SK hynix), IEDM 2023 15-6 (sec. 1.5) | 3.65-4.30 | 0.95x, inside |

The 15% is M.-J. Park et al., JSSC 58(1) 2023, Sec. VIII ("power
efficiency has been improved by up to 15%", HBM3 vs HBM2E); applying it to
the HBM1/HBM2 references is an extrapolation, stated. The two older
references and the HBM2E one disagree with each other (3.22 vs 4.3), so the
guard cannot pass both; it reports both. Nothing was adjusted to land
inside either band.

**Assumption added.** A9. The DRAM-side DQ I/O energy per bit equals the
host-side HBM3 PHY energy per bit in the same direction, unscaled for the
VDDQL 0.4 / VDDQ 0.3 V difference (no published split of the PHY energy).
Replaces A3's r_io for the I/O share; f_io (0.23, O'Connor) still sets the
array part.

### 1.8 Activation VPP and the idd0 loop label (PIMID 1.12.18)

- The SAFARI artifact's "ipp" column is NOT a VPP measurement: platform.cpp L1478-1480 (commit
  4642c61f) computes Power_VPP = P(12V_PEX) + P(3V3_PEX) - P(VCCINT) - P(VCCINT_IO) - P(HBM_1V2),
  a card residual, and generate_standardized_csvs.py L28/L208 divides it by 2.5 V. No VPP sensor is
  read.
- Activation VPP is therefore priced as a vendor-estimator RATIO of the measured VDD activate energy
  (DERIVED): Xilinx XPE 2023.1.2 HBM_Data row 44 'VPP Activate' / row 33 'VDD activate (read)':
  4G column (VU37P) 38.61/624.51 = 0.061825; 8G column (VU47P) 77.22/749.412 = 0.103041.
  Device match by stack height: HBM3 record 8-high -> 8G (0.103041); HBM2 record stack_dies 4 -> 4G
  (0.061825). The other column is printed beside it in every run. No voltage conversion (energy ratio).
  Records: idd.vpp_act; code: pimid_energy.h vppActRatioPJ (refused together with an ipp.ipp0 row).
- HBM3 activate+precharge per 1 KB: 0.39105 (VDD) x 1.103041 = 0.43134 nJ (VU37P column: 0.41523).
  HBM2: 0.3737 x 1.061825 = 0.3968 nJ. Guards printed per run (idd.act_guard_nj_per_kb): lower 0.34
  (IDD route, Chatterjee HPCA17 Table II), XPE attribution ceiling 0.50, upper 0.92 (O'Connor MICRO17 /
  Chatterjee bottom-up).
- idd0_loop: ONE_PC_PER_TRC. The record idd0 holds ONE 1 KB pseudo-channel activation per tRC (the
  SAFARI loop ran on PC0 only). The JEDEC IDD0 loops (JESD235D Table 60 p.103; JESD238B.01 Table 85
  p.154) activate PC0 AND PC1 every nRC; a vendor IDD0 must be entered as
  baseline + (IDD0 - baseline)/2. The loader refuses an HBM record without this label. The stale
  "JESD238B.01 Table 83 loops" wording of the HBM3 source string is corrected.

### 1.9 Per-stack floor halved; XPE background guard (PIMID 1.12.18)

- What the 783.3 mA floor is: the SAFARI artifact reads ONE current, HBM_1V2_I (platform.cpp L48-54, read
  L1443-1445; idd = Current_Avg). DS923: "VCC_IO_HBM must be connected to VCC_HBM", so the sensor is DRAM
  VDD + VDDQ of BOTH Alveo U55C stacks plus the FPGA HBM PHY I/O. The IDD bitstreams drive one stack
  (DRAMBender README L9-13: chip0 = stack 0 / channels 0-7). The floor comes from XCU55_no_hbm.bit, IDD2 on
  channel 0 only (extract_no_hbm.py), 821.3 - 38.0 = 783.3 mA. It is not self-refresh and not power-down.
- XPE like-for-like check on that rail (8G column, 1.2 Gb/s, 55 C): leakage of two stacks + FPGA PHY
  433 typ / 699 max mW; stack 0 clocked idle VDDQ 646 + VDD 197 mW; predicted IDD2 rail 1277 mW vs measured
  1087.4 mA x 1.2 V = 1305 mW (1.02x).
- Rule: per-stack floor = measured floor / 2, DERIVED. The even split is an assumption; XPE Static_Data
  rows 720/721 give equal DRAM leakage per stack.
  - HBM2 stack_floor_mw 939.96 -> 469.98 (783.3 mA x 1.2 V / 2).
  - HBM3 stack_floor_mw 861.63 -> 430.82 (783.3 mA x 1.1 V / 2).
  - The per-channel IDD columns are unchanged: they are the driven stack's measured increments over the whole
    783.3 mA.
- Derived values that move:
  - HBM3 datasheet background per stack: 2551.2 -> 2120.4 mW (16 x 89 mA x 1.1 V + refresh 123.2 + floor).
  - HBM3 read-energy guard "stack floor" term: 0.13 -> 0.07 pJ/b (total 4.07 -> ~4.0).
  - HBM2 standby per stack: 1305 -> 835 mW (+ refresh).
- Guard (printed, never priced; record idd.background_guard_mw_stack):
  - HBM3 [1060, 2170] mW per stack. XPE 2023.1.2 HBM_Data B29/B31, B37, B43 (8G) + Static_Data rows 720-722
    leakage, 55 C typ..max, voltage-scaled (1.1/1.2)^2 core and (1.8/2.5)^2 VPP. The low end is XPE's maximum
    rate (1.8 Gb/s); the high end is XPE's linear rate law extrapolated to 6.4 Gb/s.
  - HBM2 [860, 1900] mW (XPE 2023.1.2, all DRAM rails idle per stack).

### 1.10 HBM2 standby scaled to the record's 2.4 Gb/s preset (PIMID 1.12.18)

- Gap: the HBM2 row priced SAFARI's per-channel standby measured at 1.2 Gb/s (tCK 1666.7 ps) on the
  HBM2_2.4Gbps preset (tCK 833 ps). Its background (847 mW per stack) was below the XPE guard [860, 1900].
- Rule, as in the HBM3 derivation (sec. 1.3): the clocked standby is linear in rate (factor 2400/1200 = 2),
  voltage unchanged (1.2 V); DERIVED (provenance MEASURED -> DERIVED).
  - IDD2N 38.0125 -> 76.025; IDD3N 35.5 -> 71.0 mA per channel.
  - IDD0, IDD4R, IDD4W and IDD5 move with their baselines, so the activate, burst and refresh excesses are
    unchanged.
  - IDD0 42.725 -> 79.004741: shifted by the tRC-weighted baseline change at the preset tRC 48.314 / tRAS
    33.32 ns, so ACT stays 0.3968 nJ.
  - IDD4R 1356.9 -> 1392.4; IDD4W 924.15 -> 959.65; IDD5 92.7758 -> 130.7883.
  - IDD2P (un-sourced 7 mA) and the stack floor (469.98 mW) are unchanged.
- Read burst: already at the preset rate. The record's IDD4R excess is (3710.1 - 1067.3)/8 x 4 = 1321.4 mA:
  x4 = both PCs x 2.4/1.2 Gb/s, i.e. the measured 5.16 pJ/b per bit, constant. The gap noted in sec. 1.1 for the burst was closed by that factor; nothing more
  to scale.
- Result: HBM2 background per stack 847.4 -> 1189.8 mW. Guard [860, 1900] inside. XPE 4G at 2.4 Gb/s
  (own rate law, 25 C typ, all DRAM rails) about 1110 mW -> 1.07x.


### 1.11 No per-stack floor; the board offset is 821.3 mA (PIMID (1.12.46), user ruling 2026-10-25)

- Source: A. Olgun et al., "Understanding the Power Consumption of Modern High Bandwidth Memory: Experimental Analysis
  and Modeling Using Real HBM2 DRAM Chips", MICRO 2026 (extended: arXiv:2610.14244), Sec. 4.3: the current sensor is
  calibrated by averaging it "while all HBM2 chips are powered off (power-off current)", and that current is
  subtracted from every loop. The authors' model repository (github.com/CMU-SAFARI/HBM-Power, main branch,
  config/HBM2_1200MTs/power.json and docs/explanation/idd-derivation.md) names it "the FPGA board's power-off current
  (821.3 mA)", "the mean over the 18 valid boards", and calls the result "DRAM device only".
- So the 821.3 mA of no_hbm_idd2_measurements.csv is not "channel 0 running" (sec. 1.1) and not DRAM: the 783.3 mA
  "stack baseline" (821.3 - 38.0) and the per-stack floor built on it (sec. 1.9) are removed.
- Rule: per-channel quantity = (stack mean - 821.3 mA) / 8 (the loops drive one pseudo-channel in each of the 8
  channels; the authors spread standby over all 16 pseudo-channels, the same per-channel figure).
  - HBM2, at 1.2 Gb/s: IDD2N 38.0125 -> 33.2625, IDD3N 35.5 -> 30.75, IDD0 42.725 -> 37.975 mA; at the 2.4 Gb/s preset
    (sec. 1.10, x2): IDD2N 76.025 -> 66.525, IDD3N 71.0 -> 61.5; IDD0, IDD4R, IDD4W, IDD5 move with the baseline by
    -9.5 mA (69.504741, 1382.9, 950.15, 121.288255); IDD2P (x 28/40) 53.2175 -> 46.5675. The activate, burst and
    refresh excesses are unchanged (a uniform baseline shift cancels in the vendor method because tRC = tRAS + tRP).
  - HBM3: the clocked HBM2 standby it scales (sec. 1.3) is 33.26 mA, not (1087.4 - 821.3)/7 = 38.0, so the centre
    89.0 becomes 89.0 x 7/8 = 77.875 mA; IDD0, IDD4R, IDD4W, IDD5 shift by -11.125 mA (85.075, 1429.975, 1007.275,
    182.875); IDD2P 62.3 -> 54.5125. Excesses unchanged.
  - stack_floor_mw removed from both records (was 469.98 / 430.82 mW per stack).
- Derived values that move (datasheet duty, 358 K): HBM2 background per stack 1189.8 -> about 674 mW (now below
  the XPE guard [860, 1900], which is printed, never priced); HBM3 2120.4 -> about 1494 mW (XPE guard [1060, 2170]
  inside). The measured idle chip means (217.5-364.8 mA x 1.2 V at 1.2 Gb/s, x2 for 2.4 Gb/s, + refresh) give
  556-922 mW per HBM2 stack.

### 1.12 Standby VPP as a ratio of the VDD standby (PIMID (1.12.142), user ruling 2026-10-28)

- The measured base is VDDC + VDDQ only (sec. 1.8: the artifact's "ipp" column is a card residual, not a VPP
  sensor), and no JEDEC or vendor HBM IPP2N/IPP3N exists (NO PUBLISHED SOURCE FOUND). The standby VPP is therefore
  priced as the activation VPP is (sec. 1.8): a vendor-estimator RATIO, DERIVED.
- Xilinx XPE 2023.1.2 HBM_Data row 43 'Idle VPP Dynamic' (rail VCCAUX_HBM 2.5 V) / row 37 'idle VDD dynamic'
  (rail VCC_HBM 1.2 V), both mW per stack per Gb/s/pin, always on while the stack is clocked: 4G column (VU37P)
  8.2875/96.8 = 0.085615; 8G column (VU47P) 16.575/164.56 = 0.100723. Both rows scale with the rate the same way,
  so the ratio is rate-independent. Device match by stack height, as sec. 1.8: HBM3 (8-high) -> 0.100723; HBM2
  (4-high) -> 0.085615; the other column is printed in every run.
- Applied to the record's VDD standby power of each unit (per channel), on the VDD background's state split
  (IDD3N while a bank is open, IDD2N otherwise), at the run's temperature. No voltage conversion (energy ratio);
  XPE's own (V/Vnom)^2 rail scaling would change it by (1.8/2.5)^2/(1.1/1.2)^2 = 0.62 for HBM3 -- stated, not
  applied. XPE's VCCAUX_HBM leakage rows (Static_Data row 722) are not applied.
- No refresh, burst or activation VPP comes from this ratio (no IPP5B/IPP4R/IPP4W/IPP0 in the records), so
  nothing is counted twice. Records: idd.vpp_standby; code: pimid_energy.h ippUnitMW; switch
  power.models.dram_vpp_standby_ratio.
- Effect at 350 K: +67.2 mW per HBM2 stack, +154.1 mW per HBM3 stack. Background guard per stack (printed, never
  priced): HBM2 814.9 -> 884.5 mW (now inside XPE [860, 1900]); HBM3 1776.7 -> 1930.8 mW (inside [1060, 2170]).

---------------------------------------------------------------------------

## 2. Derivation 2 -- GDDR6

### 2.1 Search for a scaling base

- JESD250D is held. Its IDD table, Table 65
  (printed p.153 / PDF p.165), has conditions only and no values, like
  the HBM standards.
- Micron's public GDDR6 material has no IDD table.
  The Micron GDDR6X PAM4 technical brief URL returned an access-denied
  page and the Mouser TN-ED-03 fetch timed out, so neither is cited.
- No public GDDR5/GDDR5X IDD datasheet was pursued, because a GDDR6 part
  datasheet with a full IDD table is already held, and it is strictly a
  better base than any scaled GDDR5 table.
- **The held table:** Samsung K4Z80325BC, "8Gb GDDR6 SGRAM C-die, 16M x
  16Bit x 16 Banks x 2ch.", Rev. 1.3, Jan. 2020.
  - Page 1 carries Samsung's standard "reserves the right to change ..."
    notice, and no page carries a confidentiality marking (all 187
    scanned).
  - Distributor listings for this part turned up in web search. A public
    copy was NOT downloaded (the LCSC URL returned HTML), so public
    availability is by listing only.
  - No other GDDR6 vendor source was used.

### 2.2 The table (Samsung K4Z80325BC Table 82, PDF p.146)

Table 82 heading: "IDD Specifications and Test Conditions @
VDD&VDDQ=1.35V+0.0405V for 12/14/16/18Gbps", x32 mode, (0C <= Tc <= 85C).

| symbol | HC12 | HC14 (used) | HC16 |
|---|---|---|---|
| IDD0 | 404 | 430 | 452 |
| IDD1 | 485 | 520 | 545 |
| IDD2P | 210 | 230 | 248 |
| IDD2N | 299 | 310 | 327 |
| IDD3N | 422 | 460 | 477 |
| IDD4R | 1083 | 1220 | 1292 |
| IDD4W | 1242 | 1470 | 1511 |
| IDD5 (at tRFCab min) | 723 | 790 | 830 |
| IDD7 | 1390 | 1640 | 1679 |

VPP-rail currents at HC14: IPP0 14, IPP3N 10, IPP5 77, IPP7 31 mA
(VPP, not in IDD).

- Table 91, "AC parameter set for IDD test" (PDF p.155): tCK 0.57 ns,
  tRAS 30, tRP 15, tRC 45, tRFCab 120 ns. This equals the tree preset
  (nRC 79 x 0.571 = 45.1, nRAS 53 x 0.571 = 30.3) and IDDSpec trfc 120.
- The 1.25 V column (Table 84, HC14) is 350/250/380/990/1180/650/190.
  The x16 clamshell column (Table 83, HC14) is 357/276/362/817/1197/703/
  200. Neither is simulated.
- Conditions, verbatim (p.144 / JESD250D cl.8.8 printed p.152):
  - "Measurements are taken per device with the same IDD
    Measurement-Loop Patterns on both channels"
  - "IDD currents are measured as time-averaged currents, with all VDD
    balls of the device under test tied together. IPP currents are not
    included in IDD currents."
  - "All ODTs are enabled with ZQ/2 for Data, CA and WCK inputs"
- IDD5 is specified at tRFCab = tRFCab(min) back to back (Table 65
  wording), i.e. the burst refresh current the tree's idd5 column means.

**Basis.** These are per DEVICE (2 x 16-bit channels) maximum-spec
currents. This decides how the activation term is charged:
- **Burst: basis-neutral.** Per device, 760 mA excess over IDD3N moves
  32 b x 14 Gb/s = 448 Gb/s, which is 1.35 x 0.760 / 448e9 = 2.29 pJ/bit.
  The tree's held charge (1 device, 1 BL16 burst of 1.143 ns = 64 B)
  gives 1.35 x 760 x 1.143 = 1173 pJ per 64 B, the same 2.29 pJ/bit.
  Per-channel basis (half current, 2 bursts) gives the same product.
- **Activation: NOT basis-neutral.** The device IDD0 loop activates one
  bank in EACH channel per tRC, so a per-device row prices two
  activations per miss. The per-channel charge is half the device
  figure.

### 2.3 Terms

| term | base value (source) | factor(s) | derived value | band |
|---|---|---|---|---|
| vdd | 1.35 V (T82 heading; JESD250D) | none | 1.35 | 1.35-1.3905 (T82 "+0.0405V") |
| IDD0 | 430 mA (T82 HC14) | none; per device | 430 | 404-452 (HC12..HC16) |
| IDD2N | 310 mA (T82) | none | 310 | 299-327 |
| IDD3N | 460 mA (T82), ONE bank active (T82; JESD250D T65) | none | 460 | 422-477 |
| IDD4R | 1220 mA (T82) | none | 1220 | 1083-1292 |
| IDD4W | 1470 mA (T82) | none | 1470 | 1242-1511 |
| IDD5B | 790 mA (T82 IDD5, tRFCab = 120 ns T91) | none | 790 | 723-830 |
| IDD2P | 230 mA (T82, PLL/DLL off) | none | 230 | 210-248 |

**Band caveat, one-sided.** All values are specification maxima over
0-85 C and process. Typical silicon is lower by an amount no held or
fetched source states, so there is no citable lower edge. The
HC12..HC16 spread is shown as the rate sensitivity.

**Derived energies at the recommended row, and what they depend on:**
- **Burst:** 2.29 pJ/bit read, 3.04 pJ/bit write, on VDD. Termination is
  priced separately by terminationNJ. POD scheme-table value: read
  0.5 x 1.35^2 / (40 + 60) / 14 Gb/s = 0.651 pJ/bit; write
  0.5 x 1.35^2 / (40 + 120) / 14 Gb/s = 0.407 pJ/bit. The CACTI-IO
  injected figure the run prints is computed at runtime from the same
  electricals (cacti_io_wrapper.cpp:279) and was not evaluated here (no
  runs).
- **Activation, vendor method with IDD3N as ONE bank (no 1.11.86 division):**
  1.35 x (430 x 45 - 460 x 30 - 310 x 15) = 1215 pJ per device loop,
  i.e. **608 pJ per channel ACT** of a 2 KB page.
  - This term is ILL-CONDITIONED: it is a 900 pA.ns difference of
    ~19000 pA.ns terms. A +-5% error in IDD0 alone moves it from -46 pJ
    to 1261 pJ per channel ACT.
  - The well-conditioned alternative is (IDD7 - IDD4R): the IDD7 loop
    adds one ACT per channel every 20 nCK = 11.4 ns (Table 90) to the
    same read stream. That gives 1.35 x 420 mA / (2 x 87.6 M/s) =
    **3.24 nJ per ACT**, 5.3x the IDD0 route.
  - Band for the activate energy: 0.6-3.2 nJ per channel ACT. The two
    routes disagree and no held source resolves which is right.
- **With the 1.11.86 division by 16 banks (current code):** 6910 pJ per
  device loop. That is 5.7x the one-bank value and above even the IDD7
  route per activation pair. This is the one-bank-basis consequence for GDDR6.
- **Refresh:** 1.35 x (790 - 310) x 120 = 77.8 nJ per device REFab,
  40.9 mW at tREFI 1.9 us.

### 2.4 The energy-per-bit route (cross-check only)

Quote: Tom's Hardware (A. Shilov, 2020-09-06),
https://www.tomshardware.com/news/micron-reveals-gddr6x-details-the-future-of-memory-or-a-proprietary-dram
(fetched): "GDDR6X is 15% more power-efficient than GDDR6 (7.25 pj/bit
vs 7.5 pj/bit) at the device level, according to Micron."

This is secondary reporting of a Micron statement. The primary Micron
brief could not be fetched. Neither data rate nor access pattern nor
what "device level" includes is stated, and the quoted pair is
internally inconsistent: 7.25 vs 7.5 is 3%, not 15%.

Following that route anyway:
1. Subtract the tree's read termination: 7.5 - 0.651 = 6.85 pJ/bit.
2. If all of that were burst: IDD4R - IDD3N = 6.85e-12 x 448e9 / 1.35 =
   2273 mA. That is 3.0x the datasheet's 760 mA, a datasheet MAXIMUM.

So 7.5 pJ/bit cannot be a burst-only VDD figure for this class of part.
It must include activation at a high miss rate, background, VPP and/or
PHY. Any split into burst/activate shares would be a free parameter
chosen to land on the datasheet. That is why this route is recorded, not
used.

**Cross-check the other way (the datasheet row through the tree's
formula, per-channel activation 608 pJ, 2 KB page):**

| miss fraction | read pJ/bit | with IDD7-route activation |
|---|---|---|
| 1/32 | 2.98 | 3.14 |
| 1/2 | 3.53 | 6.10 |
| 1 | 4.13 | 9.26 |

Background at full bandwidth adds IDD3N x 1.35 / 448 Gb/s = 1.39
pJ/bit, and VPP adds a few percent. At high miss rates the model spans
Micron's 7.5 only with the IDD7-route activation, which is one more
reason the activation band is 0.6-3.2 nJ rather than a point.

### 2.5 Assumptions (GDDR6)

- G1. The simulated part is the Samsung K4Z80325BC class: 8 Gb,
  14 Gb/s, 1.35 V, x32 device mode = 2 x16 channels. This matches org
  GDDR6_8Gb_x16 and timing GDDR6_2000_1350mV_double.
- G2. IDD is the VDD rail only (JESD250D "all VDD balls"); VDDQ current
  is not in IDD. The DC termination is priced by terminationNJ at the
  same ODT setting the IDD conditions use (ZQ/2 = 120 ohm). Capacitive
  VDDQ switching is in neither layer (stated boundary).
- G3. IPP (VPP 1.8 V) is excluded from the row. It is about 2-4% of VDD
  power at IPP0/IPP3N and about 13% during refresh (IPP5 77 mA x 1.8 V
  against 790 x 1.35).
- G4. The row is per DEVICE, and only the activation term needs a /2
  when charged per channel access (the per-channel activation rule).

**Recommended row (per device):**

    if (tech == "GDDR6") return {1.35, 430,310,460,1220,1470,790, 120.0, 1900.0, 1, 230};

It must ship with two coupled changes, or the activate term is wrong:
- the IDD3N basis: banks_per_device = 1 for GDDR6, because IDD3N is one bank;
- the activation rule: the activation charge is per channel = half the device figure.

**Numbers-moving:**
- burst excess 150 -> 760 mA (5.1x read); 170 -> 1010 mA (5.9x write)
- background IDD2N/IDD3N 45/60 -> 310/460 (6.9x/7.7x)
- refresh excess 120 -> 330 mA over IDD3N
- idd2p 30 -> 230

---------------------------------------------------------------------------

## 3. The IDD3N basis, verbatim

- JESD235D Table 57 (printed p.101, PDF p.109): "Active Standby Current:
  tCK = tCK(min); one bank is active; CKE is HIGH; R and C inputs are
  HIGH; DQ, DM and DBI inputs are LOW -- IDD3N". The same table, IDD4R:
  "all banks activated".
- JESD238B.01 Table 83 (printed p.151, PDF p.165): "Active Standby
  Current: tCK = tCK(min); one bank is active; R and C inputs are HIGH;
  DQ, ECC and DBI inputs are LOW -- IDD3N, IDDQ3N, IPP3N, IDDQL3N".
  IDD4R (printed p.152): "all banks activated".
- JESD250D Table 65 (printed p.153, PDF p.165): "Active Standby Current:
  tCK = tCK (min); tWCK = tWCK(min); one bank active; CKE_n = LOW; all
  other inputs are HIGH -- IDD3N". IDD4R: "one bank in each of the 4
  bank groups activated". Samsung K4Z80325BC Table 82 carries the same
  text.
- Measured HBM2 (SAFARI): IDD3N1 133.4 and IDD3N16 135.3 mA/channel, so
  (IDD3N16 - IDD3N1)/15 = 0.12 mA per extra open bank.

**Consequence.** For HBM2/HBM3/GDDR6, oneBankActiveStandbyMA() should
return IDD3N unchanged (banks_per_device = 1), not
IDD2N + (IDD3N - IDD2N)/banks. For HBM this is about 0.1 mA and inert.
For GDDR6 at the datasheet row it is 460 vs 319 mA in the subtraction,
worth 5.7x on the activate energy.

---------------------------------------------------------------------------

## 4. Sources and fetch status

Held and read by page range:
- JESD235D: Table 49 (PDF p.104), cl.9.1 and Tables 57/58 (PDF pp.108-110)
- JESD238B.01: Tables 1, 3, 4 (PDF pp.17-20), Table 70 (PDF p.152),
  cl.9.1 and Tables 83/84/90 (PDF pp.164-167, 173)
- JESD250D: cl.8.8 and Table 65 (PDF pp.164-167)
- Samsung K4Z80325BC (PDF pp.1, 144-155)
- O'Connor MICRO 2017
- the SAFARI CSVs (local archive)

Fetched on 2026-09-26:
- github.com/CMU-SAFARI/HBM-Power (tree API; README; the raw files named
  in 1.1)
- the IEDM 2023 SK hynix handout
- arXiv:2602.18568
- the Tom's Hardware article
- the utexas.edu O'Connor PDF

Search-only, NOT cited: wevolver.com secondary pJ/bit claims (2.5 pJ/bit
HBM3); the jonpeddie/nasdaq Micron pages.

Fetch failed: the Micron GDDR6X brief (access denied), Mouser TN-ED-03
(timeout), the LCSC Samsung datasheet mirror (HTML returned).
