// part_records.h -- shipped parameter records (the values that used to be
// bound into the code), loaded at startup.
//
// 1.11.94 (user ruling 2026-09-27 "we should not bind the sim and param hard",
// sweep-94 rulings 13a/16/22): a calibrated set is ONE record per part, all
// fields required, provenance beside each value, selectable or replaceable as
// a whole, never patched per field. 1.11.94 was STEP 1 of the two-step
// migration: the loader read the record AND cross-checked every field against
// the code tables that still held the same values. 1.11.101 is STEP 2: the
// code tables are gone and the record is the single source -- the CACTI
// wrapper's generation, feature size and density, the Ramulator wrapper's
// preset names, channel count and dies per stack, the energy model's refresh
// ladder and channel width, and the die population all read the registry
// below. A missing record, a missing field or an inconsistent record refuses.
#ifndef PIMID_PARAMS_PART_RECORDS_H
#define PIMID_PARAMS_PART_RECORDS_H

#include <map>
#include <string>
#include <vector>

namespace pimid {
namespace params {

/* One rung of a refresh ladder: tREFI is multiplied by `factor` when the
 * operating temperature is above `above_c` (JEDEC temperature-compensated
 * refresh); rungs are listed in ascending temperature. */
struct RefreshRung { double above_c; double factor; };

/* 1.11.104 (IDD-RECORDS step 1, user ruling (a) 2026-10-05): the part's
 * measured data, transcribed into the record with its source beside each
 * value. In this release the energy model still prices from its code table
 * and CROSS-CHECKS every record value against it at registration (a
 * mismatch refuses); step 2 (1.11.105) deletes the table. */
struct IddRow {
    double vdd = 0.0;
    double idd0 = 0.0, idd2n = 0.0, idd3n = 0.0, idd4r = 0.0, idd4w = 0.0, idd5 = 0.0, idd2p = 0.0;   // mA, the row's basis
    double trfc_ns = 0.0, trefi_ns = 0.0;
    std::string source;
    bool has_iddq = false; double iddq3n = -1.0, iddq4r = -1.0, iddq4w = -1.0, vddq = -1.0; std::string iddq_source;   // VDDQ rail, when published
    bool has_ipp = false;  double ipp2n = -1.0, ipp3n = -1.0, vpp = -1.0; std::string ipp_source;                      // VPP rail, when published
};
struct IddBlock {
    std::string basis;                 // "per device" | "per channel"
    int channels_basis = 1;            // the row's channel aggregation (HBM: the stack's channels)
    std::string provenance;            // MEASURED | CALIBRATED | DERIVED
    std::string idd3n_bank_state;      // ALL_BANKS | ONE_BANK | UNVERIFIED
    std::map<int, IddRow> by_grade;    // keyed by the data rate (DDR5: 3200 / 4800 / 5600); 0 = the single row
    double e_actpre_pj_override = -1.0;   // GDDR6: the IDD7-route activation energy, pJ
    double stack_floor_mw = -1.0;         // HBM: the per-stack static floor, mW
};
struct ComponentFactorsRec {           // typical / maximum per component; apply = false for a measured row
    bool apply = false;
    double standby = 1.0, standby_premium = 1.0, act = 1.0, burst_rd = 1.0, burst_wr = 1.0, refresh = 1.0, pd = 1.0;
    std::string source;
};
struct TerminationRec {                // DQ termination scheme and electricals; the formula stays in the tool
    std::string scheme;                // POD | SSTL | LVSTL | none
    double vddq = -1.0, ron_rd = 0.0, rtt_rd = 0.0, ron_wr = 0.0, rtt_wr = 0.0;
    std::string source;
};
struct IddqBandRec { bool valid = false; double lo = 0.0, hi = 0.0; std::string source; };   // HBM VDDQ I/O band, pJ/bit

/* The DRAM part record. Field names mirror the YAML keys. */
struct DramPartRecord {
    std::string file;              // the record file that was loaded
    std::string technology;        // DDR3 .. HBM3 (canonical PIMID name)
    std::string part;              // vendor part or family name
    std::string vendor;
    std::string generation;        // vendor generation label (3x/2x, 1z, 1a, ...)
    double feature_nm = 0.0;       // the generation's feature size F
    int cell_factor_f2 = 0;        // cell area in F^2 (6 = buried wordline)
    double density_mb_per_mm2 = 0; // measured full-die density
    std::string density_source;
    std::string organization_preset;   // Ramulator org preset name
    std::string timing_preset;         // Ramulator timing preset name
    int channels = 0;              // channels the preset instantiates (HBM: per stack; DDR5: the two 32-bit sub-channels of one DIMM channel)
    int channel_width_bits = 0;    // 1.11.101: JEDEC data width of ONE channel; the DDR family's devices per rank = this / device width
    int stack_dies = 0;            // HBM core dies per stack (0 for non-stacked)
    double die_capacity_gb = 0.0;  // HBM core die capacity (0 for non-stacked)
    std::vector<RefreshRung> refresh_ladder;
    IddBlock idd;                        // 1.11.104
    ComponentFactorsRec component_factors;
    TerminationRec termination;
    IddqBandRec iddq_band;
};

/* The directory the records are read from: $PIMID_PARAMS when set, else the
 * tree the binary was built from (PIMID_PARAMS_DIR, compiled in like
 * CACTI_DATA_DIR). */
std::string paramsDir();

/* Load params/dram/<tech>.yaml. Returns false with `error` set when the file
 * is missing, unreadable, or lacks a required field. Never substitutes. */
bool loadDramPartRecord(const std::string& tech, DramPartRecord& out, std::string& error);

/* 1.11.101 (step 2): the registry. checkDramPartRecords (main.cpp) loads the
 * record of every DRAM technology a run names and registers it; every former
 * code table reads it from here. dramPartRecord() REFUSES (exit 2) for a
 * technology no record was registered for: nothing is substituted. */
void registerDramPartRecord(const DramPartRecord& rec);
const DramPartRecord& dramPartRecord(const std::string& tech);
bool hasDramPartRecord(const std::string& tech);
/* The record's internal consistency (there is no table left to compare
 * with): the channel width is a power of two from 16 to 256 bits, the ladder
 * rises in temperature with factors in (0, 1], an HBM record's die capacity
 * equals the named preset's stack capacity over its dies (ruling 16), and the
 * channel count equals the IDD row's basis the energy model prices with. */
bool validateDramPartRecord(const DramPartRecord& rec, std::vector<std::string>& errors);

/* One line stating the record in force (the only print-out; derivations stay
 * in the record file and the code comments). */
std::string describeDramPartRecord(const DramPartRecord& rec);

/* 1.11.95: the cache part record (params/cache/default.yaml): the geometry
 * every cache level is built and priced with. banks < 0 = the slice rule. */
struct CacheLevelRecord { int ways = -1; int line_bytes = -1; int banks = -1; };
struct CacheRecord {
    std::string file, record;
    int slice_mb = 2;
    /* 1.11.101 (review-93 BIG-CACHE ruling (b)): a cache above max_slices x
     * slice_mb is REPLICATED slices -- area, leakage and energy scale with the
     * slice count; latency = one slice's CACTI access + the mean mesh hop
     * count to the home slice x home_hop_cycles. Up to the bound the slice
     * rule stays (one CACTI array of up to 32 banks). */
    int max_slices = 32;
    int home_hop_cycles = 2;
    CacheLevelRecord l1d, l1i, l2, l3;
    const CacheLevelRecord& level(const std::string& name) const;   // "l1d" | "l1i" | "l2" | "l3"
};
/* Loads params/cache/default.yaml (PIMID_PARAMS honoured); false + error on a
 * missing file or field, or a value outside its range. */
bool loadCacheRecord(CacheRecord& out, std::string& error);
/* The slice rule: clamp(size_kb / (slice_mb * 1024), 1, 32). */
int cacheBanksSlice(int size_kb, int slice_mb);
std::string describeCacheRecord(const CacheRecord& rec);

/* 1.11.97: the core part record (params/core/default.yaml): the front-end
 * penalties, wrong-path fetch width and retire width of the two decoded
 * timing cores. A field a core type does not carry stays -1. For in_order,
 * retire_width -1 means "issue" (retires what it issued). Derivations of
 * every value are in the record file. */
struct CoreTypeRecord {
    int mispredict_penalty_cycles = -1;   // mispredict: in_order execute-depth bubble; ooo TOTAL redirect cost from resolution (1.12.0)
    int resteer_penalty_cycles = -1;      // BTB/RAS target resteer, decode depth (in_order)
    int fetch_width_bytes = -1;           // wrong-path fetch bytes per cycle (ooo)
    int retire_width = -1;                // commit width McPAT prices; in_order: -1 = issue width
    /* 1.12.1 (ticket #114): the in-order element's branch predictor, "pag" |
     * "none" (in_order only; ooo always runs its PAg). A config sets it per
     * element (pim.pe.branch_predictor, devices[].pim.pe.branch_predictor,
     * hosts[].branch_predictor); this is the value where none is set. */
    std::string branch_predictor;
};
struct CoreRecord {
    std::string file, record;
    CoreTypeRecord in_order, ooo;
};
/* Loads params/core/default.yaml (PIMID_PARAMS honoured); false + error on a
 * missing file or field, a value outside its range, an in_order
 * branch_predictor that is not pag or none (1.12.1), or an ooo retire_width
 * that is not the timing core's compiled ROB retire width. */
bool loadCoreRecord(CoreRecord& out, std::string& error);
/* The ROB retire width zsim OOOCore is compiled with (ReorderBuffer<128, 4>,
 * external/zsim/src/ooo_core.h). Restated here only to refuse a record that
 * disagrees with the timing core; the template is the authority. */
constexpr int kOooRobRetireWidth = 4;
std::string describeCoreRecord(const CoreRecord& rec);

} // namespace params
} // namespace pimid

#endif
