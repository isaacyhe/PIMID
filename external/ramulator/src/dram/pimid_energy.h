#ifndef RAMULATOR_DRAM_PIMID_ENERGY_H
#define RAMULATOR_DRAM_PIMID_ENERGY_H
// ---------------------------------------------------------------------------
// PIMID intensive DRAM energy layer -- RELOCATED into Ramulator2 (1.9.10).
//
// This is the single source of the per-access (intensive) DRAM energy the PIMID
// power path consumes. It lives inside external/ramulator so the physics belongs
// to Ramulator2, not the PIMID wrapper; the wrapper (ramulator_wrapper.cpp) is now
// a thin reader that forwards its timing getters into these functions.
//
// The per-tech IDD/VDD tables here are PART-NUMBER-SOURCED datasheet values
// (Micron part classes named below) -- WITH ONE EXCEPTION, the idd2p column.
// 1.11.56 (audit D006): that blanket claim covered a column that was never
// read off a datasheet. idd2p is a precharge-POWER-DOWN current; the DDR/LPDDR
// classes above publish IDD2P, but the HBM rows were entered as "30-40% of
// IDD2N per JESD precharge-standby-powerdown deltas" -- a rule of thumb, not a
// part number -- and HBM2's own entry (17 -> 7) is 41.2%, outside the band its
// comment states. The claim is narrowed here, and the consumer says so at the
// point of use (RamulatorWrapper::getBackgroundSystemMW) rather than leaving a
// header comment to carry the disclosure. The other columns are unaffected.
// 1.11.46 (FIX-PRE-FLEET L189): the claim
// that they "mirror the per-impl current_presets" was FALSE and is withdrawn:
// measured against the tree, DDR4.cpp's Default preset is {60,50,55,145,145,
// IDD5B 362} vs this table's {58,35,42,140,150, IDD5 155} -- the divergence
// lands on the IDD4W write term 1.11.5 introduced, and the upstream DDR5
// preset is a byte-identical copy of DDR4's (generic, not a DDR5 part). The
// upstream presets are unlabelled defaults for the command-driven extensive
// model (drampower_enable, off in our runs); THIS table is the authoritative
// intensive source, and IDD5 here is the average-refresh current, not
// upstream's IDD5B burst figure -- different definitions, not a typo. Ramulator2's own command-driven
// power model (update_powers()/s_total_*energy, gated by drampower_enable) produces
// EXTENSIVE totals over a full simulation; this header produces the INTENSIVE
// per-64B-access / per-device values used for analytical energy accounting. Both
// draw from the same datasheet numbers.
//
// Formulas: Micron TN-41-01 (row ACT+PRE and read-burst energy); ODT/termination =
// resistor-network dissipation VDDQ^2/Rtt over the bit period, per I/O standard.
// ---------------------------------------------------------------------------
#include <string>
#include <set>
#include <cstdlib>
#include <iostream>

namespace Ramulator {
namespace pimid_energy {

/* 1.11.57 (latent D007): SAY IT WHEN A STRING FALLS OFF THE TABLE.
 *
 * Three places in this file answer an unrecognised technology with the DDR4
 * class -- iddFor()'s trailing return, terminationNJ()'s trailing else, and
 * devicesPerAccess()/backgroundUnits()' trailing return -- and they do not
 * even agree with each other about what "unrecognised" means: iddFor matches
 * "HBM2"/"HBM3" exactly while terminationNJ and backgroundUnits match the
 * three-character prefix "HBM", so a string like "HBM2E" would be given DDR4
 * currents, zero termination and an HBM channel population, all at once. The
 * defect is invisible today only because main.cpp hard-whitelists the eleven
 * technology names and exits on anything else, so no such string can reach
 * here; the day a technology is added to that whitelist without a row here,
 * it would be priced as DDR4 in silence. Refuse to be silent instead: each
 * fallback announces itself once, naming the string and the quantity it
 * governs, so the missing row is reported rather than substituted. */
inline void announceUnknownTech(const char* fn, const std::string& tech,
                                const char* governs) {
    /* Once per (function, technology): these sit on the per-access energy
     * path, so an unguarded message would print millions of times and become
     * noise instead of a disclosure. */
    static std::set<std::string> announced;
    if (!announced.insert(std::string(fn) + "/" + tech).second) return;
    std::cerr << "[power] WARNING: pimid_energy::" << fn
              << " has no row for memory technology '" << tech
              << "' and is falling back to the DDR4 class, which governs "
              << governs << ". This is a SUBSTITUTED value, not a sourced one"
                 " -- add the technology's row to pimid_energy.h rather than"
                 " trusting this number." << std::endl;
}

/* 1.11.57 (latent D075): ONE chips-per-rank table in this file, not two.
 *
 * devicesPerAccess() and backgroundUnits() each carried their own copy of the
 * JEDEC device-width population (x4 -> 16, x8 -> 8, x16 -> 4). They agreed, so
 * nothing could differ today -- which is exactly the shape of the worst defect
 * this audit found elsewhere: a duplicated table that drifted from the object
 * it was meant to mirror. One of the two is the array-energy basis and the
 * other is the background-power basis; a divergence would have made a single
 * memory system draw standby current for one population while bursting from
 * another. There is a THIRD copy, memorySystemDieCount() in src/main.cpp,
 * which owns the AREA basis; it is outside this file and still separate. */
inline int devicesPerRank(const std::string& device_width) {
    if (device_width == "x4")  return 16;
    if (device_width == "x16") return 4;
    return 8;                       // x8, the default 64-bit rank
}

struct IDDSpec {
    double vdd;                                    // V
    double idd0, idd2n, idd3n, idd4r, idd4w, idd5; // mA (per device / per channel)
    double trfc_ns, trefi_ns;
    int    channels;                               // per-stack aggregation (HBM)
    /* 1.11.8 (#84): precharge power-down current (CKE low, fast tXP exit) --
     * the JEDEC descent state an idle controller actually enters. Same
     * datasheet classes as the columns above. */
    double idd2p;                                  // mA
};

// Per-tech datasheet/JESD IDD-class values (part-number classes in comments; see
// the assumptions register).
/* 1.11.57 (latent D001): the line that used to close the sentence above --
 * "Mirrors the per-impl current_presets/voltage_presets" -- is GONE, because
 * it was withdrawn 25 lines higher up in this same header by the 1.11.46 note
 * and never removed from the place a reader actually quotes. The two
 * statements contradicted each other inside one file, and the false one came
 * first. It is false on the very first row: DDR4.cpp's Default preset in this
 * tree is {60,50,55,145,145, IDD5B 362} against this table's {58,35,42,140,
 * 150, IDD5 155}. Nothing printed the claim, which is why it survived a
 * release that explicitly retracted it; the retraction was reachable only by
 * reading the file top to bottom. This table is the authoritative INTENSIVE
 * per-access source and does NOT mirror the upstream command-driven presets.
 */
//   DDR5  Micron 16Gb DDR5-4800 (VDD 1.1);  DDR4 Micron 8Gb DDR4-2400 (1.2);
//   DDR3  Micron 4Gb DDR3L-1600 (1.35);     GDDR6 Micron 8Gb GDDR6-14000 (1.35);
//   HBM2  JESD235 (1.2);                    HBM3 JESD238 (1.1);
//   LPDDR5 Micron 8Gb Automotive LPDDR5-6400, MT62F512M32D2/MT62F1G32D4
//          (315b Rev.D, VDD2H 1.05) -- 1.11.63; was "Micron 12Gb LPDDR5-6400",
//          a part this tree does not hold at that speed grade (the held 12 Gb
//          Y4BM is graded 7500/8533 Mb/s). This is the only row whose named
//          part is HELD in misc/ and re-read cell by cell; the other six names
//          remain unverifiable here (no such datasheet is in the tree).
/* 1.11.57 (latent F041): THE DIE DENSITIES ABOVE ARE THE IDD PART CLASSES, NOT
 * THE SIMULATED ORGANISATION. src/main.cpp emits DDR5_8Gb_x8 and DDR3_8Gb_x8,
 * against the 16 Gb and 4 Gb classes named on those two rows, so a reader who
 * takes this list as the machine's configuration gets the wrong die. It is
 * kept as a PROVENANCE list -- these are the datasheets the currents were read
 * from, which is what a current column needs -- and the mismatch is stated
 * rather than papered over by editing the densities to match, which would
 * misattribute the numbers. The substantive half of the same divergence, the
 * timing layer and the energy layer describing different parts, is carried
 * numerically by the trfc_ns/trefi_ns columns and is not a comment question. */
/* 1.11.65: TEMPERATURE-DEPENDENT REFRESH. Every DRAM family halves tREFI
 * (doubles refresh rate) above 85 C; HBM goes further. The model had been
 * temperature-FLAT: config.temperature_k reached McPAT, CACTI and NVSim but
 * never the DRAM refresh duty, so a 105 C run priced the same refresh power
 * as a 45 C one. The multiplier below is what the standards and vendor
 * controller guides specify, applied to tREFI (duty = tRFC / tREFI).
 *
 * SOURCES (all in misc/):
 *  DDR3/DDR4/DDR5, LPDDR5, GDDR6 -- one step: 2x refresh above 85 C.
 *    JESD79-5D Table 70 printed p.173: "tREFI1, 0 C <= TCASE <= 85 C, 3.9 us"
 *      and the 85 < TCASE <= 95 C row at 1.95 us (tREFI halves).
 *    JESD79-3E / Micron MT41K MT40A: "2x refresh rate" required above 85 C
 *      (the extended-temperature range, ASR/SRT).
 *    JESD209-5C Table 240 NOTE 2 p.287: 1x refresh rate at or below 85 C;
 *      the MR4 temperature-derating ladder above it (0.5x tREFI at
 *      85-105 C in the standard's MR4 table).
 *    JESD250D GDDR6: 2x refresh above 85 C via the temperature sensor
 *      readout (MR7 TS / refresh rate), same shape.
 *  HBM2/HBM3 -- a LADDER, not one step, keyed by TEMP[2:0]:
 *    Intel UG-20031 (Stratix 10 MX HBM2 IP) Table 30 p.66 and Intel Agilex
 *      7 M-series HBM2E IP UG (doc 773264) Table 5 p.17, identical:
 *      TEMP 000 -> 4 x tREFI (refresh 4x slower), 001 -> 2 x, 011 -> 1 x
 *      (nominal), 010 -> 0.5 x, 110 -> 0.25 x tREFI.
 *    AMD DS923 v1.20 p.5 note 16: "While operating the HBM above 95 C, the
 *      refresh rate must be at least 4x the refresh rate at 95 C."
 *    AMD PG276 v1.0 p.23: tREFI 3.9 us at 0-85 C, 1.95 us at 85-95 C.
 *    Consistent reading of the three: nominal to 85 C; 0.5 x tREFI at
 *      85-95 C; 0.25 x at 95-105 C; the 4x-slower and 2x-slower rungs are
 *      the cold end (below ~45 C / ~65 C, vendor-specific) which this model
 *      does NOT credit -- taking a refresh-power DISCOUNT for cold operation
 *      on a vendor-specific threshold would be inventing a benefit; the
 *      penalties above 85 C are what every source agrees on.
 *  Above 105 C: no source specifies a rate (HBM CATTRIP at 120 C, PG313
 *      p.98; DDR extended range ends at 95 C). The last rung is held and the
 *      consumer says so.
 *
 * Returns the factor to MULTIPLY tREFI by (< 1 = more frequent refresh). */
inline double refreshTempFactor(const std::string& tech, int temperature_k) {
    const double t_c = temperature_k - 273.15;
    const bool hbm = (tech.substr(0, 3) == "HBM");
    if (t_c <= 85.0) return 1.0;                 // nominal, every family
    if (!hbm)        return 0.5;                 // DDR/LPDDR/GDDR: one step
    if (t_c <= 95.0) return 0.5;                 // HBM 85-95 C  (PG276, UG-20031 010)
    return 0.25;                                 // HBM > 95 C   (DS923 n.16, UG-20031 110)
}

inline IDDSpec iddTableFor(const std::string& tech) {
    /* idd2p (last column). 1.11.56 (audit D006): this column is APPROXIMATE
     * and is the one column in the table that is not a datasheet read. The
     * DDR/LPDDR/GDDR entries are rounded IDD2P fast-exit figures for the part
     * classes named above (DDR5-4800 ~20 mA, DDR4-2400 ~25, DDR3L ~18,
     * GDDR6 ~30); the HBM entries are
     * NOT read from a part at all, they were set as "~30-40% of IDD2N per JESD
     * precharge-standby-powerdown deltas", and HBM2's 17 -> 7 is 41.2%, i.e.
     * outside the band that sentence claims. The column is live: it is the
     * IDD2P baseline backgroundUnitMW() uses under pg_enabled, so it moves the
     * printed Background line whenever measured idle residency is non-zero.
     * Keep it, but do not quote it as sourced -- the consumer emits a note. */
    /* 1.11.63 (calibration): FOUR trfc_ns values and the whole LPDDR5 IDD
     * column. Each is tagged in place below; the reasoning, once:
     *
     * trfc_ns is the numerator of the refresh duty in stateWithRefreshMW() and
     * refreshMW(), so every one of these is live on the printed Background and
     * Refresh lines. It must describe the SAME part the timing layer refreshes
     * -- the org preset src/main.cpp emits -- not the part the IDD column came
     * from. Four of the seven rows described a different die than their own
     * technology's impl file did.
     *
     *   DDR3   260.0 -> 350.0  SOURCED. JESD79-3D Table 59 "Refresh parameters
     *          by device density", clause 12.2, printed p.156 (PDF p.170):
     *          tRFC = 90/110/160/300/350 ns at 512Mb/1/2/4/8 Gb. The simulated
     *          org is DDR3_8Gb_x8, so 350 ns. 260 was the 4 Gb column of
     *          Ramulator's own table -- and that column was itself wrong (300
     *          in Table 59); it is fixed in DDR3.cpp in the same release.
     *          DDR3 refresh was priced at 74% of the standard's occupancy.
     *
     *   HBM3   160.0 -> 260.0  SOURCED. JESD238B.01 Table 93, printed p.165
     *          (PDF p.179): tRFCab = 260 ns at 4 Gb/channel (both of its
     *          realisations, 8 Gb/die 8-High and 16 Gb/die 4-High). The
     *          simulated org is HBM3_4Gb. 160 ns appears at NO HBM3 density --
     *          the tabulated values are 260/310/350/410/450 and TBD -- and it
     *          contradicted HBM3.cpp's own tRFC table, which already had 260.
     *
     *   DDR5   295.0 -> 195.0  DERIVED-FROM-IDENTITY, not sourced. JESD79-5 is
     *          NOT held by this tree, so no external check is possible; but
     *          the tree's own DDR5.cpp tRFC_TABLE gives tRFC1 = 195 ns at 8 Gb
     *          and 295 ns at 16 Gb, and the emitted org is DDR5_8Gb_x8. The
     *          energy layer was refreshing a 16 Gb die while the timing layer
     *          refreshed an 8 Gb one; they cannot both be right, and the
     *          identity that settles it -- both layers must describe the org
     *          actually simulated -- needs no standard. The provenance line
     *          below still names the 16 Gb DDR5-4800 part the IDD CURRENTS
     *          were read from; that stays a stated mismatch (F041).
     *
     *   HBM2   160.0 -> 260.0  DERIVED-FROM-IDENTITY, not sourced. The JESD235
     *          family text is NOT held (misc/ carries only the HBM ballout
     *          spreadsheet), so no external check is possible; but HBM2.cpp's
     *          own tRFC_TABLE gives 260 ns at the emitted HBM2_4Gb density.
     *          The two layers of one technology disagreed by 1.6x.
     *
     * NOT touched, deliberately, and each for a stated reason:
     *   DDR4 350.0. The audit marks the whole DDR4 energy row UNCHECKABLE
     *     (JESD79-4 is not held) and does not flag this one, so it stays. For
     *     the record it is NOT identical to DDR4.cpp's tRFC1 at 8 Gb, which is
     *     360 ns -- a 2.8% gap, an order of magnitude smaller than the four
     *     mismatches fixed above, and 350 ns is a real figure for the Micron
     *     8 Gb DDR4-2400 class the IDD column came from. Flagged here so the
     *     next pass with JESD79-4 in hand can settle it rather than rediscover
     *     it.
     *   GDDR6 220.0. JESD250D publishes no tRFC at all (Table 73 printed p.162
     *     leaves tRFCab and tRFCpb blank) and no GDDR6 vendor sheet is held.
     *   every trefi_ns. All are already correct or UNCHECKABLE. The
     * LPDDR5 trefi 3904.0 disagrees with LPDDR5.cpp's tREFI_BASE 3906 by 2 ns;
     * both are UNCHECKABLE (deferred to General LPDDR5 Spec 3 / JESD209-5,
     * neither held) so neither is moved. */
    /* 1.11.66 (round 5, F6 -- user ruling R8 #9): THE DDR5 ROW FOLLOWS THE
     * SPEED GRADE, because an IDD row and a timing row must describe one
     * part. The held Micron MT60B 16 Gb die addenda (misc/) publish x8 IDD
     * limits at exactly two grades, and both are B-bin parts:
     *   Rev A (Rev. D 02/2023) Table 6 pp.17-19, DDR5-4800B:
     *     IDD0 103, IDD2N 92, IDD3N 142, IDD4R 377, IDD4W 349, IDD5B 277,
     *     IDD2P 88 mA; tRFC1(16 Gb) 295 ns (JESD79-5D Table 71 p.173)
     *   Rev D (Rev. F 04/2024) Table 8 pp.18-20, DDR5-5600B:
     *     IDD0 53, IDD2N 49, IDD3N 91, IDD4R 218, IDD4W 241, IDD5B 377,
     *     IDD2P 47 mA; tRFC1 295 ns
     * Both are "maximum values ... worst-case process, temperature and
     * voltage" at VDD = VDDQ = 1.1 V, and both list IDD5B (burst, all-bank)
     * directly -- no IDD5R conversion needed, unlike DDR4. The 3200 row
     * (8 Gb, DDR5_3200AN) keeps its previous currents with the gap STATED:
     * no held datasheet publishes a 3200 column, so that row's IDD is
     * unsourced (lane C F6 measured it 2-3x below either addendum on
     * standby). Default grade is 4800. The energy caller passes the grade;
     * the tRFC column is 195 ns at 8 Gb and 295 ns at 16 Gb. */
    if (tech == "DDR5-3200") return {1.1, 55, 34, 42,148,168,120, 195.0, 3900.0, 1, 20};   // 8 Gb, UNSOURCED IDD (stated)
    if (tech == "DDR5-5600") return {1.1, 53, 49, 91,218,241,377, 295.0, 3900.0, 1, 47};   // MT60B Rev D T8, 16 Gb
    if (tech == "DDR5" || tech == "DDR5-4800")
                          return {1.1,103, 92,142,377,349,277, 295.0, 3900.0, 1, 88};   // MT60B Rev A T6, 16 Gb (default)
    /* 1.11.66 (round 5, F5): idd5 155 -> 362 mA. The 155 had no source.
     * Micron MT40A (misc/) tabulates IDD5R -- the DISTRIBUTED refresh current,
     * one REF every tREFI averaged in (p.318 definition) -- not the burst
     * IDD5B this column means. Converting the same Rev A row the other
     * columns come from: IDD5B = IDD2N + (IDD5R - IDD2N) x tREFI / tRFC =
     * 50 + 14 / (350/7800) = 362 mA. That is EXACTLY the IDD5B upstream
     * Ramulator2 ships in DDR4.cpp's Default current preset, which is how
     * upstream derived it too -- an independent reproduction. Refresh
     * excess over IDD3N goes 113 -> 320 mA (2.8x); the printed refresh line
     * had been 2.7x low. */
    if (tech == "DDR4")   return {1.2, 58,35,42,140,150,362, 350.0, 7800.0, 1, 25};
    if (tech == "DDR3")   return {1.35,60,32,45,175,180,210, 350.0, 7800.0, 1, 18};
    /* 1.11.63 (calibration): the LPDDR5 IDD column is re-based on a HELD part
     * datasheet. Every one of the seven currents changed.
     * SOURCE: misc/Micron_LPDDR5_MT62F_datasheet.pdf -- Micron Automotive
     * LPDDR5 SDRAM, part numbers MT62F512M32D2 / MT62F1G32D4 (315b, Rev.D
     * 4/2021), 8 Gb die -- Table 7 "IDD Parameters - Single Die", pp.14-15,
     * the VDD2H supply column at 6400 Mb/s, AIT/AAT temperature grade:
     *     IDD02H   45.0 mA   (was 32)     IDD4W2H  310 mA   (was 120)
     *     IDD2N2H  30.0 mA   (was 18)     IDD52H   170 mA   (was  90)
     *     IDD3N2H  39.0 mA   (was 24)     IDD2P2H    2.5 mA (was   4)
     *     IDD4R2H  372  mA   (was 110)
     * Table 7's own note 2 reads "BG mode. DVFSC and DVFSQ disabled", which is
     * the organization LPDDR5.cpp models (4 bank groups x 4 banks), and note 1
     * that the values are maxima over process/temperature/voltage.
     * WHY THIS PART: it is the closest held document to what is simulated --
     * same 8 Gb die density as the emitted LPDDR5_8Gb_x16 org, same 6400 Mb/s
     * speed grade as the LPDDR5_6400 timing preset. The row's old provenance
     * (12 Gb LPDDR5-6400) is not reachable: the held 12 Gb part, Y4BM
     * (misc/Micron_05092023_...y4bm...pdf), is graded 7500/8533 Mb/s and has
     * no 6400 column at all. No held datasheet produced the old row -- its
     * standby trio sat within ~15% of the Y4BM part while its burst currents
     * were 3-4x below BOTH held parts.
     * MOVES NUMBERS: read/write burst power roughly triples (IDD4R 3.4x,
     * IDD4W 2.6x), activate 1.4x, refresh 1.9x, and precharge power-down drops
     * to 0.6x. The vdd 1.05 stays: it is VDD2H typ, printed on p.1 of every
     * held Micron LPDDR5/5X datasheet. STILL UNMODELLED and stated: LPDDR5 is
     * a three-rail part (VDD1 1.8 V, VDD2H 1.05 V, VDDQ 0.5 V) and only VDD2H
     * is priced here -- the VDD1 draw (IDD01 2.9 mA, IDD4R1 7.2 mA, ...) and
     * the VDDQ draw (IDD4RQ 106 mA) are not in this struct's shape. The
     * provenance line above is updated to name the part actually used. */
    if (tech == "LPDDR5") return {1.05,45,30,39,372,310,170, 210.0, 3906.0, 1,  2.5};   // 1.11.66 (L7): tREFI 3906 = JESD209-5C 3.906 us, = LPDDR5.cpp tREFI_BASE (was 3904)
    if (tech == "GDDR6")  return {1.35,70,45,60,210,230,180, 120.0, 1900.0, 1, 30};   // 1.11.66: tRFCab 120 (Samsung T92 / SK hynix T67), was 220
    if (tech == "HBM3")   return {1.1, 30,18,22, 90,100, 70, 260.0, 3900.0, 16, 7};
    /* 1.11.66 (round 5, F7 -- user ruling R8 #10): THE HBM2 IDD ROW IS
     * MEASURED SILICON. The row this replaces (28/17/21/80/90/65 mA) traced
     * to no vendor table -- HBM2 datasheets are NDA-only and JESD235D prints
     * its IDD value columns empty by construction (Table 65 p.105). The
     * CMU-SAFARI HBM-Power artifact (misc/zen_hbm2_idd_36chips_summary.csv,
     * 2026) measured the JEDEC IDD loop patterns on 36 real HBM2 stacks
     * (AMD Alveo U55C, DRAM Bender, ~800k samples) at the stack's primary
     * VDD rail. The conversion to this row's per-CHANNEL unit is JEDEC's
     * own: JESD235D cl. 9.1 printed p.100 -- "IDD and IPP measurements are
     * taken with all channels of the HBM device simultaneously executing
     * the same pattern. However, values in the vendor's datasheet shall be
     * given per channel." So stack mean / 8 channels IS the datasheet
     * quantity. Means across 36 chips, / 8:
     *   IDD0 1125.1 -> 141   IDD2N 1087.4 -> 136   IDD3N 1067.3 -> 133
     *   IDD4R 3710.1 -> 464  IDD4W 2844.6 -> 356   IDD5B 1514.5 -> 189
     * These are as-measured operating points (IDD4R at 67.6 C mean case),
     * not temperature-normalised minima, and the chip-to-chip spread is
     * 1.3x on the burst loops and 1.6-1.8x on standby -- the per-chip file
     * zen_hbm2_idd_per_chip.csv is the band any single number here sits in.
     * Note IDD3N (133) <= IDD2N (136) on silicon: the open-row standby
     * increment is inside measurement noise, as is IDD0 - IDD3N. The old
     * row's only term that agreed with silicon was the refresh increment
     * IDD5B - IDD3N (44 vs 46 mA). IDD2P was NOT measured; the 7 mA stays as
     * the one un-sourced column and is stated so. Consequence: HBM2
     * background ~6.5x, burst increments 3-5x -- the model had priced a
     * stack's standby at 0.20 W where silicon draws 1.37 W. */
    if (tech == "HBM2")   return {1.2, 141,136,133,464,356,189, 260.0, 3900.0, 8,  7};
    /* 1.11.57 (latent D007): unknown -> DDR4 class, and it says so. This
     * governs the array activate/precharge and burst energy, the background
     * standby power and the refresh line for the whole run. */
    announceUnknownTech("iddFor", tech,
                        "array energy, background standby and refresh power");
    return {1.2, 58,35,42,140,150,155, 350.0, 7800.0, 1, 25};  // unknown -> DDR4 class
}

/* 1.11.65: the IDD row AT A TEMPERATURE. The datasheet table above is the
 * nominal-range (<= 85 C) row; this applies refreshTempFactor() to tREFI so
 * every refresh-duty consumer (stateWithRefreshMW, refreshMW, backgroundMW,
 * backgroundUnitMW, backgroundSystemMW) sees the operating point's refresh
 * rate. The default of 358 K = 85 C is the top of the nominal range, i.e. a
 * caller that does not state a temperature gets exactly the old behaviour.
 * The IDD currents themselves are NOT temperature-scaled here: datasheets
 * specify IDD at a fixed case temperature and publish no derating curve for
 * the active currents; only the refresh RATE is normatively temperature-
 * dependent. */
/* 1.11.66 (R8 #9): an energy KEY may carry a speed-grade suffix --
 * "DDR5-4800" -- which selects the IDD row; every family-level decision
 * (refresh ladder, termination scheme, devices per access, background
 * units) sees the bare technology. baseTech() strips the suffix. */
inline std::string baseTech(const std::string& key) {
    const auto dash = key.find('-');
    return (dash == std::string::npos) ? key : key.substr(0, dash);
}
inline IDDSpec iddFor(const std::string& tech, int temperature_k = 358) {
    IDDSpec s = iddTableFor(tech);
    s.trefi_ns *= refreshTempFactor(baseTech(tech), temperature_k);
    return s;
}

/* 1.11.46 (FIX-PRE-FLEET L181): DEVICES PER ACCESS. The IDD columns are
 * PER-DEVICE currents (the struct says so), but a 64 B access on a DDR-class
 * 64-bit rank engages EVERY chip in the rank simultaneously -- x8 parts: 8
 * chips each activating and bursting 8 of the 64 DQ lines. Micron TN-41-01,
 * this file's own cited formula source, multiplies per-DRAM power by the
 * number of DRAMs; we did not, so the array terms were per-DEVICE while the
 * termination term (x512 bits) was whole-rank -- the two bases the audit
 * caught being summed. HBM's IDD is per CHANNEL and an access stays in one
 * channel; LPDDR5/GDDR6 are one die per channel. */
inline int devicesPerAccess(const std::string& tech,
                            const std::string& device_width = "") {
    if (tech == "DDR3" || tech == "DDR4" || tech == "DDR5") {
        return devicesPerRank(device_width);   // 1.11.57 (latent D075)
    }
    /* 1.11.57 (latent D007): 1 is the RIGHT answer for every technology this
     * file knows about that is not DDR-class -- HBM's IDD is per channel and
     * an access stays in one channel, LPDDR5 and GDDR6 put one die behind one
     * channel -- but it is also what an unrecognised string gets, and for an
     * unrecognised string it is a guess. Distinguish the two: the known
     * technologies fall through silently, anything else says so, because the
     * consequence is that a rank's worth of array energy is charged once
     * instead of eight times. */
    if (tech.substr(0, 3) != "HBM" && tech != "LPDDR5" && tech != "GDDR6") {
        announceUnknownTech("devicesPerAccess", tech,
                            "how many devices one 64 B access engages (the "
                            "whole-rank multiplier on array energy)");
    }
    return 1;
}

// Array read energy per 64B (act+pre weighted by the CALLER'S MEASURED row-miss
// fraction, plus the read burst). bank_override_pJ_per_byte > 0 forces the
// legacy bank-energy path (user knob); 0 = IDD default.
/* 1.11.57 (audit D014): the one-line summary said "50% row-hit collapse" -- the
 * constant 1.11.52 (D003) removed. Twelve lines below, the code takes the miss
 * fraction from the caller and falls back to 0.5 only when the run carried no
 * row measurement, and the block there explains that at length. The summary a
 * reader sees FIRST still asserted the retired constant, so the file described
 * two different models of its own dominant term. Live in every DRAM run: this
 * is the function the power path calls. */
/* 1.11.57 (latent D004): THE OVERRIDE IS ON THE SAME BASIS AS THE MODEL IT
 * REPLACES. The knob returned bank_pJ_per_byte x 64 with no devicesPerAccess()
 * factor while the IDD path beside it multiplied by the whole rank, so setting
 * the override on a DDR-class part did not merely substitute a value, it also
 * silently changed the basis and dropped DDR array energy by 8x. The knob is
 * documented as "override the IDD-derived array energy", so it must land in
 * the same units the IDD path produces: a per-DEVICE bank figure, scaled to
 * the devices one access engages. This was invisible because the knob has NO
 * WRITER -- setBankEnergyOverridePJPerByte() is declared in
 * ramulator_wrapper.h, the member is initialised to 0.0, and nothing in src/
 * or include/ or any YAML key ever calls it -- so the 8x lived in a branch
 * that never runs. It runs the first time anyone wires a YAML key to it. */
/* 1.11.79 (audit round 6, R6-11): THE ACTIVATE TERM MUST NOT COME OUT NEGATIVE.
 *
 * Micron TN-41-01's activate/precharge energy is
 *     E = Vdd x (IDD0 x tRC - IDD3N x tRAS - IDD2N x (tRC - tRAS))
 * and it presumes IDD0 -- a cycling one-bank activate/precharge current --
 * exceeds the standby currents it subtracts. Six of this file's nine rows
 * satisfy that. The two 16 Gb DDR5 rows added in 1.11.66 from the MT60B
 * addenda do not: at 4800B, IDD0 103 against IDD3N 142, so
 * IDD0 x tRC = 4970 < IDD3N x tRAS + IDD2N x (tRC - tRAS) = 6041 pA.ns and
 * the term is -1178 pJ. Measured consequence on a default-grade DDR5 cell:
 * per-access read -0.344 nJ, write -1.164 nJ, "Total dynamic: -5.5 mJ" --
 * a negative energy in a reported number, and DDR5-4800 is the corpus part.
 *
 * Which side is wrong is a CALIBRATION question this file must not answer by
 * itself: either the row mis-transcribes the addendum, or the row is right and
 * the formula does not transfer to a 32-bank DDR5 device whose all-banks-active
 * IDD3N can legitimately exceed a one-bank IDD0. Both need the datasheet and a
 * ruling. What is NOT in question is that a negative energy may not be printed
 * as if it were a measurement, so the run refuses and says exactly why. */
inline void refuseNegativeActivateEnergy(const std::string& tech, const IDDSpec& s,
                                         double tRC, double tRAS, double e_actpre_pJ) {
    if (e_actpre_pJ >= 0.0) return;
    std::cerr << "[power] FATAL: the activate/precharge energy for '" << tech
              << "' comes out NEGATIVE (" << e_actpre_pJ << " pJ), so the array "
                 "energy this run would report is not a physical quantity.\n"
                 "  Micron TN-41-01: E = Vdd x (IDD0 x tRC - IDD3N x tRAS - "
                 "IDD2N x (tRC - tRAS)), which presumes IDD0 exceeds the standby "
                 "currents it subtracts.\n"
                 "  This row: IDD0 " << s.idd0 << ", IDD2N " << s.idd2n
              << ", IDD3N " << s.idd3n << " mA at Vdd " << s.vdd
              << " V, tRC " << tRC << " ns, tRAS " << tRAS << " ns"
              << " -- IDD0 x tRC = " << (s.idd0 * tRC)
              << " against " << (s.idd3n * tRAS + s.idd2n * (tRC - tRAS))
              << " subtracted.\n"
                 "  Either the IDD row mis-transcribes its datasheet, or the row "
                 "is right and this formula does not transfer to this part (a "
                 "32-bank DDR5 device's all-banks-active IDD3N can exceed a "
                 "one-bank IDD0). Resolve the row in pimid_energy.h against the "
                 "datasheet, or price this technology's array by another route; "
                 "do not read the negative number." << std::endl;
    std::exit(2);
}

inline double arrayReadNJ(const std::string& tech, double tRC, double tRAS,
                          double tBurst, double bank_override_pJ_per_byte,
                          const std::string& device_width = "",
                          double row_miss_frac = -1.0) {
    if (bank_override_pJ_per_byte > 0.0)
        return bank_override_pJ_per_byte * 64.0 / 1000.0
               * devicesPerAccess(baseTech(tech), device_width);   // 1.11.57 (D004)
    IDDSpec s = iddFor(tech);
    double e_actpre_pJ = s.vdd * (s.idd0 * tRC - s.idd3n * tRAS - s.idd2n * (tRC - tRAS));
    refuseNegativeActivateEnergy(tech, s, tRC, tRAS, e_actpre_pJ);   // 1.11.79 (R6-11)
    double e_rd_pJ     = s.vdd * (s.idd4r - s.idd3n) * tBurst;
    /* 1.11.52 (audit D003): the activate/precharge share is MEASURED, not
     * assumed. It is the dominant term -- on DDR4 the act+pre part is ~1.42
     * nJ against ~0.39 nJ of burst -- and it used to be weighted by a
     * hardcoded ROW_MISS_FRAC = 0.5, so the largest number in the array
     * energy was a coin flip. The memory interface now tracks one open row
     * per unit and exports rowHits/rowMisses; the caller passes the measured
     * miss fraction. A negative value means the run carried no row
     * measurement, and the caller is responsible for saying so -- the 0.5
     * below is then the stated fallback, not a silent default. */
    const double ROW_MISS_FRAC = (row_miss_frac >= 0.0 && row_miss_frac <= 1.0)
                                 ? row_miss_frac : 0.5;
    return (ROW_MISS_FRAC * e_actpre_pJ + e_rd_pJ) / 1000.0
           * devicesPerAccess(baseTech(tech), device_width);   // 1.11.46 (L181)
}
inline double arrayWriteNJ(const std::string& tech, double tRC, double tRAS,
                           double tBurst, double bank_override_pJ_per_byte,
                           const std::string& device_width = "",
                           double row_miss_frac = -1.0) {
    /* 1.11.5 (audit): writes consult IDD4W, not read*1.2. Same shape as the
     * read term: activate/precharge share plus the write burst current. */
    /* 1.11.57 (latent D004): the RETIRED 1.2 is gone from here too. Two lines
     * under the comment announcing that "writes consult IDD4W, not read*1.2",
     * the override branch still multiplied the user's number by exactly that
     * 1.2 -- the ratio 1.11.5 removed as unsourced. A user-supplied array
     * energy per byte is ONE figure; this file has no sourced write/read ratio
     * to apply to it (the IDD path gets its write term from IDD4W, which the
     * override deliberately bypasses), so inventing 20% on top of a number the
     * user chose is worse than reporting the number the user chose. Writes and
     * reads therefore take the same override, and the missing write premium is
     * a stated limitation of the knob rather than a fabricated constant. It
     * was invisible for the same reason as the read half: the knob has no
     * writer anywhere in the tree. */
    if (bank_override_pJ_per_byte > 0.0)
        return bank_override_pJ_per_byte * 64.0 / 1000.0
               * devicesPerAccess(baseTech(tech), device_width);   // 1.11.57 (D004)
    IDDSpec s = iddFor(tech);
    double e_actpre_pJ = s.vdd * (s.idd0 * tRC - s.idd3n * tRAS - s.idd2n * (tRC - tRAS));
    refuseNegativeActivateEnergy(tech, s, tRC, tRAS, e_actpre_pJ);   // 1.11.79 (R6-11)
    double e_wr_pJ     = s.vdd * (s.idd4w - s.idd3n) * tBurst;
    const double ROW_MISS_FRAC = (row_miss_frac >= 0.0 && row_miss_frac <= 1.0)
                                 ? row_miss_frac : 0.5;   // 1.11.52 (D003)
    return (ROW_MISS_FRAC * e_actpre_pJ + e_wr_pJ) / 1000.0
           * devicesPerAccess(baseTech(tech), device_width);   // 1.11.46 (L181)
}

/* 1.11.5 (audit): interfaceNJ REMOVED. It returned vdd*(idd4r-idd3n)*tBurst
 * -- bit-identical to the burst term already inside arrayReadNJ, so every
 * consumer that added it double-charged the DQ read current. JEDEC IDD4R is
 * measured with the outputs driving: the on-die I/O switching is already in
 * the array term. The genuinely ADDITIONAL off-chip energy is termination
 * (below), which was computed and never charged. */

/* ODT/termination per 64B, per I/O standard. term_override_pJ_per_bit >= 0 =
 * user knob.
 *
 * 1.11.57 (latent D005): the `bool termination_enable = true` parameter is
 * GONE. It was a documented knob with no writer: both call sites in
 * ramulator_wrapper.cpp passed two arguments, nothing in src/ or include/ ever
 * passed false, and no YAML key reached it. A defaulted parameter that no
 * caller can set is not a switch, it is a claim in the signature that the
 * model can be turned off -- and the next person to believe it would have
 * added the knob at the wrong layer, because "no termination" is a property of
 * the I/O standard (LVSTL, HBM's unterminated interposer) that this function
 * already decides from the technology. Deleted rather than wired up.
 *
 * 1.11.57 (latent D017): THE RATE COMES FROM THE CALLER, and there is now one
 * rate table in the tree instead of two. This function used to carry its own
 * mtps column -- DDR3 1600, DDR4 2400, DDR5 4800, GDDR6 14000, LPDDR5 6400,
 * and 3200 for anything else -- beside CactiIOWrapper::dramRateMTs(), which
 * carries the same quantity for the same technologies. The two are the halves
 * of one access: dramRateMTs decides the bandwidth and the CACTI-IO
 * termination figure, this column decided the bit period the scheme-table
 * termination is integrated over. They had already drifted: 1.11.57 (C001)
 * moved DDR5 to 3200 MT/s in dramRateMTs to match the preset this tree
 * simulates, and this column stayed at 4800, i.e. a 1.5x error waiting on the
 * day DDR5 stops taking the exact-map CACTI-IO path. It was invisible because
 * every technology that reaches this table today either has an exact CACTI-IO
 * map (DDR3/DDR4/DDR5, whose result replaces this one) or returns before the
 * rate is read (HBM). The transfer rate is a specification primitive and
 * belongs in one place; the electricals below (scheme, VDDQ, Rtt, Ron) are
 * interface-standard properties and stay here, where they are sourced.
 *
 * 1.11.22 (user decision D12) -- DERIVED FROM THE JEDEC I/O STANDARDS the
 * technologies actually cite, replacing a per-scheme fudge factor whose
 * values the 1.11.15 audit showed were transposed. Two independent errors
 * were confirmed, both from normative text:
 *
 *  POD (DDR4 = POD12/JESD8-24, DDR5 = POD11, GDDR6 = POD135/JESD8-21C):
 *    "The POD driver uses a 40/60 Ohm output impedance that drives into a
 *     60 Ohm equivalent terminator tied to VDDQ" and "the terminator is
 *     disabled when the output driver is enabled" (JESD8-21C.01 cl.3);
 *    "signals ... are not generally expected to pull to VSS ... pull-up-only
 *     parallel input termination" (JESD8-25 cl.1).
 *    => driving HIGH the line sits at VDDQ and NO DC current flows; driving
 *       LOW the loop is the driver pull-down IN SERIES with the terminator.
 *       So duty ~0.5 for random data (we had 1.0), and the loop resistance
 *       is Rpd+Rtt (we used Rtt alone -- a further 100/60 = 1.67x). Combined
 *       we overstated GDDR6 termination by 3.33x.
 *
 *  SSTL (DDR3 = SSTL-15): terminated to VTT = VDDQ/2, so current flows in
 *    BOTH states (duty 1.0 -- we had 0.5) but the voltage ACROSS the
 *    terminated loop is VDDQ/2, not VDDQ. The two corrections partly cancel,
 *    which is why the transposition was not obvious in the totals.
 *    Every DDR3 constant below is normative, from JESD79-3D:
 *      Rpd = 34 Ohm: Table 38 "Output Driver DC Electrical Characteristics",
 *        RON34Pd/RON34Pu = RZQ/7 with RZQ = 240 Ohm; selected by MR1{A5,A1}
 *        = {0,1} (Figure 10). The other legal strength is RZQ/6 = 40 Ohm.
 *      Rtt = 40 Ohm: Table 41 "ODT DC Electrical Characteristics", RTT40 =
 *        RZQ/6, selected by MR1{A9,A6,A2} = {0,1,1} (Figure 10).
 *      Mid-rail: the SAME Table 41 row builds RTT40 from RTT40Pu80 and
 *        RTT40Pd80, each RZQ/3 = 80 Ohm, i.e. a split pull-up/pull-down pair
 *        whose Thevenin point is specified as "Deviation of VM w.r.t.
 *        VDDQ/2, DVM: -5/+5 %". That row IS the v_term = VDDQ/2 below; it is
 *        not an inference from the SSTL name.
 *
 *  LVSTL (LPDDR5): GROUND-REFERENCED, and it DOES terminate. This row
 *    returned 0 until 1.11.26 on the claim "unterminated by design". Micron's
 *    LPDDR5 datasheets say otherwise, in the feature list itself:
 *    "Programmable VSS on-die termination (ODT)", "Interface-LVSTL 0.5/0.3",
 *    "VDDQ = 0.50V or 0.45V TYP; 0.30V TYP (ODT off)", RON = 40 ohm
 *    (misc/MICT-S-A0025741931-1.pdf; misc/315b-441b-561b-y52q-*.pdf).
 *    So it is a THIRD topology: POD terminates to VDDQ, SSTL to a VDDQ/2
 *    mid-rail, LVSTL to VSS. Current flows while the driver holds the line
 *    HIGH -- the mirror of POD -- duty ~0.5 for unbiased data, loop = driver
 *    pull-up + terminator. The 0.5 V rail is what makes it cheap: against
 *    DDR5's 1.1 V that is 4.8x less V^2 before resistance divides.
 *    RESIDUAL CLOSED (1.11.63, JESD209-5C acquired): the standard's own MR11
 *    definition (Table 84, printed p.144) gives "000B: Disable (Default)" for
 *    DQ ODT -- the JEDEC default operating point is UNTERMINATED, and the
 *    RZQ/1..6 ladder (RZQ = 240 ohm -> 240/120/80/60/48/40) is a controller
 *    option with no default rung. So the pre-1.11.26 "returns 0" behaviour
 *    was RIGHT for the default configuration, for a reason nobody had the
 *    document to state: current flows only when a controller has enabled
 *    ODT. rtt = 0 in the row above encodes the default; Micron's IDD
 *    conditions are ODT-off, so the array-energy layer describes the same
 *    configuration by construction.
 *  HBM: 0, and now with a normative citation rather than physics reasoning:
 *    JESD238B.01 cl.9.1 measures HBM3 read-burst current with "IOUT = 0mA;
 *    Ctotal = 2.5 pF" -- an unterminated capacitive load.
 *
 * BOUNDARY (stated, not hidden): the 0.5 POD duty assumes an unbiased bit
 * stream. DBIac is enabled during JEDEC IDD measurement and deliberately
 * skews the LOW fraction, so the true duty is data-dependent; D12's IDD
 * cross-check is what pins that residual. */
/* 1.11.63 (R7, user ruling 2026-08-24): READ/WRITE SPLIT LOOPS. The single
 * (rtt, rpd) pair this function carried since 1.11.26 priced one loop for
 * both directions, with values (48/40 for DDR4/DDR5) that match no JEDEC
 * default and no vendor IDD condition -- and DDR5's citation ("POD11") named
 * a standard that DOES NOT EXIST (the POD family is POD18/15/135/125/12/10;
 * all six are in misc/). The two directions are different circuits:
 *   READ : the DRAM drives (its calibrated RON) into the receiving side's
 *          termination (RTT_NOM class).
 *   WRITE: the controller drives into the DRAM's write termination (RTT_WR
 *          class, a deliberately stronger setting in every DDR family).
 * The values below are the vendors' own IDD MEASUREMENT CONDITIONS -- the
 * register settings the array-energy IDD rows are measured under, so the
 * two layers describe the same configuration by citation:
 *   DDR3L : RON=RZQ/7=34, RTT_NOM=RZQ/6=40, RTT_WR=RZQ/2=120
 *           (Micron MT41K p.32: "RON set to RZQ/7 (34); RTT,nom set to
 *            RZQ/6 (40); RTT(WR) set to RZQ/2 (120)")
 *   DDR4  : same trio (Micron MT40A p.315 IDD conditions)
 *   DDR5  : same trio (Micron DDR5 core sheet p.453 IDD notes: MR5 RZQ/7
 *           both drivers, MR35 RTT_NOM_WR=RTT_NOM_RD=RZQ/6, MR34
 *           RTT_WR=RZQ/2; RTT_PARK/CA/CS/CK disabled)
 *   GDDR6 : read loop = pull-down 40 + termination-characteristic 60
 *           (Samsung K4Z80325BC p.166: "Pull-Down Characteristic at 40
 *           ohms, Pull-Up/Termination Characteristic at 60 ohms"); write
 *           loop = 40 + 120 (p.144 IDD conditions: "All ODTs are enabled
 *           with ZQ/2", ZQ=240). MR1 default is termination DISABLED
 *           (p.49) -- the IDD operating point is the one priced, for
 *           layer-consistency with the IDD-sourced array energy.
 *   LPDDR5: 0 both directions (JESD209-5C Tbl 84 p.144: DQ ODT Disable
 *           (Default); Micron IDD conditions are ODT-off).
 * CONTROLLER-SIDE WRITE DRIVER, sourced as a RANGE/SET with the applied
 * value inside it -- no assumption remains:
 *   DDR4/DDR5: Intel 743844-015 (misc/, Vol.1 of the 13th/14th-gen
 *   datasheet) Table 87 p.208 gives host RON_UP(DQ) = RON_DN(DQ) =
 *   30..50 ohm for DDR5, Table 86 p.207 the same for DDR4 (host RODT(DQ):
 *   30..240 / 40..200). Applied 34 (the DRAM-class RZQ/7 value) lies
 *   inside the range; its ends move the write loop (34+120=154) by only
 *   -3%/+10%.
 *   GDDR6: Achronix Speedster7t GDDR6 User Guide UG091 (misc/), Table 3
 *   p.16: "DQ driver impedance (RON) 40/48/60 ohm" -- a host-controller
 *   PHY's settable set, matching the standard 60/40 and 48/40 pairings.
 *   Applied 40 is a member of the set; 48 or 60 would move the write
 *   loop (40+120=160) by +5%/+12.5%.
 * Per the band doctrine the range/set is stated here and the point chosen
 * within it. The override prices BOTH directions at the stated pJ/bit. */
inline double terminationNJ(const std::string& tech, double term_override_pJ_per_bit,
                            double rate_mts, bool is_write) {
    if (term_override_pJ_per_bit >= 0.0)
        return term_override_pJ_per_bit * 512.0 / 1000.0;

    /* 1.11.26: LVSTL is a THIRD topology, not an absence of one. Micron's
     * LPDDR5 datasheets state "Programmable VSS on-die termination (ODT)" with
     * VDDQ = 0.50 V nominal ODT-on and 0.30 V ODT-off, RON = 40 ohm
     * (misc/MICT-S-A0025741931-1.pdf, misc/315b-441b-561b-y52q-*.pdf). It
     * terminates to GROUND -- the mirror image of POD, which terminates to
     * VDDQ. So current flows while the driver holds the line HIGH, duty ~0.5
     * for random data, across a loop of driver pull-up plus terminator. */
    enum Scheme { POD, SSTL, LVSTL, NONE };
    Scheme sch; double vddq, ron_rd, rtt_rd, ron_wr, rtt_wr;   // R7 split; rate is the caller's
    /* 1.11.46 (FIX-PRE-FLEET L164): ONE PART per technology. The IDD row
     * above is sourced from Micron 4Gb DDR3L-1600 -- a 1.35 V part -- while
     * this line priced a 1.5 V SSTL-15 DDR3. Array and termination now
     * describe the SAME silicon: DDR3L, SSTL-135 (JESD79-3-1, the DDR3L
     * addendum keeps RZQ=240 and the T38/T41 RTT/RON tables at 1.35 V). */
    if      (tech=="DDR3")   {sch=SSTL; vddq=1.35; ron_rd=34; rtt_rd=40; ron_wr=34; rtt_wr=120;}
                                                                       // SSTL-135 (JESD79-3-1 T38/T41, RZQ=240);
                                                                       // trio 34/40/120 = MT41K p.32 IDD conditions
    /* 1.11.52 (audit D002): the rate is the SIMULATED part's rate (DDR4-2400:
     * Ramulator preset DDR4_2400R and the architecture object), not a
     * different bin. POD12 (JESD8-24) is the interface standard and applies
     * at either rate, so vddq/rtt/rpd are untouched. 1.11.57 (D017): that rate
     * now arrives as an argument, from the one table that owns it. */
    else if (tech=="DDR4")   {sch=POD;  vddq=1.2;  ron_rd=34; rtt_rd=40; ron_wr=34; rtt_wr=120;}
                                                                       // POD12 (JESD8-24); trio = MT40A p.315 IDD conds
    else if (tech=="DDR5")   {sch=POD;  vddq=1.1;  ron_rd=34; rtt_rd=40; ron_wr=34; rtt_wr=120;}
                                                                       // POD topology per JESD79-5 at 1.1 V (no separate
                                                                       // JESD8-* exists for 1.1 V -- the old "POD11" tag
                                                                       // named a nonexistent standard); trio = Micron
                                                                       // DDR5 core sheet p.453 IDD conditions
    else if (tech=="GDDR6")  {sch=POD;  vddq=1.35; ron_rd=40; rtt_rd=60; ron_wr=40; rtt_wr=120;}
                                                                       // POD135 (JESD8-30A.01 family is POD125; GDDR6
                                                                       // at 1.35 V per JESD250D); rd 40+60 = Samsung
                                                                       // K4Z80325BC p.166 driver/termination chars;
                                                                       // wr 120 = p.144 IDD "All ODTs ... ZQ/2"
    /* 1.11.26: was NONE ("LVSTL, unterminated") -- wrong. LPDDR5 does
     * terminate; it terminates to VSS. VDDQ 0.5 V is the ODT-ON rail
     * (0.30 V is the ODT-off rail), RON 40 ohm from the same datasheets.
     * RTT: the datasheet defers the ohm table to Micron's separate
     * "General LPDDR5 Specifications 2: AC/DC and Interface" document, which
     * we do not have -- so 240 ohm (RZQ, the LPDDR4/5 ODT reference) is the
     * one UNSOURCED input here and is flagged as such below. */
    /* 1.11.52 (audit D008): the LPDDR5 Rtt below is the one UNSOURCED
     * electrical input in this table -- Micron's datasheets state
     * "programmable VSS ODT" and give VDDQ and RON but not the termination
     * value we need, so 240 ohm is an assumption, and it is 240 of the
     * 280-ohm loop (a 2x error in it moves LPDDR5 termination energy
     * ~1.75x). It was disclosed only in a comment 45 lines away; the
     * consumer now reports it at the point of use (see terminationNJ). */
    /* 1.11.63 (calibration): JESD209-5C Table 84 p.144 -- DQ ODT default is
     * DISABLE. rtt = 0 is the sentinel for "no DC termination path"; the
     * LVSTL branch below prices zero termination current for it and states
     * the citation. The RZQ/1..6 ladder (240..40 ohm) is a controller
     * option, not a default; users modelling ODT-on systems override via
     * power.termination_pj_per_bit. */
    else if (tech=="LPDDR5") {sch=LVSTL; vddq=0.5; ron_rd=40; rtt_rd=0; ron_wr=40; rtt_wr=0;}
    /* 1.11.64: HBM's zero is not an omission, and the vertical interconnect
     * is NOT a missing term. Three independent confirmations that the DQ
     * link is unterminated: JESD238B.01 cl.9.1 measures HBM3 read-burst
     * current at "IOUT = 0mA; Ctotal = 2.5 pF" (a capacitive load, no DC
     * path); ISCA 2025 tutorial slide 46 (Song, Samsung) states flatly "ODT
     * not allowed in HBM (static power)"; and every HBM device paper we hold
     * lists the interface as "CMOS, un-terminated" (Chun JSSC 2021 Table I
     * p.200).
     *
     * WHY NO SEPARATE TSV TERM IS ADDED, having acquired the only public
     * measurement of one. Cho ISSCC 2018 12.3 Fig.12.3.1 (misc/) measures
     * per-TSV driver current on a real HBM2 stack -- ~880 uA multi-drop vs
     * ~610 uA with the spiral point-to-point structure, at 1.0 V and
     * 3.3 Gb/s PRBS, i.e. roughly 0.27 -> 0.19 pJ/bit for the TSV driver
     * alone. It is tempting to add that as the "missing" vertical-link
     * energy. It would DOUBLE COUNT. The IDD columns above are per-CHANNEL
     * DEVICE currents (see devicesPerAccess below), and an IDD4R/IDD4W
     * measurement is taken at the stack's supply balls with a read or write
     * burst in flight -- the TSVs are inside the device under test, so their
     * driver current is already inside the measured burst current. This is
     * exactly the structural difference from DDR-class parts, whose DQ bus
     * leaves the package and terminates externally, which is why those get a
     * termination term and HBM does not: for HBM the interface is internal
     * and is priced by the array/burst currents, not beside them.
     * The Cho figure is therefore a DECOMPOSITION insight -- what fraction
     * of stack current is vertical signalling -- and belongs in validation,
     * not in the charged model. */
    else if (tech.substr(0,3)=="HBM") return 0.0;
    else {
        /* 1.11.57 (latent D007): unknown -> POD12/DDR4 electricals, said out
         * loud. This branch also disagrees with iddFor()'s exact "HBM2"/"HBM3"
         * match three functions up: a string like "HBM2E" gets zero here (the
         * substr) and DDR4 currents there. Whitelisting keeps both unreachable
         * today. */
        announceUnknownTech("terminationNJ", tech,
                            "the DQ termination energy per 64 B access");
        sch=POD;  vddq=1.2;  ron_rd=34; rtt_rd=40; ron_wr=34; rtt_wr=120;   // DDR4 electricals, said out loud
    }
    if (sch == NONE) return 0.0;

    /* 1.11.57 (latent D017): the bit period comes from the caller's rate. A
     * non-positive rate means the caller could not source one, and this
     * function will not invent a speed bin to keep a number flowing -- it
     * reports zero termination and says why, which is visibly wrong rather
     * than plausibly wrong. */
    if (!(rate_mts > 0.0)) {
        announceUnknownTech("terminationNJ", tech,
                            "the DQ termination energy per 64 B access, which "
                            "is reported as ZERO because no data rate was "
                            "supplied for this technology");
        return 0.0;
    }
    const double t_bit_s = 1.0 / (rate_mts * 1e6);
    const double r_drv = is_write ? ron_wr : ron_rd;
    const double r_trm = is_write ? rtt_wr : rtt_rd;
    double e_per_bit_pJ;
    if (sch == LVSTL) {
        /* Ground-referenced: the loop conducts while the line is HIGH, so the
         * duty is the complement of POD's but numerically the same 0.5 for
         * unbiased data. Loop = driver pull-up + terminator to VSS.
         * The low rail is what makes this cheap: 0.5 V against DDR5's 1.1 V
         * is a 4.8x reduction in V^2 before the resistance divides.
         * 1.11.63: rtt = 0 means ODT DISABLED (JESD209-5C Tbl 84 p.144:
         * "000B: Disable (Default)") -- the DC loop does not exist, so the
         * termination term is zero, NOT a divide into (rpd + 0): that would
         * price a 40-ohm dead short and be off by the full driver current.
         * Switching energy on the unterminated line is capacitive and lives
         * in the IDD4R/IDD4W rows, same as the HBM return below. */
        if (r_trm <= 0.0) return 0.0;
        const double kHighDuty = 0.5;
        e_per_bit_pJ = kHighDuty * (vddq * vddq) / (r_drv + r_trm) * t_bit_s * 1e12;
    }
    else if (sch == POD) {
        // current only while LOW; loop = driver + terminator (R7: the pair
        // is direction-selected above -- read: DRAM RON + RX RTT_NOM class;
        // write: controller RON + DRAM RTT_WR class)
        const double kLowDuty = 0.5;
        e_per_bit_pJ = kLowDuty * (vddq * vddq) / (r_drv + r_trm) * t_bit_s * 1e12;
    } else {
        // SSTL: VTT = VDDQ/2 across the loop, drawn in both states
        const double v_term = vddq * 0.5;
        e_per_bit_pJ = (v_term * v_term) / (r_drv + r_trm) * t_bit_s * 1e12;
    }
    return e_per_bit_pJ * 512.0 / 1000.0;
}

/* All three quantities below are PER IDD-BEARING UNIT: one DDR-class chip,
 * or one HBM channel. That is the unit the JEDEC IDD tables are written
 * against. Multiplying up to the memory system is backgroundUnits() and is
 * done once, in backgroundSystemMW(). (Before 1.11.20 there was no
 * multiplication at all: an HBM stack's 8-16 channels and a DDR rank's 8
 * chips were each reported as a single device's background.) */
/* 1.11.37 (audit E15): refresh charged PER STATE.
 *
 * JEDEC IDD5 is the all-bank auto-refresh current, measured with the banks
 * precharged; the device draws it for tRFC out of every tREFI. It is an
 * ABSOLUTE current, not an increment, and a device must EXIT power-down to
 * accept a REFRESH command -- so for the tRFC/tREFI duty fraction the unit
 * draws IDD5 whatever state it was otherwise holding, and its own state
 * current for the remainder.
 *
 *     P(state) = vdd * ( idd_state * (1 - duty) + idd5 * duty )
 *
 * That expression is state-independent, which the previous structure was not:
 * refreshMW() below returns the excess over IDD3N, correct only when added to
 * an IDD3N baseline, and backgroundUnitMW() added it over the IDLE fraction
 * too, where the baseline is IDD2N or IDD2P. Since IDD2P < IDD3N, refresh was
 * UNDER-charged during power-down by vdd*(idd3n-idd2p)*duty -- for HBM3,
 * 1.1 * (22-7) * (160/3900) = 0.68 mW/unit, ~2.6% of the 26.4 mW per-unit
 * background, 10.8 mW across a 16-channel stack at full idle. Inert on the
 * present corpus (1.11.20 measured r_idle = 0: memory-bound kernels keep the
 * controller busy every phase), so this is a correctness fix, not a results
 * change -- backgroundUnitMW is bit-identical at r_idle = 0 by construction.
 *
 * The idd5 clamp guards a table row where IDD5 < the state current, which
 * would otherwise let refresh REDUCE a unit's power. */
inline double stateWithRefreshMW(const IDDSpec& s, double idd_state) {
    const double duty = (s.trefi_ns > 0.0) ? (s.trfc_ns / s.trefi_ns) : 0.0;
    const double idd5 = (s.idd5 > idd_state) ? s.idd5 : idd_state;
    return s.vdd * (idd_state * (1.0 - duty) + idd5 * duty);
}

/* The refresh EXCESS over active standby. Reported as its own line item, and
 * that is the only thing it means: it is IDD3N-relative and must not be added
 * to a baseline that is not IDD3N. backgroundUnitMW() no longer calls it. */
inline double refreshMW(const std::string& tech, int temperature_k = 358) {   // 1.11.65
    IDDSpec s = iddFor(tech, temperature_k);
    return s.vdd * (s.idd5 - s.idd3n) * (s.trfc_ns / s.trefi_ns);
}
inline double backgroundMW(const std::string& tech, int temperature_k = 358) {
    IDDSpec s = iddFor(tech, temperature_k);
    return stateWithRefreshMW(s, s.idd3n);   // == vdd*idd3n + refreshMW(tech, T)
}

/* 1.11.20 (user decision D13): POPULATION. How many IDD-bearing units the
 * memory system presents behind one channel.
 *
 * Deliberately derived from technology + JEDEC device width and NOT from
 * config.hierarchy_chips_per_rank, even though that field holds the same
 * numbers: the hierarchy field is degenerated to 1 for HOST_MC placement
 * (main.cpp, the PEs-share-the-host-MC path), because it is an address-
 * mapping fanout there. The number of chips physically drawing standby
 * current does not depend on where the PEs sit, so reading that field would
 * have silently zeroed the correction for exactly the baseline placement.
 *
 *   HBM2/HBM3   channels per stack (8 / 16), from IDDSpec::channels, which
 *               was declared in 1.11.8 for this purpose and never read.
 *               JESD238B.01 cl.9.1 specifies HBM IDD per channel.
 *   DDR3/4/5    chips per rank for a 64-bit channel: x4 -> 16, x8 -> 8,
 *               x16 -> 4. Mirrors the device-width table in main.cpp.
 *   LPDDR5      1: an x16 die serves its own channel.
 *   GDDR6       1: point-to-point, one device per channel.
 *   SRAM/NVM    1 (they do not reach this path; the fallthrough is DDR4). */
/* 1.11.52 (audit A015): the POPULATION arguments. This returns the devices
 * in ONE rank (DDR) or ONE stack (HBM); the SYSTEM may hold several ranks
 * and channels, and the area path already counts them all
 * (memorySystemDieCount = chips x ranks x channels). The two lines of one
 * report therefore described memories differing by ranks x channels: e.g.
 * two ranks put 54 mm^2 of memory area beside 1.12 W of single-rank memory
 * power. Callers pass the system's rank and channel counts so background
 * power is population-scaled the same way area is; the defaults reproduce
 * the pre-1.11.52 single-rank basis for any caller not yet updated. */
inline int backgroundUnits(const std::string& tech,
                           const std::string& device_width = "",
                           int ranks_per_channel = 1,
                           int channels = 1) {
    if (ranks_per_channel < 1) ranks_per_channel = 1;
    if (channels < 1) channels = 1;
    if (tech.substr(0, 3) == "HBM") {
        /* An HBM stack's channels ARE its population; a system with several
         * stacks multiplies by the channel count the caller reports. */
        int ch = iddFor(tech).channels;
        int per_stack = ch > 0 ? ch : 1;
        int stacks = (channels > per_stack && per_stack > 0)
                     ? (channels / per_stack) : 1;
        return per_stack * stacks;
    }
    if (tech == "DDR3" || tech == "DDR4" || tech == "DDR5") {
        /* 1.11.57 (latent D075): was a second copy of the x4/x8/x16 table
         * that devicesPerAccess() also carried. One table now. */
        return devicesPerRank(device_width) * ranks_per_channel * channels;
    }
    /* 1.11.57 (latent D007): one IDD-bearing unit per channel is correct for
     * LPDDR5 (an x16 die serves its own channel) and GDDR6 (point to point),
     * and it is a guess for anything this file does not recognise -- where it
     * silently understates the memory system's whole background power by the
     * rank population. Known technologies fall through; anything else says so. */
    if (tech != "LPDDR5" && tech != "GDDR6") {
        announceUnknownTech("backgroundUnits", tech,
                            "the population the memory system's background "
                            "power and refresh line are multiplied by");
    }
    return ranks_per_channel * channels;
}

/* 1.11.20 (user decision D15): STATE. Background power for ONE unit, given
 * the measured no-traffic residency r_idle. Three JEDEC states, not two:
 *
 *   busy  (1 - r_idle)  ACTIVE STANDBY, IDD3N -- a row is open.
 *   idle, pg off        PRECHARGE STANDBY, IDD2N. An idle controller closes
 *                       its pages; that descent is PAGE POLICY and happens
 *                       whether or not a power-management feature exists.
 *                       1.11.18 identified this correctly but left the
 *                       baseline at IDD3N to preserve pg-off bit-identity,
 *                       which meant the baseline stayed knowingly wrong.
 *                       D15 fixes the baseline instead.
 *   idle, pg on         PRECHARGE POWER-DOWN, IDD2P (CKE low).
 *
 * Refresh continues in ALL states -- DRAM must retain -- so every state pays
 * it, each against its OWN baseline (1.11.37, E15). Before that it was added
 * as one IDD3N-relative term on top of all three states, which under-charged
 * refresh during power-down.
 *
 * tXP hysteresis: entry/exit overhead means a small slice of the idle time
 * cannot reach power-down. Phases are 10k cycles and tXP is ~10 ns, so that
 * slice is <1%; the 0.99 factor states it rather than ignoring it. The slice
 * that fails to reach IDD2P sits at IDD2N, not at IDD3N -- it is still idle.
 *
 * DELIBERATE BASELINE CHANGE: r_idle > 0 with pg OFF no longer reproduces
 * backgroundMW(). That was the point of D15, and it is why the 1.11.20 gate
 * asserts a stated delta for DRAM cells rather than bit-equality. */
inline double backgroundUnitMW(const std::string& tech, double r_idle,
                               bool pg_enabled, int temperature_k = 358) {
    if (r_idle < 0.0) r_idle = 0.0;
    if (r_idle > 1.0) r_idle = 1.0;
    IDDSpec s = iddFor(tech, temperature_k);
    const double kHysteresisDerate = 0.99;
    /* Each state pays its OWN refresh (E15) -- see stateWithRefreshMW. */
    const double active_mw = stateWithRefreshMW(s, s.idd3n);  // IDD3N, row open
    const double pre_mw    = stateWithRefreshMW(s, s.idd2n);  // IDD2N, precharged
    double pd_mw           = stateWithRefreshMW(s, s.idd2p);  // IDD2P, CKE low
    if (pd_mw > pre_mw) pd_mw = pre_mw;         // guard odd rows: never a penalty
    double idle_mw;
    if (pg_enabled) {
        const double r_pd = r_idle * kHysteresisDerate;
        idle_mw = pd_mw * r_pd + pre_mw * (r_idle - r_pd);
    } else {
        idle_mw = pre_mw * r_idle;
    }
    return active_mw * (1.0 - r_idle) + idle_mw;
}

/* The memory system's background: population x per-unit state-aware power.
 * This is the only function the report should call. */
inline double backgroundSystemMW(const std::string& tech, double r_idle,
                                 bool pg_enabled,
                                 const std::string& device_width = "",
                                 int ranks_per_channel = 1,
                                 int channels = 1,        // 1.11.52 (A015)
                                 int temperature_k = 358) {   // 1.11.65
    return backgroundUnitMW(tech, r_idle, pg_enabled, temperature_k) *
           static_cast<double>(backgroundUnits(baseTech(tech), device_width,
                                               ranks_per_channel, channels));
}

} // namespace pimid_energy
} // namespace Ramulator
#endif
