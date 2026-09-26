#include "power/mcpat_wrapper.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <stdexcept>
#include <vector>
#include <climits>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cerrno>
#include <fcntl.h>

#include "globalvar.h"
#include "XML_Parse.h"
#include "processor.h"

namespace pimid {

//=============================================================================
// McPATWrapper Implementation
//=============================================================================

McPATWrapper::McPATWrapper(const SystemConfig& config)
    : config_(config)
    , mcpat_parser_(nullptr)
    , mcpat_processor_(nullptr)
    , total_cycles_(0)
    , busy_cycles_(0)
    , total_instructions_(0)
    , l1i_reads_(0)
    , l1i_read_misses_(0)
    , l1d_reads_(0)
    , l1d_writes_(0)
    , l1d_read_misses_(0)
    , l1d_write_misses_(0)
    , l2_reads_(0)
    , l2_writes_(0)
    , l2_read_misses_(0)
    , l2_write_misses_(0)
    , l3_reads_(0)
    , l3_writes_(0)
    , l3_read_misses_(0)
    , l3_write_misses_(0)
    , mc_reads_(0)
    , mc_writes_(0)
    , device_profile_(DeviceProfile::DEVICE_INORDER)
    , initialized_(false)
    , valid_(false)
    , power_computed_(false)
    , user_provided_xml_(!config.xml_file.empty())
    , error_message_("")
{
}

McPATWrapper::~McPATWrapper() {
    delete mcpat_processor_;
    delete mcpat_parser_;
    mcpat_processor_ = nullptr;
    mcpat_parser_ = nullptr;
}

void McPATWrapper::initialize() {
    if (initialized_) {
        std::cerr << "[McPATWrapper] Warning: Already initialized" << std::endl;
        return;
    }

    validateConfiguration();
    if (!valid_) {
        throw std::runtime_error("[McPATWrapper] Invalid configuration: " + error_message_);
    }

    try {
        createMcPATInput();
        initialized_ = true;

        std::cout << "[McPATWrapper] Initialized with:" << std::endl;
        std::cout << "  Cores: " << config_.num_cores << std::endl;
        std::cout << "  Core Clock: " << config_.core_clock_mhz << " MHz" << std::endl;
        std::cout << "  L1I/L1D: " << (config_.l1i_size_bytes/1024) << "/"
                  << (config_.l1d_size_bytes/1024) << " KB" << std::endl;
        std::cout << "  L2: " << (config_.l2_size_bytes/1024) << " KB" << std::endl;
        std::cout << "  L3: " << (config_.l3_size_bytes/(1024*1024)) << " MB" << std::endl;
        std::cout << "  Technology: " << config_.tech_node_nm << " nm" << std::endl;
        if (device_profile_ == DeviceProfile::OOO)
            std::cout << "  Profile: HOST_OoO (x86 out-of-order)" << std::endl;
        else if (device_profile_ == DeviceProfile::DEVICE_ALU)
            std::cout << "  Profile: DEVICE_ALU (no caches)" << std::endl;
        else   /* 1.11.89: the width McPAT is handed, printed with the profile */
            std::cout << "  Profile: DEVICE_INORDER (issue_width="
                      << config_.issue_width << ")" << std::endl;

    } catch (const std::exception& e) {
        valid_ = false;
        error_message_ = std::string("Initialization failed: ") + e.what();
        throw;
    }
}

void McPATWrapper::reconfigure(const SystemConfig& config) {
    config_ = config;
    initialized_ = false;
    power_computed_ = false;
    initialize();
}

void McPATWrapper::validateConfiguration() {
    valid_ = true;
    error_message_ = "";

    if (config_.num_cores < 1 || config_.num_cores > 1024) {
        valid_ = false;
        error_message_ = "Number of cores out of range (1-1024)";
        return;
    }

    if (config_.core_clock_mhz < 100 || config_.core_clock_mhz > 10000) {
        valid_ = false;
        error_message_ = "Core clock out of range (100-10000 MHz)";
        return;
    }

    if (config_.tech_node_nm < 7 || config_.tech_node_nm > 90) {
        valid_ = false;
        error_message_ = "Technology node out of range (7nm - 90nm)";
        return;
    }
}

void McPATWrapper::setTotalCycles(uint64_t cycles) {
    total_cycles_ = cycles;
    power_computed_ = false;
}

void McPATWrapper::setBusyCycles(uint64_t cycles) {
    busy_cycles_ = cycles;
    power_computed_ = false;
}

void McPATWrapper::setTotalInstructions(uint64_t instructions) {
    total_instructions_ = instructions;
}

void McPATWrapper::setMeasuredCoreActivity(uint64_t uops, uint64_t branches,
                                           uint64_t mispredicted) {
    meas_uops_ = uops;
    meas_branches_ = branches;
    meas_mispred_ = mispredicted;
    power_computed_ = false;
}

void McPATWrapper::setL1IAccesses(uint64_t reads, uint64_t read_misses) {
    l1i_reads_ = reads;
    l1i_read_misses_ = read_misses;
    power_computed_ = false;
}

void McPATWrapper::setL1DAccesses(uint64_t reads, uint64_t writes,
                                   uint64_t read_misses, uint64_t write_misses) {
    l1d_reads_ = reads;
    l1d_writes_ = writes;
    l1d_read_misses_ = read_misses;
    l1d_write_misses_ = write_misses;
    power_computed_ = false;
}

void McPATWrapper::setL2Accesses(uint64_t reads, uint64_t writes,
                                  uint64_t read_misses, uint64_t write_misses) {
    l2_reads_ = reads;
    l2_writes_ = writes;
    l2_read_misses_ = read_misses;
    l2_write_misses_ = write_misses;
    power_computed_ = false;
}

void McPATWrapper::setL3Accesses(uint64_t reads, uint64_t writes,
                                  uint64_t read_misses, uint64_t write_misses) {
    l3_reads_ = reads;
    l3_writes_ = writes;
    l3_read_misses_ = read_misses;
    l3_write_misses_ = write_misses;
    power_computed_ = false;
}

void McPATWrapper::setMemControllerAccesses(uint64_t reads, uint64_t writes) {
    mc_reads_ = reads;
    mc_writes_ = writes;
    power_computed_ = false;
}

void McPATWrapper::setMCTechParams(const MCTechParams& params) {
    mc_tech_ = params;
    power_computed_ = false;
}

void McPATWrapper::setNoCLevels(const std::vector<NoCLevelConfig>& levels) {
    noc_levels_ = levels;
    power_computed_ = false;
}

void McPATWrapper::setNoCActivity(const NoCActivityStats& stats) {
    noc_activity_ = stats;
    power_computed_ = false;
}

/* 1.11.16 (verification audit): ONE authority for the DRAM-periphery family
 * factors. They were duplicated as literals in computePower's block-split,
 * which is exactly the aggregate-vs-parts drift trap 1.11.12 removed by
 * moving the transform into the tool -- a future edit to the emitted factors
 * would have left the printed core split scaled by stale numbers. */
/* 1.11.21 (user ruling E1+E2, "I want CORRECTNESS"): the AREA factor is not
 * a constant. It is a RATIO between two columns of the SAME CACTI table --
 * l_phy(comm-dram) / l_phy(BASELINE) -- and mcfg.device_type is what names
 * the baseline. Shipping it as a literal meant the number was the hp ratio
 * and only the hp ratio, while device_type could move the baseline out from
 * under it. From the tables themselves:
 *
 *   l_phy (um)   hp      lstp    lop     lp-dram   comm-dram
 *     22 nm      0.009   0.014   0.011   0         0.022
 *     32 nm      0.013   0.020   0.016   0.056     0.032
 *
 *   comm-dram/hp   = 2.444 (22) / 2.462 (32)  <- the old 2.44 / 2.46
 *   comm-dram/lstp = 1.571 (22) / 1.600 (32)  <- what the override silently
 *                                                kept reporting as 2.44
 *
 * so the literal hid a 1.55x error on any non-hp baseline. Read the row.
 *
 * Note lp-dram is ALL ZERO at 22 nm and fully populated at 32/45 nm. That is
 * the fact the 1.11.13 refusal cites, and it is node-specific -- so it is
 * CHECKED here against the table rather than asserted for every node. */
/* Read one parameter row from a CACTI technology table. The five columns are
 * hp, lstp, lop, lp-dram, comm-dram in that order. temp >= 0 selects the
 * temperature-indexed variant (I_off_n), where the first number on the line is
 * the temperature in Celsius and the five columns follow it. */
static bool cactiRow(int table_nm, const char* tag, double col[5], int temp = -1) {
#ifndef CACTI_DATA_DIR
    (void)table_nm; (void)tag; (void)col; (void)temp; return false;
#else
    std::string path = std::string(CACTI_DATA_DIR) + "/tech_params/" +
                       std::to_string(table_nm) + "nm.dat";
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind(tag, 0) != 0) continue;
        std::vector<double> n;
        std::istringstream is(line);
        std::string tok;
        while (is >> tok) {
            try { size_t used = 0; double v = std::stod(tok, &used);
                  if (used == tok.size()) n.push_back(v); }
            catch (...) { /* the tag and its "(unit)" */ }
        }
        if (temp < 0) {
            if (n.size() < 5) continue;
            for (int i = 0; i < 5; ++i) col[i] = n[i];
            return true;
        }
        if (n.size() < 6 || static_cast<int>(n[0] + 0.5) != temp) continue;
        for (int i = 0; i < 5; ++i) col[i] = n[i + 1];
        return true;
    }
    return false;
#endif
}

/* 1.11.21 (user rulings E1+E2): the DRAM-periphery family factors are all
 * THREE derived from the CACTI table the run is already using. None is a
 * constant. Each reproduces the literal it replaces, at the literal's own
 * conditions -- which is how we know the derivations are the ones the
 * original numbers came from:
 *
 *   fa = l_phy(comm-dram) / l_phy(base)
 *        22nm hp 2.4444 (was 2.44)      32nm hp 2.4615 (was 2.46)
 *   fd = [(C_g_ideal + C_fringe)(cd) / (...)(base)] * (Vdd_cd/Vdd_base)^2
 *        22nm hp 0.8241 (was 0.82)      32nm hp 0.6646 (was 0.66)
 *        The Vdd^2 term is NOT a double count: applyFam multiplies McPAT's
 *        already-computed dynamic (processor.cc:443), and McPAT computed it
 *        at the BASE column's Vdd, so the conversion to the comm-dram rail
 *        has to happen here.
 *   fl = [I_off_n(cd,T) / I_off_n(base,T)] * (Vdd_cd/Vdd_base)
 *        22nm hp @50C 1.035e-5 (was 1.0e-5)   32nm hp @50C 3.119e-6 (was 3.1e-6)
 *
 * TEMPERATURE, and a defect the derivation exposed: the leakage literals
 * reproduce the table at 50 C, but temperature_k defaults to 350 K = 77 C and
 * that is what every run configures. Leakage is strongly temperature
 * dependent -- at 22nm the derived factor is 1.035e-5 at 50 C and 7.0e-6 at
 * 70 C, a 30% difference -- so the shipped constant was evaluated at a
 * temperature no run uses. fl now reads the row for the CONFIGURED
 * temperature, snapped to the nearest tabulated 10 C step.
 *
 * BASELINE, and the corner axis (E2): for a DRAM-periphery component the
 * low-power device is lp-dram, NOT logic lstp -- the low-power variant of a
 * DRAM periphery transistor is the one the DRAM table carries. So on a
 * periphery placement power.device_corner maps hp -> hp and lstp|lop ->
 * lp-dram. That column is ALL ZERO at 22 nm and fully populated at 32/45 nm,
 * so the refusal is a populated-column CHECK per node, not a blanket rule
 * justified by a 22 nm-only fact. */
/* 1.11.22: the ratio spans TWO tables, and both ends are now named.
 *
 * 1.11.21 derived the factors but read numerator AND denominator from the
 * DRAM table. McPAT does not price there: it prices the component at
 * tech_node_nm (the logic node), while dram_table_nm comes from the memory
 * TECHNOLOGY (DDR3 -> 32, everything else -> 22). When they differ, the
 * denominator was hp at the DRAM table's node while McPAT had priced at hp of
 * the logic node -- l_phy(hp) is 0.009 at 22 nm and 0.013 at 32 nm, so the
 * product was neither node's comm-dram. Same defect class as E1, one level
 * up: a within-table ratio multiplied across tables.
 *
 * Inert for the current corpus (non-DDR3 maps to 22 and the fleet runs
 * tech_node_nm=22, so the tables coincide and it cancels). It bites DDR3 at
 * any node, and every technology at a non-22 nm logic node -- both of which
 * the 300-cell fleet contains.
 *
 *   numerator   comm-dram column @ dram_table_nm   (what the PE becomes)
 *   denominator baseline column  @ logic_node_nm   (what McPAT priced) */
static bool periphFamilyFactors(int dram_table_nm, int logic_node_nm,
                                int baseline_device, int temp_k,
                                double& fa, double& fd, double& fl,
                                double& fg) {   // 1.11.54 (E004): gate-leakage ratio
    double lp_d[5], cg_d[5], cf_d[5], vd_d[5], io_d[5];   // DRAM table
    double lp_l[5], cg_l[5], cf_l[5], vd_l[5], io_l[5];   // logic table
    if (!cactiRow(dram_table_nm, "-l_phy", lp_d) ||
        !cactiRow(dram_table_nm, "-C_g_ideal", cg_d) ||
        !cactiRow(dram_table_nm, "-C_fringe", cf_d) ||
        !cactiRow(dram_table_nm, "-Vdd", vd_d)) return false;
    if (!cactiRow(logic_node_nm, "-l_phy", lp_l) ||
        !cactiRow(logic_node_nm, "-C_g_ideal", cg_l) ||
        !cactiRow(logic_node_nm, "-C_fringe", cf_l) ||
        !cactiRow(logic_node_nm, "-Vdd", vd_l)) return false;
    if (baseline_device < 0 || baseline_device > 4) return false;
    const int B = baseline_device, CD = 4;
    if (!(vd_l[B] > 0.0) || !(lp_l[B] > 0.0)) return false;   // column unpopulated
    if (!(vd_d[CD] > 0.0) || !(lp_d[CD] > 0.0)) return false;

    fa = lp_d[CD] / lp_l[B];
    const double c_base = cg_l[B] + cf_l[B], c_cd = cg_d[CD] + cf_d[CD];
    if (!(c_base > 0.0)) return false;
    const double vr = vd_d[CD] / vd_l[B];
    fd = (c_cd / c_base) * vr * vr;

    /* 1.11.52 (audit C001): index the row the way CACTI itself does.
     *
     * The I_off_n rows are labelled by an OFFSET FROM 300 K, not by Celsius:
     * external/cacti/parameter.cc:175 selects a row with
     *     if (thermal_temp == (temperature - 300))
     * where `temperature` is g_ip->temp, the same Kelvin value PIMID emits as
     * sys.temperature. This code read the label as Celsius (temp_k - 273), so
     * at the default 350 K it took row 80 -- the row CACTI uses for 380 K --
     * while McPAT priced the design at 350 K. Measured on 22nm.dat: the
     * leakage factor was 6.81e-6 where the correct row gives 1.035e-5, i.e.
     * the DRAM-periphery leakage ratio was understated by ~34%. With the
     * 1.11.52 temperature knob the error was a fixed +27..+30 K offset at
     * every legal setting, coinciding only at 400 K by clamping accident. */
    int t_row = ((static_cast<int>(temp_k) - 300 + 5) / 10) * 10;
    if (t_row < 0) t_row = 0;
    if (t_row > 100) t_row = 100;
    if (!cactiRow(dram_table_nm, "-I_off_n", io_d, t_row)) return false;
    if (!cactiRow(logic_node_nm, "-I_off_n", io_l, t_row)) return false;
    if (!(io_l[B] > 0.0)) return false;
    fl = (io_d[CD] / io_l[B]) * vr;
    /* 1.11.54 (audit E004): GATE leakage gets its OWN ratio. The transform
     * scaled gate leakage by fl -- a SUBTHRESHOLD ratio derived from the
     * I_off_n rows -- but gate leakage is oxide tunnelling, which McPAT
     * computes with cmos_Ig_leakage and CACTI tabulates separately as
     * I_g_on_n. The two do not track each other: at 22 nm the comm-dram
     * device's off-current is ~10x the hp device's while its gate current is
     * a different ratio entirely, so applying one to the other mis-scales a
     * term PIMID reports (gate_leakage is read, not merely displayed).
     * Falls back to fl when a table lacks the rows, so an older .dat cannot
     * turn this into a hard failure. */
    double ig_d[5], ig_l[5];
    if (cactiRow(dram_table_nm, "-I_g_on_n", ig_d, t_row) &&
        cactiRow(logic_node_nm, "-I_g_on_n", ig_l, t_row) &&
        ig_l[B] > 0.0 && ig_d[CD] > 0.0) {
        fg = (ig_d[CD] / ig_l[B]) * vr;
    } else {
        /* MEASURED 1.11.54: CACTI tabulates NO gate-leakage current for the
         * DRAM device columns -- I_g_on_n is 0 for both lp-dram and comm-dram
         * at 22, 32 and 45 nm, while the logic columns carry real values. So
         * this branch is the one that runs today, at every node, and the
         * gate-leakage ratio is UNSOURCED.
         *
         * The subthreshold ratio stands in for it, which is directionally
         * defensible rather than arbitrary: both mechanisms move the same way
         * between these devices. A periphery transistor is high-Vt (hence
         * fl ~ 1e-5, a huge subthreshold reduction) AND thick-oxide, because
         * it must withstand the boosted wordline rail -- and gate leakage is
         * exponential in oxide thickness, so it too collapses. The magnitude
         * is not sourced and is stated as such wherever it is reported. */
        fg = fl;
    }
    return true;
}

bool McPATWrapper::periphFactorsFor(int dram_table_nm, int logic_node_nm,
                                    int baseline_device, int temp_k,
                                    double& fa, double& fd, double& fl) {
    double fg_unused = 0.0;   // 1.11.54: reporting callers do not need it
    return periphFamilyFactors(dram_table_nm, logic_node_nm, baseline_device,
                               temp_k, fa, fd, fl, fg_unused);
}

/* 1.11.51 (L105/L68): the legacy 3-arg wrapper is GONE. It priced the
 * baseline at the DRAM table's own node with device hp and 350 K, while
 * main.cpp derived the same factors at the CONFIGURED logic node, corner
 * baseline and temperature -- two factor computations in one run, and the
 * XML (the applied one) was the wrong one. Every caller now names all four
 * inputs; a table that cannot be read is a hard stop, not a fallback. */
static void periphFamilyFactorsCfg(int table_nm, int logic_node_nm,
                                   int baseline_device, int temp_k,
                                   double& fa, double& fd, double& fl,
                                   double& fg) {   // 1.11.54 (E004)
    if (!periphFamilyFactors(table_nm, logic_node_nm, baseline_device,
                             temp_k, fa, fd, fl, fg)) {
        std::cerr << "[power] FATAL: cannot derive the DRAM-periphery factors"
                  << " (comm-dram from " << table_nm << "nm.dat / baseline"
                  << " device " << baseline_device << " from "
                  << logic_node_nm << "nm.dat at " << (temp_k - 273)
                  << "C).\n";
        std::exit(2);
    }
}

/* 1.11.29 (user ruling, step 1): does this link class have an off-package
 * SerDes PHY? McPAT's withPHY includes/excludes BOTH the SerDes area and its
 * dynamic term, so it is the single largest structural lever we have -- and
 * PIMID emitted the literal 1 for every class.
 *
 * An INTERPOSER link is a wide parallel on-package connection over microbumps:
 * no serialiser, no equaliser, no CDR. Charging it a full off-package SerDes
 * is wrong in KIND, not degree -- which is why it leads this ordering. Our own
 * termination model already returns zero for HBM "by physics (interposer)",
 * and our link table prices it at 0.5 pJ/bit against PCIe gen5's 7.0; paying
 * for a SerDes here contradicted both in the same run.
 *
 * Everything else in the table (PCIe, CXL, NVLink, UALink) genuinely rides a
 * SerDes, so they keep the PHY. CXL in particular runs on the PCIe gen5/6
 * electrical PHY -- pricing its PHY as PCIe is correct; what CXL adds beyond
 * it is coherence logic, which we do NOT model and which is recorded as an
 * acknowledged omission rather than invented. */
/* 1.11.29 (step 3): per-lane SerDes rate, Gb/s, cited per class. McPAT's
 * dynamic term is 10 mW/(Gb/s) x rate, and the rate was the literal 4 --
 * PCIe 2.0. These are the published signalling rates of the classes our link
 * table already prices:
 *   PCIe gen3  8.0 GT/s    gen4 16.0    gen5 32.0    (PCI-SIG)
 *   CXL 1.1/2.0 rides the gen5 PHY; CXL 3.0 the gen6 PHY -> gen5 rate here,
 *              which is also why pricing its PHY as PCIe is correct: it IS.
 *   NVLink 4.0 100 Gb/s per differential pair (NVIDIA)
 *   UALink 1.0 200G-class SerDes
 *   interposer parallel, no SerDes -- withPHY=0, so the rate is unused.
 * Returns <=0 when the class is unknown, which leaves McPAT's historical 4
 * rather than inventing a rate for something we cannot name. */
double McPATWrapper::linkSerDesLaneGbps(const std::string& link_type) {
    if (link_type.rfind("pcie_gen3", 0) == 0) return 8.0;
    if (link_type.rfind("pcie_gen4", 0) == 0) return 16.0;
    if (link_type.rfind("pcie_gen5", 0) == 0) return 32.0;
    if (link_type.rfind("cxl", 0)      == 0) return 32.0;
    if (link_type.rfind("nvlink", 0)   == 0) return 100.0;
    if (link_type.rfind("ualink", 0)   == 0) return 200.0;
    return -1.0;
}

bool McPATWrapper::linkHasSerDes(const std::string& link_type) {
    if (link_type.rfind("interposer", 0) == 0) return false;
    return true;
}

/* 1.11.34: lanes per PHY module, from UCIe. An interposer link is NOT counted
 * in PCIe lanes -- UCIe organises the advanced package (interposer/bridge) in
 * modules of N=64 single-ended unidirectional data lanes, against N=16 for the
 * standard organic package. Defaulting an interposer to a PCIe lane count was
 * a category error: they are different quantities.
 * Returns <=0 when the class has no module structure of its own, leaving the
 * user's power.link.num_lanes to govern. */
int McPATWrapper::linkLanesPerModule(const std::string& link_type) {
    if (link_type.rfind("interposer", 0) == 0) return 64;   // UCIe advanced pkg
    return -1;                                              // SerDes classes: user lanes
}

/* 1.11.34: does this class run a transaction/data-link protocol stack?
 * YES for every class we model, INCLUDING interposer -- and that reverses an
 * earlier assumption of mine. UCIe's die-to-die adapter explicitly carries
 * "PCIe flit mode" and "CXL flit mode" over the D2D link, so the protocol
 * logic McPAT prices as ctrl_area is genuinely present; only the SerDes is
 * not. Removing the stack for interposer, as this release first proposed,
 * would have under-counted real silicon.
 * (UCIe: FDI between protocol layer and adapter, RDI between adapter and PHY.)
 *
 * NOT SOURCEABLE, stated rather than guessed: UCIe PHY AREA. No public mm^2 or
 * gate count exists -- the IP vendors do not publish it -- so an interposer
 * PHY's area is McPAT's PCIe controller minus its SerDes, which is a bound
 * rather than a measurement. */
bool McPATWrapper::linkHasProtocolStack(const std::string& /*link_type*/) {
    return true;
}

McPATWrapper::LinkEnergyBand
McPATWrapper::linkEnergyBandPJPerBit(const std::string& link_type) {
    LinkEnergyBand b;
    /* PCIe gen5. SOURCED BAND, and the reason this change exists: the old
     * scalar was 7.0, which is BELOW the lowest published figure. A single
     * value picked without looking at the spread landed outside the spread.
     *   7.6  pJ/bit  PCIe gen5/SAS4 SerDes, Samsung 8LPP (semiwiki)
     *  11.4  pJ/bit  32 Gb/s NRZ 37dB SerDes, 10nm CMOS, PCIe Gen5 --
     *                INCLUDING PLL and clocking (IEEE, doc 9075947)
     * The 1.5x spread is mostly what the measurement counted: clocking in or
     * out. That is a boundary, not noise, so both ends are carried. */
    if (link_type.rfind("pcie_gen5", 0) == 0) {
        b.lo = 7.6; b.hi = 11.4;
        b.provenance = "Samsung 8LPP 7.6 (excl clocking) .. 10nm CMOS 11.4 (incl PLL+clocking)";
        return b;
    }
    /* PCIe gen4. TWO published figures, and the gap between them is a
     * BOUNDARY, not a spread -- they measure different things:
     *   1.93 pJ/bit  TRANSMITTER ONLY, 16 Gb/s, 28 nm CMOS, including
     *                interface, bias and BIST (MDPI Electronics 10(1):68)
     *   6.0  pJ/bit  FULL SerDes (TX+RX), Analog Bits Gen4 1-16G on Samsung
     *                7LPP/5LPE (semiwiki)
     * A real link needs both ends, so 1.93 is a LOWER BOUND that no complete
     * link can reach, not an achievable full-link figure. Carried as the band
     * floor with that stated, because hiding it would discard the measurement
     * and inventing a TX+RX total from it would be fabrication. */
    if (link_type.rfind("pcie_gen4", 0) == 0) {
        b.lo = 1.93; b.hi = 6.0;
        /* 1.11.56 (audit C002): mark the floor as a BOUND, so the scalar the
         * XML is priced from stops being the midpoint of it. The band text
         * three lines up already said 1.93 is a figure no complete link can
         * reach, and the applied number was 0.5*(1.93+6.0) = 3.965 -- 0.66x
         * the only full-link source in the entry. Averaging an unreachable
         * end into an applied value is not a band, it is a discount. With the
         * flag set, applied() returns the full-link end and the band is still
         * reported whole, floor included, so nothing is hidden. */
        b.lo_is_bound = true;
        b.provenance = "1.93 TX-ONLY 28nm (LOWER BOUND, not a full link; excluded from the applied value) .. 6.0 full SerDes Samsung 7LPP/5LPE (applied)";
        return b;
    }
    /* PCIe gen3: one figure, and it RETIRES the 5.0 this table used to return,
     * which had no source. Analog Bits Gen3 1-8G on Samsung 7LPP/5LPE. */
    if (link_type.rfind("pcie_gen3", 0) == 0) {
        b.lo = b.hi = 4.0; b.single_point = true;
        b.provenance = "4.0 full SerDes Samsung 7LPP/5LPE (semiwiki); retires unsourced 5.0";
        return b;
    }
    /* CXL rides the gen5 PHY plus a coherence delta, so its band is the gen5
     * band shifted by that delta -- derived from gen5, not independently
     * asserted, so the two cannot drift apart. */
    if (link_type.rfind("cxl", 0) == 0) {
        LinkEnergyBand g5 = linkEnergyBandPJPerBit("pcie_gen5");
        /* 1.11.56 (audit C003): the delta is UNSOURCED and now says so out
         * loud. It is not a published measurement -- it is the difference
         * between two scalars this file has since retired as unsourced
         * themselves (cxl 8.4 minus pcie_gen5 7.0), so it inherits their
         * standing rather than repairing it. We cannot source it: no
         * published figure separates CXL's coherence-controller energy from
         * the gen5 PHY it rides. It is kept because deleting it would assert
         * the opposite unsourced claim (that coherence is free), and it is
         * warned about once per process so a run that prices a CXL link
         * cannot present the +1.4 pJ/bit as a fact. */
        const double coherence_delta = 1.4;
        static bool warned_cxl_delta = false;
        if (!warned_cxl_delta) {
            warned_cxl_delta = true;
            std::cerr << "[McPATWrapper] WARNING: the CXL coherence delta ("
                      << coherence_delta << " pJ/bit, added to both ends of "
                         "the PCIe gen5 band) is UNSOURCED -- it is the "
                         "difference of two retired unsourced scalars, not a "
                         "published measurement. It governs the link SerDes "
                         "dynamic energy and the reported link-energy band "
                         "for every cxl* link type." << std::endl;
        }
        b.lo = g5.lo + coherence_delta;
        b.hi = g5.hi + coherence_delta;
        b.provenance = "gen5 band + 1.4 coherence delta (delta UNSOURCED: difference of two retired scalars, not a measurement)";
        return b;
    }
    /* UCIe. The range was ALREADY sourced and written down here, then thrown
     * away by returning 0.5. ADVANCED package (interposer/bridge, <=2 mm):
     * 0.25-0.5 pJ/bit. STANDARD package (organic, <=25 mm): 0.5-1 pJ/bit.
     * (uciexpress.org; UCIe overview, Hot Chips 2023 tutorial.) We model the
     * advanced package, so that is the band carried. */
    if (link_type.rfind("interposer", 0) == 0) {
        b.lo = 0.25; b.hi = 0.5;
        b.provenance = "UCIe advanced package 0.25-0.5 (uciexpress.org, HC2023)";
        return b;
    }
    /* NVLink. TWO ANGLES, which is what makes this a band rather than a point:
     *   1.17 pJ/bit  measured PHY -- 25 Gb/s/pin ground-referenced single-ended
     *                signalling, 16 nm FinFET (NVIDIA Research, JSSC 2019)
     *   1.30 pJ/bit  NVIDIA's product-level figure for NVLink
     * A research PHY and a shipped product are different claims; keeping both
     * ends preserves that rather than averaging one into the other. */
    if (link_type.rfind("nvlink", 0) == 0) {
        b.lo = 1.17; b.hi = 1.30;
        b.provenance = "1.17 measured PHY 16nm FinFET (JSSC 2019) .. 1.30 NVIDIA product figure";
        return b;
    }
    /* UALink 200G. SOURCED, and it RETIRES the 8.0 that D1 assumed from
     * "200G-class SerDes" -- the published figure is 3.5 pJ/bit for 200 Gb/s
     * reaching the edge of the board, INCLUDING the SerDes and short-reach
     * link DSP (arXiv 2510.15893). The old assumption was 2.3x above it.
     * Single point: only the short-reach case is published, so a longer-reach
     * UALink would sit above this and that end is NOT sourced. */
    if (link_type.rfind("ualink", 0) == 0) {
        b.lo = b.hi = 3.5; b.single_point = true;
        b.provenance = "3.5 @200G short-reach incl SerDes+DSP (arXiv 2510.15893); long-reach end NOT sourced; retires assumed 8.0";
        return b;
    }
    return b;   // invalid: caller warns and prices at zero
}

double McPATWrapper::linkEnergyPJPerBit(const std::string& link_type) {
    LinkEnergyBand b = linkEnergyBandPJPerBit(link_type);
    /* 1.11.56 (audit C002): applied(), not mid(). A band end flagged as a
     * BOUND is not a candidate value to average -- see the gen4 entry. */
    return b.valid() ? b.applied() : -1.0;
}

void McPATWrapper::setPCIeStats(const PCIeStats& stats) {
    pcie_stats_ = stats;
    power_computed_ = false;
}

void McPATWrapper::setDeviceProfile(DeviceProfile profile) {
    device_profile_ = profile;
    power_computed_ = false;
}

void McPATWrapper::createMcPATInput() {
    /* 1.11.56 (audit C014): these lines go to STDERR now. This function used
     * to run twice -- once in the parent (dead work, removed) and once in the
     * forked child -- and the parent's copy was where the log line came from.
     * With the dead call gone the only remaining caller is the child, whose
     * STDOUT is /dev/null, so on std::cout the diagnostic would have
     * disappeared along with the dead work. Stderr survives the redirect. */
    /* 1.11.60 (audit round 4, C006): SAY WHICH PASS THIS IS. There are two,
     * and until now both announced themselves with the same two lines, so the
     * log showed "Generated XML configuration:" twice -- once for
     * /tmp/mcpat_input_0.xml, which initialize() writes from a wrapper no
     * setter has touched and which nothing ever reads, and once for the file
     * the run was really priced from. A reader had no way to tell them apart
     * and would naturally take the first. initialize() is the only caller that
     * reaches here with initialized_ still false, so that flag names the pass
     * exactly. */
    const bool priced_pass = initialized_;
    std::cerr << (priced_pass
                    ? "[McPATWrapper] Creating McPAT configuration"
                    : "[McPATWrapper] Validating McPAT configuration "
                      "(pre-setter probe; not the priced input)")
              << std::endl;

    if (!config_.xml_file.empty() && user_provided_xml_) {
        std::cerr << "  Using XML file: " << config_.xml_file << std::endl;
        valid_ = true;
        return;
    }

    // Generate XML configuration from parameters
    std::string xml_content = generateXMLConfig();

    // Write to temporary file for McPAT.
    /* Unique per call so cosim per-node analysis does not overwrite the host
     * XML before we can examine it.
     * 1.11.56 (audit C014): this path is NOT diagnostic-only, whatever the
     * old note said -- it is the file the forked child hands to ParseXML, so
     * the index is load-bearing. It is also why the parent-side generation in
     * computePower() was dead work: a second call here hands back a different
     * name, so a caller cannot pre-generate "the" XML for the child. */
    /* 1.11.56 (audit C014, main.cpp half): the PARENT names the file when it
     * wants to archive it. The index lives in a function-local static that
     * only the child increments, so the parent could never predict the path
     * -- which is why the archive step in main.cpp had been copying a name
     * nothing writes. When pending_xml_path_ is set the child writes there
     * and the parent knows where to look; otherwise the old indexed name
     * stands. */
    std::string temp_xml;
    if (!pending_xml_path_.empty()) {
        temp_xml = pending_xml_path_;
    } else {
        static int xml_call_idx = 0;
        temp_xml = "/tmp/mcpat_input_" + std::to_string(xml_call_idx++) + ".xml";
    }
    std::ofstream xml_file(temp_xml);
    if (!xml_file) {
        throw std::runtime_error("Failed to create McPAT XML file");
    }

    xml_file << xml_content;
    xml_file.close();

    config_.xml_file = temp_xml;
    valid_ = true;

    /* 1.11.60 (audit round 4, C006): the probe's file is labelled as a probe.
     * It is left on disk rather than removed: config_.xml_file points at it,
     * and a direct caller of runMcPAT() (no fork, no pending path) still
     * relies on that. What changes is that the log no longer presents it as
     * the priced input. */
    std::cerr << (priced_pass ? "  Generated XML configuration: "
                              : "  Wrote configuration probe (unpriced, unread): ")
              << temp_xml << std::endl;  // 1.11.56 (C014), 1.11.60 (C006)
}

/* 1.11.57 (latent C015): the CACTI tech-node clamp, LIFTED OUT of runMcPAT()
 * so that it can be applied in the parent process. Answering the finding's
 * question -- "make the clamp visible to the parent, or make the parent stop
 * believing it applied" -- with the FIRST option, and here is why.
 *
 * WHAT WAS WRONG. runMcPAT() is called from exactly one place: inside the
 * forked child in computePower(). The clamp wrote config_.tech_node_nm = 22
 * in the CHILD's address space. The child serialises power and area numbers
 * back through the result blob and then _Exit()s; config_ does not travel.
 * So the parent kept the node the caller asked for while every number it
 * received had been priced at the clamped node -- and printDetailedResults()
 * prints "Technology: <config_.tech_node_nm> nm" from the parent's copy. A
 * 16 nm request would have been validated at 16, printed at 16, and priced
 * at 22, with the warning line the only evidence and nothing tying it to the
 * report. The same stale field also feeds periphFamilyFactorsCfg() in the
 * parent's block-split, so the two halves could have disagreed about which
 * node's device tables applied.
 *
 * WHY IT WAS INVISIBLE. No reachable path can request a sub-22 nm node any
 * more. Every McPATWrapper construction in main.cpp routes its node through
 * validateTechNodeNm(), which accepts only {22, 32, 45, 65, 90} and exits
 * otherwise, and the process-family pins can only reassign 22 or 32. So the
 * clamp has not fired since that validator landed, and the wrapper's own
 * 7-90 nm window in validateConfiguration() is never the binding constraint.
 *
 * WHY VISIBLE RATHER THAN REMOVED. The clamp is a real CACTI limit, not a
 * fallback we invented, and this class is constructible from outside main.cpp
 * (it is a public API with public setters). Keeping the clamp and making the
 * parent see it is the option that stays correct if someone constructs a
 * wrapper directly; making the parent "stop believing it applied" would mean
 * refusing out-of-range nodes here, which duplicates main.cpp's validator and
 * turns a survivable case into a hard failure for a caller that never asked
 * for one. Latched so the parent's warning is not repeated by the child that
 * inherits it across fork(). */
void McPATWrapper::clampTechNodeForCacti() {
    // CACTI reliably supports tech nodes 22-180nm.
    if (config_.tech_node_nm < 22) {
        if (!warned_tech_clamp_) {
            warned_tech_clamp_ = true;
            std::cerr << "[McPATWrapper] Warning: " << config_.tech_node_nm
                      << "nm not supported by CACTI (min 22nm). Clamping to 22nm for power estimation."
                      << " The configuration now READS 22nm everywhere, including"
                      << " the printed report -- the priced node and the reported"
                      << " node are the same number by construction."
                      << std::endl;
        }
        config_.tech_node_nm = 22;
    } else if (config_.tech_node_nm > 180) {
        if (!warned_tech_clamp_) {
            warned_tech_clamp_ = true;
            std::cerr << "[McPATWrapper] Warning: " << config_.tech_node_nm
                      << "nm not supported by CACTI (max 180nm). Clamping to 180nm for power estimation."
                      << " The configuration now READS 180nm everywhere, including"
                      << " the printed report."
                      << std::endl;
        }
        config_.tech_node_nm = 180;
    }
}

void McPATWrapper::runMcPAT() {
    /* 1.11.57 (latent C015): still applied here, but computePower() has
     * already applied it in the parent, so this call is a no-op in the normal
     * flow and exists only for a direct caller of runMcPAT(). */
    clampTechNodeForCacti();

    // Regenerate XML with current stats (uses clamped tech_node_nm)
    createMcPATInput();

    if (config_.xml_file.empty()) {
        throw std::runtime_error("[McPATWrapper] No XML file available for McPAT");
    }

    // Save CWD and stdout early so they can be restored in both success and error paths
    char saved_cwd[PATH_MAX];
    bool cwd_saved = (getcwd(saved_cwd, sizeof(saved_cwd)) != nullptr);
    std::streambuf* orig_cout = std::cout.rdbuf();

    try {
        // Clean up any previous McPAT objects
        delete mcpat_processor_;
        mcpat_processor_ = nullptr;
        delete mcpat_parser_;
        mcpat_parser_ = nullptr;

        // McPAT global: optimize for target clock rate
        opt_for_clk = true;

        // Parse XML
        mcpat_parser_ = new ParseXML();
        std::string xml_path = config_.xml_file;
        std::vector<char> path_buf(xml_path.begin(), xml_path.end());
        path_buf.push_back('\0');
        mcpat_parser_->parse(path_buf.data());

        // CACTI reads tech_params/*.dat via relative paths -- chdir to CACTI data dir
#ifdef CACTI_DATA_DIR
        if (chdir(CACTI_DATA_DIR) != 0) {
            std::cerr << "[McPATWrapper] Warning: Could not chdir to CACTI data directory: "
                      << CACTI_DATA_DIR << std::endl;
        }
#endif

        // Suppress McPAT/CACTI verbose output.
        // When running inside a subprocess-isolated McPAT child (env var set by
        // computePower()), skip the rdbuf swap entirely: CACTI may call exit(0)
        // on error_checking() failure, and the atexit cleanup of std::cout
        // would then dereference an rdbuf whose stack lifetime ended via the
        // throw/unwind path -- producing a spurious SIGSEGV in the child even
        // when CACTI itself exited cleanly. fd-level redirection (done by the
        // child) handles silencing for the subprocess case.
        const bool in_isolated_child = (getenv("PIMID_MCPAT_CHILD") != nullptr);
        std::ostringstream suppress;
        if (!in_isolated_child) {
            std::cout.rdbuf(suppress.rdbuf());
        }

        // Processor constructor does all computation
        mcpat_processor_ = new Processor(mcpat_parser_);

        // Restore stdout
        if (!in_isolated_child) {
            std::cout.rdbuf(orig_cout);
        }

        // Restore original working directory
        if (cwd_saved) {
            if (chdir(saved_cwd) != 0) { /* ignore */ }
        }

        valid_ = true;
        std::cout << "[McPATWrapper] McPAT power analysis complete" << std::endl;
    } catch (const std::exception& e) {
        // CRITICAL: Restore stdout and CWD before propagating error
        std::cout.rdbuf(orig_cout);
        if (cwd_saved) {
            if (chdir(saved_cwd) != 0) { /* ignore */ }
        }

        delete mcpat_processor_;
        mcpat_processor_ = nullptr;
        delete mcpat_parser_;
        mcpat_parser_ = nullptr;
        valid_ = false;
        throw std::runtime_error(std::string("[McPATWrapper] McPAT failed: ") + e.what());
    } catch (...) {
        std::cout.rdbuf(orig_cout);
        if (cwd_saved) {
            if (chdir(saved_cwd) != 0) { /* ignore */ }
        }

        delete mcpat_processor_;
        mcpat_processor_ = nullptr;
        delete mcpat_parser_;
        mcpat_parser_ = nullptr;
        valid_ = false;
        throw std::runtime_error("[McPATWrapper] McPAT failed with unknown error");
    }
}

// ---------------------------------------------------------------------------
// Subprocess-isolated power analysis.
//
// McPAT links CACTI 6.5-P, which carries non-reentrant globals (interface_ip
// in particular). A second McPAT::Processor() constructor in the same process
// inherits dirty globals from the first call and reliably crashes inside CACTI
// (e.g. "Must have at least one port") during dual-McPAT cosim runs. Restarting
// the process between calls is the only realistic isolation for that library.
//
// Implementation: fork() per computePower(). The child runs the existing
// in-process McPAT pipeline and serializes the result-cache fields it would
// have populated (POD doubles + a small vector of PowerMetrics) to a temp
// file, then _exits. The parent waits, reads the blob, populates its own
// cache. Existing getXxxPower()/area accessors are unchanged because they
// read from those cache fields, not the Processor object itself.
//
// On child crash (non-zero exit / signal), the parent throws -- same surface
// as the original in-process exception path.
// ---------------------------------------------------------------------------
namespace {
struct ResultBlob {
    /* 1.11.57 (latent C012): SELF-IDENTIFYING HEADER. The blob used to carry
     * no statement of what question it answered -- see inputFingerprint()
     * below for the failure that made possible. `magic` catches a file that
     * is not one of ours or was written by a differently-laid-out build;
     * `fingerprint` must equal the parent's own recomputation over the inputs
     * it configured, or the parent refuses the file instead of unpacking it
     * into its result cache. Both are first so a short/truncated file fails
     * on them rather than on a plausible-looking double. */
    uint64_t magic;
    uint64_t fingerprint;
    // Cached PowerMetrics for ComponentType enum entries we extract.
    // 7 entries today: CORE, L1_CACHE, L2_CACHE, L3_CACHE, MEMORY_CONTROLLER,
    // NOC, PCIE. Fixed-size to keep the blob layout trivially POD.
    static constexpr int kNumComponents = 7;
    McPATWrapper::PowerMetrics component_power[kNumComponents];
    McPATWrapper::PowerMetrics system_power;
    double peak_power;
    double core_area_mm2;
    double pcie_area_mm2;   // 1.11.29: link controller area (was dropped)
    double l2_area_mm2;
    double l3_area_mm2;
    double noc_area_mm2;
    double mc_area_mm2;
    double total_area_mm2;
    int num_noc_levels;          // entries that follow, can be 0
    /* 1.9.36: per-core intra-core split (dynamic+leakage, watts, ONE core).
     * Needed because a processing element declared as an ALU is emitted with
     * byte-identical input to an in-order core, so it is charged for an
     * instruction fetch unit, address translation, a load/store unit, a
     * scheduler and a floating-point unit it does not have. Replacing that with
     * a real ALU model cannot be VALIDATED without first knowing how much of the
     * present figure belongs to the parts being removed. Only the child holds
     * the model object, and its standard output is sent to the null device on
     * purpose, so the split has to travel back the same way the areas do. */
    double core_ifu_w, core_lsu_w, core_mmu_w, core_exu_w, core_pipe_w, core_undiff_w;
    /* 1.11.90: the McPAT fork's substitution ledger (basic_components.h),
     * one count per kind: array power clamps and unsolved arrays, router
     * sanitiser clamps, links left at zero power, non-finite reduction
     * factors. The child cannot refuse on its own behalf without losing the
     * count, so it carries them back and the PARENT refuses. */
    int subst_counts[4];
    // After this struct, num_noc_levels * sizeof(PowerMetrics) bytes follow.
};
constexpr McPATWrapper::ComponentType kComponentOrder[ResultBlob::kNumComponents] = {
    McPATWrapper::ComponentType::CORE,
    McPATWrapper::ComponentType::L1_CACHE,
    McPATWrapper::ComponentType::L2_CACHE,
    McPATWrapper::ComponentType::L3_CACHE,
    McPATWrapper::ComponentType::MEMORY_CONTROLLER,
    McPATWrapper::ComponentType::NOC,
    McPATWrapper::ComponentType::PCIE,
};

/* 1.11.57 (latent C012): the blob's format stamp. Bump it whenever the layout
 * of ResultBlob changes, so a file written by a different build is rejected
 * rather than reinterpreted. */
constexpr uint64_t kResultBlobMagic = 0x50494D4944425032ULL;  // "PIMIDBP2" (1.11.90: +subst_counts)

/* FNV-1a over raw bytes. Not a cryptographic hash and does not need to be:
 * its job is to make two DIFFERENT input sets collide with negligible
 * probability, not to resist an adversary. */
struct FnvHash {
    uint64_t h = 1469598103934665603ULL;
    void bytes(const void* p, size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    }
    void operator()(uint64_t v)    { bytes(&v, sizeof v); }
    void operator()(int v)         { int64_t w = v; bytes(&w, sizeof w); }
    void operator()(bool v)        { unsigned char c = v ? 1 : 0; bytes(&c, 1); }
    void operator()(double v)      { bytes(&v, sizeof v); }
    void operator()(const std::string& s) { bytes(s.data(), s.size()); (*this)(0); }
};

}  // namespace

/* 1.11.57 (latent C012): EVERYTHING THAT CHANGES A McPAT ANSWER, hashed.
 *
 * WHAT WAS WRONG. computePower()'s result blob was keyed on the process id
 * alone -- "/tmp/pimid_mcpat_blob_<pid>.bin" -- and the archived XML on a
 * per-process counter. Neither key contained a single INPUT. The complete
 * list of inputs that move a McPAT number is the body of this function: every
 * SystemConfig field (core count and clock, pipeline depth, the pe_* datapath
 * description, issue width and functional-unit counts, device scope, the four
 * cache sizes, controller count and clock, the NoC block, tech node,
 * temperature, process family and its DRAM-periphery table, subarray pitch,
 * MC PHY tier, CACTI device corner, long-channel flag, thread count and
 * interconnect projection) plus every setter's state (total and busy cycles,
 * instruction count, the measured mix, the four cache access sets, MC reads
 * and writes, the MC technology block, the NoC level vector, the legacy NoC
 * activity struct, the PCIe stats, the power-gating spec and the device
 * profile). A key holding none of them cannot tell one question from
 * another, which is the definition of a cache that may answer a question it
 * was not asked.
 *
 * WHY IT WAS INVISIBLE. Within one process the calls are fully serialised by
 * waitpid() and the blob is std::remove()d on every exit path, and across
 * processes the pid differs, so no reachable configuration collides TODAY.
 * That is exactly why it was recorded as latent rather than fixed: the
 * protection is an accident of the current call pattern, not a property of
 * the key. Two threads calling computePower() on two wrappers -- the obvious
 * way to speed up a per-node cosim sweep -- would compute the same path from
 * the same getpid(), and each parent would read whichever child wrote last.
 * The result would be a complete, plausible, silently wrong power report for
 * one of the two nodes, with no diagnostic anywhere.
 *
 * THE FIX has two halves. The key now carries the fingerprint AND a
 * per-call sequence number, so two concurrent calls cannot name the same
 * file; and the blob carries the fingerprint INSIDE it, which the parent
 * checks against its own recomputation before unpacking. The second half is
 * the one that matters: a filename can be made unique, but only a stamped
 * payload can prove that the numbers in it answer the configuration this
 * wrapper is holding.
 *
 * MAINTENANCE. A field added to SystemConfig or a new setter added to this
 * class must be added here too. A missing field does not break the build --
 * it silently narrows the key -- so the rule is: if it reaches the XML, it
 * belongs in this function. config_.xml_file is deliberately EXCLUDED: it is
 * an output of createMcPATInput(), not an input, except in the
 * user_provided_xml_ case, which is hashed as the path string instead. */
uint64_t McPATWrapper::inputFingerprint() const {
    FnvHash f;

    // --- SystemConfig: core and datapath ---
    f(config_.num_cores);         f(config_.core_clock_mhz);
    f(config_.pipeline_depth);    f(config_.pe_lanes);
    f(config_.pe_element_bits);   f(config_.pe_has_fp);
    f(config_.pe_imem_bytes);     f(config_.issue_width);
    f(config_.num_alus);          f(config_.num_muls);
    f(config_.num_fpus);          f(config_.fp_emul_cycles);
    f(config_.device_scope);
    // --- caches ---
    f(config_.l1i_size_bytes);    f(config_.l1d_size_bytes);
    f(config_.l2_size_bytes);     f(config_.l3_size_bytes);
    // --- memory controllers ---
    f(config_.num_memory_controllers);  f(config_.mc_clock_mhz);
    // --- NoC (used when noc_levels_ is empty) ---
    f(config_.has_noc);           f(config_.noc_topology);
    f(config_.noc_num_routers);   f(config_.noc_num_rows);
    f(config_.noc_num_cols);      f(config_.noc_flit_size_bits);
    f(config_.noc_input_ports);   f(config_.noc_output_ports);
    f(config_.noc_vcs_per_vnet);  f(config_.noc_vc_buffer_size);
    f(config_.noc_clock_mhz);
    // --- technology and family ---
    f(config_.tech_node_nm);      f(config_.temperature_k);
    f(config_.process_family);    f(config_.subarray_pitch_factor);
    f(config_.dram_periph_table_nm);
    f(static_cast<int>(config_.mc_phy_tier));
    f(config_.device_type);       f(config_.longer_channel_device);
    f(config_.number_hardware_threads);
    f(config_.interconnect_projection_type);
    /* Only an externally supplied XML is an INPUT; a generated one is an
     * output of this same configuration and hashing it would be circular. */
    f(user_provided_xml_);
    if (user_provided_xml_) f(config_.xml_file);

    // --- setter state: activity ---
    f(total_cycles_);   f(busy_cycles_);   f(total_instructions_);
    f(meas_uops_);      f(meas_branches_); f(meas_mispred_);
    f(meas_int_);       f(meas_mul_);      f(meas_fp_);
    f(meas_ld_);        f(meas_st_);       f(meas_mix_br_);
    f(l1i_reads_);      f(l1i_read_misses_);
    f(l1d_reads_);      f(l1d_writes_);    f(l1d_read_misses_); f(l1d_write_misses_);
    f(l2_reads_);       f(l2_writes_);     f(l2_read_misses_);  f(l2_write_misses_);
    f(l3_reads_);       f(l3_writes_);     f(l3_read_misses_);  f(l3_write_misses_);
    f(mc_reads_);       f(mc_writes_);

    // --- setter state: memory-controller technology ---
    f(mc_tech_.peak_transfer_rate);  f(mc_tech_.databus_width);
    f(mc_tech_.number_ranks);        f(mc_tech_.number_mcs);
    f(mc_tech_.memory_channels_per_mc);
    f(mc_tech_.req_window_size_per_channel);
    f(mc_tech_.io_buffer_size_per_channel);
    f(mc_tech_.block_size_bytes);    f(mc_tech_.addressbus_width);

    // --- setter state: NoC levels (the live path) ---
    f(static_cast<uint64_t>(noc_levels_.size()));
    for (const auto& lvl : noc_levels_) {
        f(lvl.name);            f(lvl.type);
        f(lvl.horizontal_nodes); f(lvl.vertical_nodes);
        f(lvl.input_ports);     f(lvl.output_ports);
        f(lvl.flit_bits);       f(lvl.clock_mhz);
        f(lvl.chip_coverage);   f(lvl.on_dram_die);
        f(lvl.total_accesses);  f(lvl.duty_cycle);
    }

    // --- setter state: legacy NoC activity (the fallback path) ---
    f(noc_activity_.total_packets);   f(noc_activity_.total_flits);
    f(noc_activity_.total_hops);      f(noc_activity_.buffer_reads);
    f(noc_activity_.buffer_writes);   f(noc_activity_.crossbar_traversals);
    f(noc_activity_.arbiter_events);  f(noc_activity_.link_traversals);
    f(noc_activity_.total_cycles);    f(noc_activity_.clock_mhz);

    // --- setter state: link/PCIe ---
    f(pcie_stats_.number_units);      f(pcie_stats_.num_channels);
    f(pcie_stats_.duty_cycle);        f(pcie_stats_.total_load_perc);
    f(pcie_stats_.transferred_bytes); f(pcie_stats_.link_pj_per_bit);
    f(pcie_stats_.link_clock_mhz);    f(pcie_stats_.link_type_name);

    // --- setter state: power gating and profile ---
    f(pg_spec_.pg_core);  f(pg_spec_.pg_noc);  f(pg_spec_.pg_mc);
    f(pg_spec_.r_core);   f(pg_spec_.r_noc);   f(pg_spec_.r_mc);
    f(pg_spec_.r_shared_cache); f(pg_spec_.have_shared_cache);
    f(static_cast<int>(device_profile_));

    return f.h;
}

void McPATWrapper::computePower() {
    if (!initialized_) {
        throw std::runtime_error("[McPATWrapper] Not initialized");
    }

    /* 1.11.56 (audit C014): the parent-side createMcPATInput() call is GONE,
     * and with it a comment that described the opposite of what happened. It
     * claimed the parent generated the XML "so the child reads the same file
     * path we configured". It could not: the child calls runMcPAT(), which
     * calls createMcPATInput() again, and that function allocates a NEW path
     * from a function-local `static int xml_call_idx`. The child therefore
     * never read the parent's file -- the parent wrote one orphan
     * /tmp/mcpat_input_N.xml per computePower() call that nothing read and
     * nothing removed, and (because the counter is copied at fork) the
     * parent's next call reused the path the previous child had written.
     * The child still generates the XML it actually parses; nothing else in
     * the parent reads config_.xml_file or valid_ after this point. */

    /* 1.11.57 (latent C015): clamp the tech node HERE, in the parent, BEFORE
     * fork(). runMcPAT() still clamps, but by then it is running in the child
     * and the mutation dies with it -- see clampTechNodeForCacti(). Applying
     * it first means the node this wrapper reports and the node McPAT prices
     * are the same field. Must precede inputFingerprint() so the parent and
     * the child hash the same node. */
    clampTechNodeForCacti();

    /* 1.11.60 (audit round 4, C006): THE ONE PASS THAT MAY SPEAK.
     *
     * Every latched diagnostic in generateXMLConfig() describes the state the
     * wrapper was handed -- whether a mix was measured, whether NoC levels
     * were supplied, whether the link controller was given a clock. Until now
     * the only pass that could print ran from initialize(), i.e. BEFORE any of
     * those setters, so it described an empty wrapper and latched itself shut;
     * the pass over the real state ran in the forked child with stdout on
     * /dev/null. The result was a log that reported the negative of the run:
     * "no measured load/store mix in this run" on a run with 132014 measured
     * loads, and "(0 reads + 0 writes = 0)" for a substitution that never
     * happened because setNoCLevels() had taken the per-level branch.
     *
     * So the evaluation happens HERE: in the parent, after every setter, after
     * the tech-node clamp, before fork(). The XML string is discarded -- the
     * child regenerates the file it parses, exactly as 1.11.56's C014 arranged
     * -- so this re-adds no orphan file and no second authority over the
     * inputs; it re-adds only the DISCLOSURE that C014 removed along with the
     * dead work. It is a string build over the current configuration, and it
     * happens once per computePower().
     *
     * The alternative -- emitting from the child on stderr, which survives the
     * /dev/null redirect -- was rejected: the child's evaluation happens after
     * the clamp mutation and inside a process whose failure mode is _Exit(),
     * so a message from there cannot be relied on to appear at all when McPAT
     * aborts, and the diagnostics are about the INPUTS, which the parent owns
     * and can state before it risks the fork. */
    {
        diagnostics_live_ = true;
        (void)generateXMLConfig();
        diagnostics_live_ = false;
    }

    /* 1.11.57 (latent C012): the blob key carries the INPUTS now -- a
     * fingerprint over every configuration field and every setter's state --
     * plus a per-call sequence number, so two concurrent computePower() calls
     * in one process cannot name the same file. The pid stays because /tmp is
     * node-local and shared between processes. See inputFingerprint() for the
     * full input list and for what the pid-only key could not distinguish. */
    const uint64_t fingerprint = inputFingerprint();
    static int blob_seq = 0;
    const int this_seq = blob_seq++;
    char blob_path[96];
    std::snprintf(blob_path, sizeof(blob_path),
                  "/tmp/pimid_mcpat_blob_%d_%d_%016llx.bin",
                  (int)getpid(), this_seq,
                  (unsigned long long)fingerprint);

    /* 1.11.56 (audit C014): choose the XML path HERE, in the parent, so
     * lastInputXmlPath() can tell a caller where the priced inputs went.
     * 1.11.57 (latent C012): the same fingerprint goes into the XML name, so
     * an archived input file states which configuration it describes instead
     * of only which call index produced it. */
    {
        static int parent_xml_idx = 0;
        char fp_hex[24];
        std::snprintf(fp_hex, sizeof(fp_hex), "%016llx",
                      (unsigned long long)fingerprint);
        pending_xml_path_ = "/tmp/mcpat_input_p" + std::to_string((int)getpid()) +
                            "_" + std::to_string(parent_xml_idx++) +
                            "_" + fp_hex + ".xml";
    }

    pid_t child = fork();
    if (child < 0) {
        throw std::runtime_error("[McPATWrapper] fork() failed for McPAT isolation");
    }

    if (child == 0) {
        // Child: do the in-process McPAT work, serialize, exit. Any CACTI
        // global pollution dies with this process.
        //
        // Silence McPAT/CACTI chatter at the fd level (not std::cout.rdbuf).
        // CACTI calls exit(0) on error_checking() failure; std::cout cleanup
        // during atexit then reads from an rdbuf whose stack lifetime ended
        // inside runMcPAT() -- that produces a spurious SIGSEGV in the child
        // even when CACTI's own logic exited cleanly. fd-level redirect
        // survives atexit because it does not require any C++ object state.
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        // Tell runMcPAT() to skip the std::cout.rdbuf swap (would crash in
        // atexit if CACTI calls exit() during the new Processor() ctor).
        setenv("PIMID_MCPAT_CHILD", "1", 1);
        try {
            pimid_reset_substitutions();   // 1.11.90: this child's ledger only
            runMcPAT();
            extractResults();

            FILE* f = std::fopen(blob_path, "wb");
            if (!f) std::_Exit(20);

            ResultBlob blob{};
            /* 1.11.57 (latent C012): stamp what this blob answers. The child
             * recomputes the fingerprint from its OWN state rather than
             * copying the parent's variable, so a child that mutated the
             * configuration on its way through runMcPAT() produces a stamp
             * that no longer matches and the parent refuses the file. */
            blob.magic       = kResultBlobMagic;
            blob.fingerprint = inputFingerprint();
            for (int i = 0; i < ResultBlob::kNumComponents; i++) {
                auto it = component_power_.find(kComponentOrder[i]);
                if (it != component_power_.end()) blob.component_power[i] = it->second;
            }
            blob.system_power    = system_power_;
            blob.peak_power      = peak_power_;
            blob.core_area_mm2   = mcpat_core_area_mm2_;
            blob.l2_area_mm2     = mcpat_l2_area_mm2_;
            blob.l3_area_mm2     = mcpat_l3_area_mm2_;
            blob.noc_area_mm2    = mcpat_noc_area_mm2_;
            blob.mc_area_mm2     = mcpat_mc_area_mm2_;
            blob.pcie_area_mm2   = mcpat_pcie_area_mm2_;
            blob.total_area_mm2  = mcpat_total_area_mm2_;
            blob.num_noc_levels  = static_cast<int>(noc_level_power_.size());
            static_assert(PIMID_SUBST_KINDS == 4, "ResultBlob::subst_counts size");
            for (int k = 0; k < PIMID_SUBST_KINDS; k++)
                blob.subst_counts[k] = pimid_substitution_count(k);
            blob.core_ifu_w = blob.core_lsu_w = blob.core_mmu_w = 0.0;
            blob.core_exu_w = blob.core_pipe_w = blob.core_undiff_w = 0.0;
            if (!mcpat_processor_->cores.empty()) {
                const Core& c0 = *mcpat_processor_->cores[0];
                /* 1.11.18 (audit go-through): UNITS. A per-core sub-block's
                 * rt_power.readOp.dynamic is ENERGY (Joules over the run) --
                 * McPAT converts it to Watts only when aggregating into the
                 * Processor level (processor.cc:114 multiplies the core's
                 * rt_power by 1/executionTime). The leakage term is already
                 * Watts. Adding them raw added Joules to Watts, so every
                 * printed block weight was wrong by the run length (and the
                 * blocks-sum-vs-total check compared incomparable units).
                 * Divide the dynamic term, as the tool does. */
                const double execT = (c0.executionTime > 0.0) ? c0.executionTime : 1.0;
                auto w = [execT](const Component* p) -> double {
                    return p ? (p->rt_power.readOp.dynamic / execT
                                + p->rt_power.readOp.leakage) : 0.0;
                };
                /* 1.11.4: block weights carry the family core-power ratio so
                 * the printed split is consistent with the scaled core total
                 * (audit: they were captured unscaled). */
                /* 1.11.15 (audit): fam_core_power_ratio_ lost its assignment
                 * in the 1.11.12 migration, so the blocks printed logic-priced
                 * values under a family-priced total. Scale each block's
                 * dynamic and leakage by the family factors DIRECTLY -- the
                 * per-core sub-objects are not transformed by processor.cc's
                 * applyFam (it works on the Processor-level aggregates).
                 * 1.11.56 (audit C027): fam_core_power_ratio_ itself is now
                 * DELETED from the header. It had been dead since 1.11.12 and
                 * its declaration still claimed these weights were multiplied
                 * by it. They are scaled here, by fdW/flW, and nowhere else. */
                double fdW = 1.0, flW = 1.0;
                if (config_.process_family == 1) {
                    double faW = 1.0;   // 1.11.16: same authority as the XML emission
                    double fgW = 1.0;   // 1.11.54 (E004)
                    periphFamilyFactorsCfg(config_.dram_periph_table_nm,
                                           config_.tech_node_nm,
                                           config_.device_type,
                                           config_.temperature_k,
                                           faW, fdW, flW, fgW);
                    (void)faW; (void)fgW;
                }
                auto wf = [&](const Component* p, const char* label) -> double {
                    if (!p) return 0.0;
                    double dyn = p->rt_power.readOp.dynamic / execT;  // 1.11.18: J -> W
                    double leak = p->rt_power.readOp.leakage;
                    /* 1.11.90: REFUSE, do not zero. A non-finite block made
                     * the printed split silently lose that block. */
                    if (!std::isfinite(dyn) || !std::isfinite(leak)) {
                        throw std::runtime_error(
                            "FATAL: McPAT produced a non-finite value for the "
                            "core sub-block '" + std::string(label) + "' (runtime dynamic "
                            + std::to_string(dyn) + " W, leakage "
                            + std::to_string(leak) + " W). This build used to "
                            "print it as 0 in the core breakdown; power was "
                            "asked for and not produced, so the run stops.");
                    }
                    return dyn * fdW + leak * flW;
                };
                blob.core_ifu_w    = wf(c0.ifu, "ifu");
                blob.core_lsu_w    = wf(c0.lsu, "lsu");
                blob.core_mmu_w    = wf(c0.mmu, "mmu");
                blob.core_exu_w    = wf(c0.exu, "exu");
                blob.core_pipe_w   = wf(c0.corepipe, "corepipe");
                blob.core_undiff_w = wf(c0.undiffCore, "undiffCore");
            }

            std::fwrite(&blob, sizeof(blob), 1, f);
            if (blob.num_noc_levels > 0) {
                std::fwrite(noc_level_power_.data(),
                            sizeof(PowerMetrics), blob.num_noc_levels, f);
            }
            std::fflush(f);
            std::fclose(f);
            std::_Exit(0);
        } catch (const std::exception& e) {
            // Write a one-line diagnostic to the original stderr so the
            // parent's output reflects WHY the child failed (otherwise we
            // see just "exit code 21" and lose the McPAT/CACTI message).
            const char* prefix = "[McPATWrapper child] exception: ";
            (void)!write(STDERR_FILENO, prefix, std::strlen(prefix));
            const char* msg = e.what();
            (void)!write(STDERR_FILENO, msg, std::strlen(msg));
            (void)!write(STDERR_FILENO, "\n", 1);
            std::_Exit(21);
        } catch (...) {
            std::_Exit(21);
        }
    }

    // Parent: wait and reap.
    int status = 0;
    pid_t w;
    do { w = waitpid(child, &status, 0); } while (w < 0 && errno == EINTR);
    if (w < 0) {
        std::remove(blob_path);
        throw std::runtime_error("[McPATWrapper] waitpid failed for McPAT child");
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::remove(blob_path);
        std::string detail;
        if (WIFSIGNALED(status)) {
            detail = "killed by signal " + std::to_string(WTERMSIG(status));
        } else if (WIFEXITED(status)) {
            detail = "exit code " + std::to_string(WEXITSTATUS(status));
        } else {
            detail = "unknown status";
        }
        throw std::runtime_error(
            "[McPATWrapper] McPAT child failed (" + detail + ")");
    }

    // Read the serialized result blob back into our cache fields.
    FILE* f = std::fopen(blob_path, "rb");
    if (!f) {
        throw std::runtime_error("[McPATWrapper] Could not open result blob");
    }
    ResultBlob blob{};
    if (std::fread(&blob, sizeof(blob), 1, f) != 1) {
        std::fclose(f);
        std::remove(blob_path);
        throw std::runtime_error("[McPATWrapper] Short read on result blob");
    }
    /* 1.11.57 (latent C012): CHECK THAT THIS BLOB ANSWERS OUR QUESTION before
     * a single number out of it reaches the result cache. The filename is now
     * unique per call and per input, but a unique name only prevents a
     * collision -- it cannot prove that the bytes at that name were produced
     * from the configuration this wrapper is holding. The stamp can, so it is
     * verified rather than trusted. Throwing is correct here: there is no
     * degraded answer to fall back to, and silently accepting a mismatched
     * blob is precisely the failure this fix exists to make impossible. */
    if (blob.magic != kResultBlobMagic) {
        std::fclose(f);
        std::remove(blob_path);
        throw std::runtime_error(
            "[McPATWrapper] Result blob has the wrong format stamp -- it was "
            "not written by this build. Refusing to read power numbers out of "
            "it.");
    }
    if (blob.fingerprint != fingerprint) {
        std::fclose(f);
        std::remove(blob_path);
        char detail[192];
        std::snprintf(detail, sizeof(detail),
                      "[McPATWrapper] Result blob answers a DIFFERENT "
                      "configuration (blob %016llx, expected %016llx). "
                      "Refusing to report it.",
                      (unsigned long long)blob.fingerprint,
                      (unsigned long long)fingerprint);
        throw std::runtime_error(detail);
    }
    /* 1.11.90: THE SUBSTITUTION LEDGER, checked in the parent. Every place
     * the McPAT fork replaced a number it could not produce (an ArrayST
     * power field clamped to 0 or an array CACTI found no organisation for,
     * a router field sanitised, a link left at zero power, a non-finite
     * reduction factor) printed a "[mcpat] WARNING" line naming it in the
     * child and counted it. Any count refuses the run: power was asked for
     * and part of it was not produced. PIMID_ALLOW_ARRAY_CLAMP=1 is the
     * stated escape for a user who wants the report anyway; the counts are
     * then printed so the log carries them. */
    {
        const int na = blob.subst_counts[PIMID_SUBST_ARRAY];
        const int nn = blob.subst_counts[PIMID_SUBST_NOC];
        const int nl = blob.subst_counts[PIMID_SUBST_LINK];
        const int nr = blob.subst_counts[PIMID_SUBST_REDUCTION];
        if (na + nn + nl + nr > 0) {
            char counts[256];
            std::snprintf(counts, sizeof(counts),
                          "%d array clamp(s) or unsolved array(s), %d router "
                          "field(s) sanitised, %d link(s) left at zero power, "
                          "%d non-finite reduction factor(s)",
                          na, nn, nl, nr);
            const char* allow = std::getenv("PIMID_ALLOW_ARRAY_CLAMP");
            if (allow && std::string(allow) == "1") {
                std::cerr << "[McPATWrapper] WARNING: PIMID_ALLOW_ARRAY_CLAMP=1:"
                             " reporting power although McPAT substituted "
                          << counts << " (each named on a [mcpat] WARNING line"
                             " above). Those structures are priced at zero or"
                             " at a stand-in, not by the model." << std::endl;
            } else {
                std::fclose(f);
                std::remove(blob_path);
                throw std::runtime_error(
                    std::string("McPAT substituted numbers it could not "
                    "produce: ") + counts + " (each named on a [mcpat] "
                    "WARNING line above). Options: change the geometry or "
                    "the node named there, or set PIMID_ALLOW_ARRAY_CLAMP=1 "
                    "to report anyway with the counts printed.");
            }
        }
    }
    component_power_.clear();
    for (int i = 0; i < ResultBlob::kNumComponents; i++) {
        component_power_[kComponentOrder[i]] = blob.component_power[i];
    }
    system_power_         = blob.system_power;
    peak_power_           = blob.peak_power;
    mcpat_core_area_mm2_  = blob.core_area_mm2;
    mcpat_l2_area_mm2_    = blob.l2_area_mm2;
    mcpat_l3_area_mm2_    = blob.l3_area_mm2;
    mcpat_noc_area_mm2_   = blob.noc_area_mm2;
    mcpat_mc_area_mm2_    = blob.mc_area_mm2;
    mcpat_pcie_area_mm2_  = blob.pcie_area_mm2;
    core_ifu_w_    = blob.core_ifu_w;      // 1.9.36
    core_lsu_w_    = blob.core_lsu_w;
    core_mmu_w_    = blob.core_mmu_w;
    core_exu_w_    = blob.core_exu_w;
    core_pipe_w_   = blob.core_pipe_w;
    core_undiff_w_ = blob.core_undiff_w;
    /* 1.9.36: report the split for a device profile. An ALU datapath has a
     * register file, an arithmetic unit and a result bus -- roughly the
     * execution unit alone. Everything else listed here is charged to it today
     * because the generator emits identical input for an ALU and an in-order
     * core. The percentage is what an ALU model has to remove, and is the
     * yardstick that model will be validated against. */
    if (device_profile_ != DeviceProfile::OOO) {
        double tot = core_ifu_w_ + core_lsu_w_ + core_mmu_w_
                   + core_exu_w_ + core_pipe_w_ + core_undiff_w_;
        double absent = core_ifu_w_ + core_lsu_w_ + core_mmu_w_;
        if (tot > 0.0) {
            std::cout << "  [CoreBreakdown] per core, "
                      << (device_profile_ == DeviceProfile::DEVICE_ALU
                              ? "DEVICE_ALU"
                          : (device_profile_ == DeviceProfile::OOO)
                              ? "OOO" : "DEVICE_INORDER")
                      << ": ifu=" << core_ifu_w_ << "W lsu=" << core_lsu_w_
                      << "W mmu=" << core_mmu_w_ << "W exu=" << core_exu_w_
                      << "W pipe=" << core_pipe_w_ << "W undiff=" << core_undiff_w_
                      << "W" << std::endl;
            /* Report the pieces, NOT a single removable percentage.
             *
             * Two McPAT conventions make a naive share misleading, and both were
             * verified in core.cc rather than assumed:
             *   - corepipe is APPORTIONED into ifu/lsu/exu/mmu/rnu (:3959-4023),
             *     so it reads zero afterwards and its cost is already inside
             *     those four. An ALU processing element still HAS a pipeline, so
             *     that share is not removable.
             *   - undiffCore is added straight to the CORE total (:4028) and to
             *     no sub-block, so the blocks do not sum to the core.
             * Consequently ifu+lsu+mmu is an upper bound on what could be
             * removed, and a loose one: instruction supply and address
             * generation are genuinely needed and sit inside it. */
            double core_tot = component_power_.count(ComponentType::CORE)
                ? component_power_[ComponentType::CORE].total_power : 0.0;
            /* 1.11.52 (audit C013): SAY WHY THE TWO DIFFER, correctly. The
             * parenthetical blamed undiffCore, which is false -- undiffCore
             * IS in the block sum (core_undiff_w). The gap is two basis
             * differences the reader cannot see:
             *   (1) POPULATION: the blocks are ONE core (cores[0]); the core
             *       total is all num_cores of them.
             *   (2) LEAKAGE BASIS: the blocks read rt_power (runtime), the
             *       total reads power (peak). 1.11.33 measured those ~6x
             *       apart, because McPAT scales the execution unit's runtime
             *       leakage with utilisation while the peak basis counts
             *       every powered device -- which is why PIMID reports peak.
             * On a 16-PE device that is ~16x times ~6x, i.e. the printed
             * blocks sum sits about two orders below the printed core total.
             * The blocks are a SHAPE (what fraction of a core each unit is),
             * not an addend of the total. */
            std::cout << "  [CoreBreakdown] blocks sum=" << tot << "W (ONE core, "
                         "runtime-leakage basis)";
            if (core_tot > 0.0) std::cout << "  core total=" << core_tot << "W ("
                                          << config_.num_cores
                                          << " core(s), peak-leakage basis) --"
                                             " different population AND different"
                                             " leakage basis; the blocks are a"
                                             " shape, not an addend";
            std::cout << std::endl;
            std::cout << "  [CoreBreakdown] ifu+lsu+mmu = " << absent << "W = "
                      << (100.0 * absent / tot) << "% of the block sum -- an UPPER BOUND on what an "
                         "ALU model removes, not the error: it includes apportioned pipeline power, "
                         "and instruction supply and address generation are still required."
                      << std::endl;
        }
    }
    mcpat_total_area_mm2_ = blob.total_area_mm2;

    noc_level_power_.clear();
    if (blob.num_noc_levels > 0) {
        noc_level_power_.resize(blob.num_noc_levels);
        if (std::fread(noc_level_power_.data(),
                       sizeof(PowerMetrics), blob.num_noc_levels, f)
            != static_cast<size_t>(blob.num_noc_levels)) {
            std::fclose(f);
            std::remove(blob_path);
            throw std::runtime_error("[McPATWrapper] Short read on NoC level blob");
        }
    }
    std::fclose(f);
    std::remove(blob_path);

    power_computed_ = true;

    std::cout << "[McPATWrapper] Power analysis complete" << std::endl;
    std::cout << "  Total Power: " << system_power_.total_power << " W" << std::endl;
}

void McPATWrapper::extractResults() {
    if (!mcpat_processor_) {
        throw std::runtime_error("[McPATWrapper] McPAT processor object is null -- cannot extract results");
    }

    // Extract real results from McPAT Processor object
    bool long_channel = (config_.longer_channel_device != 0);

    /* 1.11.90: a NON-FINITE PROCESSOR-LEVEL AGGREGATE REFUSES. These reads
     * used to be `isfinite(v) ? v : 0.0`, so one NaN router or block silently
     * zeroed a whole component's dynamic or leakage, and the run reported
     * the remainder as the total. The rule since 1.11.88 is that power was
     * asked for and not produced: the run stops. extractResults runs in the
     * forked child; the throw reaches the child's catch (which prints this
     * text on stderr) and the parent's caller turns the failed child into the
     * 1.11.52 FATAL with rc 3.
     *
     * Only a component the user DESCRIBED is checked (componentIsDescribed):
     * an undescribed positional stub is excluded from every total, peak and
     * breakdown, so it is zeroed outright rather than read. */
    auto refuseNonFinite = [](const char* comp, const char* field, double v) {
        if (std::isfinite(v)) return;
        throw std::runtime_error(
            std::string("FATAL: McPAT produced a non-finite ") + field
            + " (" + std::to_string(v) + ") for the " + comp + " component."
            " This build used to replace it with 0 and report the rest as the"
            " total; power was asked for and not produced, so the run stops."
            " Any [mcpat] or [cacti] WARNING line above names the structure.");
    };
    // Helper lambda to extract power from a McPAT Component
    auto extractComponent = [long_channel, &refuseNonFinite](
            const Component& comp, const char* cname, bool described) -> PowerMetrics {
        PowerMetrics pm;
        if (!described) return pm;   // 1.11.90: excluded from every total
        double dyn = comp.rt_power.readOp.dynamic;
        if (pimid_fault("extract")) dyn = NAN;   // 1.11.90 gate injection
        refuseNonFinite(cname, "runtime dynamic power", dyn);
        pm.runtime_dynamic = dyn;
        double sub = long_channel
            ? comp.power.readOp.longer_channel_leakage
            : comp.power.readOp.leakage;
        double gate = comp.power.readOp.gate_leakage;
        refuseNonFinite(cname, long_channel ? "long-channel subthreshold leakage"
                                            : "subthreshold leakage", sub);
        refuseNonFinite(cname, "gate leakage", gate);
        pm.subthreshold_leakage = sub;
        pm.gate_leakage         = gate;
        double pgl = long_channel
            ? comp.power.readOp.power_gated_with_long_channel_leakage
            : comp.power.readOp.power_gated_leakage;                   // 1.11.8
        refuseNonFinite(cname, "power-gated leakage", pgl);
        pm.power_gated_leakage = pgl;
        pm.total_leakage = pm.subthreshold_leakage + pm.gate_leakage;
        pm.total_dynamic = pm.runtime_dynamic;
        pm.total_power = pm.total_dynamic + pm.total_leakage;
        return pm;
    };

    // Core (includes L1 caches in McPAT's model)
    component_power_[ComponentType::CORE] = extractComponent(
        mcpat_processor_->core, "core", componentIsDescribed(ComponentType::CORE));
    // L1 is embedded in core -- zero out separate L1 to avoid double-counting
    component_power_[ComponentType::L1_CACHE] = PowerMetrics();

    // L2
    component_power_[ComponentType::L2_CACHE] = extractComponent(
        mcpat_processor_->l2, "L2", componentIsDescribed(ComponentType::L2_CACHE));

    // L3
    component_power_[ComponentType::L3_CACHE] = extractComponent(
        mcpat_processor_->l3, "L3", componentIsDescribed(ComponentType::L3_CACHE));

    // Memory Controller
    component_power_[ComponentType::MEMORY_CONTROLLER] = extractComponent(
        mcpat_processor_->mcs, "memory controller",
        componentIsDescribed(ComponentType::MEMORY_CONTROLLER));

    // NoC -- use Processor-level aggregate (already normalized energy->Watts)
    // The per-NoC nocs[i]->rt_power stores raw energy; Processor multiplies
    // by 1/executionTime during aggregation into noc.rt_power (Watts).
    noc_level_power_.clear();
    if (mcpat_processor_->numNOC > 0) {
        // Per-level breakdown: divide each nocs[i] energy by executionTime
        for (int i = 0; i < mcpat_processor_->numNOC; i++) {
            double execTime = mcpat_processor_->nocs[i]->nocdynp.executionTime;
            const std::string lvl = "NoC level " + std::to_string(i);
            if (pimid_fault("noclevel")) execTime = 0.0;   // 1.11.90 gate injection
            /* 1.11.90: a non-positive execution time used to become 1.0 s,
             * which prints this level's Joules as Watts. Refused, as is a
             * non-finite energy or leakage (the leakage was unguarded). */
            if (!(execTime > 0.0)) {
                throw std::runtime_error(
                    "FATAL: " + lvl + " has execution time "
                    + std::to_string(execTime) + " s, so its energy cannot be"
                    " turned into power. This build used to divide by 1.0 s"
                    " and print Joules as Watts. The run's cycle count or"
                    " clock did not reach this NoC level.");
            }
            PowerMetrics level_pm;
            double rawDyn = mcpat_processor_->nocs[i]->rt_power.readOp.dynamic;
            refuseNonFinite(lvl.c_str(), "runtime dynamic energy", rawDyn);
            level_pm.runtime_dynamic = rawDyn / execTime;
            level_pm.subthreshold_leakage = long_channel
                ? mcpat_processor_->nocs[i]->power.readOp.longer_channel_leakage
                : mcpat_processor_->nocs[i]->power.readOp.leakage;
            level_pm.gate_leakage = mcpat_processor_->nocs[i]->power.readOp.gate_leakage;
            refuseNonFinite(lvl.c_str(), "subthreshold leakage", level_pm.subthreshold_leakage);
            refuseNonFinite(lvl.c_str(), "gate leakage", level_pm.gate_leakage);
            level_pm.total_leakage = level_pm.subthreshold_leakage + level_pm.gate_leakage;
            level_pm.total_dynamic = level_pm.runtime_dynamic;
            level_pm.total_power = level_pm.total_dynamic + level_pm.total_leakage;
            noc_level_power_.push_back(level_pm);
        }
        // Aggregate: use Processor's pre-computed noc (already in Watts)
        component_power_[ComponentType::NOC] = extractComponent(
            mcpat_processor_->noc, "NoC", componentIsDescribed(ComponentType::NOC));
    } else {
        component_power_[ComponentType::NOC] = PowerMetrics();
    }

    // PCIe
    if (mcpat_processor_->pcie) {
        /* 1.11.7 (audit blocker): read the AGGREGATED component (pcies),
         * not the raw controller object -- the Processor multiplies by
         * number_units x clockRate during aggregation, and reading the raw
         * object under-reported dynamic by exactly that factor. */
        component_power_[ComponentType::PCIE] = extractComponent(
            mcpat_processor_->pcies, "PCIe link controller",
            componentIsDescribed(ComponentType::PCIE));
    }

    // Store areas from McPAT (um^2 -> mm^2)
    mcpat_core_area_mm2_ = mcpat_processor_->core.area.get_area() * 1e-6;
    mcpat_l2_area_mm2_ = mcpat_processor_->l2.area.get_area() * 1e-6;
    mcpat_l3_area_mm2_ = mcpat_processor_->l3.area.get_area() * 1e-6;
    mcpat_noc_area_mm2_ = mcpat_processor_->noc.area.get_area() * 1e-6;
    mcpat_mc_area_mm2_ = mcpat_processor_->mcs.area.get_area() * 1e-6;
    mcpat_pcie_area_mm2_ = mcpat_processor_->pcies.area.get_area() * 1e-6;  // 1.11.29
    mcpat_total_area_mm2_ = mcpat_processor_->area.get_area() * 1e-6;

    /* 1.11.12: the DRAM-periphery family MOVED INTO McPAT (processor.cc,
     * driven by the dram_periph_* XML params). What used to be a
     * post-scaling of extracted results is now a property of the machine
     * McPAT priced, so components, aggregate and areas are transformed by
     * the tool that owns them -- the borders rule, and the end of a class
     * of aggregate-vs-parts drift. Extraction reads what McPAT produced. */

    /* 1.11.8 (#84): power-gating interpolation at the tool boundary.
     * Endpoints are BOTH the tool's (active leakage and power_gated_leakage
     * from the same McPAT/CACTI run); the measured residency r weights them:
     * leak_eff = active*(1-r) + gated*r. Applied only to components the
     * described design gates (pg flags).
     *
     * 1.11.16 (verification audit) -- three corrections here.
     * (1) The interpolation runs on SUBTHRESHOLD leakage only. The gated
     *     endpoint is a subthreshold quantity (every McPAT producer derives
     *     it from power.readOp.leakage alone), while the old active side
     *     used sub+gate: the bases differed by the gate-leakage share, and
     *     gate-oxide tunnelling -- which a sleep rail does NOT remove -- was
     *     credited with PG savings. Gate leakage now stays at its active
     *     value.
     * (2) The 1.11.15 comment claimed savings were CONSERVATIVE because
     *     blocks without sleep-tx models "keep active leakage". They kept
     *     ZERO (the dead-endpoint defect, fixed in array.cc this release),
     *     which is anti-conservative; with the uniform analytic endpoint the
     *     residual is Vcc_min/Vdd (~0.35) of active, McPAT's own retention
     *     floor.
     * (3) Diagnostics go to STDERR: extractResults runs in the forked child
     *     whose stdout is /dev/null, so the 1.11.15 inversion warning was
     *     unreachable. The check is also two-sided now -- a near-zero gated
     *     endpoint (the failure mode that actually shipped) warns just like
     *     an inverted one. */
    {
        auto applyPG = [&](ComponentType t, double r) -> double {
            if (r <= 0.0) return 1.0;
            if (r > 1.0) r = 1.0;
            PowerMetrics& pm = component_power_[t];
            double active = pm.subthreshold_leakage;
            double gated  = pm.power_gated_leakage;
            if (gated > active) {
                std::cerr << "[pg] WARNING: gated leakage (" << gated
                          << " W) exceeds active subthreshold (" << active
                          << " W) -- endpoints on different bases; clamping, "
                             "no savings credited for this component."
                          << std::endl;
                gated = active;
            } else if (active > 0.0 && gated < 0.05 * active) {
                std::cerr << "[pg] WARNING: gated leakage (" << gated
                          << " W) is below 5% of active (" << active
                          << " W) -- a zero/degenerate gated endpoint (the "
                             "1.11.15 dead-endpoint class) credits near-total "
                             "leakage elimination; check the tool endpoints."
                          << std::endl;
            }
            double eff = active * (1.0 - r) + gated * r;
            double scale = (active > 0.0) ? eff / active : 1.0;
            pm.subthreshold_leakage *= scale;
            pm.total_leakage = pm.subthreshold_leakage + pm.gate_leakage;
            pm.total_power   = pm.total_dynamic + pm.total_leakage;
            return scale;
        };
        /* 1.11.18: shared caches gate on the shared-cache signal (#84), not
         * on core retirement; fall back to r_core only when the run carried
         * no shared-cache counter. */
        const double r_sc = pg_spec_.have_shared_cache ? pg_spec_.r_shared_cache
                                                       : pg_spec_.r_core;
        if (pg_spec_.pg_core) applyPG(ComponentType::CORE, pg_spec_.r_core);
        if (pg_spec_.pg_core) applyPG(ComponentType::L2_CACHE, r_sc);
        if (pg_spec_.pg_core) applyPG(ComponentType::L3_CACHE, r_sc);
        if (pg_spec_.pg_noc) {
            double s = applyPG(ComponentType::NOC, pg_spec_.r_noc);
            /* 1.11.16: the per-level NoC breakdown must ride the same scale
             * or the printed levels sum to the pre-gating leakage while the
             * aggregate shows the post-gating figure -- the aggregate-vs-
             * parts drift this train exists to end, one layer up. */
            for (auto& lv : noc_level_power_) {
                lv.subthreshold_leakage *= s;
                lv.total_leakage = lv.subthreshold_leakage + lv.gate_leakage;
                lv.total_power   = lv.total_dynamic + lv.total_leakage;
            }
        }
        if (pg_spec_.pg_mc)   applyPG(ComponentType::MEMORY_CONTROLLER, pg_spec_.r_mc);
    }

    /* System total.
     * 1.11.56 (audit C032): sum ONLY the components the described system
     * has. This loop used to add every entry of component_power_, including
     * two that exist purely to satisfy McPAT's positional XML parser: the
     * 1x1, zero-activity, chip_coverage=0 NoC stub emitted even when
     * has_noc is false, and the memory controller, which is emitted and
     * extracted unconditionally. The breakdown printed next to the total
     * already refused to show them, so the parts did not sum to the whole --
     * reachable today on the dual-McPAT host (has_noc false, no setNoCLevels)
     * and on the NoC probe (num_memory_controllers 0). This MOVES the total
     * on those two paths, downward, by the stubs' leakage. What it does not
     * move is getTotalArea(), which is McPAT's own Processor area and would
     * have to be rebuilt from the parts to be gated the same way; the stub's
     * area is still inside it.
     * 1.11.57 (audit C012): peak_power_ was the OTHER one this note should
     * have named, and it is gated the same way now -- see the peak block
     * below. getTotalArea() remains the single exception. */
    system_power_ = PowerMetrics();
    for (const auto& pair : component_power_) {
        if (!componentIsDescribed(pair.first)) continue;
        system_power_.runtime_dynamic += pair.second.runtime_dynamic;
        system_power_.subthreshold_leakage += pair.second.subthreshold_leakage;
        system_power_.gate_leakage += pair.second.gate_leakage;
        system_power_.total_leakage += pair.second.total_leakage;
    }
    system_power_.total_dynamic = system_power_.runtime_dynamic;
    system_power_.total_power = system_power_.total_dynamic + system_power_.total_leakage;

    // Extract real peak power from McPAT design-time power (power.readOp.dynamic)
    // This is the maximum power assuming all units active at peak frequency,
    // as opposed to rt_power.readOp.dynamic which is scaled by runtime activity.
    bool long_channel_peak = long_channel;
    /* 1.11.90: refuse a non-finite peak read, as the runtime reads above
     * (every call below is already gated on componentIsDescribed). */
    auto peakDynamic = [&refuseNonFinite](const Component& comp, const char* cname) -> double {
        double d = comp.power.readOp.dynamic;
        refuseNonFinite(cname, "peak dynamic power", d);
        return d;
    };
    auto peakLeakage = [long_channel_peak, &refuseNonFinite](const Component& comp,
                                                             const char* cname) -> double {
        double sub = long_channel_peak
            ? comp.power.readOp.longer_channel_leakage
            : comp.power.readOp.leakage;
        double gate = comp.power.readOp.gate_leakage;
        refuseNonFinite(cname, "peak subthreshold leakage", sub);
        refuseNonFinite(cname, "peak gate leakage", gate);
        return sub + gate;
    };

    double peak_dyn = 0.0;
    double peak_leak = 0.0;
    {
        // 1.11.2/1.11.4: peak reads the raw processor structures, so the CORE
        // share carries the same DRAM-periphery factors as the runtime
        // metrics -- same values, same 80C-row derivation, same plain-leakage
        // rebase (no long-channel stacking) as the block above.
        double core_pd = peakDynamic(mcpat_processor_->core, "core");
        double core_pl = peakLeakage(mcpat_processor_->core, "core");
        // 1.11.12: peak reads the same family-priced structures; no rescale.
        peak_dyn += core_pd;
        peak_leak += core_pl;
    }
    /* 1.11.57 (audit C012): the SAME PREDICATE as the runtime total.
     *
     * C032 (1.11.56) applied "report only what the described system has" to
     * the breakdown and to system_power_, and named the one quantity it did
     * not move: getTotalArea(). peak_power_ was the other one, and nobody
     * named it -- so on the two paths C032 is about (the dual-McPAT host, with
     * has_noc false and no setNoCLevels, and the NoC probe with zero memory
     * controllers) the printed "Peak Power" still included the positional
     * stubs that the printed breakdown and the printed system total both
     * excluded. Two totals a few lines apart described two different machines,
     * and 1.11.56's own C006 fix had just made reported PEAK the number that
     * moves. Small in watts -- a 1x1 zero-activity chip_coverage=0 NoC stub is
     * mostly leakage -- but it is the difference between a total that sums its
     * parts and one that does not. */
    if (componentIsDescribed(ComponentType::L2_CACHE)) {
        peak_dyn += peakDynamic(mcpat_processor_->l2, "L2");
        peak_leak += peakLeakage(mcpat_processor_->l2, "L2");
    }
    if (componentIsDescribed(ComponentType::L3_CACHE)) {
        peak_dyn += peakDynamic(mcpat_processor_->l3, "L3");
        peak_leak += peakLeakage(mcpat_processor_->l3, "L3");
    }
    if (componentIsDescribed(ComponentType::MEMORY_CONTROLLER)) {
        peak_dyn += peakDynamic(mcpat_processor_->mcs, "memory controller");
        peak_leak += peakLeakage(mcpat_processor_->mcs, "memory controller");
    }
    if (componentIsDescribed(ComponentType::NOC)) {
        peak_dyn += peakDynamic(mcpat_processor_->noc, "NoC");
        peak_leak += peakLeakage(mcpat_processor_->noc, "NoC");
    }
    if (mcpat_processor_->pcie && componentIsDescribed(ComponentType::PCIE)) {
        peak_dyn += peakDynamic(mcpat_processor_->pcies, "PCIe link controller");   // 1.11.7: aggregated
        peak_leak += peakLeakage(mcpat_processor_->pcies, "PCIe link controller");
    }
    peak_power_ = peak_dyn + peak_leak;
}

//=============================================================================
// Query functions
//=============================================================================

McPATWrapper::PowerMetrics McPATWrapper::getComponentPower(ComponentType component) const {
    auto it = component_power_.find(component);
    if (it != component_power_.end()) {
        return it->second;
    }
    return PowerMetrics();
}

McPATWrapper::PowerMetrics McPATWrapper::getSystemPower() const {
    return system_power_;
}

double McPATWrapper::getCorePower() const {
    return getComponentPower(ComponentType::CORE).total_power;
}

double McPATWrapper::getCachePower() const {
    double total = 0.0;
    total += getComponentPower(ComponentType::L1_CACHE).total_power;
    total += getComponentPower(ComponentType::L2_CACHE).total_power;
    total += getComponentPower(ComponentType::L3_CACHE).total_power;
    return total;
}

double McPATWrapper::getMemoryControllerPower() const {
    return getComponentPower(ComponentType::MEMORY_CONTROLLER).total_power;
}

double McPATWrapper::getNoCPower() const {
    return getComponentPower(ComponentType::NOC).total_power;
}

double McPATWrapper::getComponentArea(ComponentType component) const {
    if (power_computed_) {   // 1.9.34: see getTotalArea()
        switch (component) {
            case ComponentType::CORE:
                return mcpat_core_area_mm2_;
            case ComponentType::L1_CACHE:
                return 0.0;  // included in core
            case ComponentType::L2_CACHE:
                return mcpat_l2_area_mm2_;
            case ComponentType::L3_CACHE:
                return mcpat_l3_area_mm2_;
            case ComponentType::MEMORY_CONTROLLER:
                return mcpat_mc_area_mm2_;
            case ComponentType::PCIE:
                return mcpat_pcie_area_mm2_;   // 1.11.29: was falling through to 0
            case ComponentType::NOC:
                return mcpat_noc_area_mm2_;
            default:
                return 0.0;
        }
    }
    return 0.0;
}

double McPATWrapper::getTotalArea() const {
    /* 1.9.34: gate on power_computed_, not on the Processor object.
     *
     * computePower() became fork()-isolated to survive a co-simulation crash.
     * mcpat_processor_ is only ever assigned inside runMcPAT(), i.e. only in the
     * CHILD; the child extracts the areas and ships them back through the result
     * blob, and the parent caches them into mcpat_*_area_mm2_. But these two
     * accessors kept testing the Processor pointer, which in the parent is
     * permanently null -- so the areas were transported correctly and then
     * discarded, and every reported area was 0.00 mm^2 in BOTH scopes. Every
     * other cached field (system_power_, peak_power_, component_power_) is read
     * without such a guard, which is exactly why power survived and only area
     * was lost. */
    if (power_computed_) {
        return mcpat_total_area_mm2_;
    }
    return 0.0;
}

double McPATWrapper::getPeakPower() const {
    return peak_power_;
}

double McPATWrapper::getEnergyForPeriod(double time_seconds) const {
    return system_power_.total_power * time_seconds;
}

bool McPATWrapper::isValid() const {
    return valid_;
}

std::string McPATWrapper::getErrorMessage() const {
    return error_message_;
}

void McPATWrapper::printDetailedResults() const {
    if (!power_computed_) {
        std::cout << "[McPATWrapper] Power not yet computed" << std::endl;
        return;
    }

    std::cout << "\n=== McPAT Power Analysis Results ===" << std::endl;
    std::cout << "Configuration:" << std::endl;
    std::cout << "  Cores: " << config_.num_cores << " @ " << config_.core_clock_mhz << " MHz" << std::endl;
    std::cout << "  Technology: " << config_.tech_node_nm << " nm" << std::endl;

    printComponentBreakdown();

    // Per-level NoC breakdown
    if (!noc_level_power_.empty() && !noc_levels_.empty()) {
        std::cout << "\nNoC Power Breakdown (" << noc_level_power_.size() << " levels):" << std::endl;
        for (size_t i = 0; i < noc_level_power_.size() && i < noc_levels_.size(); i++) {
            const auto& pm = noc_level_power_[i];
            const auto& cfg = noc_levels_[i];
            std::cout << "  " << cfg.name
                      << " (" << cfg.horizontal_nodes << "x" << cfg.vertical_nodes
                      << (cfg.type == 0 ? " bus" : " NoC") << "):"
                      << "  " << pm.runtime_dynamic << "W dynamic, "
                      << pm.total_leakage << "W leakage" << std::endl;
        }
    }

    std::cout << "\nSystem Totals:" << std::endl;
    std::cout << "  Total Dynamic Power: " << system_power_.total_dynamic << " W" << std::endl;
    std::cout << "  Total Leakage Power: " << system_power_.total_leakage << " W" << std::endl;
    std::cout << "  Total Power: " << system_power_.total_power << " W" << std::endl;
    std::cout << "  Peak Power: " << getPeakPower() << " W" << std::endl;
    std::cout << "  Total Area: " << getTotalArea() << " mm^2" << std::endl;
    /* 1.11.27: the PE's OWN area, reported separately. Total Area bundles the
     * NoC, MC, caches and memory die, so it cannot be compared against a
     * published per-compute-unit figure -- attempting exactly that against
     * Samsung's FIMDRAM PCU compared different quantities and produced a
     * meaningless 3.8x. The core area is what the DRAM-periphery factor
     * actually scales, and it is what a silicon anchor is quoted per. */
    std::cout << "    of which PE/core: "
              << getComponentArea(ComponentType::CORE) << " mm^2"
              << " (the quantity the periphery area factor scales)" << std::endl;
    /* 1.11.29: the LINK controller, broken out. It was never reported, so the
     * question "how much of a co-sim cell is the link?" could not be answered
     * without instrumenting -- and that question decides whether the McPAT
     * PCIe-2.0 SerDes constant is a results problem or a correctness tidy-up.
     * Reported only when a link is configured. */
    {
        const auto lpm = getComponentPower(ComponentType::PCIE);
        const double lp = lpm.total_power;
        const double la = getComponentArea(ComponentType::PCIE);
        if (lp > 0.0 || la > 0.0) {
            const double tp = system_power_.total_power;
            const double ta = getTotalArea();
            std::cout << "    of which link ctrl: " << la << " mm^2 ("
                      << (ta > 0.0 ? 100.0 * la / ta : 0.0) << "% of area), "
                      << lp << " W (" << (tp > 0.0 ? 100.0 * lp / tp : 0.0)
                      << "% of power)" << std::endl;
        }
    }
    std::cout << "=====================================\n" << std::endl;
}

/* 1.11.52 (audit C016/C007): the "warned fallback" that never warned. Three
 * blocks substitute unsourced fractions of the retired instruction count when
 * a run carried no measurement -- mispredicts 1%, loads/stores 20%/10%, and
 * the int/fp/mul mix 70/10/5% -- and all three were silent, so a run priced
 * on invented activity looked exactly like a measured one in the log.
 * Latched: say it once, naming what was missing and what stood in. */
void McPATWrapper::warnUnsourcedMix(const char* which, const char* frac) const {
    /* 1.11.60 (audit round 4, C006): through diagOnce(), so the pass that
     * consumes this latch is the one over the post-setter state. It used to be
     * consumed by initialize()'s pass, where meas_ld_/meas_st_ are necessarily
     * 0 because setMeasuredMix() has not been called yet -- which is why a run
     * with 132014 measured loads printed "no measured load/store mix". */
    if (!diagOnce(warned_unsourced_mix_)) return;
    std::cout << "  [Activity] WARNING: no measured " << which
              << " in this run; McPAT is driven by UNSOURCED fractions of the "
                 "retired instruction count (" << frac
              << "). These are stand-ins, not measurements -- the core dynamic "
                 "power below rests on them." << std::endl;
}

void McPATWrapper::printSummaryLine() const {
    if (!power_computed_) return;
    std::cout << "Power: " << std::fixed << std::setprecision(2)
              << system_power_.total_power << "W ("
              << system_power_.total_dynamic << "W dynamic + "
              << system_power_.total_leakage << "W leakage), Area: "
              << getTotalArea() << " mm^2"
              << std::defaultfloat << std::endl;
}

/* 1.11.56 (audit C032): see the declaration. L1 is folded into McPAT's core
 * and is extracted as a zero, so its answer never changes a sum; it is listed
 * for completeness and to keep the printer and the total on one predicate. */
bool McPATWrapper::componentIsDescribed(ComponentType t) const {
    switch (t) {
        case ComponentType::CORE:
            return config_.num_cores > 0;
        case ComponentType::L1_CACHE:
            return config_.l1i_size_bytes > 0 || config_.l1d_size_bytes > 0;
        case ComponentType::L2_CACHE:
            return config_.l2_size_bytes > 0;
        case ComponentType::L3_CACHE:
            return config_.l3_size_bytes > 0;
        case ComponentType::MEMORY_CONTROLLER:
            return config_.num_memory_controllers > 0;
        case ComponentType::NOC:
            return config_.has_noc || !noc_levels_.empty();
        case ComponentType::PCIE:
            return pcie_stats_.number_units > 0;
    }
    return true;
}

void McPATWrapper::printComponentBreakdown() const {
    std::cout << "\nComponent Power Breakdown:" << std::endl;

    auto print_component = [](const std::string& name, const PowerMetrics& power) {
        std::cout << "  " << name << ":" << std::endl;
        std::cout << "    Dynamic: " << power.runtime_dynamic << " W" << std::endl;
        std::cout << "    Leakage: " << power.total_leakage << " W" << std::endl;
        std::cout << "    Total: " << power.total_power << " W" << std::endl;
    };

    /* 1.11.29 (user ruling): report the system the USER DEFINED. The scope is
     * theirs -- host or no host, L3 or no L3, link or no link -- so a
     * component appears only when it was configured. NoC and PCIe were
     * already gated; cores, caches and MCs were not, so a device with no L3
     * printed "L3 Cache: 0 W" as though the model had priced one. Same
     * principle as the E8 rollup: never report a component nobody asked for. */
    /* 1.11.56 (audit C032): these gates were the ONLY place the ruling was
     * applied; the system total summed everything. Both now go through
     * componentIsDescribed(), so the breakdown adds up to the total printed
     * beside it. */
    if (componentIsDescribed(ComponentType::CORE))
        print_component("Cores", getComponentPower(ComponentType::CORE));
    if (componentIsDescribed(ComponentType::L1_CACHE))
        print_component("L1 Caches", getComponentPower(ComponentType::L1_CACHE));
    if (componentIsDescribed(ComponentType::L2_CACHE))
        print_component("L2 Caches", getComponentPower(ComponentType::L2_CACHE));
    if (componentIsDescribed(ComponentType::L3_CACHE))
        print_component("L3 Cache", getComponentPower(ComponentType::L3_CACHE));
    if (componentIsDescribed(ComponentType::MEMORY_CONTROLLER))
        print_component("Memory Controllers",
                        getComponentPower(ComponentType::MEMORY_CONTROLLER));
    if (componentIsDescribed(ComponentType::NOC)) {
        print_component("NoC", getComponentPower(ComponentType::NOC));
    }
    if (componentIsDescribed(ComponentType::PCIE)) {
        print_component("Link controller (" + pcie_stats_.link_type_name + ")",
                        getComponentPower(ComponentType::PCIE));
        std::cout << "    Area:    " << getComponentArea(ComponentType::PCIE)
                  << " mm^2" << std::endl;
    }
}

//=============================================================================
// XML Generation for McPAT
//=============================================================================

std::string McPATWrapper::generateXMLConfig() const {
    std::ostringstream xml;

    /* No instruction cache => the element is described with a resident
     * instruction store instead of a cache hierarchy.
     * 1.11.56 (audit C034): the name is now honest about what it selects.
     * `alu_only` reads like "this is an ALU element", but it is not the ALU
     * SWITCH -- that is `is_alu` (device_profile_ == DEVICE_ALU) below, which
     * selects the DATAPATH sizing. This one keys on l1i_size_bytes and
     * selects the INSTRUCTION SUPPLY description. The two agree on every
     * configuration the fleet ships, but they are independent, and one
     * reachable configuration separates them: cache.l1i.size_kb: 0 with a
     * non-ALU pe_type gives an element whose instruction supply is described
     * as a scratchpad while its register files, buffers and issue width come
     * from the in-order profile rather than from pe_lanes. That is a
     * defensible description of a real machine, so it is not forced into
     * agreement here -- but it is no longer silent (see the warning below). */
    const bool no_icache = (config_.l1i_size_bytes == 0);
    bool alu_only = no_icache;
    /* 1.11.56 (audit C036): the L2 must be gated on the L2's OWN size. This
     * keyed only on l1i, so a config with an instruction cache and
     * cache.l2.enabled: false (l2_size_bytes == 0, reachable from three
     * places in main.cpp) still emitted <param name="L2_config"
     * value="0,64,..."/>. That is not a harmless zero: sharedcache.cc sets
     * interface_ip.cache_sz = 0, array.cc clamps it to 64 against nbanks 8,
     * and io.cc rejects the geometry ("Cache size must >=64") -- the McPAT
     * child throws and the whole run dies with exit code 21. A cache the
     * user switched off is now simply not described, which is the same thing
     * the ALU path has always done. */
    const bool has_l2 = (config_.l2_size_bytes > 0);
    int num_l2s = (alu_only || !has_l2) ? 0 : config_.num_cores;
    bool has_l3 = !alu_only && (config_.l3_size_bytes > 0);
    int num_l3s = has_l3 ? 1 : 0;
    int num_cache_levels = alu_only ? 0 : (has_l3 ? 3 : (has_l2 ? 2 : 1));

    // Determine number of NoC instances
    // McPAT XML parser uses positional component counting; with 0 NoCs + 0 L2s +
    // 0 L3s the counter never advances past core0, causing MC parse to fail.
    // Always emit at least 1 NoC (with minimal activity) to keep parsing correct.
    int num_nocs = 1;
    if (!noc_levels_.empty()) {
        num_nocs = static_cast<int>(noc_levels_.size());
    } else if (config_.has_noc) {
        num_nocs = 1;
    }

    // Device profile settings
    /* 1.9.37: an out-of-order PROCESSING ELEMENT is out-of-order too.
     * is_ooo gates every speculative structure in the generated description --
     * machine type, reorder buffer, instruction window, renaming, physical
     * register count, load/store ordering, FP issue width, and the reorder and
     * rename activity statistics. Testing only for the host profile meant a
     * device element declared out-of-order was described with NO reorder
     * buffer, NO instruction window and NO renaming, while the timing model
     * simulated all of it. Two halves of one model describing different
     * machines -- the recurring failure of this release train. */
    bool is_ooo = (device_profile_ == DeviceProfile::OOO);
    /* 1.9.36: DEVICE_ALU is no longer emitted as an in-order core.
     *
     * McPAT offers exactly two core models, OOO and Inorder
     * (basic_components.h:88), and both describe a PROCESSOR -- fetch, decode,
     * control. A processing element here is a DATAPATH. Until now the generator
     * branched only on is_ooo, so an ALU element and an in-order core produced
     * BYTE-IDENTICAL input and the element was charged for a branch predictor,
     * caches, address translation and a scheduler it does not have.
     *
     * This does not invent a third McPAT core type -- it cannot -- but it stops
     * describing the element as something it is not, and makes the description
     * parametric from pe_lanes / pe_element_bits / pe_has_fp / pe_imem_bytes.
     *
     * WHAT THIS DOES NOT FIX, and it is the larger term: McPAT's undifferentiated
     * core is a regression on pipeline depth fitted to 90 nm commercial parts
     * (logic.cc:752+; the in-order branch -2.19*log(d)+6.55 crosses zero at 19.9
     * stages). It accounted for roughly 99% of measured element core power at
     * every profile, because a shallow datapath pipeline is evaluated off the
     * low end of that fit where it is largest. Sizing the structures correctly
     * addresses the other ~1%. Removing the lump needs the element composed from
     * McPAT primitives (ArrayST, FunctionalUnit, interconnect), which inherit no
     * undifferentiated term. Recorded in the release notes rather than hidden. */
    const bool is_alu = (device_profile_ == DeviceProfile::DEVICE_ALU);
    /* 1.11.56 (audit C034): the two switches for one concept, said out loud.
     * `no_icache` (above) decides the INSTRUCTION SUPPLY -- resident imem and
     * no caches -- and `is_alu` decides the DATAPATH -- register files,
     * buffers, result buses and issue width sized from pe_lanes. They are set
     * from different inputs (cache.l1i.size_kb vs pe_type) and nothing
     * reconciles them, so a config can describe one machine's front end
     * beside another machine's back end. Only one direction is reachable
     * (l1i 0 on a non-ALU profile; the ALU profile forces all four cache
     * sizes to 0), and it is a legitimate machine, so it is described rather
     * than refused -- but it is stated, once, so nobody reads the resulting
     * XML as an ALU element's. */
    /* 1.11.60 (audit round 4, C006): diagOnce() -- see the gate's declaration.
     * This one described a real configuration either way (both switches are
     * set before initialize()), but it shared the latch discipline that made
     * its neighbours lie, so it is moved onto the same gate. */
    if ((no_icache != is_alu) && diagOnce(warned_profile_split_)) {
        std::cerr << "[McPATWrapper] NOTE: instruction supply and datapath are "
                     "described from different switches in this configuration -- "
                     "l1i_size_bytes=" << config_.l1i_size_bytes
                  << " selects a " << (no_icache ? "resident instruction store, "
                                                   "no caches"
                                                 : "cache hierarchy")
                  << ", while the device profile selects the "
                  << (is_alu ? "ALU datapath (sized from pe_lanes)"
                             : "in-order/OOO datapath (register files, buffers "
                               "and issue width from the core profile, NOT from "
                               "pe_lanes)")
                  << ". Both halves are emitted as configured." << std::endl;
    }
    const int  lanes  = (config_.pe_lanes > 0) ? config_.pe_lanes : 1;

    int machine_type = is_ooo ? 0 : 1;
    int x86 = is_ooo ? 1 : 0;
    int rob_size = is_ooo ? 192 : 0;
    int inst_window_size = is_ooo ? 64 : 0;
    int fp_inst_window_size = is_ooo ? 32 : 0;
    int phy_regs_irf = is_ooo ? 180 : 32;
    int phy_regs_frf = is_ooo ? 180 : 32;
    int rename_scheme = is_ooo ? 0 : 0;  // RAT-based for both
    const char* lsu_order = is_ooo ? "OOO" : "inorder";
    int pipeline_depth = is_ooo ? 19 : config_.pipeline_depth;
    int issue_width = is_ooo ? 4 : config_.issue_width;
    // FP issue width: McPAT never defaults fp_issue_width, so it MUST be emitted
    // for the OOO profile or the FPIssueQueue array is built with zero ports and
    // CACTI aborts ("Must have at least one port", exit 21). 2 matches McPAT's
    // own 4-wide x86 OOO references (Xeon, Penryn). Emitted only for is_ooo so
    // the inorder/ALU device XML stays byte-identical (those profiles never
    // build the FP issue queue, so they neither need nor previously emitted it).
    int fp_issue_width = is_ooo ? 2 : 0;
    int store_buffer = is_ooo ? 32 : 4;
    int load_buffer = is_ooo ? 32 : 4;
    int num_alus = config_.num_alus;
    int num_muls = config_.num_muls;
    int num_fpus = config_.num_fpus;
    if (is_alu) {
        /* A datapath: a register file, arithmetic units, a result bus and an
         * instruction store. No speculation, no dynamic scheduling, no caches.
         * Register file scales with lanes -- for a wide element the file and the
         * result bus dominate, which is why real parts share one compute unit
         * between banks rather than widening indefinitely. */
        phy_regs_irf   = 8 * lanes;
        const bool has_fpu_here = config_.pe_has_fp && (config_.num_fpus != 0);
        phy_regs_frf   = has_fpu_here ? (8 * lanes) : 1;  // 0 aborts CACTI
        issue_width    = lanes;
        /* 1.11.57 (latent C029): `fp_issue_width = has_fpu_here ? lanes : 0;`
         * used to sit here and is DELETED rather than emitted.
         *
         * What was wrong: it computed a value that had no exit. The parameter
         * is written into the XML only under `if (is_ooo)` a few hundred
         * lines below, and this whole block is the is_alu branch, where
         * is_ooo is false by construction. So the assignment ran, produced
         * `lanes`, and the variable was then re-read only by an emission that
         * cannot happen -- dead work wearing the shape of configuration.
         *
         * Why it was invisible: the value could not reach McPAT, so no number
         * moved. The trap was for the next reader: a live-looking assignment
         * next to issue_width invites "we compute this, why not emit it", and
         * emitting it would NOT be neutral. ParseXML would set
         * fp_issue_width, core.cc:4313-4314 would copy it into
         * coredynp.fp_issueW and fp_decodeW, and those size the FP issue
         * queue and the FP rename structures -- which the in-order machine
         * type does not build. The profiles that skip the emission are
         * exactly the profiles that have nothing to size with it.
         *
         * Deleted, not emitted, for that reason. fp_issue_width keeps its
         * is_ooo initialiser of 0 through this branch, so the generated XML
         * is byte-identical. The element's FP capability is already carried
         * where it belongs: num_fpus and phy_regs_frf, both set just below
         * from the same has_fpu_here. */
        store_buffer   = 1;      // request issue only; no queueing, no cache
        load_buffer    = 1;
        lsu_order      = "inorder";
        /* 1.9.32: arithmetic composed from the datapath instead of inherited.
         * The element previously declared one integer ALU, no multiplier and NO
         * FLOATING-POINT UNIT AT ALL -- while the timing model ran stream_triad,
         * gemv and stencil_2d on it, every one of them FP32. The two halves were
         * describing different machines again: the timing side retired
         * single-cycle floating-point operations on hardware the power side said
         * did not exist, so their energy was never charged.
         *
         * One of each unit per lane is what makes a lane a lane: a lane that
         * cannot multiply or cannot do floating point stalls on the kernels we
         * run, and the timing model charges no such stall. The multiplier is not
         * optional for the same reason -- gemv and stencil are multiply-
         * accumulate in their inner loop.
         *
         * pe_has_fp exists because an integer-only element is a real design
         * point (histogram and bfs are INT32 throughout); it is a configuration
         * choice, not a default. */
        num_alus = lanes;
        num_muls = lanes;
        /* 1.11.53 (audit C031, completing it): the ALU branch OVERWRITES the
         * FPU count, so a zero the caller already resolved was discarded and
         * re-derived from pe_has_fp. Those two flags are set from different
         * places -- main.cpp zeroes num_fpus from the NODE's floating_point
         * while pe_has_fp arrives from the pim.pe block -- so an FPU-less
         * element could emit `lanes` FPUs here while the soft-float fold
         * (which gates on config_.num_fpus == 0) ALSO charged its FP ops as
         * integer work: energy for hardware the element does not have, twice.
         * An explicit zero from the caller now wins; pe_has_fp only decides
         * the count when the caller left it unresolved. */
        const bool caller_removed_fpu = (config_.num_fpus == 0);
        num_fpus = (config_.pe_has_fp && !caller_removed_fpu) ? lanes : 0;
    }

    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml << "<component id=\"root\" name=\"root\">\n";

    // System parameters
    xml << "  <component id=\"system\" name=\"system\">\n";
    xml << "    <param name=\"number_of_cores\" value=\"" << config_.num_cores << "\"/>\n";
    xml << "    <param name=\"number_of_L1Directories\" value=\"0\"/>\n";
    xml << "    <param name=\"number_of_L2Directories\" value=\"0\"/>\n";
    xml << "    <param name=\"number_of_L2s\" value=\"" << num_l2s << "\"/>\n";
    /* 1.11.56 (audit C036): Private_L2 must follow num_l2s, not alu_only --
     * processor.cc:74 errors out when Private_L2 is set and numCore !=
     * numL2, which is exactly the state an l1i-present/L2-absent config
     * reached once num_l2s stopped being num_cores. */
    xml << "    <param name=\"Private_L2\" value=\"" << (num_l2s > 0 ? 1 : 0) << "\"/>\n";
    xml << "    <param name=\"number_of_L3s\" value=\"" << num_l3s << "\"/>\n";
    xml << "    <param name=\"number_of_NoCs\" value=\"" << num_nocs << "\"/>\n";
    xml << "    <param name=\"homogeneous_cores\" value=\"1\"/>\n";
    xml << "    <param name=\"homogeneous_L2s\" value=\"" << (num_l2s > 0 ? 1 : 0) << "\"/>\n";
    xml << "    <param name=\"homogeneous_L1Directories\" value=\"0\"/>\n";
    xml << "    <param name=\"homogeneous_L2Directories\" value=\"0\"/>\n";
    xml << "    <param name=\"homogeneous_L3s\" value=\"" << (num_l3s > 0 ? 1 : 0) << "\"/>\n";
    xml << "    <param name=\"homogeneous_ccs\" value=\"1\"/>\n";
    xml << "    <param name=\"homogeneous_NoCs\" value=\"" << (num_nocs == 1 ? 1 : 0) << "\"/>\n";
    xml << "    <param name=\"core_tech_node\" value=\"" << config_.tech_node_nm << "\"/>\n";
    xml << "    <param name=\"target_core_clockrate\" value=\"" << static_cast<int>(config_.core_clock_mhz) << "\"/>\n";
    xml << "    <param name=\"temperature\" value=\"" << config_.temperature_k << "\"/>\n";
    xml << "    <param name=\"number_cache_levels\" value=\"" << num_cache_levels << "\"/>\n";
    xml << "    <param name=\"interconnect_projection_type\" value=\"" << config_.interconnect_projection_type << "\"/>\n";
    /* 1.11.3 gate-0 verdict: device_type=4 (comm-dram) as a GLOBAL device is
     * rejected by CACTI itself -- UCA asserts power.readOp.dynamic > 0 because
     * the retention-grade device (Vth 1.0 > Vdd 0.9) only functions inside the
     * DRAM-array machinery where wordline boost exists. General logic cannot
     * be priced in that device by these tools; the factor harness below is
     * therefore the mechanism, not an interim. */
    xml << "    <param name=\"device_type\" value=\"" << config_.device_type << "\"/>\n";
    xml << "    <param name=\"longer_channel_device\" value=\"" << config_.longer_channel_device << "\"/>\n";
    /* 1.9.32: reference class, emitted explicitly instead of inherited.
     * McPAT defaults Embedded to false, i.e. "this die belongs to the server
     * population". Nothing ever overrode it, so a processing element was priced
     * against Niagara/Merom/Opteron die measurements: a five-stage element drew
     * an undifferentiated-core term of about three square millimetres at the
     * fit's 90 nm base, which is larger than everything the description
     * actually names by orders of magnitude and which supplied nearly all of
     * the element's power -- all of it leakage, since that term has no dynamic
     * component at all.
     *
     * Device scope now selects the embedded population. That is not a discount
     * applied to make a number smaller; it is the other of the two populations
     * McPAT was calibrated against, and it is the one a memory-die element
     * belongs to. It changes three things together, which is why it has to be
     * one flag rather than three edits: the undifferentiated term switches to
     * Sandia's parametrized-processor fit, the functional units switch to ARM
     * areas and per-access energies, and the wires switch from top-level global
     * to the local/semi-global stack an on-die element would actually use.
     *
     * Host scope stays on the server population, because the host is a server
     * part. */
    xml << "    <param name=\"Embedded\" value=\"" << (config_.device_scope ? 1 : 0) << "\"/>\n";
    /* Only ever read inside the embedded undifferentiated-core term, where it
     * decides whether that term exists at all. Kept at 1 so the element still
     * carries clocking and control overhead; 0 would zero it, which is a
     * cheaper answer than the truth. */
    xml << "    <param name=\"opt_clockrate\" value=\"1\"/>\n";
    /* 1.11.51 (N1): the SUBARRAY pitch penalty, emitted for EVERY family --
     * geometry does not care which process the transistors came from. The
     * family block below still emits dram_periph_pitch for the family-1
     * transform; family-0 (SRAM/NVM placements) reads this one. */
    xml << "    <param name=\"core_pitch_factor\" value=\""
        << ((config_.subarray_pitch_factor > 0.0)
                ? config_.subarray_pitch_factor : 1.0) << "\"/>\n";
    /* 1.11.12 (borders rule): the DRAM-periphery family is DESCRIBED to
     * McPAT, which owns the components and applies it internally; the
     * wrapper no longer post-scales results. Scope says which components sit
     * on the memory die: PE cores + their caches + the on-die fabric + the
     * element controllers (subarray..chip placements). Rank/channel and
     * base-die designs keep their logic on a buffer die and stay family 0. */
    if (config_.process_family == 1) {
        double fa, fd, fl;
        /* 1.11.51 (L105): the APPLIED factors now come from the same four
         * inputs main.cpp prints -- logic node, corner baseline device and
         * temperature included -- instead of a table_nm/hp/350K shortcut
         * that silently disagreed with the printed derivation whenever the
         * logic node, corner or temperature was not the default. */
        double fg = 1.0;   // 1.11.54 (E004): gate-leakage ratio, emitted below
        periphFamilyFactorsCfg(config_.dram_periph_table_nm,
                               config_.tech_node_nm, config_.device_type,
                               config_.temperature_k, fa, fd, fl, fg);
        double pitch = (config_.subarray_pitch_factor > 0.0)
                           ? config_.subarray_pitch_factor : 1.0;
        xml << "    <param name=\"dram_periph_family\" value=\"1\"/>\n";
        /* 1.11.34 (E11): fa and PITCH are emitted SEPARATELY. They were
         * multiplied into one scalar, so wherever the area factor went the
         * pitch penalty went too -- onto the caches and the memory controller.
         * Those are OFF-PITCH circuits: per Vogelsang (MICRO 2010) the on-pitch
         * circuitry is the sense-amp stripes and local wordline drivers, laid
         * out on the array's bitline pitch, while a cache or MC sits further
         * out and is limited by wiring. Only the PE core abuts the array. */
        xml << "    <param name=\"dram_periph_area\" value=\"" << fa << "\"/>\n";
        xml << "    <param name=\"dram_periph_pitch\" value=\"" << pitch << "\"/>\n";
        xml << "    <param name=\"dram_periph_dyn\" value=\"" << fd << "\"/>\n";
        xml << "    <param name=\"dram_periph_leak\" value=\"" << fl << "\"/>\n";
        /* 1.11.54 (audit E004): gate leakage is oxide tunnelling and gets its
         * OWN ratio from the I_g_on_n rows, instead of borrowing the
         * subthreshold ratio above. */
        xml << "    <param name=\"dram_periph_gate_leak\" value=\"" << fg << "\"/>\n";
        if (fg == fl)
            std::cout << "  [tech] gate-leakage ratio UNSOURCED: CACTI has no "
                         "I_g_on_n data for the DRAM device columns (0 at 22, "
                         "32 and 45 nm), so the subthreshold ratio " << fl
                      << " stands in -- same direction (high-Vt AND thick-oxide"
                         " both collapse leakage), unsourced magnitude."
                      << std::endl;
        /* 1.11.34 (E10): the scope mask is GONE. It was emitted as the
         * literal 15 -- every bit, always -- with no configuration surface, so
         * it had one reachable value and its bit structure implied a
         * selectability that did not exist.
         * The LINK CONTROLLER stays untransformed for a stated reason rather
         * than a missing mask bit: our placement ladder maps LOGIC_DIE (the
         * HBM base die) to family 0, and an HBM device reaches its host
         * THROUGH that base die; for DDR-class parts the controller is
         * host-side. Either way it is logic silicon.
         *
         * 1.11.50 (L80, closing the audit finding): the same reason covers
         * the remaining family, stated per class rather than left implied.
         * A channel-centric LPDDR/GDDR PIM module reaches the host link
         * through an interface/buffer chip (there is no host-socket LPDDR
         * or GDDR link; the SerDes endpoint is standard-logic SerDes IP
         * either way) -- so the device end of the co-sim link is logic
         * silicon at EVERY family placement, and adding it to the rebuilt
         * totals untransformed is the correct pricing, not an omission. */
    }
    /* 1.11.8: sys.power_gating enables McPAT/CACTI's sleep-transistor
     * model so per-component power_gated_leakage endpoints are computed.
     * Emitted only when the described design gates SOMETHING -- all-false
     * keeps the XML byte-identical to 1.11.7. */
    xml << "    <param name=\"power_gating\" value=\""
        << ((pg_spec_.pg_core || pg_spec_.pg_noc || pg_spec_.pg_mc) ? 1 : 0)
        << "\"/>\n";
    /* 1.9.32: datapath width. McPAT reads machine_bits ONLY to size the
     * datapath -- the integer and floating-point register widths, the load/store
     * queue entries, and the integer, multiply and floating-point bypass buses.
     * It has nothing to do with the address widths, which are emitted
     * separately below.
     *
     * A 64 was emitted for every scope, so a 32-bit processing element carried
     * 64-bit registers and 64-bit result buses. Device scope now states the
     * element's own width; host scope stays 64, which the host is. */
    int machine_bits = config_.device_scope ? config_.pe_element_bits : 64;
    /* McPAT quantises the datapath to 32-bit granularity (it computes
     * ceil(bits/32)*32 internally), so a narrower element -- an 8- or 16-bit
     * bit-serial datapath, which is a real in-memory design point the TIMING
     * model already supports via operand_width -- is priced as 32-bit. Say so
     * once rather than let the two halves quietly disagree about how wide the
     * element is; refusing would remove a capability the timing side has. */
    /* 1.11.60 (audit round 4, C006): diagOnce(). */
    if (config_.device_scope && machine_bits < 32 &&
        diagOnce(warned_narrow_datapath_)) {
        std::cerr << "[power] WARNING: operand_width=" << machine_bits
                  << " is narrower than the power model's 32-bit granularity; "
                  << "the datapath is priced as 32-bit while the timing model "
                  << "charges " << machine_bits << " bits. Register files, "
                  << "queues and result buses are therefore OVERSTATED for this "
                  << "element.\n";
    }
    xml << "    <param name=\"machine_bits\" value=\"" << machine_bits << "\"/>\n";
    xml << "    <param name=\"virtual_address_width\" value=\"48\"/>\n";
    xml << "    <param name=\"physical_address_width\" value=\"48\"/>\n";
    xml << "    <param name=\"virtual_memory_page_size\" value=\"4096\"/>\n";

    /* 1.11.57 (latent C035): IDLE CYCLES, COMPUTED ONCE AND UNABLE TO WRAP.
     *
     * WHAT WAS WRONG. Both emission sites (here and the per-core stat block
     * below) wrote `total_cycles_ - busy_cycles_` directly. Both operands are
     * uint64_t, they have independent setters -- setTotalCycles() and
     * setBusyCycles() -- and nothing in this class or its callers enforces an
     * ordering between them. busy > total is therefore representable, and the
     * subtraction does not go negative in unsigned arithmetic: it wraps. A
     * one-cycle inversion emits 18446744073709551615 idle cycles; a
     * ten-thousand-cycle one emits 18446744073709541616. Downstream of that,
     * McPAT's XML_Parse reads the per-core stat with atof() into
     * sys.core[i].idle_cycles and core.cc copies it to coredynp.idle_cycles
     * (:4348) -- in this fork nothing then reads it, and the SYSTEM-level
     * idle_cycles this line emits is not parsed at all. So the wrap would
     * today land in the archived XML rather than in a power number: an input
     * record claiming 1.8e19 idle cycles on a run of a few million, which
     * discredits the archive and would become a real power error the moment
     * any consumer -- a newer McPAT, or a PIMID-side reader -- started using
     * the stat. That is exactly the inheritance this fix prevents.
     *
     * WHY IT WAS INVISIBLE. Every reachable caller in main.cpp passes the
     * SAME value to both setters, so the difference is exactly 0 in every run
     * this tree has produced. The one asymmetric caller lives in
     * power_model_manager.cpp, which sets total cycles and never sets busy
     * cycles at all, and that class is never instantiated. The defect becomes
     * live the moment a caller aggregates busy cycles across cores while
     * reporting wall-clock cycles -- the natural thing to do for a multi-PE
     * device -- because then busy legitimately exceeds total.
     *
     * THE FIX clamps at zero and SAYS SO once, rather than clamping quietly:
     * busy > total is not a rounding artefact, it means the two counters were
     * measured against different populations or different clocks, and the
     * caller needs to know that its inputs disagree. Zero is the right
     * clamped value -- a fully busy core has no idle cycles -- but it is a
     * consequence of the inconsistency, not a measurement of it. */
    uint64_t idle_cycles = 0;
    if (total_cycles_ >= busy_cycles_) {
        idle_cycles = total_cycles_ - busy_cycles_;
    /* 1.11.60 (audit round 4, C006): diagOnce(). This one could NEVER have
     * fired on real counters: at initialize() both busy_cycles_ and
     * total_cycles_ are 0, so the comparison took the equal branch and latched
     * nothing, and by the time setTotalCycles()/setBusyCycles() had run the
     * only remaining evaluation was the child's silent one. The guard is a
     * guard again. */
    } else if (diagOnce(warned_busy_exceeds_total_)) {
        std::cerr << "[power] WARNING: busy_cycles (" << busy_cycles_
                  << ") exceeds total_cycles (" << total_cycles_
                  << "). idle_cycles is emitted as 0; the unclamped unsigned "
                     "subtraction would have wrapped to "
                  << (total_cycles_ - busy_cycles_)
                  << ". The two counters disagree -- most likely one is a "
                     "per-core sum and the other a wall-clock count -- and no "
                     "idle figure derived from them is meaningful until that "
                     "is reconciled." << std::endl;
    }

    // System statistics
    xml << "    <stat name=\"total_cycles\" value=\"" << total_cycles_ << "\"/>\n";
    xml << "    <stat name=\"idle_cycles\" value=\"" << idle_cycles << "\"/>\n";
    xml << "    <stat name=\"busy_cycles\" value=\"" << busy_cycles_ << "\"/>\n";

    // Core component -- homogeneous_cores=1 means emit exactly ONE core template
    for (int i = 0; i < 1; i++) {
        xml << "    <component id=\"system.core" << i << "\" name=\"core" << i << "\">\n";
        xml << "      <param name=\"clock_rate\" value=\"" << static_cast<int>(config_.core_clock_mhz) << "\"/>\n";
        xml << "      <param name=\"vdd\" value=\"0\"/>\n";
        xml << "      <param name=\"power_gating_vcc\" value=\"-1\"/>\n";
        xml << "      <param name=\"opt_local\" value=\"0\"/>\n";
        xml << "      <param name=\"instruction_length\" value=\"32\"/>\n";
        xml << "      <param name=\"opcode_width\" value=\"7\"/>\n";
        xml << "      <param name=\"x86\" value=\"" << x86 << "\"/>\n";
        xml << "      <param name=\"micro_opcode_width\" value=\"8\"/>\n";
        xml << "      <param name=\"machine_type\" value=\"" << machine_type << "\"/>\n";
        xml << "      <param name=\"number_hardware_threads\" value=\"" << config_.number_hardware_threads << "\"/>\n";
        xml << "      <param name=\"fetch_width\" value=\"" << issue_width << "\"/>\n";
        xml << "      <param name=\"number_instruction_fetch_ports\" value=\"1\"/>\n";
        xml << "      <param name=\"decode_width\" value=\"" << issue_width << "\"/>\n";
        xml << "      <param name=\"issue_width\" value=\"" << issue_width << "\"/>\n";
        xml << "      <param name=\"peak_issue_width\" value=\"" << issue_width << "\"/>\n";
        if (is_ooo) {
            xml << "      <param name=\"fp_issue_width\" value=\"" << fp_issue_width << "\"/>\n";
        }
        xml << "      <param name=\"commit_width\" value=\"" << issue_width << "\"/>\n";
        xml << "      <param name=\"pipelines_per_core\" value=\"1,1\"/>\n";
        xml << "      <param name=\"pipeline_depth\" value=\"" << pipeline_depth << ","
            << pipeline_depth << "\"/>\n";
        xml << "      <param name=\"ALU_per_core\" value=\"" << num_alus << "\"/>\n";
        xml << "      <param name=\"MUL_per_core\" value=\"" << num_muls << "\"/>\n";
        xml << "      <param name=\"FPU_per_core\" value=\"" << num_fpus << "\"/>\n";
        xml << "      <param name=\"instruction_buffer_size\" value=\"32\"/>\n";
        xml << "      <param name=\"decoded_stream_buffer_size\" value=\"16\"/>\n";
        xml << "      <param name=\"instruction_window_scheme\" value=\"0\"/>\n";
        xml << "      <param name=\"instruction_window_size\" value=\"" << inst_window_size << "\"/>\n";
        xml << "      <param name=\"fp_instruction_window_size\" value=\"" << fp_inst_window_size << "\"/>\n";
        xml << "      <param name=\"ROB_size\" value=\"" << rob_size << "\"/>\n";
        xml << "      <param name=\"archi_Regs_IRF_size\" value=\"32\"/>\n";
        xml << "      <param name=\"archi_Regs_FRF_size\" value=\"32\"/>\n";
        xml << "      <param name=\"phy_Regs_IRF_size\" value=\"" << phy_regs_irf << "\"/>\n";
        xml << "      <param name=\"phy_Regs_FRF_size\" value=\"" << phy_regs_frf << "\"/>\n";
        xml << "      <param name=\"rename_scheme\" value=\"" << rename_scheme << "\"/>\n";
        xml << "      <param name=\"register_windows_size\" value=\"0\"/>\n";
        xml << "      <param name=\"LSU_order\" value=\"" << lsu_order << "\"/>\n";
        xml << "      <param name=\"store_buffer_size\" value=\"" << store_buffer << "\"/>\n";
        xml << "      <param name=\"load_buffer_size\" value=\"" << load_buffer << "\"/>\n";
        xml << "      <param name=\"memory_ports\" value=\"1\"/>\n";
        xml << "      <param name=\"RAS_size\" value=\"16\"/>\n";

        // Core statistics
        uint64_t inst_per_core = total_instructions_ / std::max(1, config_.num_cores);
        double pipeline_duty_cycle = (total_cycles_ > 0)
            ? static_cast<double>(busy_cycles_) / total_cycles_
            : 0.0;

        xml << "      <stat name=\"total_instructions\" value=\"" << inst_per_core << "\"/>\n";
        /* 1.9.28: prefer MEASURED activity.
         *
         * 1.9.33: the branch count is now the simulator's OWN branch counter,
         * not the basic-block count. 1.9.28 used bbls on the reasoning that a
         * BBL terminates at a control transfer -- true for a real
         * multi-instruction basic block, but on this QEMU-fed decode path a BBL
         * averages ~1.09 instructions, so bbls ~= instrs and the "measured"
         * branch rate came out at 91.5%. The in-order core always exported a
         * real `branches` counter; the OOO core ran a predictor but exported
         * only its MISSES, so 1.9.33 added the total there too. Both cores now
         * report it, and both report it ROI-windowed like instrs.
         * The integer/floating split is still an assumption (zsim does not
         * classify retired ops), but it is now applied to the NON-branch
         * remainder instead of to the whole stream, so a branch-heavy workload
         * is no longer forced to the same mix as a floating-point one.
         *
         * 1.9.29 CORRECTION -- 1.9.28 SHIPPED THIS WITHOUT CHECKING THE BASES.
         * The activity counters and the instruction count are NOT on the same
         * base. Measured on a co-simulation dump: instrs 52,093 against uops
         * 4,247,062, bbls 649,332 and branches 580,744 -- every activity counter
         * is 10-80x the instruction count it is supposed to be a fraction of.
         * The consequence was a degenerate mix: the co-simulation host was
         * described to the power model as 91.5% branches, and the device-scope
         * path (added in 1.9.29) hit the clamp below and was described as 100%
         * branches with zero integer and zero floating-point operations. That is
         * worse than the fixed fractions it replaced -- 1.9.28 exchanged an
         * invented-but-plausible mix for a measured-but-impossible one, and the
         * A/B did not catch it because a degenerate mix barely moves the total.
         *
         * WHY THIS IS A GATE AND NOT A CORRECTION: scaling counters on
         * different bases by a factor chosen to make the ratio look right
         * would be exactly the class of invented constant this release train
         * exists to remove. So the measured set is used ONLY when it is
         * self-consistent, and otherwise the stand-in fractions are used and
         * the output says so.
         *
         * 1.11.42 STATUS (audit E22 closure): the base mismatch described
         * above is HISTORICAL -- it does not reproduce on current binaries.
         * The per-node activity split gives each node counters on its own
         * base (co-sim host measures instrs=10 against uops=11, consistent),
         * and across 75 recent gate-run logs the rejection path is never
         * taken: every run prints "instruction mix COUNTED". Both co-sim
         * nodes count from their own real decoded traces (device:
         * 3830+1707+1147 per core x 8 = 53,472 of 53,479 retired). The
         * fractions below survive only as a warned last resort for a future
         * counter regression -- and they are UNSOURCED stand-ins, not
         * "documented": the only documentation was PIMID citing itself, and
         * changelog 1.11.10 already measured the 87.5%-integer assumption
         * wrong on FP workloads. */
        const bool meas_self_consistent =
            (meas_branches_ > 0) &&
            /* branches cannot exceed instructions retired */
            (meas_branches_ / std::max(1, config_.num_cores) <= inst_per_core) &&
            /* every instruction decodes to at least one micro-op */
            (meas_uops_ == 0 ||
             meas_uops_ / std::max(1, config_.num_cores) >= inst_per_core);

        uint64_t br_per_core = meas_self_consistent
            ? meas_branches_ / std::max(1, config_.num_cores)
            : inst_per_core * 10 / 100;   // UNSOURCED stand-in (see E22 note)
        /* 1.11.60 (audit round 4, C006): diagOnce(). meas_branches_ is 0 at
         * initialize(), so this condition was false on the only pass that
         * could speak. */
        if (!meas_self_consistent && meas_branches_ > 0 &&
            diagOnce(warned_mix_)) {
            std::cout << "  [Activity] WARNING: measured instruction mix rejected as "
                         "inconsistent (instrs/core=" << inst_per_core
                      << " branches/core=" << (meas_branches_ / std::max(1, config_.num_cores))
                      << " uops/core=" << (meas_uops_ / std::max(1, config_.num_cores))
                      << "); using documented fractions instead. The counters are "
                         "on different bases -- see mcpat_wrapper.cpp." << std::endl;
        }
        if (br_per_core > inst_per_core) br_per_core = inst_per_core;
        uint64_t nonbr = inst_per_core - br_per_core;
        uint64_t int_per_core = nonbr * 875 / 1000;   // 87.5% of non-branch
        uint64_t fp_per_core  = nonbr - int_per_core;
        /* 1.11.10 (#112): use the COUNTED mix when the decoder produced one.
         * The 87.5/12.5 split above was a documented stand-in for exactly this
         * measurement; it stays as the fallback for core models that never
         * decode. int carries mul/div (McPAT has no separate integer-multiply
         * stat at this level; the FU it drives is the same execution unit). */
        const uint64_t ncores = std::max(1, config_.num_cores);
        bool mix_measured = (meas_int_ + meas_fp_ + meas_mul_) > 0;
        if (mix_measured) {
            uint64_t mi = (meas_int_ + meas_mul_) / ncores;
            uint64_t mf = meas_fp_ / ncores;
            /* 1.11.15 (audit): when the census is measured, its BRANCH class
             * replaces the core counter -- the decoder classifies ALL control
             * transfers (call/ret/jmp/indirect) while the core counter is
             * conditional-only, and the difference was silently priced at
             * zero execution energy. With census branches the classes sum to
             * the census total by construction, so the check becomes
             * SYMMETRIC: any deficit against retired instructions is printed,
             * and a deficit above 5% rejects the measurement.
             *
             * 1.11.16 (verification audit): the value logic is now independent
             * of the print-once latch, and every fallback is a REAL fallback.
             * The 1.11.15 shape had three defects: (1) the reject branch
             * zeroed mi/mf and then flowed into the assignment below -- the
             * XML carried a 0-int/0-fp/100%-branch mix, the exact 1.9.28
             * degeneracy this guard exists to prevent; (2) the <=5% residual
             * correction sat INSIDE the !warned_mix_ guard, so the number
             * depended on whether a message had been printed (and the child's
             * regenerated XML could differ from the parent's); (3) fallbacks
             * kept int/fp from the PRE-census nonbr while branch kept the
             * census value, so the classes no longer summed to retirement. */
            uint64_t pre_br = br_per_core, pre_int = int_per_core, pre_fp = fp_per_core;
            if (meas_mix_br_ > 0) {
                br_per_core = meas_mix_br_ / ncores;
                if (br_per_core > inst_per_core) br_per_core = inst_per_core;
                nonbr = inst_per_core - br_per_core;
                int_per_core = nonbr * 875 / 1000;   // fraction fallback, census-branch base
                fp_per_core  = nonbr - int_per_core;
            }
            bool census_ok = true;
            uint64_t classified = mi + mf + br_per_core;
            if (classified < inst_per_core) {
                uint64_t deficit = inst_per_core - classified;
                if (deficit * 20 > inst_per_core) {   // >5%: reject the census
                    census_ok = false;
                    if (diagOnce(warned_mix_)) {   // 1.11.60 (C006)
                        std::cout << "  [Activity] measured mix rejected: "
                                  << deficit << "/" << inst_per_core
                                  << " retired instructions per core are in NO "
                                     "class (>5%) -- census and retirement "
                                     "disagree; using documented fractions."
                                  << std::endl;
                    }
                } else if (deficit > 0) {
                    mi += deficit;    // conservative: residual as int-class
                    if (diagOnce(warned_mix_)) {   // 1.11.60 (C006)
                        std::cout << "  [Activity] mix census covers "
                                  << classified << "/" << inst_per_core
                                  << " per core (deficit " << deficit
                                  << ", <5%); residual priced as integer."
                                  << std::endl;
                    }
                }
            }
            if (!census_ok) {
                /* Full fraction fallback: the BRANCH class reverts too -- a
                 * census that disagrees with retirement by >5% is not trusted
                 * for any of its classes. */
                br_per_core  = pre_br;
                nonbr        = inst_per_core - br_per_core;
                int_per_core = pre_int;
                fp_per_core  = pre_fp;
            } else if (mi + mf > nonbr) {
                /* More classified instructions than retired ones means the two
                 * counters are on different bases -- the 1.9.28/1.11.9 defect
                 * class. Say so and keep the fractions (recomputed above on
                 * the census-branch base, so int+fp+branch still sums). */
                if (diagOnce(warned_mix_)) {   // 1.11.60 (C006)
                    std::cout << "  [Activity] measured mix (" << mi << " int+mul, "
                              << mf << " fp per core) exceeds retired non-branch "
                              << nonbr << " -- bases disagree, using fractions"
                              << std::endl;
                }
            } else {
                int_per_core = mi;
                fp_per_core  = mf;
                if (diagOnce(warned_mix_)) {   // 1.11.60 (C006)
                    std::cout << "  [Activity] instruction mix COUNTED: "
                              << int_per_core << " int+mul, " << fp_per_core
                              << " fp, " << br_per_core << " branch per core "
                              << "(decoder-classified; the 87.5/12.5 stand-in "
                                 "is not used)" << std::endl;
                }
            }
        }
        xml << "      <stat name=\"int_instructions\" value=\"" << int_per_core << "\"/>\n";
        xml << "      <stat name=\"fp_instructions\" value=\"" << fp_per_core << "\"/>\n";
        xml << "      <stat name=\"branch_instructions\" value=\"" << br_per_core << "\"/>\n";
        /* 1.9.28: measured, not a fixed 1%. Mispredicts drive pipeline-flush
         * energy, and their rate varies enormously by workload -- a regular
         * stencil and an irregular graph traversal are not the same machine. */
        /* 1.9.29: gated on the same self-consistency test. A mispredict count
         * taken from a different base than the branch count it is a subset of
         * would report a mispredict RATE that is meaningless. */
        uint64_t mispred_per_core = (meas_mispred_ > 0 && meas_self_consistent)
            ? meas_mispred_ / std::max(1, config_.num_cores)
            : inst_per_core * 1 / 100;
        if (mispred_per_core > br_per_core) mispred_per_core = br_per_core;
        xml << "      <stat name=\"branch_mispredictions\" value=\"" << mispred_per_core << "\"/>\n";
        /* 1.11.47 (FIX-PRE-FLEET L200): mixLd/mixSt were measured, parsed,
         * stored -- and never used; loads/stores stayed hardcoded 20%/10%.
         * Measured values now reach McPAT, UNSOURCED fractions only as the
         * warned fallback for cores that never decode. */
        {
            const uint64_t nc_ = std::max(1, config_.num_cores);
            if ((meas_ld_ + meas_st_) == 0)
                warnUnsourcedMix("load/store mix", "20% loads, 10% stores");
            uint64_t ld_pc = (meas_ld_ + meas_st_) > 0 ? meas_ld_ / nc_
                                                       : inst_per_core * 20 / 100;
            uint64_t st_pc = (meas_ld_ + meas_st_) > 0 ? meas_st_ / nc_
                                                       : inst_per_core * 10 / 100;
            xml << "      <stat name=\"load_instructions\" value=\"" << ld_pc << "\"/>\n";
            xml << "      <stat name=\"store_instructions\" value=\"" << st_pc << "\"/>\n";
        }
        xml << "      <stat name=\"committed_instructions\" value=\"" << inst_per_core << "\"/>\n";
        xml << "      <stat name=\"committed_int_instructions\" value=\"" << int_per_core << "\"/>\n";
        xml << "      <stat name=\"committed_fp_instructions\" value=\"" << fp_per_core << "\"/>\n";
        xml << "      <stat name=\"pipeline_duty_cycle\" value=\"" << pipeline_duty_cycle << "\"/>\n";
        xml << "      <stat name=\"total_cycles\" value=\"" << total_cycles_ << "\"/>\n";
        /* 1.11.57 (latent C035): the clamped value computed above, not a
         * second unguarded subtraction. This is the site McPAT actually
         * parses. */
        xml << "      <stat name=\"idle_cycles\" value=\"" << idle_cycles << "\"/>\n";
        xml << "      <stat name=\"busy_cycles\" value=\"" << busy_cycles_ << "\"/>\n";
        xml << "      <stat name=\"ROB_reads\" value=\"" << (is_ooo ? inst_per_core : 0ULL) << "\"/>\n";
        xml << "      <stat name=\"ROB_writes\" value=\"" << (is_ooo ? inst_per_core : 0ULL) << "\"/>\n";
        xml << "      <stat name=\"rename_reads\" value=\"" << (is_ooo ? inst_per_core : 0ULL) << "\"/>\n";
        xml << "      <stat name=\"rename_writes\" value=\"" << (is_ooo ? inst_per_core : 0ULL) << "\"/>\n";
        xml << "      <stat name=\"fp_rename_reads\" value=\"" << (is_ooo ? inst_per_core * 10 / 100 : 0ULL) << "\"/>\n";
        xml << "      <stat name=\"fp_rename_writes\" value=\"" << (is_ooo ? inst_per_core * 10 / 100 : 0ULL) << "\"/>\n";
        xml << "      <stat name=\"inst_window_reads\" value=\"" << (is_ooo ? inst_per_core : 0ULL) << "\"/>\n";
        xml << "      <stat name=\"inst_window_writes\" value=\"" << (is_ooo ? inst_per_core : 0ULL) << "\"/>\n";
        xml << "      <stat name=\"inst_window_wakeup_accesses\" value=\"" << (is_ooo ? inst_per_core : 0ULL) << "\"/>\n";
        xml << "      <stat name=\"fp_inst_window_reads\" value=\"" << (is_ooo ? inst_per_core * 10 / 100 : 0ULL) << "\"/>\n";
        xml << "      <stat name=\"fp_inst_window_writes\" value=\"" << (is_ooo ? inst_per_core * 10 / 100 : 0ULL) << "\"/>\n";
        xml << "      <stat name=\"fp_inst_window_wakeup_accesses\" value=\"" << (is_ooo ? inst_per_core * 10 / 100 : 0ULL) << "\"/>\n";
        xml << "      <stat name=\"int_regfile_reads\" value=\"" << (inst_per_core * 2) << "\"/>\n";
        xml << "      <stat name=\"int_regfile_writes\" value=\"" << inst_per_core << "\"/>\n";
        xml << "      <stat name=\"float_regfile_reads\" value=\"" << (inst_per_core * 20 / 100) << "\"/>\n";
        xml << "      <stat name=\"float_regfile_writes\" value=\"" << (inst_per_core * 10 / 100) << "\"/>\n";
        xml << "      <stat name=\"function_calls\" value=\"" << (inst_per_core / 100) << "\"/>\n";
        xml << "      <stat name=\"context_switches\" value=\"0\"/>\n";
        /* 1.11.47 (FIX-PRE-FLEET L199): the measured mix never reached the
         * stats that actually drive execution-unit power -- ialu/fpu/mul
         * stayed at 70/10/5% while the census sat computed above. Measured
         * classes now drive the units; the fractions remain only as the
         * warned no-decode fallback. */
        {
            const uint64_t nc_ = std::max(1, config_.num_cores);
            const bool mm_ = (meas_int_ + meas_fp_ + meas_mul_) > 0;
            if (!mm_) warnUnsourcedMix("instruction mix",
                                       "70% int, 10% fp, 5% mul");
            uint64_t ia_pc = mm_ ? meas_int_ / nc_ : inst_per_core * 70 / 100;
            uint64_t fp_pc = mm_ ? meas_fp_  / nc_ : inst_per_core * 10 / 100;
            uint64_t mu_pc = mm_ ? meas_mul_ / nc_ : inst_per_core * 5 / 100;
            /* 1.11.51 (L215/L223): on an FPU-less element the FP class does
             * not vanish -- it EXECUTES as a soft-float integer sequence the
             * timing side already charges at fp_emul_cycles per op. Pricing
             * it at a zero-FPU cost while adding only cycles made the
             * FPU-less element look MORE power-efficient (same energy over
             * more time), and on the ALU profile the emulation bypassed
             * every datapath scaling the element applies to its integer
             * work. The fold below routes the emulation through the integer
             * ALU stat -- one integer-op-equivalent per charged cycle, the
             * same 1-CPI equivalence the timing charge uses -- so it now
             * rides exactly the datapath factors real integer work rides. */
            if (config_.num_fpus == 0 && config_.fp_emul_cycles > 0 && fp_pc > 0) {
                uint64_t emul_ops = fp_pc * (uint64_t)config_.fp_emul_cycles;
                std::cout << "  [Activity] FPU-less element: " << fp_pc
                          << " FP ops/core priced as soft-float integer work ("
                          << emul_ops << " int-op equivalents at "
                          << config_.fp_emul_cycles << " cycles/op)."
                          << std::endl;
                ia_pc += emul_ops;
                fp_pc = 0;
            }
            xml << "      <stat name=\"ialu_accesses\" value=\"" << ia_pc << "\"/>\n";
            xml << "      <stat name=\"fpu_accesses\" value=\"" << fp_pc << "\"/>\n";
            xml << "      <stat name=\"mul_accesses\" value=\"" << mu_pc << "\"/>\n";
        }
        xml << "      <stat name=\"cdb_alu_accesses\" value=\"" << inst_per_core << "\"/>\n";
        {
            const uint64_t nc_ = std::max(1, config_.num_cores);
            const bool mm_ = (meas_int_ + meas_fp_ + meas_mul_) > 0;
            uint64_t cdb_fp = mm_ ? meas_fp_ / nc_ : inst_per_core * 10 / 100;
            if (config_.num_fpus == 0 && config_.fp_emul_cycles > 0)
                cdb_fp = 0;   // 1.11.51 (L215): no FPU result bus exists
            xml << "      <stat name=\"cdb_mul_accesses\" value=\""
                << (mm_ ? meas_mul_ / nc_ : inst_per_core * 5 / 100) << "\"/>\n";
            xml << "      <stat name=\"cdb_fpu_accesses\" value=\"" << cdb_fp << "\"/>\n";
        }

        if (alu_only) {
            /* 1.9.32: the element's instruction store, stated instead of
             * inherited. McPAT builds an instruction-fetch unit for every core
             * unconditionally, and this path emitted no icache parameters at
             * all -- so the store was constructed from ParseXML's initialize(),
             * which fills the whole config vector with the literal value 1. The
             * element's instruction supply was therefore a ONE-BYTE, one-line,
             * one-way cache: not a modelling choice, an uninitialised default
             * that nothing had ever looked at.
             *
             * What a processing element really has is a resident instruction
             * memory, sized by pe_imem_bytes -- roughly a hundred bytes for a
             * command-driven in-bank engine, kilobytes for a programmable
             * near-bank one. That size is the interesting design axis: it
             * decides which kernels can run on the element at all.
             *
             * Direct-mapped, one instruction word per line, and ZERO misses:
             * the program is resident, so there is no refill path to charge.
             *
             * RESIDUAL, named rather than hidden: this is still McPAT's cache
             * constructor, so the store carries a tag array and one-entry miss,
             * fill and prefetch structures that a scratchpad does not have.
             * That overstates the instruction store by the tag overhead. Fixing
             * it needs a pure-RAM instruction store inside McPAT's fetch unit,
             * which is the remaining half of this item. */
            int imem_bytes = config_.pe_imem_bytes;
            if (imem_bytes < 64) {
                std::cerr << "[power] WARNING: pe_imem_bytes=" << imem_bytes
                          << " is below the smallest array the model can build; "
                          << "using 64 bytes. An element this small is command-"
                          << "driven and should be described as such, not as a "
                          << "degenerate instruction memory.\n";
                imem_bytes = 64;
            }
            uint64_t ifetch_per_core = inst_per_core;
            xml << "      <component id=\"system.core" << i << ".icache\" name=\"icache\">\n";
            xml << "        <param name=\"icache_config\" value=\"" << imem_bytes
                << ",8,1,1,1,1,64,0\"/>\n";
            /* 1.11.3 (#111): buffer_sizes[0]=0 is the fork's marker for a
             * RESIDENT store -- InstFetchU builds it as a pure RAM (no tag)
             * and does not build miss/fill/prefetch buffers at all. */
            xml << "        <param name=\"buffer_sizes\" value=\"0,0,0,0\"/>\n";
            xml << "        <stat name=\"read_accesses\" value=\"" << ifetch_per_core << "\"/>\n";
            xml << "        <stat name=\"read_misses\" value=\"0\"/>\n";
            xml << "        <stat name=\"conflicts\" value=\"0\"/>\n";
            xml << "      </component>\n";
        }

        if (!alu_only) {
            // L1 icache -- use actual per-core stats
            uint64_t l1i_reads_per_core = l1i_reads_ / std::max(1, config_.num_cores);
            uint64_t l1i_misses_per_core = l1i_read_misses_ / std::max(1, config_.num_cores);
            xml << "      <component id=\"system.core" << i << ".icache\" name=\"icache\">\n";
            xml << "        <param name=\"icache_config\" value=\"" << config_.l1i_size_bytes
                << ",64,8,1,1,3,64,0\"/>\n";
            xml << "        <param name=\"buffer_sizes\" value=\"16,16,16,0\"/>\n";
            xml << "        <stat name=\"read_accesses\" value=\"" << l1i_reads_per_core << "\"/>\n";
            xml << "        <stat name=\"read_misses\" value=\"" << l1i_misses_per_core << "\"/>\n";
            xml << "        <stat name=\"conflicts\" value=\"0\"/>\n";
            xml << "      </component>\n";

            // L1 dcache -- use actual per-core stats
            uint64_t l1d_reads_per_core = l1d_reads_ / std::max(1, config_.num_cores);
            uint64_t l1d_writes_per_core = l1d_writes_ / std::max(1, config_.num_cores);
            uint64_t l1d_rmisses_per_core = l1d_read_misses_ / std::max(1, config_.num_cores);
            uint64_t l1d_wmisses_per_core = l1d_write_misses_ / std::max(1, config_.num_cores);
            xml << "      <component id=\"system.core" << i << ".dcache\" name=\"dcache\">\n";
            xml << "        <param name=\"dcache_config\" value=\"" << config_.l1d_size_bytes
                << ",64,8,1,1,3,64,0\"/>\n";
            xml << "        <param name=\"buffer_sizes\" value=\"16,16,16,16\"/>\n";
            xml << "        <stat name=\"read_accesses\" value=\"" << l1d_reads_per_core << "\"/>\n";
            xml << "        <stat name=\"write_accesses\" value=\"" << l1d_writes_per_core << "\"/>\n";
            xml << "        <stat name=\"read_misses\" value=\"" << l1d_rmisses_per_core << "\"/>\n";
            xml << "        <stat name=\"write_misses\" value=\"" << l1d_wmisses_per_core << "\"/>\n";
            xml << "        <stat name=\"conflicts\" value=\"0\"/>\n";
            xml << "      </component>\n";
        }
        xml << "    </component>\n";  // close coreN
    }

    // L2 cache: homogeneous_L2s=1 means emit exactly ONE L2 template.
    // 1.11.56 (audit C036): emitted only when an L2 was actually configured.
    if (num_l2s > 0) {
        for (int i = 0; i < 1; i++) {
            uint64_t l2_reads_per = l2_reads_ / std::max(1, config_.num_cores);
            uint64_t l2_writes_per = l2_writes_ / std::max(1, config_.num_cores);
            uint64_t l2_rmisses_per = l2_read_misses_ / std::max(1, config_.num_cores);
            uint64_t l2_wmisses_per = l2_write_misses_ / std::max(1, config_.num_cores);
            xml << "    <component id=\"system.L2" << i << "\" name=\"L2" << i << "\">\n";
            xml << "      <param name=\"L2_config\" value=\"" << config_.l2_size_bytes
                << ",64,8,8,8,23,64,1\"/>\n";
            xml << "      <param name=\"buffer_sizes\" value=\"16,16,16,16\"/>\n";
            xml << "      <param name=\"clockrate\" value=\"" << static_cast<int>(config_.core_clock_mhz) << "\"/>\n";
            xml << "      <param name=\"ports\" value=\"1,1,1\"/>\n";
            /* 1.11.49 (FIX-PRE-FLEET L69): the corner the system declares is
             * the corner the caches get. This was a hardcoded 0 (hp) in the
             * same XML whose system-level device_type carried the user's
             * power.device_corner -- silently overriding it for every L2. */
            xml << "      <param name=\"device_type\" value=\"" << config_.device_type << "\"/>\n";
            xml << "      <stat name=\"read_accesses\" value=\"" << l2_reads_per << "\"/>\n";
            xml << "      <stat name=\"write_accesses\" value=\"" << l2_writes_per << "\"/>\n";
            xml << "      <stat name=\"read_misses\" value=\"" << l2_rmisses_per << "\"/>\n";
            xml << "      <stat name=\"write_misses\" value=\"" << l2_wmisses_per << "\"/>\n";
            xml << "      <stat name=\"conflicts\" value=\"0\"/>\n";
            /* 1.11.56 (audit C006): MEASURED, not an unsourced fraction of a
             * tautology. This read busy_cycles_/total_cycles_ * 0.5. Every
             * reachable caller passes the same value to setTotalCycles and
             * setBusyCycles, so the first factor is identically 1.0 and the
             * emitted number was the bare literal 0.5 -- "this L2 is 50% busy"
             * asserted about every run this simulator has ever produced, with
             * no source. (0.5 is the value in McPAT's own Xeon reference XML,
             * which is a description of a Xeon, not of a PIM element.)
             * The quantity is measurable and we already count it: accesses
             * per cycle at this cache instance. McPAT uses cachep.duty_cycle
             * only on the is_tdp branch of SharedCache::computeEnergy, where
             * it sets the peak-power access rate, so what moves is the
             * reported PEAK power of L2 (and L3 below) -- runtime dynamic is
             * driven by the access counts and does not change. Clamped to
             * [0,1] because McPAT reads it as a per-port rate. */
            const double l2_duty =
                (total_cycles_ > 0)
                    ? std::min(1.0, static_cast<double>(l2_reads_per + l2_writes_per)
                                        / static_cast<double>(total_cycles_))
                    : 0.0;
            xml << "      <stat name=\"duty_cycle\" value=\"" << l2_duty << "\"/>\n";
            xml << "    </component>\n";
        }
    }

    // L3 cache (shared) -- skip if ALU-only or no L3
    if (has_l3) {
        xml << "    <component id=\"system.L3\" name=\"L3\">\n";
        // 1.9.10 fix: McPAT/CACTI cache_config field 1 is capacity in BYTES (as L2
        // above correctly emits). This path emitted MEGABYTES, so a 32MB LLC became a
        // "32-byte" array and CACTI's ArrayST::error_checking() rejected the geometry
        // (the standalone-L3 failure that forced host LLC to be literature-anchored).
        xml << "      <param name=\"L3_config\" value=\"" << config_.l3_size_bytes
            << ",64,16,16,16,23,64,1\"/>\n";
        xml << "      <param name=\"clockrate\" value=\"" << static_cast<int>(config_.core_clock_mhz) << "\"/>\n";
        xml << "      <param name=\"ports\" value=\"1,1,1\"/>\n";
        xml << "      <param name=\"device_type\" value=\"" << config_.device_type << "\"/>\n";  /* 1.11.49 (L69) */
        xml << "      <param name=\"buffer_sizes\" value=\"16,16,16,16\"/>\n";
        xml << "      <stat name=\"read_accesses\" value=\"" << l3_reads_ << "\"/>\n";
        xml << "      <stat name=\"write_accesses\" value=\"" << l3_writes_ << "\"/>\n";
        xml << "      <stat name=\"read_misses\" value=\"" << l3_read_misses_ << "\"/>\n";
        xml << "      <stat name=\"write_misses\" value=\"" << l3_write_misses_ << "\"/>\n";
        xml << "      <stat name=\"conflicts\" value=\"0\"/>\n";
        /* 1.11.56 (audit C006): measured, same reasoning as the L2 above --
         * the old expression was busy_cycles_/total_cycles_ * 0.3, i.e. the
         * literal 0.3, and 0.3 was not even McPAT's own reference value for a
         * shared L3 (that XML uses 1). The L3 is shared, so its accesses are
         * not divided per core. */
        const double l3_duty =
            (total_cycles_ > 0)
                ? std::min(1.0, static_cast<double>(l3_reads_ + l3_writes_)
                                    / static_cast<double>(total_cycles_))
                : 0.0;
        xml << "      <stat name=\"duty_cycle\" value=\"" << l3_duty << "\"/>\n";
        xml << "    </component>\n";
    }

    // NoC -- N instances from noc_levels_ (or single legacy instance)
    if (!noc_levels_.empty()) {
        // N heterogeneous NoC instances
        for (size_t ni = 0; ni < noc_levels_.size(); ni++) {
            const auto& lvl = noc_levels_[ni];
            xml << "    <component id=\"system.noc" << ni << "\" name=\"noc" << ni << "\">\n";
            xml << "      <param name=\"clockrate\" value=\"" << static_cast<int>(lvl.clock_mhz) << "\"/>\n";
            xml << "      <param name=\"vdd\" value=\"0\"/>\n";
            xml << "      <param name=\"power_gating_vcc\" value=\"-1\"/>\n";
            xml << "      <param name=\"type\" value=\"" << lvl.type << "\"/>\n";
            xml << "      <param name=\"horizontal_nodes\" value=\"" << lvl.horizontal_nodes << "\"/>\n";
            xml << "      <param name=\"vertical_nodes\" value=\"" << lvl.vertical_nodes << "\"/>\n";
            xml << "      <param name=\"has_global_link\" value=\"0\"/>\n";
            xml << "      <param name=\"link_throughput\" value=\"1\"/>\n";
            xml << "      <param name=\"link_latency\" value=\"1\"/>\n";
            xml << "      <param name=\"input_ports\" value=\"" << lvl.input_ports << "\"/>\n";
            xml << "      <param name=\"output_ports\" value=\"" << lvl.output_ports << "\"/>\n";
            xml << "      <param name=\"virtual_channel_per_port\" value=\""
                << std::max(1, config_.noc_vcs_per_vnet) << "\"/>\n";
            xml << "      <param name=\"input_buffer_entries_per_vc\" value=\""
                << std::max(1, config_.noc_vc_buffer_size) << "\"/>\n";
            xml << "      <param name=\"flit_bits\" value=\"" << lvl.flit_bits << "\"/>\n";
            xml << "      <param name=\"chip_coverage\" value=\"" << lvl.chip_coverage << "\"/>\n";
            /* 1.11.50 (L74): per-level family scope -- see NoCLevelConfig. */
            xml << "      <param name=\"on_dram_die\" value=\"" << lvl.on_dram_die << "\"/>\n";
            xml << "      <param name=\"link_routing_over_percentage\" value=\"0.5\"/>\n";
            xml << "      <stat name=\"total_accesses\" value=\"" << lvl.total_accesses << "\"/>\n";
            xml << "      <stat name=\"duty_cycle\" value=\"" << lvl.duty_cycle << "\"/>\n";
            xml << "    </component>\n";
        }
    } else if (config_.has_noc) {
        // Single legacy NoC instance
        int grid = static_cast<int>(std::sqrt(config_.num_cores));
        if (grid < 1) grid = 1;
        int noc_type = (config_.noc_topology == 2) ? 0 : 1;  // bus=0, else router=1
        /* 1.11.57 (latent C018): THE SUBSTITUTION ANNOUNCES ITSELF NOW.
         *
         * WHAT WAS WRONG. When no NoC activity had been supplied this branch
         * silently fell back to `mc_reads_ + mc_writes_` and emitted the
         * result as the fabric's total_accesses and duty cycle -- a number
         * that looks in every way like a measurement of the network and is
         * not one. It is wrong twice over. It is the wrong POPULATION:
         * memory-controller transactions are the subset of traffic that
         * reached DRAM, while a fabric also carries PE-to-PE, cache-to-cache
         * and control packets that never touch an MC. And it is the wrong
         * UNIT: McPAT charges a router access per traversal, so a packet
         * crossing N routers is N accesses -- which is why the live
         * per-level path in main.cpp feeds total_hops and not packet counts,
         * and why that change measured a 2.04x difference. Substituting MC
         * transactions understates router activity on both counts at once.
         *
         * WHY IT WAS INVISIBLE. This entire `else if (config_.has_noc)`
         * branch is unreachable. Every reachable construction that sets
         * has_noc also calls setNoCLevels() with a non-empty vector, which
         * routes to the per-level branch above: device scope gates both on
         * the same `num_pes > 1 || hierarchy_enabled` condition, the co-sim
         * and probe paths always call it, and the host sets has_noc = false
         * and takes the stub below. setNoCActivity() has no caller in src/ at
         * all, so noc_activity_.total_packets is 0 in every run -- meaning
         * the ternary has only ever been capable of selecting the fallback,
         * in a branch nothing enters.
         *
         * ANNOUNCED RATHER THAN REFUSED. Emitting 0 accesses would be its own
         * fabrication (a NoC that carried nothing), and refusing would fail a
         * legacy path that a caller may deliberately want as a coarse
         * estimate. So the substitution stands and says what it is, once per
         * wrapper. If the path is ever made live, the correct repair is to
         * call setNoCLevels() with measured traversals -- not to trust this
         * number. */
        uint64_t noc_accesses = noc_activity_.total_packets;
        if (noc_accesses == 0) {
            noc_accesses = mc_reads_ + mc_writes_;
            /* 1.11.60 (audit round 4, C006): diagOnce(). This is the second
             * message the audit caught stating the negative of the truth: at
             * initialize() mc_reads_/mc_writes_ are 0, so it reported the
             * substitution as "(0 reads + 0 writes = 0)" and then went silent
             * for the pass that had the real transaction counts. */
            if (diagOnce(warned_noc_substitution_)) {
                std::cerr << "[power] WARNING: legacy single-NoC path has NO "
                             "measured NoC activity; SUBSTITUTING "
                             "memory-controller transactions ("
                          << mc_reads_ << " reads + " << mc_writes_
                          << " writes = " << noc_accesses
                          << ") as the fabric's total_accesses. This is NOT a "
                             "measurement of the NoC: MC transactions exclude "
                             "all traffic that never reaches DRAM, and McPAT "
                             "charges one router access per TRAVERSAL while "
                             "this counts whole transactions -- so NoC dynamic "
                             "power below is understated on both counts. Supply "
                             "setNoCLevels() with measured traversals to price "
                             "the fabric properly." << std::endl;
            }
        }
        double noc_duty_cycle = (total_cycles_ > 0)
            ? static_cast<double>(noc_accesses) / total_cycles_
            : 0.0;

        xml << "    <component id=\"system.noc0\" name=\"noc0\">\n";
        xml << "      <param name=\"clockrate\" value=\"" << static_cast<int>(config_.core_clock_mhz) << "\"/>\n";
        xml << "      <param name=\"vdd\" value=\"0\"/>\n";
        xml << "      <param name=\"power_gating_vcc\" value=\"-1\"/>\n";
        xml << "      <param name=\"type\" value=\"" << noc_type << "\"/>\n";
        xml << "      <param name=\"horizontal_nodes\" value=\"" << grid << "\"/>\n";
        xml << "      <param name=\"vertical_nodes\" value=\"" << grid << "\"/>\n";
        xml << "      <param name=\"has_global_link\" value=\"0\"/>\n";
        xml << "      <param name=\"link_throughput\" value=\"1\"/>\n";
        xml << "      <param name=\"link_latency\" value=\"1\"/>\n";
        xml << "      <param name=\"input_ports\" value=\"5\"/>\n";
        xml << "      <param name=\"output_ports\" value=\"5\"/>\n";
        xml << "      <param name=\"virtual_channel_per_port\" value=\""
            << std::max(1, config_.noc_vcs_per_vnet) << "\"/>\n";
        xml << "      <param name=\"input_buffer_entries_per_vc\" value=\""
            << std::max(1, config_.noc_vc_buffer_size) << "\"/>\n";
        xml << "      <param name=\"flit_bits\" value=\"128\"/>\n";
        xml << "      <param name=\"chip_coverage\" value=\"1\"/>\n";
        xml << "      <param name=\"link_routing_over_percentage\" value=\"0.5\"/>\n";
        xml << "      <stat name=\"total_accesses\" value=\"" << noc_accesses << "\"/>\n";
        xml << "      <stat name=\"duty_cycle\" value=\"" << noc_duty_cycle << "\"/>\n";
        xml << "    </component>\n";
    } else {
        // Minimal NoC stub -- McPAT's positional XML parser requires at least 1 NoC
        // component to correctly offset to the MC section
        xml << "    <component id=\"system.noc0\" name=\"noc0\">\n";
        xml << "      <param name=\"clockrate\" value=\"" << static_cast<int>(config_.core_clock_mhz) << "\"/>\n";
        xml << "      <param name=\"vdd\" value=\"0\"/>\n";
        xml << "      <param name=\"power_gating_vcc\" value=\"-1\"/>\n";
        xml << "      <param name=\"type\" value=\"0\"/>\n";
        xml << "      <param name=\"horizontal_nodes\" value=\"1\"/>\n";
        xml << "      <param name=\"vertical_nodes\" value=\"1\"/>\n";
        xml << "      <param name=\"has_global_link\" value=\"0\"/>\n";
        xml << "      <param name=\"link_throughput\" value=\"1\"/>\n";
        xml << "      <param name=\"link_latency\" value=\"1\"/>\n";
        xml << "      <param name=\"input_ports\" value=\"1\"/>\n";
        xml << "      <param name=\"output_ports\" value=\"1\"/>\n";
        xml << "      <param name=\"flit_bits\" value=\"64\"/>\n";
        xml << "      <param name=\"chip_coverage\" value=\"0\"/>\n";
        xml << "      <param name=\"link_routing_over_percentage\" value=\"0\"/>\n";
        xml << "      <stat name=\"total_accesses\" value=\"0\"/>\n";
        xml << "      <stat name=\"duty_cycle\" value=\"0\"/>\n";
        xml << "    </component>\n";
    }

    // Memory controller -- uses actual mc_reads_/mc_writes_ and mc_tech_ params
    xml << "    <component id=\"system.mc\" name=\"mc\">\n";
    /* 1.11.19 (user decisions D2+D3): ONE BACKEND MODEL, THREE INTERFACE
     * TIERS. The backend is always type=0 -- McPAT's Cadence full-MC fit --
     * so every placement on the ladder is priced on the same basis and the
     * placement study stays iso-model. Only the DRIVER varies with where
     * the controller physically sits:
     *
     *   subarray..chip (on-die)  withPHY=0            no driver at all
     *   logic-die / channel      withPHY=1, class=1   interposer/TSV
     *   rank+ / host MC          withPHY=1, class=0   off-package DDR
     *
     * This REPLACES the 1.11.15/1.11.16 shape, which expressed "no PHY" as
     * type=1 -- that also silently swapped the BACKEND cost model (embedded
     * DDR3-Lite fit, ~15x area drop at 22nm), so an on-die MC was compared
     * against a rank MC on two different curves. memoryctrl.cc now honors
     * withPHY for type=0 (it used to ignore it there). */
    xml << "      <param name=\"type\" value=\"0\"/>\n";
    xml << "      <param name=\"mc_clock\" value=\"" << static_cast<int>(config_.mc_clock_mhz) << "\"/>\n";
    xml << "      <param name=\"vdd\" value=\"0\"/>\n";
    xml << "      <param name=\"power_gating_vcc\" value=\"-1\"/>\n";
    xml << "      <param name=\"peak_transfer_rate\" value=\"" << mc_tech_.peak_transfer_rate << "\"/>\n";
    /* 1.11.56 (audit C008): the five structural numbers below come from
     * MCTechParams now instead of being literals in this stream. They still
     * default to the values the literals held, so nothing moves; what changes
     * is that a controller with more than one channel, a deeper queue or a
     * different transaction granularity has somewhere to be described, and
     * that the run SAYS the description is unsourced (see the warning below)
     * instead of presenting single-channel/32/32/64/51 as facts. */
    /* 1.11.60 (audit round 4, C006): diagOnce(). */
    if (diagOnce(warned_mc_structure_)) {
        std::cerr << "[McPATWrapper] WARNING: the memory controller's structure is "
                     "UNSOURCED and in force in every run: "
                  << mc_tech_.memory_channels_per_mc << " channel(s) per MC, "
                  << mc_tech_.req_window_size_per_channel << "-entry request window, "
                  << mc_tech_.io_buffer_size_per_channel << "-entry IO buffer, "
                  << mc_tech_.block_size_bytes << " B blocks, "
                  << mc_tech_.addressbus_width << "-bit address bus (this last one "
                     "disagrees with the 48-bit physical_address_width emitted for "
                     "the cores, unreconciled). No YAML key sets any of them; they "
                     "govern MC queue/buffer area and power." << std::endl;
    }
    xml << "      <param name=\"block_size\" value=\"" << mc_tech_.block_size_bytes << "\"/>\n";
    xml << "      <param name=\"number_mcs\" value=\"" << mc_tech_.number_mcs << "\"/>\n";
    xml << "      <param name=\"memory_channels_per_mc\" value=\""
        << mc_tech_.memory_channels_per_mc << "\"/>\n";
    xml << "      <param name=\"number_ranks\" value=\"" << mc_tech_.number_ranks << "\"/>\n";
    /* 1.11.19: the interface tier (see the type comment above). */
    xml << "      <param name=\"withPHY\" value=\""
        << (config_.mc_phy_tier == SystemConfig::MCPhyTier::NONE ? 0 : 1) << "\"/>\n";
    xml << "      <param name=\"phy_class\" value=\""
        << (config_.mc_phy_tier == SystemConfig::MCPhyTier::INTERPOSER ? 1 : 0) << "\"/>\n";
    /* 1.11.19 (user decision D10): state the interface class explicitly
     * instead of riding McPAT's default. XML_Parse defaults LVDS=true, which
     * is why the code path yields ~2.2 pJ/bit while docs/changelog.md:2088
     * cited ~9 (the non-LVDS branch). A differential memory PHY is the
     * right class for every DDR-family part we model, so the value is the
     * same -- but it is now SAID, and the cited figure is reproducible. */
    xml << "      <param name=\"LVDS\" value=\"1\"/>\n";
    xml << "      <param name=\"req_window_size_per_channel\" value=\""
        << mc_tech_.req_window_size_per_channel << "\"/>\n";   // 1.11.56 (C008)
    xml << "      <param name=\"IO_buffer_size_per_channel\" value=\""
        << mc_tech_.io_buffer_size_per_channel << "\"/>\n";     // 1.11.56 (C008)
    xml << "      <param name=\"databus_width\" value=\"" << mc_tech_.databus_width << "\"/>\n";
    xml << "      <param name=\"addressbus_width\" value=\""
        << mc_tech_.addressbus_width << "\"/>\n";               // 1.11.56 (C008)
    {
        int num_mcs = std::max(1, mc_tech_.number_mcs);
        xml << "      <stat name=\"memory_accesses\" value=\"" << (mc_reads_ + mc_writes_) / num_mcs << "\"/>\n";
        xml << "      <stat name=\"memory_reads\" value=\"" << mc_reads_ / num_mcs << "\"/>\n";
        xml << "      <stat name=\"memory_writes\" value=\"" << mc_writes_ / num_mcs << "\"/>\n";
    }
    xml << "    </component>\n";

    // NIU -- mandatory stub
    xml << "    <component id=\"system.niu\" name=\"niu\">\n";
    xml << "      <param name=\"type\" value=\"1\"/>\n";
    xml << "      <param name=\"clockrate\" value=\"350\"/>\n";
    xml << "      <param name=\"vdd\" value=\"0\"/>\n";
    xml << "      <param name=\"power_gating_vcc\" value=\"-1\"/>\n";
    xml << "      <param name=\"number_units\" value=\"0\"/>\n";
    xml << "      <stat name=\"duty_cycle\" value=\"0\"/>\n";
    xml << "      <stat name=\"total_load_perc\" value=\"0\"/>\n";
    xml << "    </component>\n";

    // PCIe -- active when co-sim transfers present, otherwise stub.
    // 1.11.7: clock from the configured link (350 was a hardwired literal),
    // measured bytes + per-link-type pJ/bit drive the dynamic term inside
    // the fork (iocontrollers.cc) -- zero traffic = zero link dynamic.
    {
        /* 1.11.56 (audit C017): the 350 is UNSOURCED and was silent. It is
         * McPAT's historical niu/pcie stub clock, not a property of any link
         * this tree models, and the link controller's digital dynamic power
         * scales linearly with it -- for a gen5/CXL link whose derived
         * controller clock is 1000 MHz, falling back to 350 prices that
         * controller at 0.35x. One caller derives the clock and warns
         * (the co-sim path); the dual-McPAT HOST site does not set
         * link_clock_mhz at all, so the fallback fires there on every system
         * -scope run with a host node and power.pcie.enabled. We cannot fix
         * that caller from here, and we will not present its result as
         * priced: say what stood in, name what it governs, once. */
        int link_clk = pcie_stats_.link_clock_mhz;
        if (link_clk <= 0) {
            link_clk = 350;
            /* 1.11.60 (audit round 4, C006): diagOnce(). pcie_stats_ is set
             * by setPCIeStats() after initialize(), so number_units was 0 on
             * the pass that held the latch and this note could not fire. */
            if (pcie_stats_.number_units > 0 && diagOnce(warned_link_clock_)) {
                std::cerr << "[McPATWrapper] WARNING: no link controller clock was "
                             "supplied for link type '" << pcie_stats_.link_type_name
                          << "'; the UNSOURCED 350 MHz literal is in force. The "
                             "link controller's digital dynamic power scales "
                             "linearly with this clock." << std::endl;
            }
        }
        xml << "    <component id=\"system.pcie\" name=\"pcie\">\n";
        xml << "      <param name=\"type\" value=\"1\"/>\n";
        /* 1.11.29 step 1: from the link CLASS, not a literal. */
        const bool has_serdes = linkHasSerDes(pcie_stats_.link_type_name);
        xml << "      <param name=\"withPHY\" value=\"" << (has_serdes ? 1 : 0)
            << "\"/>\n";
        /* 1.11.29 step 3: the class's own signalling rate, not PCIe 2.0's. */
        const double lane_gbps = linkSerDesLaneGbps(pcie_stats_.link_type_name);
        if (lane_gbps > 0.0) {
            xml << "      <param name=\"serdes_lane_gbps\" value=\"" << lane_gbps
                << "\"/>\n";
        } else if (pcie_stats_.number_units > 0 && diagOnce(warned_link_class_)) {
            /* 1.11.60 (audit round 4, C006): diagOnce(); same reason as the
             * link clock note above. */
            /* 1.11.56 (audit C017): a link type this table cannot name gets
             * McPAT's historical 4 Gb/s (PCIe 2.0) SerDes rate, silently. The
             * bare name "pcie" -- which is PCIeStats' own default, and what
             * the host site leaves in place -- matches no prefix here, so a
             * host link is priced at PCIe 2.0 while the report calls it
             * whatever the run configured. Not repaired here (the rate is
             * only knowable from the class); said. */
            std::cerr << "[McPATWrapper] WARNING: link type '"
                      << pcie_stats_.link_type_name << "' names no known SerDes "
                         "class, so serdes_lane_gbps is not emitted and McPAT's "
                         "historical 4 Gb/s (PCIe 2.0) rate prices this link."
                      << std::endl;
        }
        xml << "      <param name=\"clockrate\" value=\"" << link_clk << "\"/>\n";
        xml << "      <param name=\"vdd\" value=\"0\"/>\n";
        xml << "      <param name=\"power_gating_vcc\" value=\"-1\"/>\n";
        xml << "      <param name=\"number_units\" value=\"" << pcie_stats_.number_units << "\"/>\n";
        xml << "      <param name=\"num_channels\" value=\"" << pcie_stats_.num_channels << "\"/>\n";
        xml << "      <stat name=\"duty_cycle\" value=\"" << pcie_stats_.duty_cycle << "\"/>\n";
        xml << "      <stat name=\"total_load_perc\" value=\"" << pcie_stats_.total_load_perc << "\"/>\n";
        xml << "      <stat name=\"transferred_bytes\" value=\"" << pcie_stats_.transferred_bytes << "\"/>\n";
        xml << "      <stat name=\"link_pj_per_bit\" value=\"" << pcie_stats_.link_pj_per_bit << "\"/>\n";
        xml << "    </component>\n";
    }

    // Flash controller -- mandatory stub
    xml << "    <component id=\"system.flashc\" name=\"flashc\">\n";
    xml << "      <param name=\"number_flashcs\" value=\"0\"/>\n";
    xml << "      <param name=\"type\" value=\"1\"/>\n";
    xml << "      <param name=\"withPHY\" value=\"1\"/>\n";
    xml << "      <param name=\"peak_transfer_rate\" value=\"200\"/>\n";
    xml << "      <param name=\"vdd\" value=\"0\"/>\n";
    xml << "      <param name=\"power_gating_vcc\" value=\"-1\"/>\n";
    xml << "      <stat name=\"duty_cycle\" value=\"0\"/>\n";
    xml << "    </component>\n";

    xml << "  </component>\n";
    xml << "</component>\n";

    return xml.str();
}

} // namespace pimid
