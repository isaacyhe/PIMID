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
// 1.11.91 (item 11, component-factor ruling 2026-09-26 19:30): the rows
// below STORE datasheet MAXIMA (HBM2/HBM3: measured silicon); the model
// prices TYPICAL currents derived from them by typicalFromSpec() with the
// row's ComponentFactors (componentFactorsFor). See the anchor block above
// the DDR5 rows and the function's own comment.
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
    /* 1.11.91 (audit R8-8, user ruling (a)): THE TWO RAILS THE VDD COLUMNS
     * DO NOT CARRY. Every held datasheet measures IDD on the VDD balls ONLY
     * and says so: Micron DDR5 core sheet p.448 "Any IPP or IDDQ current is
     * not included in IDD currents"; MT40A p.314 "IPP and IDDQ currents are
     * not included in IDD currents"; JESD238B.01 cl.9.1 (PDF p.164) IDD on
     * VDDC, IPP on VPP, IDDQ on VDDQ; JESD250D cl.8.8 (PDF p.164) "IPP
     * currents are not included in IDD currents". So the DQ output rail
     * (IDDQ) and the wordline pump rail (IPP) are SEPARATE terms, and until
     * this release neither was priced.
     *
     * Per-unit currents on the same basis (device / HBM channel) as the
     * columns above, each with its own rail voltage. A value < 0 means the
     * row's source publishes no such number: the consumer prices nothing
     * for it and prints n/a -- no default is substituted. The defaults below
     * are that sentinel, so a row written before 1.11.91 (or one whose sheet
     * has no IDDQ/IPP row) states the gap rather than inventing a zero. */
    double iddq3n = -1.0;   // mA on VDDQ, active standby (the burst's baseline)
    double iddq4r = -1.0;   // mA on VDDQ, burst read
    double iddq4w = -1.0;   // mA on VDDQ, burst write
    double vddq   = -1.0;   // V
    double ipp2n  = -1.0;   // mA on VPP, precharge standby
    double ipp3n  = -1.0;   // mA on VPP, active standby
    double vpp    = -1.0;   // V
    /* 1.11.91 (item 11, user ruling 2026-09-26 18:44-19:00): an EXPLICIT
     * activate+precharge energy per IDD unit, pJ, for a row whose TN-41-01
     * difference (IDD0 x tRC - IDD3N x tRAS - IDD2N x (tRC - tRAS)) is not
     * the route its derivation chose. < 0 = unset: the formula is used, as
     * on every row before this release. arrayReadNJ/arrayWriteNJ consult it
     * before computing the difference. Set on GDDR6 only (see that row).
     * Stored at the maxima like the currents; typicalFromSpec() scales it
     * by the activate-excess factor (19:30 component ruling). */
    double e_actpre_pJ_override = -1.0;
    /* 1.11.91 (item 12, user ruling option (c)): a per-STACK static power,
     * mW, priced once per stack (not per channel) and added to the
     * memory system's background by backgroundSystemMW /
     * backgroundSystemStatesMW. < 0 = none (every non-HBM row). HBM2 and
     * HBM3 only; the measurement and its reading are at the HBM2 row. */
    double stack_floor_mw = -1.0;
};

/* 1.11.91 (item 11, user ruling 2026-09-26 18:44-19:00): ONE WORD OF
 * PROVENANCE PER IDD ROW, printed on the model-inputs line and nothing
 * more. The chain, the anchors, the factors and the bands live in the
 * comment beside each row (and the changelog), never in a print-out.
 *   MEASURED    silicon measurement at the row's own basis (HBM2); no
 *               component factors
 *   DERIVED     datasheet maxima x the two-generation component factors
 *               (DDR5-3200/4800/5600, LPDDR5, LPDDR5X, GDDR6), or measured
 *               silicon of another part by a stated chain (HBM3; no
 *               component factors)
 *   CALIBRATED  this part's datasheet maxima x component factors measured
 *               on the same generation (DDR3 [G], DDR4 [S])
 * nullptr = no word (a key with no row: the unknown-technology fallback);
 * the caller prints nothing for it. [1.11.91, 19:30 ruling: the DDR5 rows
 * are reached -- the component rule keeps their activate term positive.] */
inline const char* iddRowProvenance(const std::string& key) {
    if (key == "HBM2") return "MEASURED";
    if (key == "DDR3" || key == "DDR4") return "CALIBRATED";
    if (key == "LPDDR5" || key == "LPDDR5X" || key == "GDDR6" || key == "HBM3")
        return "DERIVED";
    if (key == "DDR5" || key == "DDR5-3200" || key == "DDR5-4800" ||
        key == "DDR5-5600")
        return "DERIVED";
    return nullptr;
}

/* 1.11.91 (audit R8-2, user ruling (a)): WHICH BANK STATE IDD3N WAS
 * MEASURED IN, PER TECHNOLOGY.
 *
 * The 1.11.86 one-bank baseline (oneBankActiveStandbyMA, below) assumed that
 * every row's IDD3N is an ALL-banks-open current and diluted it to one bank:
 * IDD2N + (IDD3N - IDD2N) / banks. That assumption is TRUE for the DDR
 * classes and FALSE for HBM2, HBM3 and GDDR6, whose standards specify IDD3N
 * with ONE bank active already -- so for them the formula divided an
 * already-one-bank increment by 32 or 16 again, and the activate/precharge
 * term subtracted too little standby. The conditions, quoted from the held
 * documents (page = PDF page; printed page where it differs):
 *
 *   ALL_BANKS
 *   DDR5  Micron DDR5 core sheet (ddr5_sdram_core.pdf Rev. D 10/2022)
 *         Table 387 p.450: IDD2N "Bank Activity: All banks closed"; IDD3N
 *         "Bank Activity: All banks open". p.451: IDD4R "Bank Activity: All
 *         banks open, RD commands cycling through banks"; IDD4W likewise
 *         with WR commands.
 *   DDR4  Micron MT40A (8gb_ddr4_dram.pdf Rev. N 06/18) Table 136:
 *         p.316 IDD2N "Bank activity: all banks closed"; p.317 IDD3N "Bank
 *         activity: all banks open"; IDD4R "Bank activity: all banks open,
 *         RD commands cycling through banks: 0, 0, 1, 1, 2, 2, ...".
 *   DDR3  Micron MT41K (4Gb_DDR3L.pdf Rev. Q 12/17) Table 13 note 3 p.36:
 *         "All banks closed during IDD2N; all banks open during IDD3N";
 *         Table 15 (IDD4R loop) note 4 p.37 and Table 16 (IDD4W loop) p.38:
 *         "All banks open".
 *
 *   ONE_BANK
 *   HBM2  JESD235D cl.9.1 (PDF p.109, printed p.101): Active Standby
 *         "tCK = tCK(min); one bank is active; CKE is HIGH" -> IDD3N. The
 *         HBM2 row is the SAFARI measurement of the JEDEC loops (see its
 *         comment), so it inherits the loop's condition.
 *   HBM3  JESD238B.01 Table 83 (PDF p.165, printed p.151): "Active Standby
 *         Current: tCK = tCK(min); one bank is active" -> IDD3N.
 *   GDDR6 JESD250D Table 65 (PDF p.165, printed p.153): "Active Standby
 *         Current: tCK = tCK (min); tWCK = tWCK(min); one bank active" ->
 *         IDD3N. (The SK hynix sheet the audit also read is marked
 *         Confidential and is not cited, per the user's R8-9 ruling.)
 *
 *   UNVERIFIED
 *   LPDDR5 The public Micron Y52P LPDDR5X sheet (Table 18 PDF pp.42-43)
 *         lists the IDD values and on p.42 defers the conditions to
 *         "General LPDDR5/LPDDR5X Specifications 2", which is not held;
 *         the other held LPDDR5-class sheets likewise list values only; misc/JESD209-5C.pdf is an
 *         image-only PDF that cannot be read on this node. The 1.11.86
 *         formula is kept, and the basis is printed UNVERIFIED, until the
 *         JESD209-5C condition table is read.
 *
 * What each basis does to the activate/precharge baseline (IDD3N(1 bank),
 * the standby the IDD0 loop runs at):
 *   ALL_BANKS   IDD2N + (IDD3N - IDD2N) / banks   (the 1.11.86 dilution)
 *   ONE_BANK    IDD3N as specified
 *   UNVERIFIED  the 1.11.86 dilution, unchanged
 *
 * THE BURST TERMS' BASELINE. arrayReadNJ/arrayWriteNJ price a burst as
 * (IDD4R|IDD4W - IDD3N) x tBurst, i.e. against the standby the burst loop
 * was measured on top of. On the ALL_BANKS parts the IDD4R/IDD4W loops run
 * with all banks open (DDR5 p.451, DDR4 p.317, DDR3 pp.37-38, quoted
 * above), which is exactly the all-bank IDD3N: the subtraction is right
 * and is kept. On the ONE_BANK parts the burst loops do NOT run at the
 * one-bank state: JESD235D (PDF p.109) and JESD238B.01 (PDF p.166, printed
 * p.152) IDD4R/IDD4W "all banks activated"; JESD250D Table 65 (PDF p.165)
 * IDD4R/IDD4W "one bank in each of the 4 bank groups activated". Neither
 * standard specifies a standby current at that many open banks, so the
 * matching baseline has no source; the one-bank IDD3N -- the only active
 * standby they specify -- is subtracted, and the burst term therefore
 * carries the (unspecified) extra standby of the other open banks for the
 * burst's duration. On HBM2's measured silicon that extra is inside the
 * measurement noise (IDD3N 133 <= IDD2N 136 mA); on HBM3/GDDR6 it is not
 * bounded by any held document. Stated, not corrected. */
enum class Idd3nBasis { ALL_BANKS, ONE_BANK, UNVERIFIED };
inline Idd3nBasis idd3nBasisFor(const std::string& base_tech) {
    if (base_tech == "DDR3" || base_tech == "DDR4" || base_tech == "DDR5")
        return Idd3nBasis::ALL_BANKS;
    if (base_tech == "HBM2" || base_tech == "HBM3" || base_tech == "GDDR6")
        return Idd3nBasis::ONE_BANK;
    return Idd3nBasis::UNVERIFIED;   // LPDDR5, and anything without a row
}
inline const char* idd3nBasisName(Idd3nBasis b) {
    switch (b) {
        case Idd3nBasis::ALL_BANKS: return "ALL_BANKS";
        case Idd3nBasis::ONE_BANK:  return "ONE_BANK";
        default:                    return "UNVERIFIED";
    }
}

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
//   DDR5  Micron 16Gb DDR5-4800 (VDD 1.1);  DDR4 Micron 8Gb DDR4-2400 (1.2),
//         MT40A Die Rev B Table 148 (HELD, 1.11.91);
//   DDR3  Micron 4Gb DDR3L-1600 (1.35), MT41K Die Rev E Table 20 (HELD,
//         1.11.91);                        GDDR6 Samsung K4Z80325BC Table 82
//         (1.35; 1.11.91, was "Micron 8Gb GDDR6-14000", not held);
//   HBM2  SAFARI HBM-Power measurement (1.2); HBM3 derived from it (1.1);
//   LPDDR5 Micron Y52P LPDDR5X 16 Gb die, Table 18, 7500 Mb/s x16 (VDD2H
//          1.05), burst rate-scaled to 6400 -- 1.11.91 (the 1.11.63 source,
//          an 8 Gb LPDDR5-6400 sheet, was ruled not citable 2026-09-26).
//   1.11.91 item 11: every row stores its maxima (HBM: measurement); the
//   typical currents the model prices are typicalFromSpec() of them.
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
     * Keep it, but do not quote it as sourced -- the consumer emits a note.
     * 1.11.91 (R8-9): the DDR3 and DDR4 entries ARE now datasheet reads --
     * DDR3 32 mA = MT41K Rev E Table 20 IDD2P1 (fast exit) p.43, DDR4 25 mA
     * = MT40A Rev B Table 148 IDD2P p.332 -- and so are the two DDR5 16 Gb
     * entries (1.11.66, MT60B IDD2P). The rest of this note stands for the
     * HBM, GDDR6, LPDDR5 and DDR5-3200 entries. */
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
     *   DDR4 350.0. [1.11.91: the DDR4 CURRENTS are now read from the held
     *     MT40A Rev B (see the row); this tRFC is untouched by that.] The
     *     audit marks the whole DDR4 energy row UNCHECKABLE
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
    /* 1.11.91 (audit R8-8, user ruling (a)): the IDDQ and IPP rows of the
     * SAME two addenda, x8 column, read page-ranged. VDDQ = VDD = 1.1 V and
     * VPP = 1.8 V nominal: Micron DDR5 core sheet p.1 "VDD = VDDQ = 1.1V
     * (nom)", "VPP= 1.8V (nom)".
     *   Rev A (16gb_ddr5_sdram_dierevA.pdf Rev. D 02/2023) Table 6, 4800:
     *     IPP2N 6 (p.17), IPP3N 7 (p.18), IDDQ3N 31 (p.18),
     *     IDDQ4R 57 (p.19), IDDQ4W 198 (p.19) mA
     *   Rev D (Table 8), 5600:
     *     IPP2N 7 (p.18), IPP3N 8 (p.19), IDDQ3N 70 (p.19),
     *     IDDQ4R 218 (p.19), IDDQ4W 271 (p.20) mA
     * The IDDQ loops run under the same conditions as the IDD loops ("Same
     * conditions with IDD4R, however measuring IDDQ current", core sheet
     * p.451) -- all banks open, "Output Buffer and RTT: Enabled" -- so
     * IDDQ3N is the burst's own baseline, on the same basis as IDD3N.
     * TWO LIMITS OF THE SOURCE, stated rather than corrected:
     *  - core sheet p.448: "IDDQ values cannot be directly used to calculate
     *    IO power of the DDR5 SDRAM. They can be used to support correlation
     *    of simulated IO power to actual IO power". The user ruled to price
     *    them anyway (R8-8 (a)); they are the only published VDDQ numbers.
     *  - the IDDQ4W loop runs with the DRAM's RTT enabled (p.451; RTT_WR =
     *    RZQ/2 per the p.453 IDD notes), so the DC current of the write
     *    termination is drawn from the DRAM's VDDQ and is INSIDE IDDQ4W.
     *    terminationNJ() prices that same write loop, so on a write the two
     *    terms overlap by at most the termination term. Reads do not
     *    overlap: the read loop's DC path is fed from the receiver's VDDQ.
     * The 3200 row stays without IDDQ/IPP (no held sheet has a 3200
     * column, the same stated gap as its IDD). */
    /* 1.11.91 (item 11, user rulings 2026-09-26 18:44-19:00 and 19:30):
     * ONE ABSOLUTE VALUE PER QUANTITY, FROM MEASURED ANCHORS, BY COMPONENT.
     * Datasheet IDD values are guardbanded MAXIMA (worst process,
     * temperature, voltage); PIMID prints one typical number per quantity,
     * never a range. The rows below STORE the maxima (the datasheet as
     * printed); typicalFromSpec() (after iddTableFor) derives the typical
     * currents the model prices. Two anchors:
     *  [G] Ghose, Yaglikci, Gupta, Lee, Chandrasekar, Ma, Mutlu, "What Your
     *      DRAM Power Models Are Not Telling You: Lessons from a Detailed
     *      Experimental Study", Proc. ACM Meas. Anal. Comput. Syst.
     *      (SIGMETRICS) 2018, https://arxiv.org/abs/1807.05102 -- 50
     *      DDR3L-1600 modules, 3 vendors, measured/datasheet per current
     *      (vendor A/B/C): IDD2N 38.3/76.6/54.9%, IDD3N 23.4/53.2/33.4%,
     *      IDD0 40.2/42.6/45.4%, IDD4R (I/O-corrected) 45.9/79.5/95.4%,
     *      IDD4W 49.1/54.5/59.0%, IDD5B 88.6/72.0/88.0%, IDD7 58.4/43.5/
     *      52.7%; IDD2P1 only as ranges. Three-vendor means: IDD0 0.427,
     *      IDD2N 0.566, IDD3N 0.367, IDD4R 0.736, IDD4W 0.542, IDD5B 0.829.
     *  [S] Shi et al., "Calibrating DRAMPower Model for HPC: A Runtime
     *      Perspective from Real-Time Measurements", arXiv:2411.17960 (v3)
     *      -- Samsung M393A1G43DB0-CPB DDR4-2133 8 GB RDIMMs, HDEEM sensors
     *      on a Haswell HPC node; Fig. 4(b) datasheet -> regression-
     *      calibrated per DIMM: IDD0 1120->605, IDD2N 1040->599, IDD3N
     *      1540->539, IDD4R 1750->967, IDD4W 1590->684 mA; refresh not
     *      calibrated. Ratios: IDD0 0.540, IDD2N 0.576, IDD3N 0.350, IDD4R
     *      0.553, IDD4W 0.430. Energy-level measured/model ratios 0.25-0.51
     *      (median 0.47).
     * WHY BY COMPONENT (19:30 ruling). The model prices DIFFERENCES of the
     * IDD columns (TN-41-01: activate = IDD0 loop minus its standby
     * baseline; burst = IDD4R/W minus IDD3N; refresh = IDD5B minus the
     * state it interrupts), so a factor must act on the quantity each IDD
     * loop measures, not on each absolute current. Per-current factors
     * (the first pass of this item) broke three things: DDR5 IDD0 x 0.48
     * fell below IDD2N x 0.57 (4800B 49.4 < 52.4; 5600B 25.4 < 27.9), so
     * the activate term went negative and the run was refused; IDD3N x
     * 0.36 fell below IDD2N x 0.57 on every factored row (the R8-11 guard
     * then priced the precharged state at the active one); and IDD2P x
     * 1.00 sat above the factored IDD2N (power-down saved nothing).
     * THE COMPONENTS AND THE TWO-GENERATION FACTORS (for a generation
     * neither anchor measured; ASSUMPTION: vendors guardband it as they
     * guardbanded DDR3L and DDR4):
     *   precharged standby (IDD2N)         0.57  [G] 0.566, [S] 0.576
     *   active-standby premium             0.36/0.57: IDD3N_t = IDD2N_t +
     *     (IDD3N - IDD2N) x 0.36/0.57, clamped >= IDD2N_t -- the premium
     *     shrinks by the ratio of the two measured standby factors ([G]
     *     IDD3N 23-53% of spec, IDD2N 38-77%: measured active standby sits
     *     barely above precharged)
     *   activate excess (IDD0 loop minus its TN-41-01 standby baseline)
     *                                      0.5   [S] energy ratios 0.25-
     *     0.51 (median 0.47); [G] IDD0 loop 0.43
     *   burst excess (IDD4R - IDD3N), (IDD4W - IDD3N)
     *                                      0.6 read, 0.5 write  [G] 0.74/
     *     0.54 (I/O-corrected), [S] 0.55/0.43
     *   refresh excess (IDD5B - IDD2N)     0.83  [G] 88.6/72.0/88.0%
     *   precharge power-down (IDD2P)       0.57  same as standby ([G] gives
     *     ranges only); keeps IDD2P_t <= IDD2N_t
     * Same-generation sets: DDR3 from [G] alone (standby 0.566, premium
     * 0.367/0.566, activate 0.43 = [G]'s IDD0 loop ratio, burst 0.736/
     * 0.542, refresh 0.829, pd 0.566; CALIBRATED); DDR4 from [S] where it
     * has the ratio (standby 0.576, premium 0.350/0.576, activate 0.540 =
     * [S]'s IDD0 ratio applied to the excess, burst 0.553/0.430, pd 0.576;
     * refresh 0.83 from [G]; CALIBRATED). DDR5 (all grades), LPDDR5,
     * LPDDR5X and GDDR6 take the two-generation set (DERIVED). HBM2
     * (MEASURED) and HBM3 (DERIVED from that measurement) take NONE: they
     * are measured-typical already. IDDQ, IPP, tRFC and tREFI are not
     * factored (no anchor covers them).
     * BANDS (comment only): the per-vendor spread of [G] is the band of a
     * factor, e.g. IDD3N 0.23-0.53, IDD4R 0.46-0.95.
     *
     * DDR5 (both 16 Gb grades and the 8 Gb 3200 row): the stored rows are
     * the maxima; under the component rule the activate term is f.act x
     * the spec row's (positive, the 1.11.86 one-bank baseline), so the
     * per-current failure above no longer arises. The 3200 row's IDD is
     * unsourced (stated below); it is factored like the others. */
    if (tech == "DDR5-3200") return {1.1, 55, 34, 42,148,168,120, 195.0, 3900.0, 1, 20};   // 8 Gb, UNSOURCED IDD (stated)
    if (tech == "DDR5-5600") {
        IDDSpec s{1.1, 53, 49, 91,218,241,377, 295.0, 3900.0, 1, 47};   // MT60B Rev D T8, 16 Gb
        s.iddq3n = 70; s.iddq4r = 218; s.iddq4w = 271; s.vddq = 1.1;  // Rev D T8 pp.19-20
        s.ipp2n = 7; s.ipp3n = 8; s.vpp = 1.8;                         // Rev D T8 pp.18-19
        return s;
    }
    if (tech == "DDR5" || tech == "DDR5-4800") {
        IDDSpec s{1.1,103, 92,142,377,349,277, 295.0, 3900.0, 1, 88};   // MT60B Rev A T6, 16 Gb (default)
        s.iddq3n = 31; s.iddq4r = 57; s.iddq4w = 198; s.vddq = 1.1;    // Rev A T6 pp.18-19
        s.ipp2n = 6; s.ipp3n = 7; s.vpp = 1.8;                          // Rev A T6 pp.17-18
        return s;
    }
    /* [1.11.91: SUPERSEDED by the Rev B row below -- the 362 converted a
     * Rev A IDD5R against a standby that is not Rev A's. Kept as history.]
     * 1.11.66 (round 5, F5): idd5 155 -> 362 mA. The 155 had no source.
     * Micron MT40A (misc/) tabulates IDD5R -- the DISTRIBUTED refresh current,
     * one REF every tREFI averaged in (p.318 definition) -- not the burst
     * IDD5B this column means. Converting the same Rev A row the other
     * columns come from: IDD5B = IDD2N + (IDD5R - IDD2N) x tREFI / tRFC =
     * 50 + 14 / (350/7800) = 362 mA. That is EXACTLY the IDD5B upstream
     * Ramulator2 ships in DDR4.cpp's Default current preset, which is how
     * upstream derived it too -- an independent reproduction. Refresh
     * excess over IDD3N goes 113 -> 320 mA (2.8x); the printed refresh line
     * had been 2.7x low. */
    /* 1.11.91 (audit R8-9, user ruling 16:42-16:49): THE DDR4 ROW IS ONE
     * REVISION OF ONE SHEET. The row this replaces ({58,35,42,140,150},
     * IDD5 362 derived from Rev A's IDD2N) matched neither held MT40A
     * revision -- the activate term came out 1.7-2.3x high -- and its IDD5
     * mixed a Rev A IDD5R with another revision's standby. Every number now
     * comes from Micron MT40A (8gb_ddr4_dram.pdf Rev. N 06/18) Table 148
     * "IDD, IPP, and IDDQ Current Limits; Die Rev. B", x8, DDR4-2400
     * column, p.332:
     *     IDD0 48   IDD2N 34   IDD3N 43   IDD4R 135   IDD4W 123
     *     IDD2P 25  IDD5R 53 (x4, x8)     IPP3N 3 (ALL)       mA
     * IDD5 (this column is the BURST refresh current IDD5B, see the 1.11.66
     * note above) is converted from THAT revision's IDD5R and IDD2N by the
     * same identity: IDD5B = IDD2N + (IDD5R - IDD2N) x tREFI / tRFC =
     * 34 + 19 x 7800 / 350 = 457.4 mA.
     * IPP: Table 148 publishes IPP3N only, and its note 22 (p.334) reads
     * "IPP3N test and limit is applicable for all IDD2x, IDD3x, IDD4x and
     * IDD8 conditions" -- so IPP2N = IPP3N = 3 mA. VPP = 2.5 V (p.1
     * "VPP = 2.5V, -125mV, +250mV").
     * IDDQ: NOT PUBLISHED. The table's title names IDDQ, but its rows
     * (pp.332-333) carry no IDDQ entry, and p.314 defines only IDDQ2NT.
     * The IDDQ fields stay unset; the report prints iddq= n/a for DDR4. */
    /* 1.11.91 (item 11, 19:30 component ruling): CALIBRATED. The row
     * stores the Table 148 MAXIMA; typicalFromSpec() applies Shi's DDR4
     * component factors [S] (componentFactorsFor): standby 0.576, premium
     * 0.350/0.576, activate excess 0.540, burst excess 0.553 read / 0.430
     * write, power-down 0.576; refresh excess 0.83 from Ghose [G] (Shi did
     * not calibrate refresh). The RATIOS are used, not Shi's per-DIMM
     * absolute values, because this row is per x8 device of a different
     * part (MT40A 8 Gb Rev B, DDR4-2400) while Shi measured whole Samsung
     * 8 GB DDR4-2133 RDIMMs (HDEEM sensors, regression against DRAMPower).
     * Typical currents (static part): IDD2N 19.584, IDD3N 19.584 + 9 x
     * 0.6076 = 25.053, IDD4R 25.053 + 92 x 0.553 = 75.929, IDD4W 25.053 +
     * 80 x 0.430 = 59.453, IDD5B 19.584 + 423.4 x 0.83 = 371.006, IDD2P
     * 14.4 mA. IPP is left at the sheet's value (no anchor calibrates
     * IPP). */
    if (tech == "DDR4") {
        IDDSpec s{1.2, 48,34,43,135,123,457.4, 350.0, 7800.0, 1, 25};   // MT40A Rev B T148 p.332, x8, 2400 (MAXIMA; typical via typicalFromSpec)
        s.ipp2n = 3; s.ipp3n = 3; s.vpp = 2.5;                          // T148 p.332 + note 22 p.334; p.1
        return s;
    }
    /* 1.11.91 (audit R8-9, user ruling): THE DDR3 ROW IS ONE REVISION OF
     * ONE SHEET. Micron MT41K (4Gb_DDR3L.pdf Rev. Q 12/17) Table 20 "IDD
     * Maximum Limits Die Rev. E for 1.35/1.5V Operation", p.43, x8,
     * DDR3/3L-1600 column:
     *     IDD0 55   IDD2N 32   IDD3N 38   IDD4R 157   IDD4W 125
     *     IDD5B 235 (burst refresh, the column's meaning)             mA
     *     IDD2P1 32 ("Precharge power-down current: Fast exit")
     * The old row carried IDD4W 180 (+44%), IDD4R 175, IDD0 60, IDD3N 45
     * and IDD2P 18 -- and 18 is Table 20's IDD2P0, the SLOW-exit current,
     * while the column's own comment (and the power-down model: CKE low,
     * fast tXP exit) says fast exit. The fast-exit figure equals IDD2N on
     * this part (32 = 32), so precharge power-down saves nothing at
     * DDR3L-1600 fast exit; that is the sheet's number. DDR3 has no VPP
     * rail (MT41K p.1: "VDD = VDDQ = 1.35V") and Table 20 has no IDDQ rows:
     * both stay unset. */
    /* 1.11.91 (item 11, 19:30 component ruling): CALIBRATED. The row
     * stores the Table 20 MAXIMA; typicalFromSpec() applies Ghose's
     * three-vendor DDR3L-1600 component factors [G] -- the same
     * generation, speed and 1.35 V class the anchor measured: standby
     * 0.566, premium 0.367/0.566, activate excess 0.43 ([G]'s IDD0 loop
     * ratio), burst excess 0.736 read / 0.542 write, refresh excess 0.829,
     * power-down 0.566. Typical currents (static part): IDD2N 18.112,
     * IDD3N 18.112 + 6 x 0.6484 = 22.002, IDD4R 22.002 + 119 x 0.736 =
     * 109.586, IDD4W 22.002 + 87 x 0.542 = 69.156, IDD5B 18.112 + 203 x
     * 0.829 = 186.399, IDD2P 18.112 mA (fast exit; = IDD2N_t, as on the
     * sheet). Band (comment only): per-vendor ratios, e.g. IDD3N
     * 0.234-0.532. */
    if (tech == "DDR3")   return {1.35,55,32,38,157,125,235, 350.0, 7800.0, 1, 32};   // MT41K Rev E T20 p.43, x8, 1600 (MAXIMA; typical via typicalFromSpec)
    /* LPDDR5 row history. 1.11.63 re-based the column on an 8 Gb LPDDR5-6400
     * Micron sheet in misc/; the user ruled on 2026-09-26 that that sheet is
     * marked confidential on every page and is NOT to be cited, so its part
     * number, pages and values are no longer quoted anywhere in this file.
     * It survives only as the uncited cross-check named below. (Pre-1.11.63
     * the row traced to no held document; its burst currents were 3-4x
     * below every held LPDDR5-class part.)
     *
     * 1.11.91 (item 11, user ruling 2026-09-26): DERIVED from a PUBLIC sheet.
     * SOURCE: Micron Y52P LPDDR5X SDRAM, MT62F1G32D2 family
     * (misc/315b-441b-561b-563b-y52p-sdp-ddp-qdp-8dp-lpddr5x.pdf, Rev. H
     * 03/2025, no confidentiality marking; footer "Downloaded from
     * Arrow.com"), 16 Gb die, Table 18 "WT IDD Parameters - Single Die",
     * PDF pp.42-43, x16 mode, 7500 Mb/s column, maxima over process,
     * temperature and voltage (note 1), BG mode, DVFSC/DVFSQ disabled
     * (note 2), BL16 (note 3):
     *   VDD2H  IDD02H 53  IDD2N2H 32.5  IDD3N2H 40  IDD4R2H 390  IDD4W2H 265
     *          IDD52H 205 (all-bank refresh)  IDD2P2H 3.9            mA
     *   VDD1   IDD01 3.8  IDD2N1 1.5  IDD3N1 2.8  IDD4R1 18.0  IDD4W1 16.5
     *   VDDQ   IDD3NQ 0.6  IDD4RQ 111.9 (typical, note 4)  IDD4WQ 0.6
     * Rails (p.1): VDD1 1.80 V, VDD2H 1.05 V TYP (1.01-1.12), VDDQ 0.50 V
     * TYP (ODT on). Note 6: this vendor's IDD4R2/IDD4W2 pattern is "more
     * stringent than required by JEDEC", so they read slightly high.
     * CHAIN to the simulated LPDDR5-6400 x16 channel:
     *  1. standby and activate currents as-is (rate-independent);
     *  2. burst excess at constant energy per bit, x 6400/7500 = 0.8533:
     *       IDD4R 40 + (390 - 40) x 0.8533 = 338.67
     *       IDD4W 40 + (265 - 40) x 0.8533 = 232.00
     *  3. [19:30 component ruling] the row STORES the result of steps
     *     1-2 (maxima); typicalFromSpec() applies the two-generation
     *     component factors (anchors [G], [S] at the DDR5 rows; assumption:
     *     LPDDR5 guardbanded as DDR3L and DDR4 were): IDD2N 18.525, IDD3N
     *     18.525 + 7.5 x 0.6316 = 23.262, IDD4R 23.262 + 298.67 x 0.6 =
     *     202.46, IDD4W 23.262 + 192.0 x 0.5 = 119.26, IDD5B 18.525 + 172.5
     *     x 0.83 = 161.70, IDD2P 3.9 x 0.57 = 2.223 mA; activate = 0.5 x
     *     the TN-41-01 term of the stored row.
     * STATED MISMATCHES: the sheet is a 16 Gb die at 7500/8533 Mb/s; the
     * emitted org is LPDDR5_8Gb_x16 at 6400. The burst is rate-scaled
     * (step 2); standby, activate and refresh currents are the 16 Gb
     * die's, unscaled. trfc_ns stays 210 (the LPDDR5.cpp 8 Gb timing
     * identity; this sheet's tRFCab is 280 ns at 16 Gb, Table 6 p.11).
     * UNCITED CROSS-CHECK (confidential sheet): agrees within ~15% on the
     * burst and standby currents; its refresh and power-down currents are
     * lower by about a fifth and a third.
     * IDDQ (R8-8, VDDQ rail, charged only on accesses that cross the DQ):
     * IDD3NQ 0.6, IDD4RQ 111.9, IDD4WQ 0.6 mA at 0.5 V, NOT factored (no
     * anchor covers VDDQ, and IDD4RQ is already a typical value) and NOT
     * rate-scaled (the ruling scales the VDD2H burst only; stated).
     * VDD1 in the IPP slot (R8-8; LPDDR5 has no VPP ball, the 1.8 V VDD1 is
     * the part's high-voltage rail): IDD2N1 1.5 / IDD3N1 2.8 mA, as-is.
     * The per-access VDD1 increments (IDD4R1 18.0, IDD4W1 16.5) and VDD2L
     * (0.2 mA in every state) stay unpriced, as before. */
    if (tech == "LPDDR5") {
        IDDSpec s{1.05, 53, 32.5, 40,
                  40 + (390 - 40) * 6400.0 / 7500.0,
                  40 + (265 - 40) * 6400.0 / 7500.0,
                  205, 210.0, 3906.0, 1, 3.9};   // Y52P T18 x16 7500 MAXIMA, burst x 6400/7500 (1.11.91 item 11; typical via typicalFromSpec); tREFI 3906 = LPDDR5.cpp tREFI_BASE (1.11.66 L7)
        s.iddq3n = 0.6; s.iddq4r = 111.9; s.iddq4w = 0.6; s.vddq = 0.5;   // Y52P T18 pp.42-43, VDDQ p.1
        s.ipp2n = 1.5; s.ipp3n = 2.8; s.vpp = 1.8;                         // Y52P T18 p.42 (VDD1), VDD1 p.1
        return s;
    }
    /* 1.11.91 (item 11): LPDDR5X, the same public Y52P sheet at its own
     * 7500 Mb/s x16 column, WITHOUT rate scaling. The row stores the
     * MAXIMA; typicalFromSpec() applies the two-generation component
     * factors. DERIVED.
     * NOT SELECTABLE, AND WHY: the technology needs an LPDDR5X timing
     * preset, and no public sheet held or fetched gives the AC timing set.
     * The Y52P sheet defers every AC timing but the density-dependent
     * refresh set (Table 6 p.11: tRFCab 280, tRFCpb 140, tPBR2PBR 90,
     * tPBR2ACT 7.5 ns at 16 Gb) to "General LPDDR5/LPDDR5X Specifications",
     * which is not held. The newer public Micron automotive LPDDR5X sheet
     * (561b Y52Q DDP, MT62F512M64D2, Rev. G 05/2025, fetched 2026-09-26
     * from mouser.com Micron_10-16-2025_561b-y52q-ddp-auto-lpddr5x.pdf, 33
     * pp.) was read in full for the same purpose: it lists IDD Tables
     * 12-16, a density-dependent refresh Table 4 (8 Gb die: tRFCab 210,
     * tRFCpb 120, tPBR2PBR 90, tPBR2ACT 7.5 ns) and a 16-bank extended-
     * frequency Table 8 at 3733/4267 Mb/s only (RL/WL/nWR), and defers the
     * rest to the same General Specifications 2/3 (p.3). Neither sheet
     * gives tRCD, tRP, tRAS, tRC, tREFI, tWR, tRRD or tFAW at any bin, nor
     * RL/WL at 7500 or 8533 Mb/s. No technology string reaches this row
     * until a preset can be written from a citable source.
     * Typical (component factors, for the record): IDD2N 18.525, IDD3N
     * 23.262, IDD4R 23.262 + 350 x 0.6 = 233.26, IDD4W 23.262 + 225 x 0.5
     * = 135.76, IDD5B 161.70, IDD2P 2.223 mA.
     * trfc 280 ns (Y52P Table 6 p.11, 16 Gb); trefi 3906 ns is the LPDDR5
     * row's value and must be re-checked against the preset when it is
     * written. */
    if (tech == "LPDDR5X") {
        IDDSpec s{1.05, 53, 32.5, 40, 390, 265, 205,
                  280.0, 3906.0, 1, 3.9};   // Y52P T18 x16 7500 MAXIMA (1.11.91 item 11; typical via typicalFromSpec)
        s.iddq3n = 0.6; s.iddq4r = 111.9; s.iddq4w = 0.6; s.vddq = 0.5;
        s.ipp2n = 1.5; s.ipp3n = 2.8; s.vpp = 1.8;
        return s;
    }
    /* 1.11.91 (R8-8): GDDR6 IDDQ/IPP PLACEHOLDER. JESD250D gives the rail
     * (Table 62 printed p.151: "Pump voltage VPP 1.746 1.8 1.908 V") and
     * the condition (Table 65 note 2 printed p.153: "IPP3N test and limit
     * is applicable for all IDD2X, IDD3X, IDD4X and IDD6 conditions") but
     * no values, and it defines no IDDQ at all (cl.8.8: IDD on the VDD
     * balls, IPP on the VPP balls). The R8-9 derivation of this row fills
     * iddq3n/iddq4r/iddq4w/vddq and ipp2n/ipp3n/vpp here, as IDDSpec
     * fields on the row below; every consumer reads them from the struct,
     * so no code changes when they arrive. Until then the fields keep the
     * unset sentinel and the report prints n/a. [1.11.91 item 11: still
     * unset. Samsung Table 82 lists IPP0 14, IPP3N 10, IPP5 77, IPP7 31 mA
     * but no IPP2N, and no anchor calibrates IPP; not priced.] */
    /* 1.11.91 (item 11, R8-9/R8-5, user ruling 2026-09-26 18:44-19:00).
     * DERIVED. The old row (70/45/60/210/230/180) cited a Micron 8 Gb
     * GDDR6-14000 sheet the tree does not hold.
     * SOURCE OF THE MAXIMA: Samsung K4Z80325BC, 8Gb GDDR6 SGRAM C-die, Rev.
     * 1.3 Jan 2020 (misc/52839ef626add36b171d2cdf17bacfd2.pdf; no
     * confidentiality marking on any of its 187 pages), Table 82 PDF p.146,
     * "IDD Specifications ... @ VDD&VDDQ=1.35V", x32 mode (2 ch x16), column
     * HC14 = 14 Gb/s, per DEVICE:
     *   IDD0 430  IDD2N 310  IDD3N 460  IDD4R 1220  IDD4W 1470
     *   IDD5 790 (at tRFCab min = burst)  IDD2P 230 (PLL/DLL off)  IDD7 1640
     * Same part the tree simulates: 8 Gb, 14000, 1.35 V; Table 91 IDD AC
     * set tRC 45 / tRAS 30 / tRFCab 120 ns = GDDR6_2000_1350mV_double.
     * PER CHANNEL, NOT PER DEVICE. "Measurements are taken per device with
     * the same IDD Measurement-Loop Patterns on both channels" (p.144 =
     * JESD250D cl.8.8 printed p.152), so one x16 channel draws half the
     * device figure. A 64 B access engages ONE channel (accessPathFor:
     * 16-bit channel, BL16 x 16 b = 32 B, 2 bursts), and backgroundUnits()
     * counts ranks x channels with the preset's 2 channels per device, so
     * the row is written per channel: burst per access = half current x 2
     * bursts = the device figure x 1 burst (basis-neutral, IDD_DERIVATION
     * section 2.2); background and refresh = 2 units x half = per device.
     * The row STORES (device maximum / 2); typicalFromSpec() applies the
     * two-generation component factors (anchors [G], [S] at the DDR5 rows;
     * assumption: GDDR6 is guardbanded as DDR3L and DDR4 were) [19:30
     * component ruling]: IDD2N 155 x 0.57 = 88.35, IDD3N 88.35 + 75 x
     * 0.6316 = 135.72, IDD4R 135.72 + 380 x 0.6 = 363.72, IDD4W 135.72 +
     * 505 x 0.5 = 388.22, IDD5B 88.35 + 240 x 0.83 = 287.55, IDD2P 115 x
     * 0.57 = 65.55 mA (per channel).
     * IDD3N IS ONE BANK ACTIVE (T82; JESD250D T65 printed p.153): basis
     * ONE_BANK, the 1.11.86 /banks dilution does not apply (R8-2).
     * ACTIVATE: THE IDD7 ROUTE (e_actpre_pJ_override). Three routes, per
     * channel ACT+PRE of a 2 KB page, at the maxima:
     *   IDD0 (TN-41-01, one bank)  1.35 x (430x45 - 460x30 - 310x15) / 2
     *                              = 0.61 nJ -- ILL-CONDITIONED: a ~900
     *                              mA.ns difference of ~19000 mA.ns terms;
     *                              +-5% on IDD0 alone swings it -0.05..1.26
     *   IDD7 - IDD4R               1.35 V x (1640 - 1220) mA / 175 M ACT/s
     *                              per device (one ACT per channel every 20
     *                              nCK = 11.4 ns, Table 90; 2 x 87.6 M/s)
     *                              = 3.24 nJ -- WELL-CONDITIONED: the two
     *                              loops share the read stream and differ
     *                              only by the ACT/PRE traffic
     *   physics bracket            1.1-2.3 nJ
     * IDD7 is chosen because it is well-conditioned. The row stores the
     * maximum-derived 3240 pJ; typicalFromSpec() scales an override by the
     * activate-excess factor: 3240 x 0.5 = 1620 pJ per channel activation,
     * one per row miss (devicesPerAccess = 1 channel).
     * VDDQ termination is priced by terminationNJ (ZQ/2 = the IDD
     * conditions' ODT). Maxima kept for the record (per device): {430,310,
     * 460,1220,1470,790, IDD2P 230}; the band of each typical value is the
     * [G] per-vendor spread of its factor (comment only). */
    if (tech == "GDDR6") {
        IDDSpec s{1.35, 430/2.0, 310/2.0, 460/2.0, 1220/2.0,
                  1470/2.0, 790/2.0, 120.0, 1900.0, 1, 230/2.0};   // Samsung K4Z80325BC T82 HC14, per channel, MAXIMA (typical via typicalFromSpec)
        s.e_actpre_pJ_override = 3240.0;   // IDD7 route, per channel ACT, at the maxima (typicalFromSpec x f.act)
        return s;
    }
    /* 1.11.91 (audit R8, user ruling: DERIVE, band, record the chain;
     * item 11: one value per current, bands in this comment only).
     * DERIVED. THE HBM3 ROW IS DERIVED FROM MEASURED HBM2 SILICON BY JEDEC
     * RATIOS. It is a measured-mean base, not a datasheet maximum, so the
     * two-generation guardband factor does NOT apply.
     * The old row (30/18/22/90/100/70) cited JESD238 Table 90, which is an
     * EMPTY example table (JESD238B.01 printed p.159): it had no source.
     * BASE: CMU-SAFARI HBM-Power artifact (github.com/CMU-SAFARI/HBM-Power,
     * branch artifact; paper not yet public), 36 HBM2 stacks, U55C, per
     * channel = stack/8. Loops ran at 1.2 Gb/s/pin (tCK 1666.7 ps) on
     * pseudo-channel 0 only (file names "_pc0_"), so per PC:
     *   read burst 1.2 x (463.8-133.4) mA / 76.8 Gb/s = 5.16 pJ/bit
     *   write burst 1.2 x (355.6-133.4) / 76.8        = 3.47 pJ/bit
     *   activate 1.2 x 7.65 mA (per-chip paired) x 48.3 ns = 444 pJ / 1 KB
     *   refresh 1.2 x (189.3-135.9) x 266.7 ns = 17.1 nJ per PC-REF
     *   standby 38.0 mA/ch clocked + 783 mA/stack off-power (no_hbm_idd2)
     * TARGET: JESD238B.01 -- VDDC 1.1 V (T70), 64-bit channel = 2 PC x 32
     * (T1, cl.3.1.2), 16 banks/PC, 1 KB page/PC, 4 Gb/ch (T4), tRFCab 260
     * ns (T84), tREFI 3.9 us; 6.4 Gb/s -> 409.6 Gb/s per channel.
     * SCALING (band = product extremes):
     *   burst/bit x [(1-fio)(1.1/1.2)^2 kp + fio rio], fio 0.23 [0.10,0.25]
     *     (O'Connor MICRO'17 T3), kp [0.85,1] (SK hynix IEDM'23), rio
     *     [0.3,0.6] (judgement, VDDQL 0.4 V) -> 3.87 pJ/b [1.33,4.57]
     *     IDD4R-IDD3N 1443 mA [496,1701]; IDD4W-IDD3N 970 [334,1144]
     *   activate x (1.1/1.2)^1.5 [^1..^2] -> 389 pJ [190,833]; IDD0 =
     *     IDD2N + 389/(1.1 x 49.4) = IDD2N + 7.2 mA [3.5,15.3]
     *   refresh x2 PCs x (1.1/1.2)^1.5 -> IDD5B = IDD2N + 105 [25,110]
     *   standby 38.0 x 0.878 x (1600/600) + 783 x 0.878/16 = 132 [52,138]
     *   IDD3N = IDD2N (one bank, T83; HBM2 IDD3N1/IDD2N = 0.98 on silicon)
     *   IDD2P UNSOURCED: 0.25 x IDD2N [0.08..0.74 = LPDDR5..GDDR6 ratio]
     * These are EQUIVALENT VDDC currents carrying I/O energy too: JEDEC
     * IDD excludes IDDQ/IDDQL, and this tree prices no HBM I/O elsewhere.
     * CHECK: sequential read, miss 1/16 -> 3.92 pJ/bit [1.36,4.67];
     * SK hynix IEDM 2023 15-6: HBM2E ~4.3 pJ/bit, HBM3E 20% lower (~3.44).
     * The old row gave 0.26 pJ/bit.
     * STACK FLOOR (1.11.91 item 12, user ruling option (c)): the 783 mA
     * stack "off-power" of the base is NOT spread over the channels; it is
     * one per-stack static term (see the HBM2 row), scaled by core voltage:
     * 939.96 mW x 1.1/1.2 = 861.6 mW per stack (= 783.3 mA at 1.1 V). The
     * standby above therefore takes the floor-excluded centre: IDD2N =
     * IDD3N = 38.0 x 0.878 x (1600/600) = 89 mA [band 52-93], and every
     * column that is IDD2N + an increment follows it: IDD0 89 + 7.2 = 96.2,
     * IDD4R 89 + 1443 = 1532, IDD4W 89 + 970 = 1059, IDD5B 89 + 105 = 194,
     * IDD2P 0.25 x 89 = 22.25. The burst, activate and refresh ENERGIES are
     * unchanged by this (they are increments); only standby moves.
     * (The 132 / 139 / 1575 / 1102 / 237 / 33 figures above and in the
     * section-4 block carried 43 mA/ch of spread floor; superseded.)
     * IPP: n/a. The artifact's data/all_idd_measurements.csv "ipp" column
     * is NOT a VPP current: sources/fpga/DRAMBender/sources/api/
     * platform.cpp computes Power_VPP as the card's PCIe 12 V + 3.3 V input
     * power minus VCCINT, VCCINT_IO and HBM 1.2 V power (a whole-card
     * residual), and generate_standardized_csvs.py divides it by an assumed
     * VPP_VOLTAGE = 2.5. Its ~3.12 A per-stack standby level (chip means,
     * IDD2 loop) with +10..+120 mA of loop response is card load, not a
     * DRAM pump current, so it is not used for HBM2 or HBM3. */
    if (tech == "HBM3") {
        IDDSpec s{1.1, 89+7.2, 89, 89, 89+1443, 89+970, 89+105, 260.0, 3900.0, 16, 0.25*89};
        s.stack_floor_mw = 783.3 * 1.2 * (1.1 / 1.2);   // 861.6 mW per stack (1.11.91 item 12)
        return s;
    }
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
    /* 1.11.91 (item 11): MEASURED, AT THE PRESET'S OPERATING POINT. The
     * measurement's own operating point is not the preset's (IDD_DERIVATION
     * section 1.1): the artifact's sources/table3/configs/
     * HBM2_1.2Gbps_timing_BL4.json has "tCK_ps": 1666.7 (600 MHz = 1.2
     * Gb/s/pin), and every IDD test file the standardiser maps is
     * "hbm_idd*_..._0_1_2_3_4_5_6_7_pc0_..." -- channels 0-7, pseudo-
     * channel 0 only; program_idd4r_full() keeps one 64-bit PC busy. So the
     * measured burst moved 64 b x 1.2 Gb/s = 76.8 Gb/s per channel, while
     * the tree charges it at the preset's 2.4 Gb/s on both PCs = 128 b x
     * 2.4 = 307.2 Gb/s (64 B per tBurst 1.666 ns).
     * CONSTANT ENERGY PER BIT AT THE PRESET RATE: the burst excess over
     * IDD3N scales by 307.2 / 76.8 = 4 (means of 36 chips, / 8):
     *   IDD4R excess (463.8 - 133.4) x 4 = 1321.6 -> 1322 mA; 133 + 1322
     *   IDD4W excess (355.6 - 133.4) x 4 =  888.8 ->  889 mA; 133 + 889
     * i.e. read 1.2 x 1322 x 1.666 / 512 = 5.16 pJ/bit, the measurement's
     * own figure (the 1.11.66 row charged 1.29, 4x low). If the one-PC
     * reading of the artifact were wrong the factor is 2 (band, comment
     * only).
     * REFRESH: program_idd5() uses "int tRFC = 160;" = 266.7 ns (its
     * comment says 350 ns). The measured energy per REF is 1.2 x (189.3 -
     * 135.9) mA x 266.7 ns = 17.1 nJ. The row's trfc_ns stays 260 (the
     * HBM2.cpp timing identity, 1.11.63), so IDD5B is set to carry the
     * SAME energy per REF over that 260 ns: 136 + 53.4 x 266.7 / 260 =
     * 136 + 54.8 = 190.8 mA (was 189). The REF went to one PC; that is
     * not rescaled here (not ruled). [Item 12 below re-bases every column
     * on the floor-excluded per-channel increments; the burst and refresh
     * EXCESSES above carry over unchanged.]
     * STACK FLOOR (1.11.91 item 12, user ruling option (c)). The artifact's
     * data/no_hbm_idd2_measurements.csv averages 821.3 mA (1638 samples) for
     * IDD2 with ONE channel enabled and idle, against 1087.4 mA for the
     * all-channel IDD2 loop: (1087.4 - 821.3) / 7 = 38.0 mA per channel and
     * a 783.3 mA remainder that does not scale with channels (the
     * artifact's own sources/table3/configs/IDD_ours_allzeros.json names it
     * "off-power (778.1)"). READING: it is per STACK and inside the package
     * -- logic-die PHY, clock trees, always-on circuits; the sensed rail is
     * the stack supply (HBM_1V2 in platform.cpp), and the FPGA-side PHY runs
     * from VCCINT, which is not that rail. ASSUMPTION: all of it is drawn by
     * the stack. Priced as ONE absolute number per the user's single-value
     * rule: 783.3 mA x 1.2 V = 939.96 mW per stack, in every state, added
     * once per stack to the memory system's background (stack_floor_mw).
     * The per-channel columns are therefore the per-channel INCREMENTS:
     * (36-chip stack mean - 783.3) / 8 --
     *   IDD0  (1125.1 - 783.3) / 8 = 42.725
     *   IDD2N (1087.4 - 783.3) / 8 = 38.0125
     *   IDD3N (1067.3 - 783.3) / 8 = 35.5
     *   IDD4R 35.5 + (3710.1 - 1067.3) / 8 x 4 = 35.5 + 1321.4 = 1356.9
     *   IDD4W 35.5 + (2844.6 - 1067.3) / 8 x 4 = 35.5 +  888.65 = 924.15
     *   IDD5B 38.0125 + (1514.5 - 1087.4) / 8 x 266.7/260 = 38.0125 + 54.76
     * (the rate factor 4 and the refresh 266.7/260 as above). The floor
     * cancels in every difference, so burst and refresh energies are
     * unchanged; the activate term moves only because these are the
     * unrounded means (the 1.11.66 row carried 141/136/133): 409.8 ->
     * 373.6 pJ, i.e. the stated ill-conditioning of IDD0 - IDD3N on this
     * part. IDD2P 7 stays the un-sourced column.
     * IPP: n/a -- the artifact's "ipp" column is a card-power residual, not
     * a VPP current (see the HBM3 row). */
    if (tech == "HBM2") {
        IDDSpec s{1.2, (1125.1 - 783.3) / 8, (1087.4 - 783.3) / 8, (1067.3 - 783.3) / 8,
                  (1067.3 - 783.3) / 8 + (3710.1 - 1067.3) / 8 * 4.0,
                  (1067.3 - 783.3) / 8 + (2844.6 - 1067.3) / 8 * 4.0,
                  (1087.4 - 783.3) / 8 + (1514.5 - 1087.4) / 8 * (266.7 / 260.0),
                  260.0, 3900.0, 8,  7};
        s.stack_floor_mw = 783.3 * 1.2;   // 939.96 mW per stack (1.11.91 item 12)
        return s;
    }
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
/* 1.11.91 (item 11, user ruling 2026-09-26 19:30): TYPICAL CURRENTS BY
 * COMPONENT FACTORS. The table above stores datasheet MAXIMA (HBM2/HBM3:
 * measured silicon); the model prices typical currents. The factors act on
 * the quantities the IDD loops measure and this file's formulas consume --
 * differences, not absolute currents (the anchors, the factor table and
 * why per-current factors failed are in the block above the DDR5 rows):
 *   standby          precharged standby IDD2N
 *   standby_premium  the active-standby premium (IDD3N - IDD2N) shrinks by
 *                    the ratio of the two measured standby factors
 *   act              the activate excess: IDD0 loop minus its TN-41-01
 *                    standby baseline
 *   burst_rd/_wr     the burst excess (IDD4R - IDD3N), (IDD4W - IDD3N)
 *   refresh          the refresh excess (IDD5B - IDD2N)
 *   pd               precharge power-down IDD2P
 * apply == false: the row is already typical (HBM2 MEASURED, HBM3 DERIVED
 * from it) or is the unknown-technology fallback; typicalFromSpec returns
 * it unchanged. */
struct ComponentFactors {
    bool   apply           = false;
    double standby         = 1.0;
    double standby_premium = 1.0;
    double act             = 1.0;
    double burst_rd        = 1.0;
    double burst_wr        = 1.0;
    double refresh         = 1.0;
    double pd              = 1.0;
};
inline ComponentFactors componentFactorsFor(const std::string& key) {
    ComponentFactors f;
    if (key == "DDR3") {
        /* [G] alone (DDR3L-1600, the anchor's own generation): IDD2N 0.566,
         * IDD3N 0.367 -> premium 0.367/0.566, IDD0 loop 0.43, IDD4R 0.736
         * (I/O-corrected), IDD4W 0.542, IDD5B 0.829; pd = standby. */
        f.apply = true; f.standby = 0.566; f.standby_premium = 0.367 / 0.566;
        f.act = 0.43; f.burst_rd = 0.736; f.burst_wr = 0.542;
        f.refresh = 0.829; f.pd = 0.566;
    } else if (key == "DDR4") {
        /* [S] where it has the ratio (DDR4-2133): IDD2N 0.576, IDD3N 0.350
         * -> premium 0.350/0.576, IDD0 0.540 applied to the excess, IDD4R
         * 0.553, IDD4W 0.430; refresh 0.83 from [G] ([S] did not calibrate
         * refresh); pd = standby. */
        f.apply = true; f.standby = 0.576; f.standby_premium = 0.350 / 0.576;
        f.act = 0.540; f.burst_rd = 0.553; f.burst_wr = 0.430;
        f.refresh = 0.83; f.pd = 0.576;
    } else if (key == "DDR5" || key == "DDR5-3200" || key == "DDR5-4800" ||
               key == "DDR5-5600" || key == "LPDDR5" || key == "LPDDR5X" ||
               key == "GDDR6") {
        /* The two-generation set ([G] and [S]; assumption: guardbanded as
         * DDR3L and DDR4 were). */
        f.apply = true; f.standby = 0.57; f.standby_premium = 0.36 / 0.57;
        f.act = 0.5; f.burst_rd = 0.6; f.burst_wr = 0.5;
        f.refresh = 0.83; f.pd = 0.57;
    }
    return f;   // HBM2, HBM3, unknown: none
}
/* Declared here, defined (with its derivation) before arrayReadNJ. */
inline double oneBankActiveStandbyMA(const IDDSpec& s, int banks_per_device,
                                     Idd3nBasis basis);
/* The typical row. Each current is set so that the EXISTING formulas,
 * consuming it, reproduce the factored component exactly:
 *   idd2n_t = f.standby x idd2n
 *   idd3n_t = idd2n_t + f.standby_premium x (idd3n - idd2n), >= idd2n_t
 *   idd4r_t = idd3n_t + f.burst_rd x (idd4r - idd3n)
 *     -> burst read  vdd (idd4r_t - idd3n_t) tBurst = f.burst_rd x spec
 *   idd4w_t = idd3n_t + f.burst_wr x (idd4w - idd3n)       (same, write)
 *   idd5_t  = idd2n_t + f.refresh x (idd5 - idd2n)
 *     -> refresh excess over precharged standby = f.refresh x spec
 *   idd2p_t = f.pd x idd2p
 * IDD0. TN-41-01 as this file writes it (arrayReadNJ):
 *   E = vdd x (idd0 x tRC - I1 x tRAS - idd2n x (tRC - tRAS))
 *     = vdd x tRC x (idd0 - B),   B = (I1 x tRAS + idd2n x (tRC - tRAS)) / tRC
 * with I1 = oneBankActiveStandbyMA(row, banks, basis), the tRC-weighted
 * standby baseline the IDD0 loop runs on. Choosing
 *   idd0_t = B_t + f.act x (idd0 - B_s)
 * (B_s from the spec row, B_t from the typical row, same tRC/tRAS/banks/
 * basis) gives
 *   E_t = vdd x tRC x (idd0_t - B_t) = f.act x vdd x tRC x (idd0 - B_s)
 *       = f.act x E_spec
 * EXACTLY, at the tRC/tRAS/banks passed. The energy path (arrayReadNJ/
 * arrayWriteNJ, via iddTypicalAt) passes the simulated timing, so the
 * priced activate term is f.act x the spec term on every call, and has the
 * spec term's sign (positive on every row since 1.11.86). A timing-free
 * caller (tRC <= 0) gets idd0_t = idd2n_t + f.act x (idd0 - idd2n), the
 * tRAS -> 0 limit; no energy path consumes idd0 without the timing.
 * A row with an explicit e_actpre_pJ_override (GDDR6, the IDD7 route) has
 * the override scaled by f.act instead: the formula is not used there.
 * IDDQ, IPP, tRFC, tREFI, channels and stack_floor_mw pass through. */
inline IDDSpec typicalFromSpec(const IDDSpec& spec, const ComponentFactors& f,
                               Idd3nBasis basis, int banks_per_device = 1,
                               double tRC = 0.0, double tRAS = 0.0) {
    if (!f.apply) return spec;
    IDDSpec t = spec;
    t.idd2n = f.standby * spec.idd2n;
    t.idd3n = t.idd2n + f.standby_premium * (spec.idd3n - spec.idd2n);
    if (t.idd3n < t.idd2n) t.idd3n = t.idd2n;
    t.idd4r = t.idd3n + f.burst_rd * (spec.idd4r - spec.idd3n);
    t.idd4w = t.idd3n + f.burst_wr * (spec.idd4w - spec.idd3n);
    t.idd5  = t.idd2n + f.refresh  * (spec.idd5  - spec.idd2n);
    t.idd2p = f.pd * spec.idd2p;
    if (tRC > 0.0 && tRAS >= 0.0 && tRAS <= tRC) {
        const double b_s = (oneBankActiveStandbyMA(spec, banks_per_device, basis) * tRAS
                            + spec.idd2n * (tRC - tRAS)) / tRC;
        const double b_t = (oneBankActiveStandbyMA(t, banks_per_device, basis) * tRAS
                            + t.idd2n * (tRC - tRAS)) / tRC;
        t.idd0 = b_t + f.act * (spec.idd0 - b_s);
    } else {
        t.idd0 = t.idd2n + f.act * (spec.idd0 - spec.idd2n);
    }
    if (spec.e_actpre_pJ_override >= 0.0)
        t.e_actpre_pJ_override = f.act * spec.e_actpre_pJ_override;
    return t;
}
/* The stored (maximum) row at a temperature -- for the record and for a
 * before/after comparison; the model does not price it. */
inline IDDSpec iddSpecFor(const std::string& tech, int temperature_k = 358) {
    IDDSpec s = iddTableFor(tech);
    s.trefi_ns *= refreshTempFactor(baseTech(tech), temperature_k);
    return s;
}
/* The row every consumer prices: typical (1.11.91 item 11). */
inline IDDSpec iddFor(const std::string& tech, int temperature_k = 358) {
    IDDSpec s = typicalFromSpec(iddTableFor(tech), componentFactorsFor(tech),
                                idd3nBasisFor(baseTech(tech)));
    s.trefi_ns *= refreshTempFactor(baseTech(tech), temperature_k);
    return s;
}
/* ... and with IDD0 fixed at the activate energy's own timing (see
 * typicalFromSpec): the row arrayReadNJ/arrayWriteNJ price. */
inline IDDSpec iddTypicalAt(const std::string& tech, double tRC, double tRAS,
                            int banks_per_device) {
    return typicalFromSpec(iddTableFor(tech), componentFactorsFor(tech),
                           idd3nBasisFor(baseTech(tech)), banks_per_device,
                           tRC, tRAS);
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
/* 1.11.91 (audit R8-3/R8-4): ONE RULE FOR THE ACCESS DATA PATH.
 *
 * devicesPerAccess() used to answer "the whole rank" for every DDR class --
 * devicesPerRank(width), i.e. 64 bits / device width. That is right for DDR3
 * and DDR4, whose Ramulator channel is 64 bits wide (DDR3.cpp/DDR4.cpp
 * org.channel_width default 64), and WRONG for DDR5, whose simulated channel
 * is a 32-bit SUB-CHANNEL (DDR5.cpp org.channel_width default 32, internal
 * prefetch 16): a 64 B access there engages 4 x8 devices for 16 beats, not
 * 8. With 8 devices x BL16 the energy charged 128 B of activate-and-burst
 * per 64 B access -- both the per-device activate term and the burst term
 * were 2x on every DDR5 cell.
 *
 * The burst term had the mirror defect on LPDDR5 (R8-4): one BL16 on an x16
 * channel moves 32 B, so a 64 B access is TWO bursts, and the burst time the
 * energy used was one.
 *
 * Both answers now come from ONE description of the path an access travels,
 * in IDD-basis terms:
 *
 *   path_bits  the DQ width one 64 B access moves over
 *   unit_bits  the DQ width of ONE unit the IDD row is specified for
 *
 *   devices per access = path_bits / unit_bits
 *   bytes per burst    = path_bits x beats_per_burst / 8
 *   bursts per access  = 64 / bytes per burst (at least 1)
 *
 * beats_per_burst comes from the simulated timing preset (nBL x the family's
 * CK divisor -- see RamulatorWrapper::getBeatsPerBurst()). What each
 * technology gives:
 *
 *   DDR3   64-bit channel / x8 device  = 8 devices; BL8 x 64 b  = 64 B, 1 burst
 *   DDR4   64-bit channel / x8 device  = 8 devices; BL8 x 64 b  = 64 B, 1 burst
 *          (x4 16, x16 4 -- the JEDEC population, unchanged)
 *   DDR5   32-bit sub-channel / x8     = 4 devices; BL16 x 32 b = 64 B, 1 burst
 *          (x4 8, x16 2)                          -- 1.11.90 charged 8
 *   LPDDR5 16-bit channel / x16 die    = 1 device;  BL16 x 16 b = 32 B, 2 bursts
 *                                                 -- 1.11.90 charged 1 burst
 *   GDDR6  16-bit channel / 16-bit channel = 1 unit; BL16 x 16 b = 32 B,
 *          2 bursts. [1.11.91 item 11, R8-5 by the user's instruction: a
 *          64 B access engages ONE of the device's two x16 channels, so the
 *          IDD unit is the CHANNEL and the GDDR6 row is written per channel
 *          (half the Samsung per-DEVICE Table 82 figure; see the row). This
 *          agrees with backgroundUnits(), which counts GDDR6 units as
 *          ranks x channels and receives 2 channels per device from the
 *          GDDR6_8Gb_x16 preset: 2 x a per-channel standby = the device's.
 *          Burst energy is basis-neutral (half current x 2 bursts = device
 *          current x 1 burst, IDD_DERIVATION section 2.2); until 1.11.91 the
 *          path was held at 1 device-unit x 1 burst.]
 *   HBM2   one 128-bit channel (the IDD basis, JESD235D) = 1 unit;
 *          BL4 x 128 b = 64 B, 1 burst
 *   HBM3   one 64-bit channel (the IDD basis, JESD238B.01 cl.9.1) = 1 unit;
 *          BL8 x 64 b = 64 B, 1 burst (the IDD4R loop moves 64 B per 1.25 ns)
 *
 * For DDR3/4/5 path_bits IS the simulated Ramulator channel width, and
 * RamulatorWrapper::checkTranscribedOrganizationShape() REFUSES a run whose
 * instantiated device disagrees (live channel_width vs path_bits, preset DQ
 * vs unit_bits). For the other four the Ramulator channel_width parameter is
 * an address-mapping granularity, not the IDD basis, and is not compared. */
struct AccessPath {
    int path_bits = 64;
    int unit_bits = 8;
    std::string path_name;   // e.g. "32-bit sub-channel"
    std::string unit_name;   // e.g. "8-bit device"
    bool known = true;
};
inline AccessPath accessPathFor(const std::string& tech,
                                const std::string& device_width = "") {
    AccessPath p;
    int w = 8;
    if (device_width == "x4") w = 4;
    else if (device_width == "x16") w = 16;
    if (tech == "DDR3" || tech == "DDR4") {
        p.path_bits = 64; p.unit_bits = w;
        p.path_name = "64-bit channel";
    } else if (tech == "DDR5") {
        p.path_bits = 32; p.unit_bits = w;
        p.path_name = "32-bit sub-channel";
    } else if (tech == "LPDDR5") {
        p.path_bits = 16; p.unit_bits = 16;
        p.path_name = "16-bit channel"; p.unit_name = "16-bit die";
    } else if (tech == "GDDR6") {
        p.path_bits = 16; p.unit_bits = 16;   // 1.11.91 item 11 (R8-5): per channel
        p.path_name = "16-bit channel";
        p.unit_name = "16-bit channel (IDD basis)";
    } else if (tech == "HBM2") {
        p.path_bits = 128; p.unit_bits = 128;
        p.path_name = "128-bit channel (IDD basis)"; p.unit_name = "128-bit channel";
    } else if (tech == "HBM3") {
        p.path_bits = 64; p.unit_bits = 64;
        p.path_name = "64-bit channel (IDD basis)"; p.unit_name = "64-bit channel";
    } else {
        /* 1.11.57 (latent D007): an unrecognised string gets the DDR4 class
         * and says so, as before. */
        p.path_bits = 64; p.unit_bits = w; p.known = false;
        p.path_name = "64-bit channel (DDR4 class, substituted)";
    }
    if (p.unit_name.empty())
        p.unit_name = std::to_string(p.unit_bits) + "-bit device";
    return p;
}

inline int devicesPerAccess(const std::string& tech,
                            const std::string& device_width = "") {
    const AccessPath p = accessPathFor(tech, device_width);
    if (!p.known) {
        announceUnknownTech("devicesPerAccess", tech,
                            "how many devices one 64 B access engages (the "
                            "whole-rank multiplier on array energy)");
    }
    const int n = (p.unit_bits > 0) ? p.path_bits / p.unit_bits : 1;
    return (n > 0) ? n : 1;
}

/* 1.11.91 (R8-4): how many bursts of the simulated preset one 64 B access
 * needs on its data path. beats_per_burst <= 0 (no preset) -> 1, the
 * pre-1.11.91 charge, and the caller prints what it used. */
inline int burstsPer64B(const std::string& tech, const std::string& device_width,
                        int beats_per_burst) {
    if (beats_per_burst <= 0) return 1;
    const AccessPath p = accessPathFor(tech, device_width);
    const int bytes_per_burst = p.path_bits * beats_per_burst / 8;
    if (bytes_per_burst <= 0) return 1;
    const int n = (64 + bytes_per_burst - 1) / bytes_per_burst;
    return (n > 0) ? n : 1;
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

/* 1.11.86 (audit round 6, R6-11 resolved): THE ONE-BANK STANDBY BASELINE.
 *
 * Micron TN-41-01 prices activate+precharge by subtracting, from the IDD0
 * loop, the background that loop would have drawn anyway:
 *
 *   E = Vdd x (IDD0 x tRC - IDD3N x tRAS - IDD2N x (tRC - tRAS))
 *
 * IDD0 is JEDEC's ONE-BANK activate-precharge current: the loop cycles a
 * single bank while every other bank sits precharged. But IDD3N is specified
 * with ALL banks active. So the subtracted baseline describes a different
 * device state from the one IDD0 was measured in, and it over-subtracts by
 * whatever the other banks' active standby costs.
 *
 * On an 8-bank DDR3 that error is small and the term stays positive, which is
 * why the formula has stood. On a 32-bank DDR5 it is larger than the term
 * itself. Verified against the cited tables rather than assumed -- Micron
 * MT60B 16Gb Die Rev A Table 6 (p.17-19) and Die Rev D Table 8 (p.18-20),
 * read at the x8 column, which is this model's device width:
 *
 *              IDD0   IDD2N   IDD3N        IDD0 > IDD3N?
 *   DDR5-4800   103     92     142              no
 *   DDR5-5600    53     49      91              no
 *
 * Both rows transcribe the datasheet CORRECTLY. The rows were never the
 * defect; the baseline was. A 32-bank device really does draw more with all
 * banks open than while cycling one, and nothing is wrong with the part.
 *
 * The baseline the IDD0 loop actually runs at is one bank active and the rest
 * precharged. Deriving it from the two specified points, linear in the number
 * of open banks:
 *
 *   IDD3N(1 bank) = IDD2N + (IDD3N - IDD2N) / banks_per_device
 *
 * which reduces to the published formula when banks_per_device is 1, and
 * recovers TN-41-01's intent on every part. */
/* 1.11.91 (audit R8-2): ... and on a part whose IDD3N is ALREADY a one-bank
 * current (HBM2, HBM3, GDDR6 -- see Idd3nBasis above for the quoted
 * conditions) the same dilution divides a one-bank increment by the bank
 * count a second time. The basis now decides: ALL_BANKS dilutes (the 1.11.86
 * derivation, which is right for it), ONE_BANK takes IDD3N as specified,
 * UNVERIFIED keeps the 1.11.86 formula. */
inline double oneBankActiveStandbyMA(const IDDSpec& s, int banks_per_device,
                                     Idd3nBasis basis) {
    if (basis == Idd3nBasis::ONE_BANK) return s.idd3n;
    const int n = (banks_per_device > 0) ? banks_per_device : 1;
    return s.idd2n + (s.idd3n - s.idd2n) / static_cast<double>(n);
}

inline double arrayReadNJ(const std::string& tech, double tRC, double tRAS,
                          double tBurst, double bank_override_pJ_per_byte,
                          const std::string& device_width = "",
                          double row_miss_frac = -1.0,
                          int banks_per_device = 1) {
    if (bank_override_pJ_per_byte > 0.0)
        return bank_override_pJ_per_byte * 64.0 / 1000.0
               * devicesPerAccess(baseTech(tech), device_width);   // 1.11.57 (D004)
    IDDSpec s = iddTypicalAt(tech, tRC, tRAS, banks_per_device);   // 1.11.91 (item 11): typical, activate = f.act x spec
    const double idd3n_1b = oneBankActiveStandbyMA(s, banks_per_device,
                                idd3nBasisFor(baseTech(tech)));  // 1.11.86; basis 1.11.91 (R8-2)
    double e_actpre_pJ = (s.e_actpre_pJ_override >= 0.0)      // 1.11.91 (item 11)
        ? s.e_actpre_pJ_override
        : s.vdd * (s.idd0 * tRC - idd3n_1b * tRAS - s.idd2n * (tRC - tRAS));
    refuseNegativeActivateEnergy(tech, s, tRC, tRAS, e_actpre_pJ);   // 1.11.79 (R6-11)
    /* 1.11.91 (R8-2): the burst's baseline is the standby the IDD4R loop
     * ran on top of -- all banks open on the ALL_BANKS parts, i.e. IDD3N
     * as specified. For ONE_BANK parts see the stated residual in the
     * Idd3nBasis note. */
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
                           double row_miss_frac = -1.0,
                           int banks_per_device = 1) {
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
    IDDSpec s = iddTypicalAt(tech, tRC, tRAS, banks_per_device);   // 1.11.91 (item 11): typical, activate = f.act x spec
    const double idd3n_1b = oneBankActiveStandbyMA(s, banks_per_device,
                                idd3nBasisFor(baseTech(tech)));  // 1.11.86; basis 1.11.91 (R8-2)
    double e_actpre_pJ = (s.e_actpre_pJ_override >= 0.0)      // 1.11.91 (item 11)
        ? s.e_actpre_pJ_override
        : s.vdd * (s.idd0 * tRC - idd3n_1b * tRAS - s.idd2n * (tRC - tRAS));
    refuseNegativeActivateEnergy(tech, s, tRC, tRAS, e_actpre_pJ);   // 1.11.79 (R6-11)
    double e_wr_pJ     = s.vdd * (s.idd4w - s.idd3n) * tBurst;   // same baseline rule as the read (1.11.91)
    const double ROW_MISS_FRAC = (row_miss_frac >= 0.0 && row_miss_frac <= 1.0)
                                 ? row_miss_frac : 0.5;   // 1.11.52 (D003)
    return (ROW_MISS_FRAC * e_actpre_pJ + e_wr_pJ) / 1000.0
           * devicesPerAccess(baseTech(tech), device_width);   // 1.11.46 (L181)
}

/* 1.11.5 (audit): interfaceNJ REMOVED. It returned vdd*(idd4r-idd3n)*tBurst
 * -- bit-identical to the burst term already inside arrayReadNJ, so every
 * consumer that added it double-charged the DQ read current. [The removal
 * was right; the next two sentences of the 1.11.5 note were NOT, and are
 * corrected by 1.11.91 (audit R8-8):] "JEDEC IDD4R is measured with the
 * outputs driving: the on-die I/O switching is already in the array term" --
 * FALSE. The outputs drive during the IDD4R loop, but IDD is measured on the
 * VDD balls only and the output drivers sit on VDDQ: "Any IPP or IDDQ
 * current is not included in IDD currents" (Micron DDR5 core sheet p.448),
 * "IPP and IDDQ currents are not included in IDD currents" (MT40A p.314),
 * IDDQ on the VDDQ microbumps separately (JESD238B.01 cl.9.1, PDF p.164).
 * The output-driver current is IDDQ4R, a separate row, now priced by
 * iddqNJ() below on the accesses that cross the DQ. "The genuinely
 * ADDITIONAL off-chip energy is termination" -- incomplete for the same
 * reason: termination is the DC loop, IDDQ is the driver rail; both are
 * additional to the array term (with the stated write overlap, see the
 * DDR5 row). */

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
     * not in the charged model.
     * [1.11.91 (audit R8-8) CORRECTION: "already inside the measured burst
     * current" is FALSE for the DQ driver rail. JESD238B.01 cl.9.1 (PDF
     * p.164, printed p.150) measures IDD on the VDDC microbumps, IPP on VPP
     * and IDDQ on VDDQ separately, and for IDDQ says "DRAM vendors shall
     * provide simulated values using the IDD4R measurement-loop pattern" --
     * the DQ output drivers are NOT in IDD4R. The zero TERMINATION above is
     * unaffected (unterminated link, same citations). The DQ driver energy
     * is now priced by iddqNJ(); HBM publishes no IDDQ number, so it takes
     * the Cho 0.19-0.27 pJ/bit driver figure as a BAND (see iddqBandFor).
     * Whether Cho's per-TSV drivers sit on VDDQ or VDDC is not stated in
     * the paper; the band is the nearest held measurement of an HBM data
     * driver, used for the VDDQ rail by the user's R8-8 ruling.] */
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

/* 1.11.91 (audit R8-8, user ruling (a)): THE DQ OUTPUT RAIL, PER ACCESS.
 *
 * IDDQ is the VDDQ current -- the output drivers -- and no IDD column
 * contains it (the citations are on IDDSpec). Charged ONLY on an access that
 * drives the DQ pins, i.e. under the same crossesOffPackageDQ() rule
 * (src/main.cpp) that decides termination: host-originated accesses and
 * off-die placements pay it, on-die placements do not. The CALLER applies
 * that rule; this function prices one access that crosses:
 *
 *   E = (IDDQ4R | IDDQ4W - IDDQ3N) x VDDQ x tBurst(64 B) x devices
 *
 * -- the burst increment over the VDDQ standby the loop ran on top of, over
 * the whole 64 B access on its data path (getAccessBurstNs), times the
 * devices one access engages (devicesPerAccess), exactly the shape of the
 * VDD burst term. Returns < 0 when the row publishes no IDDQ (the caller
 * prints n/a and adds nothing). A negative increment is not clamped: it is
 * returned as < 0 too and the caller says so.
 *
 * HBM2/HBM3 publish no IDDQ value (JESD238B.01 PDF p.164: vendor-simulated;
 * JESD235D PDF p.108 likewise), so they take a BAND in energy per bit: see
 * iddqBandFor(). */
struct IddqBand { bool valid = false; double lo_pj_bit = 0.0, hi_pj_bit = 0.0; };
/* HBM2/HBM3 IDDQ BAND. Cho et al., SK hynix, ISSCC 2018 12.3 Fig.12.3.1
 * (misc/): per-TSV driver current ~880 uA (multi-drop) and ~610 uA (spiral
 * point-to-point) at 1.0 V and 3.3 Gb/s PRBS, i.e. 610e-6 x 1.0 / 3.3e9 =
 * 0.185 and 880e-6 x 1.0 / 3.3e9 = 0.267 pJ/bit: the band 0.19-0.27 pJ/bit
 * (chart-read; the pJ/bit derivation is the tree's, docs/sources.md). BAND,
 * not a point: the charged value is the midpoint, 0.23 pJ/bit, and the
 * report prints both ends. Reads only: the figure is a DRIVER energy, and
 * on a write the DRAM's DQ receives rather than drives (the host's PHY
 * drives), so a write is charged no IDDQ from this band. */
inline IddqBand iddqBandFor(const std::string& base_tech) {
    IddqBand b;
    if (base_tech == "HBM2" || base_tech == "HBM3") {
        b.valid = true; b.lo_pj_bit = 0.19; b.hi_pj_bit = 0.27;
    }
    return b;
}
inline double iddqNJ(const std::string& tech, const std::string& device_width,
                     double access_burst_ns, bool is_write,
                     double* band_lo_nj = nullptr, double* band_hi_nj = nullptr) {
    const std::string bt = baseTech(tech);
    const IddqBand band = iddqBandFor(bt);
    if (band.valid) {
        if (band_lo_nj) *band_lo_nj = is_write ? 0.0 : band.lo_pj_bit * 512.0 / 1000.0;
        if (band_hi_nj) *band_hi_nj = is_write ? 0.0 : band.hi_pj_bit * 512.0 / 1000.0;
        if (is_write) return 0.0;
        return 0.5 * (band.lo_pj_bit + band.hi_pj_bit) * 512.0 / 1000.0;
    }
    const IDDSpec s = iddFor(tech);
    const double iq = is_write ? s.iddq4w : s.iddq4r;
    if (iq < 0.0 || s.iddq3n < 0.0 || s.vddq <= 0.0 || !(access_burst_ns > 0.0))
        return -1.0;   // not published for this row: nothing priced
    const double e_pj = (iq - s.iddq3n) * s.vddq * access_burst_ns
                        * static_cast<double>(devicesPerAccess(bt, device_width));
    return e_pj / 1000.0;   // may be < 0 if the row's increment is negative
}

/* 1.11.91 (audit R8-8, user ruling (a)): THE PUMP RAIL, PER UNIT. IPP2N
 * while the unit is precharged, IPP3N while a bank is open, each x VPP, on
 * the same state split as the VDD background (backgroundUnitStatesMW): the
 * active share is the caller's bank-open fraction, the rest is precharged.
 * Precharge power-down draws IPP2N too (DDR5 Rev A IPP2P 6 = IPP2N 6,
 * Table 6 p.18; DDR4 note 22 p.334: IPP3N applies to "all IDD2x"). The
 * refresh and burst IPP rows (IPP5B, IPP4R/W, IPP0) are NOT priced -- only
 * the standby pair the ruling names. Returns < 0 when the row has no IPP
 * (HBM2/HBM3: no published value; GDDR6: pending the R8-9 row; DDR3: no VPP
 * rail; DDR5-3200: no sheet). */
inline double ippUnitMW(const std::string& tech, double active_frac) {
    const IDDSpec s = iddFor(tech);
    if (s.ipp2n < 0.0 || s.ipp3n < 0.0 || s.vpp <= 0.0) return -1.0;
    if (active_frac < 0.0) active_frac = 0.0;
    if (active_frac > 1.0) active_frac = 1.0;
    return s.vpp * (s.ipp3n * active_frac + s.ipp2n * (1.0 - active_frac));
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
/* 1.11.91 (item 12): how many HBM stacks the system's channel count is --
 * the same arithmetic backgroundUnits() uses for its HBM population. */
inline int hbmStacks(const std::string& tech, int channels) {
    const int per_stack = iddFor(tech).channels > 0 ? iddFor(tech).channels : 1;
    if (channels < 1) channels = 1;
    return (channels > per_stack) ? (channels / per_stack) : 1;
}
/* 1.11.91 (item 12, user ruling option (c)): the per-STACK static floor for
 * the memory system (HBM2/HBM3), mW; < 0 when the row has none. */
inline double stackFloorSystemMW(const std::string& tech, int channels = 1) {
    const IDDSpec s = iddFor(tech);
    if (s.stack_floor_mw < 0.0 || baseTech(tech).substr(0, 3) != "HBM") return -1.0;
    return s.stack_floor_mw * static_cast<double>(hbmStacks(tech, channels));
}
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
/* 1.11.91 (audit R8-7, user ruling (b)): TN-41-01 WITH A MEASURED BANK-OPEN
 * FRACTION. Micron TN-41-01 p.5 defines the two standby states as
 * "precharged (all of the banks are precharged) or active (one or more
 * banks are open)", with the input BNK_PRE% = the percentage of time all
 * banks are precharged. Until this release the active share was 1 - r_idle,
 * where r_idle is "phases with no memory-controller access" (10k-cycle
 * phases) or the E17 gap histogram: that is TRAFFIC in the phase, which
 * equals the bank-open time only under an open-page policy that never
 * closes. With Ramulator's ClosedRowPolicy (DDR3/4/5 in the emitted config,
 * cap 4) a busy phase can have its banks precharged most of the time.
 *
 * The states are now split by two inputs:
 *   active_frac  BNK_ACT% = 1 - BNK_PRE%: the MEASURED fraction of memory
 *                cycles with >= 1 bank open (Ramulator's bank state, the
 *                zsim `bankOpenCycles / bankOpenWindow` counters) -- IDD3N
 *                state (on ONE_BANK parts IDD3N is the one-bank current,
 *                which is the state "one or more banks open" starts at).
 *   r_pd         the CKE-low (power-down) residency, the existing phase /
 *                gap measurement, used ONLY for the power-down descent
 *                under pg_enabled. It cannot exceed the precharged share
 *                (a unit with a bank open is not in precharge power-down);
 *                it is capped there and *r_pd_used returns what was used,
 *                so the caller can say so.
 * The precharged share 1 - active_frac is IDD2N, minus the power-down slice
 * at IDD2P (with the tXP derate), exactly the old descent.
 *
 * backgroundUnitMW(tech, r_idle, ...) below is the same body with the
 * precharged share = r_idle and r_pd = r_idle, which reproduces the 1.11.90
 * number bit for bit; it is what a run without the measurement uses. */
/* The body, on the PRECHARGED share (so the legacy call below passes r_idle
 * itself and not 1 - (1 - r_idle), which is not bit-identical in floating
 * point). */
inline double backgroundUnitCoreMW(const std::string& tech, double pre_share,
                                   double r_pd, bool pg_enabled,
                                   int temperature_k, double* r_pd_used) {
    if (pre_share < 0.0) pre_share = 0.0;
    if (pre_share > 1.0) pre_share = 1.0;
    if (r_pd < 0.0) r_pd = 0.0;
    if (r_pd > pre_share) r_pd = pre_share;   // reported back through r_pd_used
    if (r_pd_used) *r_pd_used = r_pd;
    const double r_idle = pre_share;   // the precharged share (old name kept below)
    IDDSpec s = iddFor(tech, temperature_k);
    const double kHysteresisDerate = 0.99;
    /* Each state pays its OWN refresh (E15) -- see stateWithRefreshMW. */
    const double active_mw = stateWithRefreshMW(s, s.idd3n);  // IDD3N, row open
    double pre_mw          = stateWithRefreshMW(s, s.idd2n);  // IDD2N, precharged
    double pd_mw           = stateWithRefreshMW(s, s.idd2p);  // IDD2P, CKE low
    /* 1.11.91 (audit R8-11): THE IDLE DESCENT CANNOT COST MORE THAN THE
     * ACTIVE STATE IT DESCENDS FROM. The pd <= pre guard below existed; the
     * pre <= active guard beside it did not. The HBM2 row is measured silicon
     * (Zen HBM2 per-channel means, see iddTableFor) where IDD2N 136 mA sits
     * ABOVE IDD3N 133 mA -- the open-row standby increment is inside the
     * measurement noise -- so every idle cycle was priced higher than a busy
     * one and the idle descent RAISED background power (up to +2.2%). The
     * guard is applied the same way as the power-down one, and it says so
     * once per technology when it binds, naming the two currents. */
    if (pre_mw > active_mw) {
        static std::set<std::string> said_pre;
        if (r_idle > 0.0 && said_pre.insert(tech).second) {
            std::cerr << "[mem] NOTE: " << tech << " precharge standby IDD2N ("
                      << s.idd2n << " mA) exceeds active standby IDD3N ("
                      << s.idd3n << " mA) in its IDD row; the idle state is "
                         "priced at the active state's power (an idle "
                         "descent cannot raise power)." << std::endl;
        }
        pre_mw = active_mw;
    }
    if (pd_mw > pre_mw) pd_mw = pre_mw;         // guard odd rows: never a penalty
    double idle_mw;
    if (pg_enabled) {
        const double r_pd_eff = r_pd * kHysteresisDerate;
        idle_mw = pd_mw * r_pd_eff + pre_mw * (r_idle - r_pd_eff);
    } else {
        idle_mw = pre_mw * r_idle;
    }
    return active_mw * (1.0 - r_idle) + idle_mw;
}
inline double backgroundUnitStatesMW(const std::string& tech, double active_frac,
                                     double r_pd, bool pg_enabled,
                                     int temperature_k = 358,
                                     double* r_pd_used = nullptr) {
    if (active_frac < 0.0) active_frac = 0.0;
    if (active_frac > 1.0) active_frac = 1.0;
    return backgroundUnitCoreMW(tech, 1.0 - active_frac, r_pd, pg_enabled,
                                temperature_k, r_pd_used);
}
inline double backgroundUnitMW(const std::string& tech, double r_idle,
                               bool pg_enabled, int temperature_k = 358) {
    if (r_idle < 0.0) r_idle = 0.0;
    if (r_idle > 1.0) r_idle = 1.0;
    return backgroundUnitCoreMW(tech, r_idle, r_idle, pg_enabled,
                                temperature_k, nullptr);
}

/* The memory system's background: population x per-unit state-aware power.
 * This is the only function the report should call. */
inline double backgroundSystemMW(const std::string& tech, double r_idle,
                                 bool pg_enabled,
                                 const std::string& device_width = "",
                                 int ranks_per_channel = 1,
                                 int channels = 1,        // 1.11.52 (A015)
                                 int temperature_k = 358) {   // 1.11.65
    const double floor_mw = stackFloorSystemMW(tech, channels);   // 1.11.91 (item 12)
    return backgroundUnitMW(tech, r_idle, pg_enabled, temperature_k) *
           static_cast<double>(backgroundUnits(baseTech(tech), device_width,
                                               ranks_per_channel, channels))
           + (floor_mw > 0.0 ? floor_mw : 0.0);
}
/* 1.11.91 (R8-7): the same population, state-split by the measured
 * bank-open fraction (see backgroundUnitStatesMW). */
inline double backgroundSystemStatesMW(const std::string& tech, double active_frac,
                                       double r_pd, bool pg_enabled,
                                       const std::string& device_width = "",
                                       int ranks_per_channel = 1,
                                       int channels = 1,
                                       int temperature_k = 358,
                                       double* r_pd_used = nullptr) {
    const double floor_mw = stackFloorSystemMW(tech, channels);   // 1.11.91 (item 12)
    return backgroundUnitStatesMW(tech, active_frac, r_pd, pg_enabled,
                                  temperature_k, r_pd_used) *
           static_cast<double>(backgroundUnits(baseTech(tech), device_width,
                                               ranks_per_channel, channels))
           + (floor_mw > 0.0 ? floor_mw : 0.0);
}

} // namespace pimid_energy
} // namespace Ramulator
#endif
