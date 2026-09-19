#ifndef PIMID_CACTI_WRAPPER_H
#define PIMID_CACTI_WRAPPER_H

#include "common/types.h"
#include <string>
#include <memory>

// Forward declaration for CACTI types
class uca_org_t;
class InputParameter;

namespace pimid {

/**
 * Wrapper class that adapts CACTI to PIMID's memory model interface
 * Provides area, timing, and power modeling for SRAM caches/scratchpads
 */
class CACTIWrapper {
public:
    // Cell technology: 0=ITRS-HP SRAM, 1=ITRS-LSTP SRAM, 2=ITRS-LOP SRAM,
    //                  3=LP-DRAM, 4=Commercial DRAM
    enum CellType { SRAM_HP = 0, SRAM_LSTP = 1, SRAM_LOP = 2, LP_DRAM = 3, COMM_DRAM = 4 };

    struct SRAMConfig {
        /* 1.11.30 (user ruling E5): the interconnect projection, supplied by
         * the caller so CACTI and McPAT model ONE metal stack. Was pinned to 1
         * inside the wrapper while McPAT defaulted to 0. 0 = aggressive,
         * 1 = conservative (default; the aggressive column sets copper barrier
         * thickness to 0, which is not manufacturable). */
        int ic_proj_type = 1;
        // Capacity and organization
        uint64_t capacity_bytes;     // Total capacity in bytes
        uint32_t line_size;          // Cache line size in bytes
        uint32_t associativity;      // Set associativity
        uint32_t banks;              // Number of banks

        // Port configuration
        uint32_t read_write_ports;   // Number of RW ports
        uint32_t read_ports;         // Number of read-only ports
        uint32_t write_ports;        // Number of write-only ports
        uint32_t single_ended_read_ports; // Number of single-ended read ports

        // Technology
        double tech_node_nm;         // Technology node in nm (e.g., 22, 14, 7)
        /* 1.11.49 (FIX-PRE-FLEET L59): the ITRS PERIPHERY flavor. Hardwired 0
         * (ITRS-HP) for every query, so power.device_corner never reached
         * CACTI at all. 0=HP, 1=LSTP, 2=LOP; periphery/tag flavors only --
         * the data-array CELL type (SRAM vs comm-dram) is a different axis. */
        int device_corner;
        /* 1.11.57 (latent D035): KELVIN, not Celsius. This line read
         * "// Operating temperature in Celsius" while the constructor below
         * initialises it to 350 with the comment "350K = ~77C" and
         * createCACTIInput forwards it RAW to CACTI's input->temp, which
         * CACTI checks as "Temperature must be between 300 and 400 Kelvin and
         * multiple of 10" (external/cacti/io.cc) and EXITS on failure. So a
         * caller who obeyed the declaration and passed 85 would not get a
         * wrong number, it would kill the process mid-run.
         *
         * 1.11.60 (audit round 4, C012): THE REACHABILITY CLAIM THAT FOLLOWED
         * WAS ALREADY FALSE WHEN IT WAS WRITTEN. It read "no caller anywhere
         * sets SRAMConfig::temperature -- every query in the tree takes the
         * 350 K default". Five sites set it, all of them predating 1.11.57:
         * main.cpp (four query paths) and SRAMModel::initialize(), which
         * forwards sram_config_.temperature_k. They are 1.11.52's own D055
         * work -- the release that made the temperature configurable -- so the
         * note was contradicted by the change one release earlier, and it was
         * repeated as the justification for treating a documented-unit error
         * as harmless.
         *
         * What IS true, and is the honest form of the same argument: no path
         * from main.cpp can produce an illegal value, because power.temperature_k
         * is range-validated to 300-400 in steps of 10 before it reaches any of
         * them. But SRAMModel::setTemperatureK() accepts any k > 0 and forwards
         * it unchecked, so a direct user of the model class can trip the guard
         * -- which is precisely the case the old note said did not exist. The
         * guard is load-bearing, not decorative.
         *
         * The declaration is corrected and validateConfiguration() enforces
         * CACTI's own window, so a wrong unit is refused by this wrapper with a
         * readable message instead of by CACTI with an exit. */
        uint32_t temperature;        // Operating temperature in KELVIN
                                     // (300-400, multiple of 10 -- CACTI's rule)

        // Access mode and type
        bool is_cache;               // true for cache, false for scratchpad
        bool is_main_memory;         // Is this main memory (DRAM)?
        uint32_t access_mode;        // 0=normal, 1=sequential, 2=fast

        // Cell technology (SRAM_HP default; set LP_DRAM/COMM_DRAM for DRAM mode)
        CellType cell_type;

        // DRAM-specific parameters (used when cell_type is LP_DRAM or COMM_DRAM)
        uint32_t page_sz_bits;       // Page size in bits (e.g., 8192 = 1KB page)
        uint32_t burst_len;          // Burst length (e.g., 8 for DDR4)
        uint32_t int_prefetch_w;     // Internal prefetch width

        // Output width
        uint32_t output_width_bits;  // Output width in bits

        /* 1.11.14 (borders rule): the technology this array IS, so the
         * calibration can pick its vendor density. Empty = uncalibrated,
         * which is every SRAM/cache/RF/TLB query McPAT makes. */
        std::string memory_tech;

        // Optional tag configuration
        bool specific_tag;           // Use specific tag width
        uint32_t tag_width_bits;     // Tag width if specific_tag is true

        // Suppress the init banner (for latency-only queries whose energy
        // numbers are never consumed and would only mislead in logs)
        bool quiet = false;

        // Optimization objectives (0=weight, 1=deviate, 2=ED, 3=ED2)
        int obj_func_delay;
        int obj_func_dynamic_power;
        int obj_func_leakage_power;
        int obj_func_cycle_time;
        int obj_func_area;

        // Default constructor with sensible defaults
        SRAMConfig()
            : capacity_bytes(256 * 1024)  // 256 KB
            , line_size(64)
            , associativity(1)
            , banks(4)
            , read_write_ports(1)
            , read_ports(0)
            , write_ports(0)
            , single_ended_read_ports(0)
            , tech_node_nm(22)
            , device_corner(0)
            , temperature(350)  // 350K = ~77C
            , is_cache(false)
            , is_main_memory(false)
            , access_mode(0)
            , cell_type(SRAM_HP)
            , page_sz_bits(8192)
            , burst_len(8)
            , int_prefetch_w(8)
            , output_width_bits(512)
            , memory_tech("")       // 1.11.14: empty = not a calibrated DRAM query
            , specific_tag(false)
            , tag_width_bits(0)
            , obj_func_delay(0)
            , obj_func_dynamic_power(0)
            , obj_func_leakage_power(0)
            , obj_func_cycle_time(100)  // Optimize for cycle time by default
            , obj_func_area(0)
        {}
    };

    explicit CACTIWrapper(const SRAMConfig& config);
    ~CACTIWrapper();

    // Initialization and configuration
    void initialize();
    void reconfigure(const SRAMConfig& config);

    // Query CACTI results
    double getAccessTime() const;      // Access time in seconds
    double getCycleTime() const;       // Cycle time in seconds
    double getArea() const;            // Area in mm^2 (raw CACTI)

    /* 1.11.14 (#122, borders rule): JEDEC-CALIBRATED die area, computed
     * INSIDE the tool that owns the array model instead of by the caller.
     *
     * HARD SCOPE (the reason this is a method and not a transform applied to
     * getArea()). 1.11.51 (L88/L247) corrects the stated threat: McPAT does
     * link libcacti7, but it drives it through its OWN mem_array machinery
     * and never constructs a pimid::CACTIWrapper -- so McPAT's cache/RF/TLB
     * queries could never reach this method, and the old comment guarded a
     * path that does not exist. The gate is still real, for PIMID's OWN
     * other CACTIWrapper queries (cache-latency probes, SRAM-as-memory
     * arrays): a DRAM vendor-density factor must never touch those. The
     * calibration therefore applies only when the query says it is a
     * commodity-DRAM MAIN MEMORY array AND names its technology; every
     * other query returns raw CACTI, byte-identical to before. k is
     * reported alongside so a calibrated number can never pass as a raw
     * tool output. */
    struct CalibratedArea {
        double area_mm2 = 0.0;   // calibrated (or raw, when not applicable)
        double raw_mm2  = 0.0;
        double k        = 1.0;
        bool   calibrated = false;
    };
    CalibratedArea getCalibratedDieArea() const;

    /* Vendor die density (Mbit/mm^2) and the DRAM generation class its
     * technology implies -- the tables that used to live in main.cpp. */
    static double vendorDieDensity(const std::string& tech);
    /* 1.11.19 (D11): is that row a published measurement, or derived
     * from a neighbour? Reported wherever a die area is printed, so a
     * derived number can never be quoted as a sourced one. */
    static bool   vendorDieDensitySourced(const std::string& tech);
    /* 1.11.19 (D11): array share of the full die; <0 = not sourced,
     * in which case the derived array line is omitted, not guessed. */
    static double vendorArrayFraction(const std::string& tech);
    static int    generationTableNm(const std::string& tech);
    static const char* generationClass(const std::string& tech);
    /* 1.11.51 (L87): the vendor JEDEC anchor as a METHOD, so no caller
     * re-implements the arithmetic. Returns (chip_bytes -> MB) / density
     * (MB/mm^2) = mm^2/die, or 0 when the technology has no density row.
     * The 1.11.14 migration left this arithmetic duplicated in main.cpp,
     * where one copy divided MBIT by a MB/mm^2 density (an 8x error) and
     * still carried the phantom x1.12 that E29 removed from the tool. */
    static double vendorAnchorAreaMM2(const std::string& tech,
                                      uint64_t chip_bytes);
    double getDynamicReadEnergy() const;   // Read energy in nJ
    double getDynamicWriteEnergy() const;  // Write energy in nJ
    double getLeakagePower() const;    // Leakage power in mW

    // Detailed power breakdown
    double getReadDynamicPower() const;    // Dynamic power during read (mW)
    double getWriteDynamicPower() const;   // Dynamic power during write (mW)
    double getGateLeakagePower() const;    // Gate leakage power (mW)

    // Physical characteristics
    double getCacheHeight() const;     // Cache height in mm
    double getCacheWidth() const;      // Cache width in mm
    double getAreaEfficiency() const;  // Area efficiency (0-1)

    // Timing characteristics
    Cycle getAccessLatencyCycles(double freq_hz) const;
    Cycle getCycleTimeCycles(double freq_hz) const;

    //=========================================================================
    // CACTI 7.0 Subarray-Level Characteristics (NEW)
    //=========================================================================

    // Subarray timing breakdown (in seconds)
    double getDecoderDelay() const;           // Row decoder delay
    double getWordlineDelay() const;          // Wordline activation delay
    double getBitlineDelay() const;           // Bitline sensing delay
    double getSenseAmpDelay() const;          // Sense amplifier delay
    double getSubarrayOutputDelay() const;    // Subarray output driver delay
    double getHtreeDelay() const;             // H-tree interconnect delay

    // Subarray organization
    uint32_t getSubarrayRows() const;         // Number of rows per subarray
    uint32_t getSubarrayCols() const;         // Number of columns per subarray
    /* 1.11.73 (one level below the bank): CACTI's organisation, named by
     * what each field IS. The two accessors these replace were mislabelled:
     * the old getSubarraysPerMat() returned Ndbl x Ndwl, which is the
     * subarray count of the whole CACTI bank, and the old getMatsPerBank()
     * returned num_active_mats, which is the mats fired together per access
     * -- i.e. the SUBBANK's width in mats, not the bank's mat count.
     *   subbank = the horizontal line of mats activated together whose
     *             outputs concatenate into one data word (CACTI
     *             parameter.cc: num_do_b_subbank = num_do_b_mat x
     *             num_act_mats_hor_dir; number_subbanks = num_mats /
     *             num_act_mats_hor_dir). Its width is out_w = the bank's.
     * PIMID's SRAM tier below the bank is the subbank and nothing lower. */
    uint32_t getSubarraysPerBank() const;     // Ndwl x Ndbl (CACTI bank)
    uint32_t getSubarraysPerMat() const;      // num_submarray_mats
    uint32_t getMatsPerBank() const;          // subarrays/bank / subarrays/mat
    uint32_t getActiveMatsPerAccess() const;  // num_act_mats_hor_dir = mats per subbank
    uint32_t getSubbanksPerBank() const;      // mats/bank / active mats
    uint32_t getOutputWidthBits() const;      // out_w: the bank's AND the subbank's width

    // Subarray electrical parameters
    double getWordlineCapacitance() const;    // Wordline capacitance (F)
    double getWordlineResistance() const;     // Wordline resistance (Ohm)
    double getBitlineCapacitance() const;     // Bitline capacitance (F)

    // Subarray energy breakdown (in nJ)
    double getDecoderEnergy() const;
    double getWordlineEnergy() const;
    double getBitlineEnergy() const;
    double getSenseAmpEnergy() const;

    // Subarray leakage breakdown (in mW)
    double getArrayLeakage() const;
    double getWordlineLeakage() const;
    double getColumnLeakage() const;

    // Subarray area
    double getSubarrayArea() const;           // Per-subarray area (mm^2)
    double getCellArea() const;               // Per-cell area (um^2)

    //=========================================================================

    // Validation
    bool isValid() const;
    std::string getErrorMessage() const;

    // Configuration access
    const SRAMConfig& getConfig() const { return config_; }

    // Print detailed results
    void printDetailedResults() const;

private:
    // Configuration
    SRAMConfig config_;

    // CACTI result (using raw pointer with manual management)
    uca_org_t* cacti_result_;
    InputParameter* cacti_input_;

    // Cached values
    bool initialized_;
    bool valid_;
    std::string error_message_;

    // Helper functions
    void runCACTI();
    void validateConfiguration();
    InputParameter* createCACTIInput(const SRAMConfig& config);
};

} // namespace pimid

#endif // PIMID_CACTI_WRAPPER_H
