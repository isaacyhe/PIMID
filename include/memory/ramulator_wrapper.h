#ifndef PIMID_RAMULATOR_WRAPPER_H
#define PIMID_RAMULATOR_WRAPPER_H

#include "common/types.h"
#include <string>
#include <memory>
#include <functional>

// PIMID PIM extensions
#include "memory/pim_request_payload.h"
#include "memory/pim_bandwidth_tracker.h"
#include "memory/pim_controller_plugin.h"
#include "memory/internal_dram_network.h"
#include "memory/dram_architecture_v2.h"

// Forward declarations for Ramulator types
namespace Ramulator {
    class IMemorySystem;
    struct Request;
}

namespace pimid {

/* 1.11.61 (rulings R2/R4): THE SIMULATED PRESET'S OWN ORGANISATION.
 *
 * The Ramulator2 org preset the wrapper names is the authority for what part
 * this tree simulates -- it fixes the density, the bank count and the row
 * count the timing model counts cycles against. Until this release PIMID
 * carried three private copies of that organisation (the architecture object's
 * capacity fields, main.cpp's bank_rows table, and the wrapper's own
 * banks_per_rank_), each of which had drifted from it independently: the
 * capacities by 4x-32x and bank_rows by 2x on DDR3, HBM2 and HBM3.
 *
 * Ramulator2's org_presets tables are `inline static const std::map` members
 * of classes defined in .cpp files (external/ramulator/src/dram/impl/*.cpp),
 * with no header exposing them, so they cannot be read at runtime from here.
 * This struct is a TRANSCRIPTION of the exact preset rows PIMID selects, each
 * row carrying the file it was transcribed from, so that there is ONE copy in
 * PIMID instead of three and it names its upstream. `preset_source` is checked
 * for self-consistency at construction (banks x rows x cols x DQ must
 * reproduce the density), which is the guard against a transcription slip.
 *
 * UNITS, and they differ by family exactly as the preset files do:
 *   density_mb        DDR family: the whole DEVICE (GDDR6 included -- its
 *                     density product spans both channels). HBM: ONE CHANNEL
 *                     (HBM2.cpp/HBM3.cpp call it "channel density").
 *   banks_in_density  the banks that density_mb spans, on the same unit.
 *   channels_in_density  the channel levels inside the density product
 *                     (GDDR6 2, everything else 1) -- NOT the run's channel
 *                     count, which is RamulatorWrapper::getNumChannels().
 */
struct PresetOrganization {
    std::string preset_name;        // e.g. "DDR4_8Gb_x8"
    std::string preset_source;      // e.g. "external/ramulator/src/dram/impl/DDR4.cpp"
    uint64_t    density_mb = 0;     // per DEVICE (DDR family) / per CHANNEL (HBM)
    int         dq_bits = 0;
    int         channels_in_density = 1;
    int         banks_in_density = 0;
    /* 1.11.72: THE BANK GROUPING, per channel, from the same preset row.
     * bank_groups x banks_per_group x channels_in_density must reproduce
     * banks_in_density (checked in makePresetOrg). HBM folds its two
     * pseudo-channels into bank_groups, as every other site in this tree
     * does (main.cpp per-tech table, the architecture objects). */
    int         bank_groups = 0;
    int         banks_per_group = 0;
    uint64_t    rows_per_bank = 0;
    uint64_t    cols_per_row = 0;
    bool        per_channel_density = false;   // true for HBM2/HBM3
    bool        valid = false;

    // One bank of the simulated part, in MB. Same unit for every family.
    uint64_t bankSizeMB() const {
        return (banks_in_density > 0) ? density_mb / static_cast<uint64_t>(banks_in_density) : 0;
    }
    // Bytes in one row of one bank.
    uint64_t rowBytes() const { return cols_per_row * static_cast<uint64_t>(dq_bits) / 8; }
};

/* 1.11.63 (R6): THE SIMULATED TIMING PRESET'S OWN ROW, on the same terms.
 *
 * Companion of PresetOrganization above, and it exists for the same reason
 * ruling R6 gives: PIMID provides no numbers of its own, so a PIMID-side ns
 * literal that paraphrases a Ramulator timing preset must be DERIVED from that
 * preset instead. The architecture objects carried four such literals per
 * technology (tRCD/tCAS/tRP/tRAS) and two of them described a different SPEED
 * BIN from the preset the run counts cycles against -- DDR5 the 4800 bin's ns
 * against a DDR5_3200AN preset (11% high), DDR3 the 1600K bin's against a
 * DDR3_1600H preset (22% high).
 *
 * Ramulator2's timing_presets tables are `inline static const std::map`
 * members of classes defined in .cpp files, exactly like the org tables, so
 * they cannot be read at runtime from here; this is a TRANSCRIPTION of the
 * selected rows, each naming its source file.
 *
 * TWO THINGS ARE DERIVED RATHER THAN TRANSCRIBED, because upstream derives
 * them too:
 *   tCK_ps         every impl OVERWRITES the row's tCK_ps column before
 *                  anything reads it, as `ck_divisor_e6 * 1E6 / rate`. The
 *                  DIVISOR IS PER FAMILY and is not a free parameter: 2 for
 *                  the classic DDR bus whose data rate is twice the command
 *                  clock (DDR3/DDR4/DDR5/GDDR6/HBM2), 4 for HBM3 (JESD238B.01
 *                  Table 92 printed p.160 gives fCK = rate/4 in all nine
 *                  bins), 8 for LPDDR5. So the CLOCK THE MODEL RUNS AT comes
 *                  from `rate` and the family, never from the column.
 *   the ns values  n x tCK, which is what the timing model enforces.
 *
 * THE SELF-CHECK, and it is load-bearing: table_tCK_ps == the derived tCK_ps.
 * Upstream now makes that a hard contract -- each impl THROWS a
 * ConfigurationError if its preset's tCK_ps column does not mirror the
 * derivation -- so the column is authoritative and a disagreement HERE means
 * PIMID's transcription has gone stale against a preset that moved. That is a
 * real failure mode: this transcription and the HBM3/GDDR6 CK-domain
 * correction upstream were written the same afternoon. When the two disagree
 * PIMID REFUSES to read the row's cycle counts as ns rather than picking a
 * clock; see applyPresetTimingsToArchitecture(). */
struct PresetTiming {
    std::string preset_name;        // e.g. "DDR4_2400R"
    std::string preset_source;      // e.g. "external/ramulator/src/dram/impl/DDR4.cpp"
    int rate_mtps = 0;              // the row's `rate` column, MT/s on the DQ
    int nCL = 0, nRCD = 0, nRP = 0, nRAS = 0, nBL = 0;
    /* 1.11.65: the write-to-read turnaround, in CK of THIS row. The
     * same-bank-group figure (nWTRL / nWTR_L) where the family splits it,
     * the single nWTR where it does not (DDR3), and for DDR5 the tWTR_L
     * term of the standard's composite (JESD79-5D Table 334: tCCD_L_WTR =
     * CWL + WBL/2 + Max(16nCK, 10ns) -> 16 at 625 ps). Consumed by the DQ
     * turnaround penalty in main.cpp, which used to carry a hand-written
     * ns table nothing read at run time -- GDDR6's entry had drifted 1.75x
     * after the 1.11.63 tCK correction. 0 = not transcribed; the consumer
     * then refuses rather than guesses. */
    int nWTR = 0;
    int table_tCK_ps = 0;           // the row's own tCK_ps column
    int ck_divisor_e6 = 2;          // the impl's own tCK = ck_divisor_e6 * 1E6 / rate
    int tCK_ps = 0;                 // DERIVED, exactly as the impl derives it
    bool ck_consistent = false;     // table_tCK_ps == tCK_ps
    bool valid = false;

    double tCK_ns() const { return tCK_ps / 1000.0; }
    // Usable as ns only when the row carries ONE clock.
    bool derivable() const { return valid && ck_consistent && tCK_ps > 0; }
    double tCAS_ns() const { return nCL  * tCK_ns(); }
    double tRCD_ns() const { return nRCD * tCK_ns(); }
    double tRP_ns()  const { return nRP  * tCK_ns(); }
    double tRAS_ns() const { return nRAS * tCK_ns(); }
    double tWTR_ns() const { return nWTR * tCK_ns(); }   // 1.11.65
};

/**
 * Wrapper class that adapts Ramulator2 to PIMID's memory model interface
 * This provides a clean separation between PIMID and Ramulator code
 */
class RamulatorWrapper {
public:
    explicit RamulatorWrapper(const std::string& config_path, const std::string& dram_type = "DDR4");
    ~RamulatorWrapper();

    /* 1.11.61 (gate 1171A D4): mark this wrapper as a REFERENCE ANCHOR --
     * constructed only to read one bandwidth number (the B012 REF anchors),
     * never to size anything. Anchor wrappers suppress the capacity and
     * speed-bin provenance WARNINGS, which would otherwise misattribute an
     * HBM3 sizing disagreement into every DDR4/HBM2 run's log. The warnings
     * still fire on every wrapper that is actually used for the run. */
    void setAnchorQuiet(bool q) { anchor_quiet_ = q; }

    // Initialization
    void initialize();
    void loadConfig(const std::string& config_path);

    // Request handling
    bool send(Address addr, MemoryRequestType type,
              std::function<void(Address)> callback = nullptr);

    // PIM-aware request handling
    bool sendPIM(Address addr, MemoryRequestType type,
                PIMRequestPayload* pim_payload,
                std::function<void(Address)> callback = nullptr);

    bool canAccept() const;
    void tick();

    // Statistics and metrics
    uint64_t getTotalReads() const { return total_reads_; }
    uint64_t getTotalWrites() const { return total_writes_; }
    /* 1.11.57 (latent D009): getRowHits/getRowMisses/getRowConflicts and the
     * three counters behind them are DELETED. They were never incremented --
     * handleRequestCompletion() carried a "placeholder for now" comment where
     * the update belonged, and the only assignments were the constructor's
     * zeroes and resetStats()' zeroes -- so every consumer read a live-looking
     * statistic that was structurally 0. They could not be repaired either: no
     * Ramulator2 instance is ever created in this tree (all construction sites
     * pass an empty config path), so there are no internal row statistics to
     * copy. The measured row behaviour PIMID does have arrives by a different
     * road, setRowMissFraction(), from the PE memory interface's own
     * rowHits/rowMisses; the consumers that used to divide by zeroed counters
     * now use that instead. Deleted rather than left at zero so that nobody
     * treats a permanently-empty counter as a measurement. */

    // Energy metrics (from Ramulator statistics)
    double getReadEnergy() const;
    double getWriteEnergy() const;
    double getActivationEnergy() const;
    double getPrechargeEnergy() const;
    double getRefreshEnergy() const;
    double getLeakagePower() const;
    double getTotalEnergy() const;

    // 1.9.10: PER-ACCESS energy accessors + JEDEC-IDD background/refresh.
    // Unlike getReadEnergy()/getWriteEnergy() (which return a cumulative
    // total_reads_ * per-access and were the source of the 0.000 nJ bug when
    // queried on an unfed oracle), these return the *intensive* per-64B-access
    // dynamic energy and per-device standby/refresh power, independent of how
    // many accesses the wrapper has seen. Sourced from the in-tree DRAM-arch
    // bank energy (array) + a per-tech JEDEC IDD/VDD table (interface, background).
    // Override the IDD-derived array energy with a fixed pJ/byte (0 = use IDD default).
    void setBankEnergyOverridePJPerByte(double v) { energy_bank_override_pJ_per_byte_ = v; }
    /* 1.11.59 (audit C018): this used to store the width and stop there, so
     * the JEDEC device width priced the ARRAY ENERGY (4/8/16 devices per
     * access) and changed nothing on the WIDTH side -- chip_io_bits stayed at
     * the architecture object's fixed 8 for every DDR-class part, and the
     * extractor stamped that 8 "Extracted from Ramulator device configuration
     * (x4/x8/x16)". One run then priced a rank as 4 devices for energy and 8
     * for bandwidth. It now applies the width to the architecture object as
     * well; defined out of line for that reason. Order-independent: callers
     * that set the width before initialize() and callers that set it after are
     * both correct. */
    void setDeviceWidth(const std::string& w);  // 1.11.46; 1.11.59 (C018)
    /* 1.11.66 (round 5 F6, user ruling R8 #9): DDR5 SPEED GRADE as a
     * setting -- 3200, 4800 (default) or 5600 MT/s. The grade selects ONE
     * PART: timing preset (DDR5_3200AN / DDR5_4800B / DDR5_5600B), org (8 Gb
     * at 3200 -- the only 3200 datasheet class held -- and the 16 Gb Micron
     * MT60B die at 4800/5600), IDD row (Rev A = 4800B, Rev D = 5600B), the
     * data rate the energy/bandwidth basis uses, and every transcription.
     * Must be called before initialize(); ignored for other technologies. */
    void setDdr5SpeedGrade(int mtps);
    /* 1.11.72: stamp the preset's bank grouping (banks per group, groups per
     * chip) onto the architecture object, beside the R1 density stamp. The
     * object literals for DDR5 described the 8 Gb part (2 banks/BG) while
     * the default grade has simulated the 16 Gb part (4 banks/BG) since
     * 1.11.66; every oracle that read getBanksPerBankGroup() -- power
     * population, system-scope placement -- saw 16 banks on a 32-bank
     * device. */
    void applyPresetBankGroupingToArchitecture();
    int  getDdr5SpeedGrade() const { return ddr5_grade_mtps_; }
    /* 1.11.67 (re-sim pre-flight): RUN-WIDE KNOB DEFAULTS.
     *
     * main.cpp constructs this class at eighteen sites -- oracles, probes,
     * the latency helper, the bandwidth-scope report that gates ladder
     * adoption, the host-MC and multi-node M/D/1 rates. 1.11.66 routed the
     * four run-wide knobs (device width, DDR5 grade, temperature,
     * termination override) through applyDramKnobs() and reached eleven of
     * them; the other seven -- among them the latency helper and the ladder
     * gate -- kept constructing the DEFAULT part. Measured at grade 3200:
     * cycles at 3200AN, access latency and reported bandwidth at 4800B, the
     * 16 Gb org booked for an 8 Gb run. Invisible at the default grade, which
     * is why gate 1176A D9 passed.
     *
     * The knobs are run-wide by definition (one memory.dram block, one
     * power.temperature per run), so they are set ONCE, here, as soon as the
     * config is complete, and every constructor from then on starts from
     * them. applyDramKnobs() stays as the per-instance path and is now
     * idempotent with this. A library user who never calls this gets the
     * pre-1.11.67 member defaults, unchanged. */
    static void setRunWideKnobs(const std::string& device_width, int ddr5_grade_mtps,
                                int temperature_k, double termination_pj_per_bit);
    /* The key the energy layer selects an IDD row by: the technology, plus a
     * "-<grade>" suffix for DDR5 (pimid_energy.h baseTech() strips it for
     * every family-level decision). */
    std::string energyKey() const;
    /* 1.11.66: THE modelled data rate, from the transcribed preset row --
     * one authority. Since the GDDR6 rate column was corrected to the pin
     * rate, every preset's `rate` IS the rate PIMID prices, so the static
     * CactiIOWrapper::dramRateMTs() table is no longer a second authority:
     * it is retained only as the cross-check this function warns against. */
    double modelledRateMTs() const;
    /* 1.11.66 (round 5 A6): FATAL if the transcribed org (banks/rows/cols)
     * disagrees field-by-field with the device Ramulator instantiates from
     * the preset. PIMID_ORG_BREAK=1 forces the mismatch for gates. */
    void checkTranscribedOrganizationShape();
    /* 1.11.65: the DRAM's operating temperature, for the refresh-rate ladder
     * (pimid_energy.h refreshTempFactor). Default 358 K = 85 C = top of the
     * nominal range, i.e. the pre-1.11.66 behaviour. */
    void setTemperatureK(int k) { temperature_k_ = k; }
    int  getTemperatureK() const { return temperature_k_; }
    double getRefreshTempFactor() const;   // 1.11.66: the ladder multiplier in force
    /* 1.11.52 (audit D003): the MEASURED row-buffer miss fraction from the
     * run (PE-MI rowHits/rowMisses). <0 = not measured, and the array energy
     * then uses the stated 0.5 fallback while the caller says so. */
    void setRowMissFraction(double f) { row_miss_frac_ = f; }
    double getRowMissFraction() const { return row_miss_frac_; }

    double getArrayReadEnergyNJ() const;     // array rd (act+col, amortized) per 64B
    double getArrayWriteEnergyNJ() const;    // array wr per 64B
    /* 1.11.63 (R7): direction-aware. Reads price the DRAM driver into the
     * RX termination (RTT_NOM class); writes price the controller driver
     * into the DRAM write termination (RTT_WR class). Both the CACTI-IO
     * exact-map path and the scheme-table fallback carry the split. */
    double getTerminationEnergyNJ(bool is_write) const;   // ODT/termination per 64B
    /* 1.11.40 (N8): interface terms PIMID never modelled -- driver switching
     * and PHY per 64 B, and the IO area. Previously the DQ interface was
     * termination-only, which understated LPDDR5 by ~71x (the 142x quoted here
     * before rested on a superseded LPDDR5 termination row; see the D010 note
     * in getTerminationEnergyNJ) because LVSTL exists to make termination
     * negligible.
     * 1.11.57 (latent D011): UNUSED AND UNVALIDATED. Neither accessor has a
     * caller anywhere in src/ or include/; every reported DQ-interface number
     * still comes from getTerminationEnergyNJ() alone, so the interface is
     * termination-only in results even though this correction exists in code.
     * Do not cite the 1.11.40 fix as landed until a caller consumes these. */
    /* 1.11.59: the termination band, where Rtt is unsourced. Returns false
     * when the technology's electricals ARE sourced (nothing to band). */
    bool getTerminationEnergyBandNJ(double& lo_nj, double& hi_nj,
                                    std::string& provenance) const;
    double getInterfaceDynamicEnergyNJ() const;
    double getInterfaceAreaMM2() const;
    /* 1.11.60 (audit round 4, C009): true when the last getInterfaceAreaMM2()
     * returned 0.0 because CACTI-IO REFUSED to extrapolate its area
     * polynomial above 3162 MHz -- as distinct from returning 0.0 because the
     * technology has no exact parameter set. GDDR6 is the only live case (its
     * bus runs at 7000 MHz while its electricals ARE sourced), and the caller
     * that consumes the area gates on `> 0.0`, so without this the run said
     * nothing at all about a missing term. The accessor is meaningful only
     * after a getInterfaceAreaMM2() call. */
    bool interfaceAreaWithheld() const { return io_area_withheld_; }
    // Override termination energy (pJ/bit; <0 = model default, 0 = force no termination).
    void setTerminationOverridePJPerBit(double v) { energy_term_override_pJ_per_bit_ = v; }
    double getBackgroundPowerMW() const;     // per-unit active standby + refresh
    double getRefreshPowerMW() const;        // per-unit refresh component only
    /* 1.11.20 (D13+D15): the memory system's background. Population-scaled
     * (one DDR chip / one HBM channel is the IDD unit) and state-aware
     * (IDD3N busy, IDD2N idle, IDD2P idle-with-pg). device_width is the
     * JEDEC x4/x8/x16 string; "" means the x8 default. */
    /* 1.11.52 (audit A015): population = devices/rank x ranks x channels, the
     * same basis memorySystemDieCount() uses for AREA, so the Power and Area
     * lines of one report describe the same memory system. */
    int    getBackgroundUnits(const std::string& device_width = "",
                              int ranks_per_channel = 1, int channels = 1) const;
    double getBackgroundSystemMW(double r_idle, bool pg_enabled,
                                 const std::string& device_width = "",
                                 int ranks_per_channel = 1,
                                 int channels = 1) const;   // 1.11.52 (A015)

    // Configuration queries
    uint64_t getCapacity() const { return capacity_; }
    uint64_t getBandwidth() const { return bandwidth_; }
    Cycle getAverageLatency() const;

    // Cycle tracking
    Cycle getCurrentCycle() const { return current_cycle_; }

    void printStats() const;
    void resetStats();

    //=========================================================================
    // Subarray-Level Characteristics (for PIM modeling)
    //=========================================================================

    // DRAM timing breakdown (in nanoseconds)
    double getTRCD() const;                   // RAS to CAS delay
    double getTCAS() const;                   // CAS latency
    double getTRP() const;                    // Row precharge time
    double getTRAS() const;                   // Row active time
    /* 1.11.57 (latent D020): getTRRD() removed -- it returned tRAS/4, an
     * invented relation between two independently specified JEDEC timings,
     * and had no callers. */
    double getTRC() const;                    // Row cycle time
    double getTBurst() const;                 // Burst transfer time

    // Subarray organization
    uint32_t getSubarraysPerBank() const;
    uint32_t getBanksPerBankGroup() const;
    uint32_t getBankGroupsPerChip() const;
    uint32_t getChipsPerRank() const;
    uint32_t getRanksPerChannel() const;
    uint32_t getNumChannels() const { return channels_; }

    // Subarray capacity
    uint64_t getSubarraySizeKB() const;
    uint64_t getBankSizeMB() const;
    uint64_t getChipSizeMB() const;

    /* 1.11.61 (rulings R2/R4): the organisation of the Ramulator org preset
     * this wrapper names -- the part whose cycles the timing model counts.
     * Resolved from dram_type_ and the configured device width; always valid
     * for the seven DRAM technologies PIMID supports (an unrecognised type
     * falls back to DDR4_8Gb_x8, the same substitution the architecture object
     * makes and announces). */
    const PresetOrganization& getPresetOrganization() const { return preset_org_; }
    /* 1.11.63 (R6): the timing row of the same preset pair -- the speed bin
     * whose cycles this run counts. See PresetTiming. */
    const PresetTiming& getPresetTiming() const { return preset_timing_; }
    // Rows per bank of the simulated preset. 0 only if the preset is unknown.
    uint64_t getPresetRowsPerBank() const { return preset_org_.rows_per_bank; }
    /* The preset's DEVICE capacity in MB: the device itself for the DDR
     * family, the whole STACK (per-channel density x channels) for HBM. */
    uint64_t getPresetDeviceCapacityMB() const;
    /* HBM only: the CORE DIES the preset stacks, at two channels per die --
     * the same 2-channels-per-die relation memorySystemDieCount() uses in
     * main.cpp. 0 for the DDR family, which has no die stacking. */
    int getPresetDiesPerStack() const;

    // Internal port bitwidths (critical for PIM bandwidth!)
    // 1.11.60 (one fabric): the rung's upstream reference, verbatim from the
    // architecture object's VerifiedValue -- status + citation.
    std::string getLadderRungProvenance(int rung) const;
    int getSubarrayPortBits() const;
    int getBankPortBits() const;
    int getBankGroupPortBits() const;
    int getChipIOBits() const;
    int getRankDataBits() const;
    int getChannelDataBits() const;   // ONE channel, every family (1.11.57 C007)

    /* 1.11.57 (audit C004): the technology the architecture object actually
     * describes, which is NOT always the technology this wrapper was
     * constructed for -- DDR3, LPDDR5 and GDDR6 have no object and read
     * DDR4-2400's. Empty when there is no object at all. */
    std::string getArchitectureTechnology() const;

    // Hierarchical bandwidth (GB/s)
    double getSubarrayBandwidth() const;
    double getBankBandwidth() const;
    double getBankGroupBandwidth() const;
    double getChipIOBandwidth() const;
    double getRankBandwidth() const;
    double getChannelBandwidth() const;

    // Hierarchical energy (pJ per byte)
    double getSubarrayEnergyPerByte() const;
    double getBankEnergyPerByte() const;
    double getChipEnergyPerByte() const;
    double getRankEnergyPerByte() const;
    /* 1.11.57 (latent D020): getBankGroupEnergyPerByte() (bank x 1.5) and
     * getChannelEnergyPerByte() (rank x 1.5) removed -- unsourced tier
     * surcharges with no callers. */

    // Hierarchical latency (ns)
    double getSubarrayAccessLatency() const;
    double getBankAccessLatency() const;
    double getBankGroupAccessLatency() const;
    double getChipAccessLatency() const;
    double getRankAccessLatency() const;
    /* 1.11.57 (latent D020): getChannelAccessLatency() removed -- rank
     * latency x 1.2 as an unsourced "MC overhead", with no callers. */

    // Get DRAM architecture specification
    const pimid::memory::DRAMArchitectureV2* getDRAMArchitecture() const;

    //=========================================================================

    // PIM-specific configuration and queries
    void enablePIMSupport(const std::string& dram_type = "DDR4");
    void registerPE(PIMGranularity granularity, int pe_id, int target_bank);
    double getBandwidthLimit(PIMGranularity granularity) const;
    int getPortBitwidth(PIMGranularity granularity) const;
    double getEffectiveBandwidthPerPE(PIMGranularity granularity, int target_id) const;

    // Get PIM components
    std::shared_ptr<PIMBandwidthTracker> getBandwidthTracker() const {
        return bandwidth_tracker_;
    }
    std::shared_ptr<InternalDRAMNetwork> getInternalNetwork() const {
        return internal_network_;
    }
    std::shared_ptr<PIMControllerPlugin> getPIMPlugin() const {
        return pim_plugin_;
    }

private:
    // Ramulator memory system instance
    std::shared_ptr<Ramulator::IMemorySystem> ramulator_memory_system_;

    // Configuration
    std::string config_path_;
    std::string config_yaml_;  // YAML configuration content

    // Memory system parameters
    uint64_t capacity_;
    uint64_t bandwidth_;
    uint32_t channels_;
    uint32_t ranks_per_channel_;
    uint32_t banks_per_rank_;

    // Statistics tracking
    uint64_t total_reads_;
    uint64_t total_writes_;
    // 1.11.57 (latent D009): row_hits_/row_misses_/row_conflicts_ removed --
    // never incremented, and unincrementable without a Ramulator2 instance.

    // Energy tracking
    /* 1.11.60 (audit round 4, C009): the withheld-area record and its
     * say-it-once latch. See getInterfaceAreaMM2(). */
    mutable bool io_area_withheld_ = false;
    mutable bool warned_io_area_withheld_ = false;
    mutable double cached_read_energy_;
    mutable double cached_write_energy_;
    mutable double cached_leakage_power_;
    mutable Cycle last_energy_update_;
    double energy_bank_override_pJ_per_byte_ = 0.0;  // 0 = IDD default; >0 = user override
    double energy_term_override_pJ_per_bit_ = -1.0;  // <0 = model default; >=0 = user override
    /* 1.11.65 refresh-ladder input. Two defaults exist by design and agree
     * in effect (round 5 C2): the RUN default is config.temperature_k = 350 K
     * (77 C), which every oracle receives via applyDramKnobs(); this member's
     * 358 K (85 C) is the fallback for a wrapper nobody configured, chosen as
     * the top of the nominal range so an unconfigured wrapper reproduces the
     * pre-1.11.65 (temperature-flat) behaviour exactly. Both sit in the
     * <= 85 C rung, so no result differs between them. */
    int temperature_k_ = 358;
    int ddr5_grade_mtps_ = 4800; // 1.11.66 (R8 #9): 3200 | 4800 (default) | 5600
    // 1.11.67: the run-wide values setRunWideKnobs() recorded; consulted by
    // the constructor when s_run_knobs_set_ is true.
    static bool        s_run_knobs_set_;
    static std::string s_run_device_width_;
    static int         s_run_ddr5_grade_mtps_;
    static int         s_run_temperature_k_;
    static double      s_run_termination_pj_per_bit_;

    // Cycle counter
    Cycle current_cycle_;

    // Pending request tracking
    struct PendingRequest {
        Address addr;
        MemoryRequestType type;
        Cycle issue_cycle;
        std::function<void(Address)> callback;
    };
    std::vector<PendingRequest> pending_requests_;

    // PIM Support Components
    bool pim_enabled_;
    std::string dram_type_;
    bool anchor_quiet_ = false;   // 1.11.61 (D4): reference anchor, no provenance warnings
    double row_miss_frac_ = -1.0;   // 1.11.52 (D003): measured; <0 = unmeasured
    /* 1.11.46 (L181): device width for the whole-rank array-energy basis.
     * Empty = x8 default (the same convention backgroundUnits uses). */
    std::string device_width_;
    /* 1.11.59 (audit C018): which device width is currently STAMPED on
     * dram_arch_, in bits. 0 = the factory object's own organization. It is
     * reset to 0 wherever a fresh architecture object is built, so applying a
     * width twice is a no-op and applying it to a rebuilt object is not. */
    int arch_device_width_bits_ = 0;
    /* 1.11.61 (rulings R1/R2/R4): the simulated org preset's organisation.
     * Resolved in initialize() (and by parseConfiguration on the default
     * path) before anything reads a density or a row count off it. */
    PresetOrganization preset_org_;
    /* 1.11.63 (R6): the simulated TIMING preset's row. Resolved beside
     * preset_org_ and on the same schedule. */
    PresetTiming preset_timing_;
    std::shared_ptr<pimid::memory::DRAMArchitectureV2> dram_arch_;
    std::shared_ptr<PIMBandwidthTracker> bandwidth_tracker_;
    std::shared_ptr<InternalDRAMNetwork> internal_network_;
    std::shared_ptr<PIMControllerPlugin> pim_plugin_;

    // Helper functions
    void parseConfiguration();
    void createRamulatorInstance();
    Ramulator::Request createRamulatorRequest(Address addr, MemoryRequestType type);
    Ramulator::Request createPIMRequest(Address addr, MemoryRequestType type,
                                       PIMRequestPayload* pim_payload);
    void handleRequestCompletion(Ramulator::Request& req);
    void updateEnergyMetrics() const;
    void initializePIMComponents();
    // 1.11.59 (audit C018): stamp the configured JEDEC device width onto the
    // architecture object (chip DQ pins, devices per rank, and the JEDEC
    // organization coupled to them). No-op when no width is configured.
    void applyDeviceWidthToArchitecture();
    /* 1.11.61 (rulings R1/R2/R4): resolve preset_org_ from dram_type_ and the
     * configured device width. Idempotent; safe to call again after the width
     * changes. */
    void resolvePresetOrganization();
    /* 1.11.61 (ruling R1) + 1.11.63 (R6-2): stamp the SIMULATED PRESET's
     * density onto the architecture object -- chip_size_mb, bank_size_mb and
     * the derived rank_size_gb. EVERY family now: the DDR family reads
     * chip_size_mb as one DEVICE, HBM as one CORE DIE (the preset's per-channel
     * density x the two channels a core die fronts), which is the reading
     * ruling R2 fixed and R6 makes derived rather than transcribed. */
    void applyPresetDensityToArchitecture();
    /* 1.11.63 (R6): resolve preset_timing_ from dram_type_. Idempotent. */
    void resolvePresetTiming();
    /* 1.11.63 (R6-5): stamp the simulated TIMING preset's bin onto the
     * architecture object's four JEDEC ns timings. Only where the preset row
     * carries ONE clock (see PresetTiming::derivable) and only for the
     * technologies R6 rules on; refuses loudly rather than choosing a clock. */
    void applyPresetTimingsToArchitecture();
    /* 1.11.63 (R6-3): capacity_ and bandwidth_ DERIVED from the preset pair
     * (org x timing) instead of the per-technology literals the else-if chain
     * in parseConfiguration() used to carry. Called after that chain has set
     * channels_/ranks_per_channel_, and again when the device width moves. */
    void derivePresetCapacityAndBandwidth();
};

} // namespace pimid

#endif // PIMID_RAMULATOR_WRAPPER_H
