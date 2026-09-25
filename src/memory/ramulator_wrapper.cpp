#include "power/cacti_io_wrapper.h"   // 1.11.40 (N8): harnessed IO model
#include <iostream>
#include "memory/ramulator_wrapper.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <set>
#include <yaml-cpp/yaml.h>

// Include Ramulator headers
#include "base/base.h"
#include "base/request.h"
#include "memory_system/memory_system.h"
#include "base/factory.h"
#include "dram/pimid_org_probe.h"   // 1.11.66: C++17-safe window onto the live IDRAM organization
#include "dram/pimid_energy.h"   // 1.9.10: Ramulator2-resident intensive energy layer

namespace pimid {

RamulatorWrapper::RamulatorWrapper(const std::string& config_path, const std::string& dram_type)
    : config_path_(config_path),
      capacity_(0),
      bandwidth_(0),
      channels_(1),
      ranks_per_channel_(1),
      banks_per_rank_(8),
      total_reads_(0),
      total_writes_(0),
      cached_read_energy_(0.0),
      cached_write_energy_(0.0),
      cached_leakage_power_(0.0),
      last_energy_update_(0),
      current_cycle_(0),
      pim_enabled_(false),
      dram_type_(dram_type),
      dram_arch_(nullptr),
      bandwidth_tracker_(nullptr),
      internal_network_(nullptr),
      pim_plugin_(nullptr) {
    /* 1.11.67: start from the run-wide knobs when main has recorded them, so
     * an instance constructed at a site that never calls applyDramKnobs()
     * still describes the part this run simulates. The setters are used
     * (not raw assignment) so the grade is validated the same way here. */
    if (s_run_knobs_set_) {
        device_width_ = s_run_device_width_;       // before dram_arch_ exists: plain record
        ddr5_grade_mtps_ = s_run_ddr5_grade_mtps_;  // validated once in setRunWideKnobs()
        temperature_k_ = s_run_temperature_k_;
        energy_term_override_pJ_per_bit_ = s_run_termination_pj_per_bit_;
    }
}

bool        RamulatorWrapper::s_run_knobs_set_ = false;
std::string RamulatorWrapper::s_run_device_width_;
int         RamulatorWrapper::s_run_ddr5_grade_mtps_ = 4800;
int         RamulatorWrapper::s_run_temperature_k_ = 358;
double      RamulatorWrapper::s_run_termination_pj_per_bit_ = -1.0;

void RamulatorWrapper::setRunWideKnobs(const std::string& device_width, int ddr5_grade_mtps,
                                       int temperature_k, double termination_pj_per_bit) {
    if (ddr5_grade_mtps != 3200 && ddr5_grade_mtps != 4800 && ddr5_grade_mtps != 5600) {
        // Same refusal as setDdr5SpeedGrade(): the grade names a part this tree holds.
        RamulatorWrapper probe("", "DDR5");
        probe.setDdr5SpeedGrade(ddr5_grade_mtps);   // prints the FATAL and exits 2
    }
    /* 1.11.76 (audit round 6, R6-L1): THE TWO KNOBS IN THIS FUNCTION ARE NOW
     * HELD TO THE SAME STANDARD. The grade above is refused here; the width was
     * recorded raw -- the constructor comment says "plain record" -- and
     * validated only later, in computeHierarchyLatencies(). Any wrapper built
     * in the window between the two took presetWidthBits(w, 0) -> 0 and fell
     * back to x8 silently. The run does exit afterwards, so no wrong number
     * survived; what was wrong is that one function validated one of its two
     * arguments. The per-technology legality (LPDDR5 x16 only, GDDR6 no x4)
     * stays where the technology is known; this is the tech-independent half. */
    if (!device_width.empty() &&
        device_width != "x4" && device_width != "x8" && device_width != "x16") {
        std::cerr << "[mem] FATAL: memory.dram.device_width = '" << device_width
                  << "' is not a JEDEC device width. Supported values: x4, x8, x16."
                  << std::endl;
        std::exit(2);
    }
    s_run_device_width_ = device_width;
    s_run_ddr5_grade_mtps_ = ddr5_grade_mtps;
    s_run_temperature_k_ = temperature_k;
    s_run_termination_pj_per_bit_ = termination_pj_per_bit;
    s_run_knobs_set_ = true;
}

RamulatorWrapper::~RamulatorWrapper() {
    if (ramulator_memory_system_) {
        ramulator_memory_system_->finalize();
    }
}

/* 1.11.61 (rulings R1/R2/R4): THE SIMULATED PRESET'S ORGANISATION, IN ONE
 * PLACE.
 *
 * Every row below is transcribed verbatim from the Ramulator2 org_presets
 * table named beside it -- the same preset string parseConfiguration() writes
 * into the generated YAML and main.cpp's writeRamulatorConfigYaml() writes for
 * the zsim path. The transcription is checked, not trusted: the constructor
 * helper below multiplies banks x rows x cols x DQ back out and refuses any
 * row that does not reproduce its own declared density.
 *
 * Widths matter here. writeRamulatorConfigYaml() selects the org preset by
 * memory.dram.device_width, and rows and bank groups move with it (a
 * DDR4_8Gb_x4 device has 131072 rows where the x8 has 65536), so the table is
 * keyed by width wherever the preset family offers more than one.
 *
 * RESIDUAL, stated: on the wrapper's OWN default path parseConfiguration()
 * still names a fixed x8 preset per technology (the 1.11.59 C018 note says so
 * at setDeviceWidth). This table follows the CONFIGURED width, i.e. the preset
 * the run's timing model actually parses on the zsim path; on the wrapper's
 * oracle path with a non-x8 width set, the wrapper's own generated YAML and
 * this row can therefore describe different presets. That is the pre-existing
 * limitation, not a new one, and every corpus cell ran at x8 where the two
 * agree exactly. */
namespace {

int presetWidthBits(const std::string& device_width, int fallback) {
    if (device_width == "x4")  return 4;
    if (device_width == "x8")  return 8;
    if (device_width == "x16") return 16;
    return fallback;
}

pimid::PresetOrganization makePresetOrg(const char* name, const char* src,
                                        uint64_t density_mb, int dq,
                                        int channels, int banks,
                                        uint64_t rows, uint64_t cols,
                                        bool per_channel,
                                        int bank_groups, int banks_per_group) {
    pimid::PresetOrganization p;
    p.preset_name = name;
    p.preset_source = src;
    p.density_mb = density_mb;
    p.dq_bits = dq;
    p.channels_in_density = channels;
    p.banks_in_density = banks;
    /* 1.11.72: the grouping is transcribed WITH the bank count and checked
     * against it the same way the density is checked against the product:
     * groups x banks-per-group x channels must be the banks. A row that
     * cannot reproduce its own bank count is refused at the point it is
     * written. */
    p.bank_groups = bank_groups;
    p.banks_per_group = banks_per_group;
    if (bank_groups <= 0 || banks_per_group <= 0 ||
        static_cast<long long>(bank_groups) * banks_per_group * channels != banks) {
        std::cerr << "[mem] FATAL: transcribed bank grouping for " << name
                  << " (" << bank_groups << " groups x " << banks_per_group
                  << " banks x " << channels << " channel(s)) does not reproduce its "
                  << banks << " banks. Fix the transcription in ramulator_wrapper.cpp."
                  << std::endl;
        std::exit(2);
    }
    p.rows_per_bank = rows;
    p.cols_per_row = cols;
    p.per_channel_density = per_channel;

    /* The transcription check. Ramulator's own density check is
     * banks x rows x cols x DQ == density (in bits); reproducing it here means
     * a mistyped row is refused at the point it is written rather than
     * believed downstream. */
    const uint64_t bits = static_cast<uint64_t>(banks) * rows * cols *
                          static_cast<uint64_t>(dq);
    const uint64_t declared_bits = density_mb * 1024ULL * 1024ULL * 8ULL;
    p.valid = (bits == declared_bits);
    if (!p.valid) {
        std::cerr << "[mem] WARNING: the transcribed Ramulator org preset '"
                  << name << "' (" << src << ") does not reproduce its own "
                     "density: " << banks << " banks x " << rows << " rows x "
                  << cols << " cols x " << dq << " DQ = " << bits
                  << " bits against a declared " << declared_bits
                  << ". Every density and row count derived from this preset "
                     "in this run is suspect." << std::endl;
    }
    return p;
}

/* 1.11.63 (R6): the timing-row transcription helper, with the same discipline
 * makePresetOrg() has -- the row is CHECKED, not trusted. The check here mirrors
 * the one each Ramulator impl now makes for itself: it derives tCK as
 * `ck_divisor_e6 * 1E6 / rate` (2 for a classic DDR bus, 4 for HBM3 whose fCK
 * is rate/4 per JESD238B.01 Table 92, 8 for LPDDR5) and THROWS if the preset's
 * tCK_ps column does not mirror that. So the column is authoritative upstream,
 * and a disagreement computed here means PIMID's transcription of the row has
 * gone stale -- which is the failure this check exists to catch, the presets
 * having moved under it during this very release. */
pimid::PresetTiming makePresetTiming(const char* name, const char* src,
                                     int rate, int nBL, int nCL, int nRCD,
                                     int nRP, int nRAS, int table_tCK_ps,
                                     int ck_divisor_e6, int nWTR = 0) {
    pimid::PresetTiming t;
    t.preset_name = name;
    t.preset_source = src;
    t.rate_mtps = rate;
    t.nBL = nBL;
    t.nWTR = nWTR;   // 1.11.65
    t.nCL = nCL;
    t.nRCD = nRCD;
    t.nRP = nRP;
    t.nRAS = nRAS;
    t.table_tCK_ps = table_tCK_ps;
    t.ck_divisor_e6 = ck_divisor_e6;
    // The impl's own expression, integer-truncated the same way it is there.
    t.tCK_ps = (rate > 0) ? static_cast<int>(ck_divisor_e6 * 1.0e6 / rate) : 0;
    t.ck_consistent = (t.tCK_ps > 0 && t.tCK_ps == t.table_tCK_ps);
    t.valid = (rate > 0 && nCL > 0 && nRCD > 0 && nRP > 0 && nRAS > 0);
    if (!t.valid) {
        std::cerr << "[mem] WARNING: the transcribed Ramulator timing preset '"
                  << name << "' (" << src << ") is incomplete (rate " << rate
                  << ", nCL " << nCL << ", nRCD " << nRCD << ", nRP " << nRP
                  << ", nRAS " << nRAS << "). No ns timing will be derived from"
                     " it." << std::endl;
    }
    return t;
}

}  // namespace

/* 1.11.63 (R6): THE SIMULATED TIMING PRESET, transcribed once.
 *
 * Rows are verbatim from the timing_presets table of the impl named beside
 * them, and the preset NAME is the one parseConfiguration() writes into the
 * generated YAML and main.cpp's writeRamulatorConfigYaml() writes for the zsim
 * path -- the two agree per technology, checked by reading both.
 *
 * Only the five columns PIMID consumes are carried (rate, nBL, nCL, nRCD, nRP,
 * nRAS) plus the row's own tCK_ps for the consistency check. Transcribing the
 * whole row would be transcribing the timing model, which is not PIMID's job. */
void RamulatorWrapper::resolvePresetTiming() {
    std::string dt = dram_type_;
    std::transform(dt.begin(), dt.end(), dt.begin(), ::toupper);

    if (dt == "DDR3") {
        // DDR3.cpp timing_presets, row "DDR3_1600H": nBL 4, nCL/nRCD/nRP 9,
        // nRAS 28, tCK 1250 ps, tCK = 2E6/rate. (The 1600K row -- nCL 11 -- is
        // the bin PIMID's getTRCD()/getTCAS()/getTRP() used to transcribe; it
        // is NOT this run's.)
        preset_timing_ = makePresetTiming(
            "DDR3_1600H", "external/ramulator/src/dram/impl/DDR3.cpp timing_presets",
            1600, 4, 9, 9, 9, 28, 1250, 2, /*nWTR*/ 6);      // DDR3_1600H nWTR 6
    } else if (dt == "DDR4") {
        // DDR4.cpp, row "DDR4_2400R": nBL 4, nCL/nRCD/nRP 16, nRAS 39, tCK 833,
        // tCK = 2E6/rate.
        preset_timing_ = makePresetTiming(
            "DDR4_2400R", "external/ramulator/src/dram/impl/DDR4.cpp timing_presets",
            2400, 4, 16, 16, 16, 39, 833, 2, /*nWTR*/ 9);     // DDR4_2400R nWTRL 9
    } else if (dt == "DDR5") {
        /* 1.11.66 (R8 #9): three grades, one transcription each, mirroring
         * DDR5.cpp's rows. nWTR is the Max(16nCK, 10ns) term of the tCCD_L_WTR
         * composite at each tCK (T334/T335/T336): 16 / 24 / 28. */
        if (ddr5_grade_mtps_ == 5600) {
            preset_timing_ = makePresetTiming(
                "DDR5_5600B", "external/ramulator/src/dram/impl/DDR5.cpp timing_presets",
                5600, 8, 46, 45, 45, 90, 357, 2, /*nWTR*/ 28);
        } else if (ddr5_grade_mtps_ == 4800) {
            preset_timing_ = makePresetTiming(
                "DDR5_4800B", "external/ramulator/src/dram/impl/DDR5.cpp timing_presets",
                4800, 8, 40, 39, 39, 77, 416, 2, /*nWTR*/ 24);
        } else {
            // DDR5.cpp, row "DDR5_3200AN": nBL 8, nCL/nRCD/nRP 24, nRAS 52, tCK 625
            preset_timing_ = makePresetTiming(
                "DDR5_3200AN", "external/ramulator/src/dram/impl/DDR5.cpp timing_presets",
                3200, 8, 24, 24, 24, 52, 625, 2, /*nWTR*/ 16);
        }
    } else if (dt == "LPDDR5") {
        /* LPDDR5.cpp, row "LPDDR5_6400": nBL16 2, nCL 17, nRCD 15, nRPab 17,
         * nRPpb 15, nRAS 34, tCK 1250 ps, tCK = 8E6/rate. nRP is taken from the
         * PER-BANK column, which is the one PIMID's per-bank hierarchy means.
         * 1.11.63 (calibration): nCL 20 -> 17 tracking the preset -- the old
         * 20 was RL Set 0 of the NEXT frequency bin (JESD209-5C Table 225
         * p.261, row 1011B covers 6000 < rate <= 6400 and gives RL = 17). */
        preset_timing_ = makePresetTiming(
            "LPDDR5_6400", "external/ramulator/src/dram/impl/LPDDR5.cpp timing_presets",
            6400, 2, 17, 15, 15, 34, 1250, 8, /*nWTR*/ 10);   // LPDDR5_6400 nWTRL 10
    } else if (dt == "GDDR6") {
        /* GDDR6.cpp, row "GDDR6_2000_1350mV_double": nBL 2, nCL 24, nRCDRD 26,
         * nRP 26, nRAS 53, tCK 571 ps, tCK = 8E6/rate (rate 14000). 1.11.66:
         * GDDR6 is 8 bits/pin per CK (WCK 4x CK, DDR on WCK); the 1.11.63
         * transcription at 1000 ps / 2E6 mirrored a wrong derivation -- see
         * the row comment in GDDR6.cpp for the proof. nRCD is the READ
         * column; the write column (16) is a separate constraint PIMID does
         * not carry. */
        preset_timing_ = makePresetTiming(
            "GDDR6_2000_1350mV_double",
            "external/ramulator/src/dram/impl/GDDR6.cpp timing_presets",
            14000, 2, 24, 26, 26, 53, 571, 8, /*nWTR*/ 11);   // GDDR6_2000_1350mV_double nWTRL 11 (1.11.66: 8 bits/pin/CK)
    } else if (dt == "HBM2") {
        /* HBM2.cpp, row "HBM2_2.4Gbps": nBL 2, nCL/nRCDRD 20, nRP 18, nRAS
         * 40, tCK 833 ps, tCK = 2E6/rate. 1.11.63 (JESD235D): nBL 4 -> 2 (PC
         * mode BL4 = 4 UI = 2 CK; Tbl 68 p.109 tCCDS = 2 nCK proves the
         * occupancy) and nRP 20 -> 18 (tRP 15 ns, Tbl 58 p.102 -- the old 20
         * encoded a 16 ns claim that appears nowhere in the standard). */
        preset_timing_ = makePresetTiming(
            "HBM2_2.4Gbps", "external/ramulator/src/dram/impl/HBM2.cpp timing_presets",
            2400, 2, 20, 20, 18, 40, 833, 2, /*nWTR*/ 10);    // HBM2_2.4Gbps nWTRL 10
    } else if (dt == "HBM3") {
        /* HBM3.cpp, row "HBM3_6.4Gbps", AS RE-DERIVED INTO THE CK DOMAIN: nBL
         * 2, nCL/nRCDRD/nRP 26, nRAS 53, tCK 625 ps, and tCK = 4E6/rate
         * because JESD238B.01 Table 92 (printed p.160) gives fCK = rate/4 in
         * all nine bins -- the old 2E6/rate form was returning tWDQS, half a
         * CK. The ns values are unchanged by that correction (26 x 0.625 =
         * 16.25 against the previous 52 x 0.312 = 16.22); what moved is the
         * DOMAIN the cycle counts are expressed in, which is why nothing here
         * may assume either the counts or the divisor and both are read from
         * the row that is actually in the tree. */
        preset_timing_ = makePresetTiming(
            "HBM3_6.4Gbps", "external/ramulator/src/dram/impl/HBM3.cpp timing_presets",
            6400, 2, 26, 26, 26, 53, 625, 4, /*nWTR*/ 13);    // HBM3_6.4Gbps nWTRL 13 (CK domain)
    } else {
        // Unknown technology: the DDR4 substitution, announced in initialize().
        preset_timing_ = makePresetTiming(
            "DDR4_2400R",
            "external/ramulator/src/dram/impl/DDR4.cpp timing_presets (substituted)",
            2400, 4, 16, 16, 16, 39, 833, 2, /*nWTR*/ 9);     // DDR4_2400R nWTRL 9
    }
}

void RamulatorWrapper::resolvePresetOrganization() {
    std::string dt = dram_type_;
    std::transform(dt.begin(), dt.end(), dt.begin(), ::toupper);
    const int w = presetWidthBits(device_width_, 0);

    if (dt == "DDR3") {
        /* DDR3.cpp org_presets, 8 Gb rows: {density, DQ, {Ch, Ra, Ba, Ro, Co}}
         *   x4  {1,1,8, 1<<16, 1<<12}   x8 {1,1,8, 1<<16, 1<<11}   x16 {1,1,8, 1<<16, 1<<10}
         * 1.11.66 (R5 A1): this transcription was written in 1.11.61 against
         * the PRE-1.11.63 rows (131072 x 1024 for x8) and was not updated
         * when 1.11.63 corrected DDR3_8Gb_x8 to 65536 x 2048. The density
         * product is identical, so makePresetOrg's check passed, and every
         * DDR3 run built 256 subarrays/bank (should be 128) with a 1 KB page
         * where the part has 2 KB. Rows are 65536 at every 8 Gb width;
         * columns scale 4096/2048/1024 with x4/x8/x16. The shape check in
         * createRamulatorInstance() now refuses this class of drift. */
        const int    ww   = (w > 0) ? w : 8;
        const uint64_t ro = 65536ULL;
        const uint64_t co = (ww == 4) ? 4096ULL : (ww == 16) ? 1024ULL : 2048ULL;
        preset_org_ = makePresetOrg(
            (std::string("DDR3_8Gb_x") + std::to_string(ww)).c_str(),
            "external/ramulator/src/dram/impl/DDR3.cpp org_presets",
            1024, ww, 1, 8, ro, co, false,
            /* JESD79-3D 2.11: 8 banks, NO bank groups */ 1, 8);
    } else if (dt == "DDR4") {
        // DDR4.cpp:19-21  8 Gb rows: x4 {4 BG, 4 Ba, 1<<17, 1<<10},
        // x8 {4, 4, 1<<16, 1<<10}, x16 {2, 4, 1<<16, 1<<10}
        const int ww = (w > 0) ? w : 8;
        const int banks = (ww == 16) ? 8 : 16;
        const uint64_t ro = (ww == 4) ? 131072ULL : 65536ULL;
        preset_org_ = makePresetOrg(
            (std::string("DDR4_8Gb_x") + std::to_string(ww)).c_str(),
            "external/ramulator/src/dram/impl/DDR4.cpp org_presets",
            1024, ww, 1, banks, ro, 1024, false,
            /* JESD79-4: x4/x8 4 BG x 4; x16 2 BG x 4 */ (ww == 16) ? 2 : 4, 4);
    } else if (dt == "DDR5") {
        /* DDR5.cpp org_presets. 8 Gb rows: x4 {8 BG, 2 Ba, 1<<16, 1<<11},
         * x8 {8, 2, 1<<16, 1<<10}, x16 {4, 2, 1<<16, 1<<10}. 16 Gb rows: x4
         * {8, 4, 1<<16, 1<<11}, x8 {8, 4, 1<<16, 1<<10}, x16 {4, 4, 1<<16,
         * 1<<10} -- JESD79-5D Tables 4/5 p.7: the step from 8 to 16 Gb is
         * BA0 -> BA0~BA1 (2 -> 4 banks per group), rows unchanged.
         * 1.11.66 (R8 #9): the 4800 and 5600 grades are the 16 Gb Micron
         * MT60B die (both addenda are 16 Gb parts), so the org follows the
         * grade; 3200 keeps the 8 Gb part. The shape check binds each to the
         * device Ramulator instantiates. */
        const int ww = (w > 0) ? w : 8;
        const bool g16 = (ddr5_grade_mtps_ != 3200);
        const int bpg = g16 ? 4 : 2;
        const int banks = ((ww == 16) ? 4 : 8) * bpg;
        const uint64_t co = (ww == 4) ? 2048ULL : 1024ULL;
        preset_org_ = makePresetOrg(
            (std::string(g16 ? "DDR5_16Gb_x" : "DDR5_8Gb_x") + std::to_string(ww)).c_str(),
            "external/ramulator/src/dram/impl/DDR5.cpp org_presets",
            g16 ? 2048 : 1024, ww, 1, banks, 65536, co, false,
            /* JESD79-5D Tbl 4: x4/x8 8 BG, x16 4 BG; 16 Gb 4 banks/BG, 8 Gb 2 */
            (ww == 16) ? 4 : 8, bpg);
    } else if (dt == "LPDDR5") {
        // LPDDR5.cpp:21-27 -- x16 only. {1, 1, 4 BG, 4 Ba, 1<<15, 1<<10}
        preset_org_ = makePresetOrg(
            "LPDDR5_8Gb_x16",
            "external/ramulator/src/dram/impl/LPDDR5.cpp org_presets",
            1024, 16, 1, 16, 32768, 1024, false,
            /* JESD209-5C Tbl 6 BG mode: 4 BG x 4 */ 4, 4);
    } else if (dt == "GDDR6") {
        /* GDDR6.cpp:21-28. The density product INCLUDES the channel level
         * ({2, 4 BG, 4 Ba, Ro, Co}), so 8 Gb is the whole two-channel DEVICE
         * and the 32 banks are the device's, 16 per channel. */
        const int ww = (w > 0) ? w : 16;
        const uint64_t co = (ww == 8) ? 2048ULL : 1024ULL;
        preset_org_ = makePresetOrg(
            (std::string("GDDR6_8Gb_x") + std::to_string(ww)).c_str(),
            "external/ramulator/src/dram/impl/GDDR6.cpp org_presets",
            1024, ww, 2, 32, 16384, co, false,
            /* JESD250D Tbl 19, PER CHANNEL: 4 BG x 4 (x2 channels = 32) */ 4, 4);
    } else if (dt == "HBM2") {
        /* HBM2.cpp org_presets. density is PER CHANNEL ("channel density" in
         * the file's own error text). No device width applies.
         * 1.11.64 (JESD235D Tbl 4 p.6): {1 Ch, 2 Pch, 4 Bg, 4 Ba} = 32 banks
         * per channel (16 per pseudo-channel) over 16384 rows, tracking the
         * org-preset correction in HBM2.cpp -- the old transcription (16
         * banks/channel, 32768 rows) mirrored the pre-1.11.64 preset, which
         * carried half the banks and twice the rows at this density. This
         * value feeds getPresetRowsPerBank(), which is the SOLE authority for
         * subarrays_per_bank, the in-memory tree shape, the tree-coverage
         * assertion and pages_per_unit (ruling R4) -- so it must track the
         * preset or those four quantities describe a different part. */
        preset_org_ = makePresetOrg(
            "HBM2_4Gb",
            "external/ramulator/src/dram/impl/HBM2.cpp org_presets",
            512, 128, 1, 32, 16384, 64, true,
            /* 2 pseudo-channels x 4 BG folded = 8 groups x 4 banks per channel */ 8, 4);
    } else if (dt == "HBM3") {
        // HBM3.cpp:21-26. Per-channel density; 2 Pch x 4 Bg x 4 Ba = 32 banks.
        preset_org_ = makePresetOrg(
            "HBM3_4Gb",
            "external/ramulator/src/dram/impl/HBM3.cpp org_presets",
            512, 128, 1, 32, 16384, 64, true,
            /* 2 pseudo-channels x 4 BG folded = 8 groups x 4 banks per channel */ 8, 4);
    } else {
        /* Unknown technology: the same DDR4 substitution the architecture
         * object makes a few lines below, which announces itself there. */
        const int ww = (w > 0) ? w : 8;
        const int banks = (ww == 16) ? 8 : 16;
        const uint64_t ro = (ww == 4) ? 131072ULL : 65536ULL;
        preset_org_ = makePresetOrg(
            (std::string("DDR4_8Gb_x") + std::to_string(ww)).c_str(),
            "external/ramulator/src/dram/impl/DDR4.cpp org_presets (substituted)",
            1024, ww, 1, banks, ro, 1024, false,
            (ww == 16) ? 2 : 4, 4);
    }
}

uint64_t RamulatorWrapper::getPresetDeviceCapacityMB() const {
    if (!preset_org_.valid) return 0;
    if (!preset_org_.per_channel_density) return preset_org_.density_mb;
    // HBM: the stack is the device, and its channel count is PIMID's.
    const uint64_t nch = (channels_ > 0) ? channels_ : 1;
    return preset_org_.density_mb * nch;
}

int RamulatorWrapper::getPresetDiesPerStack() const {
    if (!preset_org_.per_channel_density) return 0;
    const int nch = (channels_ > 0) ? static_cast<int>(channels_) : 1;
    return (nch >= 2) ? nch / 2 : 1;   // two channels per core die
}

/* 1.11.61 (ruling R1): THE DDR-FAMILY DENSITY FOLLOWS THE PRESET, INCLUDING
 * THE THREE TECHNOLOGIES THAT HAVE NO OBJECT OF THEIR OWN.
 *
 * DDR3, LPDDR5 and GDDR6 read DDR4-2400's architecture object (see the
 * substitution notice in initialize()), so a literal in the DDR4 factory can
 * describe at most one of the four DDR-family parts. Their presets differ in
 * exactly the field that matters here -- DDR3 spreads its 8 Gb over 8 banks
 * (128 MB/bank), DDR4/DDR5/LPDDR5 over 16 (64 MB) and GDDR6 over 32 (32 MB) --
 * so the density is stamped from the preset of the technology ACTUALLY ASKED
 * FOR, after the object is built. At DDR4 and DDR5 this reproduces the factory
 * literals exactly, which is the check that the two agree.
 *
 * 1.11.63 (R6-1, R6-2): HBM IS NO LONGER EXCLUDED -- it is DERIVED ON ITS OWN
 * UNIT instead.
 *
 * Ruling R2 (1.11.61) kept HBM's chip_size_mb out of this function because
 * "follow the preset" read naively means "take the per-channel density", which
 * would have halved HBM2's die away from the Sohn ISSCC-2016 core die it lands
 * on exactly. That was the right call for the value and the wrong shape for the
 * source: it left two HBM literals (HBM2 1024 MB, HBM3 2048 MB) paraphrasing a
 * preset, which is what R6 forbids. The unit is the fix, not the exclusion:
 *
 *   chip_size_mb(HBM) = the preset's PER-CHANNEL density x the channels one
 *                       CORE DIE fronts (getPresetDiesPerStack()'s own
 *                       two-channels-per-die relation, so there is one
 *                       authority for it and not two).
 *
 * HBM2 reproduces its factory literal exactly under that derivation
 * (512 MB/channel x 2 = 1024 MB = the 8 Gb Sohn core die), which is the check
 * that the unit is right. HBM3 does NOT reproduce its 2048: the derivation
 * gives 1024 MB, and 1024 x 8 dies = 8 GiB = the preset's stack. That is R6
 * resolving the three-authority disagreement 1.11.61 recorded and left open --
 * the object follows the preset, and the capacity cross-check below is
 * therefore expected to RECONCILE on HBM3 from this release on.
 *
 * R6-2: bank_size_mb is stamped for HBM too. Its 4 MB described nothing this
 * tree simulates (the preset's bank is 512/16 = 32 MB on HBM2 and 512/32 =
 * 16 MB on HBM3), and it is the single hottest density consumer -- HBM
 * pages_per_unit moves 8x and 4x with it.
 *
 * rank_size_gb is stamped as well, since the struct declares it DERIVED from
 * chip_size_mb: the DDR family multiplies by chips_per_rank (devices), HBM by
 * the CHANNEL count against the per-channel density, which is the stack. It is
 * a dead field (no getter, no reader) and is corrected only so that a reader of
 * the object is not handed a fourth capacity.
 *
 * subarrays_per_bank and subarray_size_kb are still left alone: both are dead
 * here (the live subarray count is main.cpp's bank_rows / subarray_height), and
 * a 512 KB subarray is DDR4's row geometry, not LPDDR5's or GDDR6's, so
 * deriving them here would invent a number. */
/* 1.11.72: THE BANK GROUPING FOLLOWS THE PRESET, like the density (R1).
 *
 * The defect: 1.11.66 made the DDR5 default part 4800B / 16 Gb, whose preset
 * (DDR5_16Gb_x8, JESD79-5D Table 4) has 8 bank groups x 4 banks = 32 banks
 * per chip. The transcription followed (bpg = g16 ? 4 : 2, shape-checked
 * live on every run) and so did the timed device. The architecture object
 * did not: its literals still said 2 banks per group -- the 8 Gb part -- and
 * three consumers read the object rather than the transcription:
 * getBanksPerBankGroup()/getBankGroupsPerChip() (the power population and
 * the system-scope placement, main.cpp ~8053/~8877/~9030) and, separately,
 * main.cpp's own per-technology table (the placement tree). So the tree
 * covered 128 bank organisations where the simulated part has 256, and the
 * coverage invariant could not see it because both of its sides came from
 * the same table. Found 2026-09-20 while tabulating the hierarchy under the
 * bank; measured +2.7% cycles on the corrected tree (3 x 3 A/B, DDR4 BANK
 * 100k). Bank SIZE was coincidentally right (16 Gb / 32 = 64 MB = 65536 rows
 * x 1 KB), so only the COUNT and what derives from it were wrong.
 *
 * The fix is the R1 pattern: the preset row is the authority, and the object
 * is stamped from the transcription the shape check already verifies against
 * Ramulator. Every technology is stamped; for six of seven the object already
 * agreed and nothing moves. */
void RamulatorWrapper::applyPresetBankGroupingToArchitecture() {
    if (!dram_arch_ || !preset_org_.valid) return;
    if (preset_org_.bank_groups <= 0 || preset_org_.banks_per_group <= 0) return;
    const int old_bg = dram_arch_->organization.bank_groups_per_chip;
    const int old_bp = dram_arch_->organization.banks_per_bank_group;
    dram_arch_->organization.bank_groups_per_chip = preset_org_.bank_groups;
    dram_arch_->organization.banks_per_bank_group = preset_org_.banks_per_group;
    if ((old_bg != preset_org_.bank_groups || old_bp != preset_org_.banks_per_group)
        && !anchor_quiet_) {
        static std::set<std::string> said;
        if (said.insert(dram_type_ + preset_org_.preset_name).second) {
            std::cerr << "[mem] NOTE: " << dram_type_ << " architecture object bank grouping "
                      << old_bg << " groups x " << old_bp << " banks -> "
                      << preset_org_.bank_groups << " x " << preset_org_.banks_per_group
                      << ", stamped from preset " << preset_org_.preset_name
                      << " (the object literal described a different part)."
                      << std::endl;
        }
    }
}

void RamulatorWrapper::applyPresetDensityToArchitecture() {
    if (!dram_arch_ || !preset_org_.valid) return;
    std::string dt = dram_type_;
    std::transform(dt.begin(), dt.end(), dt.begin(), ::toupper);

    const uint64_t bank_mb = preset_org_.bankSizeMB();
    uint64_t chip_mb = 0;
    uint64_t rank_gb = 0;
    if (preset_org_.per_channel_density) {
        const int nch  = (channels_ > 0) ? static_cast<int>(channels_) : 1;
        const int dies = getPresetDiesPerStack();
        const int ch_per_die = (dies > 0) ? (nch / dies) : 1;  // 2, by that relation
        chip_mb = preset_org_.density_mb * static_cast<uint64_t>(ch_per_die > 0 ? ch_per_die : 1);
        rank_gb = preset_org_.density_mb * static_cast<uint64_t>(nch) / 1024ULL;
    } else {
        chip_mb = preset_org_.density_mb;
        const uint64_t cpr = dram_arch_->organization.chips_per_rank > 0
                                 ? static_cast<uint64_t>(dram_arch_->organization.chips_per_rank)
                                 : 1ULL;
        rank_gb = chip_mb * cpr / 1024ULL;
    }
    if (chip_mb == 0 || bank_mb == 0) return;

    const uint64_t old_chip_mb = dram_arch_->organization.chip_size_mb;
    const uint64_t old_bank_mb = dram_arch_->organization.bank_size_mb;
    const bool moved = (old_chip_mb != chip_mb) || (old_bank_mb != bank_mb);
    dram_arch_->organization.chip_size_mb = chip_mb;
    dram_arch_->organization.bank_size_mb = bank_mb;
    if (rank_gb > 0) dram_arch_->organization.rank_size_gb = rank_gb;

    /* 1.11.77 (audit round 6, R6-9): ANNOUNCE THE OVERRIDE, LIKE ITS SIBLING.
     *
     * This read: if moved AND dt is not DDR4, not DDR5 and not HBM -- and then
     * said "<dt> has no architecture object, but its DENSITY no longer
     * describes DDR4-2400's ... every other field on the object is still
     * DDR4-2400's". Both halves are now false. DDR3, LPDDR5 and GDDR6 have
     * owned objects since 1.11.68/69/70, so nothing reads DDR4's any more, and
     * the exclusion list was written when they did. Worse, the exclusion hid
     * the one place the stamp actually moves something: at the default grade
     * DDR5 simulates the 16 Gb part while its object literal says 8 Gb, so
     * every default DDR5 run silently re-stamps 1024 -> 2048 MB. Its sibling,
     * applyPresetBankGroupingToArchitecture(), announces exactly that kind of
     * override (1.11.72) -- two stamps in one file, one speaking and one not.
     *
     * The NOTE is now about what it is: the preset overriding an object
     * literal, for whichever technology it happens to. Silence means the
     * literal already agreed. */
    if (moved) {
        static std::set<std::string> said_density;
        if (!anchor_quiet_ && said_density.insert(dt + preset_org_.preset_name).second) {
            std::cerr << "[mem] NOTE: " << dt << " architecture object density "
                      << old_chip_mb << " MB device / " << old_bank_mb
                      << " MB per bank -> " << chip_mb << " MB / " << bank_mb
                      << " MB, stamped from preset " << preset_org_.preset_name
                      << " (" << preset_org_.banks_in_density
                      << " banks; the object literal described a different part)."
                      << std::endl;
        }
    }
}

/* 1.11.63 (R6-5): THE OBJECT'S ns TIMINGS FOLLOW THE PRESET'S BIN.
 *
 * 1.11.56 moved clock_freq_mhz, data_rate_mtps and tBurst_ns to the simulated
 * preset and said "the ns timings are absolute and stay". Absolute they are --
 * but they are absolute values OF A SPEED BIN, and two technologies were
 * carrying a different bin's:
 *
 *   DDR5  object 16.67 ns tRCD/tCAS/tRP -- the 4800 bin -- against
 *         DDR5_3200AN's nCL/nRCD/nRP 24 at tCK 625 ps = 15.00 ns. 11% high.
 *   DDR3  the wrapper's own getTRCD()/getTCAS()/getTRP() returned 13.75 ns,
 *         transcribed from DDR3-1600K (nCL 11), against the DDR3_1600H preset
 *         this tree simulates (nCL 9 at tCK 1250 ps = 11.25 ns). 22% high, and
 *         it is the same one-part-two-bins defect D002/1.11.56 closed
 *         elsewhere.
 *
 * These are LIVE: tRP and tRAS compose getTRC(), which prices the array
 * ACTIVATE energy, and tRP+tRCD+tCAS is bank_access_ns.
 *
 * WHAT IS DERIVED: n x tCK, from the transcribed preset row, for all four
 * timings together -- the same quantity the timing model enforces. tCK is the
 * clock Ramulator derives (1E6/(rate/2)), not the row's decorative column.
 *
 * DDR4 IS INCLUDED AND MOVES SLIGHTLY, which is stated rather than special-
 * cased away: its literals 13.32/13.32/13.32/32.0 become 13.328 (16 x 833 ps,
 * a transcription rounding) and 32.487 (39 x 833 ps). The tRAS step is
 * JEDEC_rounding: the preset holds ceil(32 ns / 833 ps) = 39 whole cycles, so
 * 32.487 ns is what the model actually keeps the row open for, and 32.0 was the
 * un-quantised spec minimum. Exempting DDR4 because its literal was CLOSE would
 * leave exactly the second authority R6 exists to remove.
 *
 * EXCLUDED -- FOUR TECHNOLOGIES, AND THE REASON IS SCOPE, NOT INABILITY. This
 * is stated plainly because an earlier draft of this note gave a technical
 * reason that stopped being true while it was being written: LPDDR5's and
 * GDDR6's rows used to carry two different clocks (tCK column against the
 * 1E6/(rate/2) the impls then enforced), which would have made "nCL in ns"
 * unanswerable. The concurrent CK-domain correction in external/ramulator
 * repaired exactly that -- every impl now derives tCK as its own per-family
 * ck_divisor_e6 * 1E6 / rate AND throws if the column does not mirror it -- so
 * all seven rows are now internally consistent and PresetTiming::derivable()
 * is true for all seven. Nothing technical prevents stamping the other four.
 *
 * What prevents it is that ruling R6 item 5 names DDR3 and DDR5, and each of
 * the four is a large, separate, re-simulation-forcing move that deserves its
 * own decision. They are quantified here so they are not mistaken for closed
 * (preset ns against the value getTRCD()/getTRP()/getTRAS() returns today):
 *
 *   LPDDR5  tCAS 25.00 vs 18.00 (+39%), tRCD/tRPpb 18.75 vs 18.00 (+4%),
 *           tRAS 42.50 vs 42.00 (+1%)
 *   GDDR6   tCAS 24.00 vs 14.80 (+62%), tRCDRD/tRP 26.00 vs 14.80 (+76%),
 *           tRAS 53.00 vs 28.00 (+89%) -- but note the GDDR6 preset is a
 *           2000 MT/s bin while PIMID models the part at 14000 MT/s, which
 *           sayPresetRate() already announces every run; "follow the preset"
 *           here means following a much slower part's timings, and that is a
 *           judgement, not arithmetic.
 *   HBM2    tRP 16.66 vs 12.50 (+33%), tRAS 33.32 vs 28.00 (+19%),
 *           tCAS/tRCD 16.66 vs 16.00 (+4%)
 *   HBM3    tRP 16.25 vs 10.00 (+63%), tRAS 33.13 vs 24.00 (+38%),
 *           tCAS/tRCD 16.25 vs 16.00 (+2%)
 * tRP and tRAS compose getTRC(), so the HBM rows price their array ACTIVATE
 * energy on a tRC roughly a third below the preset's. These are the drifts the
 * density investigation found (section 6.1) and they stay OPEN. */
void RamulatorWrapper::applyPresetTimingsToArchitecture() {
    if (!dram_arch_) return;
    std::string dt = dram_type_;
    std::transform(dt.begin(), dt.end(), dt.begin(), ::toupper);
    /* 1.11.66 (round 5, A5/F8/F9 under ruling R8 "fix all"): the stamp
     * reaches EVERY technology that owns an architecture object -- DDR3/4/5
     * as before, and HBM2/HBM3, which the note above quantified and left
     * OPEN pending a decision. The decision is R8.
     * The HBM literals this replaces: HBM2 tRP 12.5 / tRAS 28.0 against the
     * preset's 15.0 / 33.3 ns; HBM3 tRP 10.0 / tRAS 24.0 against 16.25 /
     * 33.1 -- activate energy moves HBM2 +19%, HBM3 +47%.
     *
     * 1.11.77 (audit round 6, R6-8): LPDDR5 AND GDDR6 ARE ON THE LIST NOW.
     * This block used to end "LPDDR5 and GDDR6 own no object (they borrow
     * DDR4's as an organization proxy) and are served by the per-tech
     * getters" -- true when it was written, and false from 1.11.69 and
     * 1.11.70, which gave each of them its own object. The list was never
     * extended, so those two were the only objects in the tree whose timing
     * literals were never checked against the preset they claim to describe,
     * while the `owns_object` line a few lines below already named them --
     * one function disagreeing with itself about which technologies own an
     * object. The getters still short-circuit to preset_timing_ for these
     * two, which is why no number was wrong; stamping makes the object agree
     * with the getter instead of merely happening to. Measured: extending the
     * list changes nothing on any of the seven technologies, byte for byte,
     * which is the proof that the literals were right AND the reason it is
     * safe to protect them. */
    const bool ruled = (dt == "DDR3" || dt == "DDR4" || dt == "DDR5" ||
                        dt == "HBM2" || dt == "HBM3" ||
                        dt == "LPDDR5" || dt == "GDDR6");
    if (!ruled) return;

    /* THE STALENESS GUARD. Upstream now throws a ConfigurationError if a
     * preset's tCK_ps column does not mirror its own derivation, so the column
     * is authoritative and the two can only disagree HERE -- meaning PIMID's
     * transcription of the row has fallen behind a preset that moved. That is
     * a real failure mode and not a hypothetical: this transcription and the
     * HBM3/GDDR6 CK-domain correction were written the same afternoon. Refuse
     * to read the row as ns rather than stamping a stale bin silently. */
    if (!preset_timing_.derivable()) {
        static std::set<std::string> refused;
        if (!anchor_quiet_ && refused.insert(dt).second) {
            std::cerr << "[mem] WARNING: PIMID's transcription of timing preset "
                      << preset_timing_.preset_name << " ("
                      << preset_timing_.preset_source
                      << ") is STALE or wrong: it records a tCK_ps column of "
                      << preset_timing_.table_tCK_ps
                      << " ps, but the impl's own derivation ("
                      << preset_timing_.ck_divisor_e6
                      << "E6 / rate) gives " << preset_timing_.tCK_ps
                      << " ps at " << preset_timing_.rate_mtps
                      << " MT/s. Upstream refuses a preset whose column does "
                         "not mirror that derivation, so the disagreement is on "
                         "PIMID's side. The " << dt
                      << " architecture object keeps the ns timings it was "
                         "written with, which may describe another speed bin -- "
                         "re-transcribe resolvePresetTiming() against the row "
                         "actually in the tree." << std::endl;
        }
        return;
    }

    /* 1.11.77 (audit round 6, R6-9): THE THIRD STAMP SPEAKS TOO.
     *
     * Three stamps write preset facts over this object's literals -- density,
     * bank grouping and these four times. Only the grouping announced it
     * (1.11.72), and that release exists because a silent override let an
     * object literal describe a different part for six releases without
     * anyone seeing it. This one is silent in the same way and on the same
     * technology: DDR5's object carries 24 x 0.625 = 15.0 ns, the 3200AN bin,
     * while a default run simulates 4800B and is stamped to 16.224 ns. The
     * stamp is right -- the preset is the authority (R1/R6) -- but a reader
     * comparing the object against the run could not tell it had happened. */
    const double old_trcd = dram_arch_->timing.tRCD_ns;
    const double old_tcas = dram_arch_->timing.tCAS_ns;
    const double old_trp  = dram_arch_->timing.tRP_ns;
    const double old_tras = dram_arch_->timing.tRAS_ns;
    dram_arch_->timing.tRCD_ns = preset_timing_.tRCD_ns();
    dram_arch_->timing.tCAS_ns = preset_timing_.tCAS_ns();
    dram_arch_->timing.tRP_ns  = preset_timing_.tRP_ns();
    dram_arch_->timing.tRAS_ns = preset_timing_.tRAS_ns();
    {
        auto moved1 = [](double a, double b) { return (a > b ? a - b : b - a) > 1e-9; };
        const bool t_moved = moved1(old_trcd, dram_arch_->timing.tRCD_ns) ||
                             moved1(old_tcas, dram_arch_->timing.tCAS_ns) ||
                             moved1(old_trp,  dram_arch_->timing.tRP_ns)  ||
                             moved1(old_tras, dram_arch_->timing.tRAS_ns);
        static std::set<std::string> said_timing;
        if (t_moved && !anchor_quiet_ &&
            said_timing.insert(dt + preset_timing_.preset_name).second) {
            std::cerr << "[mem] NOTE: " << dt << " architecture object timings "
                      << old_trcd << "/" << old_tcas << "/" << old_trp << "/" << old_tras
                      << " -> " << dram_arch_->timing.tRCD_ns << "/"
                      << dram_arch_->timing.tCAS_ns << "/" << dram_arch_->timing.tRP_ns
                      << "/" << dram_arch_->timing.tRAS_ns
                      << " ns (tRCD/tCAS/tRP/tRAS), stamped from preset "
                      << preset_timing_.preset_name
                      << " (the object literal described a different speed bin)."
                      << std::endl;
        }
    }
    /* 1.11.66 (A5/F8): THE CLOCK AND THE BURST ARE STAMPED TOO. clock_freq_mhz
     * is the CK the ladder rungs and getSubarrayBandwidth() multiply widths
     * by, and HBM3's object carried 3200 -- rate/2, the convention both HBM3
     * makers' silicon papers refute (Ryu p.1052, Park p.259: tCK = rate/4;
     * JESD238B.01 Table 92: fCK 1600 MHz at 6.4 Gbps). The 1.11.63 CK-domain
     * correction reached the preset and the transcription but not this
     * field, so HBM3's inner ladder tiers ran 2x fast and, through the L0
     * reference anchor, touched every technology's Garnet latencies. It is
     * now 1000 / tCK_ps of the transcribed row for every stamped technology
     * (1200 DDR3-1600, 1200 DDR4-2400, 1600 DDR5-3200, 1200 HBM2-2.4,
     * 1600 HBM3-6.4). tBurst follows the row's nBL x tCK: HBM2's object said
     * 8 beats = 3.33 ns where the preset's nBL 2 CK = 1.67 ns for a 64 B
     * access on a 128-bit channel (F9: burst energy was 2x). */
    dram_arch_->timing.clock_freq_mhz = 1000.0 / preset_timing_.tCK_ns();
    if (preset_timing_.nBL > 0)
        dram_arch_->timing.tBurst_ns = preset_timing_.nBL * preset_timing_.tCK_ns();
    /* 1.11.67 (re-sim pre-flight): THE DATA RATE IS STAMPED TOO, for the
     * technologies that OWN their object. 1.11.66 made the DDR5 part a
     * setting (3200AN / 4800B / 5600B) and moved the default to 4800B, but
     * this field kept the factory literal 3200, so the reconciliation check
     * in initialize() failed on every default DDR5 run (38400 vs 25600 MB/s)
     * and the per-level link ladder fell back to the placeholder table --
     * the D002 defect in its 1.11.56 form, reintroduced by the knob that was
     * meant to close it. The rate is the preset's own `rate` column, the same
     * number modelledRateMTs() prices termination from, so cycles, ladder and
     * energy describe one part again at every grade.
     *
     * DDR3 is deliberately NOT in this set although it is in `ruled`: it
     * reads DDR4's object as an organisation proxy (1.11.57 B001), and
     * stamping DDR3-1600's rate onto that proxy would make the check pass and
     * DDR4's ladder be adopted under a DDR3 provenance line -- exactly the
     * false claim 1.11.57 refused. Its ns timings are stamped (energy reads
     * them); its ladder stays declared placeholder until it has an object. */
    /* 1.11.68: DDR3 JOINS THE SET. The exclusion note below was written in
     * 1.11.67, when DDR3 read DDR4's object as a proxy and stamping its rate
     * would have made the reconciliation check pass and DDR4's ladder be
     * adopted under a DDR3 provenance line. DDR3 now has its own object
     * (createDDR3_1600_Verified), so stamping its rate describes the part it
     * actually simulates, which is the whole point of the check. */
    /* 1.11.70: ALL SEVEN TECHNOLOGIES NOW OWN AN OBJECT. The proxy branch in
     * the factory below is unreachable for the shipped lineup and stays only
     * as the announced fallback for a technology added without one. */
    const bool owns_object = (dt == "DDR3" || dt == "DDR4" || dt == "DDR5" ||
                              dt == "LPDDR5" || dt == "GDDR6" ||
                              dt == "HBM2" || dt == "HBM3");
    if (owns_object && preset_timing_.rate_mtps > 0)
        dram_arch_->timing.data_rate_mtps = preset_timing_.rate_mtps;
    // The two hierarchical times declare themselves as sums of the four above.
    dram_arch_->deriveHierarchicalAccessTimes();
}

/* 1.11.63 (R6-3): capacity_ AND bandwidth_ ARE DERIVED FROM THE PRESET PAIR.
 *
 * parseConfiguration()'s else-if chain carried a literal capacity and, for
 * three technologies, a literal bandwidth beside each preset name:
 *
 *   capacity_   8 GiB for every DDR-family technology and 4 GiB for both HBM
 *               stacks -- one number stated seven times, correct for the three
 *               that happen to build a 64-bit rank out of eight 8 Gb devices
 *               and wrong for the rest. An LPDDR5 run claimed 8 GiB from ONE
 *               x16 die (the same run reports 21.99 mm^2 of DRAM silicon, one
 *               8 Gb die's worth), a GDDR6 run 8 GiB from one two-channel
 *               device, and HBM3 claimed 4 GiB against a preset whose 16
 *               channels x 4 Gb is 8 GiB -- the third of the three disagreeing
 *               HBM3 authorities 1.11.61 recorded, and the one R6 says has no
 *               standing.
 *   bandwidth_  DDR4 19200, HBM2 307000, HBM3 819000 -- rate x width x channels
 *               written out by hand, with the arithmetic in a comment beside it.
 *
 * DERIVATIONS, both from primitives that already exist:
 *
 *   capacity_  = the preset's density x the POPULATION of the units that
 *               density describes. The population is not invented here: it is
 *               getBackgroundUnits(), the same devices-per-rank x ranks x
 *               channels (HBM: channels) basis 1.11.52 (A015) made the memory
 *               system's background power and its die AREA share, so Capacity,
 *               Power and Area now describe one memory system. The GDDR6
 *               correction is the preset's own channels_in_density: its density
 *               product spans BOTH channels, so dividing by it keeps the channel
 *               dimension from being booked twice -- the same trap ruling R3
 *               closed on the bandwidth side.
 *   bandwidth_ = dramRateMTs x dramChannelWidthBits x channels, for ALL seven,
 *               which is what derivedBwMBs() already did for four of them.
 *               DDR4's literal is reproduced EXACTLY (2400 x 64/8 = 19200),
 *               which is the check on extending it; HBM2 and HBM3 move by
 *               0.07% and 0.02%, being the rounding their literals carried.
 *
 * WHY THE RATE TABLE AND NOT THE TIMING PRESET'S OWN `rate` COLUMN: for six of
 * the seven they are the same number, because 1.11.52 (D002) and 1.11.57 (C001)
 * moved that table onto the simulated presets. GDDR6 WAS the exception -- its
 * rate column read 2000 (a CK-class label, not a data rate) until 1.11.66
 * corrected it to the 14000 MT/s pin rate, so all seven now agree; the note
 * that announced the divergence stays armed for a future swap. Using the rate
 * table also
 * keeps bandwidth_, the burst time and the termination energy on ONE rate,
 * which is the invariant D002 exists to hold. */
void RamulatorWrapper::derivePresetCapacityAndBandwidth() {
    if (!preset_org_.valid) return;

    const uint32_t nch = (channels_ > 0) ? channels_ : 1;

    // ---- bandwidth_ -------------------------------------------------------
    const double rate_mts = modelledRateMTs();
    const int    ch_bits  = PIMID::CactiIOWrapper::dramChannelWidthBits(dram_type_);
    if (rate_mts > 0.0 && ch_bits > 0) {
        bandwidth_ = static_cast<uint64_t>(rate_mts * (ch_bits / 8.0) * nch);
    } else if (!anchor_quiet_) {
        static std::set<std::string> bw_refused;
        if (bw_refused.insert(dram_type_).second) {
            std::cerr << "[mem] WARNING: no rate/channel-width row for '"
                      << dram_type_ << "', so its aggregate bandwidth cannot be "
                         "derived. bandwidth_ keeps whatever the default path "
                         "left it at." << std::endl;
        }
    }

    // ---- capacity_ --------------------------------------------------------
    const int units = getBackgroundUnits(device_width_,
                                         static_cast<int>(ranks_per_channel_),
                                         static_cast<int>(nch));
    const int cid = preset_org_.channels_in_density > 0
                        ? preset_org_.channels_in_density : 1;
    if (units > 0) {
        const uint64_t mb = preset_org_.density_mb *
                            static_cast<uint64_t>(units) /
                            static_cast<uint64_t>(cid);
        if (mb > 0) capacity_ = mb * 1024ULL * 1024ULL;
    }
}

void RamulatorWrapper::initialize() {
    parseConfiguration();

    /* 1.11.61 (rulings R1/R2/R4): resolve the simulated preset's organisation
     * BEFORE the architecture object is built, so the density stamp and the
     * capacity cross-check below both have it, and so main.cpp's bank_rows
     * derivation can read it off a wrapper that was only initialized. */
    resolvePresetOrganization();
    /* 1.11.63 (R6): and its timing row, for the ns stamp below. Both are
     * already resolved by parseConfiguration(); repeated here because this
     * function must not depend on that ordering. */
    resolvePresetTiming();

    // Only create a full Ramulator2 instance when a config file is provided
    // (for cycle-accurate simulation). Default configs (empty path) are used
    // as parameter oracles -- timing/power from DRAMArchitectureV2 suffices.
    if (!config_path_.empty()) {
        createRamulatorInstance();
    }
    checkTranscribedOrganizationShape();   // 1.11.66 (A6): every wrapper, not just co-sim

    // Auto-populate DRAM architecture for timing/power queries.
    // This ensures getTRCD(), getTCAS(), energy methods etc. return
    // calibrated values even without calling enablePIMSupport().
    /* 1.11.57 (audit C004): the SUBSTITUTION SAYS SO.
     *
     * Only DDR4, DDR5, HBM2 and HBM3 have an architecture object. DDR3,
     * LPDDR5 and GDDR6 fall into the else and are handed DDR4-2400's -- a
     * different part, a different speed bin, a different channel width -- and
     * the else said nothing. Every caller then read timings, widths and
     * bandwidths off an object describing DDR4 while believing it described
     * the technology it asked for; main.cpp's ladder printed exactly that, as
     * "the ladder from the GDDR6 architecture object".
     *
     * The adoption site now gates on the reconciliation check, so the false
     * ladder no longer reaches the model. This is the other half: the wrapper
     * records WHICH part the object it holds actually describes, exposes it
     * (getArchitectureTechnology()), and announces the substitution once.
     * Fabricating architecture objects for the three missing technologies
     * would be the dishonest repair; saying which part is being read is not. */
    if (!dram_arch_) {
        // 1.11.59 (audit C018): a fresh object carries its factory
        // organization, so no device width is stamped on it yet.
        arch_device_width_bits_ = 0;
        std::string dt = dram_type_;
        std::transform(dt.begin(), dt.end(), dt.begin(), ::toupper);
        if (dt == "DDR5") {
            dram_arch_ = pimid::memory::createDDR5_4800_Verified();
        } else if (dt == "HBM2") {
            dram_arch_ = pimid::memory::createHBM2_Verified();
        } else if (dt == "HBM3") {
            dram_arch_ = pimid::memory::createHBM3_Verified();
        } else if (dt == "DDR3") {
            // 1.11.68: DDR3 owns its object; it no longer reads DDR4's.
            dram_arch_ = pimid::memory::createDDR3_1600_Verified();
        } else if (dt == "LPDDR5") {
            // 1.11.69: LPDDR5 owns its object.
            dram_arch_ = pimid::memory::createLPDDR5_6400_Verified();
        } else if (dt == "GDDR6") {
            // 1.11.70: GDDR6 owns its object. No technology reads DDR4's now.
            dram_arch_ = pimid::memory::createGDDR6_14000_Verified();
        } else {
            dram_arch_ = pimid::memory::createDDR4_2400_Verified();
            if (!dt.empty() && dt != "DDR4") {
                static bool announced_substitution = false;
                if (!announced_substitution) {
                    announced_substitution = true;
                    std::cerr << "[mem] NOTE: there is no " << dt
                              << " architecture object in this tree, so the "
                                 "DDR4-2400 one is being read in its place. "
                                 "Every width, internal bandwidth and derived "
                                 "hierarchy figure this wrapper reports for "
                              << dt << " describes DDR4-2400, not " << dt
                              << ". getArchitectureTechnology() reports the "
                                 "part actually being read." << std::endl;
                }
            }
        }
    }

    /* 1.11.59 (audit C018): stamp the configured JEDEC device width onto the
     * object BEFORE anything reads a width, a bandwidth or the reconciliation
     * check below off it. No-op when no width is configured. */
    applyDeviceWidthToArchitecture();

    /* 1.11.61 (ruling R1): stamp the simulated preset's density onto the
     * object. AFTER the width -- the width decides which preset row applies --
     * and BEFORE the two checks below, which read the density. */
    applyPresetDensityToArchitecture();
    applyPresetBankGroupingToArchitecture();   // 1.11.72

    /* 1.11.63 (R6-5): and the simulated bin's ns timings, beside the density
     * and for the same reason -- the object describes the part the preset
     * simulates, timings included. */
    applyPresetTimingsToArchitecture();

    /* 1.11.57 (audit round 3, C002): this check RUNS now.
     *
     * It was written in 1.11.56 to catch exactly the drift that release
     * fixed, and it sat inside parseConfiguration() behind `if (dram_arch_)`
     * -- but initialize() calls parseConfiguration() first and only
     * populates dram_arch_ afterwards, so the pointer was always null and
     * the guard always false. It never executed once. Gate 1166D's K11 arm
     * reported "mismatch warnings=0" and that was a vacuous pass: zero
     * warnings because nothing ran, not because nothing was wrong. Moved
     * below the population, where it would have caught C001 (the CACTI-IO
     * rate table still at DDR5-4800 and HBM2-2000) on its first run. */
    /* 1.11.56: ONE PART, ONE SPEED BIN -- checked, not trusted.
     *
     * The Ramulator preset above decides the cycles this simulator
     * counts. The DRAM architecture object beside it decides every
     * bandwidth the simulator REPORTS -- chip I/O, rank, channel, and
     * since 1.11.56 the whole per-level hierarchy link ladder. Nothing
     * held the two together, and three of the four had drifted: HBM2
     * 2000 vs the 2400 preset, DDR5 4800 vs the 3200 preset, HBM3 4000
     * vs the 6400 preset. That is D002's defect (DDR4 array at 2400,
     * termination at 3200) repeated at three more technologies, and it
     * is how a device's reported memory bandwidth came to describe a
     * part 1.6x faster than the one whose cycles were being counted.
     *
     * The aggregate is the arithmetic the preset implies, so it is the
     * cross-check: channel_databus_bits x data_rate x channels / 8 must
     * reproduce bandwidth_. A future preset change that forgets the
     * architecture object now fails here instead of quietly re-describing the
     * part. */
    /* 1.11.57 (audit C007): the aggregate is now channel width x rate x
     * CHANNELS. It used to be channel width x rate alone, which was the same
     * arithmetic only because the HBM objects stored the whole stack in a
     * field named for one channel. With that field holding one channel for
     * every family, the channel count has to appear explicitly -- and it comes
     * from the preset above, which is the authority for how many channels this
     * run counts cycles for. DDR keeps channels_ = 1, so nothing moves there. */
    if (dram_arch_) {
        std::string dt = dram_type_;
        std::transform(dt.begin(), dt.end(), dt.begin(), ::toupper);
        double implied_mbs =
            (dram_arch_->datapath.channel_databus_bits.value_bits / 8.0) *
            dram_arch_->timing.data_rate_mtps *
            static_cast<double>(channels_ > 0 ? channels_ : 1);
        if (!anchor_quiet_ && bandwidth_ > 0 && implied_mbs > 0.0 &&
            std::fabs(implied_mbs - static_cast<double>(bandwidth_)) >
                0.02 * static_cast<double>(bandwidth_)) {
            /* 1.11.60 (audit round 4, C008): NAME THE SOURCE THE NUMBER
             * ACTUALLY CAME FROM. This message said "the simulated preset
             * implies <bandwidth_>", and bandwidth_ is not the preset's: for
             * DDR3/DDR5/LPDDR5/GDDR6 it is derivedBwMBs(), i.e.
             * CactiIOWrapper::dramRateMTs() x dramChannelWidthBits() x
             * channels -- PIMID's own rate table, the one the ENERGY path
             * prices from -- and for DDR4/HBM2/HBM3 it is a literal in the
             * branch above. The preset is the one authority of the three that
             * this comparison never consults. It was invisible on DDR3 and
             * LPDDR5, where the rate table and the preset happen to agree
             * (1600 and 6400); GDDR6 is where they differ, and there the line
             * quoted 112000 MB/s as the preset's implication twelve lines
             * after the wrapper had announced that same preset at a different
             * rate entirely. The comparison itself is unchanged and still
             * correct -- it is the rate table against the architecture object
             * -- so only the attribution moves. */
            std::cerr << "[ramulator] WARNING: " << dt
                      << " speed-bin mismatch. This run's configured aggregate "
                         "bandwidth is " << bandwidth_
                      << " MB/s (from PIMID's rate table via "
                         "CactiIOWrapper::dramRateMTs x channel width x "
                         "channels, or a literal where that table has no row "
                         "-- NOT from the Ramulator timing preset), but the "
                         "architecture object's channel databus x data rate x "
                         "channels gives " << implied_mbs
                      << " MB/s (" << dram_arch_->timing.data_rate_mtps
                      << " MT/s). Cycles come from the preset, reported "
                         "bandwidths come from the architecture object, and "
                         "energy comes from the rate table, so this run counts "
                         "one part and prices another."
                      << std::endl;
        }
    }

    /* 1.11.61 (ruling R2): ONE PART, ONE CAPACITY -- checked, not trusted.
     *
     * The companion of the speed-bin check above, and it exists for the same
     * reason: the Ramulator ORG preset decides the device whose cycles are
     * counted, the architecture object's chip_size_mb decides the die area
     * reported and (through bank_size_mb) the contiguous block every placed
     * element addresses, and until this release nothing held the two together.
     * They had drifted 4x-32x across the lineup -- an LPDDR5 run reported
     * 2.75 mm^2 of DRAM silicon while claiming 8 GiB of memory.
     *
     * THE UNIT DIFFERS BY FAMILY, and the check follows the field's declared
     * semantics rather than flattening them:
     *   DDR family: chip_size_mb IS the preset's device capacity. Direct.
     *   HBM:        chip_size_mb is ONE CORE DIE, and a core die fronts two
     *               channels, so chip_size_mb x dies must reproduce the
     *               preset's stack capacity (per-channel density x channels).
     *
     * 1.11.61 -> 1.11.63: THIS CHECK USED TO FIRE ON HBM3 BY DESIGN. IT MUST
     * NOT ANY MORE.
     *
     * Ruling R2 kept HBM3's chip_size_mb at 2048 MB against a preset stack of
     * 8 GiB and made the disagreement audible on every HBM3 run rather than
     * silent -- a deliberate, temporary firing, recorded in the 1.11.61 entry
     * as awaiting a ruling. R6 gave it (item 1): the object follows the preset,
     * the wrapper literal has no standing, and applyPresetDensityToArchitecture()
     * now DERIVES the core die as the preset's per-channel density x the two
     * channels a die fronts -- 1024 MB, and 1024 x 8 dies = 8192 MB = the
     * preset's stack.
     *
     * So from this release a firing on ANY technology, HBM3 included, is a REAL
     * ERROR again and not an expected note. Nothing here special-cases HBM3;
     * the check is unchanged and its input moved onto the preset. */
    if (dram_arch_ && preset_org_.valid) {
        std::string dt = dram_type_;
        std::transform(dt.begin(), dt.end(), dt.begin(), ::toupper);
        const uint64_t chip_mb = dram_arch_->organization.chip_size_mb;
        const int      dies    = getPresetDiesPerStack();
        const uint64_t implied_mb =
            chip_mb * static_cast<uint64_t>(preset_org_.per_channel_density
                                                ? (dies > 0 ? dies : 1) : 1);
        const uint64_t preset_mb = getPresetDeviceCapacityMB();
        /* SAID ONCE PER TECHNOLOGY, unlike the speed-bin check above, and for
         * a reason the speed-bin check does not have: a wrapper is built for a
         * technology the run is NOT configured for at several sites (the HBM3
         * bandwidth reference anchor is queried on every run, whatever its
         * memory technology), and this check fires on HBM3 by design. Without
         * the latch a single DDR4 run prints the HBM3 line fifteen times. The
         * message still names its own technology, so a reader can tell the
         * configured part from a reference query. */
        static std::set<std::string> capacity_warned;
        /* 1.11.61 (gate 1171A D4): anchor wrappers stay silent. The B012
         * reference anchors construct an HBM3 wrapper inside EVERY detailed
         * run to read one bandwidth number, and its initialization was
         * emitting the HBM3 capacity warning into DDR4 and HBM2 logs -- a
         * true statement misattributed to a run it does not describe. The
         * warning still fires on every wrapper actually used for the run. */
        if (!anchor_quiet_ && preset_mb > 0 && implied_mb != preset_mb &&
            capacity_warned.insert(dt).second) {
            std::cerr << "[ramulator] WARNING: " << dt
                      << " capacity mismatch between the architecture object "
                         "and the org preset this run simulates. The object's "
                         "chip_size_mb is " << chip_mb << " MB";
            if (preset_org_.per_channel_density) {
                std::cerr << " (a CORE DIE) x " << (dies > 0 ? dies : 1)
                          << " dies = " << implied_mb << " MB per stack";
            } else {
                std::cerr << " per device";
            }
            std::cerr << ", but preset " << preset_org_.preset_name << " ("
                      << preset_org_.preset_source << ") gives " << preset_mb
                      << " MB";
            if (preset_org_.per_channel_density) {
                std::cerr << " per stack (" << preset_org_.density_mb
                          << " MB/channel x " << channels_ << " channels)";
            }
            std::cerr << ". Cycles come from the preset; the reported DRAM die "
                         "area and the per-element contiguous block come from "
                         "the object, so this run counts one part and sizes "
                         "another."
                      << std::endl;
        }
    }

    current_cycle_ = 0;
    resetStats();
}

/* 1.11.59 (audit C018): THE DEVICE WIDTH REACHES THE WIDTHS, not only the
 * energy.
 *
 * What the audit found: setDeviceWidth() wrote device_width_ and nothing else.
 * That string was read at exactly two places, both array-energy calls, where it
 * sets how many devices a rank access lights up (4/8/16). Every WIDTH the
 * wrapper reports came from the architecture object instead, and the DDR
 * objects hold a fixed 8-bit chip DQ -- so with memory.dram.device_width: x16
 * set, one run priced the rank as 4 devices for energy and modelled it as 8
 * devices of 8 pins for bandwidth, and the hierarchy link ladder's L3 rung (the
 * level literally named "chip DQ pins") was 8 bits for an x4 part and an x16
 * part alike. Worse than the number: architecture_extractor.h stamped that 8
 * VERIFIED with the source "Extracted from Ramulator device configuration
 * (x4/x8/x16)" -- a provenance claim on a value that ignored the width.
 *
 * The repair is the one the finding asks for first: make the object honour the
 * width. Four coupled quantities move together, and each is JEDEC organization
 * rather than a new constant:
 *
 *   chip_io_bits          = the configured width. That IS the x4/x8/x16 datum.
 *   chips_per_rank        = rank_databus_bits / width. A DDR-class rank
 *                           presents 64 data bits, so it takes 64/width devices
 *                           to build one -- the same arithmetic, from the same
 *                           reasoning, as chipsPerRankForDeviceWidth() in
 *                           main.cpp (1.11.57, latent A013) and the die-count
 *                           helper beside it. It also makes the extractor's
 *                           "chips_per_rank x chip_io_bits" description of the
 *                           rank bus TRUE for DDR-class parts.
 *   prefetch_datapath_bits = prefetch length x width. The field's own factory
 *                           comment defines it that way ("8n prefetch x 8-bit
 *                           I/O" for DDR4, "16n prefetch x 8-bit I/O" for
 *                           DDR5); the prefetch LENGTH is what JEDEC fixes, and
 *                           it is recovered from the object rather than
 *                           re-tabulated here.
 *   bank_groups_per_chip  = halved at x16 on DDR4 (4 -> 2) and DDR5 (8 -> 4),
 *                           which is the JEDEC coupling main.cpp already
 *                           applies to the hierarchy at the same site that
 *                           calls setDeviceWidth(). Leaving it would have the
 *                           object and the hierarchy describe two different
 *                           parts of the same run.
 *
 * WHAT DOES NOT MOVE: rank_databus_bits and channel_databus_bits. A rank is 64
 * bits wide whatever devices build it, which is the whole point of the width
 * knob, so the rank rung of the ladder and the speed-bin reconciliation check
 * are untouched. And at x8 -- the default, and what every corpus cell ran --
 * all four assignments above reproduce the factory values exactly, so no
 * existing result moves. Only x4 and x16 move, and they were wrong.
 *
 * HBM is excluded and says so: a stack has no x4/x8/x16 device width. main.cpp
 * already refuses the key for HBM; this is the wrapper refusing it on its own
 * authority, since the wrapper is reachable without main.cpp.
 *
 * RESIDUAL, stated rather than papered over: the DEFAULT Ramulator2 config
 * this wrapper generates in parseConfiguration() still names a fixed org
 * preset per technology ("DDR4_8Gb_x8", "DDR5_8Gb_x8", ...), so on that path
 * the timing model's device organization does not follow this key -- only the
 * YAML main.cpp writes for the zsim path does (writeRamulatorConfigYaml takes
 * the width). This function fixes the widths the wrapper REPORTS; it does not
 * claim to have re-bound the preset the timing model would parse. */
double RamulatorWrapper::modelledRateMTs() const {
    const double table = PIMID::CactiIOWrapper::dramRateMTs(dram_type_);
    if (preset_timing_.valid && preset_timing_.rate_mtps > 0) {
        const double preset = preset_timing_.rate_mtps;
        static std::set<std::string> said;
        if (table > 0.0 && std::fabs(table - preset) > 1.0 && !anchor_quiet_ &&
            said.insert(dram_type_ + std::to_string(int(preset))).second) {
            std::cerr << "[mem] NOTE: " << dram_type_ << " is modelled at the preset's "
                      << preset << " MT/s; the static rate table still says " << table
                      << " MT/s and is ignored (one authority: the preset)." << std::endl;
        }
        return preset;
    }
    return table;
}

std::string RamulatorWrapper::energyKey() const {
    std::string dt = dram_type_;
    std::transform(dt.begin(), dt.end(), dt.begin(), ::toupper);
    return (dt == "DDR5") ? dt + "-" + std::to_string(ddr5_grade_mtps_) : dram_type_;
}

void RamulatorWrapper::setDdr5SpeedGrade(int mtps) {
    if (mtps != 3200 && mtps != 4800 && mtps != 5600) {
        std::cerr << "[mem] FATAL: memory.dram.ddr5_speed_grade = " << mtps
                  << " is not one of 3200 / 4800 / 5600 -- the three grades for "
                     "which this tree holds a timing bin (JESD79-5D Tables 283 / 287 "
                     "/ 289) AND a matching IDD row (Micron MT60B Rev A = 4800B, "
                     "Rev D = 5600B; the 3200 row keeps its stated IDD gap)."
                  << std::endl;
        std::exit(2);
    }
    ddr5_grade_mtps_ = mtps;
}

void RamulatorWrapper::setDeviceWidth(const std::string& w) {
    device_width_ = w;
    applyDeviceWidthToArchitecture();  // no-op until dram_arch_ exists
    /* 1.11.61 (rulings R1/R4): the width selects the org preset row (rows and
     * bank groups move with it), so a width set AFTER initialize() must
     * re-resolve the preset and re-stamp the density. Before initialize() this
     * is harmless: initialize() resolves and stamps again. */
    resolvePresetOrganization();
    applyPresetDensityToArchitecture();
    applyPresetBankGroupingToArchitecture();   // 1.11.72  // no-op until dram_arch_ exists
    /* 1.11.63 (R6-3/R6-5): the width also selects the devices-per-rank the
     * capacity derivation counts, so re-derive capacity_ (and, harmlessly,
     * bandwidth_, which the width does not reach) and re-stamp the ns bin.
     * Only on the DEFAULT path: with a config FILE the YAML is the authority
     * for both, and parseConfiguration() has already applied it. */
    if (config_path_.empty()) derivePresetCapacityAndBandwidth();
    applyPresetTimingsToArchitecture();  // no-op until dram_arch_ exists
}

void RamulatorWrapper::applyDeviceWidthToArchitecture() {
    if (!dram_arch_ || device_width_.empty()) return;

    int w_bits = 0;
    if (device_width_ == "x4")       w_bits = 4;
    else if (device_width_ == "x8")  w_bits = 8;
    else if (device_width_ == "x16") w_bits = 16;
    if (w_bits == 0) {
        static bool warned_bad_width = false;
        if (!warned_bad_width) {
            warned_bad_width = true;
            std::cerr << "[mem] WARNING: device width \"" << device_width_
                      << "\" is not one of x4/x8/x16. The architecture object "
                         "keeps its factory organization, so the chip DQ width, "
                         "the devices per rank and every figure derived from "
                         "them describe an x"
                      << dram_arch_->datapath.chip_io_bits.value_bits
                      << " part, not the configured one." << std::endl;
        }
        return;
    }

    std::string dt = dram_type_;
    std::transform(dt.begin(), dt.end(), dt.begin(), ::toupper);
    if (dt.rfind("HBM", 0) == 0) {
        static bool warned_hbm_width = false;
        if (!warned_hbm_width) {
            warned_hbm_width = true;
            std::cerr << "[mem] NOTE: a device width (" << device_width_
                      << ") was set for " << dt
                      << ", which has no x4/x8/x16 device organization -- an "
                         "HBM stack's interface is fixed 128-bit channels. The "
                         "width is IGNORED for the architecture object and the "
                         "widths derived from it." << std::endl;
        }
        return;
    }

    if (arch_device_width_bits_ == w_bits) return;  // already stamped

    // The width currently described by this object: the stamp if one was
    // applied, otherwise the factory's own chip DQ width.
    const int base_w = arch_device_width_bits_ > 0
                           ? arch_device_width_bits_
                           : dram_arch_->datapath.chip_io_bits.value_bits;
    if (base_w <= 0) return;

    // Prefetch LENGTH (8n, 16n, ...) recovered from the object it was written
    // on, so the JEDEC number is not re-tabulated in a second place.
    const int prefetch_length =
        dram_arch_->datapath.prefetch_datapath_bits.value_bits / base_w;

    dram_arch_->datapath.chip_io_bits.value_bits = w_bits;
    dram_arch_->datapath.chip_io_bits.status =
        pimid::memory::VerificationStatus::VERIFIED;
    dram_arch_->datapath.chip_io_bits.source =
        "JEDEC device organization " + device_width_ +
        ", from the configured memory.dram.device_width -- the same key that "
        "sets the array-energy device count. NOT read from a Ramulator device "
        "object; this wrapper's default config still names a fixed x8 org "
        "preset.";
    dram_arch_->datapath.chip_io_bits.notes =
        "External package DQ pins of one device";

    const int rank_bits = dram_arch_->datapath.rank_databus_bits.value_bits;
    if (rank_bits > 0) {
        dram_arch_->organization.chips_per_rank = rank_bits / w_bits;
    }

    if (prefetch_length > 0) {
        dram_arch_->datapath.prefetch_datapath_bits.value_bits =
            prefetch_length * w_bits;
        dram_arch_->datapath.prefetch_datapath_bits.notes =
            std::to_string(prefetch_length) + "n prefetch x " +
            std::to_string(w_bits) + "-bit device I/O";
    }

    // JEDEC couples the bank-group count to the width on DDR4 and DDR5; the
    // same table main.cpp applies to the hierarchy at the calling site.
    if (dt == "DDR4")      dram_arch_->organization.bank_groups_per_chip = (w_bits == 16) ? 2 : 4;
    else if (dt == "DDR5") dram_arch_->organization.bank_groups_per_chip = (w_bits == 16) ? 4 : 8;

    arch_device_width_bits_ = w_bits;

    if (w_bits != 8) {
        static bool announced_width = false;
        if (!announced_width) {
            announced_width = true;
            std::cerr << "[mem] NOTE: " << dt << " architecture object stamped "
                      << device_width_ << ": chip DQ " << w_bits << " bits, "
                      << dram_arch_->organization.chips_per_rank
                      << " devices per " << rank_bits << "-bit rank. The rank "
                         "and channel buses are unchanged -- a rank is 64 bits "
                         "wide whatever devices build it." << std::endl;
        }
    }
}

void RamulatorWrapper::loadConfig(const std::string& config_path) {
    config_path_ = config_path;
    parseConfiguration();
}

void RamulatorWrapper::parseConfiguration() {
    /* 1.11.63 (R6-3): the preset pair is resolved FIRST, because the
     * per-technology branch below no longer states a capacity or (for DDR4 and
     * both HBM stacks) a bandwidth -- it derives both from that pair. Both
     * resolvers depend only on dram_type_ and device_width_, so they are safe
     * this early and idempotent when initialize() calls them again. */
    resolvePresetOrganization();
    resolvePresetTiming();

    // Try to load PIMID configuration and convert to Ramulator format
    if (config_path_.empty()) {
        // Generate correct Ramulator2 YAML config based on DRAM type
        std::string dt = dram_type_;
        std::transform(dt.begin(), dt.end(), dt.begin(), ::toupper);

        // Helper lambda: generate Ramulator2 YAML config
        auto makeConfig = [](const std::string& dram_impl, const std::string& org_preset,
                            const std::string& timing_preset) -> std::string {
            /* 1.11.66: DDR5's impl REQUIRES the RFM param group (it throws
             * "ParamGroup RFM is not specified" otherwise). main.cpp's co-sim
             * emitter has supplied "RFM: BRC: 2" all along; this oracle-side
             * config never did, which went unnoticed because oracles never
             * instantiated -- until the shape check needed to. Same value as
             * main.cpp so the two emitters describe one part. */
            const std::string rfm = (dram_impl == "DDR5") ? "\n    RFM:\n      BRC: 2" : "";
            return "Frontend:\n  impl: GEM5\n\nMemorySystem:\n  impl: GenericDRAM\n  clock_ratio: 1\n"
                   "  DRAM:\n    impl: " + dram_impl + "\n    org:\n      preset: " + org_preset +
                   "\n    timing:\n      preset: " + timing_preset + rfm +
                   "\n  Controller:\n    impl: Generic\n    Scheduler:\n      impl: FRFCFS\n"
                   "    RefreshManager:\n      impl: AllBank\n"
                   "    RowPolicy:\n      impl: ClosedRowPolicy\n      cap: 4\n"
                   "  AddrMapper:\n    impl: RoBaRaCoCh\n";
        };

        /* 1.11.52 (audit D016): BANDWIDTH IS DERIVED, and a preset that
         * cannot express the modelled rate says so.
         *
         * These branches carried a literal MB/s beside a Ramulator timing
         * preset, and the two drifted apart from the rate the ENERGY path
         * prices at: DDR5 was configured at the DDR5_3200AN preset with a
         * literal 25600 MB/s while pimid_energy and CACTI-IO price DDR5 at
         * 4800 MT/s; GDDR6 carried 64000 MB/s against a 14000 MT/s energy
         * rate. Bandwidth is now rate x channel width x channels from the
         * SAME rate table the energy path uses (spec primitives), so the
         * literal cannot drift.
         *
         * The preset mismatch is NOT silently resolved: upstream Ramulator2
         * ships no DDR5 timing bin above 3200, so the timing model genuinely
         * cannot run at the modelled 4800. That is a modelling limitation,
         * and it is announced once here rather than hidden in two numbers
         * that disagree.
         *
         * 1.11.63 (R6-3): the derivedBwMBs() lambda that used to sit here is
         * GONE, and with it the last three bandwidth literals (DDR4 19200,
         * HBM2 307000, HBM3 819000) and all seven capacity literals. D016
         * derived the bandwidth for four technologies and left three stating
         * it, which is the same split R6 removes everywhere else. Both
         * quantities are now derived for all seven, once, in
         * derivePresetCapacityAndBandwidth() below the chain -- so a branch
         * here names a preset and a channel count and nothing else.
         *
         * 1.11.67 (re-sim pre-flight): THE NOTE THIS LAMBDA PRINTED WAS
         * FALSE. It compared the static rate table against the preset and
         * claimed that energy, termination and bandwidth follow the TABLE --
         * true before 1.11.63, when modelledRateMTs() made the preset the one
         * authority for all three. On every default DDR5 run since 1.11.66
         * (table 3200, preset 4800B) it announced "modelled at 3200 MT/s
         * (energy, termination and bandwidth)" one line before
         * modelledRateMTs() announced the opposite, and the first line was the
         * wrong one. The disclosure it existed for is still made, once and
         * correctly, by modelledRateMTs() itself ("the static rate table
         * still says X and is ignored"). The lambda is now a no-op kept so
         * the per-technology branches keep naming their preset and rate in
         * one place, where a future reader looks for them. */
        auto sayPresetRate = [](const std::string&, const char*, double) {
        };

        /* 1.11.66 (exposed by the shape check): the org preset named here
         * must follow the run's device width, as the transcription already
         * does -- the x8 literal made an x4/x16 run transcribe one part and
         * simulate another (caught the first time the check ran on an x4
         * DDR5 config). */
        const std::string wsuf = std::string("_x") + std::to_string(presetWidthBits(device_width_, 8));
        if (dt == "DDR3") {
            config_yaml_ = makeConfig("DDR3", ("DDR3_8Gb" + wsuf).c_str(), "DDR3_1600H");
            channels_ = 1; ranks_per_channel_ = 1;
            sayPresetRate("DDR3", "DDR3_1600H", 1600);
        } else if (dt == "DDR5") {
            {   // 1.11.66 (R8 #9): grade selects the preset pair
                const char* tp = (ddr5_grade_mtps_ == 5600) ? "DDR5_5600B"
                               : (ddr5_grade_mtps_ == 4800) ? "DDR5_4800B" : "DDR5_3200AN";
                const std::string op = std::string(ddr5_grade_mtps_ == 3200 ? "DDR5_8Gb" : "DDR5_16Gb") + wsuf;
                config_yaml_ = makeConfig("DDR5", op.c_str(), tp);
            }
            channels_ = 1; ranks_per_channel_ = 1;
            sayPresetRate("DDR5", preset_timing_.preset_name.c_str(), ddr5_grade_mtps_);
        } else if (dt == "LPDDR5") {
            config_yaml_ = makeConfig("LPDDR5", "LPDDR5_8Gb_x16", "LPDDR5_6400");
            channels_ = 1; ranks_per_channel_ = 1;
            sayPresetRate("LPDDR5", "LPDDR5_6400", 6400);
        } else if (dt == "GDDR6") {
            /* 1.11.60 (audit round 4, C007): the preset named here did not
             * exist. `grep GDDR6_2000_1.35V_x16 external/ramulator/` returns
             * nothing; the four GDDR6 timing presets this fork ships are
             * GDDR6_2000_{1350mV,1250mV}_{double,quad} (GDDR6.cpp:34-37), and
             * Ramulator2 throws ConfigurationError on an unrecognised name.
             * It survived because this generated YAML is never handed to
             * Ramulator on this path -- initialize() builds an instance only
             * when config_path_ is non-empty, and then config_yaml_ holds the
             * FILE's contents -- so the string reached nothing but the note
             * below. Named to match what the run really writes for the timing
             * model (main.cpp's GDDR6 emission), so that if this path is ever
             * made live it selects a preset that exists. */
            config_yaml_ = makeConfig("GDDR6", "GDDR6_8Gb_x16", "GDDR6_2000_1350mV_double");
            /* GDDR6 is a dual-channel part: two 16-bit fully independent
             * channels per device (JESD250D sec 2.2 p.3, Table 19 p.18,
             * Table 80 p.177). channels_ = 1 here used to contradict that spec
             * and made the analytical model treat the whole device's rate as
             * one channel's.
             *
             * 1.11.61 (ruling R3): the arithmetic in this note used to read
             * "32 GB/s/channel x 2 = 64 GB/s", which is the 16 Gb/s bin's
             * figure and not this tree's -- and dramChannelWidthBits() was
             * returning the 32-bit DEVICE width beside it, so derivedBwMBs()
             * booked the channel dimension twice and produced 112000 MB/s. On
             * this tree's own 14000 MT/s basis (CactiIOWrapper::dramRateMTs)
             * the arithmetic is 16 bits x 14000 MT/s / 8 = 28 GB/s per
             * channel, x 2 channels = 56 GB/s for the device. */
            channels_ = 2; ranks_per_channel_ = 1;
            /* 1.11.60 (audit round 4, C007): both facts in this note were
             * wrong. The preset named did not exist in the tree, and the
             * 16000 MT/s attributed to it is not a rate any GDDR6 preset
             * carries -- all four ship rate = 2000 in Ramulator2's timing
             * table, the column GDDR6.cpp:33 documents as "rate (in MT/s)"
             * and from which GDDR6.cpp:259 derives tCK as 1e6/(rate/2) ps. So
             * a reader reconciling PIMID's 14000 MT/s energy basis against the
             * timing model was handed a third number under a name that
             * matched nothing. The note now carries the preset the run writes
             * and the rate that preset holds; the claim it exists to make --
             * upstream ships no GDDR6 timing bin at the modelled rate -- is
             * unchanged and still true, since 2000 is the only rate on offer. */
            /* 1.11.66: the rate column now carries the pin rate (14000), so
             * the divergence this NOTE existed to announce -- energy at
             * 14000, timing at "2000" -- is CLOSED: both describe the same
             * 14 Gb/s part. The call stays so that a future preset swap that
             * re-opens the gap is announced again; at 14000 it is silent. */
            sayPresetRate("GDDR6", "GDDR6_2000_1350mV_double", 14000);
        } else if (dt == "HBM2") {
            config_yaml_ = makeConfig("HBM2", "HBM2_4Gb", "HBM2_2.4Gbps");
            channels_ = 8; ranks_per_channel_ = 1;
        } else if (dt == "HBM3") {
            config_yaml_ = makeConfig("HBM3", "HBM3_4Gb", "HBM3_6.4Gbps");
            channels_ = 16; ranks_per_channel_ = 1;
        } else {
            // Default: DDR4-2400
            config_yaml_ = makeConfig("DDR4", ("DDR4_8Gb" + wsuf).c_str(), "DDR4_2400R");
            channels_ = 1; ranks_per_channel_ = 1;
            sayPresetRate("DDR4", "DDR4_2400R", 2400);
        }

        /* 1.11.63 (R6-3): banks per rank, from the preset instead of beside
         * it. Two of the seven literals disagreed with the preset they were
         * written next to -- DDR4 said 8 against DDR4_8Gb_x8's 16, HBM3 said
         * 16 against HBM3_4Gb's 32 -- which is the third private copy of the
         * organisation the 1.11.61 header note named and did not actually
         * remove. channels_in_density divides out GDDR6's double count: its 32
         * banks span both channels, and this field is multiplied by channels_
         * at its one consumer (the refresh-energy bank population). */
        if (preset_org_.valid && preset_org_.banks_in_density > 0) {
            const int cid = preset_org_.channels_in_density > 0
                                ? preset_org_.channels_in_density : 1;
            banks_per_rank_ =
                static_cast<uint32_t>(preset_org_.banks_in_density / cid);
        }

        /* 1.11.63 (R6-3): capacity and bandwidth, derived from the preset pair
         * the branch above named. Must run AFTER the branch, which sets the
         * channel and rank counts both derivations read. */
        derivePresetCapacityAndBandwidth();

    } else {
        // Load configuration from file
        std::ifstream config_file(config_path_);
        if (config_file.is_open()) {
            std::stringstream buffer;
            buffer << config_file.rdbuf();
            config_yaml_ = buffer.str();

            try {
                YAML::Node config = YAML::Load(config_yaml_);

                // Parse basic parameters
                if (config["dram"]) {
                    auto dram = config["dram"];
                    if (dram["channels"]) channels_ = dram["channels"].as<uint32_t>();
                    if (dram["ranks"]) ranks_per_channel_ = dram["ranks"].as<uint32_t>();
                    if (dram["banks"]) banks_per_rank_ = dram["banks"].as<uint32_t>();
                    if (dram["capacity"]) capacity_ = dram["capacity"].as<uint64_t>();
                    if (dram["bandwidth"]) bandwidth_ = dram["bandwidth"].as<uint64_t>();
                }
            } catch (const YAML::Exception& e) {
                std::cerr << "Warning: Failed to parse YAML config: " << e.what() << std::endl;
                std::cerr << "Using default DDR4 configuration" << std::endl;
            }
        }
    }
}


/* 1.11.66 (round 5, A6/#11): THE SHAPE CHECK. Every organization
 * cross-check this tree had was a PRODUCT identity -- density == banks x
 * rows x cols x dq, capacity == chip x dies -- and a transcription carrying
 * half the banks and twice the rows satisfies every one of them. That is how
 * the HBM2 org shipped wrong for three releases (1174A), how the DDR3
 * transcription went stale against the 1.11.63 preset unnoticed (R5 A1), and
 * how the DDR5 bank count sat at 2x the part (R5 A2). This binds the
 * transcription FIELD BY FIELD to the device Ramulator actually instantiates
 * from the preset the run names: banks, rows and columns must each agree,
 * not just their product. A disagreement is FATAL -- getPresetRowsPerBank()
 * is the sole authority for subarrays_per_bank, the tree shape and
 * pages_per_unit, so a wrong shape here is a wrong part everywhere.
 *
 * It is its own step of initialize() so it runs for EVERY wrapper --
 * including the parameter oracles (empty config_path_) that never build a
 * Ramulator instance, which are exactly the wrappers whose rows feed the
 * device-scope tree. With no live instance a throwaway one is built from
 * config_yaml_ purely to read its organization (construction only, no
 * ticks) and discarded. PIMID_ORG_BREAK=1 forces the mismatch so a gate can
 * prove the check fires. */
void RamulatorWrapper::checkTranscribedOrganizationShape() {
    if (!preset_org_.valid) return;
    Ramulator::pimid_probe::LiveOrganization live;
    if (ramulator_memory_system_) {
        live = Ramulator::pimid_probe::liveOrganization(ramulator_memory_system_.get());
    } else {
        try {
            YAML::Node cfg = YAML::Load(config_yaml_);
            auto* probe_sys = Ramulator::Factory::create_memory_system(cfg);
            std::shared_ptr<Ramulator::IMemorySystem> holder(probe_sys);
            live = Ramulator::pimid_probe::liveOrganization(holder.get());
        } catch (const std::exception& e) {
            std::cerr << "[mem] NOTE: organization shape check skipped -- could "
                         "not instantiate the preset to read its organization ("
                      << e.what() << ")." << std::endl;
            return;
        }
    }
    if (!live.valid) {
        std::cerr << "[mem] NOTE: organization shape check skipped -- no IDRAM "
                     "interface reachable from the memory system." << std::endl;
        return;
    }
    /* The transcription's banks_in_density counts every bank in the density
     * unit: per CHANNEL for HBM (per_channel_density), per DEVICE otherwise
     * -- and a GDDR6 device is two channels, so the device count multiplies
     * the channel level in. pseudochannels x bankgroups x banks per channel,
     * x channels when the unit is the device. */
    long long live_banks_total = live.pseudochannels * live.bankgroups * live.banks;
    if (!preset_org_.per_channel_density && live.channels > 1) live_banks_total *= live.channels;
    long long want_banks = preset_org_.banks_in_density;
    long long want_rows  = preset_org_.rows_per_bank;
    long long want_cols  = preset_org_.cols_per_row;
    if (getenv("PIMID_ORG_BREAK") != nullptr) { want_rows *= 2; want_banks /= 2; }
    /* 1.11.72: the GROUPING is checked too. live.bankgroups is per pseudo-
     * channel; the transcription folds pseudo-channels into its group count
     * (as the objects and main.cpp's table do), so the comparison is
     * pseudochannels x bankgroups against the transcribed groups. */
    const long long live_groups = static_cast<long long>(live.pseudochannels) * live.bankgroups;
    if (preset_org_.bank_groups > 0 &&
        (live_groups != preset_org_.bank_groups || live.banks != preset_org_.banks_per_group)) {
        std::cerr << "[mem] FATAL: the transcribed bank grouping for " << preset_org_.preset_name
                  << " (" << preset_org_.bank_groups << " groups x " << preset_org_.banks_per_group
                  << " banks) does not match the device Ramulator instantiated ("
                  << live_groups << " x " << live.banks << "). Fix the transcription."
                  << std::endl;
        std::exit(2);
    }
    if (live_banks_total != want_banks || live.rows != want_rows || live.columns != want_cols) {
        std::cerr << "[mem] FATAL: the transcribed organization for "
                  << preset_org_.preset_name << " (" << preset_org_.preset_source
                  << ") does not match the device Ramulator instantiated from it: "
                     "transcription banks/rows/cols = "
                  << want_banks << "/" << want_rows << "/" << want_cols
                  << ", instantiated = " << live_banks_total << "/" << live.rows
                  << "/" << live.columns
                  << ". The density product may still agree -- that is exactly the "
                     "drift this check exists to catch. Fix the transcription in "
                     "ramulator_wrapper.cpp to mirror the preset row." << std::endl;
        std::exit(2);
    }
}

void RamulatorWrapper::createRamulatorInstance() {
    try {
        // Parse YAML configuration for Ramulator
        YAML::Node config = YAML::Load(config_yaml_);

        // Create Ramulator memory system using factory
        auto memory_system_raw = Ramulator::Factory::create_memory_system(config);
        ramulator_memory_system_ = std::shared_ptr<Ramulator::IMemorySystem>(memory_system_raw);

        if (!ramulator_memory_system_) {
            return;
        }

    } catch (const std::exception& e) {
        // Ramulator2 instance creation failed -- timing/power queries
        // will use DRAMArchitectureV2 fallback values instead.
        ramulator_memory_system_.reset();
    }
}

bool RamulatorWrapper::send(Address addr, MemoryRequestType type,
                             std::function<void(Address)> callback) {
    // Track statistics
    if (type == MemoryRequestType::READ) {
        total_reads_++;
    } else if (type == MemoryRequestType::WRITE) {
        total_writes_++;
    }

    // If Ramulator is available, use it
    if (ramulator_memory_system_) {
        Ramulator::Request req = createRamulatorRequest(addr, type);

        // Set up callback to track completion
        req.callback = [this, addr, callback](Ramulator::Request& completed_req) {
            handleRequestCompletion(completed_req);
            if (callback) {
                callback(addr);
            }
        };

        bool accepted = ramulator_memory_system_->send(req);

        if (accepted && callback) {
            // Track pending request
            pending_requests_.push_back({addr, type, current_cycle_, callback});
        }

        return accepted;
    } else {
        // Fallback: accept all requests immediately
        if (callback) {
            callback(addr);
        }
        return true;
    }
}

bool RamulatorWrapper::canAccept() const {
    if (ramulator_memory_system_) {
        // Ramulator checks internally if it can accept more requests
        // We assume it can if queue is not full (checked in send())
        return true;
    }
    return true;
}

void RamulatorWrapper::tick() {
    current_cycle_++;

    if (ramulator_memory_system_) {
        ramulator_memory_system_->tick();
    }

    // Tick PIM components
    if (pim_enabled_ && pim_plugin_) {
        pim_plugin_->tick();
    }
}

Ramulator::Request RamulatorWrapper::createRamulatorRequest(
    Address addr, MemoryRequestType type) {

    int req_type = (type == MemoryRequestType::READ) ?
                   Ramulator::Request::Type::Read :
                   Ramulator::Request::Type::Write;

    return Ramulator::Request(static_cast<Ramulator::Addr_t>(addr), req_type);
}

void RamulatorWrapper::handleRequestCompletion(Ramulator::Request& req) {
    /* 1.11.57 (latent D009): this is where the row-buffer counters were meant
     * to be updated, and the "placeholder for now" that stood here was the
     * whole of their implementation -- so getRowHits()/getRowMisses()/
     * getRowConflicts() returned 0 for the lifetime of every wrapper while
     * four consumers treated them as measurements. The counters are gone
     * (see ramulator_wrapper.h); this hook stays because it is the callback
     * Ramulator2 invokes on completion and a real instance would need it.
     * There is nothing to read yet: no Ramulator2 instance is ever created in
     * this tree, because every construction site passes an empty config path.
     * Whoever gives this wrapper a live instance should populate the row
     * statistics from req/the controller HERE, and should add the counters
     * back at the same time rather than reviving the empty ones. */
    (void)req;
}

double RamulatorWrapper::getReadEnergy() const {
    updateEnergyMetrics();
    return cached_read_energy_;
}

double RamulatorWrapper::getWriteEnergy() const {
    updateEnergyMetrics();
    return cached_write_energy_;
}

double RamulatorWrapper::getActivationEnergy() const {
    // Energy for row activations (ACT command)
    // Based on DRAM power model from literature and DRAM architecture
    //
    // DDR4 typical values from NVIDIA-HPCA17, DAS-MICRO15:
    //   - Row activation: ~2-3 nJ per activation
    //   - Includes: wordline driver, bitline precharge, sense amp settling
    //
    // HBM2: Lower due to shorter bitlines and TSV architecture (~1-1.5 nJ)

    double activation_energy_per_op_nJ = 2.5;  // Default DDR4 value

    // Use actual energy from DRAM architecture if available
    if (dram_arch_) {
        // Bank energy includes activation + column access
        // Estimate activation is ~60% of total bank energy
        activation_energy_per_op_nJ = dram_arch_->energy.bank_energy_pJ * 0.6 / 1000.0;
    }

    /* 1.11.57 (latent D009): the activation count came from row_misses_ +
     * row_conflicts_, two counters that were never incremented, so the sum was
     * always 0 and the "if no statistics" branch was the ONLY branch: every
     * activation count this function ever returned was accesses/2, i.e. a
     * hardcoded 50% row-miss rate wearing the clothes of a measurement. The
     * run does measure the row-miss fraction, it just arrives by another road
     * (setRowMissFraction, from the PE memory interface); use it, and fall
     * back to the stated 0.5 only when the run carried no measurement -- the
     * same convention arrayReadNJ() uses for the same quantity. */
    const double miss_frac = (row_miss_frac_ >= 0.0 && row_miss_frac_ <= 1.0)
                             ? row_miss_frac_ : 0.5;
    double estimated_activations =
        static_cast<double>(total_reads_ + total_writes_) * miss_frac;

    return estimated_activations * activation_energy_per_op_nJ;
}

double RamulatorWrapper::getPrechargeEnergy() const {
    // Energy for row precharges (PRE command)
    // Based on DRAM power model from literature
    //
    // DDR4 typical values:
    //   - Precharge: ~1.5-2 nJ per precharge
    //   - Includes: bitline discharge, sense amp reset
    //
    // HBM2: Lower due to TSV architecture (~0.8-1 nJ)

    double precharge_energy_per_op_nJ = 1.8;  // Default DDR4 value

    // Use actual energy from DRAM architecture if available
    if (dram_arch_) {
        // Estimate precharge is ~40% of total bank energy
        precharge_energy_per_op_nJ = dram_arch_->energy.bank_energy_pJ * 0.4 / 1000.0;
    }

    // Estimate number of precharges (roughly equal to activations)
    // 1.11.57 (latent D009): same substitution as getActivationEnergy above --
    // the two counters this used to read were structurally 0.
    const double miss_frac = (row_miss_frac_ >= 0.0 && row_miss_frac_ <= 1.0)
                             ? row_miss_frac_ : 0.5;
    double estimated_precharges =
        static_cast<double>(total_reads_ + total_writes_) * miss_frac;

    return estimated_precharges * precharge_energy_per_op_nJ;
}

double RamulatorWrapper::getRefreshEnergy() const {
    // Energy for DRAM refresh operations
    // Based on JEDEC specs and literature
    //
    // DDR4 refresh parameters:
    //   - tREFI = 7.8us (average refresh interval)
    //   - Each refresh activates one row per bank
    //   - Energy per refresh ~= activation energy
    //
    // Refresh energy = (num_refreshes) x (energy_per_refresh) x (num_banks)

    double refresh_energy_per_row_nJ = 2.0;  // Default: similar to activation

    // Use DRAM architecture energy if available
    if (dram_arch_) {
        // Refresh energy is similar to activation (same operation, different trigger)
        refresh_energy_per_row_nJ = dram_arch_->energy.bank_energy_pJ * 0.6 / 1000.0;
    }

    /* 1.11.57 (latent D014): tREFI IS PER TECHNOLOGY, and this function used
     * DDR4's 7.8 us for all seven. pimid_energy.h's IDD table has carried a
     * trefi_ns column since 1.9.10 -- DDR5 and HBM refresh every 3.9 us, GDDR6
     * every 1.9 us -- so an HBM3 run counted half the refreshes it should and
     * a GDDR6 run counted a quarter. It was invisible because this function's
     * only caller is getTotalEnergy(), whose only caller is printStats() and
     * an unreachable PowerModelManager; no reported refresh number comes from
     * here (the reported one is getRefreshPowerMW(), which reads the same
     * per-tech column correctly). Read the column instead of restating one
     * technology's value as if it were all of them.
     *
     * RESIDUAL, stated: current_cycle_ is this wrapper's own tick count, and
     * nothing in this tree ticks it, so refresh_count is 0 in practice. The
     * clock domain is the DRAM clock from the architecture object, which is
     * the domain tREFI is specified in. */
    const double trefi_ns =
        Ramulator::pimid_energy::iddFor(energyKey(), temperature_k_).trefi_ns;   // 1.11.65 (temp), 1.11.66 (grade key)

    double clock_period_ns = 1.0;  // Default 1ns modeling cycle
    if (dram_arch_) {
        clock_period_ns = 1000.0 / dram_arch_->timing.clock_freq_mhz;
    }

    double tREFI_cycles = (trefi_ns > 0.0 && clock_period_ns > 0.0)
                          ? (trefi_ns / clock_period_ns) : 0.0;
    if (!(tREFI_cycles >= 1.0)) return 0.0;
    uint64_t refresh_count = current_cycle_ / static_cast<uint64_t>(tREFI_cycles);

    // Total refresh energy = refreshes x banks x energy_per_refresh
    uint32_t total_banks = banks_per_rank_ * ranks_per_channel_ * channels_;
    return refresh_count * total_banks * refresh_energy_per_row_nJ;
}

double RamulatorWrapper::getLeakagePower() const {
    updateEnergyMetrics();
    return cached_leakage_power_;
}

double RamulatorWrapper::getTotalEnergy() const {
    /* 1.11.57 (latent D015): ACTIVATE AND PRECHARGE ARE NOT ADDED HERE ANY
     * MORE, because they were already inside the read and write terms. All
     * three come from the same source: updateEnergyMetrics() sets the per
     * access read energy to bank_energy_pJ x 64 B, which is the FULL access --
     * activate, column access and precharge -- while getActivationEnergy() and
     * getPrechargeEnergy() return a further 0.6 and 0.4 of the same
     * bank_energy_pJ per row miss. Summing all four charged the array roughly
     * twice for every row miss. main.cpp's own intensive path states the rule
     * this now follows ("getArrayReadEnergyNJ folds activation and column
     * access, so act/pre are NOT added separately"); this function predates it
     * and contradicted it. It was invisible because getTotalEnergy() is
     * reached only from printStats() (no callers) and from
     * PowerModelManager::... (never instantiated, and which additionally reads
     * this nJ value into a variable named total_energy_j).
     *
     * The two accessors are kept -- DRAMModel::tick() reports them as their own
     * line items, which is legitimate -- they simply must not be summed with a
     * total that already contains them. */
    /* 1.11.57 (latent D015, second unit): the leakage term was
     * getLeakagePower() * current_cycle_ / 1e6, which is 1000x too small.
     * getLeakagePower() returns MILLIWATTS and this total is in NANOJOULES:
     * mW x ns = 1e-3 W x 1e-9 s = 1e-12 J = 1e-3 nJ, so the divisor is 1e3,
     * not 1e6. The cycle period is the DRAM clock the wrapper's other
     * cycle-domain arithmetic uses, rather than an unstated 1 ns. */
    double clock_period_ns = 1.0;
    if (dram_arch_ && dram_arch_->timing.clock_freq_mhz > 0.0)
        clock_period_ns = 1000.0 / dram_arch_->timing.clock_freq_mhz;
    const double leakage_nJ =
        getLeakagePower() * (static_cast<double>(current_cycle_) *
                             clock_period_ns) / 1000.0;
    return getReadEnergy() + getWriteEnergy() + getRefreshEnergy() + leakage_nJ;
}

Cycle RamulatorWrapper::getAverageLatency() const {
    if (total_reads_ + total_writes_ == 0) {
        return 0;
    }

    // Calculate average DRAM latency based on row buffer hit/miss behavior
    // This is a realistic calculation based on DRAM timing parameters
    //
    // Row Hit: tCAS (Column Access Strobe) - ~13-15ns for DDR4-2400
    // Row Miss (Conflict): tRP + tRCD + tCAS - ~40ns for DDR4-2400
    //
    // Formula: avg_latency = hit_rate * hit_latency + miss_rate * miss_latency

    // Get DRAM timing parameters from architecture if available
    // For DDR4-2400: tCAS=13.32ns, tRCD=13.32ns, tRP=13.32ns
    // For HBM2: tCAS=12.5ns, tRCD=12.5ns, tRP=12.5ns
    double tCAS_cycles = 16;   // ~13.32ns @ 1.2GHz for DDR4-2400
    double tRCD_cycles = 16;   // ~13.32ns @ 1.2GHz
    double tRP_cycles = 16;    // ~13.32ns @ 1.2GHz

    // If DRAM architecture is available, use actual timing
    if (dram_arch_) {
        double clock_period_ns = 1000.0 / dram_arch_->timing.clock_freq_mhz;
        tCAS_cycles = std::ceil(dram_arch_->timing.tCAS_ns / clock_period_ns);
        tRCD_cycles = std::ceil(dram_arch_->timing.tRCD_ns / clock_period_ns);
        tRP_cycles = std::ceil(dram_arch_->timing.tRP_ns / clock_period_ns);
    }

    /* 1.11.57 (latent D009): the hit rate came from row_hits_ against three
     * counters that were never incremented, so the "use actual statistics from
     * Ramulator" branch could never be taken and every average latency this
     * function returned used the 0.5 in the else. The measured quantity is
     * row_miss_frac_, set by the caller from the run's own PE-memory-interface
     * row statistics; use it, and keep 0.5 only as the stated fallback for a
     * run that carried no measurement. */
    double hit_rate = (row_miss_frac_ >= 0.0 && row_miss_frac_ <= 1.0)
                      ? (1.0 - row_miss_frac_) : 0.5;

    // Row hit latency (just column access)
    Cycle hit_latency = static_cast<Cycle>(tCAS_cycles);

    // Row miss/conflict latency (precharge + activate + column)
    Cycle miss_latency = static_cast<Cycle>(tRP_cycles + tRCD_cycles + tCAS_cycles);

    // Weighted average latency
    double avg_latency = hit_rate * hit_latency + (1.0 - hit_rate) * miss_latency;

    return static_cast<Cycle>(avg_latency);
}

// ---------------------------------------------------------------------------
// 1.9.10: JEDEC IDD/VDD table + intensive per-access / background accessors.
//
// Root cause of the historical "0.000 nJ/access" in the power report:
//   (1) getReadEnergy()/getWriteEnergy() return total_reads_ * per-access, and
//       runPowerAnalysis queried them on a FRESH standalone oracle that never
//       processes accesses (total_reads_ == 0) while treating the result as a
//       per-access value; and
//   (2) updateEnergyMetrics() early-returns when current_cycle_==last_energy_update_
//       which is 0==0 on that oracle, so nothing is ever computed.
// The DRAM-arch presets DO carry non-zero bank energy (DDR5 1.6, HBM3 0.8
// pJ/byte), so the array term always existed -- it was just never surfaced as an
// intensive quantity. The accessors below fix that and add IDD-based interface
// (off-chip I/O) and background+refresh power, so a run yields real numbers.
// 1.9.10: the intensive DRAM energy physics has been RELOCATED into Ramulator2
// (external/ramulator/src/dram/pimid_energy.h). These accessors are now thin
// readers -- they forward the wrapper's own timing getters + the user override
// knobs into the Ramulator2-resident model. main.cpp is untouched (same API).
double RamulatorWrapper::getArrayReadEnergyNJ() const {
    return Ramulator::pimid_energy::arrayReadNJ(
        energyKey(), getTRC(), getTRAS(), getTBurst(), energy_bank_override_pJ_per_byte_,
        device_width_,        // 1.11.46 (L181): whole-rank basis
        row_miss_frac_,       // 1.11.52 (D003): measured row-miss fraction
        /* 1.11.86 (R6-11): banks per DEVICE, so the activate term can subtract
         * the one-bank standby the IDD0 loop actually runs at rather than the
         * all-bank IDD3N the datasheet specifies. */
        static_cast<int>(getBanksPerBankGroup() * getBankGroupsPerChip()));
}
double RamulatorWrapper::getArrayWriteEnergyNJ() const {
    return Ramulator::pimid_energy::arrayWriteNJ(
        energyKey(), getTRC(), getTRAS(), getTBurst(), energy_bank_override_pJ_per_byte_,
        device_width_,        // 1.11.46 (L181)
        row_miss_frac_,       // 1.11.52 (D003)
        /* 1.11.86 (R6-11): banks per DEVICE, so the activate term can subtract
         * the one-bank standby the IDD0 loop actually runs at rather than the
         * all-bank IDD3N the datasheet specifies. */
        static_cast<int>(getBanksPerBankGroup() * getBankGroupsPerChip()));
}
double RamulatorWrapper::getTerminationEnergyNJ(bool is_write) const {
    /* 1.11.40 (audit N8, user ruling: harness the model, do not table the
     * answer). The DQ termination energy now comes from CACTI-IO -- a real
     * off-chip IO model built from extracted parameters -- instead of the
     * hand-written SSTL/POD/LVSTL scheme table in pimid_energy.h.
     *
     * WHY THE SWITCH MATTERS, measured at gate 1155b (pJ/bit, this term only):
     *     tech     hand table   CACTI-IO   ratio
     *     DDR3       4.7508      (model)    ~2.6x on the FULL interface
     *     DDR4       2.5568                 ~2.1x
     *     DDR5       1.4323                 ~3.3x
     *     LPDDR5     0.0349      4.9547     142x on the full interface
     *
     * 1.11.57 (latent D010): NOT ONE OF THOSE FOUR ROWS REPRODUCES ANY MORE.
     * They are kept above as the historical measurement they are -- gate 1155b
     * ran against the 1.11.40 scheme table -- and must not be read as a
     * current property of the code. Recomputed from today's terminationNJ()
     * and today's rate table:
     *     DDR3   (1.35/2)^2 / 74 / 1.6e9   = 3.848  pJ/bit, not 4.7508
     *            (4.7508 was the pre-1.11.46 SSTL-15 1.5 V part; 1.11.46 moved
     *             DDR3 to the 1.35 V DDR3L the IDD row already described)
     *     DDR4   0.5*1.2^2 / 88 / 2.4e9    = 3.409  pJ/bit, not 2.5568
     *            (2.5568 is the 3200 MT/s figure; 1.11.52 moved DDR4 to the
     *             DDR4-2400 part this tree simulates)
     *     DDR5   0.5*1.1^2 / 88 / 3.2e9    = 2.148  pJ/bit, not 1.4323
     *            (1.4323 is the 4800 MT/s figure; 1.11.57 C001/D017 moved DDR5
     *             to the simulated DDR5_3200AN preset)
     *     LPDDR5 0.5*0.5^2 / 280 / 6.4e9   = 0.0698 pJ/bit, not 0.0349
     *            (exactly 2x: the 1.11.26 LVSTL rework introduced the 0.5 HIGH
     *             duty this row lacked)
     * Three of the four moved because the SPEED BIN was corrected, which is
     * the point: a measured comparison table is only valid against the inputs
     * it was measured with, and this one outlived three of them.
     *
     * The 142x that the paragraph below builds on therefore rests on a
     * superseded LPDDR5 row. The ARGUMENT survives -- 0.0698 pJ/bit against
     * CACTI-IO's 4.9547 is still 71x, and still a number with no physical
     * meaning for a mobile DQ interface -- but the factor is 71, not 142.
     * All of this was invisible because it is a source comment: nothing prints
     * it and no emitted value is derived from it.
     * The DDR gaps are the hand table modelling ONLY termination while the
     * interface also burns driver-switching and PHY energy. LPDDR5's gap (71x
     * on today's numbers, 142x as originally measured) is a different failure:
     * LVSTL exists precisely to eliminate static termination current, so the
     * one term the table modelled is the one term LVSTL makes negligible, and
     * a per-bit termination energy of 0.07 pJ is a number with no physical
     * meaning for a DQ interface. Real mobile DRAM interfaces sit near
     * 3-6 pJ/bit.
     *
     * TERM-BY-TERM, not aggregate: this call is the TERMINATION line, so it
     * takes CACTI-IO's termination component alone. Substituting the model's
     * full interface total here would silently fold driver and PHY energy into
     * a quantity named "termination" and make the swap uncheckable.
     *
     * The explicit override still wins, and CACTI-IO refusing (GDDR6 has no
     * parameter set) falls back to the scheme table with that stated -- a
     * refusal must not silently become zero. */
    /* 1.11.57 (latent D017): the transfer rate is handed to the scheme table
     * instead of the scheme table keeping its own copy. dramRateMTs() is the
     * one place in the tree that answers "what rate is this part", and it is
     * already what the bandwidth, the burst time and the CACTI-IO termination
     * figure come from -- the duplicate column inside pimid_energy.h had
     * already drifted to DDR5-4800 after C001 moved this table to the
     * simulated DDR5_3200AN preset. */
    const double rate = modelledRateMTs();
    if (energy_term_override_pJ_per_bit_ >= 0.0)
        return Ramulator::pimid_energy::terminationNJ(
            dram_type_, energy_term_override_pJ_per_bit_, rate, is_write);

    const int    ndq  = PIMID::CactiIOWrapper::dramChannelWidthBits(dram_type_);
    if (rate > 0.0 && ndq > 0) {
        /* activity = 1.0: this is an energy PER ACCESS, so the interface is
         * active for the whole of it. Averaging by a duty cycle here would
         * charge the access for the idle time around it. */
        PIMID::LinkIOResult io =
            PIMID::CactiIOWrapper::computeDramIO(dram_type_, ndq, rate, 1.0);
        /* ONLY AN EXACT MAP MAY REPLACE A RESULT. DDR3/DDR4/DDR5 map exactly
         * onto CACTI-IO parameter sets. LPDDR5 and HBM do not -- they borrow
         * LPDDR2 and WideIO, whose parameters were fitted for different
         * interfaces, and the borrowed numbers do not survive a sanity check
         * (LPDDR5 comes out near 34 pJ/bit against a published 3-6). Letting an
         * approximate map rewrite a result would replace one unsourced number
         * with another and call it a model. Those technologies keep the
         * existing path; the model's figure is reported as a cross-check
         * elsewhere, not substituted here. */
        /* 1.11.63 (R7): the WRITE figure is the wrapper's long-standing
         * iostate=WRITE computation; the READ figure is the second
         * extio_power_term() pass. Direction-selected here. */
        const double term_pj = is_write ? io.energy_pj_per_bit_term
                                        : io.energy_pj_per_bit_term_rd;
        if (io.valid && io.exact_map && term_pj > 0.0)
            return term_pj * 512.0 / 1000.0;   // per 64 B
    }
    /* Fall back, and say so rather than reporting zero. */
    static bool warned = false;
    if (!warned) {
        warned = true;
        std::cerr << "[power] NOTE: CACTI-IO has no parameter set for '"
                  << dram_type_ << "'; DQ termination falls back to the "
                     "pimid_energy scheme table (termination term only)."
                  << std::endl;
        /* 1.11.52 (audit D008) asked that the unsourced input be named where
         * it is used; 1.11.63 retires the disclosure because the input is no
         * longer unsourced. JESD209-5C (in misc/ since 2026-08-24), Table 84
         * p.144: DQ ODT OP[2:0] "000B: Disable (Default)". The JEDEC default
         * operating point is UNTERMINATED, so the model's default termination
         * term is zero BY CITATION, and the old "which end of the 30-240 ohm
         * host range" question dissolves -- the ladder RZQ/1..RZQ/6
         * (240/120/80/60/48/40 ohm) is a controller option with no default
         * rung. Users modelling an ODT-on system state their operating point
         * via power.termination_pj_per_bit. */
        if (dram_type_ == "LPDDR5") {
            std::cerr << "[power] NOTE: LPDDR5 DQ ODT is DISABLED by JEDEC "
                         "default (JESD209-5C Tbl 84 p.144: 000B Disable "
                         "(Default)); termination DC energy = 0. The RZQ/1..6 "
                         "ladder (240..40 ohm, RZQ=240) is a controller option "
                         "-- set power.termination_pj_per_bit to model an "
                         "ODT-on operating point. Micron IDD conditions are "
                         "ODT-off, so array energy describes this same "
                         "default configuration." << std::endl;
        }
    }
    return Ramulator::pimid_energy::terminationNJ(
        dram_type_, energy_term_override_pJ_per_bit_, rate, is_write);   // 1.11.57 (D017) + R7
}

bool RamulatorWrapper::getTerminationEnergyBandNJ(double& lo_nj, double& hi_nj,
                                                  std::string& provenance) const {
    /* 1.11.59 introduced this accessor to report LPDDR5 termination as a BAND
     * (0.036..0.143 nJ per 64 B), because the tree could not source Rtt: the
     * only citable range was host-side RODT(DQ) 30..240 ohm (Intel 743844-015
     * Tbl 89, typical column empty), a 4.0x spread in loop resistance.
     *
     * 1.11.63 RETIRES the band, because the question is now answered by the
     * standard itself. JESD209-5C (misc/, acquired 2026-08-24), Table 84
     * p.144: DQ ODT OP[2:0] "000B: Disable (Default)"; the ladder is RZQ/1..
     * RZQ/6 with RZQ = 240 ohm and 111B RFU, and NT-ODT likewise defaults
     * off. The JEDEC default operating point is UNTERMINATED -- below the old
     * band's floor, not inside it. The default termination term is therefore
     * ZERO with a citation, not a range with a caveat; the scheme table
     * (pimid_energy.h) encodes it as rtt = 0 and the LVSTL branch prices no
     * DC loop. A user modelling an ODT-on system states the operating point
     * via power.termination_pj_per_bit -- a stated configuration, and a band
     * wrapped around a user-stated point would be noise, not provenance.
     *
     * Returns false always: no technology reports a termination band any
     * more. Kept (rather than deleted) so the consumer sites in main.cpp
     * document why no band line is printed. */
    lo_nj = hi_nj = 0.0;
    provenance.clear();
    return false;
}

/* 1.11.57 (latent D011), SUPERSEDED BY 1.11.58: both are wired in now.
 *
 * The block below described a real defect -- these two had no caller, so the
 * 1.11.40 interface correction reached no reported number and the DQ
 * interface was termination-only in every result. 1.11.58 consumed both at
 * device and system scope, so that is no longer true: driver+PHY and the IO
 * area now enter the reported energy and the reported area, and the split is
 * printed. The one thing the original note said that STILL holds is the
 * reach: these return zero unless CACTI-IO has an exact parameter map, which
 * exists for DDR3, DDR4, DDR5 and GDDR6 -- and not for LPDDR5 (Rtt
 * unsourced; see the band accessor above) or HBM2/HBM3 (no electrical row at
 * all; HBM specifies driver strength in mA, not ohms, and its interface is
 * unterminated by design). The historical note is kept below for the record.
 *
 * ---- as written at 1.11.57 ----
 * THESE TWO ARE COMPUTED AND NOBODY READS THEM.
 *
 * getInterfaceDynamicEnergyNJ() and getInterfaceAreaMM2() have no caller
 * anywhere in src/ or include/ -- grep finds only these definitions and their
 * declarations in ramulator_wrapper.h. The two reported DQ-interface numbers,
 * main.cpp's device-scope and system-scope interface lines, both consume
 * getTerminationEnergyNJ() alone. So the DQ interface in every PIMID result is
 * still TERMINATION ONLY, and the 1.11.40 correction these functions carry --
 * that driver switching and PHY are the terms that matter, and that leaving
 * them out understates LPDDR5's interface by roughly two orders of magnitude
 * (see the measured table above) -- is present in the code and absent from
 * every number the simulator prints.
 *
 * They are kept, not deleted: they are a correct model of a term PIMID does
 * not yet charge, and deleting them would delete the correction rather than
 * land it. What is fixed here is the silence -- an interface-energy header
 * that advertises a fix no reported number receives is worse than no header.
 * Wiring them in belongs in main.cpp (an owner of that file must add them to
 * the interface line and re-derive, because it MOVES every DRAM interface
 * energy the corpus reports), which is why it is not done here. */
double RamulatorWrapper::getInterfaceDynamicEnergyNJ() const {
    /* 1.11.40: driver switching + PHY, per 64 B. PIMID has never modelled
     * these -- the interface was termination-only -- so this is new accounting
     * rather than a re-attribution. Zero when CACTI-IO has no parameter set,
     * which is honest: we do not know it, rather than it being absent.
     * UNUSED AND UNVALIDATED as of 1.11.57 -- see the note above. */
    const double rate = modelledRateMTs();
    const int    ndq  = PIMID::CactiIOWrapper::dramChannelWidthBits(dram_type_);
    if (rate <= 0.0 || ndq <= 0) return 0.0;
    PIMID::LinkIOResult io =
        PIMID::CactiIOWrapper::computeDramIO(dram_type_, ndq, rate, 1.0);
    if (!io.valid || !io.exact_map) return 0.0;   // exact maps only, as above
    const double non_term = io.energy_pj_per_bit - io.energy_pj_per_bit_term;
    return (non_term > 0.0) ? non_term * 512.0 / 1000.0 : 0.0;
}

double RamulatorWrapper::getInterfaceAreaMM2() const {
    /* 1.11.40: IO area, wired in at 1.11.58 (see the D011 note above).
     *
     * 1.11.60 (audit round 4, C009): TWO ZEROS, ONE RETURN VALUE, AND ONE OF
     * THEM HAD TO BE SAID OUT LOUD.
     *
     * This returned 0.0 for "CACTI-IO has no exact parameter map for this
     * technology" and 0.0 for "CACTI-IO computed the area and then WITHHELD
     * it because the run is above the polynomial's validity crossover". They
     * are not the same fact. The second is GDDR6's case and only GDDR6's:
     * its bus_freq is rate/2 = 7000 MHz against the 3162 MHz crossover, while
     * its exact_map IS true (POD135 electricals, sourced), so it receives the
     * driver+PHY ENERGY term from this very call and loses the AREA from it.
     * The consumer's gate is `if (io_area > 0.0)`, so it printed nothing at
     * all -- and a GDDR6 RANK or HOST_MC cell's system-comparable area total
     * omitted the DQ interface silicon with no line saying so, beside a DDR4
     * cell that adds 2.444 mm^2/die and prints one. The correction 1.11.58
     * landed to stop the interface being invisible was invisible again for
     * one technology.
     *
     * It cannot be repaired by returning a number: the polynomial genuinely
     * has nothing credible to say at 7000 MHz, and substituting the
     * extrapolation is what the withholding exists to prevent. So the zero
     * stands and the run STATES which zero it is, once per wrapper, from
     * here -- the consumer's gate cannot say it, because the consumer never
     * enters the branch. interfaceAreaWithheld() exposes the same distinction
     * to a caller that wants to test rather than read. */
    const double rate = modelledRateMTs();
    const int    ndq  = PIMID::CactiIOWrapper::dramChannelWidthBits(dram_type_);
    if (rate <= 0.0 || ndq <= 0) return 0.0;
    PIMID::LinkIOResult io =
        PIMID::CactiIOWrapper::computeDramIO(dram_type_, ndq, rate, 1.0);
    if (!io.valid || !io.exact_map) return 0.0;
    if (io.io_area_withheld) {
        io_area_withheld_ = true;
        if (!warned_io_area_withheld_) {
            warned_io_area_withheld_ = true;
            std::cerr << "[power] WARNING: " << dram_type_
                      << " DQ-interface AREA is WITHHELD, not zero. CACTI-IO's "
                         "area polynomial is valid to 3162 MHz and this "
                         "interface runs at " << (rate / 2.0)
                      << " MHz, where the cubic term dominates and the value "
                         "is an extrapolation. The interface ENERGY from the "
                         "same evaluation is reported normally (the power path "
                         "does not use that polynomial); only the area is "
                         "missing. Any area total for this technology omits "
                         "the DQ interface silicon -- it is incomplete, not "
                         "small. " << io.not_modelled << std::endl;
        }
        return 0.0;
    }
    io_area_withheld_ = false;
    return io.io_area_mm2;
}
double RamulatorWrapper::getRefreshTempFactor() const {
    return Ramulator::pimid_energy::refreshTempFactor(dram_type_, temperature_k_);
}

double RamulatorWrapper::getRefreshPowerMW() const {
    return Ramulator::pimid_energy::refreshMW(energyKey(), temperature_k_);   // 1.11.65 (temp), 1.11.66 (grade key)
}
double RamulatorWrapper::getBackgroundPowerMW() const {
    return Ramulator::pimid_energy::backgroundMW(energyKey(), temperature_k_);   // 1.11.65 (temp), 1.11.66 (grade key)
}
int RamulatorWrapper::getBackgroundUnits(const std::string& device_width,
                                         int ranks_per_channel,
                                         int channels) const {
    return Ramulator::pimid_energy::backgroundUnits(dram_type_, device_width,
                                                    ranks_per_channel, channels);  // 1.11.20 D13 / 1.11.52 A015
}
double RamulatorWrapper::getBackgroundSystemMW(double r_idle, bool pg_enabled,
                                               const std::string& device_width,
                                               int ranks_per_channel,
                                               int channels) const {
    /* 1.11.56 (audit D006): NAME THE UNSOURCED COLUMN WHERE IT GOVERNS A
     * PRINTED NUMBER. pimid_energy.h's header calls the whole IDD table
     * part-number-sourced, but idd2p is not one of the sourced columns: the
     * HBM rows were entered as a "30-40% of IDD2N" rule of thumb (and HBM2's
     * own entry is 41.2%, outside that band), and the rest are rounded
     * figures, not datasheet reads. idd2p is only reachable through the
     * power-down state, so it changes nothing unless power gating is on AND
     * the run measured some idle residency -- which is exactly when the
     * Background line stops being a pure IDD2N/IDD3N number. Say so once,
     * there, instead of leaving the disclosure in a comment in another repo. */
    if (pg_enabled && r_idle > 0.0) {
        static bool warned_idd2p = false;
        if (!warned_idd2p) {
            warned_idd2p = true;
            std::cerr << "[power] NOTE: memory.power_down is on and the run "
                         "measured idle residency, so the Background line for '"
                      << dram_type_ << "' rests on the APPROXIMATE IDD2P "
                         "(precharge power-down) column of pimid_energy.h. That "
                         "column is not part-number-sourced -- the HBM entries "
                         "are a 30-40%-of-IDD2N rule of thumb -- so treat the "
                         "idle share of Background as an assumption, not a "
                         "datasheet value." << std::endl;
        }
    }
    return Ramulator::pimid_energy::backgroundSystemMW(energyKey(), r_idle,
                                                       pg_enabled, device_width,
                                                       ranks_per_channel, channels,
                                                       temperature_k_);   // 1.11.66
}

void RamulatorWrapper::updateEnergyMetrics() const {
    if (current_cycle_ == last_energy_update_ && last_energy_update_ != 0) {
        return;  // Already updated this cycle (do NOT skip the initial cycle-0 pass)
    }

    // Energy per memory access, from the intensive IDD path in
    // external/ramulator/src/dram/pimid_energy.h (see below).

    /* 1.11.57 (latent D012 + D013): THE TOOL ANSWERS BOTH OF THESE.
     *
     * D013: write energy was read x 1.2 under a comment reading "writes are
     * typically 15-20% higher than read" -- the same unsourced ratio 1.11.5
     * removed from the intensive path when it wired writes to IDD4W. The
     * wrapper already exposes the IDD-derived pair, so the ratio does not have
     * to be asserted: getArrayWriteEnergyNJ()/getArrayReadEnergyNJ() is the
     * ratio JEDEC's own IDD4W/IDD4R implies for this technology (about 1.07 on
     * DDR4's row, not 1.20). Take the pair directly.
     *
     * D012: the leakage literals contradicted the comment above them by 100x.
     * The comment states "DDR4: ~80-100 mW/GB, HBM2: ~50-60 mW/GB" and the
     * code assigned 0.85 and 0.55 mW/GB -- two orders of magnitude apart, with
     * no way to tell which was meant. Neither is measured, and the tool has a
     * real answer: standby power for the whole memory system is exactly what
     * pimid_energy's IDD path computes (backgroundSystemMW), and it is per
     * device, not per gigabyte. Derive the per-GB figure from the sourced
     * IDD2N/IDD3N background instead of choosing between two literals that
     * disagree with each other.
     *
     * Both were invisible because cached_write_energy_ and
     * cached_leakage_power_ leave this class only through getWriteEnergy() and
     * getLeakagePower(), whose callers are DRAMModel::tick() -- never called --
     * and printStats(), which has no callers. */
    const double BYTES_PER_ACCESS = 64.0;
    double read_energy_per_access  = getArrayReadEnergyNJ();
    double write_energy_per_access = getArrayWriteEnergyNJ();
    double capacity_gb = capacity_ / (1024.0 * 1024.0 * 1024.0);

    if (dram_arch_ && read_energy_per_access <= 0.0) {
        /* The IDD path refused (no row for this technology); the architecture
         * object's bank energy is the remaining tool-sourced figure. */
        read_energy_per_access =
            dram_arch_->energy.bank_energy_pJ * BYTES_PER_ACCESS / 1000.0;
        write_energy_per_access = read_energy_per_access;
    }

    /* Background power for the memory system this wrapper describes, divided
     * by its capacity -- a derived mW/GB, with the derivation visible. r_idle
     * = 0 and power gating off: this is the standby floor, not a residency
     * weighted figure, which is what a leakage line means. */
    double leakage_power_mw = getBackgroundSystemMW(0.0, false, device_width_,
                                                    ranks_per_channel_,
                                                    channels_);

    // Calculate total energy
    cached_read_energy_ = total_reads_ * read_energy_per_access;
    cached_write_energy_ = total_writes_ * write_energy_per_access;

    /* 1.11.57 (D012): mW for the memory system as configured. The old form
     * was capacity_gb x a literal mW/GB whose two candidate values differed
     * by 100x; capacity_gb is kept only to report the implied per-GB figure
     * to anyone who wants it. */
    (void)capacity_gb;
    cached_leakage_power_ = leakage_power_mw;

    last_energy_update_ = current_cycle_;
}

void RamulatorWrapper::printStats() const {
    std::cout << "\n=== Ramulator DRAM Statistics ===" << std::endl;
    std::cout << "Total Reads:     " << total_reads_ << std::endl;
    std::cout << "Total Writes:    " << total_writes_ << std::endl;
    /* 1.11.57 (latent D009): the Row Hits / Row Misses / Row Conflicts lines
     * and the hit rate derived from them are gone. They printed three counters
     * that were never incremented, so this block reported "Row Hit Rate: 0%"
     * for every run that had any traffic at all -- a fabricated statistic in a
     * block headed "Statistics". What the wrapper actually knows about row
     * behaviour is the miss fraction the caller measured and handed in, and
     * whether it was measured at all. */
    if (row_miss_frac_ >= 0.0 && row_miss_frac_ <= 1.0) {
        std::cout << "Row Miss Frac:   " << row_miss_frac_
                  << " (measured by the caller)" << std::endl;
    } else {
        std::cout << "Row Miss Frac:   not measured in this run" << std::endl;
    }

    std::cout << "\n=== Energy Metrics ===" << std::endl;
    std::cout << "Read Energy:     " << getReadEnergy() << " nJ" << std::endl;
    std::cout << "Write Energy:    " << getWriteEnergy() << " nJ" << std::endl;
    std::cout << "Activation:      " << getActivationEnergy() << " nJ" << std::endl;
    std::cout << "Precharge:       " << getPrechargeEnergy() << " nJ" << std::endl;
    std::cout << "Refresh:         " << getRefreshEnergy() << " nJ" << std::endl;
    std::cout << "Leakage Power:   " << getLeakagePower() << " mW" << std::endl;
    std::cout << "Total Energy:    " << getTotalEnergy() << " nJ" << std::endl;

    // If Ramulator is active, print its statistics
    if (ramulator_memory_system_) {
        std::cout << "\n=== Ramulator Detailed Statistics ===" << std::endl;
        // Ramulator's finalize() prints statistics
        // We don't call it here to avoid ending simulation
    }
}

void RamulatorWrapper::resetStats() {
    total_reads_ = 0;
    total_writes_ = 0;
    cached_read_energy_ = 0.0;
    cached_write_energy_ = 0.0;
    cached_leakage_power_ = 0.0;
    last_energy_update_ = 0;
    pending_requests_.clear();

    // Reset PIM stats
    if (pim_enabled_ && pim_plugin_) {
        pim_plugin_->resetStats();
    }
}

// ============================================================================
// PIM-Specific Methods
// ============================================================================

void RamulatorWrapper::enablePIMSupport(const std::string& dram_type) {
    if (pim_enabled_) {
        std::cout << "PIM support already enabled!\n";
        return;
    }

    dram_type_ = dram_type;
    pim_enabled_ = true;

    std::cout << "Enabling PIM support for " << dram_type_ << "...\n";

    // Create DRAM architecture based on type
    // 1.11.59 (audit C018): a fresh object carries its factory organization,
    // so nothing is stamped on it yet; the width is re-applied below.
    arch_device_width_bits_ = 0;
    if (dram_type_ == "DDR3") {
        dram_arch_ = pimid::memory::createDDR3_1600_Verified();   // 1.11.68
        std::cout << "Using DDR3-1600H architecture specs\n";
    } else if (dram_type_ == "LPDDR5") {
        dram_arch_ = pimid::memory::createLPDDR5_6400_Verified(); // 1.11.69
        std::cout << "Using LPDDR5-6400 architecture specs\n";
    } else if (dram_type_ == "GDDR6") {
        dram_arch_ = pimid::memory::createGDDR6_14000_Verified(); // 1.11.70
        std::cout << "Using GDDR6-14000 architecture specs\n";
    } else if (dram_type_ == "DDR4") {
        dram_arch_ = pimid::memory::createDDR4_2400_Verified();
        std::cout << "Using DDR4-2400 architecture specs\n";
    } else if (dram_type_ == "DDR5") {
        dram_arch_ = pimid::memory::createDDR5_4800_Verified();
        std::cout << "Using DDR5-4800 architecture specs\n";
        std::cout << "  Key DDR5 features: 16n prefetch, 8 bank groups, dual subchannels\n";
    } else if (dram_type_ == "HBM2") {
        dram_arch_ = pimid::memory::createHBM2_Verified();
        std::cout << "Using HBM2 architecture specs\n";
    } else if (dram_type_ == "HBM3") {
        dram_arch_ = pimid::memory::createHBM3_Verified();
        std::cout << "Using HBM3 architecture specs\n";
        std::cout << "  Key HBM3 features: 4.0 GT/s, 16 pseudo-channels, 512 GB/s peak\n";
    } else {
        std::cerr << "Unknown DRAM type: " << dram_type_ << ", using DDR4\n";
        dram_arch_ = pimid::memory::createDDR4_2400_Verified();
    }

    // 1.11.59 (audit C018): this path builds a NEW architecture object, so the
    // configured device width has to be stamped onto it again.
    applyDeviceWidthToArchitecture();
    /* 1.11.61 (ruling R1): and so does the preset density -- a fresh factory
     * object carries the factory literals, which are DDR4's for the three
     * technologies with no object of their own. */
    resolvePresetOrganization();
    applyPresetDensityToArchitecture();
    applyPresetBankGroupingToArchitecture();   // 1.11.72
    /* 1.11.63 (R6-5): same for the ns bin -- a fresh factory object carries the
     * factory literals here too. */
    resolvePresetTiming();
    applyPresetTimingsToArchitecture();

    // Initialize PIM components
    initializePIMComponents();

    std::cout << "PIM support enabled!\n";
}

void RamulatorWrapper::initializePIMComponents() {
    if (!dram_arch_) {
        std::cerr << "ERROR: DRAM architecture not initialized!\n";
        return;
    }

    // Create bandwidth tracker
    bandwidth_tracker_ = std::make_shared<PIMBandwidthTracker>(dram_arch_);

    // Create PIM controller plugin
    pim_plugin_ = std::make_shared<PIMControllerPlugin>(dram_arch_, dram_type_);

    /* 1.11.57 (latent D019): ASK THE ACCESSORS THAT ARE RIGHT HERE.
     *
     * This block hardcoded "int num_subarrays = 16;  // Typical" and, below,
     * "8  // chips per rank (typical)" -- two organization facts stated as
     * literals a few hundred lines above getSubarraysPerBank() and
     * getChipsPerRank(), which resolve exactly those quantities from the
     * architecture object this function has already dereferenced. The literals
     * are wrong for most of the technologies this wrapper serves: HBM2/HBM3
     * are single-die-per-channel parts, and the subarray count is per
     * technology in the architecture object. It was invisible because
     * initializePIMComponents() runs only from enablePIMSupport(), which has
     * no callers anywhere in the tree -- the PIM plugin, the bandwidth tracker
     * and the internal network are never constructed in a PIMID run. */
    int num_subarrays = static_cast<int>(getSubarraysPerBank());
    int num_bank_groups = dram_arch_->organization.bank_groups_per_chip;
    int num_banks = dram_arch_->organization.banks_per_bank_group * num_bank_groups;
    int chips_per_rank = static_cast<int>(getChipsPerRank());

    bandwidth_tracker_->initialize(
        channels_,
        ranks_per_channel_,
        num_bank_groups,
        num_banks,
        num_subarrays
    );

    pim_plugin_->initialize(
        channels_,
        ranks_per_channel_,
        num_bank_groups,
        num_banks,
        num_subarrays
    );

    // Create internal network
    internal_network_ = createInternalDRAMNetwork(
        dram_type_,
        num_subarrays,
        dram_arch_->organization.banks_per_bank_group,
        num_bank_groups,
        chips_per_rank   // 1.11.57 (latent D019): was the literal 8
    );

    std::cout << "PIM components initialized:\n";
    std::cout << "  Channels: " << channels_ << "\n";
    std::cout << "  Ranks: " << ranks_per_channel_ << "\n";
    std::cout << "  Bank Groups: " << num_bank_groups << "\n";
    std::cout << "  Banks: " << num_banks << "\n";
    std::cout << "  Subarrays: " << num_subarrays << "\n";
    std::cout << "  Chips per rank: " << chips_per_rank << "\n";
}

bool RamulatorWrapper::sendPIM(Address addr, MemoryRequestType type,
                              PIMRequestPayload* pim_payload,
                              std::function<void(Address)> callback) {
    if (!pim_enabled_) {
        std::cerr << "ERROR: PIM support not enabled! Call enablePIMSupport() first.\n";
        return false;
    }

    if (!pim_payload) {
        std::cerr << "ERROR: PIM payload is null!\n";
        return false;
    }

    // Create Ramulator request with PIM payload
    Ramulator::Request req = createPIMRequest(addr, type, pim_payload);

    // Set up callback
    req.callback = [this, addr, callback, pim_payload](Ramulator::Request& completed_req) {
        handleRequestCompletion(completed_req);

        // Calculate total latency including PIM-specific components
        uint64_t total_latency = completed_req.depart - completed_req.arrive;
        total_latency += pim_payload->data_movement_cycles;
        total_latency += pim_payload->network_cycles;

        // Call user callback
        if (callback) {
            callback(addr);
        }

        // Call PIM completion callback
        if (pim_payload->pim_completion_callback) {
            pim_payload->pim_completion_callback();
        }
    };

    // Send to Ramulator
    if (ramulator_memory_system_) {
        bool accepted = ramulator_memory_system_->send(req);
        if (accepted && callback) {
            pending_requests_.push_back({addr, type, current_cycle_, callback});
        }
        return accepted;
    }

    // Fallback
    if (callback) {
        callback(addr);
    }
    return true;
}

Ramulator::Request RamulatorWrapper::createPIMRequest(
    Address addr, MemoryRequestType type, PIMRequestPayload* pim_payload) {

    int req_type = (type == MemoryRequestType::READ) ?
                   Ramulator::Request::Type::Read :
                   Ramulator::Request::Type::Write;

    Ramulator::Request req(static_cast<Ramulator::Addr_t>(addr), req_type);

    // Attach PIM payload
    req.m_payload = static_cast<void*>(pim_payload);

    return req;
}

void RamulatorWrapper::registerPE(PIMGranularity granularity, int pe_id, int target_bank) {
    if (!pim_enabled_) {
        std::cerr << "WARNING: PIM support not enabled!\n";
        return;
    }

    if (pim_plugin_) {
        pim_plugin_->registerPE(granularity, pe_id, target_bank);
    }
}

double RamulatorWrapper::getBandwidthLimit(PIMGranularity granularity) const {
    if (pim_enabled_ && pim_plugin_) {
        return pim_plugin_->getBandwidthLimit(granularity);
    }
    return 0.0;
}

int RamulatorWrapper::getPortBitwidth(PIMGranularity granularity) const {
    if (pim_enabled_ && pim_plugin_) {
        return pim_plugin_->getPortBitwidth(granularity);
    }
    return 0;
}

double RamulatorWrapper::getEffectiveBandwidthPerPE(PIMGranularity granularity,
                                                   int target_id) const {
    if (pim_enabled_ && pim_plugin_) {
        return pim_plugin_->getEffectiveBandwidthPerPE(granularity, target_id);
    }
    return 0.0;
}

// ============================================================================
// Subarray-Level Characteristics
// ============================================================================

/* 1.11.63 (R6-5): THE DDR3 LINE IS GONE FROM ALL FOUR OF THESE.
 *
 * DDR3 borrowed the DDR4 architecture object as an organization proxy when
 * this note was written (it has owned its own since 1.11.68), so its
 * TIMINGS used to be intercepted here -- and the values intercepted with were
 * DDR3-1600K's (nCL 11 -> 13.75 ns), while the preset this tree simulates is
 * DDR3_1600H (nCL 9 at tCK 1250 ps -> 11.25 ns). One part, two bins, 22%
 * apart, and the wrong one was the one that reached getTRC() and the array
 * activate energy.
 *
 * applyPresetTimingsToArchitecture() now stamps the DDR3_1600H bin onto the
 * borrowed object itself, so DDR3 reads its own preset's timings through the
 * ordinary dram_arch_ path and there is no second table to drift. The
 * interception had to go with it: an early return here would have made the
 * stamp inert.
 *
 * GDDR6 and LPDDR5 KEEP theirs, and the reason is in
 * applyPresetTimingsToArchitecture(): their Ramulator timing rows carry two
 * different clocks, so their cycle counts cannot be read as ns at all. These
 * values are cited to JEDEC/vendor sources instead, which is the honest
 * substitute for a preset that cannot answer.
 *
 * The trailing DDR4-2400 fallbacks (reached only with no architecture object,
 * which no path in this tree produces) now carry the derived bin figures --
 * 16 x 833 ps and 39 x 833 ps -- so no literal in this file states a bin the
 * preset already fixes. */
double RamulatorWrapper::getTRCD() const {
    /* The two short-circuits below predate 1.11.69/70. They were written when
     * GDDR6 and LPDDR5 had no object of their own and borrowed DDR4's as an
     * organization proxy, so reading dram_arch_ would have returned DDR4's
     * timing; taking preset_timing_ directly was the fix. Both now own an
     * object, and since 1.11.77 (R6-8) that object is stamped from the same
     * preset, so the two paths return the same number. The short-circuits are
     * kept because they are the more direct route to the same value and
     * because they still hold if an object is ever missing; they are no
     * longer the only thing standing between these technologies and DDR4's
     * timing. */
    /* 1.11.66 (round 5, F3/A4): DERIVED from the preset now that the preset
     * is on the right clock. The 14.8 literal this replaces WAS the vendor
     * value (Samsung K4Z Table 91 tRCDRD 15 ns at 14 Gbps) -- and the
     * derivation reproduces it: nRCDRD 26 x 0.571 = 14.85 ns. Until this
     * release the derivation could not be used because the preset sat at a
     * wrong 1000 ps clock (it would have given 26.0); with the 1.11.66 clock
     * correction the literal and the derivation agree, so R6 applies. */
    if (dram_type_ == "GDDR6")
        return preset_timing_.derivable() ? preset_timing_.nRCD * preset_timing_.tCK_ns() : 14.8;
    /* 1.11.63 (R6, gate 1173C): the literal 18.0 froze this quantity against
     * the preset -- the JESD209-5C corrections landed in the preset row and
     * MOVED NOTHING because this hardcode, not the row, feeds every LPDDR5
     * cycle and array-energy consumer (the arch object is a DDR4 stand-in
     * for this technology). Derived now: nRCD x tCK from the transcribed,
     * self-checked preset row (15 x 1.25 = 18.75 ns; JEDEC min 18 ns plus
     * JEDEC rounding). Fallback keeps the old literal only if the
     * transcription is somehow absent, and says nothing new then. */
    if (dram_type_ == "LPDDR5")
        return preset_timing_.valid ? preset_timing_.nRCD * preset_timing_.tCK_ns()
                                    : 18.0;
    if (dram_arch_) {
        return dram_arch_->timing.tRCD_ns;
    }
    return 16 * 0.833;  // DDR4_2400R nRCD 16 x tCK 833 ps
}

double RamulatorWrapper::getTCAS() const {
    /* 1.11.66: derived -- RL 24 x 0.571 = 13.7 ns (Samsung Table 91: RL 24
     * tCK). The old 14.8 literal was tRCDRD's value reused for CAS. */
    if (dram_type_ == "GDDR6")
        return preset_timing_.derivable() ? preset_timing_.nCL * preset_timing_.tCK_ns() : 14.8;
    /* 1.11.63 (R6, gate 1173C): same cure as getTRCD. nCL 17 x 1.25 =
     * 21.25 ns (JESD209-5C Tbl 225 row 1011B RL Set 0) -- the old literal
     * 18.0 predates the sourced RL and matched nothing. */
    if (dram_type_ == "LPDDR5")
        return preset_timing_.valid ? preset_timing_.nCL * preset_timing_.tCK_ns()
                                    : 18.0;
    if (dram_arch_) {
        return dram_arch_->timing.tCAS_ns;
    }
    return 16 * 0.833;  // DDR4_2400R nCL 16 x tCK 833 ps
}

double RamulatorWrapper::getTRP() const {
    /* 1.11.46 (L170): per-tech, same precedence as getTRCD/getTRAS -- tRC
     * (= tRAS + tRP) feeds the array-energy activate term. Same sources as
     * getTRAS above. */
    if (dram_type_ == "GDDR6")   // 1.11.66: derived -- nRP 26 x 0.571 = 14.85 (Samsung tRP 15 ns)
        return preset_timing_.derivable() ? preset_timing_.nRP * preset_timing_.tCK_ns() : 14.8;
    /* 1.11.63 (R6, gate 1173C): derived -- nRPpb x tCK (15 x 1.25 = 18.75). */
    if (dram_type_ == "LPDDR5")
        return preset_timing_.valid ? preset_timing_.nRP * preset_timing_.tCK_ns()
                                    : 18.0;
    if (dram_arch_) {
        return dram_arch_->timing.tRP_ns;
    }
    return 16 * 0.833;  // DDR4_2400R nRP 16 x tCK 833 ps
}

double RamulatorWrapper::getTRAS() const {
    /* 1.11.46 (FIX-PRE-FLEET L170): the same per-tech precedence getTRCD has.
     * DDR3/LPDDR5/GDDR6 BORROWED the DDR4-2400 arch struct as an ORGANIZATION
     * proxy when this was written -- all three have owned their own object
     * since 1.11.68/69/70, and since 1.11.77 (R6-8) every one of those objects
     * is stamped from its own preset, so the borrowing this note describes is
     * history. The per-tech precedence below is kept for the reason in
     * getTRCD(). Its TIMINGS fed the array-energy formulas (idd0*tRC -
     * idd3n*tRAS ...), pricing three technologies' arrays on a fourth's
     * clock. Values: DDR3-1600K from JESD79-3D (in hand, normative);
     * LPDDR5-6400 from the Micron datasheets in hand (tRAS min 42 ns);
     * GDDR6 from the same 16 Gb/s vendor-spec class getTRCD already cites --
     * a single-point source, flagged as such. */
    if (dram_type_ == "GDDR6")   // 1.11.66: derived -- nRAS 53 x 0.571 = 30.3 (Samsung tRAS 30 ns; old 28.0 was SK hynix's)
        return preset_timing_.derivable() ? preset_timing_.nRAS * preset_timing_.tCK_ns() : 28.0;
    /* 1.11.63 (R6, gate 1173C): derived -- nRAS x tCK (34 x 1.25 = 42.5;
     * the old 42.0 was the JEDEC ns min without the CK rounding). */
    if (dram_type_ == "LPDDR5")
        return preset_timing_.valid ? preset_timing_.nRAS * preset_timing_.tCK_ns()
                                    : 42.0;
    if (dram_arch_) {
        return dram_arch_->timing.tRAS_ns;
    }
    return 39 * 0.833;  // DDR4_2400R nRAS 39 x tCK 833 ps
}

/* 1.11.57 (latent D020): getTRRD() is DELETED. It returned tRAS/4 under the
 * word "Approximation" -- an invented relation between two JEDEC timings that
 * are independently specified (DDR4-2400's tRRD_S is 3.3 ns against a tRAS of
 * 32 ns, so the quarter rule overstates it by 2.4x) -- and it had no callers
 * anywhere in src/, include/ or tools/: only its own definition and its
 * declaration in the header. Nothing could read the wrong number, which is why
 * it survived four releases of timing work. Deleted rather than corrected,
 * because the correct value is a per-technology JEDEC figure this wrapper does
 * not carry, and a caller who needs tRRD should have it sourced then, not
 * inherit a division that looks like one. */
double RamulatorWrapper::getTRC() const {
    // tRC = tRAS + tRP
    return getTRAS() + getTRP();
}

double RamulatorWrapper::getTBurst() const {
    /* 1.11.46 (L170/L168): burst time is beats/rate -- specification
     * arithmetic, not a table: DDR3-1600 BL8 -> 5.0 ns; LPDDR5-6400 BL16 ->
     * 2.5 ns; GDDR6-14000 BL16 -> 1.14 ns. The DDR4-2400 fallback priced all
     * three at 3.33 ns, and the SPEED BIN now matches the IDD row's part for
     * each technology (L168). */
    /* 1.11.52 (audit D002): DDR4 and DDR5 join the same rule. The list
     * above covered three technologies and left DDR4/DDR5 on the
     * architecture object's tBurst, which is the DDR4-2400 bin (3.33 ns) --
     * while BOTH DDR4 termination paths (CACTI-IO's dramRateMTs and the
     * pimid_energy scheme table) price the same part at 3200 MT/s. One
     * part, two speed bins, and the array/termination split of a single
     * access disagreed by 1.33x. Burst time is beats/rate arithmetic from
     * the SAME rate table the termination path uses, so the two halves of
     * an access can no longer be priced at different rates. */
    if (dram_type_ == "GDDR6")  return 16.0 * 1000.0 / 14000.0;  // 1.143 ns
    if (dram_type_ == "LPDDR5") return 16.0 * 1000.0 / 6400.0;   // 2.5 ns
    if (dram_type_ == "DDR3")   return  8.0 * 1000.0 / 1600.0;   // 5.0 ns
    /* 1.11.52 (D002, resolved at the RATE): DDR4/DDR5 keep the architecture
     * object's burst, because the fix belongs one level up -- the rate table
     * now names the part this tree actually simulates, so array, termination
     * and bandwidth all derive from one number. */
    if (dram_arch_) {
        return dram_arch_->timing.tBurst_ns;
    }
    return 3.33;  // DDR4-2400 default (8-beat burst @ 2400 MT/s)
}

uint32_t RamulatorWrapper::getSubarraysPerBank() const {
    if (dram_arch_) {
        return dram_arch_->organization.subarrays_per_bank;
    }
    return 4;  // Typical default
}

uint32_t RamulatorWrapper::getBanksPerBankGroup() const {
    if (dram_arch_) {
        return dram_arch_->organization.banks_per_bank_group;
    }
    return 4;  // DDR4 default
}

uint32_t RamulatorWrapper::getBankGroupsPerChip() const {
    if (dram_arch_) {
        return dram_arch_->organization.bank_groups_per_chip;
    }
    return 4;  // DDR4 default
}

uint32_t RamulatorWrapper::getChipsPerRank() const {
    if (dram_arch_) {
        return dram_arch_->organization.chips_per_rank;
    }
    return 8;  // x8 DDR4 default
}

uint32_t RamulatorWrapper::getRanksPerChannel() const {
    if (dram_arch_) {
        return dram_arch_->organization.ranks_per_channel;
    }
    return ranks_per_channel_;
}

uint64_t RamulatorWrapper::getSubarraySizeKB() const {
    if (dram_arch_) {
        return dram_arch_->organization.subarray_size_kb;
    }
    return 512;  // 512 KB typical
}

uint64_t RamulatorWrapper::getBankSizeMB() const {
    if (dram_arch_) {
        return dram_arch_->organization.bank_size_mb;
    }
    return 2;  // 2 MB typical for DDR4
}

uint64_t RamulatorWrapper::getChipSizeMB() const {
    if (dram_arch_) {
        return dram_arch_->organization.chip_size_mb;
    }
    return 128;  // 128 MB (1 Gb chip) typical
}

/* 1.11.60 (ONE FABRIC, user requirement: "the table is legit with accurate
 * references to the upstream tools/models"): per-rung provenance, taken from
 * the architecture object's OWN VerifiedValue source strings -- the same
 * struct that carries each width -- never restated here. Rung 2 (bank group)
 * has no upstream field and is the declared x2 interleaving assumption from
 * 1.11.58; rung 6 is derived arithmetic and says so. */
std::string RamulatorWrapper::getLadderRungProvenance(int rung) const {
    auto fmt = [](const memory::VerifiedValue& v) -> std::string {
        const char* st =
            v.status == memory::VerificationStatus::VERIFIED  ? "VERIFIED"  :
            v.status == memory::VerificationStatus::INFERRED  ? "INFERRED"  :
            v.status == memory::VerificationStatus::ESTIMATED ? "ESTIMATED" : "UNKNOWN";
        return std::string(st) + ": " + v.source;
    };
    if (!dram_arch_) return "NO ARCHITECTURE OBJECT: per-technology placeholder table";
    switch (rung) {
        case 0: return fmt(dram_arch_->datapath.gsa_datapath_bits);
        case 1: return fmt(dram_arch_->datapath.bank_serialization_bits);
        case 2: return "ASSERTED: bank serialization x 2, an interleaving "
                       "assumption -- no upstream field exists (declared since "
                       "1.11.58; also in the stated-constant register)";
        case 3: return fmt(dram_arch_->datapath.chip_io_bits);
        case 4: return fmt(dram_arch_->datapath.rank_databus_bits);
        case 5: return fmt(dram_arch_->datapath.channel_databus_bits);
        case 6: return "DERIVED: channel_databus_bits x the technology's "
                       "channel count (arithmetic, not a sourced field)";
    }
    return "unknown rung";
}

int RamulatorWrapper::getSubarrayPortBits() const {
    if (dram_arch_) {
        // DRAMArchitectureV2 uses datapath stages
        return dram_arch_->datapath.gsa_datapath_bits.value_bits;  // GSA width
    }
    return 256;  // DDR4 default (256 bits from GSA)
}

int RamulatorWrapper::getBankPortBits() const {
    if (dram_arch_) {
        return dram_arch_->datapath.bank_serialization_bits.value_bits;
    }
    return 8;  // DDR4 default (NARROW!)
}

int RamulatorWrapper::getBankGroupPortBits() const {
    /* 1.11.57 (latent D020, PROMOTED -- this one is LIVE): the x2 below is an
     * unsourced multiplier, and since 1.11.56 it reaches a reported number.
     * The audit recorded it as latent on the grounds that this accessor had no
     * callers; that stopped being true when buildHierarchy started taking the
     * per-level link ladder from the architecture object (src/main.cpp, the
     * "Hierarchy link ladder from the <tech> architecture object" line), where
     * w[2] is this value. It is left AT ITS PRESENT VALUE deliberately -- a
     * silent change here would move every hierarchy level-2 link width and
     * bandwidth in the corpus -- but it no longer passes as sourced.
     *
     * What it is: the architecture object carries no bank-group datapath
     * field. The only sourced neighbour is the bank serialization width, and
     * the x2 asserts that a bank group's port is twice a bank's. That is a
     * plausible reading of bank-group interleaving and it is not a
     * specification value; nothing in JEDEC fixes a bank-group port width,
     * because a bank group is not an interface boundary. The honest fix is a
     * bank_group_port_bits field in DRAMArchitectureV2 with a source string
     * per technology, which is a change to the architecture objects and to
     * every number they feed. */
    static bool announced = false;
    if (!announced) {
        announced = true;
        std::cerr << "[mem] NOTE: the bank-group port width is bank "
                     "serialization width x 2 -- an UNSOURCED multiplier, not "
                     "a JEDEC value; a bank group is not an interface boundary "
                     "and no specification fixes its port width. It sets "
                     "hierarchy level 2's link width and bandwidth."
                  << std::endl;
    }
    if (dram_arch_) {
        /* 1.11.68: A TECHNOLOGY WITH NO BANK GROUPS GETS NO BANK-GROUP
         * MULTIPLIER. DDR3 has eight banks sitting directly on the chip;
         * bank groups arrive with DDR4. Doubling a bank's port to model the
         * port of a structure the part does not contain is a different error
         * from the unsourced-multiplier one described above, and it is one
         * the object itself can settle: when bank_groups_per_chip is 1 the
         * rung is a pass-through. This reaches ONLY DDR3 -- DDR4 has 4 bank
         * groups, DDR5 and both HBM stacks 8 -- so no adopted ladder moves. */
        if (dram_arch_->organization.bank_groups_per_chip <= 1) {
            return dram_arch_->datapath.bank_serialization_bits.value_bits;
        }
        return dram_arch_->datapath.bank_serialization_bits.value_bits * 2;
    }
    return 16;  // DDR4 default (8-bit bank serialization x 2)
}

int RamulatorWrapper::getChipIOBits() const {
    if (dram_arch_) {
        return dram_arch_->datapath.chip_io_bits.value_bits;
    }
    return 8;  // x8 DDR4 default
}

int RamulatorWrapper::getRankDataBits() const {
    if (dram_arch_) {
        return dram_arch_->datapath.rank_databus_bits.value_bits;
    }
    return 64;  // DDR4 default (8 x x8 chips)
}

/* 1.11.57 (audit C007): ONE CHANNEL for every family. The HBM objects used to
 * hold the whole stack in this field, so this accessor answered "the channel"
 * with 8 or 16 channels' worth and the ladder's channel rung sat 8x/16x above
 * the rung beneath it. The aggregate is channel x channels, which is what the
 * system root is for.
 *
 * ONE THING THIS DOES NOT FIX, stated so nobody reads the ladder as finished:
 * the system root (L6) is built by the caller as this width x
 * hierarchy_channels_per_system, which is the multi-DEVICE count and is 1 for
 * a single stack -- the DRAM channel count is hierarchy_dram_channels. With
 * the stack no longer smuggled into the channel rung, L6 is now one channel
 * wide on HBM rather than the stack. Correcting it means multiplying by the
 * DRAM channel count at that site, which is src/main.cpp's to make. */
int RamulatorWrapper::getChannelDataBits() const {
    if (dram_arch_) {
        return dram_arch_->datapath.channel_databus_bits.value_bits;
    }
    return 64;  // DDR4 default
}

/* 1.11.57 (audit C004): WHICH PART is this wrapper's architecture object
 * describing? For DDR3, LPDDR5 and GDDR6 the answer is "DDR4", because no
 * object exists for them and the DDR4-2400 one is read instead. A caller that
 * is about to attribute widths, bandwidths or a link ladder to the technology
 * it asked for can compare this against that technology first. */
std::string RamulatorWrapper::getArchitectureTechnology() const {
    if (dram_arch_) return dram_arch_->technology;
    return "";
}

double RamulatorWrapper::getSubarrayBandwidth() const {
    if (dram_arch_) {
        // Subarray bandwidth limited by GSA width
        double gsa_bits = dram_arch_->datapath.gsa_datapath_bits.value_bits;
        double clock_mhz = dram_arch_->timing.clock_freq_mhz;
        return (gsa_bits / 8.0) * (clock_mhz / 1000.0);  // GB/s
    }
    // Calculate from defaults: 256 bits @ 1.2 GHz
    return (256.0 / 8.0) * 1.2;  // 38.4 GB/s internal
}

/* 1.11.57 (audit C003): these two used to return a stored literal while their
 * five neighbours in this block compute a width times a clock. The ladder in
 * main.cpp reads all seven and back-derives each rung's CLOCK as BW * 8 /
 * width, so a literal that had stopped tracking the core clock did not present
 * as a wrong bandwidth -- it presented as a wrong frequency, and then as a
 * wrong transfer time on every crossing of that rung. Derived at the source
 * now (DRAMArchitectureV2::getBankEffectiveBW), so the two rungs follow a
 * speed-bin change like the rest of the ladder. The no-object fallbacks below
 * are DDR4's own derivation, 8 bits / 8 x 1.2 GHz and its x2. */
double RamulatorWrapper::getBankBandwidth() const {
    if (dram_arch_) {
        // Bank bandwidth limited by the serialization path, at the core clock
        return dram_arch_->getBankEffectiveBW();
    }
    return 1.2;  // 8 bits / 8 x 1.2 GHz, the DDR4-2400 default
}

double RamulatorWrapper::getBankGroupBandwidth() const {
    if (dram_arch_) {
        return dram_arch_->getBankGroupEffectiveBW();
    }
    return 2.4;  // 16 bits / 8 x 1.2 GHz, the DDR4-2400 default
}

double RamulatorWrapper::getChipIOBandwidth() const {
    if (dram_arch_) {
        double io_bits = dram_arch_->datapath.chip_io_bits.value_bits;
        double data_rate = dram_arch_->timing.data_rate_mtps;
        return (io_bits / 8.0) * (data_rate / 1000.0);  // GB/s
    }
    return (8.0 / 8.0) * 2.4;  // 2.4 GB/s @ 2400 MT/s
}

double RamulatorWrapper::getRankBandwidth() const {
    if (dram_arch_) {
        return dram_arch_->getRankBW();
    }
    return (64.0 / 8.0) * 2.4;  // 19.2 GB/s for DDR4-2400
}

double RamulatorWrapper::getChannelBandwidth() const {
    if (dram_arch_) {
        double channel_bits = dram_arch_->datapath.channel_databus_bits.value_bits;
        double data_rate = dram_arch_->timing.data_rate_mtps;
        return (channel_bits / 8.0) * (data_rate / 1000.0);  // GB/s
    }
    return (64.0 / 8.0) * 2.4;  // 19.2 GB/s for DDR4-2400
}

double RamulatorWrapper::getSubarrayEnergyPerByte() const {
    if (dram_arch_) {
        return dram_arch_->energy.subarray_energy_pJ;
    }
    return 1.0;  // 1 pJ/byte DDR4 default
}

double RamulatorWrapper::getBankEnergyPerByte() const {
    if (dram_arch_) {
        return dram_arch_->energy.bank_energy_pJ;
    }
    return 2.0;  // 2 pJ/byte DDR4 default
}

/* 1.11.57 (latent D020): getBankGroupEnergyPerByte() is DELETED. It returned
 * the bank energy x 1.5 as "approximate as slightly higher than bank energy" --
 * a 50% surcharge for crossing a boundary that costs nothing in the energy
 * model, asserted with no source -- and it had no callers: only its definition
 * and its header declaration. The energy ladder that IS consumed
 * (architecture_extractor.h) reads the architecture object's own per-tier
 * fields, never this. A caller who needs bank-group energy should get a
 * sourced field on DRAMArchitectureV2, not a multiplier. */
double RamulatorWrapper::getChipEnergyPerByte() const {
    if (dram_arch_) {
        return dram_arch_->energy.chip_energy_pJ;
    }
    return 5.0;  // 5 pJ/byte DDR4 default
}

double RamulatorWrapper::getRankEnergyPerByte() const {
    if (dram_arch_) {
        return dram_arch_->energy.rank_energy_pJ;
    }
    return 10.0;  // 10 pJ/byte DDR4 default
}

/* 1.11.57 (latent D020): getChannelEnergyPerByte() is DELETED, for the same
 * reason as getBankGroupEnergyPerByte() above -- rank energy x 1.5 as
 * "channel energy includes rank + controller overhead", an unsourced 50%
 * controller allowance, with no callers anywhere in the tree. The memory
 * controller's energy is McPAT's to report, and it is reported there. */

/* 1.11.23: these four were CIRCULAR. Each returned the architecture field
 * that architecture_extractor.h assigns FROM it, so the "primary" branch was
 * a no-op that handed back a hand-written literal, while the real derivation
 * sat unreachable in the dram_arch_==null fallback. The fallback was the
 * correct path all along.
 *
 * Derived now, from the JEDEC timing Ramulator2 already parses and the energy
 * model already consumes -- no literal, no fallback asymmetry:
 *   subarray  tRCD + tCAS            row activate + column access
 *   bank      tRP + tRCD + tCAS      the row-miss path
 *   bankgroup bank                  floor: the real term is tCCD_L - tCCD_S,
 *                                    which this wrapper does not expose
 *   chip      bank + tBurst          the burst leaves through the chip I/O
 *
 * RESIDUAL, stated: the per-tech tRCD/tCAS/tRP values these compose from are
 * JEDEC speed-bin figures TRANSCRIBED into getTRCD()/getTCAS()/getTRP() with
 * citations (JESD250 for GDDR6, JESD209-5 tRCDpb for LPDDR5, DDR3-1600K,
 * DDR4-2400 CL17) rather than read from the Ramulator2 timing preset this
 * wrapper already names ("DDR4_2400R", "DDR3_1600H", ...). Sourced, but
 * transcribed; reading the preset directly is the follow-up. */
double RamulatorWrapper::getSubarrayAccessLatency() const {
    return getTRCD() + getTCAS();
}

double RamulatorWrapper::getBankAccessLatency() const {
    return getTRP() + getTRCD() + getTCAS();
}

double RamulatorWrapper::getBankGroupAccessLatency() const {
    /* The bank-group tier's only real cost is the longer same-group
     * column-to-column delay, tCCD_L - tCCD_S. This wrapper does not expose
     * tCCD, so that term is NOT AVAILABLE -- and the previous code covered
     * the gap with a 1.1 multiplier, i.e. asserted a 10% penalty with no
     * source, charged even to technologies that have no bank groups at all.
     * Report the bank latency, which is the correct FLOOR, rather than
     * manufacture a penalty. Wiring tCCD through is the follow-up. */
    return getBankAccessLatency();
}

double RamulatorWrapper::getChipAccessLatency() const {
    return getBankAccessLatency() + getTBurst();
}

double RamulatorWrapper::getRankAccessLatency() const {
    if (dram_arch_) {
        return dram_arch_->timing.rank_access_ns;
    }
    return 80.0;  // Typical DDR4
}

/* 1.11.57 (latent D020): getChannelAccessLatency() is DELETED. It returned the
 * rank access latency x 1.2 with the comment "Channel adds MC overhead" -- a
 * 20% controller penalty with no source, charged uniformly to every
 * technology -- and it had no callers. Note that getBankGroupAccessLatency()
 * two functions up already REFUSED the same shape of number in 1.11.23,
 * dropping its 1.1x and returning the bank latency as an honest floor with the
 * missing term named (tCCD_L - tCCD_S); this function kept the pattern that
 * one abandoned. The controller's queueing and overhead are modelled in the
 * hierarchy/NoC path and in McPAT, not by a multiplier on a JEDEC timing. */

const pimid::memory::DRAMArchitectureV2* RamulatorWrapper::getDRAMArchitecture() const {
    return dram_arch_.get();
}

} // namespace pimid
