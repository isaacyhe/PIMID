#ifndef PIMID_NVSIM_WRAPPER_H
#define PIMID_NVSIM_WRAPPER_H

#include "common/types.h"
#include <string>
#include <memory>

// Forward declarations for NVSim types (in nvsim:: namespace)
namespace nvsim {
class InputParameter;
class MemCell;
class Result;
}

namespace pimid {

/**
 * Wrapper class that adapts NVSim to PIMID's memory model interface
 * Provides area, timing, and power modeling for Non-Volatile Memory
 * Supports (reachable from a PIMID config): STT-RAM, PCM, ReRAM.
 * FBDRAM, SLCNAND and MLCNAND exist in NVMType below and in NVSim, but NO
 * config string routes to them -- they are deliberately not exposed (user
 * decision 2026-07-30: keep to the technologies already supported). Do not
 * cite this header as evidence that NAND or floating-body DRAM is modelled.
 */
class NVSimWrapper {
public:
    enum class NVMType {
        STTRAM,        // Spin-Transfer Torque RAM
        PCRAM,         // Phase-Change RAM
        RERAM,         // Resistive RAM
        FBDRAM,        // Fine-grained DRAM
        SLCNAND,       // Single-Level Cell NAND Flash
        MLCNAND        // Multi-Level Cell NAND Flash
    };

    struct NVMConfig {
        // Basic configuration
        uint64_t capacity_bytes;     // Total capacity in bytes
        uint32_t word_width_bits;    // Word width in bits
        NVMType nvm_type;            // Type of NVM

        // Technology parameters
        int process_node_nm;         // Technology node in nm
        /* 1.11.49 (FIX-PRE-FLEET L77): the ITRS device roadmap. It was
         * hardwired HP inside the wrapper, so power.device_corner silently
         * did nothing for every NVM technology -- the exact corner family
         * whose refusal (1.11.13) does NOT apply, since NVSim carries real
         * HP/LSTP/LOP columns. 0=HP, 1=LSTP, 2=LOP, matching the McPAT
         * device_type convention. */
        int device_corner = 0;
        int temperature_k;           // Operating temperature in Kelvin

        // Optimization target
        bool optimize_read_latency;
        bool optimize_write_latency;
        bool optimize_read_energy;
        bool optimize_write_energy;
        bool optimize_leakage;
        bool optimize_area;

        // Cell parameters file (optional, uses defaults if empty)
        std::string cell_file;

        // For Flash memory
        uint64_t page_size_bits;     // Page size (for DRAM/Flash)
        uint64_t block_size_bits;    // Block size (for Flash)

        // Cache-specific (if used as cache)
        int associativity;           // 0 = fully associative, >0 = N-way
        bool is_cache;               // true if used as cache

        // Default constructor with sensible defaults for STT-RAM
        NVMConfig()
            : capacity_bytes(8 * 1024 * 1024)  // 8 MB default
            , word_width_bits(64)
            , nvm_type(NVMType::STTRAM)
            , process_node_nm(22)
            , temperature_k(350)  // ~77 degC
            , optimize_read_latency(false)
            , optimize_write_latency(false)
            , optimize_read_energy(true)
            , optimize_write_energy(true)
            , optimize_leakage(true)
            , optimize_area(false)
            , cell_file("")
            , page_size_bits(0)
            , block_size_bits(0)
            , associativity(0)
            , is_cache(false)
        {}
    };

    explicit NVSimWrapper(const NVMConfig& config);
    ~NVSimWrapper();

    // Initialization and configuration
    void initialize();
    void reconfigure(const NVMConfig& config);

    // Query NVSim results - Timing
    double getReadLatency() const;      // Read latency in seconds
    double getWriteLatency() const;     // Write latency in seconds

    // Query NVSim results - Energy
    double getReadDynamicEnergy() const;    // Read energy in nJ
    double getWriteDynamicEnergy() const;   // Write energy in nJ
    double getLeakagePower() const;         // Leakage power in mW
    double getReadEDP() const;              // Read Energy-Delay Product
    double getWriteEDP() const;             // Write Energy-Delay Product

    // Query NVSim results - Area
    double getArea() const;                 // Total area in mm^2
    double getHeight() const;               // Height in mm
    double getWidth() const;                // Width in mm

    // Cell-level metrics
    double getCellReadLatency() const;      // Cell read latency in seconds
    double getCellWriteLatency() const;     // Cell write latency in seconds
    /* 1.11.23: NVSim resolves the PCM/ReRAM SET and RESET paths separately
     * (FunctionUnit::setLatency/resetLatency). They were never exposed, so the
     * extractor asserted reset = write * 0.3. Returns <0 when the underlying
     * result does not carry them, so callers refuse rather than substitute. */
    /* 1.11.25: the REAL sub-bank ladder. NVSim resolves it -- SubArray derives
     * from FunctionUnit (readLatency/writeLatency) and Mat contains a SubArray
     * -- but this wrapper never exposed it. Instead getDecoderDelay() and its
     * four siblings returned INVENTED percentages of mat.readLatency (10/20/
     * 45/15/5, summing to 0.95), which read as tool output and were not.
     * 1.11.56 (audit D031) rebuilt those five on NVSim's own per-component
     * latencies, so they are tool reads now too; these two remain the right
     * source for a TIER, being the totals NVSim itself composed.
     * Return <0 when unavailable (notably on a cache hit -- the pregenerated
     * cache predates these fields), so callers refuse rather than substitute. */
    double getMatLatency() const;           // bank->mat.readLatency, s
    uint32_t getMatWidthBits() const;       // 1.11.73: bank->mat.numDataBit; 0 = unsourced (old cache)
    double getSetLatency() const;           // SET path latency, s; <0 unknown
    double getResetLatency() const;         // RESET path latency, s; <0 unknown
    double getCellArea() const;             // Cell area in um^2

    // Memory organization
    uint32_t getNumRows() const;
    uint32_t getNumColumns() const;
    uint32_t getNumBanks() const;

    // Bandwidth and throughput
    double getReadBandwidth() const;        // Read bandwidth in GB/s
    double getWriteBandwidth() const;       // Write bandwidth in GB/s

    //=========================================================================
    // Subarray-Level Characteristics (for PIM modeling)
    //=========================================================================

    // Subarray timing breakdown (in seconds)
    double getDecoderDelay() const;           // Row decoder delay
    double getWordlineDelay() const;          // Wordline activation delay
    double getBitlineDelay() const;           // Bitline sensing delay
    double getSenseAmpDelay() const;          // Sense amplifier delay
    double getColumnDecoderDelay() const;     // Column mux/decoder delay
    double getPrechargeDelay() const;         // Precharge delay

    // Subarray organization
    uint32_t getSubarrayRows() const;         // Number of rows per subarray
    uint32_t getSubarrayCols() const;         // Number of columns per subarray
    uint32_t getSubarraysPerMat() const;      // Subarrays per mat
    uint32_t getMatsPerBank() const;          // Mats per bank
    uint32_t getNumSenseAmps() const;         // Number of sense amplifiers

    // Subarray electrical parameters
    double getWordlineLength() const;         // Wordline length (m)
    double getBitlineLength() const;          // Bitline length (m)
    double getWordlineCapacitance() const;    // Wordline capacitance (F)
    double getWordlineResistance() const;     // Wordline resistance (Ohm)
    double getBitlineCapacitance() const;     // Bitline capacitance (F)
    double getBitlineResistance() const;      // Bitline resistance (Ohm)

    /* Subarray energy breakdown (in nJ) and leakage breakdown (in mW).
     * 1.11.57 (latent D059): these are NVSim's OWN per-component terms, as of
     * this release. They used to be fixed percentages of mat.readDynamicEnergy
     * (10/20/40/20/10) and mat.leakage (15/25/30) -- the same defect D031 found
     * in the six delay accessors and 1.11.56 fixed there, left behind here
     * because the energy consumers were unread. All are per SUBARRAY, matching
     * the section heading; guard with hasComponentBreakdown() first. */
    double getDecoderEnergy() const;
    double getWordlineEnergy() const;
    double getBitlineEnergy() const;
    double getSenseAmpEnergy() const;
    double getPrechargerEnergy() const;

    double getDecoderLeakage() const;
    double getWordlineLeakage() const;
    double getSenseAmpLeakage() const;

    // Subarray area
    double getSubarrayArea() const;           // Per-subarray area (mm^2)
    double getMatArea() const;                // Per-mat area (mm^2)

    //=========================================================================

    // Validation
    bool isValid() const;
    /* 1.11.57 (latent C011): IS THERE A RESULT TREE TO BREAK DOWN?
     *
     * isValid() is true on a CACHE HIT too, but a cache hit sets only the eight
     * top-level scalars -- no nvsim_result_ is ever built. Every per-component
     * accessor below (the six delays, the five energies, the three leakages,
     * the areas and the organization counts) reads nvsim_result_->bank and so
     * returns 0 on that path, which is the normal path. Callers that stamp
     * those components with NVSim provenance were therefore attributing a row
     * of zeros to a tool. Ask this first: false means the breakdown is
     * unavailable and must be reported absent, never summed and labelled. */
    bool hasComponentBreakdown() const;
    std::string getErrorMessage() const;

    // Configuration access
    const NVMConfig& getConfig() const { return config_; }

    // Print detailed results
    void printDetailedResults() const;

private:
    // Configuration
    NVMConfig config_;

    // NVSim objects (using raw pointers with manual management)
    nvsim::InputParameter* nvsim_input_;
    nvsim::MemCell* nvsim_cell_;
    nvsim::Result* nvsim_result_;

    // Cached values
    bool initialized_;
    bool valid_;
    std::string error_message_;

    // NVSim characterization cache (process-wide). The design-space search in
    // runNVSim() is expensive (~minutes, millions of design points) and depends
    // only on (nvm_type, capacity_bytes, process_node_nm). It is also invoked
    // twice per simulation (timing path + power path). When a prior run with the
    // same key exists, we reuse its scalar outputs and skip the search. These
    // per-instance fields hold the cached values; cached_ selects them in the
    // getters that matter for timing/power.
    bool cached_ = false;
    double cached_read_latency_s_ = 0.0;
    double cached_write_latency_s_ = 0.0;
    /* 1.11.25: the sub-bank ladder, carried through the pregenerated
     * cache. -1 = this cache file predates the fields. */
    double cached_mat_latency_s_ = -1.0;
    int    cached_mat_width_bits_ = -1;      // 1.11.73
    int    cached_mats_per_bank_  = -1;      // 1.11.73
    double cached_read_energy_nj_ = 0.0;
    double cached_write_energy_nj_ = 0.0;
    double cached_leakage_mw_ = 0.0;
    double cached_area_mm2_ = 0.0;

    // Helper functions
    void runNVSim();
    void validateConfiguration();
    void createNVSimInput();
    void loadCellParameters();
    std::string getCellFileName() const;
};

} // namespace pimid

#endif // PIMID_NVSIM_WRAPPER_H
