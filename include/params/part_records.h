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
    /* 1.12.18: optional iddq.wr_includes_rtt_wr (+ wr_rtt_source):
     * IDDQ4W was measured with the DRAM's RTT_WR enabled, so the write DC
     * termination current is inside it. */
    bool iddq_wr_includes_rtt_wr = false; std::string iddq_wr_rtt_source;
    bool has_ipp = false;  double ipp2n = -1.0, ipp3n = -1.0, vpp = -1.0; std::string ipp_source;                      // VPP rail, when published
    double ipp0 = -1.0, ipp4r = -1.0, ipp4w = -1.0, ipp5b = -1.0;   // 1.12.18 (bug 3b): the VPP active rows, when published (optional in ipp)
    /* (1.12.79) optional idd.standby_clock: the standby currents (IDD2N/IDD3N and their IPP pair) of a row measured at a
     * faster clock than the one modelled, scaled by modelled / measured rate for the BACKGROUND only (power-down,
     * refresh and the activate/burst baselines keep the row). 1.0 = none. */
    double standby_clock_scale = 1.0; std::string standby_clock_source;
    /* 1.12.18 (bug 2b/8a): the I/O-equivalent share inside idd4r / idd4w of a
     * row that carries one (optional block idd.io_share). */
    bool has_io_share = false; double idd4r_io = -1.0, idd4w_io = -1.0; std::string io_share_provenance, io_share_source;
    /* 1.12.18 (user ruling 2026-10-23, "Re-derive I/O share"): an io_share ANCHORED on a published
     * I/O energy carries the anchor (pJ/bit per direction) and the channel rate its mA were computed at.
     * The loader checks idd4r_ma = anchor_read_pj_bit x channel_gbps / vdd (and the write) to 0.05 mA,
     * and the energy model prices a DQ-crossing access's I/O at this share. All four keys or none. */
    bool io_share_anchored = false; double io_anchor_rd_pj_bit = -1.0, io_anchor_wr_pj_bit = -1.0, io_channel_gbps = -1.0;
    std::string io_anchor_source;
};
/* (1.12.88): the part's IDD currents vs temperature (optional idd_temperature block). form LINEAR_STANDBY: the
 * standby currents move by a published slope in mA per degree C from the temperature they were measured at,
 * held at the ends of the measured range; IDD0 and IDD5 move with IDD2N, IDD4R and IDD4W with IDD3N (the
 * activate, burst and refresh excesses stay as measured). form STEP_ABOVE: above above_c each listed current
 * takes its published derating factor; IDD5 moves with IDD2N. No block: the currents are temperature-flat. */
struct IddTemperatureRec {
    bool valid = false;
    std::string form, provenance, source;
    double reference_c = 0.0, lo_c = 0.0, hi_c = 0.0;          // LINEAR_STANDBY
    double slope_idd2n = 0.0, slope_idd3n = 0.0, slope_idd2p = 0.0;   // mA per C, per the record's basis
    double above_c = 0.0;                                       // STEP_ABOVE
    double f_idd0 = 1.0, f_idd2n = 1.0, f_idd3n = 1.0, f_idd4r = 1.0, f_idd4w = 1.0, f_idd2p = 1.0;
};
struct IddBlock {
    std::string basis;                 // "per device" | "per channel"
    int channels_basis = 1;            // the row's channel aggregation (HBM: the stack's channels)
    std::string provenance;            // MEASURED | CALIBRATED | DERIVED
    std::string idd3n_bank_state;      // ALL_BANKS | ONE_BANK | UNVERIFIED
    std::map<int, IddRow> by_grade;    // keyed by the data rate (DDR5: 3200 / 4800 / 5600); 0 = the single row
    /* (1.12.48) (user ruling 2026-10-25): optional per-device-width rows (key "x4" | "x8" | "x16") of a single-grade
     * record: the single row with the width's own published columns replacing it (idd.by_width). */
    std::map<std::string, IddRow> by_width;
    double e_actpre_pj_override = -1.0;   // GDDR6: the IDD7-route activation energy, pJ
    /* user ruling 2026-10-25 (second supply rails): the VPP share of an e_actpre_pj_override activation, pJ per IDD
     * unit, on the same route (GDDR6: IDD7 - IPP3N), at the maximum (not scaled by the activate factor). Needs
     * e_actpre_pj_override and an ipp row; refused together with ipp.ipp0 (that would price the VPP activation twice). */
    double vpp_actpre_pj_override = -1.0;
    double stack_floor_mw = -1.0;         // HBM: the per-stack static floor, mW
    /* 1.12.18 (user ruling 2026-10-24, option 5): WHAT LOOP THE ROW'S idd0 IS. HBM records
     * must say ONE_PC_PER_TRC: one 1 KB pseudo-channel activation per tRC per channel, the
     * loop the SAFARI base ran. The JEDEC IDD0 loops (JESD235D Table 60, JESD238B.01
     * Table 85) activate PC0 AND PC1 every nRC, i.e. two pages per tRC; such a value
     * must be converted before it is entered (the loader refuses any other label). */
    std::string idd0_loop;
    /* 1.12.18 (user ruling 2026-10-24): HBM activation VPP priced as a RATIO of the VDD
     * activate energy (Xilinx XPE 2023.1.2 HBM_Data VPP Activate / VDD activate (read)),
     * DERIVED; alt_* is the other XPE stack column, printed beside it. Refused together
     * with an ipp.ipp0 row (that would price the VPP activation twice). */
    bool has_vpp_act = false;
    double vpp_act_ratio = -1.0, vpp_act_alt_ratio = -1.0;
    std::string vpp_act_device, vpp_act_alt_device, vpp_act_provenance, vpp_act_source;
    /* (1.12.142) (user ruling 2026-10-28): HBM standby VPP priced as a RATIO of the VDD standby
     * power (Xilinx XPE 2023.1.2 HBM_Data 'Idle VPP Dynamic' / 'idle VDD dynamic'), DERIVED;
     * alt_* is the other XPE stack column, printed beside it. Refused together with an ipp
     * row (that would price the standby VPP twice). */
    bool has_vpp_standby = false;
    double vpp_standby_ratio = -1.0, vpp_standby_alt_ratio = -1.0;
    std::string vpp_standby_device, vpp_standby_alt_device, vpp_standby_provenance, vpp_standby_source;
    /* 1.12.18 (user ruling 2026-10-24): activate+precharge guard per 1 KB row, nJ (printed, never priced) */
    bool has_act_guard = false;
    double act_guard_lo = -1.0, act_guard_ceiling = -1.0, act_guard_hi = -1.0;
    std::string act_guard_source;
    /* 1.12.18 (user ruling 2026-10-24, floor option 2): like-for-like XPE background guard per stack, mW
     * (DRAM VDD + VDDQ + VPP rails; printed by readEnergyGuardLines, never priced) */
    bool has_bg_guard = false;
    double bg_guard_lo_mw = -1.0, bg_guard_hi_mw = -1.0;
    std::string bg_guard_source;
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
/* 1.12.18 (user ruling 2026-10-23): published whole-device read energy of older generations, a GUARD
 * printed against the model's derived total read energy (never priced, never tuned). */
struct ReadEnergyGuardRec {
    bool valid = false;
    struct Ref { std::string label; double pj_bit = 0.0; std::string source; };
    std::vector<Ref> references;
    double max_generation_gain = 0.0;   // expected band per reference: [(1 - gain) x pj_bit, pj_bit]
    std::string gain_source;
};

/* 1.12.25: a voltage variant of the part (record block 'variants'; memory.dram.ddr3_variant selects one). */
struct VariantRec { double vdd = 0.0, vddq = 0.0; std::string source; };
/* 1.12.25: one tRFC basis (record block 'trfc_bases'; memory.dram.trfc_basis selects one). */
struct TrfcBasisRec { double trfc_ns = 0.0; std::string source; };

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
    /* (1.12.52) GDDR6 die count (user ruling 2026-10-25, clean fix): channels ONE die carries, for a non-stacked part.
     * 1 (default) for the DDR family and LPDDR5 (one x16 channel per die); GDDR6 2 (JESD250D 4.1, required in its
     * record). The die population, the per-die clock trunk and the placement capacity divide by it. */
    int channels_per_die = 1;
    double die_capacity_gb = 0.0;  // HBM core die capacity (0 for non-stacked)
    std::vector<RefreshRung> refresh_ladder;
    IddBlock idd;                        // 1.11.104
    IddTemperatureRec idd_temperature;   // (1.12.88): optional
    ComponentFactorsRec component_factors;
    TerminationRec termination;
    IddqBandRec iddq_band;
    ReadEnergyGuardRec read_energy_guard;   // 1.12.18: optional (HBM3)
    /* 1.12.25 (DDR3): optional voltage variants and tRFC bases; 'variant' / 'trfc_basis' name the ones in force
     * (empty when the record has no such block) -- the record's idd rows and termination already carry them. */
    std::string variant_default, variant;
    std::map<std::string, VariantRec> variants;
    std::string trfc_basis_default, trfc_basis;
    std::map<std::string, TrfcBasisRec> trfc_bases;
    /* (1.12.61) (user ruling 2026-10-25): optional data-dependent column-burst energy (data_pattern block).
     * ONES_LINEAR: factor 1 + k (f_ones - 0.5); DQ_TOGGLE_RELATIVE: pJ(dq, tsv, bg) / pJ(reference). */
    struct DataPatternRec {
        bool valid = false;
        std::string form, provenance, source;
        double read_ones = 0.0, write_ones = 0.0;
        double floor_pj_bit = 0.0, c_dq = 0.0, c_tsv = 0.0, c_bg = 0.0, tsv_rate = 0.5, bg_rate = 0.5;
        double ref_dq_rate = 0.5, ref_tsv_rate = 0.5, ref_bg_rate = 0.5;
        bool apply_to_writes = true;
    } data_pattern;
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
/* 1.12.25: the run's choice of a record's voltage variant and tRFC basis ("" = the record's default), applied
 * whenever the record is loaded from then on (checkDramPartRecords sets it before it loads). A name the record
 * does not carry refuses at load. */
void setDramRecordSelection(const std::string& tech, const std::string& variant, const std::string& trfc_basis);
/* 1.12.18: the data rate (MT/s) the record's timing preset names ("DDR5_5600B" -> 5600); 0 when the name
 * carries none. The one place a code site that is not a Ramulator wrapper reads the record's DDR5 grade. */
int dramRecordGradeMTs(const std::string& tech);
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
/* The caches of a HOST core (cache record section `host`): size and associativity of each level, and the load-to-use
 * latency a load measures when it hits there (L1D, L2 in core cycles; the shared last-level cache in ns). The last-level
 * cache is shared and has one slice of `kb` per core of the host. */
struct HostCacheLevelRecord { int kb = -1; int ways = -1; int cycles = -1; double ns = -1.0; int slices = 1; };
struct HostCacheRecord {
    HostCacheLevelRecord l1d, l1i, l2, l3;
    bool l3_non_inclusive = false;   // host.l3.inclusion: non-inclusive (the L2s' evictions fill it) or inclusive
};
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
    HostCacheRecord host;   // the host core's caches (the levels above are every other cache: a processing element's)
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
    int fetch_width_bytes = -1;           // fetch bytes per cycle: ooo wrong-path step; both: a PE's instruction-cache read width (1.12.71)
    int retire_width = -1;                // commit width McPAT prices; in_order: -1 = issue width
    int l1d_fill_buffers = -1;            // (1.12.189) ooo: a processing element's L1D fill buffers (-1: not carried)
    /* 1.12.1 (ticket #114): the in-order element's branch predictor, "pag" |
     * "none" (in_order only; ooo always runs its PAg). A config sets it per
     * element (pim.pe.branch_predictor, devices[].pim.pe.branch_predictor,
     * hosts[].branch_predictor); this is the value where none is set. */
    std::string branch_predictor;
    /* 1.12.18: the first-level TLB geometry of the core the
     * timing model mirrors (ooo only; -1 = not carried). Derivation and
     * citations in params/core/default.yaml. */
    int itlb_entries = -1, itlb_large_entries = -1, dtlb_entries = -1, dtlb_large_entries = -1;
    int tlb_associativity = -1, stlb_entries = -1;
};
/* The out-of-order window of a HOST core (ooo_host): reorder buffer, load and store queues, scheduler and L1D fill
 * buffers of the server core the host stands for, and whether a store leaves the ROB before its line is written.
 * A processing element's out-of-order core keeps the timing core's own window. */
struct HostOooRecord {
    int rob_entries = -1, load_queue_entries = -1, store_queue_entries = -1, scheduler_entries = -1, l1d_fill_buffers = -1;
    bool store_buffer = false;
};
/* The L2 hardware prefetchers of a HOST out-of-order core (host_prefetch): the streamer's stream count, distance and
 * prefetches per L2 lookup, the spatial (pair-line) prefetcher, and the L2 superqueue that bounds the misses in flight. */
struct HostPrefetchRecord {
    bool l2_streamer = false, spatial = false;
    int streams = -1, distance_lines = -1, prefetches_per_lookup = -1, superqueue_entries = -1;
};
struct CoreRecord {
    std::string file, record;
    CoreTypeRecord in_order, ooo;
    HostOooRecord ooo_host;
    HostPrefetchRecord host_prefetch;
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
