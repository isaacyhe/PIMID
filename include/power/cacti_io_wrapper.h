/* PIMID 1.11.40 -- harness for CACTI-IO (external/cacti/extio.cc).
 *
 * WHY THIS EXISTS. The link surface was a table of DERIVED numbers: 7.0 pJ/bit,
 * 63.0 GB/s, 500 ns per transaction, and "UCIe PHY area is not sourceable".
 * Each was a single value standing where a model belongs. CACTI-IO is an
 * off-chip IO model that computes termination power, PHY static and dynamic
 * power, IO area and timing/voltage margin from EXTRACTED PARAMETERS -- swing
 * voltages, capacitances, bias and leakage currents, termination resistances,
 * and area coefficients as a polynomial in impedance and frequency. It has been
 * vendored in this tree, compiled into the CACTI library, and called by nothing.
 *
 * WHAT IT COVERS -- MEASURED, not assumed (gate 1155, audit N8). CACTI-IO's
 * Mem_IO_type spans DDR3, DDR4, LPDDR2, WideIO, Low_Swing_Diff and Serial. We
 * ran all of them:
 *
 *   DRAM-CLASS PATHS: USABLE. DDR4-3200 x64 gives IO area 2.99 mm^2,
 *   termination 262 mW, dynamic 443 mW, PHY 390 mW -- about 1.1 W, physical.
 *   This is the model's home and the reason to harness it: it replaces the
 *   hand-written SSTL/POD/LVSTL scheme table in pimid_energy.h and adds PHY
 *   power and IO area, which PIMID does not model at all.
 *
 *   SERIAL PATH: NOT USABLE, and it had never been executed by anyone. Four
 *   independent faults, all upstream: (1) nothing called it; (2) the Serial
 *   branch declared r_diff_term and left rtt1/rtt2_dq_*, rs1/rs2_dq, rtt_ca,
 *   r_stub_ca, z0, r_on and t_flight at ZERO, so rpar_write divided by zero --
 *   SIGFPE on construction; (3) num_mem_clk divides by (num_clk/2) and a
 *   serial link legitimately has num_clk = 0 (embedded clock, CDR), so integer
 *   0/2 faults; (4) the area polynomial is a FIT over DDR frequencies -- its
 *   cubic term is 0.003 at 800 MHz and 163.8 at 32 GHz, yielding 61 mm^2 of IO
 *   area for 16 lanes, and even at 8 GT/s the termination power returns
 *   3.97e+280 mW (uninitialised memory).
 *   1.11.40 repaired (2) and (3) in our fork so the path constructs, and
 *   REFUSED above 8000 MHz rather than extrapolating the cubic. It did NOT
 *   repair (4): refitting coefficients for multi-GHz serial rates cannot be
 *   done from published bands alone.
 *
 * 1.11.57 (latent C024, and C009 with it): the serial entry point
 * computeLink() is DELETED -- declaration and body both. What it was: a
 * public, compiled, documented function with NO caller anywhere in src/,
 * include/ or tools/ (verified tree-wide; linkBandwidthGBs() and
 * linkRateGTs() are equally uncalled but are pure specification lookups and
 * stay). It was invisible for exactly that reason: a wrong number that
 * nothing asks for cannot appear in a report. It was not, however, harmless
 * to keep. Its 8000 MHz fit guard admitted exactly ONE of the seven link
 * types this file knows (pcie_gen3 at 8.0 GT/s lands on the ceiling
 * precisely; gen4/gen5/cxl/interposer/nvlink/ualink are all refused), so
 * anyone wiring it up would have found six refusals and one answer -- and
 * that one answer is fault (4) above, a termination power of 3.97e+280 mW
 * returned with valid = true and an energy_pj_per_bit derived from it. A
 * caller taking the valid flag at face value would have priced a link from
 * uninitialised memory.
 *
 * The same deletion retires the C009 claim, which lived inside it. Those two
 * lines set ron_value = 50 and rtt_value = 50 ohm and called them
 * "specification facts about the electrical layer -- the one kind of single
 * value that is legitimately a constant", naming no specification. Nothing in
 * this tree supports that: no PCIe electrical document is held locally (which
 * holds the JEDEC set the DRAM table cites -- JESD79-3D, JESD8-24, JESD8-21C,
 * JESD250D -- and no PCI-SIG material), and the only other 50 in the path is
 * upstream's own untraceable default, extio_technology.cc:874
 * `r_on = (g_ip->ron_value > 0.0) ? g_ip->ron_value : 50;`. 50/50 ohm
 * single-ended is a widespread convention for a 100 ohm differential channel;
 * it is not a fact this tree can cite, and we do not invent a citation for it.
 * Note also that the Serial branch hardcodes rtt1_dq_read = 50 and never
 * reads g_ip->rtt_value, so half the assertion was inert even when executed.
 *
 * If a serial link model is wanted later, it starts from DSENT or from a
 * refit of the area polynomial -- not from reviving this function.
 *
 * So PCIe/CXL link energy stays on the 1.11.40 published bands, labelled as
 * literature values rather than model output. DSENT is the candidate model for
 * that class and is not vendored.
 *
 * VALIDATION TARGET. The published bands (PCIe gen5 7.6-11.4 pJ/bit; UCIe
 * advanced 0.25-0.5) are the test set for whatever model eventually covers
 * serial links -- not the model itself.
 *
 * GLOBAL-STATE DISCIPLINE (audit E30, and 1.11.45's correction of it).
 * CACTI-IO reads g_ip -- num_dq, num_dqs, num_ca, num_clk, duty_cycle,
 * io_type, iostate -- the same global CACTIWrapper and the McPAT fork also
 * use, and E30 records that CACTIWrapper leaves it dangling.
 *
 * 1.11.56 (audit C025): this paragraph used to describe the SAVE/MUTATE/
 * RESTORE pattern ("SAVES the whole InputParameter ... and RESTORES it") --
 * that is exactly the pattern 1.11.45 removed, and the .cpp calls it a
 * use-after-free: saving *g_ip and later writing *g_ip = saved is a write
 * THROUGH whatever the global points at, which after any CACTIWrapper's
 * death is freed memory (a dangling pointer is not null, so a null check
 * cannot save you). Leaving the retired pattern documented in the header as
 * the discipline is how it gets reintroduced.
 *
 * What the implementation ACTUALLY does is a POINTER SWAP: each entry point
 * points g_ip at its own function-static InputParameter for the duration of
 * the call, resets that object to defaults, fills the IO fields, and on every
 * exit path restores the PREVIOUS POINTER -- dereferencing nothing it does
 * not own.
 */
#ifndef PIMID_POWER_CACTI_IO_WRAPPER_H
#define PIMID_POWER_CACTI_IO_WRAPPER_H

#include <string>

namespace PIMID {

/* One evaluation of a link's IO. Powers in mW, area in mm^2. */
struct LinkIOResult {
    bool   valid = false;

    double io_area_mm2 = 0.0;          // driver + ODT active circuit, per link
    double io_power_term_mw = 0.0;     // termination (duty-cycle weighted)
    double io_power_dynamic_mw = 0.0;  // switching on dq/dqs/ca/clk
    double phy_power_mw = 0.0;         // static + dynamic PHY
    double phy_static_power_mw = 0.0;
    double phy_dynamic_power_mw = 0.0;

    double timing_margin_ui = 0.0;     // eye margins, reported not consumed
    double voltage_margin_v = 0.0;

    /* Derived for comparison against the published bands. This is the whole
     * point of the harness: a number the model PRODUCED, next to the numbers
     * the literature reports. */
    double energy_pj_per_bit = 0.0;
    /* Termination alone, for a like-for-like swap against the hand-written
     * scheme table in pimid_energy.h, which models ONLY termination. Keeping
     * the components separable is what lets the replacement be checked term by
     * term instead of as one aggregate that could hide a compensating error. */
    double energy_pj_per_bit_term = 0.0;
    /* 1.11.63 (R7, read/write split): the field above is computed with
     * CACTI-IO's iostate = WRITE (the wrapper's long-standing setting) and is
     * therefore the WRITE-direction termination. This one is a second pass of
     * extio_power_term() with iostate = READ over the same parameter set --
     * CACTI-IO natively separates power_termination_read/write (extio.cc),
     * and the split consumer charges each direction its own loop. */
    double energy_pj_per_bit_term_rd = 0.0;
    /* The DATA pins' share of the two termination figures above, at the
     * formula's 0.5 low duty (user ruling 2026-10-25: measured termination
     * duty): the figure minus a second pass with the data pins never LOW.
     * Zero where the parameter set's termination does not depend on the
     * data (SSTL, the serial and wide-I/O sets). The caller scales it by
     * measured duty / 0.5. */
    double energy_pj_per_bit_term_dq = 0.0;
    double energy_pj_per_bit_term_dq_rd = 0.0;
    /* The strobes' and the command/address pins' shares (each: the figure minus a pass without those pins). */
    double energy_pj_per_bit_term_dqs = 0.0, energy_pj_per_bit_term_dqs_rd = 0.0;
    double energy_pj_per_bit_term_ca = 0.0, energy_pj_per_bit_term_ca_rd = 0.0;
    /* The DQ loop the termination figures used: rail, driver, far-end termination (read / write) and series stub, ohm. */
    double loop_vddq = 0.0, loop_ron = 0.0, loop_rtt_rd = 0.0, loop_rtt_wr = 0.0, loop_stub = -1.0;

    /* Is the technology->Mem_IO_type map EXACT, or a nearest neighbour? Only
     * exact maps are allowed to replace an existing result. An approximate map
     * (LPDDR5 -> LPDDR2, HBM -> WideIO) is a cross-check to report, never a
     * substitution to make silently: its parameter set was fitted for a
     * different interface and its absolute values are not trustworthy. */
    bool exact_map = false;

    /* 1.11.60 (audit round 4, C009): DID THE MODEL REFUSE TO EXTRAPOLATE THE
     * AREA? io_area_mm2 is deliberately set to 0.0 above the 3162 MHz
     * validity crossover (see computeDramIO), and until now the only record of
     * that decision was a sentence at the front of `not_modelled`. A consumer
     * holding the struct saw the same 0.0 for "the polynomial is out of range
     * here" and for "this technology has no parameter set at all", and
     * RamulatorWrapper::getInterfaceAreaMM2() flattened both into one return
     * value -- so GDDR6, whose bus runs at 7000 MHz, lost its DQ-interface
     * area silently while DDR4 printed a 2.444 mm^2 line for the same term.
     * A withheld quantity is a different thing from an absent one and now
     * says so in a field a caller can test. */
    bool io_area_withheld = false;

    std::string source;                // which parameter set was used
    std::string not_modelled;          // what this result does NOT include
};

class CactiIOWrapper {
public:
    /* 1.11.57 (latent C024/C009): computeLink() was declared HERE. It is gone
     * -- see the SERIAL PATH note at the top of this file for what it did,
     * why nothing called it, and why leaving a dead entry point that returns
     * valid=true on a 3.97e+280 mW termination power was a trap rather than
     * an unused convenience. */

    /* Evaluate a DRAM INTERFACE -- CACTI-IO's validated home range, and the
     * reason this harness is worth having. Replaces the hand-written
     * SSTL/POD/LVSTL scheme table in pimid_energy.h (typed-in vddq/rtt/rpd)
     * with extracted parameters, and additionally yields PHY power and IO
     * AREA, neither of which PIMID models today.
     *   tech        PIMID memory technology name
     *   num_dq      data pins on the channel (64 for a DDR-class channel)
     *   rate_mts    transfer rate MT/s -- a specification fact
     *   activity    MEASURED read/write activity fraction, not a constant
     * Returns valid=false with `source` naming the reason when the technology
     * has no CACTI-IO counterpart. The mapping is approximate for technologies
     * postdating the model and every approximation is named in `source`. */
    /* (user ruling 2026-10-26) stub_ohm: the DQ/DQS series resistor between the
     * connector and the DRAM (CACTI-IO rs1_dq / rs2_dq) for the memory
     * node's topology; < 0 = the technology's default topology
     * (resolveDramTopology). */
    static LinkIOResult computeDramIO(const std::string& tech,
                                      int num_dq,
                                      double rate_mts,
                                      double activity,
                                      double stub_ohm = -1.0);
    /* The memory topology of a DRAM node (user ruling 2026-10-26): requested
     * "" = the technology's default. Fills `out` (name, stub, the stub's
     * source, whether it was the default) and returns true, or returns false
     * with `err` saying why the request is refused. */
    struct DramTopology {
        std::string name;
        double stub_ohm = 0.0;
        std::string source;
        bool defaulted = false;
    };
    static bool resolveDramTopology(const std::string& tech, const std::string& requested,
                                    DramTopology& out, std::string& err);

    /* DRAM specification primitives -- rate and channel width. Both are fixed
     * by the standard, so they are the legitimate kind of table entry. <0 for
     * unknown technologies. */
    static double dramRateMTs(const std::string& tech);
    static int    dramChannelWidthBits(const std::string& tech);

    /* Link CONTROLLER clock from specification primitives (1.11.42, E21):
     *     clock_MHz = rate_GT/s * 1000 / pipe_width_bits
     * The PIPE datapath width is bounded by the PIPE spec (8/16/32-bit per
     * lane; Intel PIPE Architecture Spec rev 7.1) and 32-bit is the common
     * configuration for gen3+ -- an implementation CONVENTION, stated, not a
     * per-generation constant pretending to be one. Returns <0 for link
     * families that do not use PIPE (NVLink, UALink, UCIe) -- their controller
     * clocks have no sourced basis here and the caller must say so rather
     * than defaulting. */
    static double linkControllerClockMHz(const std::string& link_type);

    /* Bandwidth from specification primitives, replacing pcie_bandwidth_GBs.
     * lanes x rate x encoding efficiency / 8. Returns <0 for unknown types. */
    static double linkBandwidthGBs(const std::string& link_type, int num_lanes);

    /* Per-lane rate and encoding for a link type -- specification facts, the
     * only kind of single value that belongs in a table. <0 if unknown. */
    static double linkRateGTs(const std::string& link_type);
    static double linkEncodingEfficiency(const std::string& link_type);
    /* (user ruling 2026-10-27: serial links calibrated) the data link layer's share (PCIe: 57.88/62.96, Neugebauer et al. SIGCOMM 2018). */
    static double linkDllEfficiency(const std::string& link_type);
    /* the class's published lane (pair) count when it has one fixed by the product, else -1 */
    static int linkDefaultLanes(const std::string& link_type);
};

}  // namespace PIMID
#endif
